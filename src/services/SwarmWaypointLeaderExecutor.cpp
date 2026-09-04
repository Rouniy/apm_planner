#include "SwarmWaypointLeaderExecutor.h"

#include "comm/SwarmTelemetryRegistry.h"

#include <QThread>

#include <algorithm>
#include <atomic>
#include <utility>

namespace
{

bool setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

QString detailOr(const QString &detail, const QString &fallback)
{
    return detail.trimmed().isEmpty() ? fallback : detail.trimmed();
}

class ProductionBackend final : public SwarmWaypointLeaderExecutorBackend
{
public:
    ProductionBackend(
        SwarmTelemetryRegistry *registry,
        ExactMissionSnapshotService *missions,
        SwarmCommandService *swarm,
        VehicleCommandService *commands,
        ParameterService *parameters,
        QObject *parent = nullptr)
        : SwarmWaypointLeaderExecutorBackend(parent)
        , m_registry(registry)
        , m_missions(missions)
        , m_swarm(swarm)
        , m_commands(commands)
        , m_parameters(parameters)
    {
        if (m_parameters) {
            connect(m_parameters, &ParameterService::exactOperationFinished,
                    this, [this](ParameterService::ExactOperationReport report) {
                const auto callback = m_callbacks.parameterFinished;
                if (callback) {
                    callback(std::move(report));
                }
            });
            connect(m_parameters, &ParameterService::exactReservationReleased,
                    this, [this](quint64 id) {
                const auto callback =
                    m_callbacks.parameterReservationReleased;
                if (callback) {
                    callback(id);
                }
            });
        }
        if (m_commands) {
            connect(m_commands, &VehicleCommandService::exactCommandFinished,
                    this, [this](VehicleCommandService::ExactCommandReport report) {
                const auto callback = m_callbacks.commandFinished;
                if (callback) {
                    callback(std::move(report));
                }
            });
            connect(m_commands,
                    &VehicleCommandService::exactReservationReleased,
                    this, [this](quint64 id) {
                const auto callback = m_callbacks.commandReservationReleased;
                if (callback) {
                    callback(id);
                }
            });
        }
        if (m_swarm) {
            connect(m_swarm, &SwarmCommandService::sessionCancelled,
                    this, [this](quint64 id, const QString &reason) {
                const auto callback = m_callbacks.swarmSessionCancelled;
                if (callback) {
                    callback(id, reason);
                }
            });
        }
    }

    void setCallbacks(Callbacks callbacks) override
    {
        m_callbacks = std::move(callbacks);
    }

    bool ready(QString *error) const override
    {
        if (error) {
            error->clear();
        }
        if (!m_registry || !m_missions || !m_swarm
            || !m_commands || !m_parameters) {
            return setError(error, QStringLiteral(
                "Waypoint Leader exact services are unavailable."));
        }
        QThread *const expected = thread();
        if (QThread::currentThread() != expected
            || m_registry->thread() != expected
            || m_missions->thread() != expected
            || m_swarm->thread() != expected
            || m_commands->thread() != expected
            || m_parameters->thread() != expected) {
            return setError(error, QStringLiteral(
                "Waypoint Leader exact services must share one thread."));
        }
        return true;
    }

    bool capture(const SwarmWaypointLeaderPlan &plan,
                 Snapshot *snapshot, QString *error) const override
    {
        if (snapshot) {
            *snapshot = Snapshot();
        }
        if (!snapshot || !ready(error)) {
            return false;
        }

        QPointer<ProductionBackend> guard(
            const_cast<ProductionBackend *>(this));
        QPointer<SwarmTelemetryRegistry> registry = m_registry;
        QPointer<ExactMissionSnapshotService> missions = m_missions;
        QList<SwarmVehicleInstanceLease> leases;
        leases.append(plan.groundMaster);
        leases.append(plan.airMaster);
        QVector<SwarmWaypointLeaderFollower> followers = plan.followers;
        std::sort(followers.begin(), followers.end(),
                  [](const SwarmWaypointLeaderFollower &left,
                     const SwarmWaypointLeaderFollower &right) {
            return left.order < right.order;
        });
        for (const auto &follower : followers) {
            leases.append(follower.lease);
        }

        for (const auto &lease : leases) {
            SwarmTelemetrySnapshot telemetry;
            if (!registry || !registry->snapshotForLease(lease, &telemetry)
                || !registry->validateLease(
                    lease, SwarmWaypointLeaderCore::MaximumTelemetryAgeMs)) {
                return setError(error, QStringLiteral(
                    "A Waypoint Leader exact vehicle lease is stale."));
            }
            SwarmWaypointLeaderVehicleState state;
            state.telemetry = telemetry;
            snapshot->vehicles.append(state);
        }

        ExactMissionSnapshot exact;
        ExactMissionSnapshotLease lease;
        const bool acquired = missions
            && missions->acquireSnapshot(
                plan.airMaster, MAV_MISSION_TYPE_MISSION, &exact, &lease);
        if (!guard) {
            return false;
        }
        const bool validMissionLease = acquired && missions
            && missions->validateSnapshotLease(lease);
        if (!guard) {
            return false;
        }
        if (!validMissionLease) {
            return setError(error, QStringLiteral(
                "The exact air-master mission snapshot is unavailable or stale."));
        }
        if (exact.contentGeneration != plan.missionContentGeneration
            || exact.contentDigest != plan.missionContentDigest
            || exact.waypointLeaderSignature
                != plan.missionSignature.toLatin1()) {
            return setError(error, QStringLiteral(
                "The exact air-master mission changed after confirmation."));
        }

        SwarmWaypointLeaderMissionSnapshot mission;
        mission.airMaster = exact.key.vehicle;
        mission.missionType = exact.key.missionType;
        mission.contentGeneration = exact.contentGeneration;
        mission.contentDigest = exact.contentDigest;
        mission.items.reserve(exact.items.size());
        for (const auto &source : exact.items) {
            SwarmWaypointLeaderMissionItem item;
            item.sequence = source.seq;
            item.command = source.command;
            item.frame = source.frame;
            item.latitudeE7 = source.x;
            item.longitudeE7 = source.y;
            item.relativeAltitudeM = source.z;
            mission.items.append(item);
        }
        snapshot->mission = mission;
        snapshot->observationClockNowMs =
            registry->observationClockNowMs();
        if (error) {
            error->clear();
        }
        return true;
    }

    SwarmCommandService::Result reserveSwarm(
        QObject *owner, const QVector<SwarmCommandMember> &members,
        int maximumBatchHz, SwarmCommandSessionToken *token,
        QString *error) override
    {
        return m_swarm
            ? m_swarm->reserve(owner, members, maximumBatchHz, token, error)
            : SwarmCommandService::Result::InvalidSession;
    }

    SwarmCommandService::Result releaseSwarm(
        const SwarmCommandSessionToken &token) override
    {
        return m_swarm ? m_swarm->release(token)
                       : SwarmCommandService::Result::InvalidSession;
    }

    SwarmCommandService::BatchReport requestPositionStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds, int rateHz) override
    {
        return m_swarm
            ? m_swarm->requestPositionStreams(token, slotIds, rateHz)
            : SwarmCommandService::BatchReport();
    }

    SwarmCommandService::BatchReport sendPositionTargets(
        const SwarmCommandSessionToken &token,
        const QVector<SwarmPositionTarget> &targets,
        SwarmCommandService::PositionTargetPriority priority) override
    {
        return m_swarm
            ? m_swarm->sendPositionTargets(token, targets, priority)
            : SwarmCommandService::BatchReport();
    }

    VehicleCommandService::ExactReservationResult reserveCommands(
        QObject *owner, const QList<SwarmVehicleInstanceLease> &leases,
        VehicleCommandService::ExactReservationToken *token,
        QString *error) override
    {
        return m_commands
            ? m_commands->reserveExactEndpoints(owner, leases, token, error)
            : VehicleCommandService::ExactReservationResult::ContextUnavailable;
    }

    bool releaseCommands(
        const VehicleCommandService::ExactReservationToken &token) override
    {
        return m_commands && m_commands->releaseExactReservation(token);
    }

    VehicleCommandService::ExactSubmitResult submitCommand(
        const VehicleCommandService::ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const VehicleCommandService::ExactCommandRequest &request,
        VehicleCommandService::ExactCommandToken *token,
        QString *error) override
    {
        return m_commands
            ? m_commands->submitExactCommandLong(
                  reservation, lease, request, token, error)
            : VehicleCommandService::ExactSubmitResult::ContextUnavailable;
    }

    ParameterService::ExactReservationResult reserveParameters(
        QObject *owner, const QList<SwarmVehicleInstanceLease> &leases,
        ParameterService::ExactReservationToken *token,
        QString *error) override
    {
        return m_parameters
            ? m_parameters->reserveExactEndpoints(owner, leases, token, error)
            : ParameterService::ExactReservationResult::ContextUnavailable;
    }

    bool cancelParameterOperation(
        const ParameterService::ExactReservationToken &reservation,
        const ParameterService::ExactOperationToken &operation,
        const QString &reason) override
    {
        return m_parameters
            && m_parameters->cancelExactOperation(
                reservation, operation, reason);
    }

    bool releaseParameters(
        const ParameterService::ExactReservationToken &token) override
    {
        return m_parameters && m_parameters->releaseExactReservation(token);
    }

    ParameterService::ExactSubmitResult submitParameterRead(
        const ParameterService::ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const ParameterService::ExactReadRequest &request,
        ParameterService::ExactOperationToken *token,
        QString *error) override
    {
        return m_parameters
            ? m_parameters->submitExactRead(
                  reservation, lease, request, token, error)
            : ParameterService::ExactSubmitResult::ContextUnavailable;
    }

    ParameterService::ExactSubmitResult submitParameterWrite(
        const ParameterService::ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const ParameterService::ExactWriteRequest &request,
        ParameterService::ExactOperationToken *token,
        QString *error) override
    {
        return m_parameters
            ? m_parameters->submitExactWrite(
                  reservation, lease, request, token, error)
            : ParameterService::ExactSubmitResult::ContextUnavailable;
    }

private:
    QPointer<SwarmTelemetryRegistry> m_registry;
    QPointer<ExactMissionSnapshotService> m_missions;
    QPointer<SwarmCommandService> m_swarm;
    QPointer<VehicleCommandService> m_commands;
    QPointer<ParameterService> m_parameters;
    Callbacks m_callbacks;
};

} // namespace

SwarmWaypointLeaderExecutor::SwarmWaypointLeaderExecutor(
    SwarmTelemetryRegistry *registry,
    ExactMissionSnapshotService *missions,
    SwarmCommandService *swarmCommands,
    VehicleCommandService *commands,
    ParameterService *parameters,
    QObject *parent)
    : SwarmWaypointLeaderExecutor(
          new ProductionBackend(registry, missions, swarmCommands,
                                commands, parameters), parent)
{
    if (m_backend) {
        m_backend->setParent(this);
    }
}

SwarmWaypointLeaderExecutor::SwarmWaypointLeaderExecutor(
    SwarmWaypointLeaderExecutorBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    m_timer.setObjectName(QStringLiteral(
        "SwarmWaypointLeaderControlTimer"));
    m_timer.setInterval(ControlIntervalMs);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout,
            this, &SwarmWaypointLeaderExecutor::controlTick);

    if (m_backend) {
        QPointer<SwarmWaypointLeaderExecutor> guard(this);
        SwarmWaypointLeaderExecutorBackend::Callbacks callbacks;
        callbacks.parameterFinished = [guard](auto report) {
            if (guard) {
                guard->parameterFinished(report);
            }
        };
        callbacks.commandFinished = [guard](auto report) {
            if (guard) {
                guard->commandFinished(report);
            }
        };
        callbacks.parameterReservationReleased = [guard](quint64 id) {
            if (guard) {
                guard->parameterReservationReleased(id);
            }
        };
        callbacks.commandReservationReleased = [guard](quint64 id) {
            if (guard) {
                guard->commandReservationReleased(id);
            }
        };
        callbacks.swarmSessionCancelled =
            [guard](quint64 id, const QString &reason) {
                if (guard) {
                    guard->swarmSessionCancelled(id, reason);
                }
            };
        m_backend->setCallbacks(std::move(callbacks));
    }
}

SwarmWaypointLeaderExecutor::~SwarmWaypointLeaderExecutor()
{
    m_destroying = true;
    m_dispatchAllowed = false;
    m_timer.stop();
    if (!m_backend) {
        return;
    }
    m_backend->setCallbacks({});
    if (m_pendingParameter.token.isValid() && m_parametersReserved) {
        m_backend->cancelParameterOperation(
            m_parameterReservation, m_pendingParameter.token,
            QStringLiteral("Waypoint Leader executor was destroyed."));
    }
    if (m_parametersReserved) {
        m_backend->releaseParameters(m_parameterReservation);
    }
    if (m_commandsReserved) {
        m_backend->releaseCommands(m_commandReservation);
    }
    if (m_swarmReserved) {
        m_backend->releaseSwarm(m_swarmToken);
    }
}

quint64 SwarmWaypointLeaderExecutor::nextRunGeneration()
{
    static std::atomic<quint64> next{0};
    quint64 result = ++next;
    if (result == 0) {
        result = ++next;
    }
    return result;
}

bool SwarmWaypointLeaderExecutor::sameToken(
    const ParameterService::ExactOperationToken &left,
    const ParameterService::ExactOperationToken &right)
{
    return left.operationId != 0 && left.operationId == right.operationId
        && left.reservationId == right.reservationId
        && left.lease.sameInstance(right.lease)
        && left.kind == right.kind && left.name == right.name
        && left.type == right.type
        && left.normalizedValue == right.normalizedValue;
}

bool SwarmWaypointLeaderExecutor::sameToken(
    const VehicleCommandService::ExactCommandToken &left,
    const VehicleCommandService::ExactCommandToken &right)
{
    return left.transactionId != 0
        && left.transactionId == right.transactionId
        && left.reservationId == right.reservationId
        && left.lease.sameInstance(right.lease)
        && left.command == right.command;
}

bool SwarmWaypointLeaderExecutor::swarmOutcomeUncertain(
    const SwarmCommandService::BatchReport &report)
{
    if (report.result
        == SwarmCommandService::Result::TransportOutcomeUncertain) {
        return true;
    }
    return std::any_of(
        report.members.cbegin(), report.members.cend(),
        [](const SwarmCommandService::MemberReport &member) {
            return member.framesAttempted > member.framesSent;
        });
}

bool SwarmWaypointLeaderExecutor::executorReady(QString *error) const
{
    if (error) {
        error->clear();
    }
    if (m_state == State::Draining) {
        return setError(error, QStringLiteral(
            "Waypoint Leader is still draining exact reservations."));
    }
    if (m_state == State::OutcomeUncertain) {
        return setError(error, QStringLiteral(
            "A prior Waypoint Leader outcome is uncertain; restart is blocked."));
    }
    if (!m_backend) {
        return setError(error, QStringLiteral(
            "Waypoint Leader executor backend is unavailable."));
    }
    if (QThread::currentThread() != thread()
        || m_backend->thread() != thread()) {
        return setError(error, QStringLiteral(
            "Waypoint Leader executor and backend must share one thread."));
    }
    return m_backend->ready(error);
}

bool SwarmWaypointLeaderExecutor::probePlan(
    const SwarmWaypointLeaderPlan &plan,
    const SwarmWaypointLeaderExecutorBackend::Snapshot &snapshot,
    QString *error) const
{
    SwarmWaypointLeaderCore probe;
    const SwarmWaypointLeaderTick tick = probe.tick(
        plan, snapshot.vehicles, snapshot.mission,
        snapshot.observationClockNowMs,
        SwarmWaypointLeaderCore::MaximumTelemetryAgeMs);
    if (!tick.shouldContinue()) {
        return setError(error, detailOr(
            tick.status,
            QStringLiteral("The exact Waypoint Leader plan is invalid.")));
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool SwarmWaypointLeaderExecutor::validatePlan(
    const SwarmWaypointLeaderPlan &plan, QString *error) const
{
    if (!executorReady(error)) {
        return false;
    }
    QPointer<SwarmWaypointLeaderExecutor> guard(
        const_cast<SwarmWaypointLeaderExecutor *>(this));
    SwarmWaypointLeaderExecutorBackend::Snapshot snapshot;
    if (!m_backend->capture(plan, &snapshot, error)) {
        return false;
    }
    if (!guard) {
        return false;
    }
    return probePlan(plan, snapshot, error);
}

bool SwarmWaypointLeaderExecutor::capture(
    const SwarmWaypointLeaderPlan &plan,
    SwarmWaypointLeaderExecutorBackend::Snapshot *snapshot,
    QString *error) const
{
    QPointer<SwarmWaypointLeaderExecutor> guard(
        const_cast<SwarmWaypointLeaderExecutor *>(this));
    if (!m_backend || !snapshot
        || !m_backend->capture(plan, snapshot, error) || !guard) {
        return false;
    }
    for (auto &vehicle : snapshot->vehicles) {
        vehicle.availableParameters.clear();
        const Capability *capability = capabilityFor(vehicle.telemetry.lease);
        if (!capability) {
            continue;
        }
        for (auto type = capability->types.cbegin();
             type != capability->types.cend(); ++type) {
            if (type.value() != ParameterType::Unknown) {
                vehicle.availableParameters.insert(type.key());
            }
        }
    }
    return true;
}

QList<SwarmVehicleInstanceLease>
SwarmWaypointLeaderExecutor::flightLeases(
    const SwarmWaypointLeaderPlan &plan) const
{
    QList<SwarmVehicleInstanceLease> result{plan.airMaster};
    QVector<SwarmWaypointLeaderFollower> followers = plan.followers;
    std::sort(followers.begin(), followers.end(),
              [](const auto &left, const auto &right) {
        return left.order < right.order;
    });
    for (const auto &follower : followers) {
        result.append(follower.lease);
    }
    return result;
}

QVector<SwarmCommandMember> SwarmWaypointLeaderExecutor::swarmMembers(
    const SwarmWaypointLeaderPlan &plan) const
{
    QVector<SwarmVehicleInstanceLease> leases{plan.groundMaster};
    const auto flight = flightLeases(plan);
    for (const auto &lease : flight) {
        leases.append(lease);
    }
    QVector<SwarmCommandMember> result;
    result.reserve(leases.size());
    for (int index = 0; index < leases.size(); ++index) {
        SwarmCommandMember member;
        member.slotId = index;
        member.lease = leases.at(index);
        member.required.fields = SwarmTelemetryRequirements::Position
            | SwarmTelemetryRequirements::Velocity;
        member.required.heartbeatMaximumAgeMs =
            SwarmWaypointLeaderCore::MaximumTelemetryAgeMs;
        member.required.positionMaximumAgeMs =
            SwarmWaypointLeaderCore::MaximumTelemetryAgeMs;
        member.required.velocityMaximumAgeMs =
            SwarmWaypointLeaderCore::MaximumTelemetryAgeMs;
        member.flightMode = SwarmCommandMember::FlightModeRequirement::Any;
        result.append(member);
    }
    return result;
}

int SwarmWaypointLeaderExecutor::slotFor(
    const SwarmVehicleInstanceLease &lease) const
{
    for (int index = 0; index < m_groupLeases.size(); ++index) {
        if (m_groupLeases.at(index).sameInstance(lease)) {
            return index;
        }
    }
    return -1;
}

SwarmWaypointLeaderExecutor::Capability *
SwarmWaypointLeaderExecutor::capabilityFor(
    const SwarmVehicleInstanceLease &lease)
{
    for (auto &capability : m_capabilities) {
        if (capability.lease.sameInstance(lease)) {
            return &capability;
        }
    }
    return nullptr;
}

const SwarmWaypointLeaderExecutor::Capability *
SwarmWaypointLeaderExecutor::capabilityFor(
    const SwarmVehicleInstanceLease &lease) const
{
    for (const auto &capability : m_capabilities) {
        if (capability.lease.sameInstance(lease)) {
            return &capability;
        }
    }
    return nullptr;
}

bool SwarmWaypointLeaderExecutor::start(
    const SwarmWaypointLeaderPlan &plan, QString *error)
{
    if (error) {
        error->clear();
    }
    if (m_state == State::OutcomeUncertain) {
        return setError(error, QStringLiteral(
            "A prior Waypoint Leader outcome is uncertain; restart is blocked."));
    }
    if (m_state != State::Idle || m_swarmReserved
        || m_commandsReserved || m_parametersReserved) {
        return setError(error, QStringLiteral(
            "Waypoint Leader is already active or draining."));
    }
    if (!validatePlan(plan, error)) {
        return false;
    }

    m_runGeneration = nextRunGeneration();
    m_plan = plan;
    m_core = SwarmWaypointLeaderCore();
    m_batch = Batch();
    m_pendingParameter = PendingParameter();
    m_pendingCommand = PendingCommand();
    m_capabilities.clear();
    m_probes.clear();
    m_probeIndex = 0;
    m_uncertain = false;
    m_drainReason.clear();
    m_groupLeases.clear();
    m_groupLeases.append(plan.groundMaster);
    m_flightLeases = flightLeases(plan);
    for (const auto &lease : m_flightLeases) {
        m_groupLeases.append(lease);
    }

    QString detail;
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    ++m_backendDepth;
    const auto swarmResult = m_backend->reserveSwarm(
        this, swarmMembers(plan), SwarmWaypointLeaderCore::ControlRateHz,
        &m_swarmToken, &detail);
    if (!guard) {
        return false;
    }
    --m_backendDepth;
    m_swarmReserved = swarmResult == SwarmCommandService::Result::Reserved
        && m_swarmToken.isValid();
    drainDeferredCallbacks();
    if (!guard) {
        return false;
    }
    if (!m_swarmReserved) {
        m_plan = {};
        m_groupLeases.clear();
        m_flightLeases.clear();
        return setError(error, detailOr(
            detail, QStringLiteral("The exact swarm group is busy.")));
    }

    ++m_backendDepth;
    const auto commandResult = m_backend->reserveCommands(
        this, m_flightLeases, &m_commandReservation, &detail);
    if (!guard) {
        return false;
    }
    --m_backendDepth;
    m_commandsReserved = commandResult
            == VehicleCommandService::ExactReservationResult::Reserved
        && m_commandReservation.isValid();
    drainDeferredCallbacks();
    if (!guard) {
        return false;
    }
    if (!m_commandsReserved) {
        beginDrain(detailOr(
            detail, QStringLiteral("Exact command reservation failed.")),
            false);
        if (guard) {
            setError(error, m_drainReason);
        }
        return false;
    }

    ++m_backendDepth;
    const auto parameterResult = m_backend->reserveParameters(
        this, m_flightLeases, &m_parameterReservation, &detail);
    if (!guard) {
        return false;
    }
    --m_backendDepth;
    m_parametersReserved = parameterResult
            == ParameterService::ExactReservationResult::Reserved
        && m_parameterReservation.isValid();
    drainDeferredCallbacks();
    if (!guard) {
        return false;
    }
    if (!m_parametersReserved) {
        beginDrain(detailOr(
            detail, QStringLiteral("Exact parameter reservation failed.")),
            false);
        if (guard) {
            setError(error, m_drainReason);
        }
        return false;
    }

    // The window validates before confirmation. Reservations can invoke
    // application policy, so capture the complete exact plan once more after
    // all three owners have been acquired and before the first transmission.
    SwarmWaypointLeaderExecutorBackend::Snapshot snapshot;
    ++m_backendDepth;
    const bool captured = m_backend->capture(plan, &snapshot, &detail);
    if (!guard) {
        return false;
    }
    --m_backendDepth;
    drainDeferredCallbacks();
    if (!guard) {
        return false;
    }
    if (!captured || !m_swarmReserved || !m_commandsReserved
        || !m_parametersReserved || !probePlan(plan, snapshot, &detail)) {
        beginDrain(detailOr(
            detail, QStringLiteral(
                "The exact plan changed while services were reserved.")),
            false);
        if (guard) {
            setError(error, m_drainReason);
        }
        return false;
    }

    m_dispatchAllowed = true;
    beginDiscovery();
    if (!guard) {
        return false;
    }
    if (!isRunning()) {
        return setError(error, detailOr(
            m_drainReason,
            QStringLiteral("Exact parameter discovery could not start.")));
    }
    return true;
}

void SwarmWaypointLeaderExecutor::beginDiscovery()
{
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    m_capabilities.clear();
    m_probes.clear();
    for (const auto &lease : m_flightLeases) {
        m_capabilities.append({lease, {}});
        m_probes.append(
            {lease,
             {QStringLiteral("RTL_ALT"), QStringLiteral("RTL_ALT_M")}, 0});
        m_probes.append(
            {lease,
             {QStringLiteral("WPNAV_ACCEL"), QStringLiteral("WP_ACC")}, 0});
    }
    m_probeIndex = 0;
    setState(State::DiscoveringParameters);
    if (!guard) {
        return;
    }
    setStatus(QStringLiteral(
        "Discovering exact parameter aliases and runtime types."));
    if (!guard) {
        return;
    }
    submitNextRead();
}

void SwarmWaypointLeaderExecutor::submitNextRead()
{
    if (!m_dispatchAllowed || m_state != State::DiscoveringParameters
        || m_pendingParameter.token.isValid()) {
        return;
    }
    if (m_probeIndex >= m_probes.size()) {
        finishDiscovery();
        return;
    }
    const AliasProbe &probe = m_probes.at(m_probeIndex);
    if (probe.nextAlias < 0 || probe.nextAlias >= probe.aliases.size()) {
        fail(QStringLiteral("The parameter alias probe is invalid."),
             SwarmWaypointLeaderBatchResult::Rejected);
        return;
    }

    ParameterService::ExactReadRequest request;
    request.name = probe.aliases.at(probe.nextAlias);
    ParameterService::ExactOperationToken token;
    QString detail;
    m_pendingParameter.runGeneration = m_runGeneration;
    m_pendingParameter.discovery = true;
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    ++m_backendDepth;
    const auto result = m_backend->submitParameterRead(
        m_parameterReservation, probe.lease, request, &token, &detail);
    if (!guard) {
        return;
    }
    --m_backendDepth;
    m_pendingParameter.token = token;
    if (result != ParameterService::ExactSubmitResult::Started
        || !token.isValid()) {
        m_pendingParameter = {};
        drainDeferredCallbacks();
        if (guard) {
            fail(detailOr(
                detail, QStringLiteral("An exact parameter read was rejected.")),
                result == ParameterService::ExactSubmitResult::
                    TransportOutcomeUncertain
                    ? SwarmWaypointLeaderBatchResult::OutcomeUncertain
                    : SwarmWaypointLeaderBatchResult::Rejected,
                result == ParameterService::ExactSubmitResult::
                    TransportOutcomeUncertain);
        }
        return;
    }
    drainDeferredCallbacks();
}

void SwarmWaypointLeaderExecutor::finishDiscovery()
{
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    SwarmWaypointLeaderExecutorBackend::Snapshot snapshot;
    QString detail;
    if (!capture(m_plan, &snapshot, &detail)
        || !probePlan(m_plan, snapshot, &detail)) {
        if (guard) {
            fail(detailOr(
                detail, QStringLiteral(
                    "The exact plan changed during parameter discovery.")),
                SwarmWaypointLeaderBatchResult::Rejected);
        }
        return;
    }
    setState(State::Running);
    if (!guard) {
        return;
    }
    setStatus(QStringLiteral(
        "Exact parameter discovery complete; Waypoint Leader is running."));
    if (!guard) {
        return;
    }
    m_timer.start();
    // One immediate control sample is allowed. Later timeout deliveries each
    // execute exactly one sample; there is deliberately no elapsed-time loop.
    controlTick();
}

void SwarmWaypointLeaderExecutor::controlTick()
{
    if (!m_dispatchAllowed
        || (m_state != State::Running
            && m_state != State::WaitingForCompletion)) {
        return;
    }
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    SwarmWaypointLeaderExecutorBackend::Snapshot snapshot;
    QString detail;
    if (!capture(m_plan, &snapshot, &detail)) {
        if (guard) {
            fail(detailOr(
                detail, QStringLiteral(
                    "The exact vehicle group became stale.")),
                SwarmWaypointLeaderBatchResult::Rejected);
        }
        return;
    }
    const auto tick = m_core.tick(
        m_plan, snapshot.vehicles, snapshot.mission,
        snapshot.observationClockNowMs,
        SwarmWaypointLeaderCore::MaximumTelemetryAgeMs);
    handleTick(tick);
}

void SwarmWaypointLeaderExecutor::handleTick(
    const SwarmWaypointLeaderTick &tick)
{
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    if (!tick.status.trimmed().isEmpty()) {
        setStatus(tick.status);
        if (!guard) {
            return;
        }
    }
    if (tick.urgent && !tick.intents.isEmpty()) {
        dispatchUrgent(tick);
        return;
    }
    if (tick.state == SwarmWaypointLeaderTick::State::Cancelling) {
        if (m_batch.coreBatchId == 0
            || m_batch.coreBatchId != tick.cancelBatchId) {
            fail(QStringLiteral(
                     "Core cancellation lost its absolute batch identity."),
                 SwarmWaypointLeaderBatchResult::OutcomeUncertain, true);
            return;
        }
        m_batch.cancellationRequested = true;
        if (!m_pendingParameter.token.isValid()
            && !m_pendingCommand.token.isValid()) {
            completeBatch(SwarmWaypointLeaderBatchResult::Cancelled,
                          QStringLiteral("The pending batch was cancelled."));
        }
        return;
    }
    if (tick.state == SwarmWaypointLeaderTick::State::Stopped
        || tick.state == SwarmWaypointLeaderTick::State::Completed) {
        beginDrain(detailOr(
            tick.status,
            tick.state == SwarmWaypointLeaderTick::State::Completed
                ? QStringLiteral("Waypoint Leader completed.")
                : QStringLiteral("Waypoint Leader stopped.")), false);
        return;
    }
    if (tick.hasIntents()) {
        if (m_batch.coreBatchId != 0) {
            fail(QStringLiteral(
                     "The core issued overlapping command batches."),
                 SwarmWaypointLeaderBatchResult::OutcomeUncertain, true);
            return;
        }
        beginBatch(tick);
        return;
    }
    setState(m_batch.coreBatchId == 0
                 ? State::Running : State::WaitingForCompletion);
}

void SwarmWaypointLeaderExecutor::beginBatch(
    const SwarmWaypointLeaderTick &tick)
{
    m_batch.runGeneration = m_runGeneration;
    m_batch.coreBatchId = tick.batchId;
    m_batch.intents = tick.intents;
    m_batch.nextIntent = 0;
    m_batch.cancellationRequested = false;
    dispatchNext();
}

void SwarmWaypointLeaderExecutor::dispatchNext()
{
    if (!m_dispatchAllowed || m_batch.runGeneration != m_runGeneration) {
        return;
    }
    if (m_batch.cancellationRequested) {
        if (!m_pendingParameter.token.isValid()
            && !m_pendingCommand.token.isValid()) {
            completeBatch(SwarmWaypointLeaderBatchResult::Cancelled,
                          QStringLiteral("The command batch was cancelled."));
        }
        return;
    }
    if (m_batch.nextIntent >= m_batch.intents.size()) {
        completeBatch(SwarmWaypointLeaderBatchResult::Succeeded, {});
        return;
    }

    using Kind = SwarmWaypointLeaderCommandIntent::Kind;
    const auto &intent = m_batch.intents.at(m_batch.nextIntent);
    if (intent.kind == Kind::RequestPositionStream) {
        QVector<int> slotIds;
        const int rateHz = intent.streamRateHz;
        int next = m_batch.nextIntent;
        while (next < m_batch.intents.size()
               && m_batch.intents.at(next).kind
                   == Kind::RequestPositionStream
               && m_batch.intents.at(next).streamRateHz == rateHz) {
            const int slot = slotFor(m_batch.intents.at(next).lease);
            if (slot < 0) {
                fail(QStringLiteral(
                         "A stream intent is outside the reserved group."),
                     SwarmWaypointLeaderBatchResult::Rejected);
                return;
            }
            slotIds.append(slot);
            ++next;
        }
        QPointer<SwarmWaypointLeaderExecutor> guard(this);
        ++m_backendDepth;
        const auto report = m_backend->requestPositionStreams(
            m_swarmToken, slotIds, rateHz);
        if (!guard) {
            return;
        }
        --m_backendDepth;
        drainDeferredCallbacks();
        if (!guard) {
            return;
        }
        if (!report.allSent()) {
            const bool uncertain = swarmOutcomeUncertain(report);
            const bool partial = m_batch.anyFrameAttempted
                || report.result == SwarmCommandService::Result::PartialSend
                || std::any_of(
                    report.members.cbegin(), report.members.cend(),
                    [](const auto &member) {
                        return member.framesSent > 0;
                    });
            completeBatch(
                uncertain
                    ? SwarmWaypointLeaderBatchResult::OutcomeUncertain
                    : (partial ? SwarmWaypointLeaderBatchResult::Partial
                               : SwarmWaypointLeaderBatchResult::Rejected),
                detailOr(report.detail, QStringLiteral(
                    "The position-stream batch failed.")));
            return;
        }
        m_batch.anyFrameAttempted = true;
        m_batch.nextIntent = next;
        dispatchNext();
        return;
    }

    if (intent.kind == Kind::PositionTarget) {
        QVector<SwarmPositionTarget> targets;
        int next = m_batch.nextIntent;
        while (next < m_batch.intents.size()
               && m_batch.intents.at(next).kind == Kind::PositionTarget) {
            const auto &source = m_batch.intents.at(next);
            const int slot = slotFor(source.lease);
            if (slot < 0) {
                fail(QStringLiteral(
                         "A target intent is outside the reserved group."),
                     SwarmWaypointLeaderBatchResult::Rejected);
                return;
            }
            SwarmPositionTarget target;
            target.slotId = slot;
            target.latitudeDegrees = source.target.latitudeDegrees;
            target.longitudeDegrees = source.target.longitudeDegrees;
            target.relativeAltitudeM =
                static_cast<float>(source.target.relativeAltitudeM);
            target.velocityNorthMps =
                static_cast<float>(source.velocity.northMps);
            target.velocityEastMps =
                static_cast<float>(source.velocity.eastMps);
            target.velocityDownMps =
                static_cast<float>(source.velocity.downMps);
            target.useVelocity = true;
            targets.append(target);
            ++next;
        }
        QPointer<SwarmWaypointLeaderExecutor> guard(this);
        ++m_backendDepth;
        const auto report = m_backend->sendPositionTargets(
            m_swarmToken, targets,
            SwarmCommandService::PositionTargetPriority::Normal);
        if (!guard) {
            return;
        }
        --m_backendDepth;
        drainDeferredCallbacks();
        if (!guard) {
            return;
        }
        if (!report.allSent()) {
            const bool uncertain = swarmOutcomeUncertain(report);
            const bool partial = m_batch.anyFrameAttempted
                || report.result == SwarmCommandService::Result::PartialSend
                || std::any_of(
                    report.members.cbegin(), report.members.cend(),
                    [](const auto &member) {
                        return member.framesSent > 0;
                    });
            completeBatch(
                uncertain
                    ? SwarmWaypointLeaderBatchResult::OutcomeUncertain
                    : (partial ? SwarmWaypointLeaderBatchResult::Partial
                               : SwarmWaypointLeaderBatchResult::Rejected),
                detailOr(report.detail, QStringLiteral(
                    "The position-target batch failed.")));
            return;
        }
        m_batch.anyFrameAttempted = true;
        m_batch.nextIntent = next;
        dispatchNext();
        return;
    }

    if (intent.kind == Kind::SetParameter) {
        const Capability *capability = capabilityFor(intent.lease);
        const ParameterType runtimeType = capability
            ? capability->types.value(
                  intent.parameterName, ParameterType::Unknown)
            : ParameterType::Unknown;
        if (runtimeType == ParameterType::Unknown) {
            fail(QStringLiteral(
                     "A parameter intent has no exact runtime type."),
                 m_batch.anyFrameAttempted
                     ? SwarmWaypointLeaderBatchResult::Partial
                     : SwarmWaypointLeaderBatchResult::Rejected,
                 m_batch.anyFrameAttempted);
            return;
        }
        ParameterService::ExactWriteRequest request;
        request.name = intent.parameterName;
        request.value = intent.parameterValue;
        request.type = runtimeType;
        request.force = false;
        ParameterService::ExactOperationToken token;
        QString detail;
        m_pendingParameter = {m_runGeneration, {}, false};
        QPointer<SwarmWaypointLeaderExecutor> guard(this);
        ++m_backendDepth;
        const auto result = m_backend->submitParameterWrite(
            m_parameterReservation, intent.lease, request, &token, &detail);
        if (!guard) {
            return;
        }
        --m_backendDepth;
        m_pendingParameter.token = token;
        if (result != ParameterService::ExactSubmitResult::Started
            || !token.isValid()) {
            m_pendingParameter = {};
            drainDeferredCallbacks();
            if (!guard) {
                return;
            }
            const bool uncertain = result
                == ParameterService::ExactSubmitResult::
                    TransportOutcomeUncertain;
            const bool partial = m_batch.anyFrameAttempted;
            fail(detailOr(
                     detail, QStringLiteral(
                         "The exact parameter write was rejected.")),
                 uncertain ? SwarmWaypointLeaderBatchResult::OutcomeUncertain
                           : (partial
                                  ? SwarmWaypointLeaderBatchResult::Partial
                                  : SwarmWaypointLeaderBatchResult::Rejected),
                 uncertain || partial);
            return;
        }
        setState(State::WaitingForCompletion);
        if (!guard) {
            return;
        }
        drainDeferredCallbacks();
        return;
    }

    VehicleCommandService::ExactCommandRequest request;
    switch (intent.kind) {
    case Kind::SetModeGuided:
        request.command = MAV_CMD_DO_SET_MODE;
        request.params[0] = static_cast<float>(
            MAV_MODE_FLAG_CUSTOM_MODE_ENABLED);
        request.params[1] = 4.0F;
        break;
    case Kind::Arm:
        request.command = MAV_CMD_COMPONENT_ARM_DISARM;
        request.params[0] = intent.arm ? 1.0F : 0.0F;
        break;
    case Kind::Takeoff:
        request.command = MAV_CMD_NAV_TAKEOFF;
        request.params[6] = static_cast<float>(intent.takeoffAltitudeM);
        break;
    case Kind::SetModeRtl:
        request.command = MAV_CMD_DO_SET_MODE;
        request.params[0] = static_cast<float>(
            MAV_MODE_FLAG_CUSTOM_MODE_ENABLED);
        request.params[1] = 6.0F;
        break;
    case Kind::RequestPositionStream:
    case Kind::SetParameter:
    case Kind::PositionTarget:
        fail(QStringLiteral("The command intent kind is invalid."),
             SwarmWaypointLeaderBatchResult::Rejected);
        return;
    }

    VehicleCommandService::ExactCommandToken token;
    QString detail;
    m_pendingCommand = {m_runGeneration, {}};
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    ++m_backendDepth;
    const auto result = m_backend->submitCommand(
        m_commandReservation, intent.lease, request, &token, &detail);
    if (!guard) {
        return;
    }
    --m_backendDepth;
    m_pendingCommand.token = token;
    if (result != VehicleCommandService::ExactSubmitResult::Started
        || !token.isValid()) {
        m_pendingCommand = {};
        drainDeferredCallbacks();
        if (!guard) {
            return;
        }
        const bool uncertain = result
            == VehicleCommandService::ExactSubmitResult::
                TransportOutcomeUncertain;
        const bool partial = m_batch.anyFrameAttempted;
        fail(detailOr(
                 detail, QStringLiteral(
                     "The exact vehicle command was rejected.")),
             uncertain ? SwarmWaypointLeaderBatchResult::OutcomeUncertain
                       : (partial
                              ? SwarmWaypointLeaderBatchResult::Partial
                              : SwarmWaypointLeaderBatchResult::Rejected),
             uncertain || partial);
        return;
    }
    setState(State::WaitingForCompletion);
    if (!guard) {
        return;
    }
    drainDeferredCallbacks();
}

void SwarmWaypointLeaderExecutor::dispatchUrgent(
    const SwarmWaypointLeaderTick &tick)
{
    QVector<SwarmPositionTarget> targets;
    for (const auto &intent : tick.intents) {
        if (intent.kind
            != SwarmWaypointLeaderCommandIntent::Kind::PositionTarget) {
            fail(QStringLiteral(
                     "Only position targets may use urgent dispatch."),
                 SwarmWaypointLeaderBatchResult::Rejected);
            return;
        }
        const int slot = slotFor(intent.lease);
        if (slot < 0) {
            fail(QStringLiteral(
                     "An urgent intent is outside the reserved group."),
                 SwarmWaypointLeaderBatchResult::Rejected);
            return;
        }
        SwarmPositionTarget target;
        target.slotId = slot;
        target.latitudeDegrees = intent.target.latitudeDegrees;
        target.longitudeDegrees = intent.target.longitudeDegrees;
        target.relativeAltitudeM =
            static_cast<float>(intent.target.relativeAltitudeM);
        target.velocityNorthMps =
            static_cast<float>(intent.velocity.northMps);
        target.velocityEastMps =
            static_cast<float>(intent.velocity.eastMps);
        target.velocityDownMps =
            static_cast<float>(intent.velocity.downMps);
        target.useVelocity = true;
        targets.append(target);
    }

    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    ++m_backendDepth;
    const auto report = m_backend->sendPositionTargets(
        m_swarmToken, targets,
        SwarmCommandService::PositionTargetPriority::Urgent);
    if (!guard) {
        return;
    }
    --m_backendDepth;
    drainDeferredCallbacks();
    if (!guard) {
        return;
    }
    if (!report.allSent()) {
        const bool uncertain = swarmOutcomeUncertain(report);
        const bool partial = m_batch.anyFrameAttempted
            || report.result == SwarmCommandService::Result::PartialSend
            || std::any_of(
                report.members.cbegin(), report.members.cend(),
                [](const auto &member) {
                    return member.framesSent > 0;
                });
        fail(detailOr(
                 report.detail, QStringLiteral(
                     "The urgent collision target failed.")),
             uncertain
                 ? SwarmWaypointLeaderBatchResult::OutcomeUncertain
                 : (partial ? SwarmWaypointLeaderBatchResult::Partial
                            : SwarmWaypointLeaderBatchResult::Rejected),
             uncertain || partial);
        return;
    }

    if (tick.cancelBatchId == 0) {
        return;
    }
    if (m_batch.coreBatchId != tick.cancelBatchId) {
        fail(QStringLiteral(
                 "Urgent dispatch lost the normal batch identity."),
             SwarmWaypointLeaderBatchResult::OutcomeUncertain, true);
        return;
    }
    m_batch.cancellationRequested = true;
    cancelPendingParameter(QStringLiteral(
        "The normal batch was cancelled for an urgent collision target."));
    if (!guard) {
        return;
    }
    if (!m_pendingParameter.token.isValid()
        && !m_pendingCommand.token.isValid()) {
        completeBatch(SwarmWaypointLeaderBatchResult::Cancelled,
                      QStringLiteral(
                          "The normal batch was cancelled for collision avoidance."));
    }
}

void SwarmWaypointLeaderExecutor::completeBatch(
    SwarmWaypointLeaderBatchResult result, const QString &detail)
{
    if (m_batch.coreBatchId == 0) {
        return;
    }
    if (m_batch.cancellationRequested
        && result != SwarmWaypointLeaderBatchResult::OutcomeUncertain
        && result != SwarmWaypointLeaderBatchResult::Partial) {
        result = SwarmWaypointLeaderBatchResult::Cancelled;
    }
    const quint64 batchId = m_batch.coreBatchId;
    m_batch = {};
    if (!m_core.completeBatch(batchId, result, detail)) {
        beginDrain(QStringLiteral(
            "The executor lost core batch correlation."), true);
        return;
    }
    if (result != SwarmWaypointLeaderBatchResult::Succeeded
        && result != SwarmWaypointLeaderBatchResult::Cancelled) {
        beginDrain(detailOr(
            detail, QStringLiteral("A command batch failed.")),
            result == SwarmWaypointLeaderBatchResult::Partial
                || result == SwarmWaypointLeaderBatchResult::OutcomeUncertain);
        return;
    }
    setState(State::Running);
}

void SwarmWaypointLeaderExecutor::fail(
    const QString &detail, SwarmWaypointLeaderBatchResult result,
    bool uncertain)
{
    if (m_batch.coreBatchId != 0
        && !m_pendingParameter.token.isValid()
        && !m_pendingCommand.token.isValid()) {
        const quint64 id = m_batch.coreBatchId;
        m_batch = {};
        m_core.completeBatch(id, result, detail);
    }
    beginDrain(detail, uncertain
               || result == SwarmWaypointLeaderBatchResult::Partial
               || result == SwarmWaypointLeaderBatchResult::OutcomeUncertain);
}

void SwarmWaypointLeaderExecutor::parameterFinished(
    const ParameterService::ExactOperationReport &report)
{
    if (m_backendDepth > 0) {
        m_deferredParameters.append(report);
        return;
    }
    processParameterFinished(report);
}

void SwarmWaypointLeaderExecutor::processParameterFinished(
    const ParameterService::ExactOperationReport &report)
{
    if (m_pendingParameter.runGeneration != m_runGeneration
        || !sameToken(report.token, m_pendingParameter.token)) {
        return;
    }
    const bool discovery = m_pendingParameter.discovery;
    m_pendingParameter = {};

    using Terminal = ParameterService::ExactTerminalResult;
    const bool uncertain =
        report.terminalResult == Terminal::WriteCancelledOutcomeUncertain
        || report.terminalResult == Terminal::WriteTimedOutOutcomeUncertain
        || report.terminalResult == Terminal::WriteTransportOutcomeUncertain
        || report.terminalResult == Terminal::WriteLeaseRetiredOutcomeUncertain
        || report.terminalResult == Terminal::WriteLinkForgottenOutcomeUncertain;
    if (!m_dispatchAllowed || m_state == State::Draining) {
        m_uncertain = m_uncertain || uncertain;
        finishDrain();
        return;
    }

    if (discovery) {
        if (m_probeIndex < 0 || m_probeIndex >= m_probes.size()) {
            fail(QStringLiteral("Parameter probe correlation was lost."),
                 SwarmWaypointLeaderBatchResult::OutcomeUncertain, true);
            return;
        }
        AliasProbe &probe = m_probes[m_probeIndex];
        if (report.terminalResult == Terminal::ReadSucceeded) {
            if (report.type == ParameterType::Unknown) {
                fail(QStringLiteral(
                         "An exact read returned an unknown runtime type."),
                     SwarmWaypointLeaderBatchResult::Rejected);
                return;
            }
            Capability *capability = capabilityFor(probe.lease);
            if (!capability) {
                fail(QStringLiteral(
                         "A parameter result is outside the reserved group."),
                     SwarmWaypointLeaderBatchResult::Rejected);
                return;
            }
            capability->types.insert(report.token.name, report.type);
            ++m_probeIndex; // A successful primary suppresses its fallback.
            submitNextRead();
            return;
        }
        if (report.terminalResult == Terminal::ReadTimedOut) {
            ++probe.nextAlias;
            if (probe.nextAlias >= probe.aliases.size()) {
                ++m_probeIndex;
            }
            submitNextRead();
            return;
        }
        fail(detailOr(
                 report.description,
                 QStringLiteral("Exact parameter discovery failed.")),
             SwarmWaypointLeaderBatchResult::Rejected);
        return;
    }

    if (report.frameAttempted) {
        m_batch.anyFrameAttempted = true;
    }
    if (report.terminalResult == Terminal::WriteSucceeded
        || report.terminalResult == Terminal::WriteSkipped) {
        if (m_batch.cancellationRequested) {
            completeBatch(
                report.terminalResult == Terminal::WriteSkipped
                    ? SwarmWaypointLeaderBatchResult::Cancelled
                    : SwarmWaypointLeaderBatchResult::Partial,
                QStringLiteral(
                    "The parameter operation completed while its batch was being cancelled."));
            return;
        }
        ++m_batch.nextIntent;
        dispatchNext();
        return;
    }
    if (report.terminalResult == Terminal::WriteCancelled
        && m_batch.cancellationRequested) {
        completeBatch(SwarmWaypointLeaderBatchResult::Cancelled,
                      report.description);
        return;
    }
    completeBatch(
        uncertain ? SwarmWaypointLeaderBatchResult::OutcomeUncertain
                  : (m_batch.anyFrameAttempted
                         ? SwarmWaypointLeaderBatchResult::Partial
                         : SwarmWaypointLeaderBatchResult::Rejected),
        detailOr(report.description,
                 QStringLiteral("The exact parameter write failed.")));
}

void SwarmWaypointLeaderExecutor::commandFinished(
    const VehicleCommandService::ExactCommandReport &report)
{
    if (m_backendDepth > 0) {
        m_deferredCommands.append(report);
        return;
    }
    processCommandFinished(report);
}

void SwarmWaypointLeaderExecutor::processCommandFinished(
    const VehicleCommandService::ExactCommandReport &report)
{
    if (m_pendingCommand.runGeneration != m_runGeneration
        || !sameToken(report.token, m_pendingCommand.token)) {
        return;
    }
    m_pendingCommand = {};
    using Terminal = VehicleCommandService::ExactTerminalResult;
    const bool accepted = report.terminalResult
        == Terminal::AcknowledgedAccepted;
    const bool uncertain =
        report.terminalResult == Terminal::TimedOutOutcomeUncertain
        || report.terminalResult == Terminal::TransportOutcomeUncertain
        || report.terminalResult == Terminal::LeaseRetiredOutcomeUncertain
        || report.terminalResult == Terminal::LinkForgottenOutcomeUncertain;
    if (!m_dispatchAllowed || m_state == State::Draining) {
        m_uncertain = m_uncertain || uncertain;
        finishDrain();
        return;
    }
    if (accepted) {
        m_batch.anyFrameAttempted = m_batch.anyFrameAttempted
            || report.frameAttempted;
        if (m_batch.cancellationRequested) {
            completeBatch(SwarmWaypointLeaderBatchResult::Partial,
                          QStringLiteral(
                              "A command was accepted while its batch was being cancelled."));
            return;
        }
        ++m_batch.nextIntent;
        dispatchNext();
        return;
    }
    completeBatch(
        uncertain ? SwarmWaypointLeaderBatchResult::OutcomeUncertain
                  : (m_batch.anyFrameAttempted
                         ? SwarmWaypointLeaderBatchResult::Partial
                         : SwarmWaypointLeaderBatchResult::Rejected),
        detailOr(report.description,
                 QStringLiteral("The exact command failed.")));
}

void SwarmWaypointLeaderExecutor::parameterReservationReleased(quint64 id)
{
    if (m_backendDepth > 0) {
        m_deferredParameterReleases.append(id);
        return;
    }
    if (m_parametersReserved
        && id == m_parameterReservation.reservationId) {
        m_parametersReserved = false;
        finishDrain();
    }
}

void SwarmWaypointLeaderExecutor::commandReservationReleased(quint64 id)
{
    if (m_backendDepth > 0) {
        m_deferredCommandReleases.append(id);
        return;
    }
    if (m_commandsReserved
        && id == m_commandReservation.reservationId) {
        m_commandsReserved = false;
        finishDrain();
    }
}

void SwarmWaypointLeaderExecutor::swarmSessionCancelled(
    quint64 id, const QString &reason)
{
    if (m_backendDepth > 0) {
        m_deferredSwarmCancellations.append({id, reason});
        return;
    }
    if (!m_swarmReserved || id != m_swarmToken.id) {
        return;
    }
    m_swarmReserved = false;
    if (m_dispatchAllowed) {
        beginDrain(detailOr(
            reason, QStringLiteral("The swarm reservation was cancelled.")),
            false);
    } else {
        finishDrain();
    }
}

void SwarmWaypointLeaderExecutor::drainDeferredCallbacks()
{
    if (m_backendDepth > 0 || m_destroying) {
        return;
    }
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    while (!m_deferredParameters.isEmpty()
           || !m_deferredCommands.isEmpty()
           || !m_deferredParameterReleases.isEmpty()
           || !m_deferredCommandReleases.isEmpty()
           || !m_deferredSwarmCancellations.isEmpty()) {
        const auto parameters = std::exchange(
            m_deferredParameters,
            QVector<ParameterService::ExactOperationReport>());
        const auto commands = std::exchange(
            m_deferredCommands,
            QVector<VehicleCommandService::ExactCommandReport>());
        const auto parameterReleases =
            std::exchange(m_deferredParameterReleases, QVector<quint64>());
        const auto commandReleases =
            std::exchange(m_deferredCommandReleases, QVector<quint64>());
        const auto swarmCancellations =
            std::exchange(
                m_deferredSwarmCancellations,
                QVector<QPair<quint64, QString>>());
        for (const auto &report : parameters) {
            processParameterFinished(report);
            if (!guard) {
                return;
            }
        }
        for (const auto &report : commands) {
            processCommandFinished(report);
            if (!guard) {
                return;
            }
        }
        for (quint64 id : parameterReleases) {
            parameterReservationReleased(id);
            if (!guard) {
                return;
            }
        }
        for (quint64 id : commandReleases) {
            commandReservationReleased(id);
            if (!guard) {
                return;
            }
        }
        for (const auto &cancelled : swarmCancellations) {
            swarmSessionCancelled(cancelled.first, cancelled.second);
            if (!guard) {
                return;
            }
        }
    }
}

void SwarmWaypointLeaderExecutor::cancelActiveRun(const QString &reason)
{
    if (m_state == State::Idle || m_state == State::OutcomeUncertain) {
        return;
    }
    beginDrain(detailOr(
        reason, QStringLiteral("Waypoint Leader was cancelled.")), false);
}

bool SwarmWaypointLeaderExecutor::requestMode(
    SwarmWaypointLeaderMode requested, QString *error)
{
    if (!m_dispatchAllowed
        || (m_state != State::Running
            && m_state != State::WaitingForCompletion)) {
        return setError(error, QStringLiteral(
            "Waypoint Leader is not accepting mode requests."));
    }
    return m_core.requestMode(requested, error);
}

bool SwarmWaypointLeaderExecutor::isRunning() const noexcept
{
    return m_state == State::DiscoveringParameters
        || m_state == State::Running
        || m_state == State::WaitingForCompletion;
}

void SwarmWaypointLeaderExecutor::beginDrain(
    const QString &reason, bool uncertain)
{
    if ((m_state == State::Idle || m_state == State::OutcomeUncertain)
        && !m_swarmReserved && !m_commandsReserved
        && !m_parametersReserved) {
        return;
    }
    m_dispatchAllowed = false;
    m_timer.stop();
    m_drainReason = detailOr(
        reason, QStringLiteral("Waypoint Leader stopped."));
    m_uncertain = m_uncertain || uncertain;
    m_core.reset();
    m_batch.cancellationRequested = true;
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    setState(State::Draining);
    if (!guard) {
        return;
    }
    setStatus(QStringLiteral("%1 Draining exact terminal outcomes.")
                  .arg(m_drainReason));
    if (!guard) {
        return;
    }

    // This call is the synchronous no-further-parameter-send barrier.
    cancelPendingParameter(m_drainReason);
    if (!guard) {
        return;
    }
    releaseReservations();
    if (!guard) {
        return;
    }
    finishDrain();
}

void SwarmWaypointLeaderExecutor::cancelPendingParameter(
    const QString &reason)
{
    if (!m_backend || !m_parametersReserved
        || !m_pendingParameter.token.isValid()) {
        return;
    }
    const auto token = m_pendingParameter.token;
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    ++m_backendDepth;
    const bool cancelled = m_backend->cancelParameterOperation(
        m_parameterReservation, token, reason);
    if (!guard) {
        return;
    }
    --m_backendDepth;
    drainDeferredCallbacks();
    if (!guard) {
        return;
    }
    if (!cancelled && sameToken(token, m_pendingParameter.token)) {
        // A full current token can fail cancellation only if backend state was
        // lost without its terminal report. No retry can be proven stopped.
        m_pendingParameter = {};
        m_uncertain = true;
    }
}

void SwarmWaypointLeaderExecutor::releaseReservations()
{
    if (!m_backend) {
        m_parametersReserved = false;
        m_commandsReserved = false;
        m_swarmReserved = false;
        m_uncertain = true;
        return;
    }

    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    if (m_parametersReserved) {
        ++m_backendDepth;
        const bool accepted =
            m_backend->releaseParameters(m_parameterReservation);
        if (!guard) {
            return;
        }
        --m_backendDepth;
        if (!accepted) {
            m_parametersReserved = false;
            m_uncertain = true;
        }
        drainDeferredCallbacks();
        if (!guard) {
            return;
        }
    }
    if (m_commandsReserved) {
        ++m_backendDepth;
        const bool accepted =
            m_backend->releaseCommands(m_commandReservation);
        if (!guard) {
            return;
        }
        --m_backendDepth;
        if (!accepted) {
            m_commandsReserved = false;
            m_uncertain = true;
        }
        drainDeferredCallbacks();
        if (!guard) {
            return;
        }
    }
    if (m_swarmReserved) {
        ++m_backendDepth;
        const auto result = m_backend->releaseSwarm(m_swarmToken);
        if (!guard) {
            return;
        }
        --m_backendDepth;
        if (result != SwarmCommandService::Result::Cancelled) {
            m_uncertain = true;
        }
        m_swarmReserved = false;
        drainDeferredCallbacks();
    }
}

void SwarmWaypointLeaderExecutor::finishDrain()
{
    if (m_state != State::Draining
        || m_parametersReserved || m_commandsReserved || m_swarmReserved
        || m_pendingParameter.token.isValid()
        || m_pendingCommand.token.isValid()) {
        return;
    }
    m_batch = {};
    m_groupLeases.clear();
    m_flightLeases.clear();
    m_capabilities.clear();
    m_probes.clear();
    m_parameterReservation = {};
    m_commandReservation = {};
    m_swarmToken = {};
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    if (m_uncertain) {
        setState(State::OutcomeUncertain);
        if (!guard) {
            return;
        }
        setStatus(QStringLiteral(
            "%1 Exact outcome is uncertain; restart is blocked.")
                      .arg(m_drainReason));
    } else {
        m_plan = {};
        setState(State::Idle);
        if (!guard) {
            return;
        }
        setStatus(m_drainReason);
    }
}

void SwarmWaypointLeaderExecutor::setState(State state)
{
    if (state == m_state) {
        return;
    }
    const bool wasRunning = isRunning();
    m_state = state;
    const bool nowRunning = isRunning();
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    emit stateChanged(state);
    if (!guard) {
        return;
    }
    if (wasRunning != nowRunning) {
        emit runningChanged(nowRunning);
        if (!guard) {
            return;
        }
    }
    emit changed();
}

void SwarmWaypointLeaderExecutor::setStatus(const QString &status)
{
    const QString value = status.trimmed();
    if (value.isEmpty() || value == m_status) {
        return;
    }
    m_status = value;
    QPointer<SwarmWaypointLeaderExecutor> guard(this);
    emit statusChanged(value);
    if (guard) {
        emit changed();
    }
}
