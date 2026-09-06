#include "ParameterRecoveryService.h"

#include "comm/VehicleTargetManager.h"

#include <QDateTime>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QSharedData>
#include <QThread>
#include <QTimer>

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
    if (error) {
        *error = message;
    }
}

bool validMavlinkParameterName(const QString &name)
{
    const QByteArray encoded = name.toLatin1();
    if (encoded.isEmpty() || encoded.size() > 16
        || QString::fromLatin1(encoded) != name) {
        return false;
    }
    for (const char byte : encoded) {
        const uchar value = static_cast<uchar>(byte);
        if (!((value >= 'A' && value <= 'Z')
              || (value >= '0' && value <= '9')
              || value == '_')) {
            return false;
        }
    }
    return true;
}

bool isSupportedClassicType(ParameterType type)
{
    return type >= ParameterType::UInt8
        && type <= ParameterType::Real32;
}

bool rawValueFitsType(double value, ParameterType type)
{
    if (!std::isfinite(value) || !isSupportedClassicType(type)) {
        return false;
    }
    if (type == ParameterType::Real32) {
        return std::isfinite(static_cast<float>(value));
    }
    if (std::trunc(value) != value) return false;
    double minimum = 0.0;
    double maximum = 0.0;
    switch (type) {
    case ParameterType::UInt8: maximum = 255.0; break;
    case ParameterType::Int8: minimum = -128.0; maximum = 127.0; break;
    case ParameterType::UInt16: maximum = 65535.0; break;
    case ParameterType::Int16: minimum = -32768.0; maximum = 32767.0; break;
    case ParameterType::UInt32:
        maximum = 4294967295.0;
        break;
    case ParameterType::Int32:
        minimum = -2147483648.0;
        maximum = 2147483647.0;
        break;
    default:
        return false;
    }
    return value >= minimum && value <= maximum;
}

bool isUncertainWriteResult(ParameterService::ExactTerminalResult result)
{
    using Result = ParameterService::ExactTerminalResult;
    return result == Result::WriteCancelledOutcomeUncertain
        || result == Result::WriteTimedOutOutcomeUncertain
        || result == Result::WriteTransportOutcomeUncertain
        || result == Result::WriteLeaseRetiredOutcomeUncertain
        || result == Result::WriteLinkForgottenOutcomeUncertain;
}

bool isTerminalReadContextLoss(ParameterService::ExactTerminalResult result)
{
    using Result = ParameterService::ExactTerminalResult;
    return result == Result::ReadLeaseRetired
        || result == Result::ReadLinkForgotten;
}
}

class ParameterRecoveryService::Plan::Data final : public QSharedData
{
public:
    quint64 planId = 0;
    QString sourcePath;
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    QVector<ConfigRawParamsFileCodec::Entry> entries;
};

ParameterRecoveryService::Plan::Plan() = default;
ParameterRecoveryService::Plan::Plan(const Plan &other) = default;
ParameterRecoveryService::Plan &ParameterRecoveryService::Plan::operator=(
    const Plan &other) = default;
ParameterRecoveryService::Plan::~Plan() = default;

bool ParameterRecoveryService::Plan::isValid() const noexcept
{
    return d && d->planId != 0 && !d->sourcePath.isEmpty()
        && d->target.isValid() && d->vehicle.isValid()
        && !d->entries.isEmpty();
}

QString ParameterRecoveryService::Plan::sourcePath() const
{
    return d ? d->sourcePath : QString();
}

VehicleTargetLease ParameterRecoveryService::Plan::target() const
{
    return d ? d->target : VehicleTargetLease();
}

SwarmVehicleInstanceLease ParameterRecoveryService::Plan::vehicle() const
{
    return d ? d->vehicle : SwarmVehicleInstanceLease();
}

QVector<ConfigRawParamsFileCodec::Entry>
ParameterRecoveryService::Plan::entries() const
{
    return d ? d->entries
             : QVector<ConfigRawParamsFileCodec::Entry>();
}

struct ParameterRecoveryService::Runtime
{
    enum class Stage {
        Idle,
        PrefetchRead,
        EnableRead,
        EnableWrite,
        MainRead,
        IdentifierResetWrite,
        MainWrite
    };

    QPointer<VehicleTargetManager> targetManager;
    QPointer<SwarmTelemetryRegistry> telemetryRegistry;
    QPointer<ParameterService> parameterService;
    QPointer<VehicleCommandService> commandService;
    RouteValidator routeValidator;

    Plan pendingPlan;
    Plan activePlan;
    quint64 nextPlanId = 1;
    quint64 nextOperationId = 1;
    quint64 operationId = 0;
    ParameterService::ExactReservationToken parameterReservation;
    ParameterService::ExactOperationToken parameterOperation;
    // Valid only while submitExactRead/Write is on this thread's stack.  The
    // callee publishes this token before any writer callback or terminal
    // signal, allowing a nested event loop to correlate the terminal report
    // before submit returns.
    ParameterService::ExactOperationToken *submissionOperationOut = nullptr;
    QList<ParameterService::ExactOperationReport> deferredReports;
    VehicleCommandService::ExactReservationToken commandReservation;
    Stage stage = Stage::Idle;
    int entryIndex = 0;
    int progressCompleted = 0;
    int progressTotal = 0;
    ParameterType currentType = ParameterType::Unknown;
    QVariant currentValue;
    QVariant currentDesiredValue;
    Receipt::Kind currentReceiptKind = Receipt::Kind::ParameterWrite;
    bool busy = false;
    bool apiInFlight = false;
    bool finishing = false;
    bool shuttingDown = false;
    bool pumpScheduled = false;
    bool submissionInFlight = false;
    bool cancelRequested = false;
    bool deadlineExpired = false;
    int overallDeadlineMs = DefaultOverallDeadlineMs;
    QElapsedTimer overallClock;
    QTimer deadlineTimer;
    QString status = QStringLiteral("Ready.");
    QStringList history;
    Report report;
    Report lastReport;
    QSet<QString> failedNames;
};

ParameterRecoveryService::ParameterRecoveryService(
    VehicleTargetManager *targetManager,
    SwarmTelemetryRegistry *telemetryRegistry,
    ParameterService *parameterService,
    VehicleCommandService *commandService,
    RouteValidator routeValidator,
    QObject *parent)
    : QObject(parent)
    , m_runtime(new Runtime)
{
    m_runtime->targetManager = targetManager;
    m_runtime->telemetryRegistry = telemetryRegistry;
    m_runtime->parameterService = parameterService;
    m_runtime->commandService = commandService;
    m_runtime->routeValidator = std::move(routeValidator);
    m_runtime->deadlineTimer.setSingleShot(true);

    qRegisterMetaType<Outcome>();
    qRegisterMetaType<Receipt>();
    qRegisterMetaType<Report>();

    connect(&m_runtime->deadlineTimer, &QTimer::timeout,
            this, &ParameterRecoveryService::handleDeadline);
    if (m_runtime->parameterService) {
        connect(m_runtime->parameterService,
                &ParameterService::exactOperationFinished,
                this,
                &ParameterRecoveryService::handleParameterFinished,
                Qt::QueuedConnection);
    }
    if (m_runtime->targetManager) {
        connect(m_runtime->targetManager,
                &VehicleTargetManager::currentTargetChanged,
                this, &ParameterRecoveryService::stateChanged);
        connect(m_runtime->targetManager,
                &VehicleTargetManager::targetGenerationSettled,
                this, [this]() { emit stateChanged(); });
    }
    if (m_runtime->telemetryRegistry) {
        connect(m_runtime->telemetryRegistry,
                &SwarmTelemetryRegistry::registryChanged,
                this, [this]() { emit stateChanged(); });
    }
}

ParameterRecoveryService::~ParameterRecoveryService()
{
    shutdown();
}

bool ParameterRecoveryService::busy() const noexcept
{
    return m_runtime->busy;
}

QString ParameterRecoveryService::status() const
{
    return m_runtime->status;
}

QStringList ParameterRecoveryService::history() const
{
    return m_runtime->history;
}

ParameterRecoveryService::Report ParameterRecoveryService::lastReport() const
{
    return m_runtime->lastReport;
}

quint64 ParameterRecoveryService::currentOperationId() const noexcept
{
    return m_runtime->operationId;
}

int ParameterRecoveryService::progressCompleted() const noexcept
{
    return m_runtime->progressCompleted;
}

int ParameterRecoveryService::progressTotal() const noexcept
{
    return m_runtime->progressTotal;
}

void ParameterRecoveryService::setOverallDeadlineForTesting(int timeoutMs)
{
    if (!m_runtime->busy) {
        m_runtime->overallDeadlineMs = qBound(
            1, timeoutMs, DefaultOverallDeadlineMs);
    }
}

bool ParameterRecoveryService::captureVehicle(
    VehicleTargetLease *target,
    SwarmVehicleInstanceLease *vehicle,
    QString *error) const
{
    if (target) *target = {};
    if (vehicle) *vehicle = {};
    if (QThread::currentThread() != thread()) {
        assignError(error,
                    QStringLiteral("Parameter recovery must run on its owner thread."));
        return false;
    }
    if (m_runtime->shuttingDown || !m_runtime->targetManager
        || !m_runtime->telemetryRegistry || !m_runtime->parameterService
        || !m_runtime->commandService || !m_runtime->routeValidator) {
        assignError(error,
                    QStringLiteral("Parameter recovery services are unavailable."));
        return false;
    }
    const VehicleTargetLease capturedTarget =
        m_runtime->targetManager->acquireTarget();
    if (!capturedTarget.isValid()) {
        assignError(error, QStringLiteral("Select a connected vehicle first."));
        return false;
    }
    const SwarmVehicleInstanceLease capturedVehicle =
        m_runtime->telemetryRegistry->acquireVehicle(
            capturedTarget.endpoint, MaximumHeartbeatAgeMs);
    if (!capturedVehicle.isValid()
        || !validateVehicle(capturedTarget, capturedVehicle, error)) {
        if (!capturedVehicle.isValid() && error && error->isEmpty()) {
            *error = QStringLiteral(
                "The selected vehicle has no fresh exact telemetry.");
        }
        return false;
    }
    if (target) *target = capturedTarget;
    if (vehicle) *vehicle = capturedVehicle;
    return true;
}

bool ParameterRecoveryService::validateVehicle(
    const VehicleTargetLease &target,
    const SwarmVehicleInstanceLease &vehicle,
    QString *error) const
{
    const QPointer<const ParameterRecoveryService> serviceGuard(this);
    if (QThread::currentThread() != thread()
        || m_runtime->shuttingDown || !m_runtime->targetManager
        || !m_runtime->telemetryRegistry || !m_runtime->parameterService
        || !m_runtime->commandService || !m_runtime->routeValidator) {
        assignError(error,
                    QStringLiteral("Parameter recovery services are unavailable."));
        return false;
    }
    if (!target.isValid() || !vehicle.isValid()
        || !target.endpoint.sameIdentity(vehicle.endpoint)
        || target.endpoint.componentId != MAV_COMP_ID_AUTOPILOT1) {
        assignError(error,
                    QStringLiteral("The captured autopilot target is invalid."));
        return false;
    }

    const auto stateIsSafe = [this, &target, &vehicle](QString *why) {
        if (m_runtime->shuttingDown || !m_runtime->targetManager
            || !m_runtime->telemetryRegistry
            || !m_runtime->targetManager->isTargetGenerationSettled()
            || !m_runtime->targetManager->isCurrentTarget(
                target.endpoint.linkId, target.endpoint.systemId,
                target.endpoint.componentId, target.generation)
            || !m_runtime->targetManager->hasFreshHeartbeat(
                target, MaximumHeartbeatAgeMs)
            || m_runtime->targetManager->heartbeatArmed(target)
            || m_runtime->targetManager->heartbeatAutopilot(target)
                != MAV_AUTOPILOT_ARDUPILOTMEGA
            || !m_runtime->telemetryRegistry->validateLease(
                vehicle, MaximumHeartbeatAgeMs)) {
            assignError(why, QStringLiteral(
                "The selected ArduPilot vehicle changed, became stale, or is armed."));
            return false;
        }
        SwarmTelemetrySnapshot snapshot;
        if (!m_runtime->telemetryRegistry->snapshotForLease(
                vehicle, &snapshot)
            || !snapshot.heartbeatValid || snapshot.armed
            || snapshot.autopilot != MAV_AUTOPILOT_ARDUPILOTMEGA
            || !m_runtime->telemetryRegistry->observationIsFresh(
                snapshot.heartbeatObservedMs, MaximumHeartbeatAgeMs)) {
            assignError(why, QStringLiteral(
                "Fresh disarmed ArduPilot telemetry is required."));
            return false;
        }
        int vehiclesOnLink = 0;
        bool capturedPresent = false;
        const QList<VehicleEndpoint> endpoints =
            m_runtime->telemetryRegistry->endpoints();
        for (const VehicleEndpoint &candidate : endpoints) {
            if (candidate.linkId != vehicle.endpoint.linkId) continue;
            ++vehiclesOnLink;
            capturedPresent = capturedPresent
                || candidate.sameIdentity(vehicle.endpoint);
        }
        if (vehiclesOnLink != 1 || !capturedPresent) {
            assignError(why, QStringLiteral(
                "Parameter recovery requires one exact vehicle on the physical link."));
            return false;
        }
        return true;
    };

    if (!stateIsSafe(error)) return false;
    const RouteValidator validator = m_runtime->routeValidator;
    QString routeError;
    const bool routeAllowed = validator(vehicle, &routeError);
    if (!serviceGuard) return false;
    if (!routeAllowed) {
        assignError(error, routeError.isEmpty()
            ? QStringLiteral("No safe exact parameter recovery route is available.")
            : routeError);
        return false;
    }
    if (!serviceGuard) return false;
    return stateIsSafe(error);
}

bool ParameterRecoveryService::canPrepare(QString *error) const
{
    if (error) error->clear();
    if (m_runtime->busy || m_runtime->apiInFlight
        || m_runtime->finishing) {
        assignError(error,
                    QStringLiteral("Another parameter recovery is active."));
        return false;
    }
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    return captureVehicle(&target, &vehicle, error);
}

bool ParameterRecoveryService::prepare(
    const QString &path, Plan *planOut, QString *error)
{
    const QPointer<ParameterRecoveryService> guard(this);
    if (planOut) *planOut = {};
    if (error) error->clear();
    if (!planOut) {
        assignError(error, QStringLiteral("A plan output is required."));
        return false;
    }
    if (m_runtime->busy || m_runtime->apiInFlight
        || m_runtime->finishing) {
        assignError(error,
                    QStringLiteral("Another parameter recovery is active."));
        return false;
    }
    m_runtime->apiInFlight = true;
    ScopeExit apiGuard([guard]() {
        if (guard) {
            guard->m_runtime->apiInFlight = false;
            emit guard->stateChanged();
        }
    });

    QFileInfo before(path);
    if (!before.exists() || !before.isFile() || before.size() < 0
        || before.size() > MaximumSourceBytes) {
        assignError(error, QStringLiteral(
            "Choose a readable parameter file no larger than 1 MiB."));
        return false;
    }
    const qint64 sourceSize = before.size();
    const QDateTime sourceModified = before.lastModified();
    QFile file(before.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        assignError(error, file.errorString());
        return false;
    }
    const QByteArray sourceBytes = file.read(MaximumSourceBytes + 1);
    if (file.error() != QFileDevice::NoError
        || sourceBytes.size() > MaximumSourceBytes
        || !file.atEnd()) {
        assignError(error, file.error() != QFileDevice::NoError
            ? file.errorString()
            : QStringLiteral("The parameter file grew beyond the 1 MiB limit."));
        return false;
    }
    file.close();
    QBuffer sourceBuffer;
    sourceBuffer.setData(sourceBytes);
    if (!sourceBuffer.open(QIODevice::ReadOnly)) {
        assignError(error,
                    QStringLiteral("The parameter file buffer could not be opened."));
        return false;
    }
    QVector<ConfigRawParamsFileCodec::Entry> entries;
    int errorLine = 0;
    QString parseError;
    if (!ConfigRawParamsFileCodec::loadOrdered(
            &sourceBuffer, &entries, &parseError, &errorLine)) {
        assignError(error, errorLine > 0
            ? QStringLiteral("%1 (line %2)").arg(parseError).arg(errorLine)
            : parseError);
        return false;
    }
    QFileInfo after(before.absoluteFilePath());
    after.refresh();
    if (!after.exists() || !after.isFile()
        || after.size() != sourceSize
        || after.lastModified() != sourceModified) {
        assignError(error,
                    QStringLiteral("The parameter file changed while it was being read."));
        return false;
    }
    if (entries.isEmpty() || entries.size() > MaximumEntries) {
        assignError(error, entries.isEmpty()
            ? QStringLiteral("The parameter file contains no restorable values.")
            : QStringLiteral("The parameter file exceeds the 10000-entry recovery limit."));
        return false;
    }
    for (const auto &entry : entries) {
        if (!validMavlinkParameterName(entry.name)
            || !std::isfinite(entry.value)) {
            assignError(error, QStringLiteral(
                "Parameter name '%1' is not a valid 16-byte MAVLink parameter name.")
                .arg(entry.name));
            return false;
        }
    }

    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    if (!captureVehicle(&target, &vehicle, error) || !guard) {
        return false;
    }
    if (m_runtime->nextPlanId == 0) {
        assignError(error,
                    QStringLiteral("Parameter recovery plan identifiers are exhausted."));
        return false;
    }

    Plan captured;
    captured.d = new Plan::Data;
    captured.d->planId = m_runtime->nextPlanId++;
    captured.d->sourcePath = after.canonicalFilePath();
    if (captured.d->sourcePath.isEmpty()) {
        captured.d->sourcePath = after.absoluteFilePath();
    }
    captured.d->target = target;
    captured.d->vehicle = vehicle;
    captured.d->entries = entries;
    m_runtime->pendingPlan = captured;
    *planOut = captured;
    m_runtime->status = QStringLiteral(
        "Recovery plan prepared for %1 parameters on %2.")
        .arg(entries.size()).arg(vehicle.endpoint.displayName());
    return true;
}

bool ParameterRecoveryService::planMatchesPending(const Plan &plan) const
{
    return plan.isValid() && m_runtime->pendingPlan.isValid()
        && plan.d.constData() == m_runtime->pendingPlan.d.constData();
}

bool ParameterRecoveryService::validate(
    const Plan &plan, QString *error) const
{
    const Plan submittedPlan = plan;
    if (error) error->clear();
    if (m_runtime->busy || m_runtime->apiInFlight
        || m_runtime->finishing || !planMatchesPending(submittedPlan)) {
        assignError(error, QStringLiteral(
            "The captured parameter recovery plan is stale or another recovery is active."));
        return false;
    }
    const QPointer<const ParameterRecoveryService> guard(this);
    const bool validVehicle =
        validateVehicle(submittedPlan.d->target,
                        submittedPlan.d->vehicle, error);
    if (!guard) return false;
    if (!validVehicle) return false;
    if (!planMatchesPending(submittedPlan)) {
        assignError(error, QStringLiteral(
            "The captured parameter recovery plan changed during validation."));
        return false;
    }
    return true;
}

ParameterRecoveryService::SubmitResult ParameterRecoveryService::execute(
    const Plan &plan, QString *error)
{
    return execute(plan, nullptr, error);
}

ParameterRecoveryService::SubmitResult ParameterRecoveryService::execute(
    const Plan &plan, quint64 *operationIdOut, QString *error)
{
    const QPointer<ParameterRecoveryService> guard(this);
    const Plan submittedPlan = plan;
    if (operationIdOut) *operationIdOut = 0;
    if (error) error->clear();
    if (m_runtime->busy || m_runtime->apiInFlight
        || m_runtime->finishing) {
        assignError(error,
                    QStringLiteral("Another parameter recovery is active."));
        return SubmitResult::Busy;
    }
    if (!planMatchesPending(submittedPlan)) {
        assignError(error,
                    QStringLiteral("The captured parameter recovery plan is stale."));
        return SubmitResult::InvalidPlan;
    }
    if (m_runtime->nextOperationId == 0) {
        assignError(error, QStringLiteral(
            "Parameter recovery operation identifiers are exhausted."));
        return SubmitResult::Unavailable;
    }
    m_runtime->apiInFlight = true;
    ScopeExit apiGuard([guard]() {
        if (guard) {
            guard->m_runtime->apiInFlight = false;
            emit guard->stateChanged();
        }
    });
    // Admit and publish ownership before the first external validator.  A UI
    // may close from that callback; operationIdOut is never touched again.
    m_runtime->activePlan = submittedPlan;
    m_runtime->pendingPlan = {};
    m_runtime->operationId = m_runtime->nextOperationId++;
    const quint64 submittedOperationId = m_runtime->operationId;
    if (operationIdOut) *operationIdOut = m_runtime->operationId;
    m_runtime->busy = true;
    m_runtime->cancelRequested = false;
    m_runtime->deadlineExpired = false;
    m_runtime->stage = Runtime::Stage::Idle;
    m_runtime->entryIndex = 0;
    m_runtime->progressCompleted = 0;
    m_runtime->progressTotal = submittedPlan.d->entries.size();
    m_runtime->failedNames.clear();
    m_runtime->report = {};
    m_runtime->report.operationId = m_runtime->operationId;
    m_runtime->report.sourcePath = submittedPlan.d->sourcePath;
    m_runtime->report.endpoint = submittedPlan.d->vehicle.endpoint;
    m_runtime->report.totalEntries = submittedPlan.d->entries.size();
    m_runtime->status = QStringLiteral("Reserving the exact recovery route...");
    appendHistory(m_runtime->status);
    m_runtime->overallClock.restart();
    m_runtime->deadlineTimer.start(m_runtime->overallDeadlineMs);

    QString reservationError;
    const bool initiallyValid = validateVehicle(
        submittedPlan.d->target, submittedPlan.d->vehicle,
        &reservationError);
    if (!guard) return SubmitResult::Unavailable;
    if (!m_runtime->busy
        || m_runtime->operationId != submittedOperationId) {
        return SubmitResult::Started;
    }
    if (m_runtime->cancelRequested || deadlineReached()) {
        m_runtime->deadlineExpired = deadlineReached();
        finish(m_runtime->deadlineExpired ? Outcome::Rejected
                                          : Outcome::Cancelled,
               m_runtime->deadlineExpired
                   ? QStringLiteral("The bounded parameter recovery deadline expired during validation.")
                   : QStringLiteral("Parameter recovery was cancelled during validation."));
        return SubmitResult::Started;
    }
    if (!initiallyValid) {
        assignError(error, reservationError);
        finish(Outcome::Rejected, reservationError.isEmpty()
            ? QStringLiteral("The exact recovery target failed final validation.")
            : reservationError);
        return SubmitResult::Unavailable;
    }

    VehicleCommandService::ExactReservationToken commandReservation;
    const auto commandResult =
        m_runtime->commandService->reserveSingleVehicleEndpoint(
            this, submittedPlan.d->target, submittedPlan.d->vehicle,
            &commandReservation, &reservationError);
    if (!guard) return SubmitResult::Unavailable;
    if (m_runtime->shuttingDown || !m_runtime->busy
        || m_runtime->operationId != submittedOperationId) {
        if (commandReservation.isValid() && m_runtime->commandService) {
            m_runtime->commandService->releaseExactReservation(
                commandReservation);
        }
        return SubmitResult::Unavailable;
    }
    if (m_runtime->cancelRequested || deadlineReached()) {
        m_runtime->deadlineExpired = deadlineReached();
        m_runtime->commandReservation = commandReservation;
        finish(m_runtime->deadlineExpired ? Outcome::Rejected
                                          : Outcome::Cancelled,
               m_runtime->deadlineExpired
                   ? QStringLiteral("The bounded parameter recovery deadline expired during reservation.")
                   : QStringLiteral("Parameter recovery was cancelled during reservation."));
        return SubmitResult::Started;
    }
    if (commandResult
        != VehicleCommandService::ExactReservationResult::Reserved) {
        assignError(error, reservationError);
        finish(Outcome::Rejected, reservationError.isEmpty()
            ? QStringLiteral("The exact command safety lane could not be reserved.")
            : reservationError);
        return SubmitResult::Unavailable;
    }
    m_runtime->commandReservation = commandReservation;

    ParameterService::ExactReservationToken parameterReservation;
    const auto parameterResult =
        m_runtime->parameterService->reserveSingleVehicleEndpoint(
            this, submittedPlan.d->target, submittedPlan.d->vehicle,
            &parameterReservation, &reservationError);
    if (!guard) return SubmitResult::Unavailable;
    if (m_runtime->shuttingDown || !m_runtime->busy
        || m_runtime->operationId != submittedOperationId) {
        if (parameterReservation.isValid()
            && m_runtime->parameterService) {
            m_runtime->parameterService->releaseExactReservation(
                parameterReservation);
        }
        return SubmitResult::Unavailable;
    }
    if (m_runtime->cancelRequested || deadlineReached()) {
        m_runtime->deadlineExpired = deadlineReached();
        m_runtime->parameterReservation = parameterReservation;
        finish(m_runtime->deadlineExpired ? Outcome::Rejected
                                          : Outcome::Cancelled,
               m_runtime->deadlineExpired
                   ? QStringLiteral("The bounded parameter recovery deadline expired during reservation.")
                   : QStringLiteral("Parameter recovery was cancelled during reservation."));
        return SubmitResult::Started;
    }
    if (parameterResult
        != ParameterService::ExactReservationResult::Reserved) {
        assignError(error, reservationError);
        finish(Outcome::Rejected, reservationError.isEmpty()
            ? QStringLiteral("The exact parameter route could not be reserved.")
            : reservationError);
        return SubmitResult::Unavailable;
    }
    m_runtime->parameterReservation = parameterReservation;

    const bool stillValid = validateVehicle(
        submittedPlan.d->target, submittedPlan.d->vehicle,
        &reservationError);
    if (!guard) return SubmitResult::Unavailable;
    if (!stillValid || m_runtime->shuttingDown
        || !m_runtime->busy || m_runtime->cancelRequested
        || deadlineReached()
        || m_runtime->operationId != submittedOperationId
        || m_runtime->activePlan.d.constData()
            != submittedPlan.d.constData()) {
        const bool expired = deadlineReached();
        m_runtime->deadlineExpired = expired;
        assignError(error, reservationError);
        finish(m_runtime->cancelRequested ? Outcome::Cancelled
                                         : Outcome::Rejected,
               expired
                   ? QStringLiteral("The bounded parameter recovery deadline expired during reservation.")
                   : m_runtime->cancelRequested
                       ? QStringLiteral("Parameter recovery was cancelled during reservation.")
                       : reservationError.isEmpty()
                           ? QStringLiteral("The exact recovery target changed during reservation.")
                           : reservationError);
        return SubmitResult::Unavailable;
    }

    m_runtime->stage = Runtime::Stage::PrefetchRead;
    m_runtime->status = QStringLiteral("Reading current parameter types...");
    emit stateChanged();
    if (!guard) return SubmitResult::Unavailable;
    schedulePump();
    return SubmitResult::Started;
}

bool ParameterRecoveryService::activeContextValid(QString *error) const
{
    const Plan activePlan = m_runtime->activePlan;
    const quint64 operationId = m_runtime->operationId;
    if (deadlineReached()) {
        assignError(error,
                    QStringLiteral("The bounded parameter recovery deadline expired."));
        return false;
    }
    if (!m_runtime->busy || !activePlan.isValid()) return false;
    const QPointer<const ParameterRecoveryService> guard(this);
    const bool validVehicle = validateVehicle(
        activePlan.d->target, activePlan.d->vehicle, error);
    if (!guard) return false;
    return validVehicle && m_runtime->busy
        && m_runtime->operationId == operationId
        && m_runtime->activePlan.d.constData()
            == activePlan.d.constData()
        && !m_runtime->cancelRequested
        && !deadlineReached();
}

bool ParameterRecoveryService::deadlineReached() const
{
    return m_runtime->busy && m_runtime->overallClock.isValid()
        && m_runtime->overallClock.elapsed()
            >= m_runtime->overallDeadlineMs;
}

void ParameterRecoveryService::schedulePump()
{
    if (!m_runtime->busy || m_runtime->pumpScheduled
        || m_runtime->parameterOperation.isValid()) {
        return;
    }
    m_runtime->pumpScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_runtime->pumpScheduled = false;
        pump();
    });
}

void ParameterRecoveryService::pump()
{
    if (!m_runtime->busy || m_runtime->submissionInFlight
        || m_runtime->parameterOperation.isValid()) return;
    const quint64 pumpingOperationId = m_runtime->operationId;
    const Plan pumpingPlan = m_runtime->activePlan;
    if (m_runtime->cancelRequested) {
        finish(Outcome::Cancelled,
               QStringLiteral("Parameter recovery was cancelled before the next write."));
        return;
    }
    if (m_runtime->deadlineExpired) {
        finish(Outcome::Rejected,
               QStringLiteral("The bounded parameter recovery deadline expired."));
        return;
    }
    QString error;
    const QPointer<ParameterRecoveryService> guard(this);
    const bool contextValid = activeContextValid(&error);
    if (!guard) return;
    // The injected route validator may run a nested event loop.  It can retire
    // this run and admit a successor, so an old pump must never finish or
    // advance whichever operation happens to be active when it resumes.
    if (!m_runtime->busy
        || m_runtime->operationId != pumpingOperationId
        || m_runtime->activePlan.d.constData()
            != pumpingPlan.d.constData()) {
        return;
    }
    if (m_runtime->cancelRequested) {
        finish(Outcome::Cancelled,
               QStringLiteral("Parameter recovery was cancelled before the next write."));
        return;
    }
    if (deadlineReached()) {
        m_runtime->deadlineExpired = true;
    }
    if (m_runtime->deadlineExpired) {
        finish(Outcome::Rejected,
               QStringLiteral("The bounded parameter recovery deadline expired."));
        return;
    }
    if (!contextValid) {
        finish(Outcome::Rejected, error.isEmpty()
            ? QStringLiteral("The exact recovery target is no longer safe.")
            : error);
        return;
    }

    const auto &entries =
        m_runtime->activePlan.d.constData()->entries;
    switch (m_runtime->stage) {
    case Runtime::Stage::PrefetchRead:
        if (m_runtime->entryIndex >= entries.size()) {
            m_runtime->entryIndex = 0;
            m_runtime->stage = Runtime::Stage::EnableRead;
            m_runtime->status = QStringLiteral(
                "Applying ENABLE parameters before the recovery pass...");
            schedulePump();
            return;
        }
        submitRead(entries.at(m_runtime->entryIndex));
        return;
    case Runtime::Stage::EnableRead:
        while (m_runtime->entryIndex < entries.size()
               && !entries.at(m_runtime->entryIndex).name.contains(
                   QStringLiteral("ENABLE"), Qt::CaseInsensitive)) {
            ++m_runtime->entryIndex;
        }
        if (m_runtime->entryIndex >= entries.size()) {
            m_runtime->entryIndex = 0;
            m_runtime->stage = Runtime::Stage::MainRead;
            m_runtime->status = QStringLiteral(
                "Applying parameters in source-file order...");
            schedulePump();
            return;
        }
        submitRead(entries.at(m_runtime->entryIndex));
        return;
    case Runtime::Stage::EnableWrite:
        submitWrite(entries.at(m_runtime->entryIndex),
                    entries.at(m_runtime->entryIndex).value,
                    m_runtime->currentType,
                    Receipt::Kind::EnableWrite);
        return;
    case Runtime::Stage::MainRead:
        if (m_runtime->entryIndex >= entries.size()) {
            const QString description = QStringLiteral(
                "Recovery completed: %1 set, %2 unchanged, %3 failed.")
                .arg(m_runtime->report.setCount)
                .arg(m_runtime->report.unchangedCount)
                .arg(m_runtime->report.failedCount);
            finish(Outcome::Completed, description);
            return;
        }
        submitRead(entries.at(m_runtime->entryIndex));
        return;
    case Runtime::Stage::IdentifierResetWrite:
        submitWrite(entries.at(m_runtime->entryIndex), 0,
                    m_runtime->currentType,
                    Receipt::Kind::IdentifierReset);
        return;
    case Runtime::Stage::MainWrite:
        submitWrite(entries.at(m_runtime->entryIndex),
                    entries.at(m_runtime->entryIndex).value,
                    m_runtime->currentType,
                    Receipt::Kind::ParameterWrite);
        return;
    case Runtime::Stage::Idle:
        finish(Outcome::Rejected,
               QStringLiteral("The parameter recovery state is invalid."));
        return;
    }
}

void ParameterRecoveryService::submitRead(
    const ConfigRawParamsFileCodec::Entry &entry)
{
    const QPointer<ParameterRecoveryService> guard(this);
    const ConfigRawParamsFileCodec::Entry submittedEntry = entry;
    const Plan submittedPlan = m_runtime->activePlan;
    const quint64 submittedOperationId = m_runtime->operationId;
    const auto reservation = m_runtime->parameterReservation;
    ParameterService::ExactReadRequest request;
    request.name = submittedEntry.name;
    ParameterService::ExactOperationToken operation;
    QString error;
    const QPointer<ParameterService> parameterService =
        m_runtime->parameterService;
    m_runtime->submissionInFlight = true;
    m_runtime->submissionOperationOut = &operation;
    const auto result = parameterService->submitExactRead(
        reservation, submittedPlan.d->vehicle,
        request, &operation, &error);
    if (!guard) return;
    m_runtime->submissionOperationOut = nullptr;
    m_runtime->submissionInFlight = false;
    const bool stillOwnsRecovery = !m_runtime->shuttingDown
        && m_runtime->busy
        && m_runtime->operationId == submittedOperationId
        && m_runtime->activePlan.d.constData()
            == submittedPlan.d.constData();
    if (!stillOwnsRecovery) {
        if (result == ParameterService::ExactSubmitResult::Started
            && operation.isValid() && parameterService) {
            parameterService->cancelExactOperation(
                reservation, operation,
                QStringLiteral("Parameter recovery retired during read submission."));
        }
        scheduleDeferredReports();
        return;
    }
    if (m_runtime->cancelRequested || deadlineReached()) {
        m_runtime->deadlineExpired = deadlineReached();
        if (result == ParameterService::ExactSubmitResult::Started
            && operation.isValid()) {
            m_runtime->parameterOperation = operation;
            parameterService->cancelExactOperation(
                reservation, operation,
                QStringLiteral("Parameter recovery retired during read submission."));
            scheduleDeferredReports();
            return; // The immutable queued terminal report settles the run.
        }
        scheduleDeferredReports();
        schedulePump();
        return;
    }
    if (result == ParameterService::ExactSubmitResult::Started) {
        m_runtime->parameterOperation = operation;
        scheduleDeferredReports();
        m_runtime->status = QStringLiteral("Reading %1...")
            .arg(submittedEntry.name);
        emit stateChanged();
        return;
    }

    const bool fatal = result == ParameterService::ExactSubmitResult::InvalidOwner
        || result == ParameterService::ExactSubmitResult::InvalidReservation
        || result == ParameterService::ExactSubmitResult::InvalidLease
        || result == ParameterService::ExactSubmitResult::StaleLease
        || result == ParameterService::ExactSubmitResult::RouteUnavailable
        || result == ParameterService::ExactSubmitResult::Busy
        || result == ParameterService::ExactSubmitResult::ContextUnavailable;
    if (fatal) {
        finish(Outcome::Rejected, error.isEmpty()
            ? QStringLiteral("The exact parameter read route became unavailable.")
            : error);
        return;
    }
    appendHistory(QStringLiteral("Read %1 failed before transmission: %2")
                  .arg(submittedEntry.name, error));
    if (m_runtime->stage == Runtime::Stage::PrefetchRead
        || m_runtime->stage == Runtime::Stage::EnableRead) {
        ++m_runtime->entryIndex;
        schedulePump();
    } else {
        recordFailure(submittedEntry.name, error);
        advanceMainEntry();
    }
}

void ParameterRecoveryService::submitWrite(
    const ConfigRawParamsFileCodec::Entry &entry,
    const QVariant &value, ParameterType type,
    Receipt::Kind receiptKind)
{
    const ConfigRawParamsFileCodec::Entry submittedEntry = entry;
    const Plan submittedPlan = m_runtime->activePlan;
    const quint64 submittedOperationId = m_runtime->operationId;
    const auto reservation = m_runtime->parameterReservation;
    const QPointer<ParameterService> parameterService =
        m_runtime->parameterService;
    QVariant normalizedValue;
    QString normalizationError;
    if (!rawValueFitsType(value.toDouble(), type)
        || !parameterService
        || !parameterService->normalizeExactValue(
            submittedPlan.d->vehicle, value, type,
            &normalizedValue, &normalizationError)) {
        const QString why = normalizationError.isEmpty()
            ? QStringLiteral("The requested value is incompatible with the live parameter type.")
            : normalizationError;
        if (m_runtime->stage == Runtime::Stage::EnableWrite) {
            appendHistory(QStringLiteral("ENABLE pre-pass write %1 failed: %2")
                          .arg(submittedEntry.name, why));
            ++m_runtime->entryIndex;
            m_runtime->stage = Runtime::Stage::EnableRead;
            schedulePump();
        } else {
            recordFailure(submittedEntry.name, why);
            advanceMainEntry();
        }
        return;
    }
    ParameterService::ExactWriteRequest request;
    request.name = submittedEntry.name;
    request.value = normalizedValue;
    request.type = type;
    request.force = true;
    const QPointer<ParameterRecoveryService> guard(this);
    request.validateBeforeWrite =
        [guard, parameterService, submittedOperationId,
         submittedPlan, submittedEntry, type,
         receiptKind](QString *error) {
            if (!guard || !guard->m_runtime->busy
                || guard->m_runtime->shuttingDown
                || guard->m_runtime->operationId != submittedOperationId
                || guard->m_runtime->cancelRequested
                || guard->m_runtime->deadlineExpired
                || guard->deadlineReached()
                || guard->m_runtime->activePlan.d.constData()
                    != submittedPlan.d.constData()) {
                assignError(error, QStringLiteral(
                    "The parameter recovery plan was retired before transmission."));
                return false;
            }
            const bool vehicleValid = guard->validateVehicle(
                submittedPlan.d->target, submittedPlan.d->vehicle, error);
            if (!guard || !vehicleValid) return false;
            if (!guard->m_runtime->busy
                || guard->m_runtime->shuttingDown
                || guard->m_runtime->operationId != submittedOperationId
                || guard->m_runtime->cancelRequested
                || guard->m_runtime->deadlineExpired
                || guard->deadlineReached()
                || guard->m_runtime->activePlan.d.constData()
                    != submittedPlan.d.constData()) {
                assignError(error, QStringLiteral(
                    "The parameter recovery plan changed during final safety validation."));
                return false;
            }
            if (receiptKind == Receipt::Kind::IdentifierReset) {
                QVariant normalizedTarget;
                QString normalizationFailure;
                if (!rawValueFitsType(submittedEntry.value, type)
                    || !parameterService
                    || !parameterService->normalizeExactValue(
                        submittedPlan.d->vehicle,
                        submittedEntry.value, type,
                        &normalizedTarget, &normalizationFailure)) {
                    assignError(error, normalizationFailure.isEmpty()
                        ? QStringLiteral(
                            "The final identifier value is no longer representable; reset was not sent.")
                        : normalizationFailure);
                    return false;
                }
            }
            return true;
        };
    ParameterService::ExactOperationToken operation;
    QString error;
    m_runtime->currentReceiptKind = receiptKind;
    m_runtime->submissionInFlight = true;
    m_runtime->submissionOperationOut = &operation;
    const auto result = parameterService->submitExactWrite(
        reservation, submittedPlan.d->vehicle,
        request, &operation, &error);
    if (!guard) return;
    m_runtime->submissionOperationOut = nullptr;
    m_runtime->submissionInFlight = false;
    const bool stillOwnsRecovery = !m_runtime->shuttingDown
        && m_runtime->busy
        && m_runtime->operationId == submittedOperationId
        && m_runtime->activePlan.d.constData()
            == submittedPlan.d.constData();
    if (!stillOwnsRecovery) {
        if (result == ParameterService::ExactSubmitResult::Started
            && operation.isValid() && parameterService) {
            parameterService->cancelExactOperation(
                reservation, operation,
                QStringLiteral("Parameter recovery retired during write submission."));
        }
        scheduleDeferredReports();
        return;
    }
    if (m_runtime->cancelRequested || deadlineReached()) {
        m_runtime->deadlineExpired = deadlineReached();
        if (result == ParameterService::ExactSubmitResult::Started
            && operation.isValid()) {
            m_runtime->parameterOperation = operation;
            parameterService->cancelExactOperation(
                reservation, operation,
                QStringLiteral("Parameter recovery retired during write submission."));
            scheduleDeferredReports();
            return; // Preserve a queued success or uncertainty report.
        }
        scheduleDeferredReports();
        schedulePump();
        return;
    }
    if (result == ParameterService::ExactSubmitResult::Started) {
        m_runtime->parameterOperation = operation;
        scheduleDeferredReports();
        m_runtime->status = QStringLiteral("Writing %1...")
            .arg(submittedEntry.name);
        emit stateChanged();
        return;
    }
    if (result == ParameterService::ExactSubmitResult::TransportOutcomeUncertain
        || result == ParameterService::ExactSubmitResult::Quarantined) {
        finish(Outcome::OutcomeUncertain, error.isEmpty()
            ? QStringLiteral("A parameter write has an uncertain outcome; recovery stopped.")
            : error + QStringLiteral(" Recovery stopped; do not retry automatically."));
        return;
    }
    const bool fatal = result == ParameterService::ExactSubmitResult::InvalidOwner
        || result == ParameterService::ExactSubmitResult::InvalidReservation
        || result == ParameterService::ExactSubmitResult::InvalidLease
        || result == ParameterService::ExactSubmitResult::StaleLease
        || result == ParameterService::ExactSubmitResult::RouteUnavailable
        || result == ParameterService::ExactSubmitResult::Busy
        || result == ParameterService::ExactSubmitResult::ContextUnavailable;
    if (fatal) {
        finish(Outcome::Rejected, error.isEmpty()
            ? QStringLiteral("The exact parameter write route became unavailable.")
            : error);
        return;
    }
    if (m_runtime->stage == Runtime::Stage::EnableWrite) {
        appendHistory(QStringLiteral("ENABLE pre-pass write %1 failed: %2")
                      .arg(submittedEntry.name, error));
        ++m_runtime->entryIndex;
        m_runtime->stage = Runtime::Stage::EnableRead;
        schedulePump();
    } else {
        recordFailure(submittedEntry.name, error);
        advanceMainEntry();
    }
}

void ParameterRecoveryService::handleParameterFinished(
    const ParameterService::ExactOperationReport &report)
{
    if (m_runtime->submissionInFlight) {
        const auto *submitted = m_runtime->submissionOperationOut;
        if (!m_runtime->busy || !m_runtime->activePlan.isValid()
            || !m_runtime->parameterReservation.isValid()
            || !submitted || !submitted->isValid()
            || report.token.operationId != submitted->operationId
            || report.token.reservationId != submitted->reservationId
            || report.token.kind != submitted->kind
            || report.token.name != submitted->name
            || !report.token.lease.sameInstance(submitted->lease)
            || report.token.reservationId
                != m_runtime->parameterReservation.reservationId
            || !report.token.lease.sameInstance(
                m_runtime->activePlan.d.constData()->vehicle)) {
            return;
        }
        // ParameterService admits only one operation globally.  Replace a
        // duplicate notification for this exact token instead of allowing
        // unrelated nested events to consume a bounded queue.
        m_runtime->deferredReports.clear();
        m_runtime->deferredReports.append(report);
        return;
    }
    if (!m_runtime->busy || !m_runtime->parameterOperation.isValid()
        || report.token.operationId
            != m_runtime->parameterOperation.operationId
        || report.token.reservationId
            != m_runtime->parameterReservation.reservationId
        || !report.token.lease.sameInstance(
            m_runtime->activePlan.d.constData()->vehicle)) {
        return;
    }
    m_runtime->parameterOperation = {};
    m_runtime->submissionInFlight = false;
    if (deadlineReached()) m_runtime->deadlineExpired = true;

    const auto stage = m_runtime->stage;
    const auto entry = m_runtime->activePlan.d.constData()->entries.at(
        m_runtime->entryIndex);
    const bool read = report.token.kind
        == ParameterService::ExactOperationKind::Read;

    // A mismatched PARAM_VALUE can be an old delayed read/write echo.  Once a
    // PARAM_SET was attempted it is not proof that the requested value was
    // rejected, so stop rather than treating that entry as a definite failure.
    if (isUncertainWriteResult(report.terminalResult)
        || (!read && report.frameAttempted
            && report.terminalResult
                == ParameterService::ExactTerminalResult::Rejected)) {
        finish(Outcome::OutcomeUncertain,
               (report.description.isEmpty()
                    ? QStringLiteral("A parameter write outcome is uncertain.")
                    : report.description)
               + QStringLiteral(
                    " Recovery stopped; preserve the receipts and do not retry automatically."));
        return;
    }
    if (read) {
        if (m_runtime->cancelRequested) {
            finish(Outcome::Cancelled,
                   QStringLiteral("Parameter recovery was cancelled; acknowledged receipts were preserved."));
            return;
        }
        if (m_runtime->deadlineExpired) {
            finish(Outcome::Rejected,
                   QStringLiteral("The bounded parameter recovery deadline expired."));
            return;
        }
        if (report.terminalResult
            == ParameterService::ExactTerminalResult::ReadSucceeded) {
            if (!isSupportedClassicType(report.type)) {
                const QString why = QStringLiteral(
                    "The vehicle reported an unsupported parameter type.");
                if (stage == Runtime::Stage::MainRead) {
                    recordFailure(entry.name, why);
                    advanceMainEntry();
                } else {
                    appendHistory(QStringLiteral("Read %1: %2")
                                  .arg(entry.name, why));
                    ++m_runtime->entryIndex;
                    schedulePump();
                }
                return;
            }
            if (stage == Runtime::Stage::PrefetchRead) {
                ++m_runtime->entryIndex;
                schedulePump();
                return;
            }
            if (stage == Runtime::Stage::EnableRead) {
                m_runtime->currentType = report.type;
                m_runtime->currentValue = report.value;
                m_runtime->stage = Runtime::Stage::EnableWrite;
                schedulePump();
                return;
            }
            if (stage == Runtime::Stage::MainRead) {
                m_runtime->currentType = report.type;
                m_runtime->currentValue = report.value;
                QVariant normalizedDesired;
                QString normalizationError;
                bool currentOk = false;
                const double current = report.value.toDouble(&currentOk);
                if (!currentOk || !std::isfinite(current)
                    || !rawValueFitsType(entry.value, report.type)
                    || !m_runtime->parameterService
                    || !m_runtime->parameterService->normalizeExactValue(
                        m_runtime->activePlan.d.constData()->vehicle,
                        entry.value, report.type,
                        &normalizedDesired, &normalizationError)) {
                    recordFailure(entry.name,
                                  normalizationError.isEmpty()
                                      ? QStringLiteral("The file value is incompatible with the live parameter type.")
                                      : normalizationError);
                    advanceMainEntry();
                    return;
                }
                m_runtime->currentDesiredValue = normalizedDesired;
                if (std::abs(current - entry.value) < 1.0e-9) {
                    ++m_runtime->report.unchangedCount;
                    appendHistory(QStringLiteral("%1 is unchanged.")
                                  .arg(entry.name));
                    advanceMainEntry();
                } else {
                    m_runtime->stage = entry.name.endsWith(
                        QStringLiteral("_ID"), Qt::CaseInsensitive)
                        ? Runtime::Stage::IdentifierResetWrite
                        : Runtime::Stage::MainWrite;
                    schedulePump();
                }
                return;
            }
        }

        if (isTerminalReadContextLoss(report.terminalResult)) {
            finish(Outcome::Rejected,
                   report.description.isEmpty()
                       ? QStringLiteral("The exact recovery target was lost during a read.")
                       : report.description);
            return;
        }
        const QString why = report.description.isEmpty()
            ? QStringLiteral("The exact parameter read failed.")
            : report.description;
        if (stage == Runtime::Stage::PrefetchRead
            || stage == Runtime::Stage::EnableRead) {
            appendHistory(QStringLiteral("Read %1 failed: %2")
                          .arg(entry.name, why));
            ++m_runtime->entryIndex;
            if (stage == Runtime::Stage::EnableRead) {
                m_runtime->stage = Runtime::Stage::EnableRead;
            }
            schedulePump();
        } else {
            recordFailure(entry.name, why);
            advanceMainEntry();
        }
        return;
    }

    const bool acknowledged = report.terminalResult
        == ParameterService::ExactTerminalResult::WriteSucceeded;
    const bool skipped = report.terminalResult
        == ParameterService::ExactTerminalResult::WriteSkipped;
    if (acknowledged || skipped) {
        if (acknowledged) {
            Receipt receipt;
            receipt.kind = m_runtime->currentReceiptKind;
            receipt.name = entry.name;
            receipt.value = report.value;
            receipt.type = report.type;
            receipt.description = report.description;
            m_runtime->report.receipts.append(receipt);
            appendHistory(QStringLiteral("Acknowledged %1 = %2.")
                          .arg(entry.name, report.value.toString()));
        } else {
            appendHistory(QStringLiteral(
                "%1 was already confirmed; no recovery write was sent.")
                .arg(entry.name));
        }
        if (stage == Runtime::Stage::EnableWrite) {
            ++m_runtime->entryIndex;
            m_runtime->stage = Runtime::Stage::EnableRead;
        } else if (stage == Runtime::Stage::IdentifierResetWrite) {
            m_runtime->stage = Runtime::Stage::MainWrite;
        } else if (stage == Runtime::Stage::MainWrite) {
            if (acknowledged) {
                ++m_runtime->report.setCount;
            } else {
                ++m_runtime->report.unchangedCount;
            }
            ++m_runtime->entryIndex;
            m_runtime->progressCompleted = m_runtime->entryIndex;
            m_runtime->stage = Runtime::Stage::MainRead;
        } else {
            finish(Outcome::Rejected,
                   QStringLiteral("A write completed in an invalid recovery stage."));
            return;
        }
        if (m_runtime->cancelRequested) {
            finish(Outcome::Cancelled,
                   QStringLiteral("Parameter recovery was cancelled; acknowledged receipts were preserved."));
            return;
        }
        if (m_runtime->deadlineExpired) {
            finish(Outcome::Rejected,
                   QStringLiteral("The bounded parameter recovery deadline expired; acknowledged receipts were preserved."));
            return;
        }
        const QPointer<ParameterRecoveryService> guard(this);
        emit stateChanged();
        if (!guard) return;
        schedulePump();
        return;
    }

    if (m_runtime->cancelRequested) {
        finish(Outcome::Cancelled,
               QStringLiteral("Parameter recovery was cancelled before another write was transmitted."));
        return;
    }
    if (m_runtime->deadlineExpired) {
        finish(Outcome::Rejected,
               QStringLiteral("The bounded parameter recovery deadline expired before another write was transmitted."));
        return;
    }

    const QString why = report.description.isEmpty()
        ? QStringLiteral("The exact parameter write was rejected.")
        : report.description;
    if (stage == Runtime::Stage::EnableWrite) {
        appendHistory(QStringLiteral("ENABLE pre-pass write %1 failed: %2")
                      .arg(entry.name, why));
        ++m_runtime->entryIndex;
        m_runtime->stage = Runtime::Stage::EnableRead;
        schedulePump();
    } else {
        recordFailure(entry.name, why);
        advanceMainEntry();
    }
}

void ParameterRecoveryService::scheduleDeferredReports()
{
    const QList<ParameterService::ExactOperationReport> reports =
        std::exchange(m_runtime->deferredReports, {});
    for (const auto &report : reports) {
        QTimer::singleShot(0, this, [this, report]() {
            handleParameterFinished(report);
        });
    }
}

void ParameterRecoveryService::recordFailure(
    const QString &name, const QString &description)
{
    if (!m_runtime->failedNames.contains(name)) {
        m_runtime->failedNames.insert(name);
        m_runtime->report.failedParameters.append(name);
        m_runtime->report.failedCount =
            m_runtime->report.failedParameters.size();
    }
    appendHistory(QStringLiteral("%1 failed: %2").arg(name, description));
}

void ParameterRecoveryService::advanceMainEntry()
{
    ++m_runtime->entryIndex;
    m_runtime->progressCompleted = m_runtime->entryIndex;
    m_runtime->stage = Runtime::Stage::MainRead;
    const QPointer<ParameterRecoveryService> guard(this);
    emit stateChanged();
    if (!guard) return;
    schedulePump();
}

bool ParameterRecoveryService::cancel(quint64 operationId)
{
    if (operationId == 0 || !m_runtime->busy
        || m_runtime->operationId != operationId
        || m_runtime->cancelRequested) {
        return false;
    }
    m_runtime->cancelRequested = true;
    m_runtime->status = QStringLiteral("Cancelling parameter recovery...");
    appendHistory(m_runtime->status);
    const QPointer<ParameterRecoveryService> guard(this);
    emit stateChanged();
    if (!guard) return true;
    if (m_runtime->parameterOperation.isValid()
        && m_runtime->parameterService) {
        if (m_runtime->parameterService->cancelExactOperation(
                m_runtime->parameterReservation,
                m_runtime->parameterOperation,
                QStringLiteral("Parameter recovery was cancelled by the operator."))) {
            return true;
        }
        // A terminal report may already be queued. It remains correlated by
        // operation token and will observe cancelRequested before advancing.
        return true;
    }
    schedulePump();
    return true;
}

void ParameterRecoveryService::handleDeadline()
{
    if (!m_runtime->busy || m_runtime->deadlineExpired) return;
    m_runtime->deadlineExpired = true;
    m_runtime->status = QStringLiteral(
        "The bounded parameter recovery deadline expired; stopping...");
    appendHistory(m_runtime->status);
    const QPointer<ParameterRecoveryService> guard(this);
    emit stateChanged();
    if (!guard) return;
    if (m_runtime->parameterOperation.isValid()
        && m_runtime->parameterService) {
        if (m_runtime->parameterService->cancelExactOperation(
                m_runtime->parameterReservation,
                m_runtime->parameterOperation,
                QStringLiteral("The bounded parameter recovery deadline expired."))) {
            return;
        }
        return; // Its immutable terminal report is already queued.
    }
    schedulePump();
}

void ParameterRecoveryService::releaseReservations()
{
    const QPointer<ParameterService> parameterService =
        m_runtime->parameterService;
    const QPointer<VehicleCommandService> commandService =
        m_runtime->commandService;
    const auto parameterReservation = m_runtime->parameterReservation;
    const auto commandReservation = m_runtime->commandReservation;
    m_runtime->parameterReservation = {};
    m_runtime->commandReservation = {};
    if (parameterService && parameterReservation.isValid()) {
        parameterService->releaseExactReservation(parameterReservation);
    }
    if (commandService && commandReservation.isValid()) {
        commandService->releaseExactReservation(commandReservation);
    }
}

void ParameterRecoveryService::finish(
    Outcome outcome, const QString &description)
{
    if (!m_runtime->busy || m_runtime->finishing) return;
    m_runtime->finishing = true;
    m_runtime->deadlineTimer.stop();
    m_runtime->report.outcome = outcome;
    m_runtime->report.description = description;
    m_runtime->report.failedCount =
        m_runtime->report.failedParameters.size();
    if (outcome == Outcome::Completed) {
        m_runtime->progressCompleted = m_runtime->progressTotal;
    }
    m_runtime->report.completedEntries =
        m_runtime->progressCompleted;
    m_runtime->report.remainingEntries = qMax(
        0, m_runtime->report.totalEntries
            - m_runtime->report.completedEntries);
    const Report report = m_runtime->report;

    m_runtime->busy = false;
    m_runtime->stage = Runtime::Stage::Idle;
    m_runtime->parameterOperation = {};
    m_runtime->submissionOperationOut = nullptr;
    m_runtime->deferredReports.clear();
    m_runtime->submissionInFlight = false;
    m_runtime->operationId = 0;
    m_runtime->activePlan = {};
    m_runtime->cancelRequested = false;
    m_runtime->deadlineExpired = false;
    m_runtime->lastReport = report;
    m_runtime->status = description;
    appendHistory(description);

    const QPointer<ParameterRecoveryService> guard(this);
    releaseReservations();
    if (!guard) return;
    m_runtime->finishing = false;
    emit stateChanged();
    if (!guard) return;
    emit operationFinished(report);
}

void ParameterRecoveryService::appendHistory(const QString &line)
{
    if (line.isEmpty()) return;
    m_runtime->history.append(line);
    while (m_runtime->history.size() > MaximumHistoryEntries) {
        m_runtime->history.removeFirst();
    }
}

void ParameterRecoveryService::shutdown()
{
    if (!m_runtime || m_runtime->shuttingDown) return;
    m_runtime->shuttingDown = true;
    m_runtime->deadlineTimer.stop();
    const QPointer<ParameterRecoveryService> guard(this);
    if (m_runtime->parameterService) {
        disconnect(m_runtime->parameterService, nullptr, this, nullptr);
        if (m_runtime->parameterOperation.isValid()) {
            m_runtime->parameterService->cancelExactOperation(
                m_runtime->parameterReservation,
                m_runtime->parameterOperation,
                QStringLiteral("Parameter recovery is shutting down."));
            if (!guard) return;
        }
    }
    if (m_runtime->targetManager) {
        disconnect(m_runtime->targetManager, nullptr, this, nullptr);
    }
    if (m_runtime->telemetryRegistry) {
        disconnect(m_runtime->telemetryRegistry, nullptr, this, nullptr);
    }
    m_runtime->parameterOperation = {};
    m_runtime->submissionOperationOut = nullptr;
    m_runtime->deferredReports.clear();
    m_runtime->submissionInFlight = false;
    m_runtime->busy = false;
    m_runtime->operationId = 0;
    m_runtime->activePlan = {};
    m_runtime->pendingPlan = {};
    releaseReservations();
}
