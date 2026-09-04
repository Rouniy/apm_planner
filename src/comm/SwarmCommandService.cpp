#include "SwarmCommandService.h"

#include "ExactLinkTransmitter.h"
#include "SwarmFlightMode.h"

#include <QSet>
#include <QScopedValueRollback>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

namespace
{

constexpr quint16 CleanPositionVelocityMask =
    POSITION_TARGET_TYPEMASK_AX_IGNORE
    | POSITION_TARGET_TYPEMASK_AY_IGNORE
    | POSITION_TARGET_TYPEMASK_AZ_IGNORE
    | POSITION_TARGET_TYPEMASK_YAW_IGNORE
    | POSITION_TARGET_TYPEMASK_YAW_RATE_IGNORE;
constexpr quint16 CleanPositionOnlyMask = CleanPositionVelocityMask
    | POSITION_TARGET_TYPEMASK_VX_IGNORE
    | POSITION_TARGET_TYPEMASK_VY_IGNORE
    | POSITION_TARGET_TYPEMASK_VZ_IGNORE;

} // namespace

void SwarmCommandService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

SwarmCommandService::SwarmCommandService(
    SwarmTelemetryRegistry *registry,
    ExactLinkTransmitter *transmitter,
    RouteValidator routeValidator,
    QObject *parent)
    : SwarmCommandService(registry, transmitter, std::move(routeValidator),
                          Clock(), parent)
{
}

SwarmCommandService::SwarmCommandService(
    SwarmTelemetryRegistry *registry,
    ExactLinkTransmitter *transmitter,
    RouteValidator routeValidator,
    Clock clock,
    QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_transmitter(transmitter)
    , m_routeValidator(std::move(routeValidator))
    , m_clock(std::move(clock))
{
    m_monotonicClock.start();
    qRegisterMetaType<SwarmCommandService::Result>();
    qRegisterMetaType<SwarmCommandService::MemberReport>();
    qRegisterMetaType<SwarmCommandService::BatchReport>();
    if (m_registry) {
        connect(m_registry, &SwarmTelemetryRegistry::endpointRetired,
                this, [this](const SwarmVehicleInstanceLease &lease,
                             SwarmTelemetryRegistry::RetirementReason) {
            for (const SwarmCommandMember &member : m_active.members) {
                if (member.lease.sameInstance(lease)) {
                    cancelActive(
                        QStringLiteral("A reserved swarm vehicle was retired; the command session stopped."),
                        true);
                    return;
                }
            }
        });
        connect(m_registry, &QObject::destroyed, this, [this]() {
            cancelActive(QStringLiteral("Swarm telemetry registry is unavailable."),
                         true);
        });
    }
    if (m_transmitter) {
        connect(m_transmitter, &QObject::destroyed, this, [this]() {
            cancelActive(QStringLiteral("Exact MAVLink transmitter is unavailable."),
                         true);
        });
    }
}

bool SwarmCommandService::routeIsEligible(
    const SwarmVehicleInstanceLease &lease, QString *error) const
{
    if (error) {
        error->clear();
    }
    if (!m_registry || !lease.isValid()
        || !m_registry->validateLease(lease)) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle instance is unavailable or stale.");
        }
        return false;
    }
    SwarmCommandMember member;
    member.lease = lease;
    return validateRoute(member, error);
}

SwarmCommandService::Result SwarmCommandService::reserve(
    QObject *owner, const QVector<SwarmCommandMember> &requestedMembers,
    int maximumBatchHz, SwarmCommandSessionToken *token, QString *error)
{
    if (token) {
        *token = SwarmCommandSessionToken();
    }
    if (error) {
        error->clear();
    }
    if (!owner || !token || !m_registry || !m_transmitter
        || !m_routeValidator || requestedMembers.isEmpty()
        || requestedMembers.size()
            > SwarmTelemetryRegistry::MaximumVehicleEndpoints
        || maximumBatchHz < 1 || maximumBatchHz > 10) {
        if (error) {
            *error = QStringLiteral("The swarm command plan is invalid.");
        }
        return Result::InvalidPlan;
    }
    if (m_operationInFlight || hasActiveSession()) {
        if (error) {
            *error = QStringLiteral(
                "Another swarm command workflow is already active.");
        }
        return Result::Busy;
    }
    QScopedValueRollback<bool> operationGuard(m_operationInFlight, true);
    QPointer<QObject> guardedOwner(owner);

    QVector<SwarmCommandMember> members = requestedMembers;
    std::sort(members.begin(), members.end(),
              [](const SwarmCommandMember &left,
                 const SwarmCommandMember &right) {
        return left.slotId < right.slotId;
    });
    QSet<int> occupiedSlots;
    for (int index = 0; index < members.size(); ++index) {
        const SwarmCommandMember &member = members.at(index);
        if (member.slotId <= 0 || occupiedSlots.contains(member.slotId)
            || !member.lease.isValid()) {
            if (error) {
                *error = QStringLiteral(
                    "Swarm slots and exact vehicle instances must be unique and valid.");
            }
            return Result::InvalidPlan;
        }
        occupiedSlots.insert(member.slotId);
        const SwarmTelemetryRequirements &required = member.required;
        constexpr int KnownTelemetryFields =
            SwarmTelemetryRequirements::Position
            | SwarmTelemetryRequirements::Velocity
            | SwarmTelemetryRequirements::Heading
            | SwarmTelemetryRequirements::Attitude
            | SwarmTelemetryRequirements::VfrHud
            | SwarmTelemetryRequirements::ExtendedSystemState;
        const auto validAge = [](int value) {
            return value >= 1 && value <= 10 * 60 * 1000;
        };
        const bool knownFlightMode = member.flightMode
                == SwarmCommandMember::FlightModeRequirement::Any
            || member.flightMode
                == SwarmCommandMember::FlightModeRequirement::ArduPilotGuided;
        if ((int(required.fields) & ~KnownTelemetryFields) != 0
            || !knownFlightMode
            || !validAge(required.heartbeatMaximumAgeMs)
            || !validAge(required.positionMaximumAgeMs)
            || !validAge(required.velocityMaximumAgeMs)
            || !validAge(required.headingMaximumAgeMs)
            || !validAge(required.attitudeMaximumAgeMs)
            || !validAge(required.vfrHudMaximumAgeMs)
            || !validAge(required.extendedSystemStateMaximumAgeMs)) {
            if (error) {
                *error = QStringLiteral(
                    "Swarm telemetry field or age requirements are invalid.");
            }
            return Result::InvalidPlan;
        }
        for (int previous = 0; previous < index; ++previous) {
            if (member.lease.sameInstance(members.at(previous).lease)
                || member.lease.endpoint.sameIdentity(
                    members.at(previous).lease.endpoint)) {
                if (error) {
                    *error = QStringLiteral(
                        "One exact vehicle endpoint cannot occupy two swarm slots.");
                }
                return Result::InvalidPlan;
            }
        }
        QString memberError;
        // Stream requests may be needed to obtain operation fields. Reserve
        // only exact identity and heartbeat; operation preflight applies the
        // full field requirements.
        Result memberFailure = Result::StaleLease;
        if (!validateMember(member, false, &memberFailure, &memberError)) {
            if (error) {
                *error = memberError;
            }
            return memberFailure;
        }
        if (!validateRoute(member, &memberError)) {
            if (error) {
                *error = memberError;
            }
            return Result::UnsafeRoute;
        }
        if (!validateMember(member, false, &memberFailure, &memberError)) {
            if (error) {
                *error = memberError;
            }
            return memberFailure;
        }
        if (m_active.id != 0) {
            if (error) {
                *error = QStringLiteral(
                    "The swarm reservation changed during route validation.");
            }
            return Result::Busy;
        }
        if (!guardedOwner) {
            if (error) {
                *error = QStringLiteral(
                    "The swarm command owner was destroyed during validation.");
            }
            return Result::Cancelled;
        }
    }

    SwarmVehicleGroupLease exactGroup;
    for (const SwarmCommandMember &member : members) {
        exactGroup.members.append(member.lease);
    }
    if (!m_registry->validateGroup(exactGroup) || !guardedOwner) {
        if (error) {
            *error = QStringLiteral(
                "The swarm group changed during reservation validation.");
        }
        return Result::StaleLease;
    }

    m_active.id = nextSessionId();
    m_active.owner = guardedOwner;
    m_active.members = members;
    m_active.maximumBatchHz = maximumBatchHz;
    m_active.lastBatchMs = -1;
    m_active.lastStreamRequestMs = -1;
    const quint64 capturedId = m_active.id;
    m_active.ownerDestroyed = connect(guardedOwner.data(), &QObject::destroyed, this,
                                      [this, capturedId]() {
        if (m_active.id == capturedId) {
            cancelActive(QStringLiteral(
                "The swarm command owner was destroyed."), true);
        }
    });
    token->id = m_active.id;
    return Result::Reserved;
}

SwarmCommandService::Result SwarmCommandService::release(
    const SwarmCommandSessionToken &token)
{
    if (!tokenIsCurrent(token)) {
        return Result::InvalidSession;
    }
    cancelActive(QString(), false);
    return Result::Cancelled;
}

SwarmCommandService::BatchReport
SwarmCommandService::requestPositionAndAttitudeStreams(
    const SwarmCommandSessionToken &token,
    const QVector<int> &slotIds, int rateHz)
{
    return requestStreams(token, slotIds, rateHz, true);
}

SwarmCommandService::BatchReport
SwarmCommandService::requestPositionStreams(
    const SwarmCommandSessionToken &token,
    const QVector<int> &slotIds, int rateHz)
{
    return requestStreams(token, slotIds, rateHz, false);
}

SwarmCommandService::BatchReport SwarmCommandService::requestStreams(
    const SwarmCommandSessionToken &token,
    const QVector<int> &slotIds, int rateHz, bool includeAttitude)
{
    if (!tokenIsCurrent(token)) {
        return preflightReport(token.id, Result::InvalidSession,
                               QStringLiteral("The swarm session is no longer active."),
                               false);
    }
    if (slotIds.isEmpty() || rateHz < 1 || rateHz > 10) {
        return preflightReport(token.id, Result::InvalidPlan,
                               QStringLiteral("The stream request is invalid."));
    }
    QVector<int> orderedSlots = slotIds;
    std::sort(orderedSlots.begin(), orderedSlots.end());
    QSet<int> unique;
    QVector<QPair<SwarmCommandMember, mavlink_message_t>> messages;
    messages.reserve(slotIds.size() * (includeAttitude ? 2 : 1));
    for (int slotId : orderedSlots) {
        const SwarmCommandMember *member = memberForSlot(slotId);
        if (!member || unique.contains(slotId)) {
            return preflightReport(
                token.id, Result::InvalidPlan,
                QStringLiteral("A requested stream slot is absent or duplicated."));
        }
        unique.insert(slotId);
        messages.append(qMakePair(
            *member, streamRequestMessage(
                *member, MAV_DATA_STREAM_POSITION, rateHz)));
        if (includeAttitude) {
            messages.append(qMakePair(
                *member, streamRequestMessage(
                    *member, MAV_DATA_STREAM_EXTRA1, rateHz)));
        }
    }
    return sendMessages(token, messages, false, false);
}

SwarmCommandService::BatchReport SwarmCommandService::sendPositionTargets(
    const SwarmCommandSessionToken &token,
    const QVector<SwarmPositionTarget> &targets)
{
    if (!tokenIsCurrent(token)) {
        return preflightReport(token.id, Result::InvalidSession,
                               QStringLiteral("The swarm session is no longer active."),
                               false);
    }
    if (targets.isEmpty()) {
        return preflightReport(token.id, Result::InvalidPlan,
                               QStringLiteral("No position targets were supplied."));
    }

    QVector<SwarmPositionTarget> orderedTargets = targets;
    std::sort(orderedTargets.begin(), orderedTargets.end(),
              [](const SwarmPositionTarget &left,
                 const SwarmPositionTarget &right) {
        return left.slotId < right.slotId;
    });
    QSet<int> unique;
    QVector<QPair<SwarmCommandMember, mavlink_message_t>> messages;
    messages.reserve(targets.size());
    for (const SwarmPositionTarget &target : orderedTargets) {
        const SwarmCommandMember *member = memberForSlot(target.slotId);
        if (!member || unique.contains(target.slotId)) {
            return preflightReport(
                token.id, Result::InvalidPlan,
                QStringLiteral("A target slot is absent or duplicated."));
        }
        if (!validTarget(target)) {
            return preflightReport(
                token.id, Result::InvalidPayload,
                QStringLiteral("A position target is non-finite or out of range."));
        }
        unique.insert(target.slotId);
        messages.append(qMakePair(*member, positionMessage(*member, target)));
    }
    return sendMessages(token, messages, true, true);
}

bool SwarmCommandService::hasActiveSession() const noexcept
{
    return m_active.id != 0 && !m_active.owner.isNull();
}

bool SwarmCommandService::validTarget(const SwarmPositionTarget &target)
{
    const bool validNumbers = target.slotId > 0
        && std::isfinite(target.latitudeDegrees)
        && std::isfinite(target.longitudeDegrees)
        && std::isfinite(target.relativeAltitudeM)
        && std::isfinite(target.velocityNorthMps)
        && std::isfinite(target.velocityEastMps)
        && std::isfinite(target.velocityDownMps)
        && target.latitudeDegrees >= -90.0
        && target.latitudeDegrees <= 90.0
        && target.longitudeDegrees >= -180.0
        && target.longitudeDegrees <= 180.0
        && std::abs(target.relativeAltitudeM) <= 100000.0F
        && std::abs(target.velocityNorthMps) <= 1000.0F
        && std::abs(target.velocityEastMps) <= 1000.0F
        && std::abs(target.velocityDownMps) <= 1000.0F;
    if (!validNumbers) {
        return false;
    }
    const qint64 latitudeE7 = std::llround(target.latitudeDegrees * 1.0e7);
    const qint64 longitudeE7 = std::llround(target.longitudeDegrees * 1.0e7);
    return latitudeE7 != 0 || longitudeE7 != 0;
}

quint64 SwarmCommandService::nextSessionId()
{
    static std::atomic<quint64> next{1};
    quint64 id = next.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        id = next.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
}

QString SwarmCommandService::endpointLabel(
    const SwarmVehicleInstanceLease &lease)
{
    return QStringLiteral("%1 — %2:%3")
        .arg(lease.endpoint.linkName.isEmpty()
                 ? QStringLiteral("Link %1").arg(lease.endpoint.linkId)
                 : lease.endpoint.linkName)
        .arg(lease.endpoint.systemId)
        .arg(lease.endpoint.componentId);
}

mavlink_message_t SwarmCommandService::positionMessage(
    const SwarmCommandMember &member,
    const SwarmPositionTarget &target) const
{
    mavlink_set_position_target_global_int_t payload{};
    payload.time_boot_ms = 0;
    payload.target_system = static_cast<quint8>(
        member.lease.endpoint.systemId);
    payload.target_component = static_cast<quint8>(
        member.lease.endpoint.componentId);
    payload.coordinate_frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    payload.type_mask = target.useVelocity
        ? CleanPositionVelocityMask : CleanPositionOnlyMask;
    payload.lat_int = static_cast<qint32>(
        std::llround(target.latitudeDegrees * 1.0e7));
    payload.lon_int = static_cast<qint32>(
        std::llround(target.longitudeDegrees * 1.0e7));
    payload.alt = target.relativeAltitudeM;
    payload.vx = target.velocityNorthMps;
    payload.vy = target.velocityEastMps;
    payload.vz = target.velocityDownMps;
    mavlink_message_t message{};
    mavlink_msg_set_position_target_global_int_encode(
        m_localSystemId, m_localComponentId, &message, &payload);
    return message;
}

mavlink_message_t SwarmCommandService::streamRequestMessage(
    const SwarmCommandMember &member, int streamId, int rateHz) const
{
    mavlink_request_data_stream_t payload{};
    payload.target_system = static_cast<quint8>(
        member.lease.endpoint.systemId);
    payload.target_component = static_cast<quint8>(
        member.lease.endpoint.componentId);
    payload.req_stream_id = static_cast<quint8>(streamId);
    payload.req_message_rate = static_cast<quint16>(rateHz);
    payload.start_stop = 1;
    mavlink_message_t message{};
    mavlink_msg_request_data_stream_encode(
        m_localSystemId, m_localComponentId, &message, &payload);
    return message;
}

const SwarmCommandMember *SwarmCommandService::memberForSlot(int slotId) const
{
    for (const SwarmCommandMember &member : m_active.members) {
        if (member.slotId == slotId) {
            return &member;
        }
    }
    return nullptr;
}

bool SwarmCommandService::tokenIsCurrent(
    const SwarmCommandSessionToken &token) const
{
    return token.isValid() && hasActiveSession() && token.id == m_active.id;
}

bool SwarmCommandService::validateMember(
    const SwarmCommandMember &member, bool requireTelemetryFields,
    Result *failure, QString *error) const
{
    if (failure) {
        *failure = Result::StaleLease;
    }
    if (!m_registry || !member.lease.isValid()) {
        if (error) {
            *error = QStringLiteral("An exact swarm vehicle lease is invalid.");
        }
        return false;
    }
    SwarmTelemetrySnapshot snapshot;
    if (!m_registry->snapshotForLease(member.lease, &snapshot)
        || !m_registry->validateLease(
            member.lease, member.required.heartbeatMaximumAgeMs)) {
        if (error) {
            *error = QStringLiteral("%1 exact vehicle lease is stale.")
                .arg(endpointLabel(member.lease));
        }
        return false;
    }
    const SwarmTelemetryRequirements &required = member.required;
    if (member.flightMode
            == SwarmCommandMember::FlightModeRequirement::ArduPilotGuided
        && !SwarmFlightMode::isExactGuided(snapshot)) {
        if (failure) {
            *failure = Result::RejectedBeforeSend;
        }
        if (error) {
            *error = QStringLiteral(
                "%1 is in %2, not the exact ArduPilot GUIDED mode.")
                .arg(endpointLabel(member.lease),
                     SwarmFlightMode::displayName(snapshot));
        }
        return false;
    }
    if (!requireTelemetryFields) {
        if (error) {
            error->clear();
        }
        return true;
    }
    const auto fresh = [this](qint64 observedMs, int maximumAgeMs) {
        return m_registry
            && m_registry->observationIsFresh(observedMs, maximumAgeMs);
    };
    QString field;
    if (required.fields.testFlag(SwarmTelemetryRequirements::Position)
        && (!snapshot.positionValid
            || !fresh(snapshot.positionObservedMs,
                      required.positionMaximumAgeMs))) {
        field = QStringLiteral("position");
    } else if (required.fields.testFlag(SwarmTelemetryRequirements::Velocity)
               && (!snapshot.velocityValid
                   || !fresh(snapshot.positionObservedMs,
                             required.velocityMaximumAgeMs))) {
        field = QStringLiteral("velocity");
    } else if (required.fields.testFlag(SwarmTelemetryRequirements::Heading)
               && (!snapshot.headingValid
                   || !(fresh(snapshot.positionObservedMs,
                              required.headingMaximumAgeMs)
                        || fresh(snapshot.vfrHudObservedMs,
                                 required.headingMaximumAgeMs)))) {
        field = QStringLiteral("heading");
    } else if (required.fields.testFlag(SwarmTelemetryRequirements::Attitude)
               && (!snapshot.attitudeValid
                   || !fresh(snapshot.attitudeObservedMs,
                             required.attitudeMaximumAgeMs))) {
        field = QStringLiteral("attitude");
    } else if (required.fields.testFlag(SwarmTelemetryRequirements::VfrHud)
               && (!snapshot.vfrHudValid
                   || !fresh(snapshot.vfrHudObservedMs,
                             required.vfrHudMaximumAgeMs))) {
        field = QStringLiteral("VFR HUD");
    } else if (required.fields.testFlag(
                   SwarmTelemetryRequirements::ExtendedSystemState)
               && (!snapshot.extendedSystemStateValid
                   || !fresh(snapshot.extendedSystemStateObservedMs,
                             required.extendedSystemStateMaximumAgeMs))) {
        field = QStringLiteral("extended system state");
    }
    if (!field.isEmpty()) {
        if (failure) {
            *failure = Result::TelemetryStale;
        }
        if (error) {
            *error = QStringLiteral("%1 %2 telemetry is unavailable or stale.")
                .arg(endpointLabel(member.lease), field);
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool SwarmCommandService::validateAll(
    QList<SwarmTelemetrySnapshot> *snapshots,
    bool requireTelemetryFields, Result *failure,
    QString *error) const
{
    if (failure) {
        *failure = Result::StaleLease;
    }
    if (snapshots) {
        snapshots->clear();
    }
    if (!m_registry || !hasActiveSession()) {
        if (error) {
            *error = QStringLiteral("The swarm session is no longer active.");
        }
        return false;
    }
    SwarmVehicleGroupLease group;
    for (const SwarmCommandMember &member : m_active.members) {
        group.members.append(member.lease);
    }
    if (!m_registry->validateGroup(group, snapshots)) {
        if (error) {
            *error = QStringLiteral(
                "A reserved swarm vehicle disappeared, expired, or was reconnected.");
        }
        return false;
    }
    const QVector<SwarmCommandMember> members = m_active.members;
    const quint64 capturedId = m_active.id;
    for (const SwarmCommandMember &member : members) {
        QString memberError;
        Result memberFailure = Result::StaleLease;
        if (!validateMember(member, requireTelemetryFields,
                            &memberFailure, &memberError)) {
            if (error) {
                *error = memberError;
            }
            if (failure) {
                *failure = memberFailure;
            }
            return false;
        }
        if (!validateRoute(member, &memberError)) {
            if (error) {
                *error = memberError;
            }
            if (failure) {
                *failure = Result::UnsafeRoute;
            }
            return false;
        }
        if (m_active.id != capturedId) {
            if (error) {
                *error = QStringLiteral(
                    "The swarm session changed during validation.");
            }
            return false;
        }
        if (!validateMember(member, requireTelemetryFields,
                            &memberFailure, &memberError)) {
            if (error) {
                *error = memberError;
            }
            if (failure) {
                *failure = memberFailure;
            }
            return false;
        }
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool SwarmCommandService::validateRoute(
    const SwarmCommandMember &member, QString *error) const
{
    if (!m_routeValidator || !m_registry
        || m_registry->currentLinkSessionEpoch(
               member.lease.endpoint.linkId)
            != member.lease.linkSessionEpoch) {
        if (error) {
            *error = QStringLiteral("Exact swarm route validation is unavailable.");
        }
        return false;
    }
    QString routeError;
    if (!m_routeValidator(member.lease, &routeError)) {
        if (error) {
            *error = routeError.trimmed().isEmpty()
                ? QStringLiteral("%1 does not have a unique writable route.")
                    .arg(endpointLabel(member.lease))
                : routeError;
        }
        return false;
    }
    if (!m_registry
        || !m_registry->validateLease(
            member.lease, member.required.heartbeatMaximumAgeMs)
        || m_registry->currentLinkSessionEpoch(
               member.lease.endpoint.linkId)
            != member.lease.linkSessionEpoch) {
        if (error) {
            *error = QStringLiteral(
                "%1 exact vehicle session changed during route validation.")
                .arg(endpointLabel(member.lease));
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

SwarmCommandService::BatchReport SwarmCommandService::preflightReport(
    quint64 requestedSessionId, Result result, const QString &detail,
    bool includeActiveMembers) const
{
    BatchReport report;
    report.sessionId = requestedSessionId;
    report.result = result;
    report.detail = detail;
    const QVector<SwarmCommandMember> members = includeActiveMembers
        && requestedSessionId == m_active.id
        ? m_active.members : QVector<SwarmCommandMember>();
    for (const SwarmCommandMember &member : members) {
        MemberReport item;
        item.slotId = member.slotId;
        item.lease = member.lease;
        item.result = result;
        item.detail = detail;
        report.members.append(item);
    }
    return report;
}

SwarmCommandService::BatchReport SwarmCommandService::sendMessages(
    const SwarmCommandSessionToken &token,
    const QVector<QPair<SwarmCommandMember, mavlink_message_t>> &messages,
    bool rateLimited, bool requireTelemetryFields)
{
    if (!tokenIsCurrent(token)) {
        return preflightReport(token.id, Result::InvalidSession,
                               QStringLiteral("The swarm session is no longer active."),
                               false);
    }
    if (m_operationInFlight) {
        return preflightReport(token.id, Result::Busy,
                               QStringLiteral("Another swarm batch is in progress."));
    }
    QScopedValueRollback<bool> operationGuard(m_operationInFlight, true);
    const quint64 capturedId = token.id;
    const quint8 localSystemId = m_localSystemId;
    const quint8 localComponentId = m_localComponentId;
    QPointer<ExactLinkTransmitter> transmitter = m_transmitter;
    QString error;
    Result validationFailure = Result::StaleLease;
    if (!validateAll(nullptr, requireTelemetryFields,
                     &validationFailure, &error)) {
        return preflightReport(
            token.id, validationFailure, error);
    }
    const qint64 current = nowMs();
    const qint64 previousSend = rateLimited
        ? m_active.lastBatchMs : m_active.lastStreamRequestMs;
    const qint64 minimumInterval = rateLimited
        ? (1000 + m_active.maximumBatchHz - 1) / m_active.maximumBatchHz
        : 1000;
    if (previousSend >= 0) {
        if (current < previousSend
            || current - previousSend < minimumInterval) {
            return preflightReport(
                token.id, Result::RateLimited,
                QStringLiteral("The swarm batch rate limit was exceeded."));
        }
    }
    // Consume the rate slot before the first callback/physical write. A
    // partially sent batch must not be retried as an unbounded burst.
    if (m_active.id == capturedId) {
        if (rateLimited) {
            m_active.lastBatchMs = current;
        } else {
            m_active.lastStreamRequestMs = current;
        }
    }

    BatchReport report;
    report.sessionId = capturedId;
    report.result = Result::SentAll;
    for (const auto &message : messages) {
        auto existing = std::find_if(
            report.members.begin(), report.members.end(),
            [&message](const MemberReport &candidate) {
                return candidate.slotId == message.first.slotId;
            });
        if (existing == report.members.end()) {
            MemberReport memberReport;
            memberReport.slotId = message.first.slotId;
            memberReport.lease = message.first.lease;
            memberReport.framesPlanned = 1;
            report.members.append(memberReport);
        } else {
            ++existing->framesPlanned;
        }
    }
    int sentCount = 0;
    for (int index = 0; index < messages.size(); ++index) {
        const SwarmCommandMember &member = messages.at(index).first;
        auto item = std::find_if(
            report.members.begin(), report.members.end(),
            [&member](const MemberReport &candidate) {
                return candidate.slotId == member.slotId;
            });
        Q_ASSERT(item != report.members.end());
        if (!tokenIsCurrent(token)) {
            item->result = Result::RejectedBeforeSend;
            item->detail = error.isEmpty()
                ? QStringLiteral("The swarm session changed during transmission.")
                : error;
            report.result = sentCount == 0
                ? Result::RejectedBeforeSend : Result::PartialSend;
            report.detail = item->detail;
            break;
        }
        Result memberFailure = Result::StaleLease;
        if (!validateMember(member, requireTelemetryFields,
                            &memberFailure, &error)
            || !validateRoute(member, &error) || !transmitter) {
            item->result = Result::RejectedBeforeSend;
            item->detail = error.isEmpty()
                ? QStringLiteral("The exact vehicle route changed before transmission.")
                : error;
            report.result = sentCount == 0
                ? Result::RejectedBeforeSend : Result::PartialSend;
            report.detail = item->detail;
            break;
        }
        // RouteValidator is an injected callback and may synchronously retire
        // a link or cancel this session. Re-establish the exact-instance
        // barrier after it returns and immediately before the physical write.
        QString postRouteError;
        Result postRouteFailure = Result::StaleLease;
        if (!validateMember(member, requireTelemetryFields,
                            &postRouteFailure, &postRouteError)
            || !tokenIsCurrent(token)
            || m_registry->currentLinkSessionEpoch(
                   member.lease.endpoint.linkId)
                != member.lease.linkSessionEpoch
            || !transmitter) {
            item->result = Result::RejectedBeforeSend;
            item->detail = postRouteError.isEmpty()
                ? QStringLiteral(
                    "The exact vehicle session changed during route validation.")
                : postRouteError;
            report.result = sentCount == 0
                ? Result::RejectedBeforeSend : Result::PartialSend;
            report.detail = item->detail;
            break;
        }
        const ExactLinkTransmitter::SendResult sent =
            transmitter->sendMessage(
                member.lease.endpoint.linkId,
                localSystemId, localComponentId,
                messages.at(index).second);
        if (sent != ExactLinkTransmitter::SendResult::Sent) {
            item->result = item->framesSent == 0
                ? Result::TransportUnavailable : Result::PartialSend;
            item->detail = QStringLiteral(
                "Transport failed while sending to %1.")
                .arg(endpointLabel(member.lease));
            report.result = sentCount == 0
                ? Result::TransportUnavailable : Result::PartialSend;
            report.detail = item->detail;
            break;
        }
        ++item->framesSent;
        item->result = item->framesSent == item->framesPlanned
            ? Result::SentAll : Result::PartialSend;
        item->detail = QStringLiteral("Queued %1 of %2 frame(s)")
            .arg(item->framesSent).arg(item->framesPlanned);
        ++sentCount;
        if (m_active.id != capturedId || !m_active.owner) {
            report.result = Result::PartialSend;
            report.detail = QStringLiteral(
                "The swarm session was cancelled during transmission; sent frames may have taken effect.");
            break;
        }
    }
    if (report.result == Result::SentAll) {
        report.detail = QStringLiteral("Queued for every exact swarm member.");
    } else {
        for (MemberReport &item : report.members) {
            if (item.framesSent == item.framesPlanned) {
                item.result = Result::SentAll;
            } else if (item.framesSent > 0) {
                item.result = Result::PartialSend;
            } else if (item.detail.isEmpty()) {
                item.result = Result::RejectedBeforeSend;
                item.detail = QStringLiteral(
                    "Not attempted after the first failure.");
            }
        }
    }
    return report;
}

void SwarmCommandService::cancelActive(
    const QString &reason, bool publishSignal)
{
    if (m_active.id == 0) {
        return;
    }
    const quint64 id = m_active.id;
    if (m_active.ownerDestroyed) {
        disconnect(m_active.ownerDestroyed);
    }
    m_active = ActiveSession();
    if (publishSignal) {
        emit sessionCancelled(id, reason);
    }
}

qint64 SwarmCommandService::nowMs() const
{
    if (m_clock) {
        return m_clock();
    }
    return m_monotonicClock.elapsed();
}
