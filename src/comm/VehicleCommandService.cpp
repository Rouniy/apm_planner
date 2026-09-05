#include "VehicleCommandService.h"

#include "ExactLinkTransmitter.h"
#include "VehicleTargetManager.h"

#include <QPointer>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

namespace
{

constexpr int MaximumExactTimeoutMs = 10 * 60 * 1000;

QString endpointLabel(const VehicleEndpoint &endpoint)
{
    return QStringLiteral("link %1, vehicle %2:%3")
        .arg(endpoint.linkId)
        .arg(endpoint.systemId)
        .arg(endpoint.componentId);
}

} // namespace

VehicleCommandService::VehicleCommandService(
    VehicleTargetManager *targetManager, ExactLinkTransmitter *transmitter,
    QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_transmitter(transmitter)
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_transmitter);
    qRegisterMetaType<ExactReservationToken>();
    qRegisterMetaType<ExactCommandRequest>();
    qRegisterMetaType<ExactCommandToken>();
    qRegisterMetaType<ExactReservationResult>();
    qRegisterMetaType<ExactSubmitResult>();
    qRegisterMetaType<ExactTerminalResult>();
    qRegisterMetaType<ExactCommandReport>();

    m_exactClock.start();
    m_exactDeadlineTimer.setSingleShot(true);
    m_exactDeadlineTimer.setTimerType(Qt::PreciseTimer);
    m_exactQuarantineTimer.setSingleShot(true);
    m_exactQuarantineTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_exactDeadlineTimer, &QTimer::timeout,
            this, &VehicleCommandService::handleExactDeadline);
    connect(&m_exactQuarantineTimer, &QTimer::timeout,
            this, &VehicleCommandService::handleQuarantineExpiry);
    connect(m_targetManager, &VehicleTargetManager::targetGenerationChanged,
            this, [this](qulonglong generation) {
        if (generation == m_targetManager->targetGeneration()) {
            clearPendingCommands();
        }
    });
}

void VehicleCommandService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

bool VehicleCommandService::configureExactTransactions(
    ExactLeaseValidator leaseValidator,
    ExactRouteValidator routeValidator)
{
    if (!m_exactReservations.isEmpty()
        || !m_pendingExactCommands.isEmpty()
        || !leaseValidator || !routeValidator) {
        return false;
    }
    m_exactLeaseValidator = std::move(leaseValidator);
    m_exactRouteValidator = std::move(routeValidator);
    return static_cast<bool>(m_exactLeaseValidator)
        && static_cast<bool>(m_exactRouteValidator);
}

void VehicleCommandService::setExactCommandTimeoutForTesting(int timeoutMs)
{
    m_exactCommandTimeoutMs = qBound(1, timeoutMs, MaximumExactTimeoutMs);
}

void VehicleCommandService::setExactQuarantineForTesting(int timeoutMs)
{
    m_exactQuarantineMs = qBound(1, timeoutMs, MaximumExactTimeoutMs);
    cleanupExpiredQuarantines();
}

VehicleCommandService::ExactReservationResult
VehicleCommandService::reserveExactEndpoints(
    QObject *owner,
    const QList<SwarmVehicleInstanceLease> &leases,
    ExactReservationToken *reservationOut,
    QString *error)
{
    if (reservationOut) {
        *reservationOut = ExactReservationToken();
    }
    if (error) {
        error->clear();
    }
    QPointer<QObject> ownerGuard(owner);
    if (!owner) {
        if (error) {
            *error = QStringLiteral("The exact command owner is missing.");
        }
        return ExactReservationResult::InvalidOwner;
    }
    if (!m_exactLeaseValidator || !m_exactRouteValidator) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle registry or route validator is unavailable.");
        }
        return ExactReservationResult::ContextUnavailable;
    }
    if (leases.isEmpty()
        || leases.size() > SwarmTelemetryRegistry::MaximumVehicleEndpoints) {
        if (error) {
            *error = QStringLiteral("The exact command group is empty or too large.");
        }
        return ExactReservationResult::InvalidLease;
    }

    QSet<VehicleEndpoint> uniqueEndpoints;
    for (const SwarmVehicleInstanceLease &lease : leases) {
        if (!lease.isValid() || uniqueEndpoints.contains(lease.endpoint)) {
            if (error) {
                *error = QStringLiteral(
                    "The exact command group contains an invalid or duplicate endpoint.");
            }
            return ExactReservationResult::InvalidLease;
        }
        uniqueEndpoints.insert(lease.endpoint);
        if (!leaseIsCurrent(lease)) {
            if (error) {
                *error = QStringLiteral("The vehicle instance on %1 is stale.")
                    .arg(endpointLabel(lease.endpoint));
            }
            return ExactReservationResult::StaleLease;
        }
        if (m_exactEndpointReservations.contains(lease.endpoint)
            || m_pendingExactByEndpoint.contains(lease.endpoint)
            || legacyCommandPendingFor(lease.endpoint)) {
            if (error) {
                *error = QStringLiteral("The command channel for %1 is busy.")
                    .arg(endpointLabel(lease.endpoint));
            }
            return ExactReservationResult::Busy;
        }
    }

    // Route validation may invoke application callbacks.  Do not publish a
    // reservation until all routes and then all leases have survived it.
    for (const SwarmVehicleInstanceLease &lease : leases) {
        QString routeError;
        if (!routeIsEligible(lease, &routeError)) {
            if (error) {
                *error = routeError.isEmpty()
                    ? QStringLiteral("No safe route is available for %1.")
                          .arg(endpointLabel(lease.endpoint))
                    : routeError;
            }
            return ExactReservationResult::RouteUnavailable;
        }
        if (ownerGuard.isNull()) {
            if (error) {
                *error = QStringLiteral(
                    "The exact command owner was destroyed during validation.");
            }
            return ExactReservationResult::InvalidOwner;
        }
    }
    for (const SwarmVehicleInstanceLease &lease : leases) {
        if (!leaseIsCurrent(lease)) {
            if (error) {
                *error = QStringLiteral(
                    "The exact command group changed during route validation.");
            }
            return ExactReservationResult::StaleLease;
        }
        if (ownerGuard.isNull()) {
            if (error) {
                *error = QStringLiteral(
                    "The exact command owner was destroyed during validation.");
            }
            return ExactReservationResult::InvalidOwner;
        }
        if (m_exactEndpointReservations.contains(lease.endpoint)
            || m_pendingExactByEndpoint.contains(lease.endpoint)
            || legacyCommandPendingFor(lease.endpoint)) {
            if (error) {
                *error = QStringLiteral(
                    "The command channel became busy during validation.");
            }
            return ExactReservationResult::Busy;
        }
    }

    const quint64 reservationId = nextExactReservationId();
    ExactReservationRecord record;
    record.owner = ownerGuard;
    record.leases = leases;
    record.ownerDestroyedConnection = connect(
        owner, &QObject::destroyed, this,
        [this, reservationId]() {
            handleExactOwnerDestroyed(reservationId);
        });
    m_exactReservations.insert(reservationId, record);
    for (const SwarmVehicleInstanceLease &lease : leases) {
        m_exactEndpointReservations.insert(lease.endpoint, reservationId);
    }
    if (reservationOut) {
        reservationOut->owner = ownerGuard;
        reservationOut->reservationId = reservationId;
        reservationOut->leases = leases;
    }
    return ExactReservationResult::Reserved;
}

bool VehicleCommandService::releaseExactReservation(
    const ExactReservationToken &reservation)
{
    auto record = m_exactReservations.find(reservation.reservationId);
    if (record == m_exactReservations.end()
        || reservation.reservationId == 0
        || reservation.owner.isNull()
        || record->owner != reservation.owner
        || record->leases != reservation.leases) {
        return false;
    }
    record->closing = true;
    disconnect(record->ownerDestroyedConnection);
    maybeReleaseClosingReservation(reservation.reservationId);
    return true;
}

VehicleCommandService::ExactSubmitResult
VehicleCommandService::submitExactCommandLong(
    const ExactReservationToken &reservation,
    const SwarmVehicleInstanceLease &lease,
    const ExactCommandRequest &request,
    ExactCommandToken *commandOut,
    QString *error)
{
    if (commandOut) {
        *commandOut = ExactCommandToken();
    }
    if (error) {
        error->clear();
    }
    if (!m_exactLeaseValidator || !m_exactRouteValidator) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle registry or route validator is unavailable.");
        }
        return ExactSubmitResult::ContextUnavailable;
    }
    if (reservation.owner.isNull()) {
        if (error) {
            *error = QStringLiteral("The exact command owner is detached.");
        }
        return ExactSubmitResult::InvalidOwner;
    }
    QPointer<QObject> ownerGuard(reservation.owner);
    if (!lease.isValid()) {
        if (error) {
            *error = QStringLiteral("The exact vehicle lease is invalid.");
        }
        return ExactSubmitResult::InvalidLease;
    }

    auto reserved = m_exactReservations.find(reservation.reservationId);
    if (reserved == m_exactReservations.end()
        || reservation.reservationId == 0
        || reserved->closing
        || reserved->owner != reservation.owner
        || reserved->leases != reservation.leases
        || !reservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId) {
        if (error) {
            *error = QStringLiteral("The exact command reservation is stale.");
        }
        return ExactSubmitResult::InvalidReservation;
    }
    if (!leaseIsCurrent(lease)) {
        if (error) {
            *error = QStringLiteral("The exact vehicle instance is stale.");
        }
        return ExactSubmitResult::StaleLease;
    }
    reserved = m_exactReservations.find(reservation.reservationId);
    if (ownerGuard.isNull()) {
        if (error) {
            *error = QStringLiteral(
                "The exact command owner was destroyed during lease validation.");
        }
        return ExactSubmitResult::InvalidOwner;
    }
    if (reserved == m_exactReservations.end() || reserved->closing
        || reserved->owner != ownerGuard
        || !reservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId) {
        if (error) {
            *error = QStringLiteral(
                "The exact command reservation changed during lease validation.");
        }
        return ExactSubmitResult::InvalidReservation;
    }
    if (m_pendingExactByEndpoint.contains(lease.endpoint)
        || legacyCommandPendingFor(lease.endpoint)) {
        if (error) {
            *error = QStringLiteral("The exact vehicle command channel is busy.");
        }
        return ExactSubmitResult::Busy;
    }
    const int commandValue = static_cast<int>(request.command);
    if (commandValue < 0
        || commandValue > std::numeric_limits<quint16>::max()) {
        if (error) {
            *error = QStringLiteral("The MAVLink command is outside the wire range.");
        }
        return ExactSubmitResult::InvalidCommand;
    }
    cleanupExpiredQuarantines();
    if (matchesQuarantine(
            lease.endpoint, static_cast<quint16>(commandValue),
            0, 0)) {
        if (error) {
            *error = QStringLiteral(
                "A late acknowledgement for this endpoint and command is still quarantined.");
        }
        return ExactSubmitResult::Quarantined;
    }

    QString routeError;
    if (!routeIsEligible(lease, &routeError)) {
        if (error) {
            *error = routeError.isEmpty()
                ? QStringLiteral("No safe exact route is available.")
                : routeError;
        }
        return ExactSubmitResult::RouteUnavailable;
    }

    // Revalidate after the route callback: it is allowed to emit signals and
    // retire a link or destroy the UI owner synchronously.
    reserved = m_exactReservations.find(reservation.reservationId);
    if (ownerGuard.isNull()) {
        if (error) {
            *error = QStringLiteral(
                "The exact command owner was destroyed during route validation.");
        }
        return ExactSubmitResult::InvalidOwner;
    }
    if (reserved == m_exactReservations.end() || reserved->closing
        || reserved->owner != ownerGuard
        || !reservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId) {
        if (error) {
            *error = QStringLiteral(
                "The exact command reservation changed during validation.");
        }
        return ExactSubmitResult::InvalidReservation;
    }
    if (!leaseIsCurrent(lease)) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle instance changed during validation.");
        }
        return ExactSubmitResult::StaleLease;
    }
    reserved = m_exactReservations.find(reservation.reservationId);
    if (ownerGuard.isNull()) {
        if (error) {
            *error = QStringLiteral(
                "The exact command owner was destroyed during final validation.");
        }
        return ExactSubmitResult::InvalidOwner;
    }
    if (reserved == m_exactReservations.end() || reserved->closing
        || reserved->owner != ownerGuard
        || !reservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId) {
        if (error) {
            *error = QStringLiteral(
                "The exact command reservation changed during final validation.");
        }
        return ExactSubmitResult::InvalidReservation;
    }
    if (m_pendingExactByEndpoint.contains(lease.endpoint)
        || legacyCommandPendingFor(lease.endpoint)) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle command channel became busy during validation.");
        }
        return ExactSubmitResult::Busy;
    }

    mavlink_command_long_t payload{};
    payload.target_system = static_cast<quint8>(lease.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(lease.endpoint.componentId);
    payload.command = static_cast<quint16>(commandValue);
    payload.confirmation = request.confirmation;
    payload.param1 = request.params[0];
    payload.param2 = request.params[1];
    payload.param3 = request.params[2];
    payload.param4 = request.params[3];
    payload.param5 = request.params[4];
    payload.param6 = request.params[5];
    payload.param7 = request.params[6];

    mavlink_message_t message{};
    mavlink_msg_command_long_encode(
        m_localSystemId, m_localComponentId, &message, &payload);

    ExactCommandToken token;
    token.transactionId = nextExactTransactionId();
    token.reservationId = reservation.reservationId;
    token.lease = lease;
    token.command = request.command;
    const int requestedTimeout = request.acknowledgementTimeoutMs > 0
        ? request.acknowledgementTimeoutMs : m_exactCommandTimeoutMs;
    const int requestedMaximumLifetime = request.maximumLifetimeMs > 0
        ? request.maximumLifetimeMs
        : DefaultExactCommandMaximumLifetimeMs;
    const qint64 submittedAtMs = m_exactClock.elapsed();
    PendingExactCommand pending;
    pending.token = token;
    pending.localSystemId = m_localSystemId;
    pending.localComponentId = m_localComponentId;
    pending.timeoutMs = qBound(1, requestedTimeout, MaximumExactTimeoutMs);
    const int maximumLifetimeMs = qBound(
        1, requestedMaximumLifetime, MaximumExactTimeoutMs);
    pending.absoluteDeadlineMs = submittedAtMs + maximumLifetimeMs;
    pending.deadlineMs = qMin(
        submittedAtMs + pending.timeoutMs, pending.absoluteDeadlineMs);
    // Install the waiter before the writer.  Test transports and in-process
    // simulations may deliver COMMAND_ACK reentrantly from the writer call.
    m_pendingExactCommands.insert(token.transactionId, pending);
    m_pendingExactByEndpoint.insert(lease.endpoint, token.transactionId);
    if (commandOut) {
        *commandOut = token;
    }
    scheduleExactDeadline();

    QPointer<VehicleCommandService> serviceGuard(this);
    bool frameWriterInvoked = false;
    const ExactLinkTransmitter::SendResult transmitted =
        m_transmitter->sendMessage(
            lease.endpoint.linkId, m_localSystemId, m_localComponentId,
            message, &frameWriterInvoked);
    if (serviceGuard.isNull()) {
        return transmitted == ExactLinkTransmitter::SendResult::SigningUnavailable
                && !frameWriterInvoked
            ? ExactSubmitResult::ContextUnavailable
            : ExactSubmitResult::TransportOutcomeUncertain;
    }

    // A synchronous ACK may already have completed and removed the waiter.
    auto submitted = m_pendingExactCommands.find(token.transactionId);
    if (submitted == m_pendingExactCommands.end()) {
        return ExactSubmitResult::Started;
    }
    submitted->frameAttempted =
        submitted->frameAttempted || frameWriterInvoked;
    if (transmitted != ExactLinkTransmitter::SendResult::Sent) {
        if (transmitted
                == ExactLinkTransmitter::SendResult::SigningUnavailable
            && !submitted->frameAttempted) {
            finishExactCommand(
                token.transactionId,
                ExactTerminalResult::RejectedBeforeTransmission,
                -1, 255, 0, 0, 0,
                QStringLiteral(
                    "Signing was unavailable; the command was rejected before transmission."),
                false);
            return ExactSubmitResult::ContextUnavailable;
        }
        finishExactCommand(
            token.transactionId,
            ExactTerminalResult::TransportOutcomeUncertain,
            -1, 255, 0, 0, 0,
            QStringLiteral(
                "The frame writer did not confirm transport; command outcome is uncertain."),
            true);
        return ExactSubmitResult::TransportOutcomeUncertain;
    }
    return ExactSubmitResult::Started;
}

bool VehicleCommandService::isExactCommandQuarantined(
    const SwarmVehicleInstanceLease &lease, MAV_CMD command)
{
    cleanupExpiredQuarantines();
    return lease.isValid()
        && matchesQuarantine(
            lease.endpoint, static_cast<quint16>(command),
            0, 0);
}

void VehicleCommandService::retireExactVehicle(
    const SwarmVehicleInstanceLease &lease)
{
    if (!lease.isValid()) {
        return;
    }
    QList<quint64> affectedReservations;
    for (auto reservation = m_exactReservations.begin();
         reservation != m_exactReservations.end(); ++reservation) {
        if (reservationContains(reservation.value(), lease)) {
            reservation->closing = true;
            affectedReservations.append(reservation.key());
        }
    }

    QList<quint64> affectedTransactions;
    for (auto pending = m_pendingExactCommands.constBegin();
         pending != m_pendingExactCommands.constEnd(); ++pending) {
        if (pending->token.lease.sameInstance(lease)) {
            affectedTransactions.append(pending.key());
        }
    }
    std::sort(affectedTransactions.begin(), affectedTransactions.end());
    for (quint64 transactionId : affectedTransactions) {
        if (m_pendingExactCommands.contains(transactionId)) {
            finishExactCommand(
                transactionId,
                ExactTerminalResult::LeaseRetiredOutcomeUncertain,
                -1, 255, 0, 0, 0,
                QStringLiteral(
                    "The exact vehicle instance retired while awaiting acknowledgement."),
                true);
        }
    }
    for (quint64 reservationId : affectedReservations) {
        maybeReleaseClosingReservation(reservationId);
    }
}

int VehicleCommandService::sendCurrentCommandLong(
    int command, int confirmation,
    float param1, float param2, float param3, float param4,
    float param5, float param6, float param7)
{
    if (command < 0 || command > std::numeric_limits<quint16>::max()
        || confirmation < 0
        || confirmation > std::numeric_limits<quint8>::max()) {
        return static_cast<int>(SendResult::InvalidTarget);
    }
    return static_cast<int>(sendCommandLong(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId,
        static_cast<MAV_CMD>(command), static_cast<quint8>(confirmation),
        param1, param2, param3, param4, param5, param6, param7));
}

VehicleCommandService::SendResult VehicleCommandService::sendCommandLong(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    MAV_CMD command, quint8 confirmation,
    float param1, float param2, float param3, float param4,
    float param5, float param6, float param7)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }

    mavlink_command_long_t payload{};
    payload.target_system = static_cast<quint8>(target.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(target.endpoint.componentId);
    payload.command = static_cast<quint16>(command);
    payload.confirmation = confirmation;
    payload.param1 = param1;
    payload.param2 = param2;
    payload.param3 = param3;
    payload.param4 = param4;
    payload.param5 = param5;
    payload.param6 = param6;
    payload.param7 = param7;

    mavlink_message_t message{};
    // Generated encoders populate the dialect-correct payload.  Re-finalizing
    // below supplies the independent sequence/version state of this link.
    mavlink_msg_command_long_encode(localSystemId, localComponentId,
                                    &message, &payload);
    return finalizeAndWrite(
        target, localSystemId, localComponentId, payload.command, message);
}

VehicleCommandService::SendResult VehicleCommandService::sendCommandInt(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    MAV_CMD command, MAV_FRAME frame,
    float param1, float param2, float param3, float param4,
    qint32 x, qint32 y, float z, quint8 current, quint8 autocontinue)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }

    mavlink_command_int_t payload{};
    payload.target_system = static_cast<quint8>(target.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(target.endpoint.componentId);
    payload.command = static_cast<quint16>(command);
    payload.frame = static_cast<quint8>(frame);
    payload.current = current;
    payload.autocontinue = autocontinue;
    payload.param1 = param1;
    payload.param2 = param2;
    payload.param3 = param3;
    payload.param4 = param4;
    payload.x = x;
    payload.y = y;
    payload.z = z;

    mavlink_message_t message{};
    mavlink_msg_command_int_encode(localSystemId, localComponentId,
                                   &message, &payload);
    return finalizeAndWrite(
        target, localSystemId, localComponentId, payload.command, message);
}

void VehicleCommandService::forgetLink(int linkId)
{
    for (auto generation = m_pendingCommands.begin();
         generation != m_pendingCommands.end();) {
        QHash<quint16, SenderIdentity> &commands = generation.value();
        for (auto command = commands.begin(); command != commands.end();) {
            if (command.value().endpoint.linkId == linkId) {
                command = commands.erase(command);
            } else {
                ++command;
            }
        }
        if (commands.isEmpty()) {
            generation = m_pendingCommands.erase(generation);
        } else {
            ++generation;
        }
    }

    QList<quint64> affectedReservations;
    for (auto reservation = m_exactReservations.begin();
         reservation != m_exactReservations.end(); ++reservation) {
        const bool usesLink = std::any_of(
            reservation->leases.cbegin(), reservation->leases.cend(),
            [linkId](const SwarmVehicleInstanceLease &lease) {
                return lease.endpoint.linkId == linkId;
            });
        if (usesLink) {
            reservation->closing = true;
            affectedReservations.append(reservation.key());
        }
    }
    QList<quint64> affectedTransactions;
    for (auto pending = m_pendingExactCommands.constBegin();
         pending != m_pendingExactCommands.constEnd(); ++pending) {
        if (pending->token.lease.endpoint.linkId == linkId) {
            affectedTransactions.append(pending.key());
        }
    }
    std::sort(affectedTransactions.begin(), affectedTransactions.end());
    for (quint64 transactionId : affectedTransactions) {
        if (m_pendingExactCommands.contains(transactionId)) {
            finishExactCommand(
                transactionId,
                ExactTerminalResult::LinkForgottenOutcomeUncertain,
                -1, 255, 0, 0, 0,
                QStringLiteral(
                    "The physical link ended while awaiting acknowledgement."),
                true);
        }
    }
    for (quint64 reservationId : affectedReservations) {
        maybeReleaseClosingReservation(reservationId);
    }
}

void VehicleCommandService::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (linkId < 0 || message.msgid != MAVLINK_MSG_ID_COMMAND_ACK) {
        return;
    }

    mavlink_command_ack_t acknowledgement{};
    mavlink_msg_command_ack_decode(&message, &acknowledgement);
    if (observeExactAcknowledgement(linkId, message, acknowledgement)) {
        return;
    }

    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid() || target.endpoint.linkId != linkId
        || target.endpoint.systemId != message.sysid
        || target.endpoint.componentId != message.compid
        || !targetIsCurrent(target)) {
        return;
    }

    auto generation = m_pendingCommands.find(target.generation);
    if (generation == m_pendingCommands.end()) {
        return;
    }
    auto pending = generation->find(acknowledgement.command);
    if (pending == generation->end()) {
        return;
    }

    const SenderIdentity sender = pending.value();
    if (!sender.endpoint.sameIdentity(target.endpoint)
        || (acknowledgement.target_system != 0
            && acknowledgement.target_system != sender.systemId)
        || (acknowledgement.target_component != 0
            && acknowledgement.target_component != sender.componentId)) {
        return;
    }

    if (acknowledgement.result != MAV_RESULT_IN_PROGRESS) {
        generation->erase(pending);
        if (generation->isEmpty()) {
            m_pendingCommands.erase(generation);
        }
    }

    emit commandAckReceived(
        target.generation,
        linkId, message.sysid, message.compid,
        acknowledgement.command, acknowledgement.result,
        acknowledgement.progress, acknowledgement.result_param2,
        acknowledgement.target_system, acknowledgement.target_component);
}

bool VehicleCommandService::targetIsCurrent(
    const VehicleTargetLease &target) const
{
    return m_targetManager && target.isValid()
        && m_targetManager->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation);
}

bool VehicleCommandService::leaseIsCurrent(
    const SwarmVehicleInstanceLease &lease) const
{
    return lease.isValid() && m_exactLeaseValidator
        && m_exactLeaseValidator(lease);
}

bool VehicleCommandService::routeIsEligible(
    const SwarmVehicleInstanceLease &lease, QString *error) const
{
    if (error) {
        error->clear();
    }
    return lease.isValid() && m_exactRouteValidator
        && m_exactRouteValidator(lease, error);
}

bool VehicleCommandService::reservationContains(
    const ExactReservationRecord &reservation,
    const SwarmVehicleInstanceLease &lease) const
{
    return std::any_of(
        reservation.leases.cbegin(), reservation.leases.cend(),
        [&lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        });
}

bool VehicleCommandService::legacyCommandPendingFor(
    const VehicleEndpoint &endpoint) const
{
    for (auto generation = m_pendingCommands.constBegin();
         generation != m_pendingCommands.constEnd(); ++generation) {
        for (auto command = generation->constBegin();
             command != generation->constEnd(); ++command) {
            if (command->endpoint.sameIdentity(endpoint)) {
                return true;
            }
        }
    }
    return false;
}

bool VehicleCommandService::exactEndpointBlocksLegacy(
    const VehicleEndpoint &endpoint, quint16 command)
{
    cleanupExpiredQuarantines();
    return m_exactEndpointReservations.contains(endpoint)
        || m_pendingExactByEndpoint.contains(endpoint)
        || matchesQuarantine(
            endpoint, command, 0, 0);
}

bool VehicleCommandService::acknowledgementTargets(
    quint8 targetSystem, quint8 targetComponent,
    quint8 localSystemId, quint8 localComponentId) const noexcept
{
    return (targetSystem == 0 || targetSystem == localSystemId)
        && (targetComponent == 0 || targetComponent == localComponentId);
}

bool VehicleCommandService::observeExactAcknowledgement(
    int linkId, const mavlink_message_t &message,
    const mavlink_command_ack_t &acknowledgement)
{
    VehicleEndpoint source;
    source.linkId = linkId;
    source.systemId = message.sysid;
    source.componentId = message.compid;

    const quint64 transactionId =
        m_pendingExactByEndpoint.value(source, 0);
    auto pending = m_pendingExactCommands.find(transactionId);
    if (transactionId != 0 && pending != m_pendingExactCommands.end()
        && pending->token.lease.endpoint.sameIdentity(source)
        && static_cast<quint16>(pending->token.command)
            == acknowledgement.command
        && acknowledgementTargets(
            acknowledgement.target_system,
            acknowledgement.target_component,
            pending->localSystemId, pending->localComponentId)) {
        const qint64 acknowledgedAtMs = m_exactClock.elapsed();
        if (acknowledgedAtMs >= pending->absoluteDeadlineMs) {
            finishExactCommand(
                transactionId,
                ExactTerminalResult::TimedOutOutcomeUncertain,
                -1, 255, 0, 0, 0,
                QStringLiteral(
                    "The exact command maximum lifetime expired; command outcome is uncertain."),
                true);
            return true;
        }
        const SwarmVehicleInstanceLease acknowledgedLease =
            pending->token.lease;
        if (!leaseIsCurrent(acknowledgedLease)) {
            if (m_pendingExactCommands.contains(transactionId)) {
                finishExactCommand(
                    transactionId,
                    ExactTerminalResult::LeaseRetiredOutcomeUncertain,
                    -1, 255, 0, 0, 0,
                    QStringLiteral(
                        "An acknowledgement arrived after the exact vehicle lease became stale."),
                    true);
            }
            return true;
        }
        // The injected registry validator is a callback boundary. It may have
        // retired the endpoint and completed this transaction reentrantly.
        pending = m_pendingExactCommands.find(transactionId);
        if (pending == m_pendingExactCommands.end()) {
            return true;
        }
        if (acknowledgement.result == MAV_RESULT_IN_PROGRESS) {
            pending->deadlineMs = qMin(
                acknowledgedAtMs + pending->timeoutMs,
                pending->absoluteDeadlineMs);
            const ExactCommandToken token = pending->token;
            scheduleExactDeadline();
            emit exactCommandProgress(
                token, acknowledgement.result,
                acknowledgement.progress,
                acknowledgement.result_param2);
        } else {
            ExactTerminalResult terminal =
                ExactTerminalResult::AcknowledgedRejected;
            QString description = QStringLiteral(
                "The vehicle rejected the exact command with MAV_RESULT %1.")
                    .arg(acknowledgement.result);
            if (acknowledgement.result == MAV_RESULT_ACCEPTED) {
                terminal = ExactTerminalResult::AcknowledgedAccepted;
                description = QStringLiteral(
                    "The vehicle acknowledged the exact command.");
            }
            finishExactCommand(
                transactionId, terminal,
                acknowledgement.result, acknowledgement.progress,
                acknowledgement.result_param2,
                acknowledgement.target_system,
                acknowledgement.target_component,
                description, false);
        }
        return true;
    }

    int quarantineIndex = -1;
    if (!matchesQuarantine(
            source, acknowledgement.command,
            acknowledgement.target_system,
            acknowledgement.target_component,
            &quarantineIndex)) {
        return false;
    }

    // A quarantined ACK belongs to an earlier transaction.  Consume it here
    // so it cannot impersonate either a new exact transaction or the legacy
    // selected-target signal.  A terminal ACK drains the ambiguity early;
    // IN_PROGRESS extends the bounded quarantine.
    if (acknowledgement.result == MAV_RESULT_IN_PROGRESS) {
        m_exactQuarantines[quarantineIndex].expiresAtMs =
            m_exactClock.elapsed() + m_exactQuarantineMs;
    } else {
        m_exactQuarantines.removeAt(quarantineIndex);
    }
    scheduleQuarantineExpiry();
    return true;
}

bool VehicleCommandService::matchesQuarantine(
    const VehicleEndpoint &endpoint, quint16 command,
    quint8 targetSystem, quint8 targetComponent, int *index) const
{
    for (int candidate = 0; candidate < m_exactQuarantines.size();
         ++candidate) {
        const QuarantinedExactCommand &quarantine =
            m_exactQuarantines.at(candidate);
        if (quarantine.endpoint.sameIdentity(endpoint)
            && quarantine.command == command
            && acknowledgementTargets(
                targetSystem, targetComponent,
                quarantine.localSystemId,
                quarantine.localComponentId)) {
            if (index) {
                *index = candidate;
            }
            return true;
        }
    }
    return false;
}

void VehicleCommandService::addQuarantine(
    const PendingExactCommand &pending)
{
    const VehicleEndpoint endpoint = pending.token.lease.endpoint;
    const quint16 command = static_cast<quint16>(pending.token.command);
    for (QuarantinedExactCommand &quarantine : m_exactQuarantines) {
        if (quarantine.endpoint.sameIdentity(endpoint)
            && quarantine.command == command
            && quarantine.localSystemId == pending.localSystemId
            && quarantine.localComponentId == pending.localComponentId) {
            quarantine.expiresAtMs =
                m_exactClock.elapsed() + m_exactQuarantineMs;
            scheduleQuarantineExpiry();
            return;
        }
    }
    QuarantinedExactCommand quarantine;
    quarantine.endpoint = endpoint;
    quarantine.command = command;
    quarantine.localSystemId = pending.localSystemId;
    quarantine.localComponentId = pending.localComponentId;
    quarantine.expiresAtMs =
        m_exactClock.elapsed() + m_exactQuarantineMs;
    m_exactQuarantines.append(quarantine);
    scheduleQuarantineExpiry();
}

void VehicleCommandService::cleanupExpiredQuarantines()
{
    const qint64 now = m_exactClock.elapsed();
    for (int index = m_exactQuarantines.size() - 1; index >= 0; --index) {
        if (m_exactQuarantines.at(index).expiresAtMs <= now) {
            m_exactQuarantines.removeAt(index);
        }
    }
    scheduleQuarantineExpiry();
}

void VehicleCommandService::scheduleExactDeadline()
{
    if (m_pendingExactCommands.isEmpty()) {
        m_exactDeadlineTimer.stop();
        return;
    }
    qint64 earliest = std::numeric_limits<qint64>::max();
    for (const PendingExactCommand &pending
         : std::as_const(m_pendingExactCommands)) {
        earliest = qMin(
            earliest, qMin(pending.deadlineMs, pending.absoluteDeadlineMs));
    }
    const qint64 remaining = qMax<qint64>(
        1, earliest - m_exactClock.elapsed());
    m_exactDeadlineTimer.start(static_cast<int>(qMin<qint64>(
        remaining, std::numeric_limits<int>::max())));
}

void VehicleCommandService::scheduleQuarantineExpiry()
{
    if (m_exactQuarantines.isEmpty()) {
        m_exactQuarantineTimer.stop();
        return;
    }
    qint64 earliest = std::numeric_limits<qint64>::max();
    for (const QuarantinedExactCommand &quarantine
         : std::as_const(m_exactQuarantines)) {
        earliest = qMin(earliest, quarantine.expiresAtMs);
    }
    const qint64 remaining = qMax<qint64>(
        1, earliest - m_exactClock.elapsed());
    m_exactQuarantineTimer.start(static_cast<int>(qMin<qint64>(
        remaining, std::numeric_limits<int>::max())));
}

void VehicleCommandService::handleExactDeadline()
{
    const qint64 now = m_exactClock.elapsed();
    QList<quint64> expired;
    for (auto pending = m_pendingExactCommands.constBegin();
         pending != m_pendingExactCommands.constEnd(); ++pending) {
        if (pending->deadlineMs <= now) {
            expired.append(pending.key());
        }
    }
    std::sort(expired.begin(), expired.end());
    for (quint64 transactionId : expired) {
        if (m_pendingExactCommands.contains(transactionId)) {
            const PendingExactCommand pending =
                m_pendingExactCommands.value(transactionId);
            const bool maximumLifetimeExpired =
                pending.absoluteDeadlineMs <= now;
            finishExactCommand(
                transactionId,
                ExactTerminalResult::TimedOutOutcomeUncertain,
                -1, 255, 0, 0, 0,
                maximumLifetimeExpired
                    ? QStringLiteral(
                        "The exact command maximum lifetime expired; command outcome is uncertain.")
                    : QStringLiteral(
                        "The acknowledgement deadline expired; command outcome is uncertain."),
                true);
        }
    }
    scheduleExactDeadline();
}

void VehicleCommandService::handleQuarantineExpiry()
{
    cleanupExpiredQuarantines();
}

void VehicleCommandService::handleExactOwnerDestroyed(
    quint64 reservationId)
{
    auto reservation = m_exactReservations.find(reservationId);
    if (reservation == m_exactReservations.end()) {
        return;
    }
    reservation->owner.clear();
    reservation->closing = true;
    maybeReleaseClosingReservation(reservationId);
}

void VehicleCommandService::finishExactCommand(
    quint64 transactionId, ExactTerminalResult result,
    int mavResult, int progress, int resultParam2,
    int acknowledgementTargetSystem,
    int acknowledgementTargetComponent,
    const QString &description, bool quarantine)
{
    auto pendingIterator = m_pendingExactCommands.find(transactionId);
    if (pendingIterator == m_pendingExactCommands.end()) {
        return;
    }
    const PendingExactCommand pending = pendingIterator.value();
    const quint64 reservationId = pending.token.reservationId;
    bool ownerDetached = true;
    const auto reservation = m_exactReservations.constFind(reservationId);
    if (reservation != m_exactReservations.constEnd()) {
        ownerDetached = reservation->owner.isNull();
    }
    if (quarantine) {
        addQuarantine(pending);
    }
    m_pendingExactCommands.erase(pendingIterator);
    if (m_pendingExactByEndpoint.value(pending.token.lease.endpoint, 0)
        == transactionId) {
        m_pendingExactByEndpoint.remove(pending.token.lease.endpoint);
    }
    scheduleExactDeadline();

    ExactCommandReport report;
    report.token = pending.token;
    report.terminalResult = result;
    report.mavResult = mavResult;
    report.progress = progress;
    report.resultParam2 = resultParam2;
    report.acknowledgementTargetSystem = acknowledgementTargetSystem;
    report.acknowledgementTargetComponent = acknowledgementTargetComponent;
    report.frameAttempted = pending.frameAttempted;
    report.ownerDetached = ownerDetached;
    report.description = description;

    QPointer<VehicleCommandService> guard(this);
    emit exactCommandFinished(report);
    if (!guard.isNull()) {
        maybeReleaseClosingReservation(reservationId);
    }
}

bool VehicleCommandService::reservationHasPending(
    quint64 reservationId) const
{
    for (const PendingExactCommand &pending
         : m_pendingExactCommands) {
        if (pending.token.reservationId == reservationId) {
            return true;
        }
    }
    return false;
}

void VehicleCommandService::maybeReleaseClosingReservation(
    quint64 reservationId)
{
    const auto reservation = m_exactReservations.constFind(reservationId);
    if (reservation == m_exactReservations.constEnd()
        || !reservation->closing
        || reservationHasPending(reservationId)) {
        return;
    }
    removeReservation(reservationId);
}

void VehicleCommandService::removeReservation(quint64 reservationId)
{
    auto reservation = m_exactReservations.find(reservationId);
    if (reservation == m_exactReservations.end()) {
        return;
    }
    const QList<SwarmVehicleInstanceLease> leases = reservation->leases;
    disconnect(reservation->ownerDestroyedConnection);
    m_exactReservations.erase(reservation);
    for (const SwarmVehicleInstanceLease &lease : leases) {
        if (m_exactEndpointReservations.value(lease.endpoint, 0)
            == reservationId) {
            m_exactEndpointReservations.remove(lease.endpoint);
        }
    }
    emit exactReservationReleased(reservationId);
}

quint64 VehicleCommandService::nextExactReservationId()
{
    do {
        ++m_nextExactReservationId;
    } while (m_nextExactReservationId == 0
             || m_exactReservations.contains(m_nextExactReservationId));
    return m_nextExactReservationId;
}

quint64 VehicleCommandService::nextExactTransactionId()
{
    do {
        ++m_nextExactTransactionId;
    } while (m_nextExactTransactionId == 0
             || m_pendingExactCommands.contains(m_nextExactTransactionId));
    return m_nextExactTransactionId;
}

VehicleCommandService::SendResult VehicleCommandService::finalizeAndWrite(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    quint16 command, mavlink_message_t message)
{
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (!m_transmitter) {
        return SendResult::TransportUnavailable;
    }
    if (exactEndpointBlocksLegacy(target.endpoint, command)) {
        // Keep the legacy enum stable while failing closed against an exact
        // reservation or a quarantined indistinguishable ACK.
        return SendResult::TransportUnavailable;
    }

    QHash<quint16, SenderIdentity> &generationCommands =
        m_pendingCommands[target.generation];
    const bool hadPrevious = generationCommands.contains(command);
    const SenderIdentity previous = generationCommands.value(command);
    generationCommands.insert(
        command,
        SenderIdentity{target.endpoint, localSystemId, localComponentId});

    const ExactLinkTransmitter::SendResult result =
        m_transmitter->sendMessage(
            target.endpoint.linkId, localSystemId, localComponentId,
            message);
    if (result != ExactLinkTransmitter::SendResult::Sent) {
        auto generation = m_pendingCommands.find(target.generation);
        if (generation != m_pendingCommands.end()) {
            if (hadPrevious) {
                generation->insert(command, previous);
            } else {
                generation->remove(command);
            }
            if (generation->isEmpty()) {
                m_pendingCommands.erase(generation);
            }
        }
        return SendResult::TransportUnavailable;
    }
    return SendResult::Sent;
}

void VehicleCommandService::clearPendingCommands()
{
    m_pendingCommands.clear();
}
