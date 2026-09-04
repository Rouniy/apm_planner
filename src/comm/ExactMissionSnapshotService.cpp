#include "ExactMissionSnapshotService.h"

#include "ExactLinkTransmitter.h"

#include <QCryptographicHash>
#include <QThread>

#include <mavlink_helpers.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include <variant>

namespace
{

class ScopeExit final
{
public:
    explicit ScopeExit(std::function<void()> callback)
        : m_callback(std::move(callback))
    {
    }

    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

    ~ScopeExit()
    {
        m_callback();
    }

private:
    std::function<void()> m_callback;
};

void appendUnsigned16(QByteArray *bytes, quint16 value)
{
    bytes->append(static_cast<char>(value & 0xffU));
    bytes->append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendUnsigned32(QByteArray *bytes, quint32 value)
{
    bytes->append(static_cast<char>(value & 0xffU));
    bytes->append(static_cast<char>((value >> 8U) & 0xffU));
    bytes->append(static_cast<char>((value >> 16U) & 0xffU));
    bytes->append(static_cast<char>((value >> 24U) & 0xffU));
}

quint32 floatBits(float value)
{
    static_assert(sizeof(float) == sizeof(quint32),
                  "MAVLink float must be 32 bits");
    quint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool terminalState(MissionTransferService::State state)
{
    return state == MissionTransferService::State::Complete
        || state == MissionTransferService::State::Cancelled
        || state == MissionTransferService::State::Error;
}

QString defaultRouteError()
{
    return QStringLiteral(
        "The exact mission route is unavailable or no longer eligible.");
}

MissionProtocolCoordinator::MissionType coordinatorMissionType(
    MAV_MISSION_TYPE missionType)
{
    switch (missionType) {
    case MAV_MISSION_TYPE_FENCE:
        return MissionProtocolCoordinator::MissionType::Fence;
    case MAV_MISSION_TYPE_RALLY:
        return MissionProtocolCoordinator::MissionType::Rally;
    case MAV_MISSION_TYPE_MISSION:
    default:
        return MissionProtocolCoordinator::MissionType::Mission;
    }
}

} // namespace

ExactMissionSnapshotService::ExactMissionSnapshotService(
    SwarmTelemetryRegistry *registry,
    ExactLinkTransmitter *transmitter,
    RouteValidator routeValidator,
    CoordinatorResolver coordinatorResolver,
    int timeoutMs,
    int maxRetries,
    QObject *parent)
    : ExactMissionSnapshotService(
          registry, transmitter, std::move(routeValidator),
          std::move(coordinatorResolver), Clock(),
          timeoutMs, maxRetries, parent)
{
}

ExactMissionSnapshotService::ExactMissionSnapshotService(
    SwarmTelemetryRegistry *registry,
    ExactLinkTransmitter *transmitter,
    RouteValidator routeValidator,
    CoordinatorResolver coordinatorResolver,
    Clock clock,
    int timeoutMs,
    int maxRetries,
    QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_transmitter(transmitter)
    , m_routeValidator(std::move(routeValidator))
    , m_coordinatorResolver(std::move(coordinatorResolver))
    , m_clock(std::move(clock))
    , m_transfer(maxRetries, 255, MAV_COMP_ID_MISSIONPLANNER)
{
    Q_ASSERT(!registry || registry->thread() == thread());
    Q_ASSERT(!transmitter || transmitter->thread() == thread());

    m_monotonicClock.start();
    m_timeoutTimer.setSingleShot(true);
    m_timeoutTimer.setInterval(std::max(1, timeoutMs));

    qRegisterMetaType<ExactMissionKey>();
    qRegisterMetaType<ExactMissionSnapshot>();
    qRegisterMetaType<ExactMissionSnapshotLease>();
    qRegisterMetaType<ExactMissionTransferToken>();
    qRegisterMetaType<ExactMissionTransferResult>();
    qRegisterMetaType<ExactMissionSnapshotService::StartResult>();
    qRegisterMetaType<ExactMissionSnapshotService::InvalidationReason>();

    if (m_registry) {
        connect(m_registry.data(), &SwarmTelemetryRegistry::endpointRetired,
                this,
                [this](const SwarmVehicleInstanceLease &vehicle,
                       SwarmTelemetryRegistry::RetirementReason reason) {
            assertServiceThread();
            QPointer<ExactMissionSnapshotService> guard(this);
            const ExactMissionTransferToken token = m_active.token;
            const bool activeVehicle = token.isValid()
                && m_active.key.vehicle.sameInstance(vehicle);
            invalidateVehicle(vehicle, invalidationReasonFor(reason));
            if (guard && activeVehicle && tokenIsCurrent(token)) {
                failActive(token, QStringLiteral(
                    "The exact vehicle instance was retired during the mission download."));
            }
        });
        connect(m_registry.data(), &QObject::destroyed, this, [this]() {
            assertServiceThread();
            QPointer<ExactMissionSnapshotService> guard(this);
            const ExactMissionTransferToken token = m_active.token;
            m_registry = nullptr;
            invalidateAll(InvalidationReason::RegistryUnavailable);
            if (guard && tokenIsCurrent(token)) {
                failActive(token, QStringLiteral(
                    "The exact vehicle registry became unavailable."));
            }
        });
    }
    if (m_transmitter) {
        connect(m_transmitter.data(), &QObject::destroyed, this, [this]() {
            assertServiceThread();
            const ExactMissionTransferToken token = m_active.token;
            m_transmitter = nullptr;
            if (tokenIsCurrent(token)
                && !terminalState(m_transfer.state())) {
                failActive(token, QStringLiteral(
                    "The exact MAVLink transmitter became unavailable."));
            }
        });
    }
}

ExactMissionSnapshotService::~ExactMissionSnapshotService()
{
    disarmTimeout();
    for (const CoordinatorWatch &watch : m_coordinatorWatches) {
        disconnect(watch.leaseChanged);
        disconnect(watch.destroyed);
    }
    m_coordinatorWatches.clear();
    if (m_active.ownerDestroyed) {
        disconnect(m_active.ownerDestroyed);
    }
    if (m_active.coordinator
        && m_active.coordinator->owns(m_active.coordinatorLease)) {
        m_active.coordinator->release(m_active.coordinatorLease);
    }
    m_active = ActiveTransfer();
    m_transfer.reset();
}

void ExactMissionSnapshotService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    assertServiceThread();
    if (m_operationInFlight || busy()) {
        return;
    }
    m_localSystemId = systemId;
    m_localComponentId = componentId;
    m_transfer.setLocalIdentity(systemId, componentId);
}

ExactMissionSnapshotService::StartResult
ExactMissionSnapshotService::requestDownload(
    QObject *owner,
    const SwarmVehicleInstanceLease &vehicle,
    MAV_MISSION_TYPE missionType,
    ExactMissionTransferToken *token,
    QString *error)
{
    assertServiceThread();
    if (token) {
        *token = ExactMissionTransferToken();
    }
    if (error) {
        error->clear();
    }
    if (m_operationInFlight || busy()) {
        if (error) {
            *error = QStringLiteral(
                "Another exact mission transaction is already active.");
        }
        return StartResult::Busy;
    }
    m_operationInFlight = true;
    const QPointer<ExactMissionSnapshotService> operationOwner(this);
    ScopeExit operationGuard([operationOwner]() {
        if (operationOwner) {
            operationOwner->m_operationInFlight = false;
        }
    });
    if (!owner || owner->thread() != thread()) {
        if (error) {
            *error = QStringLiteral(
                "The mission owner must live on the exact mission service thread.");
        }
        return StartResult::InvalidOwner;
    }
    QPointer<QObject> guardedOwner(owner);
    if (!supportedMissionType(missionType)) {
        if (error) {
            *error = QStringLiteral("Unsupported MAVLink mission type.");
        }
        return StartResult::UnsupportedMissionType;
    }
    if (!exactLeaseIsCurrent(vehicle)) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle lease is stale or unavailable.");
        }
        return StartResult::InvalidLease;
    }
    if (!m_transmitter) {
        if (error) {
            *error = QStringLiteral(
                "The exact MAVLink transmitter is unavailable.");
        }
        return StartResult::TransportUnavailable;
    }
    if (missionType != MAV_MISSION_TYPE_MISSION
        && m_transmitter->outboundVersion(vehicle.endpoint.linkId) == 1U) {
        if (error) {
            *error = QStringLiteral(
                "Fence and rally mission types require MAVLink 2 on the exact link.");
        }
        return StartResult::IncompatibleProtocolVersion;
    }
    // These callbacks are application-owned code.  Keep independent callable
    // instances on the stack so deleting this service from inside either
    // callback cannot destroy the std::function that is currently executing.
    const RouteValidator routeValidator = m_routeValidator;
    const CoordinatorResolver coordinatorResolver = m_coordinatorResolver;
    if (!routeValidator) {
        if (error) {
            *error = QStringLiteral(
                "No exact mission route policy is installed.");
        }
        return StartResult::UnsafeRoute;
    }
    if (!coordinatorResolver) {
        if (error) {
            *error = QStringLiteral(
                "No mission-protocol coordinator resolver is installed.");
        }
        return StartResult::MissionCoordinatorUnavailable;
    }

    QPointer<ExactMissionSnapshotService> guard(this);
    QString routeError;
    const bool routeAccepted = routeValidator(vehicle, &routeError);
    if (!guard) {
        if (token) {
            *token = ExactMissionTransferToken();
        }
        if (error) {
            *error = QStringLiteral(
                "The exact mission service was destroyed during route validation.");
        }
        return StartResult::TransportUnavailable;
    }
    if (!guardedOwner) {
        if (error) {
            *error = QStringLiteral(
                "The mission owner was destroyed during route validation.");
        }
        return StartResult::InvalidOwner;
    }
    if (!routeAccepted) {
        if (error) {
            *error = routeError.isEmpty() ? defaultRouteError() : routeError;
        }
        return StartResult::UnsafeRoute;
    }
    // RouteValidator is injected application code and may synchronously retire
    // the endpoint, end its link session, or destroy a dependency.
    if (!exactLeaseIsCurrent(vehicle)) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle session changed during route validation.");
        }
        return StartResult::InvalidLease;
    }
    if (!m_transmitter) {
        if (error) {
            *error = QStringLiteral(
                "The exact MAVLink transmitter disappeared during route validation.");
        }
        return StartResult::TransportUnavailable;
    }
    if (missionType != MAV_MISSION_TYPE_MISSION
        && m_transmitter->outboundVersion(vehicle.endpoint.linkId) == 1U) {
        if (error) {
            *error = QStringLiteral(
                "The exact link changed to MAVLink 1 during route validation.");
        }
        return StartResult::IncompatibleProtocolVersion;
    }
    if (m_nextTransferId == std::numeric_limits<quint64>::max()) {
        if (error) {
            *error = QStringLiteral(
                "The exact mission transfer identifier is exhausted.");
        }
        return StartResult::IdentifierExhausted;
    }

    QPointer<MissionProtocolCoordinator> coordinator =
        coordinatorResolver(vehicle);
    if (!guard) {
        if (error) {
            *error = QStringLiteral(
                "The exact mission service was destroyed while resolving the mission-protocol coordinator.");
        }
        return StartResult::MissionCoordinatorUnavailable;
    }
    if (!coordinator || coordinator->thread() != guard->thread()) {
        if (error) {
            *error = QStringLiteral(
                "The selected vehicle has no compatible mission-protocol coordinator.");
        }
        return StartResult::MissionCoordinatorUnavailable;
    }
    QPointer<ExactMissionSnapshotService> coordinatorGuard(this);
    const MissionProtocolCoordinator::LeaseToken coordinatorLease =
        coordinator->tryAcquire(this, coordinatorMissionType(missionType));
    if (!coordinatorGuard) {
        if (token) {
            *token = ExactMissionTransferToken();
        }
        return StartResult::MissionCoordinatorUnavailable;
    }
    if (!coordinator || !coordinatorLease.isValid()
        || !coordinator->owns(coordinatorLease)) {
        if (error) {
            *error = QStringLiteral(
                "Another mission operation already owns the selected vehicle protocol.");
        }
        return StartResult::MissionOwnerBusy;
    }
    const auto releaseCoordinator = [&coordinator, &coordinatorLease]() {
        if (coordinator && coordinator->owns(coordinatorLease)) {
            coordinator->release(coordinatorLease);
        }
    };
    if (!guardedOwner || !exactLeaseIsCurrent(vehicle)) {
        releaseCoordinator();
        if (error) {
            *error = guardedOwner
                ? QStringLiteral(
                    "The exact vehicle session changed while acquiring the mission protocol.")
                : QStringLiteral(
                    "The mission owner was destroyed while acquiring the mission protocol.");
        }
        return guardedOwner ? StartResult::InvalidLease
                            : StartResult::InvalidOwner;
    }

    m_transfer.reset();
    if (!m_transfer.setLocalIdentity(m_localSystemId, m_localComponentId)) {
        releaseCoordinator();
        if (error) {
            *error = QStringLiteral(
                "The mission protocol state could not accept the local identity.");
        }
        return StartResult::TransportUnavailable;
    }
    MissionTransferService::Key protocolKey;
    protocolKey.systemId = static_cast<quint8>(vehicle.endpoint.systemId);
    protocolKey.componentId = static_cast<quint8>(vehicle.endpoint.componentId);
    protocolKey.missionType = missionType;
    const MissionTransferService::Transition first =
        m_transfer.startDownload(protocolKey);
    if (!first.handled) {
        releaseCoordinator();
        if (error) {
            *error = QStringLiteral(
                "The mission protocol state rejected the download.");
        }
        return StartResult::TransportUnavailable;
    }

    ++m_nextTransferId;
    m_active.token.id = m_nextTransferId;
    m_active.key = ExactMissionKey{vehicle, missionType};
    m_active.owner = guardedOwner;
    m_active.coordinator = coordinator;
    m_active.coordinatorLease = coordinatorLease;
    watchCoordinator(coordinator.data());
    const ExactMissionTransferToken activeToken = m_active.token;
    m_active.ownerDestroyed = connect(
        guardedOwner.data(), &QObject::destroyed, this,
        [this, activeToken](QObject *) {
        if (!tokenIsCurrent(activeToken)) {
            return;
        }
        // The download is already authoritative once the complete item
        // sequence has moved the protocol into a terminal state. Destruction
        // of the UI owner while the best-effort terminal ACK is being sent
        // must not downgrade that completed snapshot to an error.
        if (terminalState(m_transfer.state())) {
            return;
        }
        const MissionTransferService::Transition cancelled =
            m_transfer.cancel(QStringLiteral(
                "The exact mission download owner was destroyed."));
        if (!cancelled.handled) {
            failActive(activeToken, QStringLiteral(
                "The exact mission download owner was destroyed."));
            return;
        }
        applyTransition(activeToken, cancelled);
    });
    if (token) {
        *token = activeToken;
    }

    emit busyChanged(true);
    if (!guard) {
        if (token) {
            *token = ExactMissionTransferToken();
        }
        return StartResult::TransportUnavailable;
    }
    // A direct busyChanged receiver may synchronously cancel the operation.
    if (tokenIsCurrent(activeToken)) {
        applyTransition(activeToken, first);
    }
    if (!guard) {
        if (token) {
            *token = ExactMissionTransferToken();
        }
        if (error) {
            *error = QStringLiteral(
                "The exact mission service was destroyed while starting the mission transfer.");
        }
        return StartResult::TransportUnavailable;
    }
    return StartResult::Started;
}

bool ExactMissionSnapshotService::cancel(
    const ExactMissionTransferToken &token, const QString &reason)
{
    assertServiceThread();
    if (!tokenIsCurrent(token)) {
        return false;
    }
    const MissionTransferService::Transition cancelled =
        m_transfer.cancel(reason);
    return cancelled.handled && applyTransition(token, cancelled);
}

bool ExactMissionSnapshotService::acquireSnapshot(
    const SwarmVehicleInstanceLease &vehicle,
    MAV_MISSION_TYPE missionType,
    ExactMissionSnapshot *snapshot,
    ExactMissionSnapshotLease *lease) const
{
    assertServiceThread();
    if (snapshot) {
        *snapshot = ExactMissionSnapshot();
    }
    if (lease) {
        *lease = ExactMissionSnapshotLease();
    }
    if (!snapshot || !supportedMissionType(missionType)
        || !exactLeaseIsCurrent(vehicle)) {
        return false;
    }
    const ExactMissionKey key{vehicle, missionType};
    const auto found = m_snapshots.constFind(key);
    if (found == m_snapshots.constEnd() || !found->isValid()) {
        return false;
    }
    *snapshot = *found;
    if (lease) {
        lease->key = found->key;
        lease->contentGeneration = found->contentGeneration;
        lease->contentDigest = found->contentDigest;
    }
    return true;
}

bool ExactMissionSnapshotService::validateSnapshotLease(
    const ExactMissionSnapshotLease &lease) const
{
    assertServiceThread();
    if (!lease.isValid() || !exactLeaseIsCurrent(lease.key.vehicle)) {
        return false;
    }
    const auto found = m_snapshots.constFind(lease.key);
    return found != m_snapshots.constEnd()
        && found->contentGeneration == lease.contentGeneration
        && found->contentDigest == lease.contentDigest;
}

QList<ExactMissionSnapshot> ExactMissionSnapshotService::snapshots() const
{
    assertServiceThread();
    QList<ExactMissionSnapshot> result;
    result.reserve(m_snapshots.size());
    for (auto item = m_snapshots.constBegin();
         item != m_snapshots.constEnd(); ++item) {
        if (exactLeaseIsCurrent(item.key().vehicle)) {
            result.append(item.value());
        }
    }
    std::sort(result.begin(), result.end(),
              [](const ExactMissionSnapshot &left,
                 const ExactMissionSnapshot &right) {
        const VehicleEndpoint &a = left.key.vehicle.endpoint;
        const VehicleEndpoint &b = right.key.vehicle.endpoint;
        if (a != b) {
            return a < b;
        }
        if (left.key.vehicle.linkSessionEpoch
            != right.key.vehicle.linkSessionEpoch) {
            return left.key.vehicle.linkSessionEpoch
                < right.key.vehicle.linkSessionEpoch;
        }
        if (left.key.vehicle.instanceEpoch
            != right.key.vehicle.instanceEpoch) {
            return left.key.vehicle.instanceEpoch
                < right.key.vehicle.instanceEpoch;
        }
        return static_cast<quint8>(left.key.missionType)
            < static_cast<quint8>(right.key.missionType);
    });
    return result;
}

bool ExactMissionSnapshotService::invalidateForKnownMissionMutation(
    const SwarmVehicleInstanceLease &vehicle,
    MAV_MISSION_TYPE missionType)
{
    assertServiceThread();
    if (!vehicle.isValid() || !supportedMissionType(missionType)) {
        return false;
    }
    return invalidateKey(
        ExactMissionKey{vehicle, missionType},
        InvalidationReason::KnownMissionMutation);
}

QByteArray ExactMissionSnapshotService::contentDigestFor(
    const QVector<mavlink_mission_item_int_t> &items)
{
    QByteArray canonical;
    canonical.reserve(4 + items.size() * 40);
    appendUnsigned32(&canonical, static_cast<quint32>(items.size()));
    for (const mavlink_mission_item_int_t &item : items) {
        // target_system/target_component address the GCS in downloaded packets;
        // they are transport metadata rather than vehicle mission content.
        appendUnsigned16(&canonical, item.seq);
        appendUnsigned16(&canonical, item.command);
        canonical.append(static_cast<char>(item.frame));
        canonical.append(static_cast<char>(item.current));
        canonical.append(static_cast<char>(item.autocontinue));
        canonical.append(static_cast<char>(item.mission_type));
        appendUnsigned32(&canonical, floatBits(item.param1));
        appendUnsigned32(&canonical, floatBits(item.param2));
        appendUnsigned32(&canonical, floatBits(item.param3));
        appendUnsigned32(&canonical, floatBits(item.param4));
        appendUnsigned32(&canonical, static_cast<quint32>(item.x));
        appendUnsigned32(&canonical, static_cast<quint32>(item.y));
        appendUnsigned32(&canonical, floatBits(item.z));
    }
    return QCryptographicHash::hash(
        canonical, QCryptographicHash::Sha256).toHex().toUpper();
}

QByteArray ExactMissionSnapshotService::waypointLeaderSignatureFor(
    const QVector<mavlink_mission_item_int_t> &items)
{
    // Bit-for-bit textual contract of MP10 WaypointLeaderMissionPath:
    // dictionaryKey:command:frame:x:y:SingleToInt32Bits(z);
    QByteArray canonical;
    canonical.reserve(items.size() * 64);
    for (int index = 0; index < items.size(); ++index) {
        const mavlink_mission_item_int_t &item = items.at(index);
        canonical.append(QByteArray::number(index));
        canonical.append(':');
        canonical.append(QByteArray::number(item.command));
        canonical.append(':');
        canonical.append(QByteArray::number(item.frame));
        canonical.append(':');
        canonical.append(QByteArray::number(item.x));
        canonical.append(':');
        canonical.append(QByteArray::number(item.y));
        canonical.append(':');
        canonical.append(QByteArray::number(
            static_cast<qint32>(floatBits(item.z))));
        canonical.append(';');
    }
    return QCryptographicHash::hash(
        canonical, QCryptographicHash::Sha256).toHex().toUpper();
}

void ExactMissionSnapshotService::observeMessage(
    int linkId, quint64 linkSessionEpoch,
    const mavlink_message_t &message)
{
    assertServiceThread();
    const ExactMissionTransferToken token = m_active.token;
    if (message.msgid == MAVLINK_MSG_ID_MISSION_ACK) {
        mavlink_mission_ack_t acknowledgement{};
        mavlink_msg_mission_ack_decode(&message, &acknowledgement);
        const MAV_MISSION_TYPE acknowledgedType =
            static_cast<MAV_MISSION_TYPE>(acknowledgement.mission_type);
        if (supportedMissionType(acknowledgedType)) {
            QList<ExactMissionKey> mutated;
            for (auto item = m_snapshots.constBegin();
                 item != m_snapshots.constEnd(); ++item) {
                const ExactMissionKey &key = item.key();
                const VehicleEndpoint &endpoint = key.vehicle.endpoint;
                const bool isActiveDownload = token.isValid()
                    && m_active.key == key;
                if (!isActiveDownload
                    && key.missionType == acknowledgedType
                    && endpoint.linkId == linkId
                    && key.vehicle.linkSessionEpoch == linkSessionEpoch
                    && endpoint.systemId == message.sysid
                    && endpoint.componentId == message.compid) {
                    mutated.append(key);
                }
            }
            for (const ExactMissionKey &key : mutated) {
                QPointer<ExactMissionSnapshotService> guard(this);
                invalidateKey(
                    key, InvalidationReason::KnownMissionMutation);
                if (!guard) {
                    return;
                }
            }
        }
    }
    if (!token.isValid()) {
        return;
    }
    const SwarmVehicleInstanceLease vehicle = m_active.key.vehicle;
    if (linkId != vehicle.endpoint.linkId
        || linkSessionEpoch == 0
        || linkSessionEpoch != vehicle.linkSessionEpoch
        || message.sysid != vehicle.endpoint.systemId
        || message.compid != vehicle.endpoint.componentId
        || !exactLeaseIsCurrent(vehicle)) {
        return;
    }

    QString routeError;
    QPointer<ExactMissionSnapshotService> routeGuard(this);
    const bool routeAccepted = validateRouteWithBarrier(token, &routeError);
    if (!routeGuard) {
        return;
    }
    if (!routeAccepted) {
        if (tokenIsCurrent(token)) {
            failActive(token, routeError.isEmpty()
                ? defaultRouteError() : routeError);
        }
        return;
    }

    MissionTransferService::Transition transition;
    switch (message.msgid) {
    case MAVLINK_MSG_ID_MISSION_COUNT: {
        mavlink_mission_count_t payload{};
        mavlink_msg_mission_count_decode(&message, &payload);
        transition = m_transfer.handleMissionCount(
            message.sysid, message.compid, payload);
        break;
    }
    case MAVLINK_MSG_ID_MISSION_ITEM_INT: {
        mavlink_mission_item_int_t payload{};
        mavlink_msg_mission_item_int_decode(&message, &payload);
        transition = m_transfer.handleMissionItemInt(
            message.sysid, message.compid, payload);
        break;
    }
    case MAVLINK_MSG_ID_MISSION_ITEM: {
        mavlink_mission_item_t payload{};
        mavlink_msg_mission_item_decode(&message, &payload);
        transition = m_transfer.handleMissionItem(
            message.sysid, message.compid, payload);
        break;
    }
    case MAVLINK_MSG_ID_MISSION_ACK: {
        mavlink_mission_ack_t payload{};
        mavlink_msg_mission_ack_decode(&message, &payload);
        transition = m_transfer.handleMissionAck(
            message.sysid, message.compid, payload);
        break;
    }
    default:
        return;
    }
    if (transition.handled && tokenIsCurrent(token)) {
        applyTransition(token, transition);
    }
}

void ExactMissionSnapshotService::timeout()
{
    assertServiceThread();
    const ExactMissionTransferToken token = m_active.token;
    if (!token.isValid() || m_timerToken != token.id) {
        return;
    }
    disarmTimeout();
    QString routeError;
    QPointer<ExactMissionSnapshotService> routeGuard(this);
    const bool routeAccepted = validateRouteWithBarrier(token, &routeError);
    if (!routeGuard) {
        return;
    }
    if (!routeAccepted) {
        if (tokenIsCurrent(token)) {
            failActive(token, routeError.isEmpty()
                ? defaultRouteError() : routeError);
        }
        return;
    }
    applyTransition(token, m_transfer.onTimeout());
}

bool ExactMissionSnapshotService::supportedMissionType(
    MAV_MISSION_TYPE missionType) noexcept
{
    return missionType == MAV_MISSION_TYPE_MISSION
        || missionType == MAV_MISSION_TYPE_FENCE
        || missionType == MAV_MISSION_TYPE_RALLY;
}

ExactMissionSnapshotService::InvalidationReason
ExactMissionSnapshotService::invalidationReasonFor(
    SwarmTelemetryRegistry::RetirementReason reason) noexcept
{
    switch (reason) {
    case SwarmTelemetryRegistry::RetirementReason::LinkSessionEnded:
        return InvalidationReason::LinkSessionEnded;
    case SwarmTelemetryRegistry::RetirementReason::NoLongerCommandCapable:
        return InvalidationReason::NoLongerCommandCapable;
    case SwarmTelemetryRegistry::RetirementReason::BootTimeReset:
        return InvalidationReason::BootTimeReset;
    case SwarmTelemetryRegistry::RetirementReason::HeartbeatStale:
    default:
        return InvalidationReason::HeartbeatStale;
    }
}

bool ExactMissionSnapshotService::validCompletedItems(
    const QVector<mavlink_mission_item_int_t> &items,
    MAV_MISSION_TYPE missionType) noexcept
{
    if (!supportedMissionType(missionType)
        || items.size() > std::numeric_limits<quint16>::max()) {
        return false;
    }
    for (int index = 0; index < items.size(); ++index) {
        const mavlink_mission_item_int_t &item = items.at(index);
        if (item.seq != static_cast<quint16>(index)
            || item.mission_type != static_cast<quint8>(missionType)) {
            return false;
        }
    }
    return true;
}

bool ExactMissionSnapshotService::tokenIsCurrent(
    const ExactMissionTransferToken &token) const noexcept
{
    return token.isValid() && m_active.token.id == token.id;
}

bool ExactMissionSnapshotService::exactLeaseIsCurrent(
    const SwarmVehicleInstanceLease &vehicle) const
{
    return m_registry && vehicle.isValid()
        && m_registry->currentLinkSessionEpoch(vehicle.endpoint.linkId)
            == vehicle.linkSessionEpoch
        && m_registry->validateLease(vehicle);
}

bool ExactMissionSnapshotService::validateRouteWithBarrier(
    const ExactMissionTransferToken &token, QString *error)
{
    if (error) {
        error->clear();
    }
    if (!tokenIsCurrent(token) || !m_registry || !m_transmitter
        || !m_active.coordinator
        || !m_active.coordinator->owns(m_active.coordinatorLease)) {
        if (error) {
            *error = defaultRouteError();
        }
        return false;
    }
    const SwarmVehicleInstanceLease vehicle = m_active.key.vehicle;
    if (!exactLeaseIsCurrent(vehicle)) {
        if (error) {
            *error = QStringLiteral("The exact vehicle lease is stale.");
        }
        return false;
    }
    const RouteValidator routeValidator = m_routeValidator;
    if (!routeValidator) {
        if (error) {
            *error = QStringLiteral("No exact mission route policy is installed.");
        }
        return false;
    }

    QPointer<ExactMissionSnapshotService> guard(this);
    QString callbackError;
    const bool accepted = routeValidator(vehicle, &callbackError);
    if (!guard) {
        if (error) {
            *error = QStringLiteral(
                "The exact mission service was destroyed during route validation.");
        }
        return false;
    }
    if (!accepted) {
        if (error) {
            *error = callbackError.isEmpty()
                ? defaultRouteError() : callbackError;
        }
        return false;
    }
    // Re-establish every exact-instance barrier after injected code returns.
    if (!tokenIsCurrent(token) || !m_registry || !m_transmitter
        || !m_active.coordinator
        || !m_active.coordinator->owns(m_active.coordinatorLease)
        || !exactLeaseIsCurrent(vehicle)
        || !m_active.key.vehicle.sameInstance(vehicle)) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle session changed during route validation.");
        }
        return false;
    }
    if (m_active.key.missionType != MAV_MISSION_TYPE_MISSION
        && m_transmitter->outboundVersion(vehicle.endpoint.linkId) == 1U) {
        if (error) {
            *error = QStringLiteral(
                "Fence and rally mission types require MAVLink 2 on the exact link.");
        }
        return false;
    }
    return true;
}

bool ExactMissionSnapshotService::sendOutbound(
    const ExactMissionTransferToken &token,
    const MissionTransferService::OutboundMessage &outbound,
    QString *error)
{
    if (error) {
        error->clear();
    }
    QPointer<ExactMissionSnapshotService> routeGuard(this);
    const bool routeAccepted = validateRouteWithBarrier(token, error);
    if (!routeGuard || !routeAccepted) {
        return false;
    }

    // Build schema payloads headerless. Generated *_encode() helpers finalize
    // through the process-global MAVLink channel; if it currently speaks v1,
    // an extension-bearing mission_type byte is overwritten by that first
    // checksum. ExactLinkTransmitter must be the sole finalizer for the
    // physical destination link.
    mavlink_message_t message{};
    char *const bytes = _MAV_PAYLOAD_NON_CONST(&message);
    switch (outbound.type) {
    case MissionTransferService::MessageType::MissionRequestList: {
        const auto *payload =
            std::get_if<mavlink_mission_request_list_t>(&outbound.payload);
        if (!payload) {
            return false;
        }
        message.msgid = MAVLINK_MSG_ID_MISSION_REQUEST_LIST;
        message.len = MAVLINK_MSG_ID_MISSION_REQUEST_LIST_LEN;
        _mav_put_uint8_t(bytes, 0, payload->target_system);
        _mav_put_uint8_t(bytes, 1, payload->target_component);
        _mav_put_uint8_t(bytes, 2, payload->mission_type);
        break;
    }
    case MissionTransferService::MessageType::MissionRequest: {
        const auto *payload =
            std::get_if<mavlink_mission_request_t>(&outbound.payload);
        if (!payload) {
            return false;
        }
        message.msgid = MAVLINK_MSG_ID_MISSION_REQUEST;
        message.len = MAVLINK_MSG_ID_MISSION_REQUEST_LEN;
        _mav_put_uint16_t(bytes, 0, payload->seq);
        _mav_put_uint8_t(bytes, 2, payload->target_system);
        _mav_put_uint8_t(bytes, 3, payload->target_component);
        _mav_put_uint8_t(bytes, 4, payload->mission_type);
        break;
    }
    case MissionTransferService::MessageType::MissionRequestInt: {
        const auto *payload =
            std::get_if<mavlink_mission_request_int_t>(&outbound.payload);
        if (!payload) {
            return false;
        }
        message.msgid = MAVLINK_MSG_ID_MISSION_REQUEST_INT;
        message.len = MAVLINK_MSG_ID_MISSION_REQUEST_INT_LEN;
        _mav_put_uint16_t(bytes, 0, payload->seq);
        _mav_put_uint8_t(bytes, 2, payload->target_system);
        _mav_put_uint8_t(bytes, 3, payload->target_component);
        _mav_put_uint8_t(bytes, 4, payload->mission_type);
        break;
    }
    case MissionTransferService::MessageType::MissionAck: {
        const auto *payload =
            std::get_if<mavlink_mission_ack_t>(&outbound.payload);
        if (!payload) {
            return false;
        }
        message.msgid = MAVLINK_MSG_ID_MISSION_ACK;
        message.len = MAVLINK_MSG_ID_MISSION_ACK_LEN;
        _mav_put_uint8_t(bytes, 0, payload->target_system);
        _mav_put_uint8_t(bytes, 1, payload->target_component);
        _mav_put_uint8_t(bytes, 2, payload->type);
        _mav_put_uint8_t(bytes, 3, payload->mission_type);
        break;
    }
    case MissionTransferService::MessageType::MissionCount:
    case MissionTransferService::MessageType::MissionItem:
    case MissionTransferService::MessageType::MissionItemInt:
        if (error) {
            *error = QStringLiteral(
                "An upload-only mission message appeared in a download transaction.");
        }
        return false;
    }

    if (!tokenIsCurrent(token) || !exactLeaseIsCurrent(m_active.key.vehicle)
        || !m_transmitter) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral(
                "The exact vehicle session changed before transmission.");
        }
        return false;
    }
    const SwarmVehicleInstanceLease vehicle = m_active.key.vehicle;
    QPointer<ExactLinkTransmitter> transmitter = m_transmitter;
    QPointer<ExactMissionSnapshotService> guard(this);
    const ExactLinkTransmitter::SendResult sent = transmitter->sendMessage(
        vehicle.endpoint.linkId, m_localSystemId, m_localComponentId, message);
    if (!guard) {
        if (error) {
            *error = QStringLiteral(
                "The exact mission service was destroyed during transmission.");
        }
        return false;
    }
    if (!tokenIsCurrent(token)) {
        if (error) {
            *error = QStringLiteral(
                "The mission transaction was cancelled during transmission.");
        }
        return false;
    }
    if (sent != ExactLinkTransmitter::SendResult::Sent) {
        if (error) {
            *error = QStringLiteral(
                "The exact MAVLink transport rejected the mission message.");
        }
        return false;
    }
    return true;
}

bool ExactMissionSnapshotService::applyTransition(
    const ExactMissionTransferToken &token,
    const MissionTransferService::Transition &transition)
{
    if (!tokenIsCurrent(token) || !transition.handled) {
        return false;
    }
    disarmTimeout();

    bool sent = true;
    QString sendError;
    if (transition.outbound) {
        QPointer<ExactMissionSnapshotService> guard(this);
        sent = sendOutbound(token, *transition.outbound, &sendError);
        if (!guard) {
            return false;
        }
        if (!tokenIsCurrent(token)) {
            return false;
        }
    }

    if (!sent && !terminalState(m_transfer.state())) {
        m_transfer.fail(sendError.isEmpty()
            ? QStringLiteral("Unable to send an exact mission protocol message.")
            : sendError);
    }
    if (terminalState(m_transfer.state())) {
        const bool completed =
            m_transfer.state() == MissionTransferService::State::Complete;
        finishActive(token);
        // A terminal ACK is best effort. Once the complete item sequence has
        // been received, its snapshot remains authoritative even if that ACK
        // cannot be written.
        return sent || completed;
    }
    if (m_transfer.isActive()) {
        armTimeout(token);
    }
    return sent;
}

void ExactMissionSnapshotService::failActive(
    const ExactMissionTransferToken &token,
    const QString &reason,
    MAV_MISSION_RESULT result)
{
    if (!tokenIsCurrent(token)) {
        return;
    }
    disarmTimeout();
    m_transfer.fail(reason, result);
    finishActive(token);
}

void ExactMissionSnapshotService::finishActive(
    const ExactMissionTransferToken &token)
{
    if (!tokenIsCurrent(token)) {
        return;
    }
    disarmTimeout();

    ExactMissionTransferResult result;
    result.token = token;
    result.key = m_active.key;
    result.state = m_transfer.state();
    result.missionResult = m_transfer.missionResult();
    result.errorString = m_transfer.errorString();

    bool replaced = false;
    if (result.state == MissionTransferService::State::Complete
        && result.missionResult == MAV_MISSION_ACCEPTED) {
        const QVector<mavlink_mission_item_int_t> items =
            m_transfer.downloadedItems();
        if (!exactLeaseIsCurrent(result.key.vehicle)) {
            result.state = MissionTransferService::State::Error;
            result.missionResult = MAV_MISSION_ERROR;
            result.errorString = QStringLiteral(
                "The exact vehicle instance changed before snapshot commit.");
        } else if (!validCompletedItems(items, result.key.missionType)) {
            result.state = MissionTransferService::State::Error;
            result.missionResult = MAV_MISSION_INVALID_SEQUENCE;
            result.errorString = QStringLiteral(
                "The completed mission sequence is not canonical.");
        } else {
            result.snapshot = commitCompletedSnapshot(result.key, items);
            replaced = true;
        }
    }

    if (m_active.ownerDestroyed) {
        disconnect(m_active.ownerDestroyed);
    }
    QPointer<ExactMissionSnapshotService> releaseGuard(this);
    if (m_active.coordinator
        && m_active.coordinator->owns(m_active.coordinatorLease)) {
        m_active.coordinator->release(m_active.coordinatorLease);
    }
    if (!releaseGuard) {
        return;
    }
    if (replaced) {
        const auto current = m_snapshots.constFind(result.key);
        replaced = current != m_snapshots.constEnd()
            && current->observationRevision
                == result.snapshot.observationRevision;
    }
    m_active = ActiveTransfer();
    m_transfer.reset();

    QPointer<ExactMissionSnapshotService> guard(this);
    const bool previousOperationState = m_operationInFlight;
    m_operationInFlight = true;
    ScopeExit operationGuard([guard, previousOperationState]() {
        if (guard) {
            guard->m_operationInFlight = previousOperationState;
        }
    });
    if (replaced) {
        // Publish the immutable replacement fact before opening any generic
        // state-notification reentrancy window. A receiver may retire the
        // endpoint, in which case snapshotInvalidated follows this signal and
        // the stale replacement is never announced afterwards.
        emit snapshotReplaced(result.snapshot);
        if (!guard) {
            return;
        }
        const auto current = m_snapshots.constFind(result.key);
        if (current != m_snapshots.constEnd()
            && current->observationRevision
                == result.snapshot.observationRevision) {
            emit revisionChanged(m_revision);
            if (!guard) {
                return;
            }
        }
    }
    emit transferFinished(result);
    if (!guard) {
        return;
    }
    if (previousOperationState) {
        // An immediate terminal transition can occur inside requestDownload().
        // Do not announce reusable capacity until that outer API call has
        // unwound and released its reentrancy guard.
        QTimer::singleShot(0, guard.data(), [guard]() {
            if (guard && !guard->busy()
                && !guard->m_operationInFlight) {
                emit guard->busyChanged(false);
            }
        });
        return;
    }
    m_operationInFlight = false;
    emit busyChanged(false);
}

ExactMissionSnapshot ExactMissionSnapshotService::commitCompletedSnapshot(
    const ExactMissionKey &key,
    const QVector<mavlink_mission_item_int_t> &items)
{
    ExactMissionSnapshot snapshot;
    snapshot.key = key;
    snapshot.items = items;
    snapshot.contentDigest = contentDigestFor(items);
    snapshot.waypointLeaderSignature = waypointLeaderSignatureFor(items);
    snapshot.confirmedAtMs = nowMs();
    snapshot.observationRevision = nextObservationRevision();

    const auto previous = m_snapshots.constFind(key);
    snapshot.contentGeneration =
        previous != m_snapshots.constEnd()
            && previous->contentDigest == snapshot.contentDigest
        ? previous->contentGeneration : nextContentGeneration();
    m_snapshots.insert(key, snapshot);
    m_snapshotCoordinators.insert(key, m_active.coordinator);
    return snapshot;
}

void ExactMissionSnapshotService::watchCoordinator(
    MissionProtocolCoordinator *coordinator)
{
    if (!coordinator) {
        return;
    }
    for (const CoordinatorWatch &watch : m_coordinatorWatches) {
        if (watch.coordinator == coordinator) {
            return;
        }
    }

    CoordinatorWatch watch;
    watch.coordinator = coordinator;
    watch.leaseChanged = connect(
        coordinator, &MissionProtocolCoordinator::leaseChanged,
        this,
        [this, coordinator](
            QObject *owner,
            MissionProtocolCoordinator::MissionType missionType,
            qulonglong) {
            if (!owner || owner == this) {
                return;
            }
            QList<ExactMissionKey> invalidated;
            for (auto item = m_snapshotCoordinators.constBegin();
                 item != m_snapshotCoordinators.constEnd(); ++item) {
                if (item.value() == coordinator
                    && coordinatorMissionType(item.key().missionType)
                        == missionType) {
                    invalidated.append(item.key());
                }
            }
            QPointer<ExactMissionSnapshotService> guard(this);
            for (const ExactMissionKey &key : invalidated) {
                invalidateKey(
                    key, InvalidationReason::KnownMissionMutation);
                if (!guard) {
                    return;
                }
            }
        });
    watch.destroyed = connect(
        coordinator, &QObject::destroyed, this,
        [this, coordinator](QObject *) {
            QList<ExactMissionKey> invalidated;
            for (auto item = m_snapshotCoordinators.constBegin();
                 item != m_snapshotCoordinators.constEnd(); ++item) {
                if (item.value() == coordinator) {
                    invalidated.append(item.key());
                }
            }
            QPointer<ExactMissionSnapshotService> guard(this);
            for (const ExactMissionKey &key : invalidated) {
                invalidateKey(
                    key, InvalidationReason::KnownMissionMutation);
                if (!guard) {
                    return;
                }
            }
        });
    m_coordinatorWatches.append(watch);
}

bool ExactMissionSnapshotService::invalidateKey(
    const ExactMissionKey &key, InvalidationReason reason)
{
    const auto found = m_snapshots.constFind(key);
    if (found == m_snapshots.constEnd()) {
        return false;
    }
    m_snapshots.remove(key);
    m_snapshotCoordinators.remove(key);
    nextObservationRevision();
    PendingSnapshotEvent event;
    event.key = key;
    event.reason = reason;
    publishSnapshotEvents({event});
    return true;
}

void ExactMissionSnapshotService::invalidateVehicle(
    const SwarmVehicleInstanceLease &vehicle,
    InvalidationReason reason)
{
    QList<ExactMissionKey> removed;
    for (auto item = m_snapshots.constBegin();
         item != m_snapshots.constEnd(); ++item) {
        if (item.key().vehicle.sameInstance(vehicle)) {
            removed.append(item.key());
        }
    }
    if (removed.isEmpty()) {
        return;
    }
    QList<PendingSnapshotEvent> events;
    for (const ExactMissionKey &key : removed) {
        m_snapshots.remove(key);
        m_snapshotCoordinators.remove(key);
        PendingSnapshotEvent event;
        event.key = key;
        event.reason = reason;
        events.append(event);
    }
    nextObservationRevision();
    publishSnapshotEvents(events);
}

void ExactMissionSnapshotService::invalidateAll(InvalidationReason reason)
{
    if (m_snapshots.isEmpty()) {
        return;
    }
    QList<PendingSnapshotEvent> events;
    events.reserve(m_snapshots.size());
    for (auto item = m_snapshots.constBegin();
         item != m_snapshots.constEnd(); ++item) {
        PendingSnapshotEvent event;
        event.key = item.key();
        event.reason = reason;
        events.append(event);
    }
    m_snapshots.clear();
    m_snapshotCoordinators.clear();
    nextObservationRevision();
    publishSnapshotEvents(events);
}

void ExactMissionSnapshotService::publishSnapshotEvents(
    QList<PendingSnapshotEvent> events)
{
    if (events.isEmpty()) {
        return;
    }
    QPointer<ExactMissionSnapshotService> guard(this);
    emit revisionChanged(m_revision);
    if (!guard) {
        return;
    }
    for (const PendingSnapshotEvent &event : events) {
        if (event.replaced) {
            emit snapshotReplaced(event.snapshot);
        } else {
            emit snapshotInvalidated(event.key, event.reason);
        }
        if (!guard) {
            return;
        }
    }
}

void ExactMissionSnapshotService::armTimeout(
    const ExactMissionTransferToken &token)
{
    disarmTimeout();
    if (!tokenIsCurrent(token)) {
        return;
    }
    m_timerToken = token.id;
    m_timeoutConnection = connect(
        &m_timeoutTimer, &QTimer::timeout, this,
        [this, token]() {
        if (tokenIsCurrent(token) && m_timerToken == token.id) {
            timeout();
        }
    });
    m_timeoutTimer.start();
}

void ExactMissionSnapshotService::disarmTimeout()
{
    m_timeoutTimer.stop();
    if (m_timeoutConnection) {
        disconnect(m_timeoutConnection);
    }
    m_timeoutConnection = QMetaObject::Connection();
    m_timerToken = 0;
}

qint64 ExactMissionSnapshotService::nowMs() const
{
    const qint64 sampled = m_clock ? m_clock() : m_monotonicClock.elapsed();
    const qint64 nonNegative = std::max<qint64>(0, sampled);
    m_lastNowMs = std::max(m_lastNowMs, nonNegative);
    return m_lastNowMs;
}

quint64 ExactMissionSnapshotService::nextContentGeneration()
{
    ++m_nextContentGeneration;
    if (m_nextContentGeneration == 0) {
        ++m_nextContentGeneration;
    }
    return m_nextContentGeneration;
}

quint64 ExactMissionSnapshotService::nextObservationRevision()
{
    ++m_revision;
    if (m_revision == 0) {
        ++m_revision;
    }
    return m_revision;
}

void ExactMissionSnapshotService::assertServiceThread() const
{
    Q_ASSERT(QThread::currentThread() == thread());
}
