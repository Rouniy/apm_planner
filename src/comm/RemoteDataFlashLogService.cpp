#include "RemoteDataFlashLogService.h"

#include "ExactLinkTransmitter.h"
#include "ParameterService.h"
#include "VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QSharedData>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
class ScopeExit final
{
public:
    explicit ScopeExit(std::function<void()> callback)
        : m_callback(std::move(callback)) {}
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;
    ~ScopeExit() { m_callback(); }

private:
    std::function<void()> m_callback;
};

void assignError(QString *error, const QString &message)
{
    if (error) *error = message;
}

bool sameFrozenValue(const QVariant &left, const QVariant &right,
                     ParameterType type)
{
    switch (type) {
    case ParameterType::UInt8:
    case ParameterType::UInt16:
    case ParameterType::UInt32: {
        bool leftOk = false;
        bool rightOk = false;
        const qulonglong lhs = left.toULongLong(&leftOk);
        const qulonglong rhs = right.toULongLong(&rightOk);
        return leftOk && rightOk && lhs == rhs;
    }
    case ParameterType::Int8:
    case ParameterType::Int16:
    case ParameterType::Int32: {
        bool leftOk = false;
        bool rightOk = false;
        const qlonglong lhs = left.toLongLong(&leftOk);
        const qlonglong rhs = right.toLongLong(&rightOk);
        return leftOk && rightOk && lhs == rhs;
    }
    case ParameterType::Real32: {
        bool leftOk = false;
        bool rightOk = false;
        const float lhs = left.toFloat(&leftOk);
        const float rhs = right.toFloat(&rightOk);
        return leftOk && rightOk && std::isfinite(lhs)
            && std::isfinite(rhs) && lhs == rhs;
    }
    default:
        return false;
    }
}

bool supportedClassicType(ParameterType type)
{
    switch (type) {
    case ParameterType::UInt8:
    case ParameterType::Int8:
    case ParameterType::UInt16:
    case ParameterType::Int16:
    case ParameterType::UInt32:
    case ParameterType::Int32:
    case ParameterType::Real32:
        return true;
    default:
        return false;
    }
}

bool backendValue(const ParameterRecord &record, quint32 *value)
{
    if (value) *value = 0;
    if (!supportedClassicType(record.type)) return false;
    bool ok = false;
    const double numeric = record.value.toDouble(&ok);
    if (!ok || !std::isfinite(numeric) || numeric < 0.0
        || numeric > double(std::numeric_limits<quint32>::max())
        || std::trunc(numeric) != numeric) {
        return false;
    }
    if (value) *value = static_cast<quint32>(numeric);
    return true;
}

QString endpointText(const VehicleEndpoint &endpoint)
{
    return QStringLiteral("link %1, system %2, component %3")
        .arg(endpoint.linkId).arg(endpoint.systemId).arg(endpoint.componentId);
}
}

class RemoteDataFlashLogService::Plan::Data final : public QSharedData
{
public:
    quint64 planId = 0;
    QString directoryPath;
    QString fileStem;
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    quint8 localSystemId = 0;
    quint8 localComponentId = 0;
    QVariant backendValue;
    ParameterType backendType = ParameterType::Unknown;
};

RemoteDataFlashLogService::Plan::Plan() = default;
RemoteDataFlashLogService::Plan::Plan(const Plan &other) = default;
RemoteDataFlashLogService::Plan &RemoteDataFlashLogService::Plan::operator=(
    const Plan &other) = default;
RemoteDataFlashLogService::Plan::~Plan() = default;

bool RemoteDataFlashLogService::Plan::isValid() const noexcept
{
    return d && d->planId != 0 && !d->directoryPath.isEmpty()
        && !d->fileStem.isEmpty() && d->target.isValid()
        && d->vehicle.isValid() && d->localSystemId != 0
        && d->localComponentId != 0 && d->backendValue.isValid()
        && d->backendType != ParameterType::Unknown;
}

QString RemoteDataFlashLogService::Plan::directoryPath() const
{
    return d ? d->directoryPath : QString();
}

QString RemoteDataFlashLogService::Plan::fileStem() const
{
    return d ? d->fileStem : QString();
}

QString RemoteDataFlashLogService::Plan::destinationDescription() const
{
    if (!d) return {};
    return QDir(d->directoryPath).filePath(
        d->fileStem + QStringLiteral("-<unique>.bin (or .partial.bin)"));
}

VehicleTargetLease RemoteDataFlashLogService::Plan::target() const
{
    return d ? d->target : VehicleTargetLease{};
}

SwarmVehicleInstanceLease RemoteDataFlashLogService::Plan::vehicle() const
{
    return d ? d->vehicle : SwarmVehicleInstanceLease{};
}

quint8 RemoteDataFlashLogService::Plan::localSystemId() const
{
    return d ? d->localSystemId : 0;
}

quint8 RemoteDataFlashLogService::Plan::localComponentId() const
{
    return d ? d->localComponentId : 0;
}

struct RemoteDataFlashLogService::Runtime
{
    QPointer<VehicleTargetManager> targetManager;
    QPointer<SwarmTelemetryRegistry> telemetryRegistry;
    QPointer<ParameterService> parameterService;
    QPointer<ExactLinkTransmitter> transmitter;
    QPointer<RemoteDataFlashLogWriter> writer;
    RouteValidator routeValidator;
    Clock clock;
    QElapsedTimer monotonic;
    QTimer *timeoutTimer = nullptr;

    quint8 localSystemId = 0;
    quint8 localComponentId = 0;
    int firstBlockTimeoutMs = DefaultFirstBlockTimeoutMs;
    int streamSilenceMs = DefaultStreamSilenceMs;
    int maximumSessionMs = DefaultMaximumSessionMs;
    int stopQuietMs = DefaultStopQuietMs;

    bool starting = false;
    bool preparing = false;
    bool finishing = false;
    bool shuttingDown = false;
    Phase phase = Phase::Idle;
    quint64 nextPlanId = 1;
    quint64 nextOperationId = 1;
    quint64 operationId = 0;
    Plan pendingPlan;
    Plan activePlan;
    QString status;
    QString stagingPath;
    QStringList history;
    Report lastReport;
    Outcome pendingOutcome = Outcome::ProtocolError;
    QString pendingDescription;

    qint64 admittedAtMs = -1;
    qint64 startSentAtMs = -1;
    qint64 lastStoredAtMs = -1;
    qint64 stopRequestedAtMs = -1;
    qint64 blocksStored = 0;
    qint64 duplicateBlocks = 0;
    quint32 highestSequence = 0;
    bool hasBlocks = false;
    bool sequenceZeroObserved = false;
    bool startAttempted = false;
    bool stopAttempted = false;
    bool stopSubmitted = false;
    bool saveRequested = false;
    bool silenceAdvisory = false;
    bool discardInitiated = false;
    QSet<quint32> preSequenceZeroBlocks;
    QHash<quint32, QByteArray> pendingBlocks;
    std::shared_ptr<bool> startAttemptInFlight;
    std::shared_ptr<bool> stopAttemptInFlight;

    bool startWasAttempted() const noexcept
    {
        return startAttempted
            || (startAttemptInFlight && *startAttemptInFlight);
    }

    bool stopWasAttempted() const noexcept
    {
        return stopAttempted
            || (stopAttemptInFlight && *stopAttemptInFlight);
    }

    qint64 nowMs() const
    {
        return clock ? clock() : monotonic.elapsed();
    }

    bool busy() const noexcept
    {
        return preparing || starting || finishing || operationId != 0;
    }
};

RemoteDataFlashLogService::RemoteDataFlashLogService(
    VehicleTargetManager *targetManager,
    SwarmTelemetryRegistry *telemetryRegistry,
    ParameterService *parameterService,
    ExactLinkTransmitter *transmitter,
    quint8 localSystemId,
    quint8 localComponentId,
    RouteValidator routeValidator,
    QObject *parent)
    : RemoteDataFlashLogService(
          targetManager, telemetryRegistry, parameterService, transmitter,
          localSystemId, localComponentId, std::move(routeValidator), Clock{},
          DefaultFirstBlockTimeoutMs, DefaultStreamSilenceMs,
          DefaultMaximumSessionMs, DefaultStopQuietMs, parent)
{
}

RemoteDataFlashLogService::RemoteDataFlashLogService(
    VehicleTargetManager *targetManager,
    SwarmTelemetryRegistry *telemetryRegistry,
    ParameterService *parameterService,
    ExactLinkTransmitter *transmitter,
    quint8 localSystemId,
    quint8 localComponentId,
    RouteValidator routeValidator,
    Clock clock,
    int firstBlockTimeoutMs,
    int streamSilenceMs,
    int maximumSessionMs,
    int stopQuietMs,
    QObject *parent)
    : QObject(parent), m_runtime(new Runtime)
{
    qRegisterMetaType<Report>();
    m_runtime->targetManager = targetManager;
    m_runtime->telemetryRegistry = telemetryRegistry;
    m_runtime->parameterService = parameterService;
    m_runtime->transmitter = transmitter;
    m_runtime->localSystemId = localSystemId;
    m_runtime->localComponentId = localComponentId;
    m_runtime->routeValidator = std::move(routeValidator);
    m_runtime->clock = std::move(clock);
    m_runtime->firstBlockTimeoutMs = qMax(1, firstBlockTimeoutMs);
    m_runtime->streamSilenceMs = qMax(1, streamSilenceMs);
    m_runtime->maximumSessionMs = qMax(1, maximumSessionMs);
    m_runtime->stopQuietMs = qMax(0, stopQuietMs);
    m_runtime->monotonic.start();

    auto *writer = new RemoteDataFlashLogWriter(this);
    m_runtime->writer = writer;
    connect(writer, &RemoteDataFlashLogWriter::opened, this,
            &RemoteDataFlashLogService::handleWriterOpened);
    connect(writer, &RemoteDataFlashLogWriter::blockStored, this,
            &RemoteDataFlashLogService::handleBlockStored);
    connect(writer, &RemoteDataFlashLogWriter::failed, this,
            &RemoteDataFlashLogService::handleWriterFailed);
    connect(writer, &RemoteDataFlashLogWriter::finished, this,
            &RemoteDataFlashLogService::handleWriterFinished);

    m_runtime->timeoutTimer = new QTimer(this);
    const int shortest = qMin(qMin(m_runtime->firstBlockTimeoutMs,
                                   m_runtime->streamSilenceMs),
                              qMax(1, m_runtime->maximumSessionMs));
    m_runtime->timeoutTimer->setInterval(qBound(1, shortest / 4, 1000));
    connect(m_runtime->timeoutTimer, &QTimer::timeout, this,
            &RemoteDataFlashLogService::checkTimeouts);

    if (targetManager) {
        connect(targetManager, &VehicleTargetManager::currentTargetChanged,
                this, [this] { checkTimeouts(); });
    }
    if (telemetryRegistry) {
        connect(telemetryRegistry, &SwarmTelemetryRegistry::endpointRetired,
                this, [this](SwarmVehicleInstanceLease,
                             SwarmTelemetryRegistry::RetirementReason) {
            checkTimeouts();
        });
        connect(telemetryRegistry, &SwarmTelemetryRegistry::linkSessionEnded,
                this, [this](int, qulonglong) { checkTimeouts(); });
    }
}

RemoteDataFlashLogService::~RemoteDataFlashLogService()
{
    shutdown();
}

bool RemoteDataFlashLogService::busy() const noexcept
{
    return m_runtime->busy();
}

RemoteDataFlashLogService::Phase RemoteDataFlashLogService::phase() const noexcept
{
    return m_runtime->phase;
}

quint64 RemoteDataFlashLogService::currentOperationId() const noexcept
{
    return m_runtime->operationId;
}

RemoteDataFlashLogService::Plan RemoteDataFlashLogService::activePlan() const
{
    return m_runtime->activePlan;
}

QString RemoteDataFlashLogService::status() const
{
    return m_runtime->status;
}

QStringList RemoteDataFlashLogService::history() const
{
    return m_runtime->history;
}

RemoteDataFlashLogService::Report RemoteDataFlashLogService::lastReport() const
{
    return m_runtime->lastReport;
}

qint64 RemoteDataFlashLogService::blocksStored() const noexcept
{
    return m_runtime->blocksStored;
}

qint64 RemoteDataFlashLogService::bytesStored() const noexcept
{
    return m_runtime->blocksStored * RemoteDataFlashLogWriter::BlockBytes;
}

bool RemoteDataFlashLogService::captureVehicle(
    VehicleTargetLease *target, SwarmVehicleInstanceLease *vehicle,
    QString *error) const
{
    const QPointer<const RemoteDataFlashLogService> guard(this);
    if (target) *target = {};
    if (vehicle) *vehicle = {};
    if (m_runtime->shuttingDown || !m_runtime->targetManager
        || !m_runtime->telemetryRegistry || !m_runtime->parameterService
        || !m_runtime->parameterService->store() || !m_runtime->transmitter
        || !m_runtime->writer || m_runtime->localSystemId == 0
        || m_runtime->localComponentId == 0 || !m_runtime->routeValidator) {
        assignError(error, QStringLiteral(
            "Remote DataFlash logging services are unavailable."));
        return false;
    }
    const VehicleTargetLease selected =
        m_runtime->targetManager->acquireTarget();
    if (!selected.isValid()
        || !m_runtime->targetManager->isTargetGenerationSettled()) {
        assignError(error, QStringLiteral(
            "Select one settled connected vehicle first."));
        return false;
    }
    const SwarmVehicleInstanceLease exact =
        m_runtime->telemetryRegistry->acquireVehicle(
            selected.endpoint, MaximumHeartbeatAgeMs);
    if (!exact.isValid()) {
        assignError(error, QStringLiteral(
            "The selected vehicle has no fresh exact heartbeat."));
        return false;
    }
    if (!validateVehicle(selected, exact, true, error) || !guard) return false;
    if (target) *target = selected;
    if (vehicle) *vehicle = exact;
    return true;
}

bool RemoteDataFlashLogService::validateVehicle(
    const VehicleTargetLease &target,
    const SwarmVehicleInstanceLease &vehicle,
    bool requireDisarmed,
    QString *error) const
{
    const QPointer<const RemoteDataFlashLogService> guard(this);
    const QPointer<VehicleTargetManager> targetManager =
        m_runtime->targetManager;
    const QPointer<SwarmTelemetryRegistry> registry =
        m_runtime->telemetryRegistry;
    const auto stateIsSafe = [&]() -> bool {
        if (!targetManager || !registry || !target.isValid()
            || !vehicle.isValid()
            || !target.endpoint.sameIdentity(vehicle.endpoint)
            || vehicle.endpoint.componentId != MAV_COMP_ID_AUTOPILOT1
            || !targetManager->isTargetGenerationSettled()
            || !targetManager->isCurrentTarget(
                   target.endpoint.linkId, target.endpoint.systemId,
                   target.endpoint.componentId, target.generation)
            || !registry->validateLease(vehicle, MaximumHeartbeatAgeMs)) {
            return false;
        }
        int autopilotsOnLink = 0;
        for (const VehicleEndpoint &endpoint : registry->endpoints()) {
            if (endpoint.linkId == vehicle.endpoint.linkId
                && endpoint.componentId == MAV_COMP_ID_AUTOPILOT1) {
                ++autopilotsOnLink;
            }
        }
        if (autopilotsOnLink != 1) return false;
        SwarmTelemetrySnapshot telemetry;
        return registry->snapshotForLease(vehicle, &telemetry)
            && telemetry.heartbeatValid
            && registry->observationIsFresh(
                   telemetry.heartbeatObservedMs, MaximumHeartbeatAgeMs)
            && telemetry.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA
            && (!requireDisarmed || !telemetry.armed);
    };
    if (!stateIsSafe()) {
        assignError(error, QStringLiteral(
            "The exact route must contain one fresh ArduPilot autopilot"
            " and starting requires it to be disarmed."));
        return false;
    }
    const RouteValidator route = m_runtime->routeValidator;
    if (!route || !route(vehicle, error)) return false;
    if (!guard) return false;
    if (!stateIsSafe()) {
        assignError(error, QStringLiteral(
            "The target, physical route, heartbeat, or armed state changed during validation."));
        return false;
    }
    return true;
}

bool RemoteDataFlashLogService::canPrepare(QString *error) const
{
    const QPointer<const RemoteDataFlashLogService> guard(this);
    if (error) error->clear();
    if (m_runtime->busy()) {
        assignError(error, QStringLiteral(
            "A remote DataFlash log capture is already active."));
        return false;
    }
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    if (!captureVehicle(&target, &vehicle, error)) return false;
    if (!guard || !m_runtime->parameterService
        || !m_runtime->parameterService->store()) return false;
    const ParameterSnapshot snapshot =
        m_runtime->parameterService->store()->snapshot(vehicle.endpoint);
    const quint8 component = static_cast<quint8>(vehicle.endpoint.componentId);
    if (!snapshot.isComplete()
        || !snapshot.endpoint().sameIdentity(vehicle.endpoint)
        || !snapshot.contains(component, QStringLiteral("LOG_BACKEND_TYPE"))) {
        assignError(error, QStringLiteral(
            "Load a complete exact parameter list containing LOG_BACKEND_TYPE first."));
        return false;
    }
    quint32 flags = 0;
    if (!backendValue(snapshot.value(component,
                                     QStringLiteral("LOG_BACKEND_TYPE")),
                      &flags) || (flags & 2U) == 0U) {
        assignError(error, QStringLiteral(
            "LOG_BACKEND_TYPE must already include the MAVLink backend bit (2); this tool does not change parameters."));
        return false;
    }
    return true;
}

bool RemoteDataFlashLogService::prepare(
    const QString &directoryPath, Plan *planOut, QString *error)
{
    const QString submittedDirectory = directoryPath;
    const QPointer<RemoteDataFlashLogService> guard(this);
    if (planOut) *planOut = {};
    if (error) error->clear();
    if (!planOut) {
        assignError(error, QStringLiteral("A plan output is required."));
        return false;
    }
    if (m_runtime->busy()) {
        assignError(error, QStringLiteral(
            "A remote DataFlash log capture is already active."));
        return false;
    }
    m_runtime->pendingPlan = {};
    const QFileInfo directory(submittedDirectory);
    const QString canonical = directory.canonicalFilePath();
    if (!directory.exists() || !directory.isDir() || canonical.isEmpty()
        || directory.isSymLink()) {
        assignError(error, QStringLiteral(
            "Choose an existing non-symlink log directory."));
        return false;
    }

    m_runtime->preparing = true;
    ScopeExit preparingGuard([guard]() {
        if (guard) guard->m_runtime->preparing = false;
    });

    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    if (!captureVehicle(&target, &vehicle, error) || !guard) return false;
    const ParameterSnapshot snapshot =
        m_runtime->parameterService->store()->snapshot(vehicle.endpoint);
    const quint8 component = static_cast<quint8>(vehicle.endpoint.componentId);
    if (!snapshot.isComplete()
        || !snapshot.endpoint().sameIdentity(vehicle.endpoint)
        || !snapshot.contains(component, QStringLiteral("LOG_BACKEND_TYPE"))) {
        assignError(error, QStringLiteral(
            "Load a complete exact parameter list containing LOG_BACKEND_TYPE first."));
        return false;
    }
    const ParameterRecord backend = snapshot.value(
        component, QStringLiteral("LOG_BACKEND_TYPE"));
    quint32 flags = 0;
    if (!backendValue(backend, &flags) || (flags & 2U) == 0U) {
        assignError(error, QStringLiteral(
            "LOG_BACKEND_TYPE must already include the MAVLink backend bit (2); this tool does not change parameters."));
        return false;
    }

    quint64 planId = m_runtime->nextPlanId++;
    if (planId == 0) planId = m_runtime->nextPlanId++;
    if (planId == 0) {
        assignError(error, QStringLiteral("Remote log plan identifiers are exhausted."));
        return false;
    }
    Plan plan;
    plan.d = new Plan::Data;
    plan.d->planId = planId;
    plan.d->directoryPath = canonical;
    plan.d->fileStem = QStringLiteral("remote-dataflash-%1-sys%2-p%3")
        .arg(QDateTime::currentDateTimeUtc().toString(
                 QStringLiteral("yyyyMMdd-HHmmss")))
        .arg(vehicle.endpoint.systemId)
        .arg(planId);
    plan.d->target = target;
    plan.d->vehicle = vehicle;
    plan.d->localSystemId = m_runtime->localSystemId;
    plan.d->localComponentId = m_runtime->localComponentId;
    plan.d->backendValue = backend.value;
    plan.d->backendType = backend.type;
    m_runtime->pendingPlan = plan;
    *planOut = plan;
    return true;
}

bool RemoteDataFlashLogService::validatePlanInternal(
    const Plan &plan, bool requireDisarmed, QString *error) const
{
    const Plan pinned = plan;
    const bool matchesPrepared = m_runtime->pendingPlan.isValid()
        && pinned.d.constData() == m_runtime->pendingPlan.d.constData();
    const bool matchesActive = m_runtime->activePlan.isValid()
        && pinned.d.constData() == m_runtime->activePlan.d.constData();
    if (!pinned.isValid() || (!matchesPrepared && !matchesActive)
        || pinned.d->localSystemId != m_runtime->localSystemId
        || pinned.d->localComponentId != m_runtime->localComponentId) {
        assignError(error, QStringLiteral(
            "The remote DataFlash logging plan is stale or was replaced."));
        return false;
    }
    const QPointer<const RemoteDataFlashLogService> guard(this);
    if (!validateVehicle(pinned.d->target, pinned.d->vehicle,
                         requireDisarmed, error) || !guard) return false;
    if (!m_runtime->parameterService || !m_runtime->parameterService->store()) {
        assignError(error, QStringLiteral("Parameter service is unavailable."));
        return false;
    }
    const ParameterSnapshot snapshot = m_runtime->parameterService->store()->snapshot(
        pinned.d->vehicle.endpoint);
    const quint8 component = static_cast<quint8>(
        pinned.d->vehicle.endpoint.componentId);
    if (!snapshot.isComplete()
        || !snapshot.endpoint().sameIdentity(pinned.d->vehicle.endpoint)
        || !snapshot.contains(component, QStringLiteral("LOG_BACKEND_TYPE"))) {
        assignError(error, QStringLiteral(
            "The complete exact parameter snapshot changed after confirmation."));
        return false;
    }
    const ParameterRecord current = snapshot.value(
        component, QStringLiteral("LOG_BACKEND_TYPE"));
    quint32 flags = 0;
    if (current.type != pinned.d->backendType
        || !sameFrozenValue(current.value, pinned.d->backendValue,
                            current.type)
        || !backendValue(current, &flags) || (flags & 2U) == 0U) {
        assignError(error, QStringLiteral(
            "LOG_BACKEND_TYPE changed after confirmation; prepare again."));
        return false;
    }
    const QFileInfo directory(pinned.d->directoryPath);
    if (!directory.exists() || !directory.isDir()
        || directory.isSymLink()
        || directory.canonicalFilePath() != pinned.d->directoryPath) {
        assignError(error, QStringLiteral(
            "The selected log directory changed after confirmation."));
        return false;
    }
    return true;
}

bool RemoteDataFlashLogService::validate(
    const Plan &plan, QString *error) const
{
    if (error) error->clear();
    if (m_runtime->busy()) {
        assignError(error, QStringLiteral(
            "A remote DataFlash log capture is already active."));
        return false;
    }
    return validatePlanInternal(plan, true, error);
}

RemoteDataFlashLogService::StartResult RemoteDataFlashLogService::start(
    const Plan &plan, quint64 *operationIdOut, QString *error)
{
    const Plan submitted = plan;
    if (operationIdOut) *operationIdOut = 0;
    if (error) error->clear();
    if (!operationIdOut || !submitted.isValid()
        || !m_runtime->pendingPlan.isValid()
        || submitted.d.constData() != m_runtime->pendingPlan.d.constData()) {
        assignError(error, QStringLiteral(
            "A current prepared remote logging plan is required."));
        return StartResult::InvalidPlan;
    }
    if (m_runtime->busy()) {
        assignError(error, QStringLiteral(
            "A remote DataFlash log capture is already active."));
        return StartResult::Busy;
    }
    if (m_runtime->shuttingDown || !m_runtime->writer
        || !m_runtime->transmitter) {
        assignError(error, QStringLiteral(
            "Remote DataFlash logging services are unavailable."));
        return StartResult::Unavailable;
    }

    quint64 operationId = m_runtime->nextOperationId++;
    if (operationId == 0) operationId = m_runtime->nextOperationId++;
    if (operationId == 0) {
        assignError(error, QStringLiteral(
            "Remote log operation identifiers are exhausted."));
        return StartResult::Unavailable;
    }

    // Publish ownership before the first injected route or transport callback.
    m_runtime->starting = true;
    m_runtime->operationId = operationId;
    m_runtime->activePlan = submitted;
    m_runtime->phase = Phase::Opening;
    m_runtime->status = QStringLiteral("Opening a private staging file…");
    m_runtime->admittedAtMs = m_runtime->nowMs();
    m_runtime->startSentAtMs = -1;
    m_runtime->lastStoredAtMs = -1;
    m_runtime->stopRequestedAtMs = -1;
    m_runtime->blocksStored = 0;
    m_runtime->duplicateBlocks = 0;
    m_runtime->highestSequence = 0;
    m_runtime->hasBlocks = false;
    m_runtime->sequenceZeroObserved = false;
    m_runtime->startAttempted = false;
    m_runtime->stopAttempted = false;
    m_runtime->stopSubmitted = false;
    m_runtime->saveRequested = false;
    m_runtime->silenceAdvisory = false;
    m_runtime->discardInitiated = false;
    m_runtime->stagingPath.clear();
    m_runtime->preSequenceZeroBlocks.clear();
    m_runtime->pendingBlocks.clear();
    m_runtime->startAttemptInFlight.reset();
    m_runtime->stopAttemptInFlight.reset();
    *operationIdOut = operationId;
    const QPointer<RemoteDataFlashLogService> guard(this);
    emit stateChanged();
    if (!guard) return StartResult::Unavailable;
    if (!operationIsCurrent(operationId)) {
        assignError(error, QStringLiteral(
            "Remote logging start was cancelled or replaced before validation."));
        return StartResult::Unavailable;
    }

    QString validationError;
    const bool valid = validatePlanInternal(
        submitted, true, &validationError);
    if (!guard) return StartResult::Unavailable;
    if (!operationIsCurrent(operationId)) {
        assignError(error, QStringLiteral(
            "Remote logging start was cancelled or replaced during validation."));
        return StartResult::Unavailable;
    }
    if (!valid) {
        m_runtime->starting = false;
        finishReport(Outcome::LeaseRetired, validationError);
        assignError(error, validationError);
        return StartResult::InvalidPlan;
    }
    if (!operationIsCurrent(operationId) || m_runtime->shuttingDown) {
        assignError(error, QStringLiteral("Remote logging start was cancelled."));
        return StartResult::Unavailable;
    }
    m_runtime->pendingPlan = {};
    m_runtime->starting = false;
    if (!m_runtime->writer->open(submitted.d->directoryPath,
                                 submitted.d->fileStem, operationId)) {
        finishReport(Outcome::IoError,
                     QStringLiteral("Cannot open the remote log staging writer."));
        assignError(error, QStringLiteral(
            "Cannot open the remote log staging writer."));
        return StartResult::IoError;
    }
    if (m_runtime->timeoutTimer) m_runtime->timeoutTimer->start();
    emit stateChanged();
    return StartResult::Started;
}

bool RemoteDataFlashLogService::operationIsCurrent(
    quint64 operationId) const noexcept
{
    return operationId != 0 && m_runtime->operationId == operationId
        && m_runtime->activePlan.isValid();
}

void RemoteDataFlashLogService::handleWriterOpened(
    quint64 operationId, const QString &path)
{
    if (!operationIsCurrent(operationId)
        || m_runtime->phase != Phase::Opening) return;
    const Plan plan = m_runtime->activePlan;
    const auto *const planIdentity = plan.d.constData();
    m_runtime->stagingPath = path;
    QString routeError;
    const QPointer<RemoteDataFlashLogService> guard(this);
    if (!validatePlanInternal(plan, true, &routeError) || !guard) {
        if (guard && operationIsCurrent(operationId)) {
            beginDiscard(Outcome::LeaseRetired, routeError, false);
        }
        return;
    }
    if (!operationIsCurrent(operationId)
        || m_runtime->phase != Phase::Opening
        || m_runtime->activePlan.d.constData() != planIdentity) return;
    if (deadlineReached()) {
        beginDiscard(Outcome::TimedOut,
                     QStringLiteral(
                         "The remote log session lifetime expired before START; nothing was published."),
                     false);
        return;
    }

    m_runtime->phase = Phase::AwaitingSequenceZero;
    m_runtime->status = QStringLiteral(
        "START submitted; waiting up to 10 seconds for sequence zero…");
    m_runtime->startSentAtMs = m_runtime->nowMs();
    bool attempted = false;
    const bool sent = sendStatus(
        operationId, MAV_REMOTE_LOG_DATA_BLOCK_START,
        MAV_REMOTE_LOG_DATA_BLOCK_ACK, &attempted);
    if (!guard || !operationIsCurrent(operationId)
        || m_runtime->phase != Phase::AwaitingSequenceZero
        || m_runtime->activePlan.d.constData() != planIdentity) return;
    m_runtime->startAttempted = attempted;
    if (!sent) {
        beginDiscard(Outcome::TransportFailed,
                     attempted
                         ? QStringLiteral(
                               "START may have been transmitted, but the transport did not confirm submission; nothing was published.")
                         : QStringLiteral(
                               "START was rejected before transmission; nothing was published."),
                     false);
        return;
    }
    emit stateChanged();
}

bool RemoteDataFlashLogService::sendStatus(
    quint64 operationId, quint32 sequence, quint8 status, bool *attempted)
{
    if (attempted) *attempted = false;
    if (!operationIsCurrent(operationId) || !m_runtime->transmitter) return false;
    const Plan plan = m_runtime->activePlan;
    const auto *data = plan.d.constData();
    const auto *const planIdentity = data;
    const Phase expectedPhase = m_runtime->phase;
    QString routeError;
    const QPointer<RemoteDataFlashLogService> guard(this);
    const bool requireDisarmed =
        sequence == MAV_REMOTE_LOG_DATA_BLOCK_START;
    if (!validateVehicle(data->target, data->vehicle, requireDisarmed,
                         &routeError)
        || !guard || !operationIsCurrent(operationId)
        || m_runtime->phase != expectedPhase
        || m_runtime->activePlan.d.constData() != planIdentity
        || !m_runtime->transmitter) {
        return false;
    }
    if (sequence != MAV_REMOTE_LOG_DATA_BLOCK_STOP && deadlineReached()) {
        return false;
    }
    // ArduPilot handles the logger protocol in the autopilot component.  ACKs
    // remain pinned there as well; addressing component 155 can be forwarded
    // to another logging component by a MAVLink router.
    const quint8 targetComponent = static_cast<quint8>(
        data->vehicle.endpoint.componentId);
    const auto frameAttempted = std::make_shared<bool>(false);
    if (sequence == MAV_REMOTE_LOG_DATA_BLOCK_START) {
        m_runtime->startAttemptInFlight = frameAttempted;
    } else if (sequence == MAV_REMOTE_LOG_DATA_BLOCK_STOP) {
        m_runtime->stopAttemptInFlight = frameAttempted;
    }
    const ExactLinkTransmitter::SendResult result =
        m_runtime->transmitter->sendRemoteLogBlockStatus(
            data->vehicle.endpoint.linkId, data->vehicle.linkSessionEpoch,
            data->localSystemId, data->localComponentId,
            static_cast<quint8>(data->vehicle.endpoint.systemId),
            targetComponent, sequence, status, frameAttempted.get());
    if (attempted) *attempted = *frameAttempted;
    if (!guard || !operationIsCurrent(operationId)
        || m_runtime->phase != expectedPhase
        || m_runtime->activePlan.d.constData() != planIdentity) return false;
    if (sequence == MAV_REMOTE_LOG_DATA_BLOCK_START) {
        m_runtime->startAttempted =
            m_runtime->startAttempted || *frameAttempted;
        if (m_runtime->startAttemptInFlight == frameAttempted) {
            m_runtime->startAttemptInFlight.reset();
        }
    } else if (sequence == MAV_REMOTE_LOG_DATA_BLOCK_STOP) {
        m_runtime->stopAttempted =
            m_runtime->stopAttempted || *frameAttempted;
        if (m_runtime->stopAttemptInFlight == frameAttempted) {
            m_runtime->stopAttemptInFlight.reset();
        }
    }
    return result == ExactLinkTransmitter::SendResult::Sent;
}

void RemoteDataFlashLogService::observeMessage(
    int linkId, quint64 linkSessionEpoch, const mavlink_message_t &message)
{
    if (!m_runtime->operationId || !m_runtime->activePlan.isValid()
        || (m_runtime->phase != Phase::AwaitingSequenceZero
            && m_runtime->phase != Phase::Receiving
            && m_runtime->phase != Phase::Stopping)
        || message.msgid != MAVLINK_MSG_ID_REMOTE_LOG_DATA_BLOCK) return;

    const quint64 operationId = m_runtime->operationId;
    const Plan plan = m_runtime->activePlan;
    const auto *data = plan.d.constData();
    const auto *const planIdentity = data;
    const Phase expectedPhase = m_runtime->phase;
    if (linkId != data->vehicle.endpoint.linkId
        || linkSessionEpoch != data->vehicle.linkSessionEpoch
        || message.sysid != data->vehicle.endpoint.systemId
        || message.compid != MAV_COMP_ID_LOG
        || message.len < 6) return;

    mavlink_remote_log_data_block_t block{};
    mavlink_msg_remote_log_data_block_decode(&message, &block);
    if (block.target_system != data->localSystemId
        || block.target_component != data->localComponentId) return;
    const QByteArray payload(
        reinterpret_cast<const char *>(block.data),
        RemoteDataFlashLogWriter::BlockBytes);

    QString routeError;
    const QPointer<RemoteDataFlashLogService> guard(this);
    if (!validateVehicle(data->target, data->vehicle, false, &routeError)
        || !guard) {
        if (guard && operationIsCurrent(operationId)) {
            beginDiscard(Outcome::LeaseRetired, routeError, false);
        }
        return;
    }
    if (!operationIsCurrent(operationId)
        || m_runtime->phase != expectedPhase
        || m_runtime->activePlan.d.constData() != planIdentity
        || !m_runtime->writer) return;
    if (m_runtime->pendingBlocks.contains(block.seqno)) {
        if (m_runtime->pendingBlocks.value(block.seqno) != payload) {
            beginDiscard(Outcome::ProtocolError,
                         QStringLiteral(
                             "Conflicting bytes arrived for a block still pending disk storage; the recovery capture was stopped."),
                         false);
        }
        // The original queued copy is not ACKed until blockStored. Repeated
        // 100 ms retransmissions therefore cannot fill the bounded disk queue.
        return;
    }
    if (!m_runtime->sequenceZeroObserved
        && !m_runtime->preSequenceZeroBlocks.contains(block.seqno)
        && m_runtime->preSequenceZeroBlocks.size()
               + m_runtime->pendingBlocks.size()
               >= MaximumPreSequenceZeroBlocks) {
        beginDiscard(Outcome::ProtocolError,
                     QStringLiteral(
                         "More than 256 blocks arrived before sequence zero; ownership could not be established."),
                     false);
        return;
    }
    m_runtime->pendingBlocks.insert(block.seqno, payload);
    if (!m_runtime->writer->append(block.seqno, payload)) {
        m_runtime->pendingBlocks.remove(block.seqno);
        m_runtime->discardInitiated = true;
        m_runtime->phase = Phase::Discarding;
        m_runtime->pendingOutcome = Outcome::IoError;
        m_runtime->pendingDescription = QStringLiteral(
            "The bounded staging writer rejected a remote log block; nothing was published.");
        m_runtime->status = m_runtime->pendingDescription;
        emit stateChanged();
    }
}

void RemoteDataFlashLogService::handleBlockStored(
    quint64 operationId, quint32 sequence, bool duplicate)
{
    if (!operationIsCurrent(operationId)) return;
    m_runtime->pendingBlocks.remove(sequence);
    m_runtime->lastStoredAtMs = m_runtime->nowMs();
    m_runtime->silenceAdvisory = false;
    if (duplicate) {
        ++m_runtime->duplicateBlocks;
    } else {
        ++m_runtime->blocksStored;
        m_runtime->highestSequence = m_runtime->hasBlocks
            ? qMax(m_runtime->highestSequence, sequence) : sequence;
        m_runtime->hasBlocks = true;
    }

    // Enforce absolute/first-block deadlines from the monotonic clock even if
    // the timer event is delayed behind disk or transport callbacks.
    const QPointer<RemoteDataFlashLogService> timeoutGuard(this);
    checkTimeouts();
    if (!timeoutGuard || !operationIsCurrent(operationId)) return;

    if (m_runtime->phase != Phase::AwaitingSequenceZero
        && m_runtime->phase != Phase::Receiving
        && m_runtime->phase != Phase::Stopping) {
        emit stateChanged();
        return;
    }
    if (!m_runtime->sequenceZeroObserved) {
        m_runtime->preSequenceZeroBlocks.insert(sequence);
        if (sequence != 0) {
            emit stateChanged();
            return;
        }
        m_runtime->sequenceZeroObserved = true;
        m_runtime->phase = Phase::Receiving;
        m_runtime->status = QStringLiteral(
            "Receiving remote DataFlash blocks. START remains unacknowledged by the protocol.");
        QList<quint32> sequences = m_runtime->preSequenceZeroBlocks.values();
        std::sort(sequences.begin(), sequences.end());
        sequences.removeAll(0);
        sequences.prepend(0);
        m_runtime->preSequenceZeroBlocks.clear();
        const QPointer<RemoteDataFlashLogService> guard(this);
        for (quint32 storedSequence : sequences) {
            const bool sent = sendStatus(operationId, storedSequence,
                                         MAV_REMOTE_LOG_DATA_BLOCK_ACK);
            if (!guard || !operationIsCurrent(operationId)) return;
            if (!sent) {
                if (guard && operationIsCurrent(operationId)) {
                    beginDiscard(Outcome::TransportFailed,
                                 QStringLiteral(
                                     "A stored remote log block could not be acknowledged; nothing was published."),
                                 false);
                }
                return;
            }
        }
    } else if (m_runtime->phase == Phase::Receiving) {
        const QPointer<RemoteDataFlashLogService> guard(this);
        const bool sent = sendStatus(operationId, sequence,
                                     MAV_REMOTE_LOG_DATA_BLOCK_ACK);
        if (!guard || !operationIsCurrent(operationId)) return;
        if (!sent) {
            if (guard && operationIsCurrent(operationId)) {
                beginDiscard(Outcome::TransportFailed,
                             QStringLiteral(
                                 "A stored remote log block could not be acknowledged; nothing was published."),
                             false);
            }
            return;
        }
    }
    if (!operationIsCurrent(operationId)) return;
    emit stateChanged();
}

bool RemoteDataFlashLogService::stopAndSave(
    quint64 operationId, QString *error)
{
    if (error) error->clear();
    if (!operationIsCurrent(operationId)
        || (m_runtime->phase != Phase::AwaitingSequenceZero
            && m_runtime->phase != Phase::Receiving)) {
        assignError(error, QStringLiteral(
            "The selected remote log operation is no longer active."));
        return false;
    }
    if (!m_runtime->sequenceZeroObserved) {
        const QString message = QStringLiteral(
            "Sequence zero was never stored. No STOP was sent because this application cannot claim ownership of the remote stream; flushed blocks are retained only as an unpublished .part recovery file and may belong to an earlier or competing client session.");
        beginDiscard(Outcome::StartUnconfirmed, message, false, true);
        return true;
    }
    const Plan plan = m_runtime->activePlan;
    const auto *const planIdentity = plan.d.constData();
    const Phase expectedPhase = m_runtime->phase;
    QString routeError;
    const QPointer<RemoteDataFlashLogService> guard(this);
    if (!validateVehicle(plan.d->target, plan.d->vehicle, false,
                         &routeError) || !guard) {
        if (guard && operationIsCurrent(operationId)
            && m_runtime->phase == expectedPhase
            && m_runtime->activePlan.d.constData() == planIdentity) {
            beginDiscard(Outcome::LeaseRetired, routeError, false);
        }
        assignError(error, routeError);
        return false;
    }

    if (!operationIsCurrent(operationId)
        || m_runtime->phase != expectedPhase
        || m_runtime->activePlan.d.constData() != planIdentity) {
        assignError(error, QStringLiteral(
            "The remote log operation changed during Stop validation."));
        return false;
    }

    m_runtime->phase = Phase::Stopping;
    m_runtime->saveRequested = true;
    m_runtime->stopRequestedAtMs = m_runtime->nowMs();
    m_runtime->status = QStringLiteral(
        "STOP submitted without protocol acknowledgement; capturing in-flight blocks before publication…");
    bool attempted = false;
    const bool sent = sendStatus(
        operationId, MAV_REMOTE_LOG_DATA_BLOCK_STOP,
        MAV_REMOTE_LOG_DATA_BLOCK_ACK, &attempted);
    if (!guard || !operationIsCurrent(operationId)
        || m_runtime->phase != Phase::Stopping
        || m_runtime->activePlan.d.constData() != planIdentity) return false;
    m_runtime->stopAttempted = attempted;
    m_runtime->stopSubmitted = sent;
    if (!sent) {
        m_runtime->status = attempted
            ? QStringLiteral(
                  "STOP transmission is uncertain; saving the local capture without claiming the remote logger stopped…")
            : QStringLiteral(
                  "STOP was rejected before transmission; saving the local capture without claiming the remote logger stopped…");
    }
    if (m_runtime->stopQuietMs == 0) {
        m_runtime->phase = Phase::Publishing;
        if (!m_runtime->writer || !m_runtime->writer->finish(true)) {
            beginDiscard(Outcome::IoError,
                         QStringLiteral(
                             "The staging writer could not begin publication."),
                         false);
            return false;
        }
    }
    emit stateChanged();
    return true;
}

bool RemoteDataFlashLogService::cancel(
    quint64 operationId, QString *error)
{
    const QPointer<RemoteDataFlashLogService> guard(this);
    if (error) error->clear();
    if (!operationIsCurrent(operationId)) {
        assignError(error, QStringLiteral(
            "The selected remote log operation is no longer active."));
        return false;
    }
    const bool shouldStop = m_runtime->sequenceZeroObserved
        && m_runtime->phase != Phase::Stopping
        && m_runtime->phase != Phase::Publishing;
    beginDiscard(Outcome::Cancelled,
                 QStringLiteral(
                     "Remote log capture cancelled; the staging file was discarded."),
                 shouldStop, false);
    return bool(guard);
}

void RemoteDataFlashLogService::beginDiscard(
    Outcome outcome, const QString &description, bool sendStopIfOwned,
    bool preservePartial)
{
    if (!m_runtime->operationId || m_runtime->finishing) return;
    if (m_runtime->discardInitiated) return;
    m_runtime->discardInitiated = true;
    const quint64 operationId = m_runtime->operationId;
    const auto *const planIdentity = m_runtime->activePlan.d.constData();
    m_runtime->phase = Phase::Discarding;
    m_runtime->saveRequested = false;
    m_runtime->pendingOutcome = outcome;
    m_runtime->pendingDescription = description;
    m_runtime->status = description;
    const QPointer<RemoteDataFlashLogService> guard(this);
    if (sendStopIfOwned && m_runtime->sequenceZeroObserved
        && m_runtime->activePlan.isValid()) {
        const Plan plan = m_runtime->activePlan;
        QString ignored;
        const bool routeValid = validateVehicle(
            plan.d->target, plan.d->vehicle, false, &ignored);
        if (!guard) return;
        if (routeValid && operationIsCurrent(operationId)) {
            bool attempted = false;
            const bool submitted = sendStatus(
                operationId, MAV_REMOTE_LOG_DATA_BLOCK_STOP,
                MAV_REMOTE_LOG_DATA_BLOCK_ACK, &attempted);
            if (!guard || !operationIsCurrent(operationId)) return;
            m_runtime->stopAttempted = m_runtime->stopAttempted || attempted;
            m_runtime->stopSubmitted = m_runtime->stopSubmitted || submitted;
        }
    }
    if (!guard) return;
    if (!operationIsCurrent(operationId)
        || m_runtime->phase != Phase::Discarding
        || m_runtime->activePlan.d.constData() != planIdentity) return;
    if (!m_runtime->writer || !m_runtime->writer->busy()
        || !m_runtime->writer->finish(false, preservePartial)) {
        finishReport(outcome, description);
        return;
    }
    emit stateChanged();
}

void RemoteDataFlashLogService::handleWriterFailed(
    quint64 operationId, const QString &reason)
{
    if (!operationIsCurrent(operationId)) return;
    if (m_runtime->phase != Phase::Discarding) {
        m_runtime->pendingOutcome = Outcome::IoError;
        m_runtime->pendingDescription = reason;
        m_runtime->phase = Phase::Discarding;
        m_runtime->discardInitiated = true;
        m_runtime->status = reason;
        emit stateChanged();
    }
}

void RemoteDataFlashLogService::handleWriterFinished(
    quint64 operationId, const RemoteDataFlashLogWriter::Result &result)
{
    if (!operationIsCurrent(operationId)) return;
    Report report;
    report.operationId = operationId;
    report.endpoint = m_runtime->activePlan.d.constData()->vehicle.endpoint;
    report.destinationPath = result.path;
    report.bytes = result.bytes;
    report.blocks = result.blocks;
    report.duplicateBlocks = result.duplicateBlocks;
    report.missingBlocks = result.missingBlocks;
    report.highestSequence = result.highestSequence;
    for (const auto &range : result.missingRanges) {
        report.missingRanges.append(MissingRange{range.first, range.last});
    }
    report.sequenceZeroObserved = m_runtime->sequenceZeroObserved;
    report.startAttempted = m_runtime->startWasAttempted();
    report.stopAttempted = m_runtime->stopWasAttempted();
    report.stopSubmitted = m_runtime->stopSubmitted;
    report.warnings = result.warnings;
    if (result.published && result.success) {
        report.outcome = Outcome::SavedUnverified;
        report.description = QStringLiteral(
            "Local remote DataFlash capture published. The protocol does not acknowledge STOP, flush, or end-of-file.");
        if (!m_runtime->stopSubmitted) {
            report.warnings.append(QStringLiteral(
                "STOP was not confirmed submitted; the remote logger may still be active."));
        }
    } else {
        report.outcome = m_runtime->pendingOutcome;
        report.description = m_runtime->pendingDescription.isEmpty()
            ? result.error : m_runtime->pendingDescription;
        if (!result.error.isEmpty()
            && result.error != report.description) {
            report.warnings.append(result.error);
        }
    }

    m_runtime->lastReport = report;
    appendHistory(QStringLiteral("%1: %2")
                      .arg(endpointText(report.endpoint), report.description));
    m_runtime->finishing = true;
    m_runtime->starting = false;
    m_runtime->operationId = 0;
    m_runtime->pendingPlan = {};
    m_runtime->activePlan = {};
    m_runtime->phase = Phase::Idle;
    m_runtime->status = report.description;
    m_runtime->preSequenceZeroBlocks.clear();
    m_runtime->pendingBlocks.clear();
    if (m_runtime->timeoutTimer) m_runtime->timeoutTimer->stop();
    const QPointer<RemoteDataFlashLogService> guard(this);
    emit stateChanged();
    if (!guard) return;
    emit operationFinished(report);
    if (!guard) return;
    m_runtime->finishing = false;
    emit stateChanged();
}

void RemoteDataFlashLogService::finishReport(
    Outcome outcome, const QString &description)
{
    if (!m_runtime->operationId) return;
    Report report;
    report.operationId = m_runtime->operationId;
    if (m_runtime->activePlan.isValid()) {
        report.endpoint = m_runtime->activePlan.d.constData()->vehicle.endpoint;
    }
    report.outcome = outcome;
    report.blocks = m_runtime->blocksStored;
    report.duplicateBlocks = m_runtime->duplicateBlocks;
    report.bytes = m_runtime->hasBlocks
        ? (qint64(m_runtime->highestSequence) + 1)
              * RemoteDataFlashLogWriter::BlockBytes
        : 0;
    report.highestSequence = m_runtime->highestSequence;
    report.sequenceZeroObserved = m_runtime->sequenceZeroObserved;
    report.startAttempted = m_runtime->startWasAttempted();
    report.stopAttempted = m_runtime->stopWasAttempted();
    report.stopSubmitted = m_runtime->stopSubmitted;
    report.description = description;
    m_runtime->lastReport = report;
    if (report.endpoint.isValid()) {
        appendHistory(QStringLiteral("%1: %2")
                          .arg(endpointText(report.endpoint), description));
    }
    m_runtime->finishing = true;
    m_runtime->starting = false;
    m_runtime->operationId = 0;
    m_runtime->pendingPlan = {};
    m_runtime->activePlan = {};
    m_runtime->phase = Phase::Idle;
    m_runtime->status = description;
    m_runtime->preSequenceZeroBlocks.clear();
    m_runtime->pendingBlocks.clear();
    if (m_runtime->timeoutTimer) m_runtime->timeoutTimer->stop();
    const QPointer<RemoteDataFlashLogService> guard(this);
    emit stateChanged();
    if (!guard) return;
    emit operationFinished(report);
    if (!guard) return;
    m_runtime->finishing = false;
    emit stateChanged();
}

void RemoteDataFlashLogService::appendHistory(const QString &line)
{
    if (line.isEmpty()) return;
    m_runtime->history.append(line);
    while (m_runtime->history.size() > MaximumHistoryEntries) {
        m_runtime->history.removeFirst();
    }
}

bool RemoteDataFlashLogService::deadlineReached() const
{
    return m_runtime->operationId != 0 && m_runtime->admittedAtMs >= 0
        && m_runtime->nowMs() - m_runtime->admittedAtMs
               >= m_runtime->maximumSessionMs;
}

void RemoteDataFlashLogService::checkTimeouts()
{
    if (!m_runtime->operationId || !m_runtime->activePlan.isValid()
        || m_runtime->finishing) return;
    const quint64 operationId = m_runtime->operationId;
    if (m_runtime->phase == Phase::Stopping) {
        if (m_runtime->stopRequestedAtMs >= 0
            && m_runtime->nowMs() - m_runtime->stopRequestedAtMs
                   >= m_runtime->stopQuietMs) {
            m_runtime->phase = Phase::Publishing;
            m_runtime->status = QStringLiteral(
                "Draining flushed blocks and publishing the local capture…");
            if (!m_runtime->writer || !m_runtime->writer->finish(true)) {
                beginDiscard(Outcome::IoError,
                             QStringLiteral(
                                 "The staging writer could not begin publication."),
                             false);
            } else {
                emit stateChanged();
            }
        }
        return;
    }
    if (m_runtime->phase == Phase::Publishing
        || m_runtime->phase == Phase::Discarding) return;

    const Plan plan = m_runtime->activePlan;
    const auto *const planIdentity = plan.d.constData();
    const Phase expectedPhase = m_runtime->phase;
    QString routeError;
    const QPointer<RemoteDataFlashLogService> guard(this);
    const bool routeValid = validateVehicle(
        plan.d->target, plan.d->vehicle,
        m_runtime->phase == Phase::Opening, &routeError);
    if (!guard) return;
    if (!operationIsCurrent(operationId)
        || m_runtime->phase != expectedPhase
        || m_runtime->activePlan.d.constData() != planIdentity) return;
    if (!routeValid) {
        beginDiscard(Outcome::LeaseRetired, routeError, false);
        return;
    }
    if (deadlineReached()) {
        beginDiscard(Outcome::TimedOut,
                     QStringLiteral(
                         "The eight-hour remote log session limit expired; nothing was automatically published."),
                     m_runtime->sequenceZeroObserved);
        return;
    }
    const qint64 now = m_runtime->nowMs();
    if (m_runtime->phase == Phase::AwaitingSequenceZero
        && m_runtime->startSentAtMs >= 0
        && now - m_runtime->startSentAtMs
               >= m_runtime->firstBlockTimeoutMs) {
        beginDiscard(Outcome::StartUnconfirmed,
                     QStringLiteral(
                         "Sequence zero was not stored within the start deadline; ownership was not established and nothing was published. Another client using the same GCS identity may already own the logger; stop it manually or reboot before retrying. Any retained .part may contain that earlier or competing session's bytes."),
                     false);
        return;
    }
    if (m_runtime->phase == Phase::Receiving
        && m_runtime->lastStoredAtMs >= 0
        && now - m_runtime->lastStoredAtMs
               >= m_runtime->streamSilenceMs
        && !m_runtime->silenceAdvisory) {
        // LOG_DISARMED=0 legitimately produces no blocks while the vehicle is
        // idle.  Fresh heartbeat/route checks above are the liveness boundary;
        // silence alone is advisory and never destroys a capture.
        m_runtime->silenceAdvisory = true;
        m_runtime->status = QStringLiteral(
            "No remote log block has been stored for 15 seconds. The exact vehicle is still live; capture remains active until Stop or Cancel.");
        emit stateChanged();
    }
}

void RemoteDataFlashLogService::shutdown()
{
    if (m_runtime->shuttingDown) return;
    m_runtime->shuttingDown = true;
    if (m_runtime->operationId) {
        beginDiscard(Outcome::ShuttingDown,
                     QStringLiteral(
                         "Application shutdown stopped only the local capture and preserved any flushed blocks as an unpublished .part recovery file. No STOP was sent to the remote logger."),
                     false, true);
    }
}
