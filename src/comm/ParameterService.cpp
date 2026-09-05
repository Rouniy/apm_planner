#include "ParameterService.h"

#include "ExactLinkTransmitter.h"
#include "VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"

#include <QSet>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace
{

constexpr int MaximumExactIntervalMs = 10 * 60 * 1000;

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

QString parameterName(const char *bytes, int size)
{
    QByteArray name(bytes, size);
    const int terminator = name.indexOf('\0');
    if (terminator >= 0) {
        name.truncate(terminator);
    }
    return QString::fromLatin1(name);
}

QString endpointLabel(const VehicleEndpoint &endpoint)
{
    return QStringLiteral("link %1, vehicle %2:%3")
        .arg(endpoint.linkId)
        .arg(endpoint.systemId)
        .arg(endpoint.componentId);
}

} // namespace

ParameterService::ParameterService(
    VehicleTargetManager *targetManager,
    ExactLinkTransmitter *transmitter,
    QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_transmitter(transmitter)
    , m_store(new ParameterStore(this))
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_transmitter);
    qRegisterMetaType<MavlinkComponentInstanceLease>();
    qRegisterMetaType<ExactReservationToken>();
    qRegisterMetaType<ExactReservationResult>();
    qRegisterMetaType<ExactOperationKind>();
    qRegisterMetaType<ExactReadRequest>();
    qRegisterMetaType<ExactWriteRequest>();
    qRegisterMetaType<ExactOperationToken>();
    qRegisterMetaType<ExactSubmitResult>();
    qRegisterMetaType<ExactTerminalResult>();
    qRegisterMetaType<ExactOperationReport>();
    m_exactClock.start();
    m_exactRetryTimer.setSingleShot(true);
    m_exactRetryTimer.setTimerType(Qt::PreciseTimer);
    m_exactQuarantineTimer.setSingleShot(true);
    m_exactQuarantineTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_exactRetryTimer, &QTimer::timeout,
            this, &ParameterService::handleExactRetryTimeout);
    connect(&m_exactQuarantineTimer, &QTimer::timeout,
            this, &ParameterService::cleanupExpiredExactQuarantines);
    m_listRetryTimer.setSingleShot(true);
    connect(&m_listRetryTimer, &QTimer::timeout,
            this, &ParameterService::handleListRetryTimeout);
    connect(m_targetManager,
            &VehicleTargetManager::targetGenerationChanged,
            this, &ParameterService::handleTargetGenerationChanged);
    connect(m_targetManager,
            &VehicleTargetManager::targetGenerationSettled,
            this, &ParameterService::handleTargetGenerationSettled);
    connect(m_targetManager, &VehicleTargetManager::currentTargetChanged,
            this, &ParameterService::syncSelectedEndpoint);
    m_lastHandledTargetGeneration = m_targetManager->targetGeneration();
    m_cancellingTransactions =
        !m_targetManager->isTargetGenerationSettled();
    syncSelectedEndpoint();
}

QObject *ParameterService::storeObject() const
{
    return m_store;
}

void ParameterService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

bool ParameterService::configureExactTransactions(
    ExactLeaseValidator leaseValidator,
    ExactRouteValidator routeValidator)
{
    if (m_exactApiInFlight || m_exactOperationActive
        || !m_exactReservations.isEmpty()
        || !leaseValidator || !routeValidator) {
        return false;
    }
    m_exactLeaseValidator = std::move(leaseValidator);
    m_exactRouteValidator = std::move(routeValidator);
    return static_cast<bool>(m_exactLeaseValidator)
        && static_cast<bool>(m_exactRouteValidator);
}

bool ParameterService::configureSingleVehicleExactRoute(
    ExactRouteValidator routeValidator)
{
    if (m_exactApiInFlight || m_exactOperationActive
        || !m_exactReservations.isEmpty() || !routeValidator) {
        return false;
    }
    m_singleVehicleExactRouteValidator = std::move(routeValidator);
    return static_cast<bool>(m_singleVehicleExactRouteValidator);
}

bool ParameterService::configureComponentExactTransactions(
    ComponentLeaseValidator leaseValidator,
    ComponentRouteValidator routeValidator)
{
    if (m_exactApiInFlight || m_exactOperationActive
        || !m_exactReservations.isEmpty()
        || !leaseValidator || !routeValidator) {
        return false;
    }
    m_componentLeaseValidator = std::move(leaseValidator);
    m_componentRouteValidator = std::move(routeValidator);
    return static_cast<bool>(m_componentLeaseValidator)
        && static_cast<bool>(m_componentRouteValidator);
}

ParameterService::ExactReservationResult
ParameterService::reserveExactEndpoints(
    QObject *owner,
    const QList<SwarmVehicleInstanceLease> &leases,
    ExactReservationToken *reservationOut,
    QString *error)
{
    QList<ExactInstanceLease> exactLeases;
    exactLeases.reserve(leases.size());
    for (const SwarmVehicleInstanceLease &lease : leases) {
        exactLeases.append(exactInstance(lease));
    }
    return reserveExactEndpointsWithPolicy(
        owner, exactLeases, ExactReservationPolicy::Swarm,
        VehicleTargetLease(), reservationOut, error);
}

ParameterService::ExactReservationResult
ParameterService::reserveSingleVehicleEndpoint(
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

ParameterService::ExactReservationResult
ParameterService::reserveComponentEndpoint(
    QObject *owner,
    const MavlinkComponentInstanceLease &lease,
    ExactReservationToken *reservationOut,
    QString *error)
{
    return reserveExactEndpointsWithPolicy(
        owner, QList<ExactInstanceLease>{exactInstance(lease)},
        ExactReservationPolicy::Component,
        VehicleTargetLease(), reservationOut, error);
}

ParameterService::ExactReservationResult
ParameterService::reserveExactEndpointsWithPolicy(
    QObject *owner,
    const QList<ExactInstanceLease> &leases,
    ExactReservationPolicy policy,
    const VehicleTargetLease &target,
    ExactReservationToken *reservationOut,
    QString *error)
{
    if (reservationOut) {
        *reservationOut = ExactReservationToken();
    }
    if (error) {
        error->clear();
    }
    QPointer<ParameterService> serviceGuard(this);
    if (m_exactApiInFlight) {
        if (error) {
            *error = QStringLiteral(
                "Another exact parameter API call is validating application policy.");
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
    if (!owner || owner == this || owner->thread() != thread()) {
        if (error) {
            *error = QStringLiteral(
                "The exact parameter owner must be a separate object on the service thread.");
        }
        return ExactReservationResult::InvalidOwner;
    }
    const bool singleVehicle =
        policy == ExactReservationPolicy::SingleVehicle;
    const bool component = policy == ExactReservationPolicy::Component;
    const bool leaseConfigured = component
        ? static_cast<bool>(m_componentLeaseValidator)
        : static_cast<bool>(m_exactLeaseValidator);
    const bool routeConfigured = component
        ? static_cast<bool>(m_componentRouteValidator)
        : singleVehicle
            ? static_cast<bool>(m_singleVehicleExactRouteValidator)
            : static_cast<bool>(m_exactRouteValidator);
    if (!leaseConfigured || !routeConfigured) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle registry or route validator is unavailable.");
        }
        return ExactReservationResult::ContextUnavailable;
    }
    if (((singleVehicle || component) && leases.size() != 1)
        || leases.isEmpty()
        || leases.size() > SwarmTelemetryRegistry::MaximumVehicleEndpoints) {
        if (error) {
            *error = QStringLiteral(
                "The exact parameter group is empty or too large.");
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

    QSet<VehicleEndpoint> endpoints;
    for (const ExactInstanceLease &lease : leases) {
        if (!lease.isValid()
            || (component
                != (lease.domain == ExactLeaseDomain::Component))
            || endpoints.contains(lease.endpoint)) {
            if (error) {
                *error = QStringLiteral(
                    "The exact parameter group contains an invalid or duplicate endpoint.");
            }
            return ExactReservationResult::InvalidLease;
        }
        endpoints.insert(lease.endpoint);
        const bool leaseCurrent = exactLeaseIsCurrent(lease);
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
        if (exactEndpointReserved(lease.endpoint)
            || legacyOperationTouches(lease.endpoint)) {
            if (error) {
                *error = QStringLiteral(
                    "The parameter protocol for %1 is busy.")
                    .arg(endpointLabel(lease.endpoint));
            }
            return ExactReservationResult::Busy;
        }
    }

    // Route validators are application callbacks. Publish nothing until every
    // route and lease has survived the callback boundary.
    for (const ExactInstanceLease &lease : leases) {
        QString routeError;
        const bool routeEligible = exactRouteIsEligible(
            lease, policy, &routeError);
        if (serviceGuard.isNull()) {
            return ExactReservationResult::ContextUnavailable;
        }
        if (!routeEligible) {
            if (error) {
                *error = routeError.isEmpty()
                    ? QStringLiteral("No safe parameter route is available for %1.")
                          .arg(endpointLabel(lease.endpoint))
                    : routeError;
            }
            return ExactReservationResult::RouteUnavailable;
        }
        if (ownerGuard.isNull()) {
            if (error) {
                *error = QStringLiteral(
                    "The exact parameter owner was destroyed during validation.");
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
        const bool leaseCurrent = exactLeaseIsCurrent(lease);
        if (serviceGuard.isNull()) {
            return ExactReservationResult::ContextUnavailable;
        }
        if (!leaseCurrent) {
            if (error) {
                *error = QStringLiteral(
                    "The exact parameter group changed during route validation.");
            }
            return ExactReservationResult::StaleLease;
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
        if (ownerGuard.isNull()) {
            if (error) {
                *error = QStringLiteral(
                    "The exact parameter owner was destroyed during validation.");
            }
            return ExactReservationResult::InvalidOwner;
        }
        if (exactEndpointReserved(lease.endpoint)
            || legacyOperationTouches(lease.endpoint)) {
            if (error) {
                *error = QStringLiteral(
                    "The parameter protocol became busy during validation.");
            }
            return ExactReservationResult::Busy;
        }
    }

    // A numeric endpoint can be rediscovered as a different exact instance.
    // ParameterStore has endpoint granularity, so never mix values carrying
    // incompatible instance provenance. Clear an older exact snapshot before
    // publishing the successor reservation, then repeat every externally
    // mutable eligibility check across the store's signal boundary.
    QSet<VehicleEndpoint> conflictingCacheEndpoints;
    for (const ExactInstanceLease &lease : leases) {
        const auto endpointCache =
            m_exactCachedValues.constFind(lease.endpoint);
        if (endpointCache == m_exactCachedValues.constEnd()) {
            continue;
        }
        const bool conflicts = std::any_of(
            endpointCache->cbegin(), endpointCache->cend(),
            [&lease](const ExactCachedValue &value) {
                return !value.lease.sameInstance(lease);
            });
        if (conflicts) {
            conflictingCacheEndpoints.insert(lease.endpoint);
        }
    }
    for (const VehicleEndpoint &endpoint :
         std::as_const(conflictingCacheEndpoints)) {
        m_exactCachedValues.remove(endpoint);
        m_store->removeEndpoint(endpoint);
        if (serviceGuard.isNull()) {
            return ExactReservationResult::ContextUnavailable;
        }
        if (ownerGuard.isNull()) {
            if (error) {
                *error = QStringLiteral(
                    "The exact parameter owner was destroyed during cache invalidation.");
            }
            return ExactReservationResult::InvalidOwner;
        }
    }
    if (!conflictingCacheEndpoints.isEmpty()) {
        syncSelectedEndpoint();
        if (serviceGuard.isNull()) {
            return ExactReservationResult::ContextUnavailable;
        }
        if (ownerGuard.isNull()) {
            if (error) {
                *error = QStringLiteral(
                    "The exact parameter owner was destroyed while restoring the selected endpoint.");
            }
            return ExactReservationResult::InvalidOwner;
        }
        for (const ExactInstanceLease &lease : leases) {
            if (exactEndpointReserved(lease.endpoint)
                || legacyOperationTouches(lease.endpoint)) {
                if (error) {
                    *error = QStringLiteral(
                        "The parameter protocol became busy during cache invalidation.");
                }
                return ExactReservationResult::Busy;
            }
            const bool leaseCurrent = exactLeaseIsCurrent(lease);
            if (serviceGuard.isNull()) {
                return ExactReservationResult::ContextUnavailable;
            }
            if (!leaseCurrent) {
                if (error) {
                    *error = QStringLiteral(
                        "The exact instance changed during cache invalidation.");
                }
                return ExactReservationResult::StaleLease;
            }
            QString routeError;
            const bool routeEligible = exactRouteIsEligible(
                lease, policy, &routeError);
            if (serviceGuard.isNull()) {
                return ExactReservationResult::ContextUnavailable;
            }
            if (!routeEligible) {
                if (error) {
                    *error = routeError.isEmpty()
                        ? QStringLiteral(
                            "The exact route changed during cache invalidation.")
                        : routeError;
                }
                return ExactReservationResult::RouteUnavailable;
            }
            if (ownerGuard.isNull()) {
                if (error) {
                    *error = QStringLiteral(
                        "The exact parameter owner was destroyed during cache validation.");
                }
                return ExactReservationResult::InvalidOwner;
            }
            const bool finalLeaseCurrent = exactLeaseIsCurrent(lease);
            if (serviceGuard.isNull()) {
                return ExactReservationResult::ContextUnavailable;
            }
            if (!finalLeaseCurrent) {
                if (error) {
                    *error = QStringLiteral(
                        "The exact instance changed during final cache validation.");
                }
                return ExactReservationResult::StaleLease;
            }
            if (ownerGuard.isNull()) {
                if (error) {
                    *error = QStringLiteral(
                        "The exact parameter owner was destroyed during final cache validation.");
                }
                return ExactReservationResult::InvalidOwner;
            }
            if (exactEndpointReserved(lease.endpoint)
                || legacyOperationTouches(lease.endpoint)) {
                if (error) {
                    *error = QStringLiteral(
                        "The parameter protocol became busy during final cache validation.");
                }
                return ExactReservationResult::Busy;
            }
            if (singleVehicle
                && (!targetIsCurrent(target)
                    || !m_targetManager->isTargetGenerationSettled())) {
                if (error) {
                    *error = QStringLiteral(
                        "The selected vehicle target changed during cache invalidation.");
                }
                return ExactReservationResult::StaleLease;
            }
        }
    }

    const quint64 reservationId = nextExactReservationId();
    ExactReservationRecord record;
    record.owner = ownerGuard;
    record.leases = leases;
    record.policy = policy;
    record.target = target;
    record.ownerDestroyedConnection = connect(
        owner, &QObject::destroyed, this,
        [this, reservationId]() {
            handleExactOwnerDestroyed(reservationId);
        });
    m_exactReservations.insert(reservationId, record);
    for (const ExactInstanceLease &lease : leases) {
        m_exactEndpointReservations.insert(
            lease.endpoint, reservationId);
    }
    if (reservationOut) {
        reservationOut->owner = ownerGuard;
        reservationOut->reservationId = reservationId;
        if (component) {
            const ExactInstanceLease &stored = leases.first();
            MavlinkComponentInstanceLease publicLease;
            publicLease.endpoint = stored.endpoint;
            publicLease.linkSessionEpoch = stored.linkSessionEpoch;
            publicLease.instanceEpoch = stored.instanceEpoch;
            reservationOut->componentLeases = {publicLease};
        } else {
            reservationOut->leases.reserve(leases.size());
            for (const ExactInstanceLease &stored : leases) {
                SwarmVehicleInstanceLease publicLease;
                publicLease.endpoint = stored.endpoint;
                publicLease.linkSessionEpoch = stored.linkSessionEpoch;
                publicLease.instanceEpoch = stored.instanceEpoch;
                reservationOut->leases.append(publicLease);
            }
        }
    }
    return ExactReservationResult::Reserved;
}

bool ParameterService::releaseExactReservation(
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
    maybeReleaseExactReservation(reservation.reservationId);
    return true;
}

bool ParameterService::cancelExactOperation(
    const ExactReservationToken &reservation,
    const ExactOperationToken &operation,
    const QString &reason)
{
    const auto reserved = m_exactReservations.constFind(
        reservation.reservationId);
    if (!operation.isValid() || reservation.reservationId == 0
        || reservation.owner.isNull()
        || reserved == m_exactReservations.constEnd()
        || reserved->owner != reservation.owner
        || !reservationTokenMatches(reservation, *reserved)
        || operation.reservationId != reservation.reservationId
        || !m_exactOperationActive
        || m_exactOperation.token.operationId != operation.operationId
        || m_exactOperation.token.reservationId != operation.reservationId
        || m_exactOperation.token.kind != operation.kind
        || m_exactOperation.token.name != operation.name
        || m_exactOperation.token.type != operation.type
        || m_exactOperation.token.normalizedValue
            != operation.normalizedValue
        || !exactInstance(m_exactOperation.token).sameInstance(
            exactInstance(operation))) {
        return false;
    }

    const bool write = m_exactOperation.token.kind
        == ExactOperationKind::Write;
    const bool outcomeUncertain = write && m_exactOperation.frameAttempted;
    QString description = reason.trimmed();
    if (description.isEmpty()) {
        if (!write) {
            description = QStringLiteral(
                "The exact parameter read was cancelled.");
        } else if (outcomeUncertain) {
            description = QStringLiteral(
                "The exact parameter write was cancelled after transmission; outcome is uncertain.");
        } else {
            description = QStringLiteral(
                "The exact parameter write was cancelled before transmission.");
        }
    } else if (outcomeUncertain) {
        description += QStringLiteral(
            " The write may already have reached the vehicle; outcome is uncertain.");
    }

    finishExactOperation(
        !write
            ? ExactTerminalResult::ReadCancelled
            : (outcomeUncertain
                   ? ExactTerminalResult::WriteCancelledOutcomeUncertain
                   : ExactTerminalResult::WriteCancelled),
        QVariant(), ParameterType::Unknown, description,
        outcomeUncertain);
    return true;
}

ParameterService::ExactSubmitResult ParameterService::submitExactRead(
    const ExactReservationToken &reservation,
    const SwarmVehicleInstanceLease &lease,
    const ExactReadRequest &request,
    ExactOperationToken *operationOut,
    QString *error)
{
    return submitExactOperation(
        reservation, exactInstance(lease), ExactOperationKind::Read,
        request.name, QVariant(), ParameterType::Unknown, false,
        operationOut, error);
}

ParameterService::ExactSubmitResult ParameterService::submitExactWrite(
    const ExactReservationToken &reservation,
    const SwarmVehicleInstanceLease &lease,
    const ExactWriteRequest &request,
    ExactOperationToken *operationOut,
    QString *error)
{
    return submitExactOperation(
        reservation, exactInstance(lease), ExactOperationKind::Write,
        request.name, request.value, request.type, request.force,
        operationOut, error);
}

ParameterService::ExactSubmitResult ParameterService::submitComponentRead(
    const ExactReservationToken &reservation,
    const MavlinkComponentInstanceLease &lease,
    const ExactReadRequest &request,
    ExactOperationToken *operationOut,
    QString *error)
{
    return submitExactOperation(
        reservation, exactInstance(lease), ExactOperationKind::Read,
        request.name, QVariant(), ParameterType::Unknown, false,
        operationOut, error);
}

ParameterService::ExactSubmitResult ParameterService::submitComponentWrite(
    const ExactReservationToken &reservation,
    const MavlinkComponentInstanceLease &lease,
    const ExactWriteRequest &request,
    ExactOperationToken *operationOut,
    QString *error)
{
    return submitExactOperation(
        reservation, exactInstance(lease), ExactOperationKind::Write,
        request.name, request.value, request.type, request.force,
        operationOut, error);
}

void ParameterService::retireExactVehicle(
    const SwarmVehicleInstanceLease &lease)
{
    retireExactInstance(exactInstance(lease));
}

void ParameterService::retireComponent(
    const MavlinkComponentInstanceLease &lease)
{
    retireExactInstance(exactInstance(lease));
}

void ParameterService::retireExactInstance(
    const ExactInstanceLease &lease)
{
    if (!lease.isValid()) {
        return;
    }
    QList<quint64> affectedReservations;
    for (auto reservation = m_exactReservations.begin();
         reservation != m_exactReservations.end(); ++reservation) {
        if (exactReservationContains(reservation.value(), lease)) {
            reservation->closing = true;
            affectedReservations.append(reservation.key());
        }
    }
    const bool operationBelongsToLease = m_exactOperationActive
        && exactInstance(m_exactOperation.token).sameInstance(lease);
    bool cachedValueBelongsToLease = false;
    bool cachedValueBelongsToAnotherInstance = false;
    auto cached = m_exactCachedValues.find(lease.endpoint);
    if (cached != m_exactCachedValues.end()) {
        for (auto value = cached->begin(); value != cached->end();) {
            if (value->lease.sameInstance(lease)) {
                cachedValueBelongsToLease = true;
                value = cached->erase(value);
            } else {
                cachedValueBelongsToAnotherInstance = true;
                ++value;
            }
        }
        if (cachedValueBelongsToLease
            && cachedValueBelongsToAnotherInstance) {
            // This should be prevented by the reservation admission barrier.
            // If legacy state predating that barrier is encountered, fail
            // closed instead of leaving retired values attributed to a newer
            // instance in the endpoint-granular ParameterStore.
            m_exactCachedValues.erase(cached);
            cachedValueBelongsToAnotherInstance = false;
        } else if (cached->isEmpty()) {
            m_exactCachedValues.erase(cached);
        }
    }

    // Component retirement notifications can be delayed past rediscovery of
    // the same numeric endpoint. With no component-domain ownership there is
    // nothing for this service to retire, and wiping a possibly unrelated
    // selected-vehicle snapshot would be unsafe. Swarm retirement preserves
    // its historical behavior: it also invalidates ordinary parameter data
    // for the retired autopilot instance.
    if (affectedReservations.isEmpty() && !operationBelongsToLease
        && !cachedValueBelongsToLease
        && lease.domain == ExactLeaseDomain::Component) {
        return;
    }
    QPointer<ParameterService> guard(this);
    if (!cachedValueBelongsToAnotherInstance) {
        m_store->removeEndpoint(lease.endpoint);
    }
    if (guard.isNull()) {
        return;
    }
    if (m_exactOperationActive && operationBelongsToLease
        && exactInstance(m_exactOperation.token).sameInstance(lease)) {
        const bool write = m_exactOperation.token.kind
            == ExactOperationKind::Write;
        finishExactOperation(
            write
                ? ExactTerminalResult::WriteLeaseRetiredOutcomeUncertain
                : ExactTerminalResult::ReadLeaseRetired,
            QVariant(), ParameterType::Unknown,
            QStringLiteral(
                "The exact vehicle instance retired during the parameter operation."),
            write && m_exactOperation.frameAttempted);
        if (guard.isNull()) {
            return;
        }
    }
    for (quint64 reservationId : affectedReservations) {
        maybeReleaseExactReservation(reservationId);
        if (guard.isNull()) {
            return;
        }
    }
}

bool ParameterService::isExactWriteQuarantined(
    const SwarmVehicleInstanceLease &lease,
    const QString &name,
    const QVariant &value,
    ParameterType type)
{
    return isComponentWriteQuarantined(
        MavlinkComponentInstanceLease{
            lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch},
        name, value, type);
}

bool ParameterService::isComponentWriteQuarantined(
    const MavlinkComponentInstanceLease &lease,
    const QString &name,
    const QVariant &value,
    ParameterType type)
{
    cleanupExpiredExactQuarantines();
    if (!lease.isValid()) {
        return false;
    }
    QVariant normalized;
    if (!normalizeParameterWrite(
            VehicleTargetLease{lease.endpoint, 1}, value, type,
            &normalized, nullptr, nullptr)) {
        return false;
    }
    return matchesExactWriteQuarantine(lease.endpoint, name);
}

int ParameterService::requestCurrentParameterList()
{
    return static_cast<int>(requestParameterList(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId));
}

int ParameterService::requestCurrentParameterRead(const QString &name)
{
    return static_cast<int>(requestParameterRead(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId, name));
}

int ParameterService::requestCurrentParameterReadByIndex(int index)
{
    return static_cast<int>(requestParameterReadByIndex(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId, index));
}

bool ParameterService::cancelCurrentParameterList()
{
    const VehicleTargetLease current = m_targetManager->acquireTarget();
    if (m_listActive) {
        if (!current.isValid()
            || current.generation != m_listTarget.generation
            || !current.endpoint.sameIdentity(m_listTarget.endpoint)) {
            return false;
        }
        cancelParameterList(true);
        return true;
    }
    if (!m_queuedList.active || !current.isValid()
        || current.generation != m_queuedList.target.generation
        || !current.endpoint.sameIdentity(m_queuedList.target.endpoint)) {
        return false;
    }
    cancelQueuedParameterList(true);
    return true;
}

int ParameterService::setCurrentParameter(
    const QString &name, const QVariant &value, int type)
{
    if (type < static_cast<int>(ParameterType::UInt8)
        || type > static_cast<int>(ParameterType::Real64)) {
        return static_cast<int>(SendResult::InvalidParameter);
    }
    return static_cast<int>(setParameter(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId,
        name, value, static_cast<ParameterType>(type)));
}

qulonglong ParameterService::writeCurrentParameter(
    const QString &name, const QVariant &value, bool force)
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid()) {
        return 0;
    }
    const ParameterSnapshot snapshot = m_store->snapshot(target.endpoint);
    const quint8 componentId =
        static_cast<quint8>(target.endpoint.componentId);
    if (!snapshot.contains(componentId, name)) {
        return 0;
    }
    const ParameterRecord record = snapshot.value(componentId, name);
    quint64 transactionId = 0;
    const bool wasPumping = m_pumpingTransactions;
    m_pumpingTransactions = true;
    const SendResult result = enqueueParameterWrite(
        target, m_localSystemId, m_localComponentId,
        name, value, record.type, force, true, 0, &transactionId);
    m_pumpingTransactions = wasPumping;
    if (!wasPumping && result == SendResult::Sent) {
        // The transaction ID must be returned to QML before a no-op,
        // synchronous acknowledgement or immediate transport failure can
        // emit the terminal signal carrying that ID.
        QTimer::singleShot(0, this, [this]() { pumpTransactions(); });
    }
    return result == SendResult::Sent ? transactionId : 0;
}

qulonglong ParameterService::writeCurrentParameters(
    const QVariantList &changes, bool force)
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid() || !targetIsCurrent(target) || changes.isEmpty()) {
        return 0;
    }
    if (exactEndpointReserved(target.endpoint)) {
        return 0;
    }

    struct Candidate
    {
        QString name;
        QVariant value;
        ParameterType type = ParameterType::Unknown;
    };
    QList<Candidate> candidates;
    candidates.reserve(changes.size());
    QSet<QString> names;
    const ParameterSnapshot snapshot = m_store->snapshot(target.endpoint);
    const quint8 componentId =
        static_cast<quint8>(target.endpoint.componentId);
    for (const QVariant &change : changes) {
        const QVariantMap map = change.toMap();
        const QString name = map.value(QStringLiteral("name")).toString();
        const QVariant value = map.value(QStringLiteral("value"));
        if (name.isEmpty() || !value.isValid() || names.contains(name)
            || !snapshot.contains(componentId, name)) {
            return 0;
        }
        const ParameterType type = snapshot.value(componentId, name).type;
        if (!validateParameterWrite(target, name, value, type)) {
            return 0;
        }
        names.insert(name);
        candidates.append(Candidate{name, value, type});
    }

    // Mission Planner intends enabling parameters to be written last. Its
    // historical comparator accidentally did the opposite; preserve the safe
    // documented behaviour and make ordering deterministic within each group.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &left, const Candidate &right) {
        const bool leftEnables = left.name.endsWith(
            QStringLiteral("ENABLE"), Qt::CaseSensitive);
        const bool rightEnables = right.name.endsWith(
            QStringLiteral("ENABLE"), Qt::CaseSensitive);
        if (leftEnables != rightEnables) {
            return !leftEnables;
        }
        return left.name < right.name;
    });

    cleanupExpiredExactQuarantines();
    for (const Candidate &candidate : std::as_const(candidates)) {
        QVariant normalized;
        if (!normalizeParameterWrite(
                target, candidate.value, candidate.type,
                &normalized, nullptr, nullptr)
            || matchesExactWriteQuarantine(
                target.endpoint, candidate.name)) {
            return 0;
        }
    }

    const quint64 batchId = m_nextBatchId++;
    m_pendingBatches.insert(batchId, PendingBatch{candidates.size(), 0, 0, 0});

    // Queue the complete, prevalidated batch before any item can start. This
    // keeps a signal handler from interleaving another write inside the batch.
    const bool wasPumping = m_pumpingTransactions;
    m_pumpingTransactions = true;
    for (const Candidate &candidate : candidates) {
        quint64 ignoredTransactionId = 0;
        const SendResult result = enqueueParameterWrite(
            target, m_localSystemId, m_localComponentId,
            candidate.name, candidate.value, candidate.type,
            force, true, batchId, &ignoredTransactionId);
        Q_ASSERT(result == SendResult::Sent);
    }
    m_pumpingTransactions = wasPumping;

    emit parameterBatchStarted(
        batchId, target.generation,
        target.endpoint.linkId, target.endpoint.systemId,
        target.endpoint.componentId, candidates.size());
    if (!wasPumping) {
        // As with a single high-level write, let the caller receive batchId
        // before any terminal batch signal can be emitted.
        QTimer::singleShot(0, this, [this]() { pumpTransactions(); });
    }
    return batchId;
}

bool ParameterService::cancelParameterWrite(qulonglong transactionId)
{
    if (transactionId == 0) {
        return false;
    }
    if (m_writeActive
        && m_activeWrite.transactionId == transactionId) {
        cancelActiveWrite();
        return true;
    }
    for (int index = 0; index < m_writeQueue.size(); ++index) {
        if (m_writeQueue.at(index).transactionId != transactionId) {
            continue;
        }
        const PendingWrite cancelled = m_writeQueue.takeAt(index);
        m_writesBeforeQueuedList.remove(cancelled.transactionId);
        emit parameterWriteCancelled(
            cancelled.transactionId, cancelled.batchId,
            cancelled.target.generation,
            cancelled.target.endpoint.linkId,
            cancelled.target.endpoint.systemId,
            cancelled.target.endpoint.componentId,
            cancelled.name);
        finishBatchItem(cancelled.batchId, false);
        pumpTransactions();
        return true;
    }
    return false;
}

QVariant ParameterService::currentParameterValue(const QString &name) const
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid()) {
        return {};
    }
    const ParameterSnapshot snapshot = m_store->snapshot(target.endpoint);
    const quint8 componentId =
        static_cast<quint8>(target.endpoint.componentId);
    return snapshot.contains(componentId, name)
        ? snapshot.value(componentId, name).value : QVariant{};
}

QVariantList ParameterService::currentParameters() const
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid()) {
        return {};
    }
    QList<ParameterRecord> records =
        m_store->snapshot(target.endpoint).records();
    std::sort(records.begin(), records.end(),
              [](const ParameterRecord &left,
                 const ParameterRecord &right) {
        return left.key.name < right.key.name;
    });
    QVariantList result;
    result.reserve(records.size());
    for (const ParameterRecord &record : records) {
        result.append(QVariantMap{
            {QStringLiteral("componentId"), record.key.componentId},
            {QStringLiteral("name"), record.key.name},
            {QStringLiteral("value"), record.value},
            {QStringLiteral("type"), static_cast<int>(record.type)},
            {QStringLiteral("index"), record.index},
            {QStringLiteral("reportedCount"), record.reportedCount}
        });
    }
    return result;
}

ParameterService::SendResult ParameterService::requestParameterList(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (exactEndpointReserved(target.endpoint)) {
        return SendResult::Busy;
    }
    cleanupExpiredExactQuarantines();
    if (exactEndpointQuarantined(target.endpoint)) {
        return SendResult::Busy;
    }

    // The manager has already installed the exact final identity, but signal
    // delivery may still be invalidating the previous generation. Preserve
    // the new intent without cancelling/transmitting from inside that phase.
    if (m_cancellingTransactions
        || !m_targetManager->isTargetGenerationSettled()) {
        if (m_queuedList.active
            && m_queuedList.target.generation == target.generation
            && m_queuedList.target.endpoint.sameIdentity(target.endpoint)) {
            return SendResult::Sent;
        }
        if (m_queuedList.active) {
            cancelQueuedParameterList(true);
        }
        m_queuedList.active = true;
        m_queuedList.target = target;
        m_queuedList.localSystemId = localSystemId;
        m_queuedList.localComponentId = localComponentId;
        m_writesBeforeQueuedList.clear();
        if (m_writeActive) {
            m_writesBeforeQueuedList.insert(m_activeWrite.transactionId);
        }
        for (const PendingWrite &write : m_writeQueue) {
            m_writesBeforeQueuedList.insert(write.transactionId);
        }
        return SendResult::Sent;
    }

    if (m_listActive
        && m_listTarget.generation == target.generation
        && m_listTarget.endpoint.sameIdentity(target.endpoint)) {
        // Setup, Config and QML consumers can coexist. A second refresh for
        // the same exact target joins the active transfer instead of
        // discarding its staged progress and flooding the link.
        return SendResult::Sent;
    }
    if (m_queuedList.active
        && m_queuedList.target.generation == target.generation
        && m_queuedList.target.endpoint.sameIdentity(target.endpoint)) {
        return SendResult::Sent;
    }
    if (m_listActive) {
        cancelParameterList(true);
    }
    if (m_queuedList.active) {
        cancelQueuedParameterList(true);
    }

    // A PARAM_VALUE is both list data and the acknowledgement for PARAM_SET.
    // Never overlap these protocols: a refresh requested during writes becomes
    // the next queue boundary and subsequent writes wait behind it.
    if (m_writeActive || !m_writeQueue.isEmpty()) {
        m_queuedList.active = true;
        m_queuedList.target = target;
        m_queuedList.localSystemId = localSystemId;
        m_queuedList.localComponentId = localComponentId;
        m_writesBeforeQueuedList.clear();
        if (m_writeActive) {
            m_writesBeforeQueuedList.insert(m_activeWrite.transactionId);
        }
        for (const PendingWrite &write : m_writeQueue) {
            m_writesBeforeQueuedList.insert(write.transactionId);
        }
        return SendResult::Sent;
    }
    return startParameterList(target, localSystemId, localComponentId);
}

ParameterService::SendResult ParameterService::startParameterList(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (exactEndpointReserved(target.endpoint)) {
        return SendResult::Busy;
    }
    cleanupExpiredExactQuarantines();
    if (exactEndpointQuarantined(target.endpoint)) {
        return SendResult::Busy;
    }
    m_listActive = true;
    m_listTarget = target;
    m_listLocalSystemId = localSystemId;
    m_listLocalComponentId = localComponentId;
    m_listReportedCount = 0;
    m_listReceivedIndices.clear();
    m_listIndexAttempts.clear();
    m_listNextMissingIndex = 0;
    m_listWholeRetries = 0;
    m_store->beginLoad(target.endpoint);
    emit listStarted(target.generation,
                     target.endpoint.linkId,
                     target.endpoint.systemId,
                     target.endpoint.componentId);

    const SendResult result = transmitParameterListRequest();
    if (result != SendResult::Sent) {
        failParameterList(QStringLiteral(
            "Unable to send PARAM_REQUEST_LIST on the selected link."));
        return result;
    }
    restartListTimer(m_listInactivityMs);
    return result;
}

ParameterService::SendResult ParameterService::requestParameterRead(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    const QString &name)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (exactEndpointReserved(target.endpoint)) {
        return SendResult::Busy;
    }
    cleanupExpiredExactQuarantines();
    if (exactEndpointQuarantined(target.endpoint)) {
        return SendResult::Busy;
    }
    QByteArray nameBytes;
    if (!parameterNameBytes(name, &nameBytes)) {
        return SendResult::InvalidParameter;
    }
    // A targetGenerationChanged listener can request data for the newly
    // selected endpoint while older listeners are still cancelling the old
    // generation. Do not transmit into that half-settled state: the explicit
    // StaleTarget result lets direct C++/QML callers retry after settlement.
    if (m_cancellingTransactions
        || !m_targetManager->isTargetGenerationSettled()) {
        return SendResult::StaleTarget;
    }

    const bool hadPrevious = m_pendingReads.contains(name);
    const PendingRead previous = m_pendingReads.value(name);
    m_pendingReads.insert(name, PendingRead{target});

    mavlink_param_request_read_t payload{};
    payload.target_system = static_cast<quint8>(target.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(target.endpoint.componentId);
    payload.param_index = -1;
    std::memcpy(payload.param_id, nameBytes.constData(),
                static_cast<size_t>(nameBytes.size()));
    mavlink_message_t message{};
    mavlink_msg_param_request_read_encode(
        localSystemId, localComponentId, &message, &payload);

    const SendResult result = send(
        target, localSystemId, localComponentId, message);
    if (result != SendResult::Sent) {
        const auto pending = m_pendingReads.constFind(name);
        if (pending != m_pendingReads.constEnd()
            && pending->target.generation == target.generation
            && pending->target.endpoint.sameIdentity(target.endpoint)) {
            if (hadPrevious) {
                m_pendingReads.insert(name, previous);
            } else {
                m_pendingReads.remove(name);
            }
        }
    }
    return result;
}

ParameterService::SendResult ParameterService::requestParameterReadByIndex(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    int index)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (exactEndpointReserved(target.endpoint)) {
        return SendResult::Busy;
    }
    cleanupExpiredExactQuarantines();
    if (exactEndpointQuarantined(target.endpoint)) {
        return SendResult::Busy;
    }
    if (index < 0 || index > std::numeric_limits<qint16>::max()) {
        return SendResult::InvalidParameter;
    }
    if (m_cancellingTransactions
        || !m_targetManager->isTargetGenerationSettled()) {
        return SendResult::StaleTarget;
    }

    mavlink_param_request_read_t payload{};
    payload.target_system = static_cast<quint8>(target.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(target.endpoint.componentId);
    payload.param_index = static_cast<qint16>(index);
    mavlink_message_t message{};
    mavlink_msg_param_request_read_encode(
        localSystemId, localComponentId, &message, &payload);
    return send(target, localSystemId, localComponentId, message);
}

ParameterService::SendResult ParameterService::setParameter(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    const QString &name, const QVariant &value, ParameterType type,
    bool force)
{
    return enqueueParameterWrite(
        target, localSystemId, localComponentId,
        name, value, type, force, false, 0, nullptr);
}

void ParameterService::observePhysicalMessage(
    int linkId, quint64 linkSessionEpoch,
    const mavlink_message_t &message)
{
    // Select the consumer exactly once before either path can emit a terminal
    // signal.  A synchronous completion handler may create a successor
    // operation for the same endpoint/name; the current physical frame must
    // never be reconsidered under that successor's domain.
    const ExactInstanceLease entryLease =
        exactInstance(m_exactOperation.token);
    const VehicleEndpoint source = endpointFor(linkId, message);
    const bool componentParameter = m_exactOperationActive
        && message.msgid == MAVLINK_MSG_ID_PARAM_VALUE
        && entryLease.domain == ExactLeaseDomain::Component
        && entryLease.endpoint.sameIdentity(source);
    if (componentParameter) {
        observeComponentMessage(linkId, linkSessionEpoch, message);
        return;
    }
    observeMessage(linkId, message);
}

void ParameterService::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (linkId < 0) {
        return;
    }

    const VehicleEndpoint source = endpointFor(linkId, message);
    if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        mavlink_heartbeat_t heartbeat{};
        mavlink_msg_heartbeat_decode(&message, &heartbeat);
        if (heartbeat.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA) {
            setEncoding(source, ParameterEncoding::CStyleCast);
        }
        return;
    }
    if (message.msgid == MAVLINK_MSG_ID_AUTOPILOT_VERSION) {
        mavlink_autopilot_version_t version{};
        mavlink_msg_autopilot_version_decode(&message, &version);
        if (version.capabilities & MAV_PROTOCOL_CAPABILITY_PARAM_FLOAT) {
            setEncoding(source, ParameterEncoding::CStyleCast);
        } else if (version.capabilities
                   & MAV_PROTOCOL_CAPABILITY_PARAM_UNION) {
            setEncoding(source, ParameterEncoding::Bytewise);
        }
        return;
    }
    if (message.msgid != MAVLINK_MSG_ID_PARAM_VALUE
        || !source.isValid()) {
        return;
    }

    mavlink_param_value_t payload{};
    mavlink_msg_param_value_decode(&message, &payload);
    const QString name = parameterName(
        payload.param_id, MAVLINK_MSG_PARAM_VALUE_FIELD_PARAM_ID_LEN);
    const ParameterType type = parameterType(payload.param_type);
    const ExactInstanceLease activeExactLease =
        exactInstance(m_exactOperation.token);
    const bool exactCandidate = m_exactOperationActive
        && activeExactLease.domain == ExactLeaseDomain::Swarm
        && m_exactOperation.token.name == name
        && activeExactLease.endpoint.sameIdentity(source);
    const quint64 candidateWriteId =
        m_writeActive && m_activeWrite.name == name
            && sameEnvelope(m_activeWrite.target, linkId, message)
            && targetIsCurrent(m_activeWrite.target)
        ? m_activeWrite.transactionId : 0;
    const ParameterEncoding decodeEncoding = exactCandidate
        ? m_exactOperation.encoding
        : candidateWriteId != 0
            ? m_activeWrite.encoding : encodingFor(source);
    bool decoded = false;
    const QVariant value = ParameterCodec::decodeClassic(
        payload.param_value, type, decodeEncoding, &decoded);
    if (name.isEmpty() || !decoded) {
        return;
    }

    cleanupExpiredExactQuarantines();
    if (matchesExactWriteQuarantine(source, name)) {
        // PARAM_VALUE has no transaction identifier. A quarantined echo must
        // not mutate cache state or satisfy any newer legacy/exact waiter.
        return;
    }
    if (exactCandidate) {
        observeExactParameterValue(
            linkId, message, source, name, type, value,
            payload.param_count, payload.param_index);
        return;
    }
    if (exactEndpointReserved(source)) {
        // A reservation is an exclusive PARAM_VALUE consumer boundary.  An
        // unsolicited or late legacy response must not mutate its cache.
        return;
    }

    // Legacy parameter operations remain leased to the selected endpoint.
    // Once the generation changes, late packets from the cancelled operation
    // cannot mutate a cache the user may later select again.
    if (!m_targetManager->isTargetGenerationSettled()) {
        return;
    }
    const VehicleTargetLease currentTarget =
        m_targetManager->acquireTarget();
    if (!sameEnvelope(currentTarget, linkId, message)
        || !targetIsCurrent(currentTarget)) {
        return;
    }
    touchLegacyTrafficFence(source);

    // A selected-target observation is not pinned to a swarm instance lease.
    // It remains valid legacy cache data, but it revokes any older exact-cache
    // provenance so a later exact write cannot skip from a stale value.
    auto exactEndpointCache = m_exactCachedValues.find(source);
    if (exactEndpointCache != m_exactCachedValues.end()) {
        exactEndpointCache->remove(name);
        if (exactEndpointCache->isEmpty()) {
            m_exactCachedValues.erase(exactEndpointCache);
        }
    }

    if (!m_store->ingest(source, payload.param_count, payload.param_index,
                         name, value, type)) {
        return;
    }
    if (!m_targetManager->isTargetGenerationSettled()
        || !targetIsCurrent(currentTarget)) {
        return;
    }

    emit parameterValueReceived(
        currentTarget.generation,
        source.linkId, source.systemId, source.componentId,
        payload.param_count, payload.param_index,
        name, value, static_cast<int>(type));
    if (!m_targetManager->isTargetGenerationSettled()
        || !targetIsCurrent(currentTarget)) {
        return;
    }

    if (m_listActive && sameEnvelope(m_listTarget, linkId, message)
        && targetIsCurrent(m_listTarget)) {
        if (payload.param_count != std::numeric_limits<quint16>::max()
            && payload.param_count > 0) {
            m_listReportedCount = qMax(
                m_listReportedCount, static_cast<int>(payload.param_count));
        }
        const bool countableIndex =
            payload.param_index != std::numeric_limits<quint16>::max()
            && payload.param_index < m_listReportedCount;
        const bool newIndex = countableIndex
            && !m_listReceivedIndices.contains(payload.param_index);
        if (newIndex) {
            m_listReceivedIndices.insert(payload.param_index);
            m_listIndexAttempts.remove(payload.param_index);
        }

        // ParameterSnapshot intentionally keeps exposing the last committed
        // data while a refresh is staged. Its isComplete() can therefore be
        // true for the old snapshot; completion must use this transaction's
        // own unique wire indices instead.
        if (m_listReportedCount > 0
            && m_listReceivedIndices.size() >= m_listReportedCount) {
            completeParameterList(source);
        } else if (newIndex) {
            // Mission Planner immediately enters recovery after seeing the
            // nominal final index with gaps; otherwise inactivity is measured
            // from the most recent unique list value.
            const bool nominalLast = m_listReportedCount > 0
                && payload.param_index == m_listReportedCount - 1;
            restartListTimer(nominalLast ? 0 : m_listInactivityMs);
        }
    }

    const auto read = m_pendingReads.find(name);
    if (read != m_pendingReads.end()
        && sameEnvelope(read->target, linkId, message)
        && targetIsCurrent(read->target)) {
        const VehicleTargetLease completed = read->target;
        m_pendingReads.erase(read);
        emit parameterRead(
            completed.generation, source.linkId, source.systemId,
            source.componentId, name, value, static_cast<int>(type));
    }

    if (candidateWriteId != 0
        && m_writeActive
        && m_activeWrite.transactionId == candidateWriteId
        && m_activeWrite.name == name
        && sameEnvelope(m_activeWrite.target, linkId, message)
        && targetIsCurrent(m_activeWrite.target)) {
        // Classic PARAM_VALUE has no transaction identifier. A same-name
        // value can be an unsolicited update, a read response, another GCS's
        // write, or a delayed echo. Only an exact type/value match is a safe
        // acknowledgement; mismatches remain authoritative cache updates but
        // leave this transaction pending for its exact echo or timeout.
        if (m_activeWrite.type == type
            && ParameterCodec::valuesEqual(
                m_activeWrite.value, value, type)) {
            acknowledgeActiveWrite(value, type);
        }
    }
}

void ParameterService::observeComponentMessage(
    int linkId, quint64 linkSessionEpoch,
    const mavlink_message_t &message)
{
    if (!m_exactOperationActive || linkId < 0
        || linkSessionEpoch == 0
        || message.msgid != MAVLINK_MSG_ID_PARAM_VALUE) {
        return;
    }
    const ExactInstanceLease lease = exactInstance(m_exactOperation.token);
    const VehicleEndpoint source = endpointFor(linkId, message);
    if (lease.domain != ExactLeaseDomain::Component
        || lease.linkSessionEpoch != linkSessionEpoch
        || !lease.endpoint.sameIdentity(source)) {
        return;
    }

    mavlink_param_value_t payload{};
    mavlink_msg_param_value_decode(&message, &payload);
    const QString name = parameterName(
        payload.param_id, MAVLINK_MSG_PARAM_VALUE_FIELD_PARAM_ID_LEN);
    if (name.isEmpty() || name != m_exactOperation.token.name) {
        return;
    }
    const ParameterType type = parameterType(payload.param_type);
    bool decoded = false;
    const QVariant value = ParameterCodec::decodeClassic(
        payload.param_value, type, m_exactOperation.encoding, &decoded);
    if (!decoded) {
        return;
    }

    cleanupExpiredExactQuarantines();
    if (matchesExactWriteQuarantine(source, name)) {
        return;
    }
    observeExactParameterValue(
        linkId, message, source, name, type, value,
        payload.param_count, payload.param_index);
}

void ParameterService::setEncoding(
    const VehicleEndpoint &endpoint, ParameterEncoding encoding)
{
    if (endpoint.isValid()) {
        m_encodings.insert(endpoint, encoding);
    }
}

void ParameterService::forgetLink(int linkId)
{
    QPointer<ParameterService> guard(this);
    QSet<quint64> cancelled;
    m_cancellingTransactions = true;
    QList<quint64> exactReservations;
    for (auto reservation = m_exactReservations.begin();
         reservation != m_exactReservations.end(); ++reservation) {
        const bool includesLink = std::any_of(
            reservation->leases.cbegin(), reservation->leases.cend(),
            [linkId](const ExactInstanceLease &lease) {
                return lease.endpoint.linkId == linkId;
            });
        if (includesLink) {
            reservation->closing = true;
            exactReservations.append(reservation.key());
        }
    }
    if (m_exactOperationActive
        && exactInstance(m_exactOperation.token).endpoint.linkId == linkId) {
        const bool write = m_exactOperation.token.kind
            == ExactOperationKind::Write;
        finishExactOperation(
            write
                ? ExactTerminalResult::WriteLinkForgottenOutcomeUncertain
                : ExactTerminalResult::ReadLinkForgotten,
            QVariant(), ParameterType::Unknown,
            QStringLiteral(
                "The exact parameter link was forgotten during the operation."),
            write && m_exactOperation.frameAttempted);
        if (guard.isNull()) {
            return;
        }
    }
    for (auto cached = m_exactCachedValues.begin();
         cached != m_exactCachedValues.end();) {
        if (cached.key().linkId == linkId) {
            cached = m_exactCachedValues.erase(cached);
        } else {
            ++cached;
        }
    }
    if (m_listActive && m_listTarget.endpoint.linkId == linkId) {
        cancelled.insert(m_listTarget.generation);
        cancelParameterList(true);
        if (guard.isNull()) {
            return;
        }
    }
    if (m_queuedList.active
        && m_queuedList.target.endpoint.linkId == linkId) {
        cancelled.insert(m_queuedList.target.generation);
        cancelQueuedParameterList(true);
        if (guard.isNull()) {
            return;
        }
    }
    for (auto it = m_pendingReads.begin(); it != m_pendingReads.end();) {
        if (it->target.endpoint.linkId == linkId) {
            cancelled.insert(it->target.generation);
            it = m_pendingReads.erase(it);
        } else {
            ++it;
        }
    }
    if (m_writeActive && m_activeWrite.target.endpoint.linkId == linkId) {
        cancelled.insert(m_activeWrite.target.generation);
        cancelActiveWrite();
        if (guard.isNull()) {
            return;
        }
    }
    for (const PendingWrite &write : m_writeQueue) {
        if (write.target.endpoint.linkId == linkId) {
            cancelled.insert(write.target.generation);
        }
    }
    cancelQueuedWritesForGeneration(0, linkId);
    if (guard.isNull()) {
        return;
    }
    for (auto it = m_encodings.begin(); it != m_encodings.end();) {
        if (it.key().linkId == linkId) {
            it = m_encodings.erase(it);
        } else {
            ++it;
        }
    }
    for (auto fence = m_legacyTrafficFenceExpiries.begin();
         fence != m_legacyTrafficFenceExpiries.end();) {
        if (fence.key().linkId == linkId) {
            fence = m_legacyTrafficFenceExpiries.erase(fence);
        } else {
            ++fence;
        }
    }
    const QList<VehicleEndpoint> endpoints = m_store->cachedEndpoints();
    for (const VehicleEndpoint &endpoint : endpoints) {
        if (endpoint.linkId == linkId) {
            m_store->removeEndpoint(endpoint);
            if (guard.isNull()) {
                return;
            }
        }
    }
    for (quint64 generation : cancelled) {
        emit transactionsCancelled(generation);
        if (guard.isNull()) {
            return;
        }
    }
    for (quint64 reservationId : exactReservations) {
        maybeReleaseExactReservation(reservationId);
        if (guard.isNull()) {
            return;
        }
    }
    m_cancellingTransactions = false;
    pumpTransactions();
}

void ParameterService::setRetryPolicyForTesting(
    int inactivityMs, int missingBurstIntervalMs,
    int maximumWholeListRetries, int maximumIndexAttempts)
{
    m_listInactivityMs = qMax(1, inactivityMs);
    m_listMissingBurstIntervalMs = qMax(1, missingBurstIntervalMs);
    m_listMaximumWholeRetries = qMax(0, maximumWholeListRetries);
    m_listMaximumIndexAttempts = qMax(1, maximumIndexAttempts);
}

void ParameterService::setWriteRetryPolicyForTesting(
    int acknowledgementTimeoutMs, int maximumRetries)
{
    m_writeAcknowledgementTimeoutMs = qMax(1, acknowledgementTimeoutMs);
    m_writeMaximumRetries = qMax(0, maximumRetries);
}

void ParameterService::setExactRetryPolicyForTesting(
    int readRetryIntervalMs,
    int readMaximumRetries,
    int writeRetryIntervalMs,
    int writeMaximumRetries,
    int writeMaximumLifetimeMs,
    int quarantineMs)
{
    m_exactReadRetryIntervalMs = qBound(
        1, readRetryIntervalMs, MaximumExactIntervalMs);
    m_exactReadMaximumRetries = qBound(0, readMaximumRetries, 1000000);
    m_exactWriteRetryIntervalMs = qBound(
        1, writeRetryIntervalMs, MaximumExactIntervalMs);
    m_exactWriteMaximumRetries = qBound(
        0, writeMaximumRetries, 1000000);
    m_exactWriteMaximumLifetimeMs = qBound(
        1, writeMaximumLifetimeMs, MaximumExactIntervalMs);
    m_exactWriteQuarantineMs = qBound(
        1, quarantineMs, MaximumExactIntervalMs);
    cleanupExpiredExactQuarantines();
}

ParameterService::SendResult ParameterService::enqueueParameterWrite(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    const QString &name, const QVariant &value, ParameterType type,
    bool force, bool schemaBound, quint64 batchId,
    quint64 *transactionId)
{
    if (transactionId) {
        *transactionId = 0;
    }
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (exactEndpointReserved(target.endpoint)) {
        return SendResult::Busy;
    }
    if (!validateParameterWrite(target, name, value, type)) {
        return SendResult::InvalidParameter;
    }

    PendingWrite write;
    write.transactionId = m_nextWriteTransactionId++;
    if (write.transactionId == 0) {
        write.transactionId = m_nextWriteTransactionId++;
    }
    write.batchId = batchId;
    write.target = target;
    write.localSystemId = localSystemId;
    write.localComponentId = localComponentId;
    write.name = name;
    write.type = type;
    write.requestedValue = value;
    if (!normalizeParameterWrite(
            target, value, type, &write.value,
            &write.wireValue, &write.encoding)) {
        return SendResult::InvalidParameter;
    }
    cleanupExpiredExactQuarantines();
    if (matchesExactWriteQuarantine(target.endpoint, name)) {
        return SendResult::Busy;
    }
    write.force = force;
    write.schemaBound = schemaBound;

    if (transactionId) {
        *transactionId = write.transactionId;
    }
    m_writeQueue.enqueue(write);
    pumpTransactions();
    return SendResult::Sent;
}

bool ParameterService::validateParameterWrite(
    const VehicleTargetLease &target,
    const QString &name, const QVariant &value, ParameterType type,
    QByteArray *nameBytes) const
{
    if (!target.isValid() || !targetIsCurrent(target) || !value.isValid()) {
        return false;
    }
    QByteArray encodedName;
    if (!parameterNameBytes(name, &encodedName)) {
        return false;
    }
    if (!normalizeParameterWrite(
            target, value, type, nullptr, nullptr, nullptr)) {
        return false;
    }
    if (nameBytes) {
        *nameBytes = encodedName;
    }
    return true;
}

bool ParameterService::normalizeParameterWrite(
    const VehicleTargetLease &target,
    const QVariant &value, ParameterType type,
    QVariant *normalizedValue, float *wireValue,
    ParameterEncoding *encoding) const
{
    const ParameterEncoding selectedEncoding = encodingFor(target.endpoint);
    bool encoded = false;
    const float encodedValue = ParameterCodec::encodeClassic(
        value, type, selectedEncoding, &encoded);
    if (!encoded) {
        return false;
    }
    bool decoded = false;
    const QVariant normalized = ParameterCodec::decodeClassic(
        encodedValue, type, selectedEncoding, &decoded);
    if (!decoded) {
        return false;
    }
    // C-style integer parameters travel through a float. Values that cannot
    // make that round trip exactly would otherwise wait forever for an ACK the
    // vehicle can never echo as requested.
    if (ParameterCodec::isInteger(type)
        && !ParameterCodec::valuesEqual(value, normalized, type)) {
        return false;
    }
    if (normalizedValue) {
        *normalizedValue = normalized;
    }
    if (wireValue) {
        *wireValue = encodedValue;
    }
    if (encoding) {
        *encoding = selectedEncoding;
    }
    return true;
}

void ParameterService::pumpTransactions()
{
    if (m_cancellingTransactions
        || !m_targetManager->isTargetGenerationSettled()) {
        return;
    }
    if (m_pumpingTransactions) {
        m_pumpAgain = true;
        return;
    }

    m_pumpingTransactions = true;
    do {
        m_pumpAgain = false;
        if (m_listActive || m_writeActive) {
            break;
        }

        if (m_queuedList.active
            && m_writesBeforeQueuedList.isEmpty()) {
            const QueuedListRequest queued = m_queuedList;
            m_queuedList = {};
            const SendResult result = startParameterList(
                queued.target, queued.localSystemId,
                queued.localComponentId);
            if (result == SendResult::Sent && m_listActive) {
                break;
            }
            continue;
        }

        if (m_writeQueue.isEmpty()) {
            // A boundary can only retain IDs for queued/active writes. Avoid a
            // deadlock if an operation was removed by a future cancellation
            // path without reaching a terminal helper.
            if (m_queuedList.active) {
                m_writesBeforeQueuedList.clear();
                m_pumpAgain = true;
            }
            continue;
        }

        m_activeWrite = m_writeQueue.dequeue();
        m_writeActive = true;
        startActiveWrite();
    } while (m_pumpAgain
             || (!m_listActive && !m_writeActive
                 && (m_queuedList.active || !m_writeQueue.isEmpty())));
    m_pumpingTransactions = false;
}

void ParameterService::startActiveWrite()
{
    if (!m_writeActive) {
        return;
    }
    if (!targetIsCurrent(m_activeWrite.target)) {
        failActiveWrite(
            WriteFailureReason::StaleTarget,
            QStringLiteral("The selected vehicle target changed."));
        return;
    }

    const ParameterSnapshot snapshot = m_store->snapshot(
        m_activeWrite.target.endpoint);
    const quint8 componentId = static_cast<quint8>(
        m_activeWrite.target.endpoint.componentId);
    if (m_activeWrite.schemaBound) {
        if (!snapshot.contains(componentId, m_activeWrite.name)) {
            failActiveWrite(
                WriteFailureReason::InvalidParameter,
                QStringLiteral("The parameter is no longer present in the current vehicle snapshot."));
            return;
        }
        if (snapshot.value(componentId, m_activeWrite.name).type
            != m_activeWrite.type) {
            failActiveWrite(
                WriteFailureReason::TypeMismatch,
                QStringLiteral("The parameter type changed while the write was queued."));
            return;
        }
    }
    if (!normalizeParameterWrite(
            m_activeWrite.target, m_activeWrite.requestedValue,
            m_activeWrite.type, &m_activeWrite.value,
            &m_activeWrite.wireValue, &m_activeWrite.encoding)) {
        failActiveWrite(
            WriteFailureReason::InvalidParameter,
            QStringLiteral("The value cannot be represented using the current parameter encoding."));
        return;
    }
    m_activeWrite.skip = false;
    if (!m_activeWrite.force
        && snapshot.contains(componentId, m_activeWrite.name)) {
        const ParameterRecord committed = snapshot.value(
            componentId, m_activeWrite.name);
        m_activeWrite.skip = committed.type == m_activeWrite.type
            && ParameterCodec::valuesEqual(
                committed.value, m_activeWrite.value,
                m_activeWrite.type);
    }

    const quint64 transactionId = m_activeWrite.transactionId;
    emit parameterWriteStarted(
        transactionId, m_activeWrite.batchId,
        m_activeWrite.target.generation,
        m_activeWrite.target.endpoint.linkId,
        m_activeWrite.target.endpoint.systemId,
        m_activeWrite.target.endpoint.componentId,
        m_activeWrite.name, m_activeWrite.value,
        static_cast<int>(m_activeWrite.type));
    if (!m_writeActive
        || m_activeWrite.transactionId != transactionId) {
        return;
    }
    if (m_activeWrite.skip) {
        skipActiveWrite();
        return;
    }

    const SendResult result = transmitActiveWrite();
    if (!m_writeActive
        || m_activeWrite.transactionId != transactionId) {
        // The transmitter callback may synchronously deliver PARAM_VALUE and
        // complete this operation before sendMessage() returns.
        return;
    }
    if (result != SendResult::Sent) {
        failActiveWrite(
            result == SendResult::StaleTarget
                ? WriteFailureReason::StaleTarget
                : result == SendResult::InvalidParameter
                    ? WriteFailureReason::InvalidParameter
                    : WriteFailureReason::TransportUnavailable,
            QStringLiteral("Unable to send PARAM_SET on the selected link."));
        return;
    }
    scheduleActiveWriteTimeout();
}

ParameterService::SendResult ParameterService::transmitActiveWrite()
{
    if (!m_writeActive) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(m_activeWrite.target)) {
        return SendResult::StaleTarget;
    }
    QByteArray nameBytes;
    if (!parameterNameBytes(m_activeWrite.name, &nameBytes)) {
        return SendResult::InvalidParameter;
    }

    mavlink_param_set_t payload{};
    payload.target_system = static_cast<quint8>(
        m_activeWrite.target.endpoint.systemId);
    payload.target_component = static_cast<quint8>(
        m_activeWrite.target.endpoint.componentId);
    payload.param_value = m_activeWrite.wireValue;
    payload.param_type = static_cast<quint8>(m_activeWrite.type);
    std::memcpy(payload.param_id, nameBytes.constData(),
                static_cast<size_t>(nameBytes.size()));
    mavlink_message_t message{};
    mavlink_msg_param_set_encode(
        m_activeWrite.localSystemId, m_activeWrite.localComponentId,
        &message, &payload);
    ++m_activeWrite.attempts;
    return send(m_activeWrite.target,
                m_activeWrite.localSystemId,
                m_activeWrite.localComponentId, message);
}

void ParameterService::scheduleActiveWriteTimeout()
{
    if (!m_writeActive) {
        return;
    }
    const quint64 transactionId = m_activeWrite.transactionId;
    const int attempt = m_activeWrite.attempts;
    QTimer::singleShot(
        m_writeAcknowledgementTimeoutMs, this,
        [this, transactionId, attempt]() {
            handleWriteRetryTimeout(transactionId, attempt);
        });
}

void ParameterService::handleWriteRetryTimeout(
    quint64 transactionId, int attempt)
{
    if (!m_writeActive
        || m_activeWrite.transactionId != transactionId
        || m_activeWrite.attempts != attempt) {
        return;
    }
    if (!targetIsCurrent(m_activeWrite.target)) {
        failActiveWrite(
            WriteFailureReason::StaleTarget,
            QStringLiteral("The selected vehicle target changed."));
        return;
    }
    if (m_activeWrite.attempts >= 1 + m_writeMaximumRetries) {
        failActiveWrite(
            WriteFailureReason::Timeout,
            QStringLiteral("The vehicle did not acknowledge PARAM_SET."));
        return;
    }
    const SendResult result = transmitActiveWrite();
    if (!m_writeActive
        || m_activeWrite.transactionId != transactionId) {
        return;
    }
    if (result != SendResult::Sent) {
        failActiveWrite(
            result == SendResult::StaleTarget
                ? WriteFailureReason::StaleTarget
                : WriteFailureReason::TransportUnavailable,
            QStringLiteral("A PARAM_SET retry could not use the selected link."));
        return;
    }
    emit parameterWriteRetried(
        m_activeWrite.transactionId, m_activeWrite.attempts);
    scheduleActiveWriteTimeout();
}

void ParameterService::acknowledgeActiveWrite(
    const QVariant &value, ParameterType type)
{
    if (!m_writeActive) {
        return;
    }
    const PendingWrite completed = m_activeWrite;
    m_writeActive = false;
    m_activeWrite = {};
    m_writesBeforeQueuedList.remove(completed.transactionId);

    emit parameterWriteAcknowledged(
        completed.target.generation,
        completed.target.endpoint.linkId,
        completed.target.endpoint.systemId,
        completed.target.endpoint.componentId,
        completed.name, value, static_cast<int>(type));
    emit parameterWriteCompleted(
        completed.transactionId, completed.batchId,
        completed.target.generation,
        completed.target.endpoint.linkId,
        completed.target.endpoint.systemId,
        completed.target.endpoint.componentId,
        completed.name, value, static_cast<int>(type));
    finishBatchItem(completed.batchId, true);
    pumpTransactions();
}

void ParameterService::skipActiveWrite()
{
    if (!m_writeActive) {
        return;
    }
    const PendingWrite skipped = m_activeWrite;
    m_writeActive = false;
    m_activeWrite = {};
    m_writesBeforeQueuedList.remove(skipped.transactionId);

    // Existing widgets treat the legacy acknowledgement as terminal success.
    emit parameterWriteAcknowledged(
        skipped.target.generation,
        skipped.target.endpoint.linkId,
        skipped.target.endpoint.systemId,
        skipped.target.endpoint.componentId,
        skipped.name, skipped.value, static_cast<int>(skipped.type));
    emit parameterWriteSkipped(
        skipped.transactionId, skipped.batchId,
        skipped.target.generation,
        skipped.target.endpoint.linkId,
        skipped.target.endpoint.systemId,
        skipped.target.endpoint.componentId,
        skipped.name, skipped.value, static_cast<int>(skipped.type));
    finishBatchItem(skipped.batchId, true);
    pumpTransactions();
}

void ParameterService::failActiveWrite(
    WriteFailureReason reason, const QString &message)
{
    if (!m_writeActive) {
        return;
    }
    const PendingWrite failed = m_activeWrite;
    m_writeActive = false;
    m_activeWrite = {};
    m_writesBeforeQueuedList.remove(failed.transactionId);
    emit parameterWriteFailed(
        failed.transactionId, failed.batchId,
        failed.target.generation,
        failed.target.endpoint.linkId,
        failed.target.endpoint.systemId,
        failed.target.endpoint.componentId,
        failed.name, static_cast<int>(reason), message);
    finishBatchItem(failed.batchId, false);
    pumpTransactions();
}

void ParameterService::cancelActiveWrite()
{
    if (!m_writeActive) {
        return;
    }
    const PendingWrite cancelled = m_activeWrite;
    m_writeActive = false;
    m_activeWrite = {};
    m_writesBeforeQueuedList.remove(cancelled.transactionId);
    emit parameterWriteCancelled(
        cancelled.transactionId, cancelled.batchId,
        cancelled.target.generation,
        cancelled.target.endpoint.linkId,
        cancelled.target.endpoint.systemId,
        cancelled.target.endpoint.componentId,
        cancelled.name);
    finishBatchItem(cancelled.batchId, false);
    pumpTransactions();
}

void ParameterService::cancelQueuedWritesForGeneration(
    quint64 generation, int linkId)
{
    QQueue<PendingWrite> retained;
    QList<PendingWrite> cancelled;
    while (!m_writeQueue.isEmpty()) {
        const PendingWrite write = m_writeQueue.dequeue();
        const bool generationMatches = generation == 0
            || write.target.generation == generation;
        const bool linkMatches = linkId < 0
            || write.target.endpoint.linkId == linkId;
        if (generationMatches && linkMatches) {
            cancelled.append(write);
        } else {
            retained.enqueue(write);
        }
    }
    m_writeQueue = retained;
    for (const PendingWrite &write : cancelled) {
        m_writesBeforeQueuedList.remove(write.transactionId);
        emit parameterWriteCancelled(
            write.transactionId, write.batchId,
            write.target.generation,
            write.target.endpoint.linkId,
            write.target.endpoint.systemId,
            write.target.endpoint.componentId,
            write.name);
        finishBatchItem(write.batchId, false);
    }
}

void ParameterService::finishBatchItem(quint64 batchId, bool succeeded)
{
    if (batchId == 0) {
        return;
    }
    auto batch = m_pendingBatches.find(batchId);
    if (batch == m_pendingBatches.end()) {
        return;
    }
    ++batch->completed;
    if (succeeded) {
        ++batch->succeeded;
    } else {
        ++batch->failed;
    }
    const PendingBatch progress = *batch;
    const bool complete = progress.completed >= progress.total;
    if (complete) {
        m_pendingBatches.erase(batch);
    }
    emit parameterBatchProgress(
        batchId, progress.completed, progress.total,
        progress.succeeded, progress.failed);
    if (complete) {
        emit parameterBatchCompleted(
            batchId, progress.succeeded, progress.failed);
    }
}

void ParameterService::cancelQueuedParameterList(bool notify)
{
    if (!m_queuedList.active) {
        return;
    }
    const QueuedListRequest cancelled = m_queuedList;
    m_queuedList = {};
    m_writesBeforeQueuedList.clear();
    if (notify) {
        emit listCancelled(
            cancelled.target.generation,
            cancelled.target.endpoint.linkId,
            cancelled.target.endpoint.systemId,
            cancelled.target.endpoint.componentId);
    }
    pumpTransactions();
}

ParameterService::ExactSubmitResult
ParameterService::submitExactOperation(
    const ExactReservationToken &reservation,
    const ExactInstanceLease &lease,
    ExactOperationKind kind,
    const QString &name,
    const QVariant &value,
    ParameterType type,
    bool force,
    ExactOperationToken *operationOut,
    QString *error)
{
    if (operationOut) {
        *operationOut = ExactOperationToken();
    }
    if (error) {
        error->clear();
    }
    QPointer<ParameterService> serviceGuard(this);
    if (m_exactApiInFlight) {
        if (error) {
            *error = QStringLiteral(
                "Another exact parameter API call is validating application policy.");
        }
        return ExactSubmitResult::Busy;
    }
    m_exactApiInFlight = true;
    ScopeExit apiCallGuard([serviceGuard]() {
        if (!serviceGuard.isNull()) {
            serviceGuard->m_exactApiInFlight = false;
        }
    });
    const bool componentLease = lease.domain == ExactLeaseDomain::Component;
    if (componentLease
            ? !m_componentLeaseValidator
            : !m_exactLeaseValidator) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle registry is unavailable.");
        }
        return ExactSubmitResult::ContextUnavailable;
    }
    if (m_exactOperationActive) {
        if (error) {
            *error = QStringLiteral(
                "Another exact parameter operation is already active.");
        }
        return ExactSubmitResult::Busy;
    }
    if (!lease.isValid()) {
        if (error) {
            *error = QStringLiteral("The exact vehicle lease is invalid.");
        }
        return ExactSubmitResult::InvalidLease;
    }
    QByteArray nameBytes;
    if (!parameterNameBytes(name, &nameBytes)) {
        if (error) {
            *error = QStringLiteral("The exact parameter name is invalid.");
        }
        return ExactSubmitResult::InvalidParameter;
    }
    QPointer<QObject> ownerGuard(reservation.owner);
    auto reserved = m_exactReservations.find(reservation.reservationId);
    if (reservation.reservationId == 0 || ownerGuard.isNull()
        || reserved == m_exactReservations.end() || reserved->closing
        || reserved->owner != ownerGuard
        || !reservationTokenMatches(reservation, *reserved)
        || !exactReservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId) {
        if (error) {
            *error = ownerGuard.isNull()
                ? QStringLiteral("The exact parameter owner is unavailable.")
                : QStringLiteral(
                    "The exact parameter reservation is invalid or closing.");
        }
        return ownerGuard.isNull()
            ? ExactSubmitResult::InvalidOwner
            : ExactSubmitResult::InvalidReservation;
    }
    const ExactReservationPolicy reservationPolicy = reserved->policy;
    if ((componentLease
             && reservationPolicy != ExactReservationPolicy::Component)
        || (!componentLease
            && reservationPolicy == ExactReservationPolicy::Component)) {
        if (error) {
            *error = QStringLiteral(
                "The exact parameter reservation belongs to a different lease domain.");
        }
        return ExactSubmitResult::InvalidReservation;
    }
    const VehicleTargetLease reservationTarget = reserved->target;
    const bool routeConfigured =
        reservationPolicy == ExactReservationPolicy::Component
        ? static_cast<bool>(m_componentRouteValidator)
        : reservationPolicy == ExactReservationPolicy::SingleVehicle
            ? static_cast<bool>(m_singleVehicleExactRouteValidator)
            : static_cast<bool>(m_exactRouteValidator);
    if (!routeConfigured) {
        if (error) {
            *error = QStringLiteral(
                "The exact parameter route validator is unavailable.");
        }
        return ExactSubmitResult::ContextUnavailable;
    }
    if (!exactReservationTargetIsCurrent(*reserved)) {
        if (error) {
            *error = QStringLiteral(
                "The selected vehicle target is stale or still changing.");
        }
        return ExactSubmitResult::StaleLease;
    }
    const bool initialLeaseCurrent = exactLeaseIsCurrent(lease);
    if (serviceGuard.isNull()) {
        return ExactSubmitResult::ContextUnavailable;
    }
    if (!initialLeaseCurrent) {
        if (error) {
            *error = QStringLiteral("The exact vehicle lease is stale.");
        }
        return ExactSubmitResult::StaleLease;
    }
    reserved = m_exactReservations.find(reservation.reservationId);
    if (reserved == m_exactReservations.end()
        || reserved->closing || reserved->owner != ownerGuard
        || !reservationTokenMatches(reservation, *reserved)
        || !exactReservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId
        || reserved->policy != reservationPolicy
        || reserved->target.generation != reservationTarget.generation
        || !reserved->target.endpoint.sameIdentity(
            reservationTarget.endpoint)
        || !exactReservationTargetIsCurrent(*reserved)) {
        if (error) {
            *error = QStringLiteral(
                "The selected vehicle target changed during instance validation.");
        }
        return ExactSubmitResult::StaleLease;
    }
    if (legacyOperationTouches(lease.endpoint)) {
        if (error) {
            *error = QStringLiteral(
                "A legacy parameter operation owns this endpoint.");
        }
        return ExactSubmitResult::Busy;
    }
    cleanupExpiredExactQuarantines();
    if (matchesExactWriteQuarantine(lease.endpoint, name)) {
        if (error) {
            *error = QStringLiteral(
                "A previous uncertain write response for this exact parameter is still quarantined.");
        }
        return ExactSubmitResult::Quarantined;
    }
    const quint8 operationLocalSystemId = m_localSystemId;
    const quint8 operationLocalComponentId = m_localComponentId;

    QVariant normalizedValue;
    float wireValue = 0.0F;
    ParameterEncoding encoding = encodingFor(lease.endpoint);
    if (kind == ExactOperationKind::Write) {
        VehicleTargetLease normalizationTarget;
        normalizationTarget.endpoint = lease.endpoint;
        if (!value.isValid()
            || !normalizeParameterWrite(
                normalizationTarget, value, type,
                &normalizedValue, &wireValue, &encoding)) {
            if (error) {
                *error = QStringLiteral(
                    "The exact parameter value cannot be represented.");
            }
            return ExactSubmitResult::InvalidParameter;
        }
    }

    QString routeError;
    const bool routeEligible = exactRouteIsEligible(
        lease, reservationPolicy, &routeError);
    if (serviceGuard.isNull()) {
        return ExactSubmitResult::ContextUnavailable;
    }
    if (!routeEligible) {
        if (error) {
            *error = routeError.isEmpty()
                ? QStringLiteral("No safe exact parameter route is available.")
                : routeError;
        }
        return ExactSubmitResult::RouteUnavailable;
    }

    // The route validator can synchronously retire endpoints, release the
    // reservation, or start another operation. Revalidate every dependency.
    reserved = m_exactReservations.find(reservation.reservationId);
    if (ownerGuard.isNull()) {
        if (error) {
            *error = QStringLiteral(
                "The exact parameter owner was destroyed during validation.");
        }
        return ExactSubmitResult::InvalidOwner;
    }
    if (m_exactOperationActive || reserved == m_exactReservations.end()
        || reserved->closing || reserved->owner != ownerGuard
        || !reservationTokenMatches(reservation, *reserved)
        || !exactReservationContains(*reserved, lease)
        || m_exactEndpointReservations.value(lease.endpoint, 0)
            != reservation.reservationId
        || legacyOperationTouches(lease.endpoint)) {
        if (error) {
            *error = QStringLiteral(
                "The parameter protocol became busy during validation.");
        }
        return ExactSubmitResult::Busy;
    }
    if (reserved->policy != reservationPolicy
        || reserved->target.generation != reservationTarget.generation
        || !reserved->target.endpoint.sameIdentity(
            reservationTarget.endpoint)
        || !exactReservationTargetIsCurrent(*reserved)) {
        if (error) {
            *error = QStringLiteral(
                "The selected vehicle target changed during route validation.");
        }
        return ExactSubmitResult::StaleLease;
    }
    const bool finalLeaseCurrent = exactLeaseIsCurrent(lease);
    if (serviceGuard.isNull()) {
        return ExactSubmitResult::ContextUnavailable;
    }
    if (!finalLeaseCurrent) {
        if (error) {
            *error = QStringLiteral(
                "The exact vehicle lease changed during route validation.");
        }
        return ExactSubmitResult::StaleLease;
    }
    reserved = m_exactReservations.find(reservation.reservationId);
    if (reserved == m_exactReservations.end()
        || reserved->closing || reserved->owner != ownerGuard
        || !reservationTokenMatches(reservation, *reserved)
        || reserved->policy != reservationPolicy
        || reserved->target.generation != reservationTarget.generation
        || !reserved->target.endpoint.sameIdentity(
            reservationTarget.endpoint)
        || !exactReservationTargetIsCurrent(*reserved)) {
        if (error) {
            *error = QStringLiteral(
                "The exact reservation or selected target changed during validation.");
        }
        return ExactSubmitResult::StaleLease;
    }
    cleanupExpiredExactQuarantines();
    if (matchesExactWriteQuarantine(lease.endpoint, name)) {
        if (error) {
            *error = QStringLiteral(
                "A previous uncertain write response appeared during validation.");
        }
        return ExactSubmitResult::Quarantined;
    }
    if (kind == ExactOperationKind::Write) {
        VehicleTargetLease normalizationTarget;
        normalizationTarget.endpoint = lease.endpoint;
        if (!normalizeParameterWrite(
                normalizationTarget, value, type,
                &normalizedValue, &wireValue, &encoding)) {
            if (error) {
                *error = QStringLiteral(
                    "The exact parameter encoding changed to an incompatible representation during validation.");
            }
            return ExactSubmitResult::InvalidParameter;
        }
    }

    // Policy callbacks can recurse into this API.  Keep them excluded while
    // validating, but allow normal synchronous completion handlers to submit
    // the next operation after the waiter below has reached a terminal state.
    m_exactApiInFlight = false;

    PendingExactOperation operation;
    operation.token.operationId = nextExactOperationId();
    operation.token.reservationId = reservation.reservationId;
    if (componentLease) {
        operation.token.componentLease.endpoint = lease.endpoint;
        operation.token.componentLease.linkSessionEpoch =
            lease.linkSessionEpoch;
        operation.token.componentLease.instanceEpoch = lease.instanceEpoch;
    } else {
        operation.token.lease.endpoint = lease.endpoint;
        operation.token.lease.linkSessionEpoch = lease.linkSessionEpoch;
        operation.token.lease.instanceEpoch = lease.instanceEpoch;
    }
    operation.token.kind = kind;
    operation.token.name = name;
    operation.token.type = type;
    operation.token.normalizedValue = normalizedValue;
    operation.localSystemId = operationLocalSystemId;
    operation.localComponentId = operationLocalComponentId;
    operation.encoding = encoding;
    operation.wireValue = wireValue;
    if (kind == ExactOperationKind::Read) {
        operation.retryIntervalMs = m_exactReadRetryIntervalMs;
        operation.maximumAttempts = 1 + m_exactReadMaximumRetries;
    } else {
        operation.retryIntervalMs = m_exactWriteRetryIntervalMs;
        operation.maximumAttempts = 1 + m_exactWriteMaximumRetries;
        operation.absoluteDeadlineMs =
            m_exactClock.elapsed() + m_exactWriteMaximumLifetimeMs;
    }

    m_exactOperation = operation;
    m_exactOperationActive = true;
    if (operationOut) {
        *operationOut = operation.token;
    }

    if (kind == ExactOperationKind::Write && !force
        && exactCacheMatches(lease, name, normalizedValue, type)) {
        finishExactOperation(
            ExactTerminalResult::WriteSkipped,
            normalizedValue, type,
            QStringLiteral(
                "The same exact-instance value is already confirmed."),
            false);
        return ExactSubmitResult::Started;
    }

    return transmitExactOperation();
}

ParameterService::ExactSubmitResult
ParameterService::transmitExactOperation()
{
    if (!m_exactOperationActive) {
        return ExactSubmitResult::Busy;
    }
    const quint64 operationId = m_exactOperation.token.operationId;
    const ExactInstanceLease operationLease =
        exactInstance(m_exactOperation.token);
    const bool write = m_exactOperation.token.kind
        == ExactOperationKind::Write;
    QByteArray nameBytes;
    if (!parameterNameBytes(m_exactOperation.token.name, &nameBytes)) {
        finishExactOperation(
            m_exactOperation.token.kind == ExactOperationKind::Write
                ? ExactTerminalResult::Rejected
                : ExactTerminalResult::ReadTransportFailure,
            QVariant(), ParameterType::Unknown,
            QStringLiteral("The exact parameter name became invalid."),
            false);
        return ExactSubmitResult::InvalidParameter;
    }

    mavlink_message_t message{};
    if (m_exactOperation.token.kind == ExactOperationKind::Read) {
        mavlink_param_request_read_t payload{};
        payload.target_system = static_cast<quint8>(
            operationLease.endpoint.systemId);
        payload.target_component = static_cast<quint8>(
            operationLease.endpoint.componentId);
        payload.param_index = -1;
        std::memcpy(payload.param_id, nameBytes.constData(),
                    static_cast<size_t>(nameBytes.size()));
        mavlink_msg_param_request_read_encode(
            m_exactOperation.localSystemId,
            m_exactOperation.localComponentId,
            &message, &payload);
    } else {
        mavlink_param_set_t payload{};
        payload.target_system = static_cast<quint8>(
            operationLease.endpoint.systemId);
        payload.target_component = static_cast<quint8>(
            operationLease.endpoint.componentId);
        payload.param_value = m_exactOperation.wireValue;
        payload.param_type = static_cast<quint8>(
            m_exactOperation.token.type);
        std::memcpy(payload.param_id, nameBytes.constData(),
                    static_cast<size_t>(nameBytes.size()));
        mavlink_msg_param_set_encode(
            m_exactOperation.localSystemId,
            m_exactOperation.localComponentId,
            &message, &payload);
    }

    if (write
        && m_exactClock.elapsed() >= m_exactOperation.absoluteDeadlineMs) {
        const bool frameAttempted = m_exactOperation.frameAttempted;
        finishExactOperation(
            frameAttempted
                ? ExactTerminalResult::WriteTimedOutOutcomeUncertain
                : ExactTerminalResult::WriteTimedOut,
            QVariant(), ParameterType::Unknown,
            frameAttempted
                ? QStringLiteral(
                    "The exact parameter write maximum lifetime expired before retransmission; outcome is uncertain.")
                : QStringLiteral(
                    "The exact parameter write maximum lifetime expired before its first transmission."),
            frameAttempted);
        return ExactSubmitResult::Started;
    }
    ++m_exactOperation.attempts;
    const bool priorFrameAttempted = m_exactOperation.frameAttempted;
    bool frameWriterInvoked = false;
    QPointer<ParameterService> guard(this);
    const ExactLinkTransmitter::SendResult sent =
        m_transmitter->sendMessage(
            operationLease.endpoint.linkId,
            m_exactOperation.localSystemId,
            m_exactOperation.localComponentId, message,
            &frameWriterInvoked);
    if (guard.isNull()) {
        return write && (priorFrameAttempted || frameWriterInvoked)
            ? ExactSubmitResult::TransportOutcomeUncertain
            : ExactSubmitResult::TransportUnavailable;
    }
    // A test transport or in-process simulation can deliver PARAM_VALUE
    // synchronously from sendMessage(). The exact waiter is already installed.
    if (!m_exactOperationActive
        || m_exactOperation.token.operationId != operationId) {
        return ExactSubmitResult::Started;
    }
    m_exactOperation.frameAttempted =
        priorFrameAttempted || frameWriterInvoked;
    if (sent != ExactLinkTransmitter::SendResult::Sent) {
        const bool rejectedBeforeTransmission =
            write
            && sent == ExactLinkTransmitter::SendResult::SigningUnavailable
            && !m_exactOperation.frameAttempted;
        finishExactOperation(
            rejectedBeforeTransmission
                ? ExactTerminalResult::Rejected
                : write
                ? ExactTerminalResult::WriteTransportOutcomeUncertain
                : ExactTerminalResult::ReadTransportFailure,
            QVariant(), ParameterType::Unknown,
            rejectedBeforeTransmission
                ? QStringLiteral(
                    "Signing was unavailable; the exact parameter write was rejected before transmission.")
                : QStringLiteral(
                    "The frame writer did not confirm exact parameter transport."),
            write && m_exactOperation.frameAttempted);
        return write && !rejectedBeforeTransmission
            ? ExactSubmitResult::TransportOutcomeUncertain
            : ExactSubmitResult::TransportUnavailable;
    }
    scheduleExactRetry();
    return ExactSubmitResult::Started;
}

void ParameterService::scheduleExactRetry()
{
    if (!m_exactOperationActive) {
        m_exactRetryTimer.stop();
        return;
    }
    qint64 delay = m_exactOperation.retryIntervalMs;
    if (m_exactOperation.token.kind == ExactOperationKind::Write) {
        delay = qMin(
            delay,
            m_exactOperation.absoluteDeadlineMs - m_exactClock.elapsed());
    }
    m_exactRetryTimer.start(static_cast<int>(qMax<qint64>(1, delay)));
}

void ParameterService::handleExactRetryTimeout()
{
    if (!m_exactOperationActive) {
        return;
    }
    const ExactOperationToken token = m_exactOperation.token;
    const ExactInstanceLease lease = exactInstance(token);
    QPointer<ParameterService> guard(this);
    const bool write = token.kind == ExactOperationKind::Write;
    auto reservation = m_exactReservations.constFind(token.reservationId);
    if (reservation == m_exactReservations.constEnd()
        || !exactReservationTargetIsCurrent(*reservation)) {
        retireExactInstance(lease);
        return;
    }
    const ExactReservationPolicy reservationPolicy = reservation->policy;
    if (write
        && m_exactClock.elapsed() >= m_exactOperation.absoluteDeadlineMs) {
        finishExactOperation(
            ExactTerminalResult::WriteTimedOutOutcomeUncertain,
            QVariant(), ParameterType::Unknown,
            QStringLiteral(
                "The exact parameter write maximum lifetime expired; outcome is uncertain."),
            m_exactOperation.frameAttempted);
        return;
    }
    if (m_exactOperation.attempts >= m_exactOperation.maximumAttempts) {
        finishExactOperation(
            write
                ? ExactTerminalResult::WriteTimedOutOutcomeUncertain
                : ExactTerminalResult::ReadTimedOut,
            QVariant(), ParameterType::Unknown,
            write
                ? QStringLiteral(
                    "The exact parameter write exhausted bounded retries; outcome is uncertain.")
                : QStringLiteral(
                    "The exact parameter read exhausted bounded retries."),
            write && m_exactOperation.frameAttempted);
        return;
    }
    const bool initialLeaseCurrent = exactLeaseIsCurrent(lease);
    if (guard.isNull()) {
        return;
    }
    if (!initialLeaseCurrent) {
        retireExactInstance(lease);
        return;
    }
    if (!m_exactOperationActive
        || m_exactOperation.token.operationId != token.operationId) {
        return;
    }
    reservation = m_exactReservations.constFind(token.reservationId);
    if (reservation == m_exactReservations.constEnd()
        || reservation->policy != reservationPolicy
        || !exactReservationTargetIsCurrent(*reservation)) {
        retireExactInstance(lease);
        return;
    }
    QString routeError;
    const bool routeEligible = exactRouteIsEligible(
        lease, reservationPolicy, &routeError);
    if (guard.isNull()) {
        return;
    }
    if (!routeEligible) {
        if (m_exactOperationActive
            && m_exactOperation.token.operationId == token.operationId) {
            finishExactOperation(
                write
                    ? ExactTerminalResult::WriteTransportOutcomeUncertain
                    : ExactTerminalResult::ReadTransportFailure,
                QVariant(), ParameterType::Unknown,
                routeError.isEmpty()
                    ? QStringLiteral(
                        "The exact parameter route became unavailable.")
                    : routeError,
                write && m_exactOperation.frameAttempted);
        }
        return;
    }
    if (!m_exactOperationActive
        || m_exactOperation.token.operationId != token.operationId) {
        return;
    }
    reservation = m_exactReservations.constFind(token.reservationId);
    if (reservation == m_exactReservations.constEnd()
        || reservation->policy != reservationPolicy
        || !exactReservationTargetIsCurrent(*reservation)) {
        retireExactInstance(lease);
        return;
    }
    const bool finalLeaseCurrent = exactLeaseIsCurrent(lease);
    if (guard.isNull()) {
        return;
    }
    if (!finalLeaseCurrent) {
        retireExactInstance(lease);
        return;
    }
    reservation = m_exactReservations.constFind(token.reservationId);
    if (!m_exactOperationActive
        || m_exactOperation.token.operationId != token.operationId) {
        return;
    }
    if (reservation == m_exactReservations.constEnd()
        || reservation->policy != reservationPolicy
        || !exactReservationTargetIsCurrent(*reservation)) {
        retireExactInstance(lease);
        return;
    }
    const ExactSubmitResult result = transmitExactOperation();
    if (guard.isNull()) {
        return;
    }
    if (result == ExactSubmitResult::Started
        && m_exactOperationActive
        && m_exactOperation.token.operationId == token.operationId) {
        emit exactOperationRetried(
            m_exactOperation.token, m_exactOperation.attempts);
    }
}

void ParameterService::finishExactOperation(
    ExactTerminalResult result,
    const QVariant &value,
    ParameterType type,
    const QString &description,
    bool quarantine)
{
    if (!m_exactOperationActive) {
        return;
    }
    const PendingExactOperation completed = m_exactOperation;
    m_exactRetryTimer.stop();
    m_exactOperationActive = false;
    m_exactOperation = PendingExactOperation();
    if (quarantine && completed.frameAttempted
        && completed.token.kind == ExactOperationKind::Write) {
        addExactWriteQuarantine(completed);
    }

    bool ownerDetached = true;
    const auto reservation = m_exactReservations.constFind(
        completed.token.reservationId);
    if (reservation != m_exactReservations.constEnd()) {
        ownerDetached = reservation->owner.isNull();
    }
    ExactOperationReport report;
    report.token = completed.token;
    report.terminalResult = result;
    report.value = value.isValid()
        ? value : completed.token.normalizedValue;
    report.type = type != ParameterType::Unknown
        ? type : completed.token.type;
    report.attempts = completed.attempts;
    report.frameAttempted = completed.frameAttempted;
    report.ownerDetached = ownerDetached;
    report.description = description;

    QPointer<ParameterService> guard(this);
    emit exactOperationFinished(report);
    if (!guard.isNull()) {
        maybeReleaseExactReservation(completed.token.reservationId);
    }
}

bool ParameterService::observeExactParameterValue(
    int linkId,
    const mavlink_message_t &message,
    const VehicleEndpoint &source,
    const QString &name,
    ParameterType type,
    const QVariant &value,
    int parameterCount,
    int parameterIndex)
{
    const ExactInstanceLease operationLease =
        exactInstance(m_exactOperation.token);
    if (!m_exactOperationActive
        || m_exactOperation.token.name != name
        || !operationLease.endpoint.sameIdentity(source)
        || source.linkId != linkId
        || source.systemId != message.sysid
        || source.componentId != message.compid) {
        return false;
    }
    const quint64 operationId = m_exactOperation.token.operationId;
    const auto reservation = m_exactReservations.constFind(
        m_exactOperation.token.reservationId);
    if (reservation == m_exactReservations.constEnd()
        || !exactReservationTargetIsCurrent(*reservation)) {
        retireExactInstance(operationLease);
        return true;
    }
    if (m_exactOperation.token.kind == ExactOperationKind::Write
        && m_exactClock.elapsed()
            >= m_exactOperation.absoluteDeadlineMs) {
        finishExactOperation(
            ExactTerminalResult::WriteTimedOutOutcomeUncertain,
            QVariant(), ParameterType::Unknown,
            QStringLiteral(
                "The exact parameter write maximum lifetime expired; outcome is uncertain."),
            m_exactOperation.frameAttempted);
        return true;
    }
    QPointer<ParameterService> guard(this);
    const bool leaseCurrent = exactLeaseIsCurrent(operationLease);
    if (guard.isNull()) {
        return true;
    }
    if (!leaseCurrent) {
        retireExactInstance(operationLease);
        return true;
    }
    if (!m_exactOperationActive
        || m_exactOperation.token.operationId != operationId) {
        return true;
    }
    const auto currentReservation = m_exactReservations.constFind(
        m_exactOperation.token.reservationId);
    if (currentReservation == m_exactReservations.constEnd()
        || !exactReservationTargetIsCurrent(*currentReservation)) {
        retireExactInstance(operationLease);
        return true;
    }
    const bool ingested = m_store->ingest(
            source, parameterCount, parameterIndex,
            name, value, type);
    if (guard.isNull()) {
        return true;
    }
    if (!ingested) {
        return true;
    }
    if (!m_exactOperationActive
        || m_exactOperation.token.operationId != operationId) {
        return true;
    }
    auto verifiedReservation = m_exactReservations.constFind(
        m_exactOperation.token.reservationId);
    if (verifiedReservation == m_exactReservations.constEnd()
        || !exactReservationContains(*verifiedReservation, operationLease)
        || m_exactEndpointReservations.value(operationLease.endpoint, 0)
            != m_exactOperation.token.reservationId
        || !exactReservationTargetIsCurrent(*verifiedReservation)) {
        retireExactInstance(operationLease);
        return true;
    }
    const ExactReservationPolicy policy = verifiedReservation->policy;
    const bool leaseStillCurrent = exactLeaseIsCurrent(operationLease);
    if (guard.isNull()) {
        return true;
    }
    if (!leaseStillCurrent) {
        retireExactInstance(operationLease);
        return true;
    }
    if (!m_exactOperationActive
        || m_exactOperation.token.operationId != operationId) {
        return true;
    }
    QString routeError;
    const bool routeStillEligible = exactRouteIsEligible(
        operationLease, policy, &routeError);
    if (guard.isNull()) {
        return true;
    }
    if (!routeStillEligible) {
        m_store->removeEndpoint(operationLease.endpoint);
        if (guard.isNull()
            || !m_exactOperationActive
            || m_exactOperation.token.operationId != operationId) {
            return true;
        }
        const bool write = m_exactOperation.token.kind
            == ExactOperationKind::Write;
        finishExactOperation(
            write
                ? ExactTerminalResult::WriteTransportOutcomeUncertain
                : ExactTerminalResult::ReadTransportFailure,
            QVariant(), ParameterType::Unknown,
            routeError.isEmpty()
                ? QStringLiteral(
                    "The exact parameter route changed while publishing the response.")
                : routeError,
            write && m_exactOperation.frameAttempted);
        return true;
    }
    const bool finalLeaseCurrent = exactLeaseIsCurrent(operationLease);
    if (guard.isNull()) {
        return true;
    }
    if (!finalLeaseCurrent) {
        retireExactInstance(operationLease);
        return true;
    }
    if (!m_exactOperationActive
        || m_exactOperation.token.operationId != operationId) {
        return true;
    }
    verifiedReservation = m_exactReservations.constFind(
        m_exactOperation.token.reservationId);
    if (verifiedReservation == m_exactReservations.constEnd()
        || verifiedReservation->policy != policy
        || !exactReservationContains(*verifiedReservation, operationLease)
        || m_exactEndpointReservations.value(operationLease.endpoint, 0)
            != m_exactOperation.token.reservationId
        || !exactReservationTargetIsCurrent(*verifiedReservation)) {
        retireExactInstance(operationLease);
        return true;
    }
    rememberExactValue(operationLease, name, value, type);

    if (m_exactOperation.token.kind == ExactOperationKind::Read) {
        finishExactOperation(
            ExactTerminalResult::ReadSucceeded,
            value, type,
            QStringLiteral("The exact parameter read succeeded."),
            false);
        return true;
    }
    if (m_exactOperation.token.type != type) {
        return true;
    }
    if (ParameterCodec::valuesEqual(
            m_exactOperation.token.normalizedValue, value, type)) {
        finishExactOperation(
            ExactTerminalResult::WriteSucceeded,
            value, type,
            QStringLiteral("The vehicle echoed the exact parameter value."),
            false);
    } else {
        finishExactOperation(
            ExactTerminalResult::Rejected,
            value, type,
            QStringLiteral(
                "The vehicle echoed a different value for the exact parameter."),
            false);
    }
    return true;
}

void ParameterService::rememberExactValue(
    const ExactInstanceLease &lease,
    const QString &name,
    const QVariant &value,
    ParameterType type)
{
    m_exactCachedValues[lease.endpoint].insert(
        name, ExactCachedValue{lease, name, value, type});
}

bool ParameterService::exactCacheMatches(
    const ExactInstanceLease &lease,
    const QString &name,
    const QVariant &value,
    ParameterType type) const
{
    const auto endpointCache = m_exactCachedValues.constFind(lease.endpoint);
    if (endpointCache == m_exactCachedValues.constEnd()) {
        return false;
    }
    const auto cached = endpointCache->constFind(name);
    return cached != endpointCache->constEnd()
        && cached->lease.sameInstance(lease)
        && cached->type == type
        && ParameterCodec::valuesEqual(cached->value, value, type);
}

bool ParameterService::matchesExactWriteQuarantine(
    const VehicleEndpoint &endpoint,
    const QString &name,
    int *index) const
{
    for (int candidate = 0;
         candidate < m_exactWriteQuarantines.size(); ++candidate) {
        const ExactWriteQuarantine &quarantine =
            m_exactWriteQuarantines.at(candidate);
        if (quarantine.endpoint.sameIdentity(endpoint)
            && quarantine.name == name) {
            if (index) {
                *index = candidate;
            }
            return true;
        }
    }
    return false;
}

bool ParameterService::exactEndpointQuarantined(
    const VehicleEndpoint &endpoint) const
{
    return std::any_of(
        m_exactWriteQuarantines.cbegin(),
        m_exactWriteQuarantines.cend(),
        [&endpoint](const ExactWriteQuarantine &quarantine) {
            return quarantine.endpoint.sameIdentity(endpoint);
        });
}

void ParameterService::addExactWriteQuarantine(
    const PendingExactOperation &operation)
{
    const ExactInstanceLease lease = exactInstance(operation.token);
    int existing = -1;
    const bool found = matchesExactWriteQuarantine(
        lease.endpoint,
        operation.token.name,
        &existing);
    ExactWriteQuarantine quarantine;
    quarantine.endpoint = lease.endpoint;
    quarantine.name = operation.token.name;
    quarantine.normalizedValue = operation.token.normalizedValue;
    quarantine.type = operation.token.type;
    quarantine.localSystemId = operation.localSystemId;
    quarantine.localComponentId = operation.localComponentId;
    quarantine.expiresAtMs =
        m_exactClock.elapsed() + m_exactWriteQuarantineMs;
    if (found) {
        m_exactWriteQuarantines[existing] = quarantine;
    } else {
        m_exactWriteQuarantines.append(quarantine);
    }
    scheduleExactQuarantineExpiry();
}

void ParameterService::cleanupExpiredExactQuarantines()
{
    const qint64 now = m_exactClock.elapsed();
    for (int index = m_exactWriteQuarantines.size() - 1;
         index >= 0; --index) {
        if (m_exactWriteQuarantines.at(index).expiresAtMs <= now) {
            m_exactWriteQuarantines.removeAt(index);
        }
    }
    scheduleExactQuarantineExpiry();
}

void ParameterService::scheduleExactQuarantineExpiry()
{
    if (m_exactWriteQuarantines.isEmpty()) {
        m_exactQuarantineTimer.stop();
        return;
    }
    qint64 earliest = std::numeric_limits<qint64>::max();
    for (const ExactWriteQuarantine &quarantine
         : std::as_const(m_exactWriteQuarantines)) {
        earliest = qMin(earliest, quarantine.expiresAtMs);
    }
    const qint64 remaining = qMax<qint64>(
        1, earliest - m_exactClock.elapsed());
    m_exactQuarantineTimer.start(static_cast<int>(qMin<qint64>(
        remaining, std::numeric_limits<int>::max())));
}

void ParameterService::touchLegacyTrafficFence(
    const VehicleEndpoint &endpoint)
{
    if (!endpoint.isValid()) {
        return;
    }
    const qint64 now = m_exactClock.elapsed();
    for (auto fence = m_legacyTrafficFenceExpiries.begin();
         fence != m_legacyTrafficFenceExpiries.end();) {
        if (*fence <= now) {
            fence = m_legacyTrafficFenceExpiries.erase(fence);
        } else {
            ++fence;
        }
    }
    m_legacyTrafficFenceExpiries.insert(
        endpoint, now + m_exactWriteQuarantineMs);
}

bool ParameterService::legacyTrafficFenced(
    const VehicleEndpoint &endpoint) const
{
    const auto fence = m_legacyTrafficFenceExpiries.constFind(endpoint);
    return fence != m_legacyTrafficFenceExpiries.constEnd()
        && *fence > m_exactClock.elapsed();
}

void ParameterService::handleExactOwnerDestroyed(quint64 reservationId)
{
    auto reservation = m_exactReservations.find(reservationId);
    if (reservation == m_exactReservations.end()) {
        return;
    }
    reservation->owner.clear();
    reservation->closing = true;
    maybeReleaseExactReservation(reservationId);
}

bool ParameterService::exactReservationHasPending(
    quint64 reservationId) const
{
    return m_exactOperationActive
        && m_exactOperation.token.reservationId == reservationId;
}

void ParameterService::maybeReleaseExactReservation(quint64 reservationId)
{
    const auto reservation = m_exactReservations.constFind(reservationId);
    if (reservation == m_exactReservations.constEnd()
        || !reservation->closing
        || exactReservationHasPending(reservationId)) {
        return;
    }
    removeExactReservation(reservationId);
}

void ParameterService::removeExactReservation(quint64 reservationId)
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

quint64 ParameterService::nextExactReservationId()
{
    do {
        ++m_nextExactReservationId;
    } while (m_nextExactReservationId == 0
             || m_exactReservations.contains(m_nextExactReservationId));
    return m_nextExactReservationId;
}

quint64 ParameterService::nextExactOperationId()
{
    do {
        ++m_nextExactOperationId;
    } while (m_nextExactOperationId == 0
             || (m_exactOperationActive
                 && m_exactOperation.token.operationId
                    == m_nextExactOperationId));
    return m_nextExactOperationId;
}

ParameterService::ExactInstanceLease ParameterService::exactInstance(
    const SwarmVehicleInstanceLease &lease)
{
    return ExactInstanceLease{
        lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch,
        ExactLeaseDomain::Swarm};
}

ParameterService::ExactInstanceLease ParameterService::exactInstance(
    const MavlinkComponentInstanceLease &lease)
{
    return ExactInstanceLease{
        lease.endpoint, lease.linkSessionEpoch, lease.instanceEpoch,
        ExactLeaseDomain::Component};
}

ParameterService::ExactInstanceLease ParameterService::exactInstance(
    const ExactOperationToken &token)
{
    if (token.componentLease.isValid() && !token.lease.isValid()) {
        return exactInstance(token.componentLease);
    }
    return exactInstance(token.lease);
}

bool ParameterService::reservationTokenMatches(
    const ExactReservationToken &token,
    const ExactReservationRecord &record)
{
    if (record.policy == ExactReservationPolicy::Component) {
        return token.leases.isEmpty()
            && token.componentLeases.size() == 1
            && record.leases.size() == 1
            && record.leases.first().sameInstance(
                exactInstance(token.componentLeases.first()));
    }
    if (!token.componentLeases.isEmpty()
        || token.leases.size() != record.leases.size()) {
        return false;
    }
    for (int index = 0; index < token.leases.size(); ++index) {
        if (!record.leases.at(index).sameInstance(
                exactInstance(token.leases.at(index)))) {
            return false;
        }
    }
    return true;
}

bool ParameterService::exactLeaseIsCurrent(
    const ExactInstanceLease &lease) const
{
    if (!lease.isValid()) {
        return false;
    }
    if (lease.domain == ExactLeaseDomain::Component) {
        const ComponentLeaseValidator validator =
            m_componentLeaseValidator;
        MavlinkComponentInstanceLease publicLease;
        publicLease.endpoint = lease.endpoint;
        publicLease.linkSessionEpoch = lease.linkSessionEpoch;
        publicLease.instanceEpoch = lease.instanceEpoch;
        return validator && validator(publicLease);
    }
    const ExactLeaseValidator validator = m_exactLeaseValidator;
    SwarmVehicleInstanceLease publicLease;
    publicLease.endpoint = lease.endpoint;
    publicLease.linkSessionEpoch = lease.linkSessionEpoch;
    publicLease.instanceEpoch = lease.instanceEpoch;
    return validator && validator(publicLease);
}

bool ParameterService::exactRouteIsEligible(
    const ExactInstanceLease &lease,
    ExactReservationPolicy policy,
    QString *error) const
{
    if (error) {
        error->clear();
    }
    if (!lease.isValid()) {
        return false;
    }
    if (policy == ExactReservationPolicy::Component) {
        if (lease.domain != ExactLeaseDomain::Component) {
            return false;
        }
        const ComponentRouteValidator validator =
            m_componentRouteValidator;
        MavlinkComponentInstanceLease publicLease;
        publicLease.endpoint = lease.endpoint;
        publicLease.linkSessionEpoch = lease.linkSessionEpoch;
        publicLease.instanceEpoch = lease.instanceEpoch;
        return validator && validator(publicLease, error);
    }
    if (lease.domain != ExactLeaseDomain::Swarm) {
        return false;
    }
    const ExactRouteValidator validator =
        policy == ExactReservationPolicy::SingleVehicle
        ? m_singleVehicleExactRouteValidator : m_exactRouteValidator;
    SwarmVehicleInstanceLease publicLease;
    publicLease.endpoint = lease.endpoint;
    publicLease.linkSessionEpoch = lease.linkSessionEpoch;
    publicLease.instanceEpoch = lease.instanceEpoch;
    return validator && validator(publicLease, error);
}

bool ParameterService::exactReservationTargetIsCurrent(
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

bool ParameterService::exactReservationContains(
    const ExactReservationRecord &reservation,
    const ExactInstanceLease &lease) const
{
    return std::any_of(
        reservation.leases.cbegin(), reservation.leases.cend(),
        [&lease](const ExactInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        });
}

bool ParameterService::legacyOperationTouches(
    const VehicleEndpoint &endpoint) const
{
    if ((m_listActive
         && m_listTarget.endpoint.sameIdentity(endpoint))
        || (m_queuedList.active
            && m_queuedList.target.endpoint.sameIdentity(endpoint))
        || (m_writeActive
            && m_activeWrite.target.endpoint.sameIdentity(endpoint))
        || legacyTrafficFenced(endpoint)) {
        return true;
    }
    for (const PendingRead &read : m_pendingReads) {
        if (read.target.endpoint.sameIdentity(endpoint)) {
            return true;
        }
    }
    for (const PendingWrite &write : m_writeQueue) {
        if (write.target.endpoint.sameIdentity(endpoint)) {
            return true;
        }
    }
    return false;
}

bool ParameterService::exactEndpointReserved(
    const VehicleEndpoint &endpoint) const
{
    return m_exactEndpointReservations.contains(endpoint);
}

bool ParameterService::targetIsCurrent(
    const VehicleTargetLease &target) const
{
    return m_targetManager && target.isValid()
        && m_targetManager->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation);
}

bool ParameterService::parameterNameBytes(
    const QString &name, QByteArray *bytes)
{
    if (!bytes || name.isEmpty() || name.contains(QChar::Null)) {
        return false;
    }
    const QByteArray latin1 = name.toLatin1();
    if (latin1.isEmpty()
        || latin1.size() > MAVLINK_MSG_PARAM_SET_FIELD_PARAM_ID_LEN
        || QString::fromLatin1(latin1) != name) {
        return false;
    }
    *bytes = latin1;
    return true;
}

ParameterType ParameterService::parameterType(quint8 mavlinkType)
{
    if (mavlinkType < static_cast<quint8>(ParameterType::UInt8)
        || mavlinkType > static_cast<quint8>(ParameterType::Real64)) {
        return ParameterType::Unknown;
    }
    return static_cast<ParameterType>(mavlinkType);
}

bool ParameterService::sameEnvelope(
    const VehicleTargetLease &target, int linkId,
    const mavlink_message_t &message)
{
    return target.isValid()
        && target.endpoint.linkId == linkId
        && target.endpoint.systemId == message.sysid
        && target.endpoint.componentId == message.compid;
}

VehicleEndpoint ParameterService::endpointFor(
    int linkId, const mavlink_message_t &message) const
{
    const QList<VehicleEndpoint> endpoints = m_targetManager->endpoints();
    for (const VehicleEndpoint &endpoint : endpoints) {
        if (endpoint.linkId == linkId
            && endpoint.systemId == message.sysid
            && endpoint.componentId == message.compid) {
            return endpoint;
        }
    }
    VehicleEndpoint endpoint;
    endpoint.linkId = linkId;
    endpoint.systemId = message.sysid;
    endpoint.componentId = message.compid;
    return endpoint;
}

ParameterEncoding ParameterService::encodingFor(
    const VehicleEndpoint &endpoint) const
{
    return m_encodings.value(endpoint, ParameterEncoding::Bytewise);
}

ParameterService::SendResult ParameterService::send(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    mavlink_message_t message)
{
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (!m_transmitter) {
        return SendResult::TransportUnavailable;
    }
    auto exactCache = m_exactCachedValues.find(target.endpoint);
    if (exactCache != m_exactCachedValues.end()) {
        if (message.msgid == MAVLINK_MSG_ID_PARAM_SET) {
            mavlink_param_set_t payload{};
            mavlink_msg_param_set_decode(&message, &payload);
            exactCache->remove(parameterName(
                payload.param_id, MAVLINK_MSG_PARAM_SET_FIELD_PARAM_ID_LEN));
        } else if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
            mavlink_param_request_read_t payload{};
            mavlink_msg_param_request_read_decode(&message, &payload);
            if (payload.param_index < 0) {
                exactCache->remove(parameterName(
                    payload.param_id,
                    MAVLINK_MSG_PARAM_REQUEST_READ_FIELD_PARAM_ID_LEN));
            } else {
                m_exactCachedValues.erase(exactCache);
                exactCache = m_exactCachedValues.end();
            }
        } else if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_LIST) {
            m_exactCachedValues.erase(exactCache);
            exactCache = m_exactCachedValues.end();
        }
        if (exactCache != m_exactCachedValues.end()
            && exactCache->isEmpty()) {
            m_exactCachedValues.erase(exactCache);
        }
    }
    // Classic PARAM messages carry no transaction identifier.  Fence an exact
    // reservation before calling the transport because it may reply
    // synchronously, and retain the fence after cancellation/transport doubt.
    touchLegacyTrafficFence(target.endpoint);
    return m_transmitter->sendMessage(
               target.endpoint.linkId, localSystemId, localComponentId,
               message)
            == ExactLinkTransmitter::SendResult::Sent
        ? SendResult::Sent : SendResult::TransportUnavailable;
}

ParameterService::SendResult ParameterService::transmitParameterListRequest()
{
    if (!m_listActive || !m_listTarget.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(m_listTarget)) {
        return SendResult::StaleTarget;
    }

    mavlink_param_request_list_t payload{};
    payload.target_system =
        static_cast<quint8>(m_listTarget.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(m_listTarget.endpoint.componentId);
    mavlink_message_t message{};
    mavlink_msg_param_request_list_encode(
        m_listLocalSystemId, m_listLocalComponentId, &message, &payload);
    return send(m_listTarget,
                m_listLocalSystemId, m_listLocalComponentId, message);
}

ParameterService::SendResult ParameterService::transmitParameterIndexRequest(
    int index)
{
    if (!m_listActive || !m_listTarget.isValid()) {
        return SendResult::InvalidTarget;
    }
    return requestParameterReadByIndex(
        m_listTarget, m_listLocalSystemId, m_listLocalComponentId, index);
}

void ParameterService::completeParameterList(const VehicleEndpoint &source)
{
    if (!m_listActive) {
        return;
    }
    const VehicleTargetLease completed = m_listTarget;
    clearParameterListState();
    emit listCompleted(
        completed.generation,
        source.linkId, source.systemId, source.componentId);
    pumpTransactions();
}

void ParameterService::failParameterList(const QString &reason)
{
    if (!m_listActive) {
        return;
    }
    const VehicleTargetLease failed = m_listTarget;
    m_store->failLoad(failed.endpoint);
    clearParameterListState();
    emit listFailed(
        failed.generation,
        failed.endpoint.linkId,
        failed.endpoint.systemId,
        failed.endpoint.componentId,
        reason);
    pumpTransactions();
}

void ParameterService::cancelParameterList(bool notify)
{
    if (!m_listActive) {
        return;
    }
    const VehicleTargetLease cancelled = m_listTarget;
    m_store->cancelLoad(cancelled.endpoint);
    clearParameterListState();
    if (notify) {
        emit listCancelled(
            cancelled.generation,
            cancelled.endpoint.linkId,
            cancelled.endpoint.systemId,
            cancelled.endpoint.componentId);
    }
    pumpTransactions();
}

void ParameterService::clearParameterListState()
{
    m_listRetryTimer.stop();
    m_listActive = false;
    m_listTarget = {};
    m_listReportedCount = 0;
    m_listReceivedIndices.clear();
    m_listIndexAttempts.clear();
    m_listNextMissingIndex = 0;
    m_listWholeRetries = 0;
}

void ParameterService::restartListTimer(int intervalMs)
{
    if (m_listActive) {
        m_listRetryTimer.start(qMax(0, intervalMs));
    }
}

void ParameterService::handleListRetryTimeout()
{
    if (!m_listActive) {
        return;
    }
    if (!targetIsCurrent(m_listTarget)) {
        const quint64 generation = m_listTarget.generation;
        cancelParameterList(true);
        emit transactionsCancelled(generation);
        return;
    }

    const int received = m_listReceivedIndices.size();
    const bool belowSeventyFivePercent = m_listReportedCount <= 0
        || received * 4 < m_listReportedCount * 3;
    if (belowSeventyFivePercent
        && m_listWholeRetries < m_listMaximumWholeRetries) {
        ++m_listWholeRetries;
        const SendResult result = transmitParameterListRequest();
        if (result != SendResult::Sent) {
            failParameterList(QStringLiteral(
                "Parameter list retry could not use the selected link."));
            return;
        }
        restartListTimer(m_listInactivityMs);
        return;
    }

    if (m_listReportedCount <= 0) {
        failParameterList(QStringLiteral(
            "The vehicle did not report a parameter count."));
        return;
    }

    constexpr int MaximumBurstSize = 10;
    int queued = 0;
    int scanned = 0;
    int index = qBound(0, m_listNextMissingIndex,
                       m_listReportedCount - 1);
    while (scanned < m_listReportedCount && queued < MaximumBurstSize) {
        if (!m_listReceivedIndices.contains(index)
            && m_listIndexAttempts.value(index, 0)
                   < m_listMaximumIndexAttempts) {
            const SendResult result = transmitParameterIndexRequest(index);
            if (result != SendResult::Sent) {
                failParameterList(QStringLiteral(
                    "A missing parameter could not be requested on the selected link."));
                return;
            }
            m_listIndexAttempts[index] =
                m_listIndexAttempts.value(index, 0) + 1;
            ++queued;
        }
        index = (index + 1) % m_listReportedCount;
        ++scanned;
    }
    m_listNextMissingIndex = index;

    if (queued == 0) {
        failParameterList(QStringLiteral(
            "Missing parameters did not answer bounded index retries."));
        return;
    }
    restartListTimer(m_listMissingBurstIntervalMs);
}

void ParameterService::handleTargetGenerationChanged(
    qulonglong generation)
{
    // A nested selection can complete while Qt is still delivering an older
    // signal. Its generation argument distinguishes the stale delivery from
    // the final target state.
    if (generation != m_targetManager->targetGeneration()
        || generation == m_lastHandledTargetGeneration) {
        return;
    }
    m_lastHandledTargetGeneration = generation;
    QList<ExactInstanceLease> staleSingleVehicleLeases;
    for (auto reservation = m_exactReservations.begin();
         reservation != m_exactReservations.end(); ++reservation) {
        if (reservation->policy == ExactReservationPolicy::SingleVehicle
            && reservation->target.generation != generation) {
            reservation->closing = true;
            for (const ExactInstanceLease &lease :
                 reservation->leases) {
                if (!staleSingleVehicleLeases.contains(lease)) {
                    staleSingleVehicleLeases.append(lease);
                }
            }
        }
    }
    QPointer<ParameterService> guard(this);
    for (const ExactInstanceLease &lease :
         staleSingleVehicleLeases) {
        retireExactInstance(lease);
        if (guard.isNull()) {
            return;
        }
    }
    cancelTransactions(generation);
    if (guard.isNull()) {
        return;
    }
    syncSelectedEndpoint();
}

void ParameterService::handleTargetGenerationSettled(
    qulonglong generation)
{
    if (generation != m_targetManager->targetGeneration()
        || generation != m_lastHandledTargetGeneration) {
        return;
    }
    m_cancellingTransactions = false;
    pumpTransactions();
}

void ParameterService::syncSelectedEndpoint()
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    m_store->selectEndpoint(target.isValid()
                                ? target.endpoint : VehicleEndpoint{});
}

void ParameterService::cancelTransactions(quint64 currentGeneration)
{
    QSet<quint64> generations;
    if (m_listActive
        && m_listTarget.generation != currentGeneration) {
        generations.insert(m_listTarget.generation);
    }
    if (m_queuedList.active
        && m_queuedList.target.generation != currentGeneration) {
        generations.insert(m_queuedList.target.generation);
    }
    for (const PendingRead &read : m_pendingReads) {
        if (read.target.generation != currentGeneration) {
            generations.insert(read.target.generation);
        }
    }
    if (m_writeActive
        && m_activeWrite.target.generation != currentGeneration) {
        generations.insert(m_activeWrite.target.generation);
    }
    for (const PendingWrite &write : m_writeQueue) {
        if (write.target.generation != currentGeneration) {
            generations.insert(write.target.generation);
        }
    }

    // Capture the generations before emitting any cancellation signal. A
    // handler is allowed to enqueue work for the already-selected new target;
    // that new work must not be swept up by this old-generation cleanup.
    m_cancellingTransactions = true;
    if (m_queuedList.active
        && m_queuedList.target.generation != currentGeneration) {
        cancelQueuedParameterList(true);
    }
    if (m_listActive
        && m_listTarget.generation != currentGeneration) {
        cancelParameterList(true);
    }
    if (m_writeActive
        && m_activeWrite.target.generation != currentGeneration) {
        cancelActiveWrite();
    }
    for (auto it = m_pendingReads.begin(); it != m_pendingReads.end();) {
        if (it->target.generation != currentGeneration) {
            it = m_pendingReads.erase(it);
        } else {
            ++it;
        }
    }
    for (quint64 generation : generations) {
        cancelQueuedWritesForGeneration(generation);
    }
    for (quint64 generation : generations) {
        emit transactionsCancelled(generation);
    }
    // VehicleTargetManager's settled phase releases the queue only after all
    // generation/current-target listeners have invalidated their old state.
}
