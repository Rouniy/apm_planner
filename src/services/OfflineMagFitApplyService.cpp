#include "OfflineMagFitApplyService.h"

#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterStore.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QSharedData>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
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

struct FrozenParameter
{
    QString name;
    QVariant initialValue;
    ParameterType type = ParameterType::Unknown;
    bool valueInvariant = false;
};

void assignError(QString *error, const QString &message)
{
    if (error) *error = message;
}

bool finiteVector(const MagVector &value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

bool supportedClassicType(ParameterType type)
{
    return type >= ParameterType::UInt8
        && type <= ParameterType::Real32;
}

bool rawValueFitsType(double value, ParameterType type)
{
    if (!std::isfinite(value) || !supportedClassicType(type)) return false;
    if (type == ParameterType::Real32) {
        return std::isfinite(static_cast<float>(value));
    }
    if (std::trunc(value) != value) return false;
    switch (type) {
    case ParameterType::UInt8: return value >= 0.0 && value <= 255.0;
    case ParameterType::Int8: return value >= -128.0 && value <= 127.0;
    case ParameterType::UInt16: return value >= 0.0 && value <= 65535.0;
    case ParameterType::Int16: return value >= -32768.0 && value <= 32767.0;
    case ParameterType::UInt32:
        return value >= 0.0 && value <= 4294967295.0;
    case ParameterType::Int32:
        return value >= -2147483648.0 && value <= 2147483647.0;
    default:
        return false;
    }
}

bool sameFrozenValue(const QVariant &left, const QVariant &right,
                     ParameterType type)
{
    bool leftOk = false;
    bool rightOk = false;
    switch (type) {
    case ParameterType::UInt8:
    case ParameterType::UInt16:
    case ParameterType::UInt32: {
        const qulonglong lhs = left.toULongLong(&leftOk);
        const qulonglong rhs = right.toULongLong(&rightOk);
        return leftOk && rightOk && lhs == rhs;
    }
    case ParameterType::Int8:
    case ParameterType::Int16:
    case ParameterType::Int32: {
        const qlonglong lhs = left.toLongLong(&leftOk);
        const qlonglong rhs = right.toLongLong(&rightOk);
        return leftOk && rightOk && lhs == rhs;
    }
    case ParameterType::Real32: {
        const float lhs = left.toFloat(&leftOk);
        const float rhs = right.toFloat(&rightOk);
        return leftOk && rightOk && std::isfinite(lhs)
            && std::isfinite(rhs) && lhs == rhs;
    }
    default:
        return false;
    }
}

bool uncertainWriteResult(ParameterService::ExactTerminalResult result)
{
    using Result = ParameterService::ExactTerminalResult;
    return result == Result::WriteCancelledOutcomeUncertain
        || result == Result::WriteTimedOutOutcomeUncertain
        || result == Result::WriteTransportOutcomeUncertain
        || result == Result::WriteLeaseRetiredOutcomeUncertain
        || result == Result::WriteLinkForgottenOutcomeUncertain;
}

QString suffixForCompass(int compass)
{
    return compass == 1 ? QString() : QString::number(compass);
}
}

class OfflineMagFitApplyService::Plan::Data final : public QSharedData
{
public:
    quint64 planId = 0;
    QString sourcePath;
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    QVector<OfflineMagFitResult> results;
    QVector<Write> writes;
    QVector<FrozenParameter> schema;
};

OfflineMagFitApplyService::Plan::Plan() = default;
OfflineMagFitApplyService::Plan::Plan(const Plan &other) = default;
OfflineMagFitApplyService::Plan &OfflineMagFitApplyService::Plan::operator=(
    const Plan &other) = default;
OfflineMagFitApplyService::Plan::~Plan() = default;

bool OfflineMagFitApplyService::Plan::isValid() const noexcept
{
    return d && d->planId != 0 && !d->sourcePath.isEmpty()
        && d->target.isValid() && d->vehicle.isValid()
        && !d->results.isEmpty() && !d->writes.isEmpty()
        && !d->schema.isEmpty();
}

QString OfflineMagFitApplyService::Plan::sourcePath() const
{
    return d ? d->sourcePath : QString();
}

VehicleTargetLease OfflineMagFitApplyService::Plan::target() const
{
    return d ? d->target : VehicleTargetLease();
}

SwarmVehicleInstanceLease OfflineMagFitApplyService::Plan::vehicle() const
{
    return d ? d->vehicle : SwarmVehicleInstanceLease();
}

QVector<OfflineMagFitResult> OfflineMagFitApplyService::Plan::results() const
{
    return d ? d->results : QVector<OfflineMagFitResult>();
}

QVector<OfflineMagFitApplyService::Write>
OfflineMagFitApplyService::Plan::writes() const
{
    return d ? d->writes : QVector<Write>();
}

struct OfflineMagFitApplyService::Runtime
{
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
    VehicleCommandService::ExactReservationToken commandReservation;
    ParameterService::ExactReservationToken parameterReservation;
    ParameterService::ExactOperationToken parameterOperation;
    ParameterService::ExactOperationToken *submissionOperationOut = nullptr;
    ParameterService::ExactOperationReport deferredReport;
    bool hasDeferredReport = false;
    int writeIndex = 0;
    int progressCompleted = 0;
    int progressTotal = 0;
    bool busy = false;
    bool apiInFlight = false;
    bool finishing = false;
    bool shuttingDown = false;
    bool nextScheduled = false;
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
};

OfflineMagFitApplyService::OfflineMagFitApplyService(
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
    qRegisterMetaType<Write>();
    qRegisterMetaType<Report>();
    connect(&m_runtime->deadlineTimer, &QTimer::timeout,
            this, &OfflineMagFitApplyService::handleDeadline);
    if (m_runtime->parameterService) {
        connect(m_runtime->parameterService,
                &ParameterService::exactOperationFinished,
                this, &OfflineMagFitApplyService::handleParameterFinished,
                Qt::QueuedConnection);
    }
    if (m_runtime->targetManager) {
        connect(m_runtime->targetManager,
                &VehicleTargetManager::currentTargetChanged,
                this, &OfflineMagFitApplyService::stateChanged);
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

OfflineMagFitApplyService::~OfflineMagFitApplyService()
{
    shutdown();
}

bool OfflineMagFitApplyService::busy() const noexcept
{
    return m_runtime->busy;
}

QString OfflineMagFitApplyService::status() const
{
    return m_runtime->status;
}

QStringList OfflineMagFitApplyService::history() const
{
    return m_runtime->history;
}

OfflineMagFitApplyService::Report OfflineMagFitApplyService::lastReport() const
{
    return m_runtime->lastReport;
}

quint64 OfflineMagFitApplyService::currentOperationId() const noexcept
{
    return m_runtime->operationId;
}

int OfflineMagFitApplyService::progressCompleted() const noexcept
{
    return m_runtime->progressCompleted;
}

int OfflineMagFitApplyService::progressTotal() const noexcept
{
    return m_runtime->progressTotal;
}

void OfflineMagFitApplyService::setOverallDeadlineForTesting(int timeoutMs)
{
    if (!m_runtime->busy) {
        m_runtime->overallDeadlineMs = qBound(
            1, timeoutMs, DefaultOverallDeadlineMs);
    }
}

bool OfflineMagFitApplyService::validateVehicle(
    const VehicleTargetLease &target,
    const SwarmVehicleInstanceLease &vehicle,
    QString *error) const
{
    const QPointer<const OfflineMagFitApplyService> guard(this);
    if (QThread::currentThread() != thread()
        || m_runtime->shuttingDown || !m_runtime->targetManager
        || !m_runtime->telemetryRegistry || !m_runtime->parameterService
        || !m_runtime->commandService || !m_runtime->routeValidator) {
        assignError(error, QStringLiteral(
            "Offline MagFit apply services are unavailable."));
        return false;
    }
    if (!target.isValid() || !vehicle.isValid()
        || !target.endpoint.sameIdentity(vehicle.endpoint)
        || target.endpoint.componentId != MAV_COMP_ID_AUTOPILOT1) {
        assignError(error, QStringLiteral(
            "The captured autopilot target is invalid."));
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
        if (!m_runtime->telemetryRegistry->snapshotForLease(vehicle, &snapshot)
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
        for (const VehicleEndpoint &candidate
             : m_runtime->telemetryRegistry->endpoints()) {
            if (candidate.linkId != vehicle.endpoint.linkId) continue;
            ++vehiclesOnLink;
            capturedPresent = capturedPresent
                || candidate.sameIdentity(vehicle.endpoint);
        }
        if (vehiclesOnLink != 1 || !capturedPresent) {
            assignError(why, QStringLiteral(
                "Offline MagFit apply requires one exact vehicle on the physical link."));
            return false;
        }
        return true;
    };

    if (!stateIsSafe(error)) return false;
    const RouteValidator validator = m_runtime->routeValidator;
    QString routeError;
    const bool routeAllowed = validator(vehicle, &routeError);
    if (!guard) return false;
    if (!routeAllowed) {
        assignError(error, routeError.isEmpty()
            ? QStringLiteral("No safe exact MagFit apply route is available.")
            : routeError);
        return false;
    }
    return stateIsSafe(error);
}

bool OfflineMagFitApplyService::captureVehicle(
    VehicleTargetLease *target,
    SwarmVehicleInstanceLease *vehicle,
    QString *error) const
{
    if (target) *target = {};
    if (vehicle) *vehicle = {};
    if (m_runtime->shuttingDown || !m_runtime->targetManager
        || !m_runtime->telemetryRegistry) {
        assignError(error, QStringLiteral(
            "Offline MagFit apply services are unavailable."));
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
    if (!capturedVehicle.isValid()) {
        assignError(error, QStringLiteral(
            "The selected vehicle has no fresh exact telemetry."));
        return false;
    }
    if (!validateVehicle(capturedTarget, capturedVehicle, error)) return false;
    if (target) *target = capturedTarget;
    if (vehicle) *vehicle = capturedVehicle;
    return true;
}

bool OfflineMagFitApplyService::canPrepare(QString *error) const
{
    const QPointer<const OfflineMagFitApplyService> guard(this);
    if (error) error->clear();
    if (m_runtime->busy || m_runtime->apiInFlight || m_runtime->finishing) {
        assignError(error, QStringLiteral(
            "Another offline MagFit apply operation is active."));
        return false;
    }
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    if (!captureVehicle(&target, &vehicle, error)) return false;
    if (!guard || !m_runtime->parameterService
        || !m_runtime->parameterService->store()) return false;
    const ParameterSnapshot snapshot =
        m_runtime->parameterService->store()->snapshot(vehicle.endpoint);
    if (!snapshot.isComplete()
        || !snapshot.endpoint().sameIdentity(vehicle.endpoint)) {
        assignError(error, QStringLiteral(
            "Load a complete parameter list for the exact selected autopilot first."));
        return false;
    }
    return true;
}

bool OfflineMagFitApplyService::prepare(
    const OfflineMagFitReport &analysis, Plan *planOut, QString *error)
{
    const OfflineMagFitReport submittedAnalysis = analysis;
    const QPointer<OfflineMagFitApplyService> guard(this);
    if (planOut) *planOut = {};
    if (error) error->clear();
    if (!planOut) {
        assignError(error, QStringLiteral("A plan output is required."));
        return false;
    }
    if (m_runtime->busy || m_runtime->apiInFlight || m_runtime->finishing) {
        assignError(error, QStringLiteral(
            "Another offline MagFit apply operation is active."));
        return false;
    }
    // A new preparation attempt supersedes any earlier unconfirmed plan,
    // including when the new analysis or schema is rejected.
    m_runtime->pendingPlan = {};
    if (!submittedAnalysis.success || submittedAnalysis.cancelled
        || !submittedAnalysis.error.isEmpty()
        || submittedAnalysis.sourcePath.trimmed().isEmpty()
        || submittedAnalysis.isTelemetryLog || !submittedAnalysis.applyEligible
        || submittedAnalysis.results.isEmpty()
        || submittedAnalysis.results.size() > 3
        || submittedAnalysis.throttleThreshold < 0
        || submittedAnalysis.throttleThreshold > 100) {
        assignError(error, submittedAnalysis.applyUnavailableReason.isEmpty()
            ? QStringLiteral(
                "A successful, apply-eligible DataFlash MagFit analysis is required.")
            : submittedAnalysis.applyUnavailableReason);
        return false;
    }

    QVector<OfflineMagFitResult> results = submittedAnalysis.results;
    std::sort(results.begin(), results.end(),
              [](const OfflineMagFitResult &left,
                 const OfflineMagFitResult &right) {
        return left.compass < right.compass;
    });
    QSet<int> compasses;
    for (const OfflineMagFitResult &result : results) {
        const bool meaningful = result.compass >= 1 && result.compass <= 3
            && !compasses.contains(result.compass)
            && result.sourceSamples > 0 && result.usedSamples > 0
            && result.usedSamples <= result.sourceSamples
            && result.coverageOctants > 0 && result.coverageOctants <= 8
            && finiteVector(result.loggedOffsets)
            && finiteVector(result.sphereOffsets)
            && finiteVector(result.offsets)
            && finiteVector(result.diagonals)
            && finiteVector(result.offDiagonals)
            && std::isfinite(result.sphereRadius)
            && result.sphereRadius > 0.0
            && std::isfinite(result.sphereRmsError)
            && result.sphereRmsError >= 0.0
            && std::isfinite(result.rmsError)
            && result.rmsError >= 0.0;
        if (!meaningful) {
            assignError(error, QStringLiteral(
                "Offline MagFit returned invalid or incomplete calibration metadata."));
            return false;
        }
        compasses.insert(result.compass);
    }

    m_runtime->apiInFlight = true;
    ScopeExit apiGuard([guard]() {
        if (guard) {
            guard->m_runtime->apiInFlight = false;
            emit guard->stateChanged();
        }
    });
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    if (!captureVehicle(&target, &vehicle, error) || !guard) return false;
    const ParameterSnapshot snapshot =
        m_runtime->parameterService->store()->snapshot(vehicle.endpoint);
    if (!snapshot.isComplete()
        || !snapshot.endpoint().sameIdentity(vehicle.endpoint)) {
        assignError(error, QStringLiteral(
            "Load a complete parameter list for the exact selected autopilot first."));
        return false;
    }

    QVector<Write> writes;
    QVector<FrozenParameter> schema;
    QSet<QString> frozenNames;
    const quint8 componentId = static_cast<quint8>(
        vehicle.endpoint.componentId);
    const auto appendFrozen = [&](const QString &name,
                                  bool valueInvariant,
                                  ParameterRecord *recordOut = nullptr,
                                  bool required = true) -> bool {
        if (!snapshot.contains(componentId, name)) {
            if (!required) return true;
            assignError(error, QStringLiteral(
                "The exact vehicle does not expose required parameter %1; nothing was written.")
                .arg(name));
            return false;
        }
        const ParameterRecord record = snapshot.value(componentId, name);
        if (!record.value.isValid() || !supportedClassicType(record.type)) {
            assignError(error, QStringLiteral(
                "Parameter %1 has no supported exact live type.").arg(name));
            return false;
        }
        if (!frozenNames.contains(name)) {
            frozenNames.insert(name);
            schema.append(FrozenParameter{
                name, record.value, record.type, valueInvariant});
        }
        if (recordOut) *recordOut = record;
        return true;
    };
    const auto appendWrite = [&](const QString &name, double rawValue,
                                 bool required) -> bool {
        ParameterRecord record;
        if (!appendFrozen(name, false, &record, required)) return false;
        if (!snapshot.contains(componentId, name)) return true;
        QVariant normalized;
        QString normalizationError;
        if (!record.value.isValid() || !rawValueFitsType(rawValue, record.type)
            || !m_runtime->parameterService->normalizeExactValue(
                vehicle, rawValue, record.type,
                &normalized, &normalizationError)) {
            assignError(error, normalizationError.isEmpty()
                ? QStringLiteral(
                    "Calibration value for %1 is incompatible with the exact live parameter type.")
                    .arg(name)
                : normalizationError);
            return false;
        }
        writes.append(Write{name, normalized, record.type});
        return true;
    };

    const auto requireLoggedFrameParameter = [
        &submittedAnalysis, &appendFrozen, error](
            const QString &name, int minimum, int maximum) -> bool {
        if (!submittedAnalysis.loggedFrameParameters.contains(name)) {
            assignError(error, QStringLiteral(
                "MagFit lacks a stable logged value for frame parameter %1; nothing was written.")
                .arg(name));
            return false;
        }
        const double loggedValue =
            submittedAnalysis.loggedFrameParameters.value(name);
        if (!std::isfinite(loggedValue)
            || std::trunc(loggedValue) != loggedValue
            || loggedValue < minimum || loggedValue > maximum) {
            assignError(error, QStringLiteral(
                "Logged frame parameter %1 is invalid or uses an unsupported custom rotation.")
                .arg(name));
            return false;
        }
        ParameterRecord liveRecord;
        if (!appendFrozen(name, true, &liveRecord)) return false;
        bool liveOk = false;
        const qlonglong liveValue = liveRecord.value.toLongLong(&liveOk);
        if (!liveOk || !ParameterCodec::isInteger(liveRecord.type)
            || liveValue != static_cast<qlonglong>(loggedValue)) {
            assignError(error, QStringLiteral(
                "Live frame parameter %1 does not match the analyzed log; nothing was written.")
                .arg(name));
            return false;
        }
        return true;
    };

    if (!requireLoggedFrameParameter(
            QStringLiteral("AHRS_ORIENTATION"), 0, 43)) {
        return false;
    }
    for (const OfflineMagFitResult &result : results) {
        const QString suffix = suffixForCompass(result.compass);
        const QString orientationName =
            QStringLiteral("COMPASS_ORIENT%1").arg(suffix);
        const QString externalName = result.compass == 1
            ? QStringLiteral("COMPASS_EXTERNAL")
            : QStringLiteral("COMPASS_EXTERN%1").arg(result.compass);
        if (!requireLoggedFrameParameter(orientationName, 0, 43)
            || !requireLoggedFrameParameter(externalName, 0, 2)) {
            return false;
        }
    }

    double maximumOffset = 1800.0;
    double maximumRms = 16.0;
    const auto readOptionalPositiveLimit = [&](const QString &name,
                                                double *limit) -> bool {
        if (!snapshot.contains(componentId, name)) return true;
        ParameterRecord record;
        if (!appendFrozen(name, true, &record, false)) return false;
        bool ok = false;
        const double value = record.value.toDouble(&ok);
        if (!ok || !std::isfinite(value) || value <= 0.0) {
            assignError(error, QStringLiteral(
                "Safety limit parameter %1 is invalid; nothing was written.")
                .arg(name));
            return false;
        }
        *limit = value;
        return true;
    };
    if (!readOptionalPositiveLimit(QStringLiteral("COMPASS_OFFS_MAX"),
                                   &maximumOffset)
        || !readOptionalPositiveLimit(QStringLiteral("COMPASS_CAL_FIT"),
                                      &maximumRms)) {
        return false;
    }

    for (const OfflineMagFitResult &result : results) {
        if (!submittedAnalysis.loggedDeviceIds.contains(result.compass)
            || submittedAnalysis.loggedDeviceIds.value(result.compass) == 0) {
            assignError(error, QStringLiteral(
                "MagFit lacks a proven non-zero device identity for compass %1.")
                .arg(result.compass));
            return false;
        }
        const quint32 loggedDeviceId =
            submittedAnalysis.loggedDeviceIds.value(result.compass);
        const QString suffix = suffixForCompass(result.compass);
        const QString deviceName = QStringLiteral("COMPASS_DEV_ID%1")
            .arg(suffix);
        ParameterRecord deviceRecord;
        if (!appendFrozen(deviceName, true, &deviceRecord)) return false;
        bool deviceOk = false;
        const qulonglong liveDeviceId =
            deviceRecord.value.toULongLong(&deviceOk);
        if (!deviceOk || !ParameterCodec::isInteger(deviceRecord.type)
            || liveDeviceId != loggedDeviceId) {
            assignError(error, QStringLiteral(
                "Compass %1 device identity does not match the analyzed log; nothing was written.")
                .arg(result.compass));
            return false;
        }
        const QString priorityName = QStringLiteral("COMPASS_PRIO%1_ID")
            .arg(result.compass);
        if (snapshot.contains(componentId, priorityName)) {
            ParameterRecord priorityRecord;
            if (!appendFrozen(priorityName, true, &priorityRecord)) {
                return false;
            }
            bool priorityOk = false;
            const qulonglong priorityId =
                priorityRecord.value.toULongLong(&priorityOk);
            if (!priorityOk
                || !ParameterCodec::isInteger(priorityRecord.type)
                || (priorityId != 0 && priorityId != loggedDeviceId)) {
                assignError(error, QStringLiteral(
                    "Compass priority slot %1 does not match the analyzed device; automatic remapping is refused.")
                    .arg(result.compass));
                return false;
            }
        }
        if (!(result.sphereRadius > 150.0
              && result.sphereRadius < 950.0)
            || result.rmsError > maximumRms
            || std::abs(result.offsets.x) >= maximumOffset
            || std::abs(result.offsets.y) >= maximumOffset
            || std::abs(result.offsets.z) >= maximumOffset) {
            assignError(error, QStringLiteral(
                "Compass %1 fit exceeds the vehicle calibration safety limits; nothing was written.")
                .arg(result.compass));
            return false;
        }
        if (result.hasEllipsoid
            && (!(result.diagonals.x > 0.2 && result.diagonals.x < 5.0)
                || !(result.diagonals.y > 0.2 && result.diagonals.y < 5.0)
                || !(result.diagonals.z > 0.2 && result.diagonals.z < 5.0)
                || std::abs(result.offDiagonals.x) >= 1.0
                || std::abs(result.offDiagonals.y) >= 1.0
                || std::abs(result.offDiagonals.z) >= 1.0)) {
            assignError(error, QStringLiteral(
                "Compass %1 ellipsoid fit exceeds safe matrix bounds; nothing was written.")
                .arg(result.compass));
            return false;
        }
    }

    if (!appendWrite(QStringLiteral("COMPASS_LEARN"), 0.0, false)) {
        return false;
    }
    for (const OfflineMagFitResult &result : results) {
        const QString suffix = suffixForCompass(result.compass);
        if (!appendWrite(QStringLiteral("COMPASS_OFS%1_X").arg(suffix),
                         result.offsets.x, true)
            || !appendWrite(QStringLiteral("COMPASS_OFS%1_Y").arg(suffix),
                            result.offsets.y, true)
            || !appendWrite(QStringLiteral("COMPASS_OFS%1_Z").arg(suffix),
                            result.offsets.z, true)) {
            return false;
        }
        const QString diaX = QStringLiteral("COMPASS_DIA%1_X").arg(suffix);
        if (result.hasEllipsoid) {
            if (!snapshot.contains(componentId, diaX)) {
                assignError(error, QStringLiteral(
                    "The exact vehicle does not support ellipsoid parameters for compass %1; reanalyze with sphere-only fitting.")
                    .arg(result.compass));
                return false;
            }
            if (!appendWrite(diaX, result.diagonals.x, true)
                || !appendWrite(
                    QStringLiteral("COMPASS_DIA%1_Y").arg(suffix),
                    result.diagonals.y, true)
                || !appendWrite(
                    QStringLiteral("COMPASS_DIA%1_Z").arg(suffix),
                    result.diagonals.z, true)
                || !appendWrite(
                    QStringLiteral("COMPASS_ODI%1_X").arg(suffix),
                    result.offDiagonals.x, true)
                || !appendWrite(
                    QStringLiteral("COMPASS_ODI%1_Y").arg(suffix),
                    result.offDiagonals.y, true)
                || !appendWrite(
                    QStringLiteral("COMPASS_ODI%1_Z").arg(suffix),
                    result.offDiagonals.z, true)) {
                return false;
            }
        }
    }
    for (const Write &write : writes) {
        bool valueOk = false;
        const double wireValue = write.value.toDouble(&valueOk);
        const bool safe = valueOk && std::isfinite(wireValue)
            && (!write.name.startsWith(QStringLiteral("COMPASS_OFS"))
                || std::abs(wireValue) < maximumOffset)
            && (!write.name.startsWith(QStringLiteral("COMPASS_DIA"))
                || (wireValue > 0.2 && wireValue < 5.0))
            && (!write.name.startsWith(QStringLiteral("COMPASS_ODI"))
                || std::abs(wireValue) < 1.0);
        if (!safe) {
            assignError(error, QStringLiteral(
                "The exact wire value for %1 crosses a calibration safety bound; nothing was written.")
                .arg(write.name));
            return false;
        }
    }
    if (writes.isEmpty() || m_runtime->nextPlanId == 0) {
        assignError(error, QStringLiteral(
            "No compatible MagFit calibration writes could be prepared."));
        return false;
    }

    Plan captured;
    captured.d = new Plan::Data;
    captured.d->planId = m_runtime->nextPlanId++;
    captured.d->sourcePath = submittedAnalysis.sourcePath;
    captured.d->target = target;
    captured.d->vehicle = vehicle;
    captured.d->results = results;
    captured.d->writes = writes;
    captured.d->schema = schema;
    m_runtime->pendingPlan = captured;
    *planOut = captured;
    m_runtime->status = QStringLiteral(
        "Prepared %1 exact MagFit parameter writes for %2. Confirm that the log came from this vehicle.")
        .arg(writes.size()).arg(vehicle.endpoint.displayName());
    appendHistory(m_runtime->status);
    return true;
}

bool OfflineMagFitApplyService::planMatchesPending(const Plan &plan) const
{
    return plan.isValid() && m_runtime->pendingPlan.isValid()
        && plan.d.constData() == m_runtime->pendingPlan.d.constData();
}

bool OfflineMagFitApplyService::validatePlanSchema(
    const Plan &plan, bool requireInitialValues, QString *error) const
{
    if (!plan.isValid() || !m_runtime->parameterService
        || !m_runtime->parameterService->store()) {
        assignError(error, QStringLiteral("The MagFit apply plan is invalid."));
        return false;
    }
    const auto *data = plan.d.constData();
    const ParameterSnapshot snapshot =
        m_runtime->parameterService->store()->snapshot(data->vehicle.endpoint);
    if (!snapshot.isComplete()
        || !snapshot.endpoint().sameIdentity(data->vehicle.endpoint)) {
        assignError(error, QStringLiteral(
            "The complete exact parameter snapshot is no longer available."));
        return false;
    }
    const quint8 componentId = static_cast<quint8>(
        data->vehicle.endpoint.componentId);
    for (const FrozenParameter &frozen : data->schema) {
        if (!snapshot.contains(componentId, frozen.name)) {
            assignError(error, QStringLiteral(
                "Required parameter %1 disappeared from the exact snapshot.")
                .arg(frozen.name));
            return false;
        }
        const ParameterRecord current = snapshot.value(
            componentId, frozen.name);
        if (current.type != frozen.type) {
            assignError(error, QStringLiteral(
                "Parameter %1 changed type after confirmation.")
                .arg(frozen.name));
            return false;
        }
        const Write *confirmedWrite = nullptr;
        if (!requireInitialValues && !frozen.valueInvariant
            && m_runtime->busy
            && m_runtime->activePlan.d.constData() == data) {
            for (int index = 0;
                 index < qMin(m_runtime->writeIndex, data->writes.size());
                 ++index) {
                if (data->writes.at(index).name == frozen.name) {
                    confirmedWrite = &data->writes.at(index);
                    break;
                }
            }
        }
        const QVariant expected = confirmedWrite
            ? confirmedWrite->value : frozen.initialValue;
        if (!sameFrozenValue(current.value, expected, frozen.type)) {
            assignError(error, QStringLiteral(
                "Parameter %1 changed outside the confirmed MagFit apply sequence.")
                .arg(frozen.name));
            return false;
        }
    }
    for (const Write &write : data->writes) {
        QVariant normalized;
        QString normalizationError;
        if (!m_runtime->parameterService->normalizeExactValue(
                data->vehicle, write.value, write.type,
                &normalized, &normalizationError)
            || !sameFrozenValue(normalized, write.value, write.type)) {
            assignError(error, normalizationError.isEmpty()
                ? QStringLiteral(
                    "Parameter %1 is no longer representable with the exact link encoding.")
                    .arg(write.name)
                : normalizationError);
            return false;
        }
    }
    return true;
}

bool OfflineMagFitApplyService::validate(
    const Plan &plan, QString *error) const
{
    const Plan submittedPlan = plan;
    if (error) error->clear();
    if (m_runtime->busy || m_runtime->apiInFlight || m_runtime->finishing
        || !planMatchesPending(submittedPlan)) {
        assignError(error, QStringLiteral(
            "The captured MagFit apply plan is stale or another operation is active."));
        return false;
    }
    const QPointer<const OfflineMagFitApplyService> guard(this);
    const bool vehicleValid = validateVehicle(
        submittedPlan.d->target, submittedPlan.d->vehicle, error);
    if (!guard || !vehicleValid) return false;
    if (!planMatchesPending(submittedPlan)) return false;
    return validatePlanSchema(submittedPlan, true, error);
}

OfflineMagFitApplyService::SubmitResult OfflineMagFitApplyService::execute(
    const Plan &plan, quint64 *operationIdOut, QString *error)
{
    const Plan submittedPlan = plan;
    const QPointer<OfflineMagFitApplyService> guard(this);
    if (operationIdOut) *operationIdOut = 0;
    if (error) error->clear();
    if (m_runtime->busy || m_runtime->apiInFlight || m_runtime->finishing) {
        assignError(error, QStringLiteral(
            "Another offline MagFit apply operation is active."));
        return SubmitResult::Busy;
    }
    if (!planMatchesPending(submittedPlan)) {
        assignError(error, QStringLiteral("The MagFit apply plan is stale."));
        return SubmitResult::InvalidPlan;
    }
    if (m_runtime->nextOperationId == 0) {
        assignError(error, QStringLiteral(
            "MagFit apply operation identifiers are exhausted."));
        return SubmitResult::Unavailable;
    }
    m_runtime->apiInFlight = true;
    ScopeExit apiGuard([guard]() {
        if (guard) {
            guard->m_runtime->apiInFlight = false;
            emit guard->stateChanged();
        }
    });

    m_runtime->activePlan = submittedPlan;
    m_runtime->pendingPlan = {};
    m_runtime->operationId = m_runtime->nextOperationId++;
    const quint64 admittedId = m_runtime->operationId;
    if (operationIdOut) *operationIdOut = admittedId;
    m_runtime->busy = true;
    m_runtime->cancelRequested = false;
    m_runtime->deadlineExpired = false;
    m_runtime->writeIndex = 0;
    m_runtime->progressCompleted = 0;
    m_runtime->progressTotal = submittedPlan.d->writes.size();
    m_runtime->report = {};
    m_runtime->report.operationId = admittedId;
    m_runtime->report.sourcePath = submittedPlan.d->sourcePath;
    m_runtime->report.endpoint = submittedPlan.d->vehicle.endpoint;
    m_runtime->report.totalWrites = submittedPlan.d->writes.size();
    m_runtime->status = QStringLiteral("Reserving the exact MagFit apply route...");
    appendHistory(m_runtime->status);
    m_runtime->overallClock.restart();
    m_runtime->deadlineTimer.start(m_runtime->overallDeadlineMs);

    QString why;
    const bool targetValid = validateVehicle(
        submittedPlan.d->target, submittedPlan.d->vehicle, &why);
    if (!guard) return SubmitResult::Unavailable;
    if (!m_runtime->busy || m_runtime->operationId != admittedId) {
        return SubmitResult::Started;
    }
    if (m_runtime->cancelRequested || deadlineReached()) {
        m_runtime->deadlineExpired = deadlineReached();
        finish(m_runtime->deadlineExpired ? Outcome::Rejected
                                          : Outcome::Cancelled,
               m_runtime->deadlineExpired
                   ? QStringLiteral("The bounded MagFit apply deadline expired during validation.")
                   : QStringLiteral("MagFit apply was cancelled during validation."));
        return SubmitResult::Started;
    }
    if (!targetValid || !validatePlanSchema(submittedPlan, true, &why)) {
        assignError(error, why);
        finish(Outcome::Rejected, why.isEmpty()
            ? QStringLiteral("The exact MagFit apply plan failed final validation.")
            : why);
        return SubmitResult::Unavailable;
    }

    VehicleCommandService::ExactReservationToken commandReservation;
    const auto commandResult =
        m_runtime->commandService->reserveSingleVehicleEndpoint(
            this, submittedPlan.d->target, submittedPlan.d->vehicle,
            &commandReservation, &why);
    if (!guard) return SubmitResult::Unavailable;
    if (m_runtime->shuttingDown || !m_runtime->busy
        || m_runtime->operationId != admittedId) {
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
                   ? QStringLiteral("The bounded MagFit apply deadline expired during reservation.")
                   : QStringLiteral("MagFit apply was cancelled during reservation."));
        return SubmitResult::Started;
    }
    if (commandResult
        != VehicleCommandService::ExactReservationResult::Reserved) {
        assignError(error, why);
        finish(Outcome::Rejected, why.isEmpty()
            ? QStringLiteral("The exact command safety lane could not be reserved.")
            : why);
        return SubmitResult::Unavailable;
    }
    m_runtime->commandReservation = commandReservation;

    ParameterService::ExactReservationToken parameterReservation;
    const auto parameterResult =
        m_runtime->parameterService->reserveSingleVehicleEndpoint(
            this, submittedPlan.d->target, submittedPlan.d->vehicle,
            &parameterReservation, &why);
    if (!guard) return SubmitResult::Unavailable;
    if (m_runtime->shuttingDown || !m_runtime->busy
        || m_runtime->operationId != admittedId) {
        if (parameterReservation.isValid() && m_runtime->parameterService) {
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
                   ? QStringLiteral("The bounded MagFit apply deadline expired during reservation.")
                   : QStringLiteral("MagFit apply was cancelled during reservation."));
        return SubmitResult::Started;
    }
    if (parameterResult
        != ParameterService::ExactReservationResult::Reserved) {
        assignError(error, why);
        finish(Outcome::Rejected, why.isEmpty()
            ? QStringLiteral("The exact parameter lane could not be reserved.")
            : why);
        return SubmitResult::Unavailable;
    }
    m_runtime->parameterReservation = parameterReservation;

    const bool finalTargetValid = validateVehicle(
        submittedPlan.d->target, submittedPlan.d->vehicle, &why);
    if (!guard) return SubmitResult::Unavailable;
    if (!finalTargetValid || !m_runtime->busy
        || m_runtime->operationId != admittedId
        || m_runtime->cancelRequested || deadlineReached()
        || m_runtime->activePlan.d.constData()
            != submittedPlan.d.constData()
        || !validatePlanSchema(submittedPlan, true, &why)) {
        const bool cancelled = m_runtime->cancelRequested;
        const bool expired = deadlineReached();
        m_runtime->deadlineExpired = expired;
        assignError(error, why);
        finish(cancelled ? Outcome::Cancelled : Outcome::Rejected,
               expired
                   ? QStringLiteral("The bounded MagFit apply deadline expired during reservation.")
                   : cancelled
                       ? QStringLiteral("MagFit apply was cancelled during reservation.")
                       : why.isEmpty()
                           ? QStringLiteral("The exact MagFit apply plan changed during reservation.")
                           : why);
        return SubmitResult::Unavailable;
    }

    m_runtime->status = QStringLiteral("Writing MagFit calibration parameters...");
    emit stateChanged();
    if (!guard) return SubmitResult::Unavailable;
    scheduleNext();
    return SubmitResult::Started;
}

bool OfflineMagFitApplyService::deadlineReached() const
{
    return m_runtime->busy && m_runtime->overallClock.isValid()
        && m_runtime->overallClock.elapsed()
            >= m_runtime->overallDeadlineMs;
}

void OfflineMagFitApplyService::scheduleNext()
{
    if (!m_runtime->busy || m_runtime->nextScheduled
        || m_runtime->parameterOperation.isValid()) return;
    const quint64 scheduledOperationId = m_runtime->operationId;
    m_runtime->nextScheduled = true;
    QTimer::singleShot(0, this, [this, scheduledOperationId]() {
        if (!m_runtime->busy
            || m_runtime->operationId != scheduledOperationId) {
            return;
        }
        m_runtime->nextScheduled = false;
        submitCurrentWrite();
    });
}

void OfflineMagFitApplyService::submitCurrentWrite()
{
    if (!m_runtime->busy || m_runtime->submissionInFlight
        || m_runtime->parameterOperation.isValid()) return;
    const Plan submittedPlan = m_runtime->activePlan;
    const quint64 submittedId = m_runtime->operationId;
    if (m_runtime->cancelRequested) {
        finish(Outcome::Cancelled,
               QStringLiteral("MagFit apply was cancelled before the next write."));
        return;
    }
    if (deadlineReached()) m_runtime->deadlineExpired = true;
    if (m_runtime->deadlineExpired) {
        finish(Outcome::Rejected,
               QStringLiteral("The bounded MagFit apply deadline expired."));
        return;
    }
    if (!submittedPlan.isValid()
        || m_runtime->writeIndex >= submittedPlan.d->writes.size()) {
        finish(Outcome::Completed,
               QStringLiteral(
                   "Applied %1 MagFit calibration values. Reboot the autopilot and verify heading before flight.")
                   .arg(m_runtime->report.confirmedWrites));
        return;
    }

    QString why;
    const QPointer<OfflineMagFitApplyService> guard(this);
    const bool targetValid = validateVehicle(
        submittedPlan.d->target, submittedPlan.d->vehicle, &why);
    if (!guard) return;
    if (!m_runtime->busy || m_runtime->operationId != submittedId
        || m_runtime->activePlan.d.constData()
            != submittedPlan.d.constData()) return;
    if (!targetValid || !validatePlanSchema(submittedPlan, false, &why)) {
        finish(Outcome::Rejected, why.isEmpty()
            ? QStringLiteral("The exact MagFit apply context is no longer safe.")
            : why);
        return;
    }
    if (m_runtime->cancelRequested || deadlineReached()) {
        m_runtime->deadlineExpired = deadlineReached();
        finish(m_runtime->deadlineExpired ? Outcome::Rejected
                                          : Outcome::Cancelled,
               m_runtime->deadlineExpired
                   ? QStringLiteral("The bounded MagFit apply deadline expired.")
                   : QStringLiteral("MagFit apply was cancelled before transmission."));
        return;
    }

    const Write write = submittedPlan.d->writes.at(m_runtime->writeIndex);
    const QPointer<ParameterService> parameterService =
        m_runtime->parameterService;
    const auto reservation = m_runtime->parameterReservation;
    ParameterService::ExactWriteRequest request;
    request.name = write.name;
    request.value = write.value;
    request.type = write.type;
    request.force = true;
    request.validateBeforeWrite =
        [guard, parameterService, submittedPlan, submittedId,
         write](QString *error) {
            if (!guard || !guard->m_runtime->busy
                || guard->m_runtime->shuttingDown
                || guard->m_runtime->operationId != submittedId
                || guard->m_runtime->cancelRequested
                || guard->m_runtime->deadlineExpired
                || guard->deadlineReached()
                || guard->m_runtime->activePlan.d.constData()
                    != submittedPlan.d.constData()) {
                assignError(error, QStringLiteral(
                    "The MagFit apply plan was retired before transmission."));
                return false;
            }
            const bool targetValid = guard->validateVehicle(
                submittedPlan.d->target, submittedPlan.d->vehicle, error);
            if (!guard || !targetValid) return false;
            if (!guard->m_runtime->busy
                || guard->m_runtime->shuttingDown
                || guard->m_runtime->operationId != submittedId
                || guard->m_runtime->cancelRequested
                || guard->m_runtime->deadlineExpired
                || guard->deadlineReached()
                || guard->m_runtime->activePlan.d.constData()
                    != submittedPlan.d.constData()) {
                assignError(error, QStringLiteral(
                    "The MagFit apply plan changed during final safety validation."));
                return false;
            }
            if (!guard->validatePlanSchema(submittedPlan, false, error)) {
                return false;
            }
            QVariant normalized;
            QString normalizationError;
            if (!parameterService
                || !parameterService->normalizeExactValue(
                    submittedPlan.d->vehicle, write.value, write.type,
                    &normalized, &normalizationError)
                || !sameFrozenValue(normalized, write.value, write.type)) {
                assignError(error, normalizationError.isEmpty()
                    ? QStringLiteral(
                        "The MagFit value is no longer representable with the exact link encoding.")
                    : normalizationError);
                return false;
            }
            return true;
        };

    ParameterService::ExactOperationToken operation;
    QString submissionError;
    m_runtime->submissionInFlight = true;
    m_runtime->submissionOperationOut = &operation;
    const auto result = parameterService->submitExactWrite(
        reservation, submittedPlan.d->vehicle,
        request, &operation, &submissionError);
    if (!guard) return;
    m_runtime->submissionOperationOut = nullptr;
    m_runtime->submissionInFlight = false;
    const bool stillOwns = !m_runtime->shuttingDown && m_runtime->busy
        && m_runtime->operationId == submittedId
        && m_runtime->activePlan.d.constData()
            == submittedPlan.d.constData();
    if (!stillOwns) {
        if (result == ParameterService::ExactSubmitResult::Started
            && operation.isValid() && parameterService) {
            parameterService->cancelExactOperation(
                reservation, operation,
                QStringLiteral("MagFit apply retired during write submission."));
        }
        scheduleDeferredReport();
        return;
    }
    if (m_runtime->cancelRequested || deadlineReached()) {
        m_runtime->deadlineExpired = deadlineReached();
        if (result == ParameterService::ExactSubmitResult::Started
            && operation.isValid()) {
            m_runtime->parameterOperation = operation;
            parameterService->cancelExactOperation(
                reservation, operation,
                QStringLiteral("MagFit apply retired during write submission."));
            scheduleDeferredReport();
            return;
        }
        scheduleDeferredReport();
        scheduleNext();
        return;
    }
    if (result == ParameterService::ExactSubmitResult::Started) {
        m_runtime->parameterOperation = operation;
        scheduleDeferredReport();
        m_runtime->status = QStringLiteral("Writing %1...").arg(write.name);
        emit stateChanged();
        return;
    }
    scheduleDeferredReport();
    if (result == ParameterService::ExactSubmitResult::TransportOutcomeUncertain
        || result == ParameterService::ExactSubmitResult::Quarantined) {
        finish(Outcome::OutcomeUncertain,
               submissionError.isEmpty()
                   ? QStringLiteral("A MagFit parameter write has an uncertain outcome; do not retry automatically.")
                   : submissionError + QStringLiteral(
                         " Outcome is uncertain; do not retry automatically."));
    } else {
        finish(Outcome::Rejected, submissionError.isEmpty()
            ? QStringLiteral("The exact MagFit parameter write was rejected before transmission.")
            : submissionError);
    }
}

void OfflineMagFitApplyService::handleParameterFinished(
    const ParameterService::ExactOperationReport &report)
{
    if (m_runtime->submissionInFlight) {
        const auto *submitted = m_runtime->submissionOperationOut;
        if (!m_runtime->busy || !m_runtime->activePlan.isValid()
            || !m_runtime->parameterReservation.isValid()
            || !submitted || !submitted->isValid()
            || report.token.operationId != submitted->operationId
            || report.token.reservationId != submitted->reservationId
            || report.token.kind != ParameterService::ExactOperationKind::Write
            || report.token.name != submitted->name
            || !report.token.lease.sameInstance(submitted->lease)
            || report.token.reservationId
                != m_runtime->parameterReservation.reservationId
            || !report.token.lease.sameInstance(
                m_runtime->activePlan.d.constData()->vehicle)) {
            return;
        }
        m_runtime->deferredReport = report;
        m_runtime->hasDeferredReport = true;
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
    if (deadlineReached()) m_runtime->deadlineExpired = true;
    const Write write = m_runtime->activePlan.d.constData()->writes.at(
        m_runtime->writeIndex);
    if (uncertainWriteResult(report.terminalResult)
        || (report.frameAttempted
            && report.terminalResult
                == ParameterService::ExactTerminalResult::Rejected)) {
        finish(Outcome::OutcomeUncertain,
               (report.description.isEmpty()
                    ? QStringLiteral("A MagFit parameter write outcome is uncertain.")
                    : report.description)
               + QStringLiteral(" Preserve receipts and do not retry automatically."));
        return;
    }
    if (report.terminalResult
        == ParameterService::ExactTerminalResult::WriteSucceeded) {
        m_runtime->report.receipts.append(write);
        ++m_runtime->report.confirmedWrites;
        ++m_runtime->writeIndex;
        m_runtime->progressCompleted = m_runtime->writeIndex;
        appendHistory(QStringLiteral("Acknowledged %1 = %2.")
                      .arg(write.name, report.value.toString()));
        if (m_runtime->cancelRequested) {
            finish(Outcome::Cancelled,
                   QStringLiteral("MagFit apply was cancelled; acknowledged receipts were preserved."));
            return;
        }
        if (m_runtime->deadlineExpired) {
            finish(Outcome::Rejected,
                   QStringLiteral("The bounded MagFit apply deadline expired; acknowledged receipts were preserved."));
            return;
        }
        const QPointer<OfflineMagFitApplyService> guard(this);
        emit stateChanged();
        if (!guard) return;
        scheduleNext();
        return;
    }
    finish(Outcome::Rejected,
           report.description.isEmpty()
               ? QStringLiteral("The MagFit parameter write was not acknowledged.")
               : report.description);
}

void OfflineMagFitApplyService::scheduleDeferredReport()
{
    if (!m_runtime->hasDeferredReport) return;
    const auto report = m_runtime->deferredReport;
    m_runtime->hasDeferredReport = false;
    QTimer::singleShot(0, this, [this, report]() {
        handleParameterFinished(report);
    });
}

bool OfflineMagFitApplyService::cancel(quint64 operationId)
{
    if (operationId == 0 || !m_runtime->busy
        || m_runtime->operationId != operationId
        || m_runtime->cancelRequested) return false;
    m_runtime->cancelRequested = true;
    m_runtime->status = QStringLiteral("Cancelling MagFit apply...");
    appendHistory(m_runtime->status);
    const QPointer<OfflineMagFitApplyService> guard(this);
    emit stateChanged();
    if (!guard) return true;
    if (m_runtime->parameterOperation.isValid()
        && m_runtime->parameterService) {
        m_runtime->parameterService->cancelExactOperation(
            m_runtime->parameterReservation,
            m_runtime->parameterOperation,
            QStringLiteral("MagFit apply was cancelled by the operator."));
        return true;
    }
    scheduleNext();
    return true;
}

void OfflineMagFitApplyService::handleDeadline()
{
    if (!m_runtime->busy || m_runtime->deadlineExpired) return;
    m_runtime->deadlineExpired = true;
    m_runtime->status = QStringLiteral(
        "The bounded MagFit apply deadline expired; stopping...");
    appendHistory(m_runtime->status);
    const QPointer<OfflineMagFitApplyService> guard(this);
    emit stateChanged();
    if (!guard) return;
    if (m_runtime->parameterOperation.isValid()
        && m_runtime->parameterService) {
        m_runtime->parameterService->cancelExactOperation(
            m_runtime->parameterReservation,
            m_runtime->parameterOperation,
            QStringLiteral("The bounded MagFit apply deadline expired."));
        return;
    }
    scheduleNext();
}

void OfflineMagFitApplyService::releaseReservations()
{
    const QPointer<OfflineMagFitApplyService> guard(this);
    const QPointer<ParameterService> parameters = m_runtime->parameterService;
    const QPointer<VehicleCommandService> commands = m_runtime->commandService;
    const auto parameterReservation = m_runtime->parameterReservation;
    const auto commandReservation = m_runtime->commandReservation;
    m_runtime->parameterReservation = {};
    m_runtime->commandReservation = {};
    if (parameters && parameterReservation.isValid()) {
        parameters->releaseExactReservation(parameterReservation);
    }
    if (!guard) return;
    if (commands && commandReservation.isValid()) {
        commands->releaseExactReservation(commandReservation);
    }
}

void OfflineMagFitApplyService::finish(
    Outcome outcome, const QString &description)
{
    if (!m_runtime->busy || m_runtime->finishing) return;
    m_runtime->finishing = true;
    m_runtime->deadlineTimer.stop();
    m_runtime->report.outcome = outcome;
    m_runtime->report.description = description;
    if (outcome == Outcome::Completed) {
        m_runtime->progressCompleted = m_runtime->progressTotal;
    }
    m_runtime->report.remainingWrites = qMax(
        0, m_runtime->report.totalWrites
            - m_runtime->report.confirmedWrites);
    const Report report = m_runtime->report;

    m_runtime->busy = false;
    m_runtime->operationId = 0;
    m_runtime->activePlan = {};
    m_runtime->parameterOperation = {};
    m_runtime->submissionOperationOut = nullptr;
    m_runtime->submissionInFlight = false;
    m_runtime->hasDeferredReport = false;
    m_runtime->nextScheduled = false;
    m_runtime->cancelRequested = false;
    m_runtime->deadlineExpired = false;
    m_runtime->lastReport = report;
    m_runtime->status = description;
    appendHistory(description);

    const QPointer<OfflineMagFitApplyService> guard(this);
    releaseReservations();
    if (!guard) return;
    m_runtime->finishing = false;
    emit stateChanged();
    if (!guard) return;
    emit operationFinished(report);
}

void OfflineMagFitApplyService::appendHistory(const QString &line)
{
    if (line.isEmpty()) return;
    m_runtime->history.append(line);
    while (m_runtime->history.size() > MaximumHistoryEntries) {
        m_runtime->history.removeFirst();
    }
}

void OfflineMagFitApplyService::shutdown()
{
    if (!m_runtime || m_runtime->shuttingDown) return;
    m_runtime->shuttingDown = true;
    m_runtime->deadlineTimer.stop();
    const QPointer<OfflineMagFitApplyService> guard(this);
    if (m_runtime->parameterService) {
        disconnect(m_runtime->parameterService, nullptr, this, nullptr);
        if (m_runtime->parameterOperation.isValid()) {
            m_runtime->parameterService->cancelExactOperation(
                m_runtime->parameterReservation,
                m_runtime->parameterOperation,
                QStringLiteral("MagFit apply is shutting down."));
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
    m_runtime->submissionInFlight = false;
    m_runtime->hasDeferredReport = false;
    m_runtime->nextScheduled = false;
    m_runtime->busy = false;
    m_runtime->operationId = 0;
    m_runtime->activePlan = {};
    m_runtime->pendingPlan = {};
    releaseReservations();
}
