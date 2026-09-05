#include "DeveloperVehicleToolService.h"

#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"

#include <QMetaType>
#include <QPointer>
#include <QThread>

#include <cmath>
#include <utility>

namespace
{
class ScopeExit final
{
public:
    explicit ScopeExit(std::function<void()> callback)
        : m_callback(std::move(callback))
    {
    }
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

bool isParameterAction(DeveloperVehicleToolService::Action action)
{
    return action == DeveloperVehicleToolService::Action::SetQnh
        || action
            == DeveloperVehicleToolService::Action::AdjustBarometerAltitude;
}

bool samePlan(const DeveloperVehicleToolService::Plan &left,
              const DeveloperVehicleToolService::Plan &right)
{
    if (left.planId != right.planId || left.action != right.action
        || left.target.generation != right.target.generation
        || !left.target.endpoint.sameIdentity(right.target.endpoint)
        || !left.vehicle.sameInstance(right.vehicle)
        || left.parameterName != right.parameterName
        || left.parameterType != right.parameterType
        || left.parameterRevision != right.parameterRevision
        || left.capabilityParameterName != right.capabilityParameterName
        || left.capabilityType != right.capabilityType
        || left.capabilityRevision != right.capabilityRevision) {
        return false;
    }
    if (!isParameterAction(left.action)) {
        return true;
    }
    if (!ParameterCodec::valuesEqual(
            left.originalValue, right.originalValue, left.parameterType)) {
        return false;
    }
    return left.capabilityParameterName.isEmpty()
        || ParameterCodec::valuesEqual(
            left.capabilityValue, right.capabilityValue,
            left.capabilityType);
}

bool finiteVariant(const QVariant &value, double *number = nullptr)
{
    bool converted = false;
    const double convertedValue = value.toDouble(&converted);
    if (!converted || !std::isfinite(convertedValue)) {
        return false;
    }
    if (number) {
        *number = convertedValue;
    }
    return true;
}
}

DeveloperVehicleToolService::DeveloperVehicleToolService(
    VehicleTargetManager *targetManager,
    SwarmTelemetryRegistry *telemetryRegistry,
    ParameterService *parameterService,
    VehicleCommandService *commandService,
    RouteValidator routeValidator,
    QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_telemetryRegistry(telemetryRegistry)
    , m_parameterService(parameterService)
    , m_commandService(commandService)
    , m_routeValidator(std::move(routeValidator))
    , m_status(QStringLiteral("Ready."))
{
    qRegisterMetaType<Action>();
    qRegisterMetaType<Plan>();
    qRegisterMetaType<Outcome>();
    qRegisterMetaType<Report>();

    if (m_parameterService) {
        connect(m_parameterService,
                &ParameterService::exactOperationFinished,
                this,
                &DeveloperVehicleToolService::handleParameterFinished);
        if (m_parameterService->store()) {
            connect(m_parameterService->store(),
                    &ParameterStore::endpointStateChanged,
                    this,
                    [this]() { emit stateChanged(); });
            connect(m_parameterService->store(),
                    &ParameterStore::endpointParameterChanged,
                    this,
                    [this]() { emit stateChanged(); });
        }
    }
    if (m_commandService) {
        connect(m_commandService,
                &VehicleCommandService::exactCommandFinished,
                this,
                &DeveloperVehicleToolService::handleCommandFinished);
    }
    if (m_targetManager) {
        connect(m_targetManager,
                &VehicleTargetManager::currentTargetChanged,
                this,
                &DeveloperVehicleToolService::stateChanged);
        connect(m_targetManager,
                &VehicleTargetManager::targetGenerationSettled,
                this,
                [this]() { emit stateChanged(); });
    }
    if (m_telemetryRegistry) {
        connect(m_telemetryRegistry,
                &SwarmTelemetryRegistry::registryChanged,
                this,
                [this]() { emit stateChanged(); });
    }
}

DeveloperVehicleToolService::~DeveloperVehicleToolService()
{
    shutdown();
}

void DeveloperVehicleToolService::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    const QPointer<DeveloperVehicleToolService> serviceGuard(this);

    if (m_parameterService) {
        if (m_parameterService->store()) {
            disconnect(m_parameterService->store(), nullptr, this, nullptr);
        }
        disconnect(m_parameterService, nullptr, this, nullptr);
        if (m_parameterReservation.isValid()) {
            if (m_parameterOperation.isValid()) {
                m_parameterService->cancelExactOperation(
                    m_parameterReservation, m_parameterOperation,
                    QStringLiteral("Developer tools are shutting down."));
                if (!serviceGuard) {
                    return;
                }
            }
            m_parameterService->releaseExactReservation(
                m_parameterReservation);
            if (!serviceGuard) {
                return;
            }
        }
    }
    if (m_commandService) {
        disconnect(m_commandService, nullptr, this, nullptr);
        if (m_commandReservation.isValid()) {
            m_commandService->releaseExactReservation(m_commandReservation);
            if (!serviceGuard) {
                return;
            }
        }
    }
    if (m_targetManager) {
        disconnect(m_targetManager, nullptr, this, nullptr);
    }
    if (m_telemetryRegistry) {
        disconnect(m_telemetryRegistry, nullptr, this, nullptr);
    }

    m_parameterReservation = {};
    m_parameterOperation = {};
    m_commandReservation = {};
    m_commandOperation = {};
    m_pendingPlan = {};
    m_activePlan = {};
    m_activeKind = ActiveKind::None;
    m_operationId = 0;
    m_busy = false;
}

bool DeveloperVehicleToolService::captureVehicle(
    VehicleTargetLease *target,
    SwarmVehicleInstanceLease *vehicle,
    QString *error) const
{
    const QPointer<const DeveloperVehicleToolService> serviceGuard(this);
    if (target) {
        *target = {};
    }
    if (vehicle) {
        *vehicle = {};
    }
    if (QThread::currentThread() != thread()) {
        assignError(error,
                    QStringLiteral("Developer tools must run on their owner thread."));
        return false;
    }
    if (m_shuttingDown || !m_targetManager || !m_telemetryRegistry
        || !m_parameterService || !m_commandService || !m_routeValidator) {
        assignError(error,
                    QStringLiteral("Developer vehicle services are unavailable."));
        return false;
    }

    const VehicleTargetLease capturedTarget =
        m_targetManager->acquireTarget();
    if (!capturedTarget.isValid()) {
        assignError(error, QStringLiteral("Select a connected vehicle first."));
        return false;
    }
    const SwarmVehicleInstanceLease capturedVehicle =
        m_telemetryRegistry->acquireVehicle(
            capturedTarget.endpoint, MaximumHeartbeatAgeMs);
    if (!capturedVehicle.isValid()) {
        assignError(error,
                    QStringLiteral("The selected vehicle has no fresh exact telemetry."));
        return false;
    }
    if (!validateVehicle(capturedTarget, capturedVehicle, error)) {
        return false;
    }
    if (!serviceGuard) {
        assignError(error,
                    QStringLiteral("Developer vehicle services were destroyed during validation."));
        return false;
    }
    if (target) {
        *target = capturedTarget;
    }
    if (vehicle) {
        *vehicle = capturedVehicle;
    }
    return true;
}

bool DeveloperVehicleToolService::validateVehicle(
    const VehicleTargetLease &target,
    const SwarmVehicleInstanceLease &vehicle,
    QString *error) const
{
    const QPointer<const DeveloperVehicleToolService> serviceGuard(this);
    if (QThread::currentThread() != thread()) {
        assignError(error,
                    QStringLiteral("Developer tools must run on their owner thread."));
        return false;
    }
    if (m_shuttingDown || !m_targetManager || !m_telemetryRegistry
        || !m_parameterService || !m_commandService || !m_routeValidator) {
        assignError(error,
                    QStringLiteral("Developer vehicle services are unavailable."));
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
        if (m_shuttingDown || !m_targetManager || !m_telemetryRegistry
            || !m_parameterService || !m_commandService
            || !m_targetManager->isTargetGenerationSettled()
            || !m_targetManager->isCurrentTarget(
                target.endpoint.linkId,
                target.endpoint.systemId,
                target.endpoint.componentId,
                target.generation)
            || !m_targetManager->hasFreshHeartbeat(
                target, MaximumHeartbeatAgeMs)
            || m_targetManager->heartbeatArmed(target)
            || m_targetManager->heartbeatAutopilot(target)
                != MAV_AUTOPILOT_ARDUPILOTMEGA
            || !m_telemetryRegistry->validateLease(
                vehicle, MaximumHeartbeatAgeMs)) {
            assignError(why,
                        QStringLiteral(
                            "The selected ArduPilot vehicle changed, became stale, or is armed."));
            return false;
        }

        SwarmTelemetrySnapshot snapshot;
        if (!m_telemetryRegistry->snapshotForLease(vehicle, &snapshot)
            || !snapshot.heartbeatValid
            || !m_telemetryRegistry->observationIsFresh(
                snapshot.heartbeatObservedMs, MaximumHeartbeatAgeMs)
            || snapshot.armed
            || snapshot.autopilot != MAV_AUTOPILOT_ARDUPILOTMEGA) {
            assignError(why,
                        QStringLiteral(
                            "Fresh disarmed ArduPilot telemetry is required."));
            return false;
        }

        int vehiclesOnPhysicalLink = 0;
        bool capturedEndpointPresent = false;
        const QList<VehicleEndpoint> endpoints =
            m_telemetryRegistry->endpoints();
        for (const VehicleEndpoint &candidate : endpoints) {
            if (candidate.linkId != vehicle.endpoint.linkId) {
                continue;
            }
            ++vehiclesOnPhysicalLink;
            capturedEndpointPresent = capturedEndpointPresent
                || candidate.sameIdentity(vehicle.endpoint);
        }
        if (vehiclesOnPhysicalLink != 1 || !capturedEndpointPresent) {
            assignError(why,
                        QStringLiteral(
                            "Developer actions require one exact vehicle on the physical link."));
            return false;
        }
        return true;
    };

    if (!stateIsSafe(error)) {
        return false;
    }
    const RouteValidator routeValidator = m_routeValidator;
    QString routeError;
    if (!routeValidator(vehicle, &routeError)) {
        if (!serviceGuard) {
            assignError(error,
                        QStringLiteral("Developer vehicle services were destroyed during route validation."));
            return false;
        }
        assignError(error,
                    routeError.isEmpty()
                        ? QStringLiteral("No safe exact vehicle route is available.")
                        : routeError);
        return false;
    }
    if (!serviceGuard) {
        assignError(error,
                    QStringLiteral("Developer vehicle services were destroyed during route validation."));
        return false;
    }
    // Route validation is an injected callback and may synchronously mutate
    // target selection, telemetry, or service ownership.
    return stateIsSafe(error);
}

bool DeveloperVehicleToolService::captureParameter(
    Action action, Plan *plan, QString *error) const
{
    if (!plan || !isParameterAction(action) || !m_parameterService
        || !m_parameterService->store()) {
        assignError(error,
                    QStringLiteral("The parameter repository is unavailable."));
        return false;
    }
    const ParameterSnapshot snapshot =
        m_parameterService->store()->snapshot(plan->vehicle.endpoint);
    if (!snapshot.isComplete()) {
        assignError(error,
                    QStringLiteral("Read a complete vehicle parameter set first."));
        return false;
    }

    const quint8 componentId =
        static_cast<quint8>(plan->vehicle.endpoint.componentId);
    const QString primary = QStringLiteral("GND_ABS_PRESS");
    const QString fallback = QStringLiteral("BARO1_GND_PRESS");
    const QString parameter = snapshot.contains(componentId, primary)
        ? primary : (snapshot.contains(componentId, fallback)
            ? fallback : QString());
    if (parameter.isEmpty()) {
        assignError(error,
                    QStringLiteral(
                        "Neither GND_ABS_PRESS nor BARO1_GND_PRESS is available."));
        return false;
    }
    const ParameterRecord record = snapshot.value(componentId, parameter);
    if (record.type != ParameterType::Real32
        || !finiteVariant(record.value)) {
        assignError(error,
                    QStringLiteral("The barometer pressure must be a valid REAL32 parameter."));
        return false;
    }

    plan->parameterName = parameter;
    plan->originalValue = record.value;
    plan->parameterType = record.type;
    plan->parameterRevision = record.targetRevision;

    if (parameter == fallback) {
        const QString boardOptions = QStringLiteral("BRD_OPTIONS");
        if (!snapshot.contains(componentId, boardOptions)) {
            assignError(error,
                        QStringLiteral(
                            "BARO1_GND_PRESS is internal/read-only; this tool never changes BRD_OPTIONS."));
            return false;
        }
        const ParameterRecord capability =
            snapshot.value(componentId, boardOptions);
        bool converted = false;
        const qlonglong optionBits = capability.value.toLongLong(&converted);
        if (!converted || optionBits < 0
            || !ParameterCodec::isInteger(capability.type)
            || (optionBits & (1LL << 2)) == 0) {
            assignError(error,
                        QStringLiteral(
                            "Firmware does not permit internal pressure writes; this tool never changes BRD_OPTIONS."));
            return false;
        }
        plan->capabilityParameterName = boardOptions;
        plan->capabilityValue = capability.value;
        plan->capabilityType = capability.type;
        plan->capabilityRevision = capability.targetRevision;
    }
    return true;
}

bool DeveloperVehicleToolService::parameterStillMatches(
    const Plan &plan, QString *error) const
{
    if (!isParameterAction(plan.action)) {
        return true;
    }
    if (!m_parameterService || !m_parameterService->store()) {
        assignError(error,
                    QStringLiteral("The parameter repository is unavailable."));
        return false;
    }
    const ParameterSnapshot snapshot =
        m_parameterService->store()->snapshot(plan.vehicle.endpoint);
    const quint8 componentId =
        static_cast<quint8>(plan.vehicle.endpoint.componentId);
    if (!snapshot.isComplete()
        || !snapshot.contains(componentId, plan.parameterName)) {
        assignError(error,
                    QStringLiteral("The captured parameter snapshot is no longer available."));
        return false;
    }

    const QString preferred = snapshot.contains(
        componentId, QStringLiteral("GND_ABS_PRESS"))
        ? QStringLiteral("GND_ABS_PRESS")
        : QStringLiteral("BARO1_GND_PRESS");
    const ParameterRecord current =
        snapshot.value(componentId, plan.parameterName);
    if (preferred != plan.parameterName
        || current.type != plan.parameterType
        || current.targetRevision != plan.parameterRevision
        || !ParameterCodec::valuesEqual(
            current.value, plan.originalValue, plan.parameterType)) {
        assignError(error,
                    QStringLiteral(
                        "The parameter snapshot changed while confirmation was open."));
        return false;
    }
    if (!plan.capabilityParameterName.isEmpty()) {
        if (!snapshot.contains(componentId, plan.capabilityParameterName)) {
            assignError(error,
                        QStringLiteral("The BRD_OPTIONS capability snapshot changed."));
            return false;
        }
        const ParameterRecord capability = snapshot.value(
            componentId, plan.capabilityParameterName);
        bool converted = false;
        const qlonglong optionBits = capability.value.toLongLong(&converted);
        if (!converted || optionBits < 0
            || !ParameterCodec::isInteger(capability.type)
            || (optionBits & (1LL << 2)) == 0
            || capability.type != plan.capabilityType
            || capability.targetRevision != plan.capabilityRevision
            || !ParameterCodec::valuesEqual(
                capability.value, plan.capabilityValue,
                plan.capabilityType)) {
            assignError(error,
                        QStringLiteral("The BRD_OPTIONS capability snapshot changed."));
            return false;
        }
    }
    return true;
}

bool DeveloperVehicleToolService::planMatchesPending(const Plan &plan) const
{
    return m_pendingPlan.isValid() && samePlan(plan, m_pendingPlan);
}

bool DeveloperVehicleToolService::canPrepare(Action action,
                                             QString *error) const
{
    const QPointer<const DeveloperVehicleToolService> serviceGuard(this);
    if (error) {
        error->clear();
    }
    if (m_busy || m_apiInFlight || m_finishing) {
        assignError(error,
                    QStringLiteral("Another developer vehicle action is active."));
        return false;
    }
    VehicleTargetLease target;
    SwarmVehicleInstanceLease vehicle;
    if (!captureVehicle(&target, &vehicle, error)) {
        return false;
    }
    if (!serviceGuard) {
        assignError(error,
                    QStringLiteral("Developer vehicle services were destroyed during validation."));
        return false;
    }
    if (isParameterAction(action)) {
        Plan plan;
        plan.action = action;
        plan.target = target;
        plan.vehicle = vehicle;
        return captureParameter(action, &plan, error);
    }
    switch (action) {
    case Action::ForceAccelCalibrated:
    case Action::ForceCompassCalibrated:
    case Action::RebootVehicle:
    case Action::RebootToDfu:
        return true;
    case Action::SetQnh:
    case Action::AdjustBarometerAltitude:
        break;
    }
    assignError(error, QStringLiteral("The developer action is invalid."));
    return false;
}

bool DeveloperVehicleToolService::prepare(Action action,
                                          Plan *planOut,
                                          QString *error)
{
    const QPointer<DeveloperVehicleToolService> serviceGuard(this);
    if (planOut) {
        *planOut = {};
    }
    if (error) {
        error->clear();
    }
    if (!planOut) {
        assignError(error, QStringLiteral("A plan output is required."));
        return false;
    }
    if (m_busy || m_apiInFlight || m_finishing) {
        assignError(error,
                    QStringLiteral("Another developer vehicle action is active."));
        return false;
    }
    m_apiInFlight = true;
    ScopeExit apiGuard([serviceGuard]() {
        if (serviceGuard) {
            serviceGuard->m_apiInFlight = false;
            emit serviceGuard->stateChanged();
        }
    });
    if (m_nextPlanId == 0) {
        assignError(error, QStringLiteral("Developer plan identifiers are exhausted."));
        return false;
    }

    Plan captured;
    captured.action = action;
    if (!captureVehicle(&captured.target, &captured.vehicle, error)) {
        return false;
    }
    if (!serviceGuard) {
        assignError(error,
                    QStringLiteral("Developer vehicle services were destroyed during validation."));
        return false;
    }
    if (isParameterAction(action)
        && !captureParameter(action, &captured, error)) {
        return false;
    }
    if (!isParameterAction(action)) {
        switch (action) {
        case Action::ForceAccelCalibrated:
        case Action::ForceCompassCalibrated:
        case Action::RebootVehicle:
        case Action::RebootToDfu:
            break;
        default:
            assignError(error, QStringLiteral("The developer action is invalid."));
            return false;
        }
    }

    captured.planId = m_nextPlanId++;
    m_pendingPlan = captured;
    *planOut = captured;
    return true;
}

bool DeveloperVehicleToolService::validate(const Plan &plan,
                                           QString *error) const
{
    const QPointer<const DeveloperVehicleToolService> serviceGuard(this);
    if (error) {
        error->clear();
    }
    if (m_busy || m_apiInFlight || m_finishing) {
        assignError(error,
                    QStringLiteral("Another developer vehicle action is active."));
        return false;
    }
    if (!plan.isValid() || !planMatchesPending(plan)) {
        assignError(error,
                    QStringLiteral("The captured developer action plan is stale."));
        return false;
    }
    if (!validateVehicle(plan.target, plan.vehicle, error)
        || !serviceGuard) {
        return false;
    }
    if (m_busy || m_apiInFlight || m_finishing
        || !planMatchesPending(plan)) {
        assignError(error,
                    QStringLiteral("The captured developer action plan changed during validation."));
        return false;
    }
    return parameterStillMatches(plan, error);
}

bool DeveloperVehicleToolService::calculateRequestedValue(
    const Plan &plan, double input, QVariant *requested, QString *error) const
{
    if (requested) {
        *requested = {};
    }
    if (!std::isfinite(input)) {
        assignError(error, QStringLiteral("The entered value must be finite."));
        return false;
    }
    if (plan.action == Action::SetQnh) {
        if (input < MinimumPressurePa || input > MaximumPressurePa) {
            assignError(error,
                        QStringLiteral("QNH pressure must be between 80000 and 120000 Pa."));
            return false;
        }
        if (requested) {
            *requested = input;
        }
        return true;
    }
    if (plan.action == Action::AdjustBarometerAltitude) {
        if (input < -MaximumAltitudeAdjustmentMetres
            || input > MaximumAltitudeAdjustmentMetres
            || std::abs(input) < 1.0e-9) {
            assignError(error,
                        QStringLiteral(
                            "Altitude correction must be non-zero and between -100 and 100 m."));
            return false;
        }
        double pressure = 0.0;
        if (!finiteVariant(plan.originalValue, &pressure)) {
            assignError(error, QStringLiteral("The captured pressure is invalid."));
            return false;
        }
        const double adjusted = pressure + input * PressurePerMetrePa;
        if (!std::isfinite(adjusted)
            || adjusted < MinimumPressurePa
            || adjusted > MaximumPressurePa) {
            assignError(error,
                        QStringLiteral("The adjusted pressure is outside the safe range."));
            return false;
        }
        if (requested) {
            *requested = adjusted;
        }
        return true;
    }
    if (requested) {
        *requested = {};
    }
    return true;
}

DeveloperVehicleToolService::SubmitResult
DeveloperVehicleToolService::execute(const Plan &plan,
                                     double value,
                                     QString *error)
{
    const QPointer<DeveloperVehicleToolService> serviceGuard(this);
    if (error) {
        error->clear();
    }
    if (m_busy || m_apiInFlight || m_finishing) {
        assignError(error,
                    QStringLiteral("Another developer vehicle action is active."));
        return SubmitResult::Busy;
    }
    m_apiInFlight = true;
    ScopeExit apiGuard([serviceGuard]() {
        if (serviceGuard) {
            serviceGuard->m_apiInFlight = false;
            emit serviceGuard->stateChanged();
        }
    });
    if (!plan.isValid() || !planMatchesPending(plan)) {
        assignError(error,
                    QStringLiteral("The captured developer action plan is stale."));
        return SubmitResult::InvalidPlan;
    }

    QVariant requestedValue;
    if (!calculateRequestedValue(plan, value, &requestedValue, error)) {
        return SubmitResult::InvalidValue;
    }
    if (!validateVehicle(plan.target, plan.vehicle, error)
        || !serviceGuard
        || !parameterStillMatches(plan, error)) {
        return SubmitResult::Unavailable;
    }
    if (m_busy || m_shuttingDown || !planMatchesPending(plan)) {
        assignError(error,
                    QStringLiteral("The captured developer action plan changed during validation."));
        return SubmitResult::InvalidPlan;
    }
    if (m_nextOperationId == 0) {
        assignError(error,
                    QStringLiteral("Developer operation identifiers are exhausted."));
        return SubmitResult::Unavailable;
    }

    m_activePlan = plan;
    m_pendingPlan = {};
    m_operationId = m_nextOperationId++;
    m_parameterOperation = {};
    m_commandOperation = {};
    m_busy = true;
    m_status = actionName(plan.action) + QStringLiteral(": submitting...");
    appendHistory(m_status);
    const quint64 operationId = m_operationId;

    if (isParameterAction(plan.action)) {
        m_activeKind = ActiveKind::Parameter;
        // Own the exact command lane as well as the parameter lane.  This
        // prevents a legacy compass calibration (which aliases the same
        // target and safety state) from starting while a pressure write is
        // awaiting acknowledgement or retrying.
        VehicleCommandService::ExactReservationToken commandReservation;
        QString submitError;
        const auto commandReserveResult =
            m_commandService->reserveSingleVehicleEndpoint(
                this, plan.target, plan.vehicle,
                &commandReservation, &submitError);
        if (!serviceGuard) {
            return SubmitResult::Unavailable;
        }
        if (m_shuttingDown || !m_busy || m_operationId != operationId) {
            if (commandReservation.isValid() && m_commandService) {
                m_commandService->releaseExactReservation(
                    commandReservation);
            }
            assignError(error,
                        QStringLiteral("Developer tools shut down during command-lane reservation."));
            return SubmitResult::Unavailable;
        }
        if (commandReserveResult
            != VehicleCommandService::ExactReservationResult::Reserved) {
            assignError(error, submitError);
            finish(Outcome::Rejected,
                   submitError.isEmpty()
                       ? QStringLiteral("The exact command safety lane could not be reserved.")
                       : submitError);
            return SubmitResult::Unavailable;
        }
        m_commandReservation = commandReservation;

        ParameterService::ExactReservationToken reservation;
        const auto reserveResult =
            m_parameterService->reserveSingleVehicleEndpoint(
                this, plan.target, plan.vehicle, &reservation, &submitError);
        if (!serviceGuard) {
            return SubmitResult::Unavailable;
        }
        if (m_shuttingDown || !m_busy || m_operationId != operationId) {
            if (reservation.isValid() && m_parameterService) {
                m_parameterService->releaseExactReservation(reservation);
            }
            assignError(error,
                        QStringLiteral("Developer tools shut down during parameter reservation."));
            return SubmitResult::Unavailable;
        }
        if (reserveResult
            != ParameterService::ExactReservationResult::Reserved) {
            assignError(error, submitError);
            finish(Outcome::Rejected,
                   submitError.isEmpty()
                       ? QStringLiteral("The exact parameter route could not be reserved.")
                       : submitError);
            return SubmitResult::Unavailable;
        }
        m_parameterReservation = reservation;

        if (!validateVehicle(plan.target, plan.vehicle, &submitError)
            || !serviceGuard
            || !parameterStillMatches(plan, &submitError)) {
            assignError(error, submitError);
            finish(Outcome::Rejected, submitError);
            return SubmitResult::Unavailable;
        }

        ParameterService::ExactWriteRequest request;
        request.name = plan.parameterName;
        request.value = requestedValue;
        request.type = plan.parameterType;
        request.force = true;
        const QPointer<DeveloperVehicleToolService> guard(this);
        request.validateBeforeWrite =
            [guard, plan, operationId](QString *gateError) {
                if (!guard || !guard->m_busy
                    || guard->m_operationId != operationId
                    || guard->m_activeKind != ActiveKind::Parameter
                    || !samePlan(guard->m_activePlan, plan)) {
                    assignError(gateError,
                                QStringLiteral("The developer parameter plan was retired."));
                    return false;
                }
                return guard->validateVehicle(
                           plan.target, plan.vehicle, gateError)
                    && guard->parameterStillMatches(plan, gateError);
            };

        ParameterService::ExactOperationToken operation;
        const auto submitResult = m_parameterService->submitExactWrite(
            reservation, plan.vehicle, request, &operation, &submitError);
        if (!serviceGuard) {
            return SubmitResult::Unavailable;
        }
        if (m_shuttingDown) {
            return SubmitResult::Unavailable;
        }
        if (!m_busy || m_operationId != operationId) {
            return SubmitResult::Started;
        }
        if (submitResult != ParameterService::ExactSubmitResult::Started) {
            assignError(error, submitError);
            const Outcome outcome = submitResult
                    == ParameterService::ExactSubmitResult::TransportOutcomeUncertain
                ? Outcome::OutcomeUncertain : Outcome::Rejected;
            finish(outcome,
                   submitError.isEmpty()
                       ? QStringLiteral("The parameter write could not start.")
                       : submitError);
            return SubmitResult::Unavailable;
        }
        m_parameterOperation = operation;
    } else {
        m_activeKind = ActiveKind::Command;
        VehicleCommandService::ExactReservationToken reservation;
        QString submitError;
        const auto reserveResult =
            m_commandService->reserveSingleVehicleEndpoint(
                this, plan.target, plan.vehicle, &reservation, &submitError);
        if (!serviceGuard) {
            return SubmitResult::Unavailable;
        }
        if (m_shuttingDown || !m_busy || m_operationId != operationId) {
            if (reservation.isValid() && m_commandService) {
                m_commandService->releaseExactReservation(reservation);
            }
            assignError(error,
                        QStringLiteral("Developer tools shut down during command reservation."));
            return SubmitResult::Unavailable;
        }
        if (reserveResult
            != VehicleCommandService::ExactReservationResult::Reserved) {
            assignError(error, submitError);
            finish(Outcome::Rejected,
                   submitError.isEmpty()
                       ? QStringLiteral("The exact command route could not be reserved.")
                       : submitError);
            return SubmitResult::Unavailable;
        }
        m_commandReservation = reservation;

        if (!validateVehicle(plan.target, plan.vehicle, &submitError)
            || !serviceGuard) {
            assignError(error, submitError);
            finish(Outcome::Rejected, submitError);
            return SubmitResult::Unavailable;
        }

        VehicleCommandService::ExactCommandRequest request;
        if (plan.action == Action::ForceAccelCalibrated) {
            request.command = MAV_CMD_PREFLIGHT_CALIBRATION;
            request.params[4] = 76.0F;
        } else if (plan.action == Action::ForceCompassCalibrated) {
            request.command = MAV_CMD_PREFLIGHT_CALIBRATION;
            request.params[1] = 76.0F;
        } else if (plan.action == Action::RebootVehicle) {
            request.command = MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN;
            request.params[0] = 1.0F;
        } else if (plan.action == Action::RebootToDfu) {
            request.command = MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN;
            request.params[0] = 42.0F;
            request.params[1] = 24.0F;
            request.params[2] = 71.0F;
            request.params[3] = 99.0F;
        } else {
            finish(Outcome::Rejected,
                   QStringLiteral("The developer command is invalid."));
            return SubmitResult::InvalidPlan;
        }
        const QPointer<DeveloperVehicleToolService> guard(this);
        request.validateBeforeWrite =
            [guard, plan, operationId](QString *gateError) {
                if (!guard || !guard->m_busy
                    || guard->m_operationId != operationId
                    || guard->m_activeKind != ActiveKind::Command
                    || !samePlan(guard->m_activePlan, plan)) {
                    assignError(gateError,
                                QStringLiteral("The developer command plan was retired."));
                    return false;
                }
                return guard->validateVehicle(
                    plan.target, plan.vehicle, gateError);
            };

        VehicleCommandService::ExactCommandToken command;
        const auto submitResult =
            m_commandService->submitExactCommandLong(
                reservation, plan.vehicle, request, &command, &submitError);
        if (!serviceGuard) {
            return SubmitResult::Unavailable;
        }
        if (m_shuttingDown) {
            return SubmitResult::Unavailable;
        }
        if (!m_busy || m_operationId != operationId) {
            return SubmitResult::Started;
        }
        if (submitResult != VehicleCommandService::ExactSubmitResult::Started) {
            assignError(error, submitError);
            const Outcome outcome = submitResult
                    == VehicleCommandService::ExactSubmitResult::TransportOutcomeUncertain
                ? Outcome::OutcomeUncertain : Outcome::Rejected;
            finish(outcome,
                   submitError.isEmpty()
                       ? QStringLiteral("The vehicle command could not start.")
                       : submitError);
            return SubmitResult::Unavailable;
        }
        m_commandOperation = command;
    }

    emit stateChanged();
    return SubmitResult::Started;
}

void DeveloperVehicleToolService::handleParameterFinished(
    const ParameterService::ExactOperationReport &report)
{
    if (!m_busy || m_activeKind != ActiveKind::Parameter
        || report.token.reservationId == 0
        || report.token.reservationId
            != m_parameterReservation.reservationId
        || !report.token.lease.sameInstance(m_activePlan.vehicle)
        || report.token.kind != ParameterService::ExactOperationKind::Write
        || report.token.name != m_activePlan.parameterName
        || (m_parameterOperation.isValid()
            && report.token.operationId
                != m_parameterOperation.operationId)) {
        return;
    }

    Outcome outcome = Outcome::Rejected;
    switch (report.terminalResult) {
    case ParameterService::ExactTerminalResult::WriteSucceeded:
    case ParameterService::ExactTerminalResult::WriteSkipped:
        outcome = Outcome::Succeeded;
        break;
    case ParameterService::ExactTerminalResult::WriteCancelledOutcomeUncertain:
    case ParameterService::ExactTerminalResult::WriteTimedOutOutcomeUncertain:
    case ParameterService::ExactTerminalResult::WriteTransportOutcomeUncertain:
    case ParameterService::ExactTerminalResult::WriteLeaseRetiredOutcomeUncertain:
    case ParameterService::ExactTerminalResult::WriteLinkForgottenOutcomeUncertain:
        outcome = Outcome::OutcomeUncertain;
        break;
    default:
        outcome = Outcome::Rejected;
        break;
    }
    const QString description = report.description.isEmpty()
        ? QStringLiteral("The exact parameter transaction ended.")
        : report.description;
    finish(outcome, description);
}

void DeveloperVehicleToolService::handleCommandFinished(
    const VehicleCommandService::ExactCommandReport &report)
{
    if (!m_busy || m_activeKind != ActiveKind::Command
        || report.token.reservationId == 0
        || report.token.reservationId != m_commandReservation.reservationId
        || !report.token.lease.sameInstance(m_activePlan.vehicle)
        || (m_commandOperation.isValid()
            && report.token.transactionId
                != m_commandOperation.transactionId)) {
        return;
    }

    Outcome outcome = Outcome::OutcomeUncertain;
    switch (report.terminalResult) {
    case VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted:
        outcome = Outcome::Succeeded;
        break;
    case VehicleCommandService::ExactTerminalResult::AcknowledgedRejected:
    case VehicleCommandService::ExactTerminalResult::RejectedBeforeTransmission:
        outcome = Outcome::Rejected;
        break;
    case VehicleCommandService::ExactTerminalResult::TimedOutOutcomeUncertain:
    case VehicleCommandService::ExactTerminalResult::TransportOutcomeUncertain:
    case VehicleCommandService::ExactTerminalResult::LeaseRetiredOutcomeUncertain:
    case VehicleCommandService::ExactTerminalResult::LinkForgottenOutcomeUncertain:
        outcome = Outcome::OutcomeUncertain;
        break;
    }
    QString description = report.description.isEmpty()
        ? QStringLiteral("The exact command transaction ended.")
        : report.description;
    if (m_activePlan.action == Action::RebootToDfu
        && outcome == Outcome::OutcomeUncertain) {
        description += QStringLiteral(
            " DFU may reboot before acknowledgement. Connection loss does not confirm DFU entry.");
    }
    finish(outcome, description);
}

void DeveloperVehicleToolService::finish(Outcome outcome,
                                         const QString &description)
{
    if (!m_busy || m_finishing) {
        return;
    }
    m_finishing = true;

    const ParameterService::ExactReservationToken parameterReservation =
        m_parameterReservation;
    const VehicleCommandService::ExactReservationToken commandReservation =
        m_commandReservation;
    Report report;
    report.operationId = m_operationId;
    report.action = m_activePlan.action;
    report.endpoint = m_activePlan.vehicle.endpoint;
    report.outcome = outcome;
    report.description = description;

    m_busy = false;
    m_activeKind = ActiveKind::None;
    m_parameterReservation = {};
    m_parameterOperation = {};
    m_commandReservation = {};
    m_commandOperation = {};
    m_operationId = 0;
    m_activePlan = {};
    m_lastReport = report;
    m_status = actionName(report.action) + QStringLiteral(": ")
        + description;
    appendHistory(m_status);

    QPointer<DeveloperVehicleToolService> guard(this);
    if (m_parameterService && parameterReservation.isValid()) {
        m_parameterService->releaseExactReservation(parameterReservation);
        if (!guard) {
            return;
        }
    }
    if (m_commandService && commandReservation.isValid()) {
        m_commandService->releaseExactReservation(commandReservation);
    }
    if (!guard) {
        return;
    }
    m_finishing = false;
    emit stateChanged();
    if (!guard) {
        return;
    }
    emit operationFinished(report);
}

void DeveloperVehicleToolService::appendHistory(const QString &line)
{
    if (line.isEmpty()) {
        return;
    }
    m_history.append(line);
    while (m_history.size() > MaximumHistoryEntries) {
        m_history.removeFirst();
    }
}

QString DeveloperVehicleToolService::actionName(Action action)
{
    switch (action) {
    case Action::SetQnh:
        return QStringLiteral("Set QNH");
    case Action::AdjustBarometerAltitude:
        return QStringLiteral("Adjust Barometer Altitude");
    case Action::ForceAccelCalibrated:
        return QStringLiteral("Force Accel Calibrated");
    case Action::ForceCompassCalibrated:
        return QStringLiteral("Force Compass Calibrated");
    case Action::RebootVehicle:
        return QStringLiteral("Reboot Vehicle");
    case Action::RebootToDfu:
        return QStringLiteral("Reboot to DFU");
    }
    return QStringLiteral("Developer action");
}
