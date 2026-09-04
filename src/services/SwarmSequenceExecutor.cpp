#include "SwarmSequenceExecutor.h"

#include "comm/SwarmFlightMode.h"
#include "comm/SwarmTelemetryRegistry.h"

#include <QSet>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
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

bool validOrigin(const SwarmSequenceOrigin &origin)
{
    return origin.valid && std::isfinite(origin.latitude)
        && std::isfinite(origin.longitude)
        && origin.latitude >= -90.0 && origin.latitude <= 90.0
        && origin.longitude >= -180.0 && origin.longitude <= 180.0
        && (std::abs(origin.latitude) > std::numeric_limits<double>::epsilon()
            || std::abs(origin.longitude)
                > std::numeric_limits<double>::epsilon());
}

class ProductionBackend final : public SwarmSequenceExecutorBackend
{
public:
    ProductionBackend(SwarmTelemetryRegistry *registry,
                      SwarmCommandService *swarm,
                      VehicleCommandService *commands)
        : m_registry(registry)
        , m_swarm(swarm)
        , m_commands(commands)
    {
        if (m_commands) {
            connect(m_commands.data(),
                    &VehicleCommandService::exactCommandFinished,
                    this, [this](VehicleCommandService::ExactCommandReport report) {
                const auto callback = m_callbacks.commandFinished;
                if (callback) {
                    callback(std::move(report));
                }
            });
            connect(m_commands.data(),
                    &VehicleCommandService::exactReservationReleased,
                    this, [this](quint64 reservationId) {
                const auto callback = m_callbacks.commandReservationReleased;
                if (callback) {
                    callback(reservationId);
                }
            });
        }
        if (m_swarm) {
            connect(m_swarm.data(), &SwarmCommandService::sessionCancelled,
                    this, [this](quint64 sessionId, const QString &reason) {
                const auto callback = m_callbacks.swarmSessionCancelled;
                if (callback) {
                    callback(sessionId, reason);
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
        if (!m_registry || !m_swarm || !m_commands) {
            return setError(error, QStringLiteral(
                "Sequence exact services are unavailable."));
        }
        QThread *const expected = thread();
        if (QThread::currentThread() != expected
            || m_registry->thread() != expected
            || m_swarm->thread() != expected
            || m_commands->thread() != expected) {
            return setError(error, QStringLiteral(
                "Sequence exact services must share one thread."));
        }
        return true;
    }

    qint64 observationClockNowMs() const override
    {
        return m_registry ? m_registry->observationClockNowMs() : -1;
    }

    bool observationIsFresh(qint64 observedMs,
                            int maximumAgeMs) const override
    {
        return m_registry
            && m_registry->observationIsFresh(observedMs, maximumAgeMs);
    }

    bool snapshotForLease(const SwarmVehicleInstanceLease &lease,
                          SwarmTelemetrySnapshot *snapshot) const override
    {
        return m_registry && snapshot
            && m_registry->snapshotForLease(lease, snapshot);
    }

    bool routeIsEligible(const SwarmVehicleInstanceLease &lease,
                         QString *error) const override
    {
        return m_swarm && m_swarm->routeIsEligible(lease, error);
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
        const QVector<SwarmPositionTarget> &targets) override
    {
        return m_swarm ? m_swarm->sendPositionTargets(token, targets)
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

private:
    QPointer<SwarmTelemetryRegistry> m_registry;
    QPointer<SwarmCommandService> m_swarm;
    QPointer<VehicleCommandService> m_commands;
    Callbacks m_callbacks;
};

} // namespace

SwarmSequenceExecutor::SwarmSequenceExecutor(
    SwarmTelemetryRegistry *registry,
    SwarmCommandService *swarmCommands,
    VehicleCommandService *commands,
    QObject *parent)
    : SwarmSequenceExecutor(
          new ProductionBackend(registry, swarmCommands, commands), parent)
{
    if (m_backend) {
        m_backend->setParent(this);
    }
}

SwarmSequenceExecutor::SwarmSequenceExecutor(
    SwarmSequenceExecutorBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    m_heartbeatTimer.setObjectName(QStringLiteral(
        "SwarmSequenceHeartbeatBarrierTimer"));
    m_heartbeatTimer.setInterval(100);
    m_heartbeatTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_heartbeatTimer, &QTimer::timeout,
            this, &SwarmSequenceExecutor::heartbeatPoll);

    if (m_backend) {
        QPointer<SwarmSequenceExecutor> guard(this);
        SwarmSequenceExecutorBackend::Callbacks callbacks;
        callbacks.commandFinished = [guard](auto report) {
            if (guard) {
                guard->commandFinished(report);
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

SwarmSequenceExecutor::~SwarmSequenceExecutor()
{
    m_destroying = true;
    m_dispatchAllowed = false;
    m_heartbeatTimer.stop();
    if (!m_backend) {
        return;
    }
    m_backend->setCallbacks({});
    if (m_commandsReserved) {
        m_backend->releaseCommands(m_commandReservation);
    }
    if (m_swarmReserved) {
        m_backend->releaseSwarm(m_swarmToken);
    }
}

quint64 SwarmSequenceExecutor::nextPreparationId()
{
    static std::atomic<quint64> next{0};
    quint64 result = ++next;
    if (result == 0) {
        result = ++next;
    }
    return result;
}

quint64 SwarmSequenceExecutor::nextOperationGeneration()
{
    static std::atomic<quint64> next{0};
    quint64 result = ++next;
    if (result == 0) {
        result = ++next;
    }
    return result;
}

bool SwarmSequenceExecutor::sameCommandToken(
    const VehicleCommandService::ExactCommandToken &left,
    const VehicleCommandService::ExactCommandToken &right)
{
    return left.transactionId != 0
        && left.transactionId == right.transactionId
        && left.reservationId == right.reservationId
        && left.lease.sameInstance(right.lease)
        && left.command == right.command;
}

bool SwarmSequenceExecutor::isCopter(
    const SwarmTelemetrySnapshot &snapshot)
{
    return snapshot.heartbeatValid
        && snapshot.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA
        && SwarmFlightMode::isCopter(snapshot.vehicleType);
}

bool SwarmSequenceExecutor::batchOutcomeUncertain(
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

bool SwarmSequenceExecutor::executorReady(QString *error) const
{
    if (error) {
        error->clear();
    }
    if (m_state == State::OutcomeUncertain) {
        return setError(error, QStringLiteral(
            "A prior Sequence command outcome is uncertain; restart is blocked."));
    }
    if (m_state == State::PartialEffect) {
        return setError(error, QStringLiteral(
            "A prior Sequence operation had a partial vehicle effect; restart is blocked."));
    }
    if (m_state != State::Idle || m_finishing
        || m_operation != SwarmSequenceOperation::None
        || m_swarmReserved || m_commandsReserved
        || m_completionPending || m_pendingCommand.token.isValid()) {
        return setError(error, QStringLiteral(
            "A Sequence operation is active or still draining."));
    }
    if (!m_backend) {
        return setError(error, QStringLiteral(
            "The Sequence executor backend is unavailable."));
    }
    if (QThread::currentThread() != thread()
        || m_backend->thread() != thread()) {
        return setError(error, QStringLiteral(
            "The Sequence executor and backend must share one thread."));
    }
    return m_backend->ready(error);
}

bool SwarmSequenceExecutor::validateVehicle(
    const SwarmVehicleInstanceLease &lease,
    bool requirePosition, bool requireGuided,
    SwarmTelemetrySnapshot *snapshot, QString *error) const
{
    if (snapshot) {
        *snapshot = SwarmTelemetrySnapshot();
    }
    if (!m_backend || !snapshot || !lease.isValid()
        || !m_backend->snapshotForLease(lease, snapshot)
        || !snapshot->lease.sameInstance(lease)) {
        return setError(error, QStringLiteral(
            "A Sequence exact vehicle instance is unavailable or stale."));
    }
    if (!isCopter(*snapshot)) {
        return setError(error, QStringLiteral(
            "Sequence flight requires current ArduCopter autopilots."));
    }
    if (!m_backend->observationIsFresh(
            snapshot->heartbeatObservedMs, MaximumHeartbeatAgeMs)) {
        return setError(error, QStringLiteral(
            "A Sequence vehicle heartbeat is unavailable or stale."));
    }
    if (requirePosition
        && (!snapshot->positionValid
            || !m_backend->observationIsFresh(
                snapshot->positionObservedMs, MaximumPositionAgeMs))) {
        return setError(error, QStringLiteral(
            "A Sequence vehicle position is unavailable or stale."));
    }
    if (requireGuided && !SwarmFlightMode::isExactGuided(*snapshot)) {
        return setError(error, QStringLiteral(
            "Every Run Step vehicle must already be in exact ArduCopter GUIDED mode."));
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool SwarmSequenceExecutor::canonicalRunStep(
    const SwarmSequenceRunStepRequest &request, bool captureOrigin,
    SwarmSequencePreparedRunStep *prepared, QString *error) const
{
    if (prepared) {
        *prepared = SwarmSequencePreparedRunStep();
    }
    if (!prepared || !m_backend || request.layoutId.trimmed().isEmpty()
        || !request.anchor.isValid() || request.assignments.isEmpty()
        || request.assignments.size()
            > SwarmTelemetryRegistry::MaximumVehicleEndpoints) {
        return setError(error, QStringLiteral(
            "The Sequence Run Step plan is incomplete or too large."));
    }

    SwarmTelemetrySnapshot anchorSnapshot;
    if (!validateVehicle(request.anchor, true, false,
                         &anchorSnapshot, error)) {
        return false;
    }

    SwarmSequenceOrigin origin = request.origin;
    if (!origin.valid && captureOrigin) {
        origin.valid = true;
        origin.latitude = anchorSnapshot.latitudeDegrees;
        origin.longitude = anchorSnapshot.longitudeDegrees;
    }
    if (!validOrigin(origin)) {
        return setError(error, QStringLiteral(
            "The captured Sequence origin is invalid."));
    }

    QVector<SwarmSequenceExactAssignment> assignments = request.assignments;
    std::sort(assignments.begin(), assignments.end(),
              [](const auto &left, const auto &right) {
        return left.systemId < right.systemId;
    });
    QSet<int> systems;
    QSet<VehicleEndpoint> endpoints;
    SwarmSequenceLayout layout;
    layout.id = request.layoutId;
    for (const auto &assignment : assignments) {
        if (assignment.systemId < 1 || assignment.systemId > 255
            || systems.contains(assignment.systemId)
            || !assignment.lease.isValid()
            || endpoints.contains(assignment.lease.endpoint)) {
            return setError(error, QStringLiteral(
                "Sequence assignments must have unique system ids and exact endpoints."));
        }
        systems.insert(assignment.systemId);
        endpoints.insert(assignment.lease.endpoint);
        layout.offsets.insert(assignment.systemId, assignment.offset);

        SwarmTelemetrySnapshot snapshot;
        if (!validateVehicle(assignment.lease, true, true,
                             &snapshot, error)) {
            return false;
        }
        QString routeError;
        if (!m_backend->routeIsEligible(assignment.lease, &routeError)) {
            return setError(error, detailOr(
                routeError, QStringLiteral(
                    "A Sequence assignment has no exact writable route.")));
        }
    }

    SwarmSequenceDocument document;
    document.layouts.append(layout);
    const SwarmSequenceIssue validation = SwarmSequenceFile::validate(document);
    if (!validation) {
        return setError(error, validation.message);
    }

    QVector<SwarmSequenceTarget> targets;
    targets.reserve(assignments.size());
    for (const auto &assignment : assignments) {
        SwarmSequenceGeodeticPoint projected;
        const SwarmSequenceIssue projectedResult =
            SwarmSequenceGeometry::projectEastNorth(
                origin.latitude, origin.longitude,
                assignment.offset, &projected);
        if (!projectedResult) {
            return setError(error, projectedResult.message);
        }
        if (std::abs(projected.latitude)
                <= std::numeric_limits<double>::epsilon()
            && std::abs(projected.longitude)
                <= std::numeric_limits<double>::epsilon()) {
            return setError(error, QStringLiteral(
                "A Sequence target cannot be the invalid zero coordinate."));
        }
        SwarmSequenceTarget target;
        target.systemId = assignment.systemId;
        target.lease = assignment.lease;
        target.latitude = projected.latitude;
        target.longitude = projected.longitude;
        target.relativeAltitudeM = projected.altitudeM;
        targets.append(target);
    }

    prepared->preparationId = nextPreparationId();
    prepared->layoutId = request.layoutId;
    prepared->anchor = request.anchor;
    prepared->origin = origin;
    prepared->assignments = assignments;
    prepared->targets = targets;
    if (error) {
        error->clear();
    }
    return true;
}

bool SwarmSequenceExecutor::canonicalPreparedRunStep(
    const SwarmSequencePreparedRunStep &input,
    SwarmSequencePreparedRunStep *prepared, QString *error) const
{
    if (!input.isValid()) {
        return setError(error, QStringLiteral(
            "The prepared Sequence Run Step plan is invalid."));
    }
    SwarmSequenceRunStepRequest request;
    request.layoutId = input.layoutId;
    request.anchor = input.anchor;
    request.origin = input.origin;
    request.assignments = input.assignments;
    if (!canonicalRunStep(request, false, prepared, error)) {
        return false;
    }
    prepared->preparationId = input.preparationId;
    return true;
}

bool SwarmSequenceExecutor::canonicalTakeoff(
    const QVector<SwarmSequenceTakeoffAssignment> &requested,
    SwarmSequencePreparedTakeoff *prepared, QString *error) const
{
    if (prepared) {
        *prepared = SwarmSequencePreparedTakeoff();
    }
    if (!prepared || !m_backend || requested.isEmpty()
        || requested.size()
            > SwarmTelemetryRegistry::MaximumVehicleEndpoints) {
        return setError(error, QStringLiteral(
            "Assign between 1 and 24 exact Copters before Sequence takeoff."));
    }
    QVector<SwarmSequenceTakeoffAssignment> assignments = requested;
    std::sort(assignments.begin(), assignments.end(),
              [](const auto &left, const auto &right) {
        return left.systemId < right.systemId;
    });
    QSet<int> systems;
    QSet<VehicleEndpoint> endpoints;
    for (const auto &assignment : assignments) {
        if (assignment.systemId < 1 || assignment.systemId > 255
            || systems.contains(assignment.systemId)
            || !assignment.lease.isValid()
            || endpoints.contains(assignment.lease.endpoint)) {
            return setError(error, QStringLiteral(
                "Sequence takeoff assignments must be unique exact endpoints."));
        }
        systems.insert(assignment.systemId);
        endpoints.insert(assignment.lease.endpoint);
        SwarmTelemetrySnapshot snapshot;
        if (!validateVehicle(assignment.lease, false, false,
                             &snapshot, error)) {
            return false;
        }
        QString routeError;
        if (!m_backend->routeIsEligible(assignment.lease, &routeError)) {
            return setError(error, detailOr(
                routeError, QStringLiteral(
                    "A Sequence takeoff vehicle has no exact writable route.")));
        }
    }
    prepared->preparationId = nextPreparationId();
    prepared->assignments = assignments;
    prepared->altitudeM = TakeoffAltitudeM;
    if (error) {
        error->clear();
    }
    return true;
}

bool SwarmSequenceExecutor::prepareRunStep(
    const SwarmSequenceRunStepRequest &request,
    SwarmSequencePreparedRunStep *prepared, QString *error) const
{
    if (!executorReady(error)) {
        if (prepared) {
            *prepared = SwarmSequencePreparedRunStep();
        }
        return false;
    }
    return canonicalRunStep(request, true, prepared, error);
}

bool SwarmSequenceExecutor::prepareTakeoff(
    const QVector<SwarmSequenceTakeoffAssignment> &assignments,
    SwarmSequencePreparedTakeoff *prepared, QString *error) const
{
    if (!executorReady(error)) {
        if (prepared) {
            *prepared = SwarmSequencePreparedTakeoff();
        }
        return false;
    }
    return canonicalTakeoff(assignments, prepared, error);
}

QVector<SwarmCommandMember> SwarmSequenceExecutor::runMembers(
    const SwarmSequencePreparedRunStep &prepared) const
{
    QVector<SwarmCommandMember> result;
    result.reserve(prepared.assignments.size());
    for (const auto &assignment : prepared.assignments) {
        SwarmCommandMember member;
        member.slotId = assignment.systemId;
        member.lease = assignment.lease;
        member.required.fields = SwarmTelemetryRequirements::Position;
        member.required.heartbeatMaximumAgeMs = MaximumHeartbeatAgeMs;
        member.required.positionMaximumAgeMs = MaximumPositionAgeMs;
        member.flightMode =
            SwarmCommandMember::FlightModeRequirement::ArduPilotGuided;
        result.append(member);
    }
    return result;
}

QVector<SwarmCommandMember> SwarmSequenceExecutor::takeoffMembers(
    const SwarmSequencePreparedTakeoff &prepared) const
{
    QVector<SwarmCommandMember> result;
    result.reserve(prepared.assignments.size());
    for (const auto &assignment : prepared.assignments) {
        SwarmCommandMember member;
        member.slotId = assignment.systemId;
        member.lease = assignment.lease;
        member.required.fields = SwarmTelemetryRequirements::NoFields;
        member.required.heartbeatMaximumAgeMs = MaximumHeartbeatAgeMs;
        member.flightMode = SwarmCommandMember::FlightModeRequirement::Any;
        result.append(member);
    }
    return result;
}

QList<SwarmVehicleInstanceLease> SwarmSequenceExecutor::runLeases(
    const SwarmSequencePreparedRunStep &prepared) const
{
    QList<SwarmVehicleInstanceLease> result;
    for (const auto &assignment : prepared.assignments) {
        result.append(assignment.lease);
    }
    return result;
}

QList<SwarmVehicleInstanceLease> SwarmSequenceExecutor::takeoffLeases(
    const SwarmSequencePreparedTakeoff &prepared) const
{
    QList<SwarmVehicleInstanceLease> result;
    for (const auto &assignment : prepared.assignments) {
        result.append(assignment.lease);
    }
    return result;
}

bool SwarmSequenceExecutor::beginReservations(
    const QVector<SwarmCommandMember> &members,
    const QList<SwarmVehicleInstanceLease> &leases,
    QString *error)
{
    QString detail;
    QPointer<SwarmSequenceExecutor> guard(this);
    ++m_backendDepth;
    const auto swarmResult = m_backend->reserveSwarm(
        this, members, StreamRateHz, &m_swarmToken, &detail);
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
        const QString failure = detailOr(
            detail, QStringLiteral("The exact swarm group is busy."));
        beginDrain(SwarmSequenceOperationResult::Rejected, failure);
        return setError(error, failure);
    }

    ++m_backendDepth;
    const auto commandResult = m_backend->reserveCommands(
        this, leases, &m_commandReservation, &detail);
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
    if (!m_swarmReserved || !m_commandsReserved) {
        const QString failure = !m_swarmReserved
            ? QStringLiteral(
                  "The exact swarm reservation changed while command ownership was reserved.")
            : detailOr(
                  detail,
                  QStringLiteral("The exact command reservation failed."));
        beginDrain(SwarmSequenceOperationResult::Rejected, failure);
        return setError(error, failure);
    }
    return true;
}

bool SwarmSequenceExecutor::runStep(
    const SwarmSequencePreparedRunStep &input, QString *error)
{
    if (!executorReady(error)) {
        return false;
    }
    SwarmSequencePreparedRunStep prepared;
    if (!canonicalPreparedRunStep(input, &prepared, error)) {
        return false;
    }

    m_generation = nextOperationGeneration();
    m_operation = SwarmSequenceOperation::RunStep;
    m_runPlan = prepared;
    m_takeoffPlan = {};
    m_workingReport = {};
    m_workingReport.operationGeneration = m_generation;
    m_workingReport.operation = m_operation;
    m_workingReport.origin = prepared.origin;
    m_workingReport.targets = prepared.targets;
    for (const auto &assignment : prepared.assignments) {
        SwarmSequenceVehicleResult vehicle;
        vehicle.systemId = assignment.systemId;
        vehicle.lease = assignment.lease;
        m_workingReport.vehicles.append(vehicle);
    }
    m_completionPending = false;
    m_commandReleaseRequested = false;

    QPointer<SwarmSequenceExecutor> guard(this);
    if (!beginReservations(runMembers(prepared), runLeases(prepared), error)
        || !guard) {
        return false;
    }

    SwarmSequencePreparedRunStep revalidated;
    QString detail;
    ++m_backendDepth;
    const bool stillValid = canonicalPreparedRunStep(
        prepared, &revalidated, &detail);
    if (!guard) {
        return false;
    }
    --m_backendDepth;
    drainDeferredCallbacks();
    if (!guard) {
        return false;
    }
    if (!m_swarmReserved || !m_commandsReserved
        || m_operation != SwarmSequenceOperation::RunStep
        || m_state == State::Draining) {
        const QString failure = QStringLiteral(
            "The exact Run Step reservations changed during revalidation.");
        beginDrain(SwarmSequenceOperationResult::Rejected, failure);
        return setError(error, failure);
    }
    if (!stillValid) {
        beginDrain(SwarmSequenceOperationResult::Rejected,
                   detailOr(detail, QStringLiteral(
                       "The exact Run Step plan changed while services were reserved.")));
        return setError(error, detailOr(detail, QStringLiteral(
            "The exact Run Step plan changed while services were reserved.")));
    }
    m_runPlan = revalidated;
    m_dispatchAllowed = true;
    const quint64 operationGeneration = m_generation;
    setState(State::RunningStep);
    if (!guard) {
        return false;
    }
    if (m_generation != operationGeneration || !m_dispatchAllowed
        || m_state != State::RunningStep
        || !m_swarmReserved || !m_commandsReserved) {
        return setError(error, QStringLiteral(
            "Sequence Run Step was cancelled before dispatch."));
    }
    setStatus(QStringLiteral("Sending exact Sequence Run Step."));
    if (!guard) {
        return false;
    }
    if (m_generation != operationGeneration || !m_dispatchAllowed
        || m_state != State::RunningStep
        || !m_swarmReserved || !m_commandsReserved) {
        return setError(error, QStringLiteral(
            "Sequence Run Step was cancelled before dispatch."));
    }
    dispatchRunStep();
    return true;
}

bool SwarmSequenceExecutor::startTakeoff(
    const SwarmSequencePreparedTakeoff &input, QString *error)
{
    if (!executorReady(error)) {
        return false;
    }
    if (!input.isValid()
        || !std::isfinite(input.altitudeM)
        || std::abs(input.altitudeM - TakeoffAltitudeM) > 1.0e-9) {
        return setError(error, QStringLiteral(
            "The prepared Sequence takeoff plan is invalid."));
    }
    SwarmSequencePreparedTakeoff prepared;
    if (!canonicalTakeoff(input.assignments, &prepared, error)) {
        return false;
    }
    prepared.preparationId = input.preparationId;

    m_generation = nextOperationGeneration();
    m_operation = SwarmSequenceOperation::Takeoff;
    m_runPlan = {};
    m_takeoffPlan = prepared;
    m_workingReport = {};
    m_workingReport.operationGeneration = m_generation;
    m_workingReport.operation = m_operation;
    for (const auto &assignment : prepared.assignments) {
        SwarmSequenceVehicleResult vehicle;
        vehicle.systemId = assignment.systemId;
        vehicle.lease = assignment.lease;
        m_workingReport.vehicles.append(vehicle);
    }
    m_takeoffVehicleIndex = 0;
    m_takeoffPhase = TakeoffPhase::Guided;
    m_completionPending = false;
    m_commandReleaseRequested = false;

    QPointer<SwarmSequenceExecutor> guard(this);
    if (!beginReservations(
            takeoffMembers(prepared), takeoffLeases(prepared), error)
        || !guard) {
        return false;
    }

    SwarmSequencePreparedTakeoff revalidated;
    QString detail;
    ++m_backendDepth;
    const bool stillValid = canonicalTakeoff(
        prepared.assignments, &revalidated, &detail);
    if (!guard) {
        return false;
    }
    --m_backendDepth;
    drainDeferredCallbacks();
    if (!guard) {
        return false;
    }
    if (!m_swarmReserved || !m_commandsReserved
        || m_operation != SwarmSequenceOperation::Takeoff
        || m_state == State::Draining) {
        const QString failure = QStringLiteral(
            "The exact takeoff reservations changed during revalidation.");
        beginDrain(SwarmSequenceOperationResult::Rejected, failure);
        return setError(error, failure);
    }
    if (!stillValid) {
        beginDrain(SwarmSequenceOperationResult::Rejected,
                   detailOr(detail, QStringLiteral(
                       "The exact takeoff group changed while services were reserved.")));
        return setError(error, detailOr(detail, QStringLiteral(
            "The exact takeoff group changed while services were reserved.")));
    }
    revalidated.preparationId = prepared.preparationId;
    m_takeoffPlan = revalidated;
    m_dispatchAllowed = true;
    const quint64 operationGeneration = m_generation;
    setState(State::TakingOff);
    if (!guard) {
        return false;
    }
    if (m_generation != operationGeneration || !m_dispatchAllowed
        || m_state != State::TakingOff
        || !m_swarmReserved || !m_commandsReserved) {
        return setError(error, QStringLiteral(
            "Sequence takeoff was cancelled before dispatch."));
    }
    setStatus(QStringLiteral(
        "Starting the exact GUIDED, ARM and TAKEOFF sequence."));
    if (!guard) {
        return false;
    }
    if (m_generation != operationGeneration || !m_dispatchAllowed
        || m_state != State::TakingOff
        || !m_swarmReserved || !m_commandsReserved) {
        return setError(error, QStringLiteral(
            "Sequence takeoff was cancelled before dispatch."));
    }
    beginTakeoffVehicle();
    return true;
}

void SwarmSequenceExecutor::dispatchRunStep()
{
    if (!m_dispatchAllowed || m_operation != SwarmSequenceOperation::RunStep
        || !m_swarmReserved || !m_commandsReserved) {
        return;
    }
    SwarmTelemetrySnapshot anchorSnapshot;
    QString anchorError;
    if (!validateVehicle(m_runPlan.anchor, true, false,
                         &anchorSnapshot, &anchorError)) {
        beginDrain(SwarmSequenceOperationResult::Rejected,
                   detailOr(anchorError, QStringLiteral(
                       "The exact Sequence anchor became stale before dispatch.")));
        return;
    }
    QVector<int> slotIds;
    for (const auto &assignment : m_runPlan.assignments) {
        slotIds.append(assignment.systemId);
    }

    QPointer<SwarmSequenceExecutor> guard(this);
    ++m_backendDepth;
    const auto streamReport = m_backend->requestPositionStreams(
        m_swarmToken, slotIds, StreamRateHz);
    if (!guard) {
        return;
    }
    --m_backendDepth;
    drainDeferredCallbacks();
    if (!guard || !m_dispatchAllowed) {
        return;
    }
    if (!streamReport.allSent()) {
        // A stream-rate request is setup, not a flight-control effect.  Do not
        // let its protocol frames turn a pre-target failure into Partial or
        // block a safe retry.
        beginDrain(SwarmSequenceOperationResult::Rejected, detailOr(
            streamReport.detail,
            QStringLiteral("The Sequence position-stream request failed.")));
        return;
    }

    QVector<SwarmPositionTarget> targets;
    targets.reserve(m_runPlan.targets.size());
    for (const auto &source : m_runPlan.targets) {
        SwarmPositionTarget target;
        target.slotId = source.systemId;
        target.latitudeDegrees = source.latitude;
        target.longitudeDegrees = source.longitude;
        target.relativeAltitudeM =
            static_cast<float>(source.relativeAltitudeM);
        target.velocityNorthMps = 0.0F;
        target.velocityEastMps = 0.0F;
        target.velocityDownMps = 0.0F;
        target.useVelocity = true;
        targets.append(target);
    }

    ++m_backendDepth;
    const auto targetReport = m_backend->sendPositionTargets(
        m_swarmToken, targets);
    if (!guard) {
        return;
    }
    --m_backendDepth;
    accumulateBatch(targetReport);
    for (const auto &member : targetReport.members) {
        for (auto &vehicle : m_workingReport.vehicles) {
            if (vehicle.systemId == member.slotId
                && vehicle.lease.sameInstance(member.lease)) {
                vehicle.targetSent = member.framesSent > 0;
                vehicle.detail = member.detail;
            }
        }
    }
    drainDeferredCallbacks();
    if (!guard || !m_dispatchAllowed) {
        return;
    }
    if (!targetReport.allSent()) {
        beginDrain(failedBatchResult(targetReport), detailOr(
            targetReport.detail,
            QStringLiteral("The Sequence position-target batch failed.")));
        return;
    }
    complete(SwarmSequenceOperationResult::SentAll,
             QStringLiteral("Sequence layout '%1' was queued to %2 exact vehicle(s).")
                 .arg(m_runPlan.layoutId)
                 .arg(m_runPlan.targets.size()));
}

void SwarmSequenceExecutor::beginTakeoffVehicle()
{
    if (!m_dispatchAllowed
        || m_operation != SwarmSequenceOperation::Takeoff) {
        return;
    }
    if (m_takeoffVehicleIndex >= m_takeoffPlan.assignments.size()) {
        const int takeoffAccepted = int(std::count_if(
            m_workingReport.vehicles.cbegin(),
            m_workingReport.vehicles.cend(),
            [](const SwarmSequenceVehicleResult &vehicle) {
                return vehicle.takeoffAccepted;
            }));
        const bool allAccepted = std::all_of(
            m_workingReport.vehicles.cbegin(),
            m_workingReport.vehicles.cend(),
            [](const SwarmSequenceVehicleResult &vehicle) {
                return vehicle.takeoffAccepted;
            });
        const bool anyAccepted = std::any_of(
            m_workingReport.vehicles.cbegin(),
            m_workingReport.vehicles.cend(),
            [](const SwarmSequenceVehicleResult &vehicle) {
                return vehicle.guidedAccepted || vehicle.armAccepted
                    || vehicle.takeoffAccepted;
            });
        complete(allAccepted ? SwarmSequenceOperationResult::SentAll
                             : (anyAccepted
                                    ? SwarmSequenceOperationResult::Partial
                                    : SwarmSequenceOperationResult::Rejected),
                 allAccepted
                     ? QStringLiteral(
                           "Sequence takeoff was accepted by %1 exact vehicle(s).")
                           .arg(m_takeoffPlan.assignments.size())
                     : QStringLiteral(
                           "Sequence takeoff completed with known results: %1 accepted takeoff, %2 failed; no unknown ACK outcomes remain.")
                           .arg(takeoffAccepted)
                           .arg(m_takeoffPlan.assignments.size()
                                - takeoffAccepted));
        return;
    }
    m_takeoffPhase = TakeoffPhase::Guided;
    submitTakeoffCommand(m_takeoffPhase);
}

void SwarmSequenceExecutor::submitTakeoffCommand(TakeoffPhase phase)
{
    if (!m_dispatchAllowed
        || m_operation != SwarmSequenceOperation::Takeoff
        || m_pendingCommand.token.isValid()
        || m_takeoffVehicleIndex < 0
        || m_takeoffVehicleIndex >= m_takeoffPlan.assignments.size()) {
        return;
    }

    SwarmSequencePreparedTakeoff currentGroup;
    QString detail;
    QPointer<SwarmSequenceExecutor> guard(this);
    ++m_backendDepth;
    const bool groupValid = canonicalTakeoff(
        m_takeoffPlan.assignments, &currentGroup, &detail);
    if (!guard) {
        return;
    }
    --m_backendDepth;
    drainDeferredCallbacks();
    if (!guard || !m_dispatchAllowed || !m_swarmReserved
        || !m_commandsReserved) {
        return;
    }
    if (!groupValid) {
        beginDrain(SwarmSequenceOperationResult::OutcomeUncertain,
                   detailOr(detail, QStringLiteral(
                       "The exact takeoff group became stale.")));
        return;
    }
    const auto &assignment =
        m_takeoffPlan.assignments.at(m_takeoffVehicleIndex);
    SwarmTelemetrySnapshot snapshot;
    if (!validateVehicle(assignment.lease, false, false,
                         &snapshot, &detail)) {
        beginDrain(SwarmSequenceOperationResult::OutcomeUncertain,
                   detailOr(detail, QStringLiteral(
                       "The exact takeoff vehicle became stale.")));
        return;
    }
    if ((phase == TakeoffPhase::Arm || phase == TakeoffPhase::Takeoff)
        && !SwarmFlightMode::isExactGuided(snapshot)) {
        beginDrain(SwarmSequenceOperationResult::OutcomeUncertain,
                   QStringLiteral(
                       "A vehicle left GUIDED during the takeoff sequence."));
        return;
    }
    if (phase == TakeoffPhase::Takeoff && !snapshot.armed) {
        beginDrain(SwarmSequenceOperationResult::OutcomeUncertain,
                   QStringLiteral(
                       "A vehicle was not armed after its accepted ARM command."));
        return;
    }

    VehicleCommandService::ExactCommandRequest request;
    switch (phase) {
    case TakeoffPhase::Guided:
        request.command = MAV_CMD_DO_SET_MODE;
        request.params[0] = static_cast<float>(
            MAV_MODE_FLAG_CUSTOM_MODE_ENABLED);
        request.params[1] = 4.0F;
        break;
    case TakeoffPhase::Arm:
        request.command = MAV_CMD_COMPONENT_ARM_DISARM;
        request.params[0] = 1.0F;
        break;
    case TakeoffPhase::Takeoff:
        request.command = MAV_CMD_NAV_TAKEOFF;
        request.params[6] = static_cast<float>(TakeoffAltitudeM);
        break;
    }

    m_takeoffPhase = phase;
    m_heartbeatBaselineMs = snapshot.heartbeatObservedMs;
    m_pendingCommand.generation = m_generation;
    m_pendingCommand.phase = phase;
    m_pendingCommand.vehicleIndex = m_takeoffVehicleIndex;
    m_pendingCommand.token = {};
    VehicleCommandService::ExactCommandToken token;
    ++m_backendDepth;
    const auto result = m_backend->submitCommand(
        m_commandReservation, assignment.lease, request, &token, &detail);
    if (!guard) {
        return;
    }
    --m_backendDepth;
    m_pendingCommand.token = token;
    drainDeferredCallbacks();
    if (!guard || !m_dispatchAllowed) {
        return;
    }
    if (result != VehicleCommandService::ExactSubmitResult::Started
        || !token.isValid()) {
        m_pendingCommand = {};
        if (result
            == VehicleCommandService::ExactSubmitResult::
                TransportOutcomeUncertain) {
            beginDrain(SwarmSequenceOperationResult::OutcomeUncertain,
                       detailOr(detail, QStringLiteral(
                           "The exact takeoff command transport is uncertain.")));
        } else {
            rejectCurrentTakeoffVehicle(detailOr(
                detail, QStringLiteral(
                    "The exact takeoff command was rejected before submission.")));
        }
    }
}

void SwarmSequenceExecutor::beginHeartbeatBarrier(
    HeartbeatExpectation expectation, qint64 baselineHeartbeatMs)
{
    if (!m_dispatchAllowed || !m_backend
        || expectation == HeartbeatExpectation::None) {
        return;
    }
    m_heartbeatExpectation = expectation;
    m_heartbeatBaselineMs = baselineHeartbeatMs;
    const qint64 now = m_backend->observationClockNowMs();
    m_heartbeatDeadlineMs = now < 0
        ? HeartbeatBarrierTimeoutMs : now + HeartbeatBarrierTimeoutMs;
    QPointer<SwarmSequenceExecutor> guard(this);
    setState(State::WaitingForHeartbeat);
    if (!guard || !m_dispatchAllowed) {
        return;
    }
    setStatus(expectation == HeartbeatExpectation::Guided
        ? QStringLiteral(
              "Waiting for a newer exact heartbeat confirming GUIDED.")
        : QStringLiteral(
              "Waiting for a newer exact heartbeat confirming armed state."));
    if (!guard || !m_dispatchAllowed) {
        return;
    }
    m_heartbeatTimer.start();
}

void SwarmSequenceExecutor::heartbeatPoll()
{
    if (!m_dispatchAllowed || !m_backend
        || m_state != State::WaitingForHeartbeat
        || m_heartbeatExpectation == HeartbeatExpectation::None
        || m_takeoffVehicleIndex < 0
        || m_takeoffVehicleIndex >= m_takeoffPlan.assignments.size()) {
        return;
    }
    const qint64 now = m_backend->observationClockNowMs();
    if (now < 0 || now >= m_heartbeatDeadlineMs) {
        beginDrain(SwarmSequenceOperationResult::OutcomeUncertain,
                   QStringLiteral(
                       "An accepted takeoff command was not confirmed by a newer heartbeat."));
        return;
    }

    const auto &assignment =
        m_takeoffPlan.assignments.at(m_takeoffVehicleIndex);
    SwarmTelemetrySnapshot snapshot;
    QString detail;
    if (!validateVehicle(assignment.lease, false, false,
                         &snapshot, &detail)) {
        beginDrain(SwarmSequenceOperationResult::OutcomeUncertain,
                   detailOr(detail, QStringLiteral(
                       "The vehicle became stale during heartbeat confirmation.")));
        return;
    }
    if (snapshot.heartbeatObservedMs <= m_heartbeatBaselineMs) {
        return;
    }
    const bool satisfied =
        (m_heartbeatExpectation == HeartbeatExpectation::Guided
         && SwarmFlightMode::isExactGuided(snapshot))
        || (m_heartbeatExpectation == HeartbeatExpectation::Armed
            && snapshot.armed);
    if (!satisfied) {
        return;
    }

    const HeartbeatExpectation completed = m_heartbeatExpectation;
    m_heartbeatTimer.stop();
    m_heartbeatExpectation = HeartbeatExpectation::None;
    QPointer<SwarmSequenceExecutor> guard(this);
    setState(State::TakingOff);
    if (!guard || !m_dispatchAllowed) {
        return;
    }
    if (completed == HeartbeatExpectation::Guided) {
        submitTakeoffCommand(TakeoffPhase::Arm);
    } else {
        submitTakeoffCommand(TakeoffPhase::Takeoff);
    }
}

void SwarmSequenceExecutor::rejectCurrentTakeoffVehicle(
    const QString &detail)
{
    if (m_takeoffVehicleIndex >= 0
        && m_takeoffVehicleIndex < m_workingReport.vehicles.size()) {
        m_workingReport.vehicles[m_takeoffVehicleIndex].detail = detail;
    }
    ++m_takeoffVehicleIndex;
    beginTakeoffVehicle();
}

void SwarmSequenceExecutor::commandFinished(
    const VehicleCommandService::ExactCommandReport &report)
{
    if (m_backendDepth > 0) {
        m_deferredCommands.append(report);
        return;
    }
    processCommandFinished(report);
}

void SwarmSequenceExecutor::processCommandFinished(
    const VehicleCommandService::ExactCommandReport &report)
{
    if (m_pendingCommand.generation != m_generation
        || !sameCommandToken(report.token, m_pendingCommand.token)) {
        return;
    }
    const PendingCommand pending = m_pendingCommand;
    m_pendingCommand = {};

    using Terminal = VehicleCommandService::ExactTerminalResult;
    const bool accepted = report.terminalResult
        == Terminal::AcknowledgedAccepted;
    const bool uncertain =
        report.terminalResult == Terminal::TimedOutOutcomeUncertain
        || report.terminalResult == Terminal::TransportOutcomeUncertain
        || report.terminalResult == Terminal::LeaseRetiredOutcomeUncertain
        || report.terminalResult == Terminal::LinkForgottenOutcomeUncertain;
    SwarmSequenceVehicleResult *vehicle = nullptr;
    if (pending.vehicleIndex >= 0
        && pending.vehicleIndex < m_workingReport.vehicles.size()) {
        vehicle = &m_workingReport.vehicles[pending.vehicleIndex];
    }
    if (accepted && vehicle) {
        if (pending.phase == TakeoffPhase::Guided) {
            vehicle->guidedAccepted = true;
        } else if (pending.phase == TakeoffPhase::Arm) {
            vehicle->armAccepted = true;
        } else {
            vehicle->takeoffAccepted = true;
        }
    }
    if (report.frameAttempted) {
        ++m_workingReport.framesAttempted;
    }
    if (accepted) {
        ++m_workingReport.framesSent;
    }
    ++m_workingReport.framesPlanned;

    if (!m_dispatchAllowed || m_state == State::Draining) {
        if (uncertain) {
            m_workingReport.result =
                SwarmSequenceOperationResult::OutcomeUncertain;
            m_workingReport.description = detailOr(
                report.description,
                QStringLiteral("A draining exact command outcome is uncertain."));
        } else if (accepted) {
            m_workingReport.result = SwarmSequenceOperationResult::Partial;
            m_workingReport.description = QStringLiteral(
                "A command was accepted while Sequence cancellation was draining.");
        }
        releaseReservations();
        return;
    }

    if (uncertain) {
        beginDrain(SwarmSequenceOperationResult::OutcomeUncertain,
                   detailOr(report.description, QStringLiteral(
                       "The exact takeoff command outcome is uncertain.")));
        return;
    }
    if (!accepted) {
        rejectCurrentTakeoffVehicle(detailOr(
            report.description,
            QStringLiteral("The vehicle rejected its Sequence takeoff command.")));
        return;
    }

    if (pending.phase == TakeoffPhase::Guided) {
        beginHeartbeatBarrier(
            HeartbeatExpectation::Guided, m_heartbeatBaselineMs);
    } else if (pending.phase == TakeoffPhase::Arm) {
        beginHeartbeatBarrier(
            HeartbeatExpectation::Armed, m_heartbeatBaselineMs);
    } else {
        ++m_takeoffVehicleIndex;
        beginTakeoffVehicle();
    }
}

void SwarmSequenceExecutor::commandReservationReleased(quint64 reservationId)
{
    if (m_backendDepth > 0) {
        m_deferredCommandReleases.append(reservationId);
        return;
    }
    if (m_commandsReserved
        && reservationId == m_commandReservation.reservationId) {
        m_commandsReserved = false;
        if (m_dispatchAllowed) {
            const bool pendingAttempt = m_pendingCommand.token.isValid();
            m_pendingCommand = {};
            beginDrain(pendingAttempt
                           ? SwarmSequenceOperationResult::OutcomeUncertain
                           : (m_workingReport.framesSent > 0
                                  ? SwarmSequenceOperationResult::Partial
                                  : SwarmSequenceOperationResult::Rejected),
                       QStringLiteral(
                           "The exact command reservation was released unexpectedly."));
        } else {
            releaseReservations();
        }
    }
}

void SwarmSequenceExecutor::swarmSessionCancelled(
    quint64 sessionId, const QString &reason)
{
    if (m_backendDepth > 0) {
        m_deferredSwarmCancellations.append({sessionId, reason});
        return;
    }
    if (!m_swarmReserved || sessionId != m_swarmToken.id) {
        return;
    }
    m_swarmReserved = false;
    if (m_dispatchAllowed) {
        const bool pendingAttempt = m_pendingCommand.token.isValid();
        beginDrain(pendingAttempt
                       ? SwarmSequenceOperationResult::OutcomeUncertain
                       : (m_workingReport.framesSent > 0
                              ? SwarmSequenceOperationResult::Partial
                              : SwarmSequenceOperationResult::Rejected),
                   detailOr(reason, QStringLiteral(
                       "The exact swarm reservation was cancelled.")));
    } else {
        finishDrain();
    }
}

void SwarmSequenceExecutor::drainDeferredCallbacks()
{
    if (m_backendDepth > 0 || m_destroying) {
        return;
    }
    QPointer<SwarmSequenceExecutor> guard(this);
    while (!m_deferredCommands.isEmpty()
           || !m_deferredCommandReleases.isEmpty()
           || !m_deferredSwarmCancellations.isEmpty()) {
        const auto commands = std::exchange(
            m_deferredCommands,
            QVector<VehicleCommandService::ExactCommandReport>());
        const auto releases = std::exchange(
            m_deferredCommandReleases, QVector<quint64>());
        const auto cancellations = std::exchange(
            m_deferredSwarmCancellations,
            QVector<QPair<quint64, QString>>());
        for (const auto &report : commands) {
            processCommandFinished(report);
            if (!guard) {
                return;
            }
        }
        for (quint64 reservationId : releases) {
            commandReservationReleased(reservationId);
            if (!guard) {
                return;
            }
        }
        for (const auto &cancelled : cancellations) {
            swarmSessionCancelled(cancelled.first, cancelled.second);
            if (!guard) {
                return;
            }
        }
    }
}

void SwarmSequenceExecutor::accumulateBatch(
    const SwarmCommandService::BatchReport &report)
{
    for (const auto &member : report.members) {
        m_workingReport.framesPlanned += member.framesPlanned;
        m_workingReport.framesAttempted += member.framesAttempted;
        m_workingReport.framesSent += member.framesSent;
    }
}

SwarmSequenceOperationResult SwarmSequenceExecutor::failedBatchResult(
    const SwarmCommandService::BatchReport &report) const
{
    if (batchOutcomeUncertain(report)) {
        return SwarmSequenceOperationResult::OutcomeUncertain;
    }
    const bool partial = m_workingReport.framesSent > 0
        || report.result == SwarmCommandService::Result::PartialSend
        || std::any_of(report.members.cbegin(), report.members.cend(),
                       [](const auto &member) {
            return member.framesSent > 0;
        });
    return partial ? SwarmSequenceOperationResult::Partial
                   : SwarmSequenceOperationResult::Rejected;
}

void SwarmSequenceExecutor::complete(
    SwarmSequenceOperationResult result, const QString &description)
{
    beginDrain(result, description);
}

void SwarmSequenceExecutor::cancelActiveOperation(const QString &reason)
{
    if (m_state == State::Idle || m_state == State::Draining
        || m_state == State::PartialEffect
        || m_state == State::OutcomeUncertain) {
        return;
    }
    const bool knownEffect = std::any_of(
        m_workingReport.vehicles.cbegin(),
        m_workingReport.vehicles.cend(),
        [](const SwarmSequenceVehicleResult &vehicle) {
            return vehicle.targetSent || vehicle.guidedAccepted
                || vehicle.armAccepted || vehicle.takeoffAccepted;
        });
    beginDrain(knownEffect ? SwarmSequenceOperationResult::Partial
                           : SwarmSequenceOperationResult::Cancelled,
               detailOr(reason, QStringLiteral(
                   "The Sequence operation was cancelled.")));
}

void SwarmSequenceExecutor::beginDrain(
    SwarmSequenceOperationResult result, const QString &description)
{
    if (m_state == State::Draining) {
        return;
    }
    if ((m_state == State::Idle || m_state == State::PartialEffect
         || m_state == State::OutcomeUncertain)
        && m_operation == SwarmSequenceOperation::None
        && !m_swarmReserved && !m_commandsReserved) {
        return;
    }
    m_dispatchAllowed = false;
    m_heartbeatTimer.stop();
    m_heartbeatExpectation = HeartbeatExpectation::None;
    m_workingReport.result = result;
    m_workingReport.description = detailOr(
        description, QStringLiteral("The Sequence operation stopped."));
    m_completionPending = true;
    QPointer<SwarmSequenceExecutor> guard(this);
    setState(State::Draining);
    if (!guard) {
        return;
    }
    setStatus(QStringLiteral("%1 Draining exact terminal outcomes.")
                  .arg(m_workingReport.description));
    if (!guard) {
        return;
    }
    releaseReservations();
}

void SwarmSequenceExecutor::releaseReservations()
{
    if (!m_backend) {
        m_commandsReserved = false;
        m_swarmReserved = false;
        m_pendingCommand = {};
        m_workingReport.result =
            SwarmSequenceOperationResult::OutcomeUncertain;
        finishDrain();
        return;
    }
    QPointer<SwarmSequenceExecutor> guard(this);
    if (m_commandsReserved) {
        if (!m_commandReleaseRequested) {
            m_commandReleaseRequested = true;
            ++m_backendDepth;
            const bool accepted =
                m_backend->releaseCommands(m_commandReservation);
            if (!guard) {
                return;
            }
            --m_backendDepth;
            if (!accepted) {
                m_commandsReserved = false;
                m_pendingCommand = {};
                m_workingReport.result =
                    SwarmSequenceOperationResult::OutcomeUncertain;
                m_workingReport.description = QStringLiteral(
                    "The exact command reservation could not be drained.");
            }
            drainDeferredCallbacks();
            if (!guard) {
                return;
            }
        }
        if (m_commandsReserved) {
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
            m_workingReport.result =
                SwarmSequenceOperationResult::OutcomeUncertain;
            m_workingReport.description = QStringLiteral(
                "The exact swarm reservation could not be released cleanly.");
        }
        m_swarmReserved = false;
        drainDeferredCallbacks();
        if (!guard) {
            return;
        }
    }
    finishDrain();
}

void SwarmSequenceExecutor::finishDrain()
{
    if (m_state != State::Draining || m_commandsReserved || m_swarmReserved
        || m_pendingCommand.token.isValid() || !m_completionPending) {
        return;
    }
    m_completionPending = false;
    m_lastReport = m_workingReport;
    const SwarmSequenceOperationReport completedReport = m_lastReport;
    const bool partial = m_lastReport.result
        == SwarmSequenceOperationResult::Partial;
    const bool uncertain = m_lastReport.result
        == SwarmSequenceOperationResult::OutcomeUncertain;
    const QString terminalStatus = m_lastReport.description;
    m_runPlan = {};
    m_takeoffPlan = {};
    m_pendingCommand = {};
    m_swarmToken = {};
    m_commandReservation = {};
    m_commandReleaseRequested = false;
    m_heartbeatBaselineMs = -1;
    m_heartbeatDeadlineMs = -1;
    m_operation = SwarmSequenceOperation::None;
    m_finishing = true;
    QPointer<SwarmSequenceExecutor> guard(this);
    setStatus(partial
        ? QStringLiteral(
              "%1 A partial vehicle effect occurred; restart is blocked.")
              .arg(terminalStatus)
        : (uncertain
               ? QStringLiteral(
                     "%1 Exact outcome is uncertain; restart is blocked.")
                     .arg(terminalStatus)
               : terminalStatus));
    if (!guard) {
        return;
    }
    setState(partial ? State::PartialEffect
                     : (uncertain ? State::OutcomeUncertain : State::Idle));
    if (!guard) {
        return;
    }
    emit operationFinished(completedReport);
    if (!guard) {
        return;
    }
    m_finishing = false;
    emit changed();
}

bool SwarmSequenceExecutor::isActive() const noexcept
{
    return m_state == State::RunningStep || m_state == State::TakingOff
        || m_state == State::WaitingForHeartbeat
        || m_state == State::Draining;
}

void SwarmSequenceExecutor::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    QPointer<SwarmSequenceExecutor> guard(this);
    emit stateChanged(state);
    if (guard) {
        emit changed();
    }
}

void SwarmSequenceExecutor::setStatus(const QString &status)
{
    const QString value = status.trimmed();
    if (value.isEmpty() || value == m_status) {
        return;
    }
    m_status = value;
    QPointer<SwarmSequenceExecutor> guard(this);
    emit statusChanged(value);
    if (guard) {
        emit changed();
    }
}
