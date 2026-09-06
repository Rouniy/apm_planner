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
    qRegisterMetaType<ExactCommandIntRequest>();
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
            this, &VehicleCommandService::handleTargetGenerationChanged);
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
    if (m_exactApiInFlight || !m_exactReservations.isEmpty()
        || !m_pendingExactCommands.isEmpty()
        || !leaseValidator || !routeValidator) {
        return false;
    }
    m_exactLeaseValidator = std::move(leaseValidator);
    m_exactRouteValidator = std::move(routeValidator);
    return static_cast<bool>(m_exactLeaseValidator)
        && static_cast<bool>(m_exactRouteValidator);
}

bool VehicleCommandService::configureSingleVehicleExactRoute(
    ExactRouteValidator routeValidator)
{
    if (m_exactApiInFlight || !m_exactReservations.isEmpty()
        || !m_pendingExactCommands.isEmpty() || !routeValidator) {
        return false;
    }
    m_singleVehicleExactRouteValidator = std::move(routeValidator);
    return static_cast<bool>(m_singleVehicleExactRouteValidator);
}

bool VehicleCommandService::configureComponentExactTransactions(
    ComponentLeaseValidator leaseValidator, ComponentRouteValidator routeValidator)
{
    if (m_exactApiInFlight || !m_exactReservations.isEmpty() || !m_pendingExactCommands.isEmpty()
        || !leaseValidator || !routeValidator) return false;
    m_componentLeaseValidator = std::move(leaseValidator);
    m_componentRouteValidator = std::move(routeValidator);
    return true;
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
    QList<ExactInstanceLease> instances;
    for (const auto &lease : leases) instances.append(exactInstance(lease));
    return reserveExactEndpointsWithPolicy(
        owner, instances, ExactReservationPolicy::Swarm,
        VehicleTargetLease(), reservationOut, error);
}

VehicleCommandService::ExactReservationResult
VehicleCommandService::reserveSingleVehicleEndpoint(
    QObject *owner,
    const VehicleTargetLease &target,
    const SwarmVehicleInstanceLease &lease,
    ExactReservationToken *reservationOut,
    QString *error)
{
    return reserveExactEndpointsWithPolicy(
        owner, QList<ExactInstanceLease>{exactInstance(lease)},
        ExactReservationPolicy::SingleVehicle,
        target, reservationOut, error);
}

VehicleCommandService::ExactReservationResult VehicleCommandService::reserveComponentEndpoint(
    QObject *owner, const MavlinkComponentInstanceLease &lease,
    ExactReservationToken *out, QString *error)
{
    return reserveExactEndpointsWithPolicy(owner, {exactInstance(lease)},
        ExactReservationPolicy::Component, {}, out, error);
}

VehicleCommandService::ExactReservationResult
VehicleCommandService::reserveExactEndpointsWithPolicy(
    QObject *owner,
    const QList<ExactInstanceLease> &requestedLeases,
    ExactReservationPolicy policy,
    const VehicleTargetLease &requestedTarget,
    ExactReservationToken *reservationOut,
    QString *error)
{
    const auto leases = requestedLeases;
    const auto target = requestedTarget;
    if (reservationOut) {
        *reservationOut = ExactReservationToken();
    }
    if (error) {
        error->clear();
    }
    QPointer<VehicleCommandService> serviceGuard(this);
    if (m_exactApiInFlight) {
        if (error) {
            *error = QStringLiteral(
                "Another exact command API call is validating application policy.");
        }
        return ExactReservationResult::Busy;
    }
    m_exactApiInFlight = true;
    ScopeExit apiCallGuard([serviceGuard]() {
        if (!serviceGuard.isNull()) {
            serviceGuard->m_exactApiInFlight = false;
        }
    });

    QPointer<QObject> ownerGuard(owner);
    if (!owner) {
        if (error) {
            *error = QStringLiteral("The exact command owner is missing.");
        }
        return ExactReservationResult::InvalidOwner;
    }
    const bool singleVehicle =
        policy == ExactReservationPolicy::SingleVehicle;
    const bool component = policy == ExactReservationPolicy::Component;
    if (!validatorsAvailable(policy)) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle registry or route validator is unavailable.");
        }
        return ExactReservationResult::ContextUnavailable;
    }
    if (((singleVehicle || component) && leases.size() != 1) || leases.isEmpty()
        || leases.size() > SwarmTelemetryRegistry::MaximumVehicleEndpoints) {
        if (error) {
            *error = QStringLiteral("The exact command group is empty or too large.");
        }
        return ExactReservationResult::InvalidLease;
    }
    if (singleVehicle
        && (!target.isValid()
            || !target.endpoint.sameIdentity(leases.first().endpoint))) {
        if (error) {
            *error = QStringLiteral(
                "The selected vehicle target does not match the exact instance.");
        }
        return ExactReservationResult::InvalidLease;
    }
    if (singleVehicle
        && (!targetIsCurrent(target)
            || !m_targetManager->isTargetGenerationSettled())) {
        if (error) {
            *error = QStringLiteral(
                "The selected vehicle target is stale or still changing.");
        }
        return ExactReservationResult::StaleLease;
    }

    QSet<VehicleEndpoint> uniqueEndpoints;
    for (const ExactInstanceLease &lease : leases) {
        if (!lease.isValid() || uniqueEndpoints.contains(lease.endpoint)
            || component != (lease.domain == ExactLeaseDomain::Component)) {
            if (error) {
                *error = QStringLiteral(
                    "The exact command group contains an invalid or duplicate endpoint.");
            }
            return ExactReservationResult::InvalidLease;
        }
        uniqueEndpoints.insert(lease.endpoint);
        const bool leaseCurrent = leaseIsCurrent(lease);
        if (serviceGuard.isNull()) {
            return ExactReservationResult::ContextUnavailable;
        }
        if (!leaseCurrent) {
            if (error) {
                *error = QStringLiteral("The vehicle instance on %1 is stale.")
                    .arg(endpointLabel(lease.endpoint));
            }
            return ExactReservationResult::StaleLease;
        }
        if (singleVehicle
            && (!targetIsCurrent(target)
                || !m_targetManager->isTargetGenerationSettled())) {
            if (error) {
                *error = QStringLiteral(
                    "The selected vehicle target changed during instance validation.");
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
    for (const ExactInstanceLease &lease : leases) {
        QString routeError;
        const bool routeEligible = routeIsEligible(lease, policy, &routeError);
        if (serviceGuard.isNull()) {
            return ExactReservationResult::ContextUnavailable;
        }
        if (!routeEligible) {
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
        if (singleVehicle
            && (!targetIsCurrent(target)
                || !m_targetManager->isTargetGenerationSettled())) {
            if (error) {
                *error = QStringLiteral(
                    "The selected vehicle target changed during route validation.");
            }
            return ExactReservationResult::StaleLease;
        }
    }
    for (const ExactInstanceLease &lease : leases) {
        const bool leaseCurrent = leaseIsCurrent(lease);
        if (serviceGuard.isNull()) {
            return ExactReservationResult::ContextUnavailable;
        }
        if (!leaseCurrent) {
            if (error) {
                *error = QStringLiteral(
                    "The exact command group changed during route validation.");
            }
            return ExactReservationResult::StaleLease;
        }
        if (singleVehicle
            && (!targetIsCurrent(target)
                || !m_targetManager->isTargetGenerationSettled())) {
            if (error) {
                *error = QStringLiteral(
                    "The selected vehicle target changed during final validation.");
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
    record.target = target;
    record.policy = policy;
    record.ownerDestroyedConnection = connect(
        owner, &QObject::destroyed, this,
        [this, reservationId]() {
            handleExactOwnerDestroyed(reservationId);
        });
    m_exactReservations.insert(reservationId, record);
    for (const ExactInstanceLease &lease : leases) {
        m_exactEndpointReservations.insert(lease.endpoint, reservationId);
    }
    if (reservationOut) {
        reservationOut->owner = ownerGuard;
        reservationOut->reservationId = reservationId;
        for (const auto &lease : leases) {
            if (component) reservationOut->componentLeases.append(
                {lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch});
            else reservationOut->leases.append({lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch});
        }
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
        || !reservationTokenMatches(reservation, *record)) {
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
    return submitCommand(reservation, exactInstance(lease), request, nullptr,
                         commandOut, error);
}

VehicleCommandService::ExactSubmitResult
VehicleCommandService::submitExactCommandInt(
    const ExactReservationToken &reservation,
    const SwarmVehicleInstanceLease &lease,
    const ExactCommandIntRequest &request,
    ExactCommandToken *commandOut,
    QString *error)
{
    const ExactCommandIntRequest pinnedRequest = request;
    ExactCommandRequest policy;
    policy.command = pinnedRequest.command;
    policy.acknowledgementTimeoutMs = pinnedRequest.acknowledgementTimeoutMs;
    policy.maximumLifetimeMs = pinnedRequest.maximumLifetimeMs;
    policy.validateBeforeWrite = pinnedRequest.validateBeforeWrite;
    policy.maximumRetries = pinnedRequest.maximumRetries;
    return submitCommand(reservation, exactInstance(lease), policy,
                         &pinnedRequest, commandOut, error);
}

VehicleCommandService::ExactSubmitResult VehicleCommandService::submitComponentCommandLong(
    const ExactReservationToken &reservation, const MavlinkComponentInstanceLease &lease,
    const ExactCommandRequest &request, ExactCommandToken *out, QString *error)
{
    return submitCommand(reservation, exactInstance(lease), request, nullptr,
                         out, error);
}

VehicleCommandService::ExactSubmitResult VehicleCommandService::submitCommand(
    const ExactReservationToken &requestedReservation, const ExactInstanceLease &requestedLease,
    const ExactCommandRequest &request, const ExactCommandIntRequest *intRequest,
    ExactCommandToken *commandOut, QString *error)
{
    const auto reservation = requestedReservation;
    const auto lease = requestedLease;
    const ExactCommandRequest commandRequest = request;
    const bool commandInt = intRequest != nullptr;
    const ExactCommandIntRequest commandIntRequest = intRequest
        ? *intRequest : ExactCommandIntRequest{};
    if (commandOut) {
        *commandOut = ExactCommandToken();
    }
    if (error) {
        error->clear();
    }
    QPointer<VehicleCommandService> serviceGuard(this);
    if (m_exactApiInFlight) {
        if (error) {
            *error = QStringLiteral(
                "Another exact command API call is validating application policy.");
        }
        return ExactSubmitResult::Busy;
    }
    m_exactApiInFlight = true;
    ScopeExit apiCallGuard([serviceGuard]() {
        if (!serviceGuard.isNull()) {
            serviceGuard->m_exactApiInFlight = false;
        }
    });
    if (lease.domain == ExactLeaseDomain::Component ? !m_componentLeaseValidator : !m_exactLeaseValidator) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle registry is unavailable.");
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
        || !reservationTokenMatches(reservation, *reserved)
        || !reservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId) {
        if (error) {
            *error = QStringLiteral("The exact command reservation is stale.");
        }
        return ExactSubmitResult::InvalidReservation;
    }
    const ExactReservationPolicy reservationPolicy = reserved->policy;
    const VehicleTargetLease reservationTarget = reserved->target;
    if (!validatorsAvailable(reservationPolicy)) {
        if (error) {
            *error = QStringLiteral(
                "The exact command route validator is unavailable.");
        }
        return ExactSubmitResult::ContextUnavailable;
    }
    if (!reservationTargetIsCurrent(*reserved)) {
        if (error) {
            *error = QStringLiteral(
                "The selected vehicle target is stale or still changing.");
        }
        return ExactSubmitResult::StaleLease;
    }

    const bool initialLeaseCurrent = leaseIsCurrent(lease);
    if (serviceGuard.isNull()) {
        return ExactSubmitResult::ContextUnavailable;
    }
    if (!initialLeaseCurrent) {
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
        || !reservationTokenMatches(reservation, *reserved)
        || !reservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId
        || reserved->policy != reservationPolicy
        || reserved->target.generation != reservationTarget.generation
        || !reserved->target.endpoint.sameIdentity(
            reservationTarget.endpoint)
        || !reservationTargetIsCurrent(*reserved)) {
        if (error) {
            *error = QStringLiteral(
                "The selected vehicle target changed during lease validation.");
        }
        return ExactSubmitResult::StaleLease;
    }
    if (m_pendingExactByEndpoint.contains(lease.endpoint)
        || legacyCommandPendingFor(lease.endpoint)) {
        if (error) {
            *error = QStringLiteral("The exact vehicle command channel is busy.");
        }
        return ExactSubmitResult::Busy;
    }
    const int commandValue = static_cast<int>(commandRequest.command);
    if (commandRequest.maximumRetries < 0 || commandRequest.maximumRetries > 3 || commandValue < 0
        || commandValue > std::numeric_limits<quint16>::max()) {
        if (error) {
            *error = QStringLiteral("The command must fit the MAVLink wire range and retries must be between 0 and 3.");
        }
        return ExactSubmitResult::InvalidCommand;
    }
    const int frameValue = static_cast<int>(commandIntRequest.frame);
    if (commandInt && (frameValue < 0
                       || frameValue > std::numeric_limits<quint8>::max())) {
        if (error) {
            *error = QStringLiteral(
                "The COMMAND_INT coordinate frame must fit the MAVLink wire range.");
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
    const bool routeEligible = routeIsEligible(lease, reservationPolicy, &routeError);
    if (serviceGuard.isNull()) {
        return ExactSubmitResult::ContextUnavailable;
    }
    if (!routeEligible) {
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
        || !reservationTokenMatches(reservation, *reserved)
        || !reservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId
        || reserved->policy != reservationPolicy
        || reserved->target.generation != reservationTarget.generation
        || !reserved->target.endpoint.sameIdentity(
            reservationTarget.endpoint)) {
        if (error) {
            *error = QStringLiteral(
                "The exact command reservation changed during validation.");
        }
        return ExactSubmitResult::InvalidReservation;
    }
    if (!reservationTargetIsCurrent(*reserved)) {
        if (error) {
            *error = QStringLiteral(
                "The selected vehicle target changed during route validation.");
        }
        return ExactSubmitResult::StaleLease;
    }
    const bool finalLeaseCurrent = leaseIsCurrent(lease);
    if (serviceGuard.isNull()) {
        return ExactSubmitResult::ContextUnavailable;
    }
    if (!finalLeaseCurrent) {
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
        || !reservationTokenMatches(reservation, *reserved)
        || !reservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId
        || reserved->policy != reservationPolicy
        || reserved->target.generation != reservationTarget.generation
        || !reserved->target.endpoint.sameIdentity(
            reservationTarget.endpoint)
        || !reservationTargetIsCurrent(*reserved)) {
        if (error) {
            *error = QStringLiteral(
                "The exact command reservation or selected target changed during final validation.");
        }
        return ExactSubmitResult::StaleLease;
    }
    if (m_pendingExactByEndpoint.contains(lease.endpoint)
        || legacyCommandPendingFor(lease.endpoint)) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle command channel became busy during validation.");
        }
        return ExactSubmitResult::Busy;
    }

    if (commandRequest.validateBeforeWrite) {
        QString safetyError;
        const bool permitted =
            commandRequest.validateBeforeWrite(&safetyError);
        if (serviceGuard.isNull()) {
            return ExactSubmitResult::ContextUnavailable;
        }
        if (!permitted) {
            if (error) {
                *error = safetyError.isEmpty()
                    ? QStringLiteral(
                        "The command safety conditions changed before transmission.")
                    : safetyError;
            }
            return ExactSubmitResult::RouteUnavailable;
        }

        // The safety callback is deliberately the final application callback
        // before transmission. It may synchronously change selection or
        // retire the lease, so repeat only pure stored-state comparisons
        // before creating a waiter or frame. Calling route/lease policy again
        // here could itself invalidate the safety result.
        reserved = m_exactReservations.find(reservation.reservationId);
        if (ownerGuard.isNull()) {
            if (error) {
                *error = QStringLiteral(
                    "The exact command owner was destroyed during safety validation.");
            }
            return ExactSubmitResult::InvalidOwner;
        }
        if (reserved == m_exactReservations.end() || reserved->closing
            || reserved->owner != ownerGuard
            || !reservationTokenMatches(reservation, *reserved)
            || !reservationContains(*reserved, lease)
            || m_exactEndpointReservations.value(lease.endpoint, 0)
                != reservation.reservationId
            || reserved->policy != reservationPolicy
            || reserved->target.generation != reservationTarget.generation
            || !reserved->target.endpoint.sameIdentity(
                reservationTarget.endpoint)
            || !reservationTargetIsCurrent(*reserved)) {
            if (error) {
                *error = QStringLiteral(
                    "The selected vehicle target changed during safety validation.");
            }
            return ExactSubmitResult::StaleLease;
        }
    }

    // Policy callbacks can recurse into this API. Exclude them while
    // validating, then permit a synchronous ACK handler to submit the next
    // command after the waiter below has reached a terminal state.
    m_exactApiInFlight = false;

    ExactCommandToken token;
    token.transactionId = nextExactTransactionId();
    token.reservationId = reservation.reservationId;
    if (lease.domain == ExactLeaseDomain::Component)
        token.componentLease = {lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch};
    else token.lease = {lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch};
    token.command = commandRequest.command;
    const int requestedTimeout = commandRequest.acknowledgementTimeoutMs > 0
        ? commandRequest.acknowledgementTimeoutMs : m_exactCommandTimeoutMs;
    const int requestedMaximumLifetime = commandRequest.maximumLifetimeMs > 0
        ? commandRequest.maximumLifetimeMs
        : DefaultExactCommandMaximumLifetimeMs;
    const qint64 submittedAtMs = m_exactClock.elapsed();
    PendingExactCommand pending;
    pending.token = token;
    pending.request = commandRequest;
    pending.commandInt = commandInt;
    pending.commandIntRequest = commandIntRequest;
    pending.remainingRetries = commandRequest.maximumRetries;
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
    return transmitExactCommand(token.transactionId);
}

VehicleCommandService::ExactSubmitResult VehicleCommandService::transmitExactCommand(quint64 transactionId)
{
    auto active = m_pendingExactCommands.find(transactionId);
    if (active == m_pendingExactCommands.end()) return ExactSubmitResult::InvalidReservation;
    const auto attempted = std::make_shared<bool>(false);
    active->inFlightAttempt = attempted;
    const PendingExactCommand pending = *active;
    const auto token = pending.token;
    const auto lease = exactInstance(token);
    mavlink_message_t message{};
    if (pending.commandInt) {
        mavlink_command_int_t payload{};
        payload.target_system = quint8(lease.endpoint.systemId);
        payload.target_component = quint8(lease.endpoint.componentId);
        payload.command = quint16(token.command);
        payload.frame = quint8(pending.commandIntRequest.frame);
        payload.current = pending.commandIntRequest.current;
        payload.autocontinue = pending.commandIntRequest.autocontinue;
        payload.param1 = pending.commandIntRequest.params[0];
        payload.param2 = pending.commandIntRequest.params[1];
        payload.param3 = pending.commandIntRequest.params[2];
        payload.param4 = pending.commandIntRequest.params[3];
        payload.x = pending.commandIntRequest.x;
        payload.y = pending.commandIntRequest.y;
        payload.z = pending.commandIntRequest.z;
        mavlink_msg_command_int_encode(
            pending.localSystemId, pending.localComponentId, &message,
            &payload);
    } else {
        mavlink_command_long_t payload{};
        payload.target_system = quint8(lease.endpoint.systemId);
        payload.target_component = quint8(lease.endpoint.componentId);
        payload.command = quint16(token.command);
        payload.confirmation = quint8(qMin(
            255, int(pending.request.confirmation) + pending.attemptIndex));
        payload.param1 = pending.request.params[0];
        payload.param2 = pending.request.params[1];
        payload.param3 = pending.request.params[2];
        payload.param4 = pending.request.params[3];
        payload.param5 = pending.request.params[4];
        payload.param6 = pending.request.params[5];
        payload.param7 = pending.request.params[6];
        mavlink_msg_command_long_encode(
            pending.localSystemId, pending.localComponentId, &message,
            &payload);
    }
    QPointer<VehicleCommandService> serviceGuard(this);
    QString guardError;
    const ExactLinkTransmitter::SendResult transmitted =
        pending.commandInt
        ? m_transmitter->sendGuardedCommandInt(
              lease.endpoint.linkId, pending.localSystemId,
              pending.localComponentId, message,
              [serviceGuard, transactionId, attempted, &guardError] {
                  return serviceGuard
                      && serviceGuard->validateCommandBeforeWriter(
                          transactionId, attempted, &guardError);
              }, attempted.get())
        : m_transmitter->sendGuardedCommandLong(
              lease.endpoint.linkId, pending.localSystemId,
              pending.localComponentId, message,
              [serviceGuard, transactionId, attempted, &guardError] {
                  return serviceGuard
                      && serviceGuard->validateCommandBeforeWriter(
                          transactionId, attempted, &guardError);
              }, attempted.get());
    if (serviceGuard.isNull()) {
        return !pending.wasFrameAttempted()
            ? ExactSubmitResult::ContextUnavailable
            : ExactSubmitResult::TransportOutcomeUncertain;
    }

    // A synchronous ACK may already have completed and removed the waiter.
    auto submitted = m_pendingExactCommands.find(token.transactionId);
    if (submitted == m_pendingExactCommands.end()) {
        return ExactSubmitResult::Started;
    }
    submitted->frameAttempted =
        submitted->frameAttempted || *attempted;
    submitted->transmissionAttempts += *attempted ? 1 : 0;
    submitted->inFlightAttempt.reset();
    if (transmitted != ExactLinkTransmitter::SendResult::Sent) {
        if (!submitted->wasFrameAttempted()) {
            finishExactCommand(
                token.transactionId,
                ExactTerminalResult::RejectedBeforeTransmission,
                -1, 255, 0, 0, 0,
                !guardError.isEmpty() ? guardError
                    : transmitted == ExactLinkTransmitter::SendResult::SigningUnavailable
                        ? QStringLiteral("Signing was unavailable; the command was rejected before transmission.")
                        : QStringLiteral("The command was rejected before reaching the frame writer."),
                false);
            return ExactSubmitResult::ContextUnavailable;
        }
        finishExactCommand(
            token.transactionId,
            m_exactClock.elapsed() >= submitted->absoluteDeadlineMs
                ? ExactTerminalResult::TimedOutOutcomeUncertain
                : ExactTerminalResult::TransportOutcomeUncertain,
            -1, 255, 0, 0, 0,
            !guardError.isEmpty() ? guardError : QStringLiteral(
                "The frame writer did not confirm transport; command outcome is uncertain."),
            true);
        return ExactSubmitResult::TransportOutcomeUncertain;
    }
    return ExactSubmitResult::Started;
}

bool VehicleCommandService::validateCommandBeforeWriter(quint64 transactionId,
    const std::shared_ptr<bool> &attempt, QString *error)
{
    const QPointer<VehicleCommandService> guard(this);
    auto pending = m_pendingExactCommands.constFind(transactionId);
    if (pending == m_pendingExactCommands.cend() || pending->inFlightAttempt != attempt) return false;
    const PendingExactCommand snapshot = *pending;
    const auto lease = exactInstance(snapshot.token);
    const auto reservationId = snapshot.token.reservationId;
    const auto initial = m_exactReservations.constFind(reservationId);
    if (initial == m_exactReservations.cend()) return false;
    const ExactReservationRecord reservedSnapshot = *initial;
    const bool previousApiState = m_exactApiInFlight;
    m_exactApiInFlight = true;
    ScopeExit apiGuard([guard, previousApiState] {
        if (guard) guard->m_exactApiInFlight = previousApiState;
    });
    const auto refuse = [error](const QString &reason) {
        if (error) *error = reason;
        return false;
    };
    const auto current = [&]() {
        if (!guard) return false;
        const auto live = m_pendingExactCommands.constFind(transactionId);
        const auto reserved = m_exactReservations.constFind(reservationId);
        if (live == m_pendingExactCommands.cend() || live->inFlightAttempt != attempt
            || reserved == m_exactReservations.cend() || reserved->closing || reserved->owner.isNull()
            || reserved->owner != reservedSnapshot.owner || reserved->policy != reservedSnapshot.policy
            || reserved->leases != reservedSnapshot.leases || !reservationTargetIsCurrent(*reserved)
            || !reservationContains(*reserved, lease)
            || m_exactEndpointReservations.value(lease.endpoint, 0) != reservationId
            || m_pendingExactByEndpoint.value(lease.endpoint, 0) != transactionId)
            return refuse(QStringLiteral("The exact command operation or owner changed after signing; this frame was not transmitted."));
        if (m_exactClock.elapsed() >= snapshot.absoluteDeadlineMs)
            return refuse(QStringLiteral("The command maximum lifetime expired after signing; this frame was not transmitted."));
        return true;
    };
    if (!current()) return false;
    const bool firstLease = leaseIsCurrent(lease);
    if (!current()) return false;
    if (!firstLease) return refuse(QStringLiteral("The exact instance became stale after signing; this frame was not transmitted."));
    QString routeError;
    const bool route = routeIsEligible(lease, reservedSnapshot.policy, &routeError);
    if (!current()) return false;
    if (!route) return refuse(routeError.isEmpty()
        ? QStringLiteral("The exact route became unavailable after signing; this frame was not transmitted.") : routeError);
    const bool finalLease = leaseIsCurrent(lease);
    if (!current()) return false;
    if (!finalLease) return refuse(QStringLiteral("The exact instance changed during post-signing validation; this frame was not transmitted."));
    if (snapshot.request.validateBeforeWrite) {
        const bool allowed = snapshot.request.validateBeforeWrite(&routeError);
        if (!current()) return false;
        if (!allowed) return refuse(routeError.isEmpty()
            ? QStringLiteral("The command safety gate rejected the signed frame before transmission.") : routeError);
    }
    return true;
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

bool VehicleCommandService::isExactEndpointBusy(
    const VehicleEndpoint &endpoint, MAV_CMD command)
{
    return exactEndpointBlocksLegacy(endpoint, static_cast<quint16>(command));
}

void VehicleCommandService::retireExactVehicle(
    const SwarmVehicleInstanceLease &lease)
{
    retireInstance(exactInstance(lease));
}

void VehicleCommandService::retireComponent(const MavlinkComponentInstanceLease &lease)
{
    retireInstance(exactInstance(lease));
}

void VehicleCommandService::retireInstance(const ExactInstanceLease &lease)
{
    const QPointer<VehicleCommandService> serviceGuard(this);
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
        if (exactInstance(pending->token).sameInstance(lease)) {
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
            if (serviceGuard.isNull()) {
                return;
            }
        }
    }
    for (quint64 reservationId : affectedReservations) {
        maybeReleaseClosingReservation(reservationId);
        if (serviceGuard.isNull()) {
            return;
        }
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
    const QPointer<VehicleCommandService> serviceGuard(this);
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
            [linkId](const ExactInstanceLease &lease) {
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
        if (exactInstance(pending->token).endpoint.linkId == linkId) {
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
            if (serviceGuard.isNull()) {
                return;
            }
        }
    }
    for (quint64 reservationId : affectedReservations) {
        maybeReleaseClosingReservation(reservationId);
        if (serviceGuard.isNull()) {
            return;
        }
    }
}

void VehicleCommandService::observeComponentMessage(int linkId, quint64 physicalEpoch,
    const mavlink_message_t &message)
{
    if (linkId < 0 || physicalEpoch == 0 || message.msgid != MAVLINK_MSG_ID_COMMAND_ACK) return;
    mavlink_command_ack_t acknowledgement{};
    mavlink_msg_command_ack_decode(&message, &acknowledgement);
    observeExactAcknowledgement(linkId, message, acknowledgement, ExactLeaseDomain::Component, physicalEpoch);
}

void VehicleCommandService::observePhysicalMessage(int linkId, quint64 physicalEpoch,
    const mavlink_message_t &message)
{
    if (linkId < 0 || physicalEpoch == 0 || message.msgid != MAVLINK_MSG_ID_COMMAND_ACK) return;
    mavlink_command_ack_t acknowledgement{};
    mavlink_msg_command_ack_decode(&message, &acknowledgement);
    const QPointer<VehicleCommandService> guard(this);
    if (observeExactAcknowledgement(linkId, message, acknowledgement,
                                   ExactLeaseDomain::Component, physicalEpoch) || !guard) return;
    observeMessageImpl(linkId, physicalEpoch, message);
}

void VehicleCommandService::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    observeMessageImpl(linkId, 0, message);
}

void VehicleCommandService::observeMessageImpl(
    int linkId, quint64 physicalEpoch, const mavlink_message_t &message)
{
    if (linkId < 0 || message.msgid != MAVLINK_MSG_ID_COMMAND_ACK) {
        return;
    }

    mavlink_command_ack_t acknowledgement{};
    mavlink_msg_command_ack_decode(&message, &acknowledgement);
    if (observeExactAcknowledgement(linkId, message, acknowledgement, ExactLeaseDomain::Swarm, physicalEpoch)) {
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

VehicleCommandService::ExactInstanceLease VehicleCommandService::exactInstance(const SwarmVehicleInstanceLease &lease)
{ return {lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch, ExactLeaseDomain::Swarm}; }
VehicleCommandService::ExactInstanceLease VehicleCommandService::exactInstance(const MavlinkComponentInstanceLease &lease)
{ return {lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch, ExactLeaseDomain::Component}; }
VehicleCommandService::ExactInstanceLease VehicleCommandService::exactInstance(const ExactCommandToken &token)
{ return token.isComponentOperation() ? exactInstance(token.componentLease) : exactInstance(token.lease); }

bool VehicleCommandService::reservationTokenMatches(const ExactReservationToken &token,
    const ExactReservationRecord &record)
{
    if (!token.isValid() || token.owner != record.owner) return false;
    QList<ExactInstanceLease> leases;
    for (const auto &lease : token.leases) leases.append(exactInstance(lease));
    for (const auto &lease : token.componentLeases) leases.append(exactInstance(lease));
    return leases == record.leases;
}

bool VehicleCommandService::validatorsAvailable(ExactReservationPolicy policy) const
{
    if (policy == ExactReservationPolicy::Component)
        return bool(m_componentLeaseValidator) && bool(m_componentRouteValidator);
    return bool(m_exactLeaseValidator) && (policy == ExactReservationPolicy::SingleVehicle
        ? bool(m_singleVehicleExactRouteValidator) : bool(m_exactRouteValidator));
}

bool VehicleCommandService::leaseIsCurrent(const ExactInstanceLease &lease) const
{
    if (!lease.isValid()) return false;
    if (lease.domain == ExactLeaseDomain::Component) {
        const auto validator = m_componentLeaseValidator;
        return validator && validator({lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch});
    }
    const auto validator = m_exactLeaseValidator;
    return validator && validator({lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch});
}

bool VehicleCommandService::routeIsEligible(const ExactInstanceLease &lease,
    ExactReservationPolicy policy, QString *error) const
{
    if (policy == ExactReservationPolicy::Component) {
        if (lease.domain != ExactLeaseDomain::Component) return false;
        const auto validator = m_componentRouteValidator;
        return validator && validator({lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch}, error);
    }
    if (lease.domain != ExactLeaseDomain::Swarm) return false;
    const auto validator = policy == ExactReservationPolicy::SingleVehicle
        ? m_singleVehicleExactRouteValidator : m_exactRouteValidator;
    return validator && validator({lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch}, error);
}

bool VehicleCommandService::reservationTargetIsCurrent(
    const ExactReservationRecord &reservation) const
{
    return reservation.policy != ExactReservationPolicy::SingleVehicle
        || (reservation.leases.size() == 1
            && reservation.target.isValid()
            && reservation.target.endpoint.sameIdentity(
                reservation.leases.first().endpoint)
            && targetIsCurrent(reservation.target)
            && m_targetManager->isTargetGenerationSettled());
}

void VehicleCommandService::handleTargetGenerationChanged(
    qulonglong generation)
{
    // Nested target selection may finish while an older signal is still
    // being delivered. Only the manager's final current generation may
    // retire state.
    if (!m_targetManager
        || generation != m_targetManager->targetGeneration()) {
        return;
    }

    clearPendingCommands();

    QList<quint64> affectedReservations;
    for (auto reservation = m_exactReservations.begin();
         reservation != m_exactReservations.end(); ++reservation) {
        if (reservation->policy == ExactReservationPolicy::SingleVehicle
            && reservation->target.generation != generation) {
            reservation->closing = true;
            affectedReservations.append(reservation.key());
        }
    }
    if (affectedReservations.isEmpty()) {
        return;
    }

    QList<quint64> affectedTransactions;
    for (auto pending = m_pendingExactCommands.constBegin();
         pending != m_pendingExactCommands.constEnd(); ++pending) {
        if (affectedReservations.contains(
                pending->token.reservationId)) {
            affectedTransactions.append(pending.key());
        }
    }
    std::sort(affectedTransactions.begin(), affectedTransactions.end());
    QPointer<VehicleCommandService> serviceGuard(this);
    for (quint64 transactionId : affectedTransactions) {
        if (m_pendingExactCommands.contains(transactionId)) {
            finishExactCommand(
                transactionId,
                ExactTerminalResult::LeaseRetiredOutcomeUncertain,
                -1, 255, 0, 0, 0,
                QStringLiteral(
                    "The selected vehicle target changed while awaiting acknowledgement."),
                true);
            if (serviceGuard.isNull()) {
                return;
            }
        }
    }
    for (quint64 reservationId : affectedReservations) {
        maybeReleaseClosingReservation(reservationId);
        if (serviceGuard.isNull()) {
            return;
        }
    }
}

bool VehicleCommandService::reservationContains(
    const ExactReservationRecord &reservation,
    const ExactInstanceLease &lease) const
{
    return std::any_of(
        reservation.leases.cbegin(), reservation.leases.cend(),
        [&lease](const ExactInstanceLease &candidate) {
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
    const mavlink_command_ack_t &acknowledgement, ExactLeaseDomain domain, quint64 physicalEpoch)
{
    VehicleEndpoint source;
    source.linkId = linkId;
    source.systemId = message.sysid;
    source.componentId = message.compid;

    const quint64 transactionId =
        m_pendingExactByEndpoint.value(source, 0);
    auto pending = m_pendingExactCommands.find(transactionId);
    if (transactionId != 0 && pending != m_pendingExactCommands.end()
        && exactInstance(pending->token).endpoint.sameIdentity(source)
        && exactInstance(pending->token).domain == domain
        && (physicalEpoch == 0 || exactInstance(pending->token).linkSessionEpoch == physicalEpoch)
        && static_cast<quint16>(pending->token.command)
            == acknowledgement.command
        && acknowledgementTargets(
            acknowledgement.target_system,
            acknowledgement.target_component,
            pending->localSystemId, pending->localComponentId)) {
        // An old ACK can arrive reentrantly while the first frame is still
        // being signed. There is no transmitted command to acknowledge yet.
        if (!pending->wasFrameAttempted()) return true;
        auto reservation = m_exactReservations.find(
            pending->token.reservationId);
        if (reservation == m_exactReservations.end()
            || !reservationTargetIsCurrent(*reservation)) {
            if (reservation != m_exactReservations.end()) {
                reservation->closing = true;
            }
            finishExactCommand(
                transactionId,
                ExactTerminalResult::LeaseRetiredOutcomeUncertain,
                -1, 255, 0, 0, 0,
                QStringLiteral(
                    "An acknowledgement arrived after the selected vehicle target changed."),
                true);
            return true;
        }
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
        const ExactInstanceLease acknowledgedLease = exactInstance(pending->token);
        QPointer<VehicleCommandService> serviceGuard(this);
        const bool acknowledgedLeaseCurrent =
            leaseIsCurrent(acknowledgedLease);
        if (serviceGuard.isNull()) {
            return true;
        }
        if (!acknowledgedLeaseCurrent) {
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
        if (m_exactClock.elapsed() >= pending->absoluteDeadlineMs) {
            finishExactCommand(transactionId, ExactTerminalResult::TimedOutOutcomeUncertain,
                -1, 255, 0, 0, 0,
                QStringLiteral("The command maximum lifetime expired during acknowledgement validation; outcome is uncertain."), true);
            return true;
        }
        reservation = m_exactReservations.find(
            pending->token.reservationId);
        if (reservation == m_exactReservations.end()
            || !reservationTargetIsCurrent(*reservation)) {
            if (reservation != m_exactReservations.end()) {
                reservation->closing = true;
            }
            finishExactCommand(
                transactionId,
                ExactTerminalResult::LeaseRetiredOutcomeUncertain,
                -1, 255, 0, 0, 0,
                QStringLiteral(
                    "The selected vehicle target changed during acknowledgement validation."),
                true);
            return true;
        }
        if (acknowledgement.result == MAV_RESULT_IN_PROGRESS) {
            pending->remainingRetries = 0;
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
            &quarantineIndex, int(domain), physicalEpoch)) {
        return false;
    }

    // A quarantined ACK belongs to an earlier transaction.  Consume it here
    // so it cannot impersonate either a new exact transaction or the legacy
    // selected-target signal. A terminal ACK can drain a single transmission
    // early, but not retries: additional indistinguishable ACKs may follow.
    // IN_PROGRESS extends the bounded quarantine.
    if (acknowledgement.result == MAV_RESULT_IN_PROGRESS) {
        m_exactQuarantines[quarantineIndex].expiresAtMs =
            m_exactClock.elapsed() + m_exactQuarantineMs;
    } else if (!m_exactQuarantines[quarantineIndex].multipleTransmissions) {
        m_exactQuarantines.removeAt(quarantineIndex);
    }
    scheduleQuarantineExpiry();
    return true;
}

bool VehicleCommandService::matchesQuarantine(
    const VehicleEndpoint &endpoint, quint16 command,
    quint8 targetSystem, quint8 targetComponent, int *index, int domain, quint64 physicalEpoch) const
{
    for (int candidate = 0; candidate < m_exactQuarantines.size();
         ++candidate) {
        const QuarantinedExactCommand &quarantine =
            m_exactQuarantines.at(candidate);
        if (quarantine.endpoint.sameIdentity(endpoint)
            && quarantine.command == command
            && (domain < 0 || int(quarantine.domain) == domain)
            && (physicalEpoch == 0 || quarantine.linkSessionEpoch == physicalEpoch)
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
    const auto lease = exactInstance(pending.token);
    const VehicleEndpoint endpoint = lease.endpoint;
    const quint16 command = static_cast<quint16>(pending.token.command);
    for (QuarantinedExactCommand &quarantine : m_exactQuarantines) {
        if (quarantine.endpoint.sameIdentity(endpoint)
            && quarantine.command == command
            && quarantine.domain == lease.domain && quarantine.linkSessionEpoch == lease.linkSessionEpoch
            && quarantine.localSystemId == pending.localSystemId
            && quarantine.localComponentId == pending.localComponentId) {
            quarantine.expiresAtMs =
                m_exactClock.elapsed() + m_exactQuarantineMs;
            quarantine.multipleTransmissions = quarantine.multipleTransmissions
                || pending.attemptedTransmissions() > 1;
            scheduleQuarantineExpiry();
            return;
        }
    }
    QuarantinedExactCommand quarantine;
    quarantine.endpoint = endpoint;
    quarantine.domain = lease.domain;
    quarantine.linkSessionEpoch = lease.linkSessionEpoch;
    quarantine.multipleTransmissions = pending.attemptedTransmissions() > 1;
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
    const QPointer<VehicleCommandService> serviceGuard(this);
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
            // A preceding terminal callback may have delivered IN_PROGRESS
            // for this still-owned transaction after the expired list was made.
            if (pending.deadlineMs > m_exactClock.elapsed()
                && pending.absoluteDeadlineMs > m_exactClock.elapsed()) continue;
            const bool maximumLifetimeExpired =
                pending.absoluteDeadlineMs <= now;
            if (!maximumLifetimeExpired && pending.remainingRetries > 0) {
                retryExactCommand(transactionId);
                if (!serviceGuard) return;
                continue;
            }
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
            if (serviceGuard.isNull()) {
                return;
            }
        }
    }
    scheduleExactDeadline();
}

void VehicleCommandService::retryExactCommand(quint64 transactionId)
{
    QPointer<VehicleCommandService> guard(this);
    auto active = m_pendingExactCommands.find(transactionId);
    if (active == m_pendingExactCommands.end()) return;
    // Nested event delivery while a signer/writer or another admission gate
    // is on the stack must never issue a second concurrent attempt.
    if (m_exactApiInFlight || active->inFlightAttempt) {
        active->deadlineMs = qMin(m_exactClock.elapsed() + 20, active->absoluteDeadlineMs);
        return;
    }
    const PendingExactCommand snapshot = *active;
    const auto lease = exactInstance(snapshot.token);
    const auto reservationId = snapshot.token.reservationId;
    const auto initial = m_exactReservations.constFind(reservationId);
    if (initial == m_exactReservations.cend()) {
        finishExactCommand(transactionId, ExactTerminalResult::TransportOutcomeUncertain,
            -1, 255, 0, 0, 0, QStringLiteral("The command reservation disappeared before retry; outcome is uncertain."), true);
        return;
    }
    const ExactReservationRecord reservedSnapshot = *initial;
    m_exactApiInFlight = true;
    ScopeExit apiGuard([guard] { if (guard) guard->m_exactApiInFlight = false; });
    const auto fail = [guard, transactionId](const QString &reason) {
        if (guard && guard->m_pendingExactCommands.contains(transactionId))
            guard->finishExactCommand(transactionId, ExactTerminalResult::TransportOutcomeUncertain,
                -1, 255, 0, 0, 0, reason, true);
    };
    const auto current = [&]() {
        if (!guard || !m_pendingExactCommands.contains(transactionId)) return false;
        const auto reserved = m_exactReservations.constFind(reservationId);
        if (reserved == m_exactReservations.cend() || reserved->closing || reserved->owner.isNull()
            || reserved->owner != reservedSnapshot.owner || reserved->policy != reservedSnapshot.policy
            || reserved->leases != reservedSnapshot.leases || !reservationTargetIsCurrent(*reserved)
            || !reservationContains(*reserved, lease)
            || m_exactEndpointReservations.value(lease.endpoint, 0) != reservationId) {
            fail(QStringLiteral("The command owner, reservation or selected target changed before retry; outcome is uncertain."));
            return false;
        }
        if (m_exactClock.elapsed() >= snapshot.absoluteDeadlineMs) {
            finishExactCommand(transactionId, ExactTerminalResult::TimedOutOutcomeUncertain,
                -1, 255, 0, 0, 0, QStringLiteral("The command maximum lifetime expired during retry validation; outcome is uncertain."), true);
            return false;
        }
        return true;
    };
    if (!current()) return;
    const bool initialLease = leaseIsCurrent(lease);
    if (!current()) return;
    if (!initialLease) { fail(QStringLiteral("The component or vehicle instance changed before retry; outcome is uncertain.")); return; }
    QString error;
    const bool route = routeIsEligible(lease, reservedSnapshot.policy, &error);
    if (!current()) return;
    if (!route) { fail(error.isEmpty() ? QStringLiteral("The command route is unavailable before retry; outcome is uncertain.") : error); return; }
    const bool finalLease = leaseIsCurrent(lease);
    if (!current()) return;
    if (!finalLease) { fail(QStringLiteral("The component or vehicle instance changed during retry validation; outcome is uncertain.")); return; }
    if (snapshot.request.validateBeforeWrite) {
        const bool allowed = snapshot.request.validateBeforeWrite(&error);
        if (!current()) return;
        if (!allowed) { fail(error.isEmpty() ? QStringLiteral("The command safety gate rejected a retry; outcome is uncertain.") : error); return; }
    }
    active = m_pendingExactCommands.find(transactionId);
    if (active == m_pendingExactCommands.end()) return;
    // A reentrant IN_PROGRESS ACK may have disabled retries without finishing.
    if (active->remainingRetries <= 0 || active->deadlineMs > m_exactClock.elapsed()) return;
    --active->remainingRetries; ++active->attemptIndex;
    active->deadlineMs = qMin(m_exactClock.elapsed() + active->timeoutMs, active->absoluteDeadlineMs);
    m_exactApiInFlight = false;
    transmitExactCommand(transactionId);
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
    // COMMAND_ACK has no transaction nonce. Even a definite ACK after retry
    // can be followed by an ACK for an earlier identical transmission. Keep
    // that ambiguity in the one shared LONG/INT/legacy quarantine domain.
    if (pending.attemptedTransmissions() > 1
        && (result == ExactTerminalResult::AcknowledgedAccepted
            || result == ExactTerminalResult::AcknowledgedRejected)) {
        quarantine = true;
    }
    const bool normalizedBeforeTransmission = !pending.wasFrameAttempted()
        && result != ExactTerminalResult::RejectedBeforeTransmission
        && result != ExactTerminalResult::AcknowledgedAccepted
        && result != ExactTerminalResult::AcknowledgedRejected;
    if (!pending.wasFrameAttempted()
        && result != ExactTerminalResult::AcknowledgedAccepted
        && result != ExactTerminalResult::AcknowledgedRejected) {
        result = ExactTerminalResult::RejectedBeforeTransmission;
        quarantine = false;
    }
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
    if (m_pendingExactByEndpoint.value(exactInstance(pending.token).endpoint, 0)
        == transactionId) {
        m_pendingExactByEndpoint.remove(exactInstance(pending.token).endpoint);
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
    report.frameAttempted = pending.wasFrameAttempted();
    report.transmissionAttempts = pending.attemptedTransmissions();
    report.ownerDetached = ownerDetached;
    report.description = normalizedBeforeTransmission
        ? QStringLiteral("The command ended before transmission because its ownership, route, or deadline was invalidated. No frame writer was invoked.")
        : description;

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
    const QList<ExactInstanceLease> leases = reservation->leases;
    disconnect(reservation->ownerDestroyedConnection);
    m_exactReservations.erase(reservation);
    for (const ExactInstanceLease &lease : leases) {
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
