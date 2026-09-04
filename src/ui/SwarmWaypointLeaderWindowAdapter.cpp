#include "SwarmWaypointLeaderWindowAdapter.h"

#include "comm/SwarmFlightMode.h"

#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace
{

bool sameVehicle(const SwarmVehicleInstanceLease &left,
                 const SwarmVehicleInstanceLease &right) noexcept
{
    return left.isValid() && right.isValid() && left.sameInstance(right);
}

bool commandCapable(const SwarmTelemetrySnapshot &snapshot) noexcept
{
    return snapshot.lease.isValid()
        && snapshot.lease.endpoint.componentId == MAV_COMP_ID_AUTOPILOT1
        && snapshot.heartbeatValid
        && snapshot.autopilot != MAV_AUTOPILOT_INVALID
        && snapshot.vehicleType != MAV_TYPE_GCS;
}

bool flightCapable(const SwarmTelemetrySnapshot &snapshot) noexcept
{
    return commandCapable(snapshot)
        && snapshot.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA
        && SwarmFlightMode::isCopter(snapshot.vehicleType);
}

QString vehicleLabel(const SwarmVehicleInstanceLease &lease)
{
    const QString link = lease.endpoint.linkName.trimmed().isEmpty()
        ? QObject::tr("Link %1").arg(lease.endpoint.linkId)
        : lease.endpoint.linkName.trimmed();
    return QStringLiteral("%1 — %2:%3")
        .arg(link)
        .arg(lease.endpoint.systemId)
        .arg(lease.endpoint.componentId);
}

QString firmwareLabel(const SwarmTelemetrySnapshot &snapshot)
{
    if (snapshot.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA) {
        if (SwarmFlightMode::isCopter(snapshot.vehicleType)) {
            return QStringLiteral("ArduCopter");
        }
        if (SwarmFlightMode::isPlane(snapshot.vehicleType)) {
            return QStringLiteral("ArduPlane");
        }
        if (SwarmFlightMode::isRover(snapshot.vehicleType)) {
            return QStringLiteral("ArduRover");
        }
        return QStringLiteral("ArduPilot");
    }
    if (snapshot.autopilot == MAV_AUTOPILOT_PX4) {
        return QStringLiteral("PX4");
    }
    if (snapshot.autopilot == MAV_AUTOPILOT_INVALID) {
        return QObject::tr("Unsupported");
    }
    return QObject::tr("Autopilot %1").arg(snapshot.autopilot);
}

QString liveStatus(const SwarmTelemetrySnapshot &snapshot)
{
    if (!snapshot.heartbeatValid) {
        return QObject::tr("Heartbeat unavailable");
    }
    return QStringLiteral("%1; %2; %3")
        .arg(SwarmFlightMode::displayName(snapshot))
        .arg(snapshot.armed ? QObject::tr("armed")
                            : QObject::tr("disarmed"))
        .arg(snapshot.positionValid ? QObject::tr("position live")
                                    : QObject::tr("position unavailable"));
}

bool lessSnapshot(const SwarmTelemetrySnapshot &left,
                  const SwarmTelemetrySnapshot &right)
{
    if (left.lease.endpoint != right.lease.endpoint) {
        return left.lease.endpoint < right.lease.endpoint;
    }
    if (left.lease.linkSessionEpoch != right.lease.linkSessionEpoch) {
        return left.lease.linkSessionEpoch < right.lease.linkSessionEpoch;
    }
    return left.lease.instanceEpoch < right.lease.instanceEpoch;
}

bool convertMission(const ExactMissionSnapshot &source,
                    const SwarmVehicleInstanceLease &expectedAir,
                    SwarmWaypointLeaderMissionSnapshot *destination,
                    QString *error)
{
    if (destination) {
        *destination = SwarmWaypointLeaderMissionSnapshot();
    }
    if (error) {
        error->clear();
    }
    if (!destination) {
        if (error) {
            *error = QObject::tr("No Waypoint Leader mission destination was provided.");
        }
        return false;
    }
    if (!source.isValid()
        || !sameVehicle(source.key.vehicle, expectedAir)
        || source.key.missionType != MAV_MISSION_TYPE_MISSION) {
        if (error) {
            *error = QObject::tr(
                "No complete exact mission observation belongs to this air master.");
        }
        return false;
    }

    SwarmWaypointLeaderMissionSnapshot converted;
    converted.airMaster = source.key.vehicle;
    converted.missionType = source.key.missionType;
    converted.contentGeneration = source.contentGeneration;
    converted.contentDigest = source.contentDigest;
    converted.items.reserve(source.items.size());
    for (const mavlink_mission_item_int_t &item : source.items) {
        SwarmWaypointLeaderMissionItem convertedItem;
        convertedItem.sequence = item.seq;
        convertedItem.command = item.command;
        convertedItem.frame = item.frame;
        convertedItem.latitudeE7 = item.x;
        convertedItem.longitudeE7 = item.y;
        convertedItem.relativeAltitudeM = item.z;
        converted.items.append(convertedItem);
    }

    SwarmWaypointLeaderMissionPath path;
    QString pathError;
    if (!SwarmWaypointLeaderMissionPath::build(
            converted, &path, &pathError)) {
        if (error) {
            *error = pathError.trimmed().isEmpty()
                ? QObject::tr(
                    "The exact mission has no usable Waypoint Leader path.")
                : pathError;
        }
        return false;
    }

    *destination = std::move(converted);
    return true;
}

QString missionStartError(
    ExactMissionSnapshotService::StartResult result)
{
    using Result = ExactMissionSnapshotService::StartResult;
    switch (result) {
    case Result::Busy:
        return QObject::tr(
            "Another exact mission transaction already owns the mission protocol.");
    case Result::InvalidOwner:
        return QObject::tr("The mission refresh owner is unavailable.");
    case Result::InvalidLease:
        return QObject::tr("The exact air-master instance is stale.");
    case Result::UnsupportedMissionType:
        return QObject::tr("The requested mission type is unsupported.");
    case Result::IncompatibleProtocolVersion:
        return QObject::tr("The exact link protocol version is incompatible.");
    case Result::MissionCoordinatorUnavailable:
        return QObject::tr(
            "The air master has no mission-protocol coordinator.");
    case Result::MissionOwnerBusy:
        return QObject::tr(
            "Another operation owns this vehicle's mission protocol.");
    case Result::UnsafeRoute:
        return QObject::tr("The exact mission route is not command-safe.");
    case Result::TransportUnavailable:
        return QObject::tr("The exact mission transport is unavailable.");
    case Result::IdentifierExhausted:
        return QObject::tr("Mission transfer identifiers are exhausted.");
    case Result::Started:
        break;
    }
    return QObject::tr("The exact mission refresh could not be started.");
}

class ApplicationBackend final
    : public SwarmWaypointLeaderWindowAdapterBackend
{
public:
    ApplicationBackend(SwarmTelemetryRegistry *registry,
                       ExactMissionSnapshotService *missions,
                       SwarmWaypointLeaderExecutor *executor)
        : m_registry(registry)
        , m_missions(missions)
        , m_executor(executor)
    {
        if (m_registry) {
            connect(m_registry.data(),
                    &SwarmTelemetryRegistry::registryChanged,
                    this, [this](qulonglong) {
                if (m_callbacks.vehiclesChanged) {
                    m_callbacks.vehiclesChanged();
                }
            });
            connect(m_registry.data(), &QObject::destroyed,
                    this, [this]() {
                m_registry = nullptr;
                if (m_callbacks.availabilityChanged) {
                    m_callbacks.availabilityChanged();
                }
            });
        }
        if (m_missions) {
            connect(m_missions.data(),
                    &ExactMissionSnapshotService::snapshotReplaced,
                    this, [this](const ExactMissionSnapshot &) {
                if (m_callbacks.missionCacheChanged) {
                    m_callbacks.missionCacheChanged();
                }
            });
            connect(m_missions.data(),
                    &ExactMissionSnapshotService::snapshotInvalidated,
                    this, [this](const ExactMissionKey &,
                                 ExactMissionSnapshotService::InvalidationReason) {
                if (m_callbacks.missionCacheChanged) {
                    m_callbacks.missionCacheChanged();
                }
            });
            connect(m_missions.data(),
                    &ExactMissionSnapshotService::transferFinished,
                    this, [this](const ExactMissionTransferResult &result) {
                if (m_callbacks.missionFinished) {
                    m_callbacks.missionFinished(result);
                }
            });
            connect(m_missions.data(), &QObject::destroyed,
                    this, [this]() {
                m_missions = nullptr;
                if (m_callbacks.availabilityChanged) {
                    m_callbacks.availabilityChanged();
                }
            });
        }
        if (m_executor) {
            connect(m_executor.data(), &SwarmWaypointLeaderExecutor::changed,
                    this, [this]() {
                if (m_callbacks.executorChanged) {
                    m_callbacks.executorChanged();
                }
            });
            connect(m_executor.data(), &QObject::destroyed,
                    this, [this]() {
                m_executor = nullptr;
                if (m_callbacks.availabilityChanged) {
                    m_callbacks.availabilityChanged();
                }
            });
        }
    }

    void setCallbacks(Callbacks callbacks) override
    {
        m_callbacks = std::move(callbacks);
    }

    QVector<SwarmTelemetrySnapshot> vehicleSnapshots() const override
    {
        QVector<SwarmTelemetrySnapshot> result;
        if (!m_registry) {
            return result;
        }
        const QList<VehicleEndpoint> endpoints = m_registry->endpoints();
        result.reserve(endpoints.size());
        for (const VehicleEndpoint &endpoint : endpoints) {
            SwarmTelemetrySnapshot snapshot;
            if (m_registry->acquireSnapshot(endpoint, &snapshot)) {
                result.append(snapshot);
            }
        }
        return result;
    }

    void refreshVehicles() override
    {
        if (m_registry) {
            m_registry->retireStaleEndpoints();
        }
    }

    bool acquireMission(
        const SwarmVehicleInstanceLease &airMaster,
        ExactMissionSnapshot *snapshot) const override
    {
        return m_missions && snapshot
            && m_missions->acquireSnapshot(
                airMaster, MAV_MISSION_TYPE_MISSION, snapshot);
    }

    ExactMissionSnapshotService::StartResult requestMission(
        QObject *owner,
        const SwarmVehicleInstanceLease &airMaster,
        ExactMissionTransferToken *token,
        QString *error) override
    {
        if (!m_missions) {
            if (token) {
                *token = ExactMissionTransferToken();
            }
            if (error) {
                *error = QObject::tr(
                    "The exact mission service is unavailable.");
            }
            return ExactMissionSnapshotService::StartResult::
                TransportUnavailable;
        }
        return m_missions->requestDownload(
            owner, airMaster, MAV_MISSION_TYPE_MISSION, token, error);
    }

    bool cancelMission(const ExactMissionTransferToken &token,
                       const QString &reason) override
    {
        return m_missions && m_missions->cancel(token, reason);
    }

    bool executorReady(QString *error) const override
    {
        if (m_executor) {
            return m_executor->executorReady(error);
        }
        if (error) {
            *error = QObject::tr(
                "The application Waypoint Leader executor is unavailable.");
        }
        return false;
    }

    bool validatePlan(const SwarmWaypointLeaderPlan &plan,
                      QString *error) const override
    {
        if (m_executor) {
            return m_executor->validatePlan(plan, error);
        }
        if (error) {
            *error = QObject::tr(
                "The application Waypoint Leader executor is unavailable.");
        }
        return false;
    }

    bool start(const SwarmWaypointLeaderPlan &plan,
               QString *error) override
    {
        if (m_executor) {
            return m_executor->start(plan, error);
        }
        if (error) {
            *error = QObject::tr(
                "The application Waypoint Leader executor is unavailable.");
        }
        return false;
    }

    void cancelActiveRun(const QString &reason) override
    {
        if (m_executor) {
            m_executor->cancelActiveRun(reason);
        }
    }

    bool requestMode(SwarmWaypointLeaderMode mode,
                     QString *error) override
    {
        if (m_executor) {
            return m_executor->requestMode(mode, error);
        }
        if (error) {
            *error = QObject::tr(
                "The application Waypoint Leader executor is unavailable.");
        }
        return false;
    }

    bool isRunning() const noexcept override
    {
        return m_executor && m_executor->isRunning();
    }

    SwarmWaypointLeaderMode mode() const noexcept override
    {
        return m_executor ? m_executor->mode()
                          : SwarmWaypointLeaderMode::Idle;
    }

    QString statusText() const override
    {
        return m_executor
            ? m_executor->statusText()
            : QObject::tr("Waypoint Leader executor is unavailable.");
    }

private:
    QPointer<SwarmTelemetryRegistry> m_registry;
    QPointer<ExactMissionSnapshotService> m_missions;
    QPointer<SwarmWaypointLeaderExecutor> m_executor;
    Callbacks m_callbacks;
};

} // namespace

class SwarmWaypointLeaderWindowAdapter::Implementation
{
public:
    struct MissionRequest
    {
        SwarmVehicleInstanceLease airMaster;
        ExactMissionTransferToken token;

        bool isActive() const noexcept
        {
            return airMaster.isValid() && token.isValid();
        }
    };

    Implementation(SwarmWaypointLeaderWindowAdapter *adapter,
                   SwarmWaypointLeaderWindowAdapterBackend *nextBackend,
                   std::unique_ptr<SwarmWaypointLeaderWindowAdapterBackend>
                       owned)
        : q(adapter)
        , backend(nextBackend)
        , ownedBackend(std::move(owned))
    {
        if (!backend) {
            return;
        }
        backendDestroyed = QObject::connect(
            backend.data(), &QObject::destroyed, q, [this]() {
                backend = nullptr;
                request = MissionRequest();
                requestStarting = false;
                provisionalResults.clear();
                profileMission = SwarmWaypointLeaderMissionSnapshot();
                profilePath = SwarmWaypointLeaderMissionPath();
                scheduleChanged();
            });
        SwarmWaypointLeaderWindowAdapterBackend::Callbacks callbacks;
        callbacks.vehiclesChanged = [this]() { scheduleChanged(); };
        callbacks.missionCacheChanged = [this]() {
            refreshProfileCache();
            scheduleChanged();
        };
        callbacks.missionFinished =
            [this](ExactMissionTransferResult result) {
                missionFinished(std::move(result));
            };
        callbacks.executorChanged = [this]() {
            missionStatus.clear();
            scheduleChanged();
        };
        callbacks.availabilityChanged = [this]() {
            if (request.isActive() && backend) {
                const MissionRequest interrupted = request;
                request = MissionRequest();
                backend->cancelMission(
                    interrupted.token,
                    QStringLiteral(
                        "A Waypoint Leader application dependency disappeared."));
            }
            refreshProfileCache();
            scheduleChanged();
        };
        backend->setCallbacks(std::move(callbacks));
    }

    ~Implementation()
    {
        changedHandler = {};
        changedScheduled = false;
        if (backend) {
            backend->setCallbacks({});
            const MissionRequest active = request;
            request = MissionRequest();
            if (active.isActive()) {
                backend->cancelMission(
                    active.token,
                    QStringLiteral(
                        "Waypoint Leader mission adapter was destroyed."));
            }
        }
        QObject::disconnect(backendDestroyed);
        ownedBackend.reset();
        backend = nullptr;
    }

    void scheduleChanged()
    {
        if (changedScheduled) {
            return;
        }
        changedScheduled = true;
        QTimer::singleShot(0, q, [this]() {
            changedScheduled = false;
            const ChangedHandler callback = changedHandler;
            if (callback) {
                callback();
            }
        });
    }

    QVector<SwarmWaypointLeaderWindowVehicle> vehicles() const
    {
        QVector<SwarmWaypointLeaderWindowVehicle> result;
        if (!backend) {
            return result;
        }
        QVector<SwarmTelemetrySnapshot> snapshots =
            backend->vehicleSnapshots();
        std::sort(snapshots.begin(), snapshots.end(), lessSnapshot);
        result.reserve(std::min(
            snapshots.size(), SwarmTelemetryRegistry::MaximumVehicleEndpoints));
        for (const SwarmTelemetrySnapshot &snapshot : snapshots) {
            if (!snapshot.lease.isValid()
                || result.size()
                    >= SwarmTelemetryRegistry::MaximumVehicleEndpoints) {
                continue;
            }
            const bool duplicate = std::any_of(
                result.cbegin(), result.cend(),
                [&snapshot](const SwarmWaypointLeaderWindowVehicle &existing) {
                    return existing.lease.sameInstance(snapshot.lease);
                });
            if (duplicate) {
                continue;
            }

            SwarmWaypointLeaderWindowVehicle vehicle;
            vehicle.lease = snapshot.lease;
            vehicle.label = vehicleLabel(snapshot.lease);
            vehicle.firmware = firmwareLabel(snapshot);
            vehicle.liveStatus = liveStatus(snapshot);
            vehicle.commandedTarget = QStringLiteral("—");
            vehicle.groundEligible = commandCapable(snapshot);
            vehicle.flightEligible = flightCapable(snapshot);
            vehicle.relativeAltitudeM = snapshot.relativeAltitudeM;

            if (profilePath.isValid() && snapshot.positionValid) {
                const SwarmFollowPathPoint point{
                    snapshot.latitudeDegrees,
                    snapshot.longitudeDegrees,
                    snapshot.relativeAltitudeM};
                double alongM = 0.0;
                double offPathM = 0.0;
                if (profilePath.closest(point, &alongM, &offPathM)) {
                    vehicle.profilePositionValid = true;
                    vehicle.pathDistanceM = alongM;
                    vehicle.missionPosition = QObject::tr("%1 m; off %2 m")
                        .arg(alongM, 0, 'f', 1)
                        .arg(offPathM, 0, 'f', 1);
                }
            }
            if (vehicle.missionPosition.isEmpty()) {
                vehicle.missionPosition = QStringLiteral("—");
            }
            result.append(std::move(vehicle));
        }
        return result;
    }

    bool snapshotFor(const SwarmVehicleInstanceLease &airMaster,
                     ExactMissionSnapshot *snapshot) const
    {
        if (snapshot) {
            *snapshot = ExactMissionSnapshot();
        }
        if (!backend || !snapshot || !airMaster.isValid()
            || !backend->acquireMission(airMaster, snapshot)) {
            return false;
        }
        return snapshot->isValid()
            && snapshot->key.missionType == MAV_MISSION_TYPE_MISSION
            && sameVehicle(snapshot->key.vehicle, airMaster);
    }

    bool missionForAirMaster(
        const SwarmVehicleInstanceLease &airMaster,
        SwarmWaypointLeaderMissionSnapshot *mission,
        QString *error)
    {
        ExactMissionSnapshot exact;
        if (!snapshotFor(airMaster, &exact)) {
            if (mission) {
                *mission = SwarmWaypointLeaderMissionSnapshot();
            }
            if (error) {
                *error = QObject::tr(
                    "No successful exact mission observation is available for this air master.");
            }
            return false;
        }
        SwarmWaypointLeaderMissionSnapshot converted;
        if (!convertMission(exact, airMaster, &converted, error)) {
            if (mission) {
                *mission = SwarmWaypointLeaderMissionSnapshot();
            }
            return false;
        }
        if (!sameVehicle(profileMission.airMaster, converted.airMaster)
            || profileMission.contentGeneration
                != converted.contentGeneration
            || profileMission.contentDigest != converted.contentDigest) {
            SwarmWaypointLeaderMissionPath path;
            QString ignored;
            if (SwarmWaypointLeaderMissionPath::build(
                    converted, &path, &ignored)) {
                profileMission = converted;
                profilePath = std::move(path);
                scheduleChanged();
            }
        }
        if (mission) {
            *mission = std::move(converted);
        }
        return true;
    }

    quint64 missionObservationRevision(
        const SwarmVehicleInstanceLease &airMaster) const noexcept
    {
        ExactMissionSnapshot snapshot;
        return snapshotFor(airMaster, &snapshot)
            ? snapshot.observationRevision : 0;
    }

    bool airMasterEligible(
        const SwarmVehicleInstanceLease &airMaster) const
    {
        if (!backend || !airMaster.isValid()) {
            return false;
        }
        const QVector<SwarmTelemetrySnapshot> snapshots =
            backend->vehicleSnapshots();
        return std::any_of(
            snapshots.cbegin(), snapshots.cend(),
            [&airMaster](const SwarmTelemetrySnapshot &snapshot) {
                return snapshot.lease.sameInstance(airMaster)
                    && flightCapable(snapshot);
            });
    }

    bool refreshMission(const SwarmVehicleInstanceLease &airMaster,
                        QString *error)
    {
        if (error) {
            error->clear();
        }
        if (!backend) {
            if (error) {
                *error = QObject::tr(
                    "The exact mission service is unavailable.");
            }
            return false;
        }
        if (!airMasterEligible(airMaster)) {
            if (error) {
                *error = QObject::tr(
                    "The selected air master is not a live exact ArduCopter instance.");
            }
            return false;
        }
        if (requestStarting || request.isActive()) {
            if (error) {
                *error = QObject::tr(
                    "This Waypoint Leader window already owns a mission refresh.");
            }
            return false;
        }

        requestStarting = true;
        provisionalAirMaster = airMaster;
        provisionalResults.clear();
        provisionalCancelRequested = false;
        provisionalCancelReason.clear();

        ExactMissionTransferToken token;
        QString detail;
        const QPointer<SwarmWaypointLeaderWindowAdapter> guard(q);
        const QPointer<SwarmWaypointLeaderWindowAdapterBackend>
            backendGuard = backend;
        const ExactMissionSnapshotService::StartResult result =
            backendGuard->requestMission(q, airMaster, &token, &detail);
        if (!guard) {
            return false;
        }
        if (!backend || backend.data() != backendGuard.data()) {
            requestStarting = false;
            provisionalResults.clear();
            if (error) {
                *error = QObject::tr(
                    "The exact mission backend disappeared while starting the refresh.");
            }
            return false;
        }

        requestStarting = false;
        const bool cancelRequested = provisionalCancelRequested;
        const QString cancelReason = provisionalCancelReason;
        provisionalCancelRequested = false;
        provisionalCancelReason.clear();
        provisionalAirMaster = SwarmVehicleInstanceLease();

        if (result != ExactMissionSnapshotService::StartResult::Started
            || !token.isValid()) {
            provisionalResults.clear();
            if (error) {
                *error = detail.trimmed().isEmpty()
                    ? missionStartError(result) : detail;
            }
            missionStatus = error ? *error : missionStartError(result);
            scheduleChanged();
            return false;
        }

        request.airMaster = airMaster;
        request.token = token;
        const QVector<ExactMissionTransferResult> buffered =
            std::exchange(
                provisionalResults,
                QVector<ExactMissionTransferResult>());
        for (const ExactMissionTransferResult &finished : buffered) {
            processMissionFinished(finished);
            if (!guard) {
                return true;
            }
        }
        if (cancelRequested && request.isActive()
            && sameVehicle(request.airMaster, airMaster)) {
            cancelMissionRefresh(airMaster, cancelReason);
        }
        return true;
    }

    void cancelMissionRefresh(
        const SwarmVehicleInstanceLease &airMaster,
        const QString &reason)
    {
        if (requestStarting
            && sameVehicle(provisionalAirMaster, airMaster)) {
            provisionalCancelRequested = true;
            provisionalCancelReason = reason;
            provisionalResults.clear();
            return;
        }
        if (!backend || !request.isActive()
            || !sameVehicle(request.airMaster, airMaster)) {
            return;
        }
        const ExactMissionTransferToken token = request.token;
        request = MissionRequest();
        backend->cancelMission(token, reason);
        scheduleChanged();
    }

    void missionFinished(ExactMissionTransferResult result)
    {
        if (requestStarting) {
            provisionalResults.append(std::move(result));
            return;
        }
        processMissionFinished(result);
    }

    void processMissionFinished(
        const ExactMissionTransferResult &result)
    {
        if (!request.isActive()
            || result.token.id != request.token.id
            || result.key.missionType != MAV_MISSION_TYPE_MISSION
            || !sameVehicle(result.key.vehicle, request.airMaster)) {
            return;
        }
        const SwarmVehicleInstanceLease completedAir = request.airMaster;
        request = MissionRequest();
        if (result.succeeded()
            && sameVehicle(result.snapshot.key.vehicle, completedAir)) {
            missionStatus.clear();
            SwarmWaypointLeaderMissionSnapshot converted;
            QString ignored;
            if (convertMission(result.snapshot, completedAir,
                               &converted, &ignored)) {
                SwarmWaypointLeaderMissionPath path;
                if (SwarmWaypointLeaderMissionPath::build(
                        converted, &path, &ignored)) {
                    profileMission = std::move(converted);
                    profilePath = std::move(path);
                }
            }
        } else {
            missionStatus = result.errorString.trimmed().isEmpty()
                ? QObject::tr("The exact mission refresh failed.")
                : result.errorString;
        }
        scheduleChanged();
    }

    void refreshProfileCache()
    {
        if (!profileMission.airMaster.isValid()) {
            return;
        }
        ExactMissionSnapshot exact;
        SwarmWaypointLeaderMissionSnapshot converted;
        QString ignored;
        if (!snapshotFor(profileMission.airMaster, &exact)
            || !convertMission(exact, profileMission.airMaster,
                               &converted, &ignored)) {
            profileMission = SwarmWaypointLeaderMissionSnapshot();
            profilePath = SwarmWaypointLeaderMissionPath();
            return;
        }
        SwarmWaypointLeaderMissionPath path;
        if (SwarmWaypointLeaderMissionPath::build(
                converted, &path, &ignored)) {
            profileMission = std::move(converted);
            profilePath = std::move(path);
        }
    }

    SwarmWaypointLeaderWindowAdapter *q = nullptr;
    QPointer<SwarmWaypointLeaderWindowAdapterBackend> backend;
    std::unique_ptr<SwarmWaypointLeaderWindowAdapterBackend> ownedBackend;
    QMetaObject::Connection backendDestroyed;
    ChangedHandler changedHandler;
    MissionRequest request;
    SwarmVehicleInstanceLease provisionalAirMaster;
    QVector<ExactMissionTransferResult> provisionalResults;
    SwarmWaypointLeaderMissionSnapshot profileMission;
    SwarmWaypointLeaderMissionPath profilePath;
    QString missionStatus;
    QString provisionalCancelReason;
    bool requestStarting = false;
    bool provisionalCancelRequested = false;
    bool changedScheduled = false;
};

SwarmWaypointLeaderWindowAdapter::SwarmWaypointLeaderWindowAdapter(
    SwarmTelemetryRegistry *registry,
    ExactMissionSnapshotService *missions,
    SwarmWaypointLeaderExecutor *executor,
    QObject *parent)
    : SwarmWaypointLeaderWindowInterface(parent)
{
    auto backend = std::make_unique<ApplicationBackend>(
        registry, missions, executor);
    auto *rawBackend = backend.get();
    m_impl = std::make_unique<Implementation>(
        this, rawBackend, std::move(backend));
}

SwarmWaypointLeaderWindowAdapter::SwarmWaypointLeaderWindowAdapter(
    SwarmWaypointLeaderWindowAdapterBackend *backend,
    QObject *parent)
    : SwarmWaypointLeaderWindowInterface(parent)
    , m_impl(std::make_unique<Implementation>(
          this, backend,
          std::unique_ptr<SwarmWaypointLeaderWindowAdapterBackend>()))
{
}

SwarmWaypointLeaderWindowAdapter::~SwarmWaypointLeaderWindowAdapter() = default;

QVector<SwarmWaypointLeaderWindowVehicle>
SwarmWaypointLeaderWindowAdapter::vehicles() const
{
    return m_impl->vehicles();
}

void SwarmWaypointLeaderWindowAdapter::refreshVehicles()
{
    if (m_impl->backend) {
        m_impl->backend->refreshVehicles();
    }
    m_impl->scheduleChanged();
}

bool SwarmWaypointLeaderWindowAdapter::missionForAirMaster(
    const SwarmVehicleInstanceLease &airMaster,
    SwarmWaypointLeaderMissionSnapshot *mission,
    QString *error) const
{
    return m_impl->missionForAirMaster(airMaster, mission, error);
}

quint64 SwarmWaypointLeaderWindowAdapter::missionObservationRevision(
    const SwarmVehicleInstanceLease &airMaster) const noexcept
{
    return m_impl->missionObservationRevision(airMaster);
}

bool SwarmWaypointLeaderWindowAdapter::refreshMission(
    const SwarmVehicleInstanceLease &airMaster, QString *error)
{
    return m_impl->refreshMission(airMaster, error);
}

void SwarmWaypointLeaderWindowAdapter::cancelMissionRefresh(
    const SwarmVehicleInstanceLease &airMaster,
    const QString &reason)
{
    m_impl->cancelMissionRefresh(airMaster, reason);
}

bool SwarmWaypointLeaderWindowAdapter::executorReady(QString *error) const
{
    if (m_impl->backend) {
        return m_impl->backend->executorReady(error);
    }
    if (error) {
        *error = tr("The Waypoint Leader application services are unavailable.");
    }
    return false;
}

bool SwarmWaypointLeaderWindowAdapter::validatePlan(
    const SwarmWaypointLeaderPlan &plan, QString *error) const
{
    if (m_impl->backend) {
        return m_impl->backend->validatePlan(plan, error);
    }
    if (error) {
        *error = tr("The Waypoint Leader executor is unavailable.");
    }
    return false;
}

bool SwarmWaypointLeaderWindowAdapter::start(
    const SwarmWaypointLeaderPlan &plan, QString *error)
{
    m_impl->missionStatus.clear();
    if (m_impl->backend) {
        return m_impl->backend->start(plan, error);
    }
    if (error) {
        *error = tr("The Waypoint Leader executor is unavailable.");
    }
    return false;
}

void SwarmWaypointLeaderWindowAdapter::cancelActiveRun(
    const QString &reason)
{
    if (m_impl->backend) {
        m_impl->backend->cancelActiveRun(reason);
    }
}

bool SwarmWaypointLeaderWindowAdapter::requestMode(
    SwarmWaypointLeaderMode mode, QString *error)
{
    if (m_impl->backend) {
        return m_impl->backend->requestMode(mode, error);
    }
    if (error) {
        *error = tr("The Waypoint Leader executor is unavailable.");
    }
    return false;
}

bool SwarmWaypointLeaderWindowAdapter::isRunning() const noexcept
{
    return m_impl->backend && m_impl->backend->isRunning();
}

SwarmWaypointLeaderMode SwarmWaypointLeaderWindowAdapter::mode() const noexcept
{
    return m_impl->backend ? m_impl->backend->mode()
                           : SwarmWaypointLeaderMode::Idle;
}

QString SwarmWaypointLeaderWindowAdapter::statusText() const
{
    if (!m_impl->missionStatus.trimmed().isEmpty()) {
        return m_impl->missionStatus;
    }
    return m_impl->backend
        ? m_impl->backend->statusText()
        : tr("Waypoint Leader application services are unavailable.");
}

void SwarmWaypointLeaderWindowAdapter::setChangedHandler(
    ChangedHandler handler)
{
    m_impl->changedHandler = std::move(handler);
}
