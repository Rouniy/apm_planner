#ifndef DEVELOPERVEHICLETOOLSERVICE_H
#define DEVELOPERVEHICLETOOLSERVICE_H

#include "comm/ParameterService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleEndpoint.h"
#include "core/parameters/ParameterCodec.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>

class VehicleTargetManager;

/**
 * Application-owned orchestrator for the deliberately dangerous, single-
 * vehicle actions exposed by Mission Planner's Developer page.
 *
 * prepare() captures the selected vehicle instance and, for parameter tools,
 * the committed parameter value.  The UI must obtain consent after prepare()
 * and call execute() with that same immutable plan.  Every externally callable
 * validation seam is followed by an identity/lifetime recheck, and the exact
 * parameter/command services remain responsible for their retry-time gates.
 */
class DeveloperVehicleToolService final : public QObject
{
    Q_OBJECT

public:
    enum class Action {
        SetQnh,
        AdjustBarometerAltitude,
        ForceAccelCalibrated,
        ForceCompassCalibrated,
        RebootVehicle,
        RebootToDfu,
        UpgradeBootloader
    };
    Q_ENUM(Action)

    struct Plan
    {
        quint64 planId = 0;
        Action action = Action::SetQnh;
        VehicleTargetLease target;
        SwarmVehicleInstanceLease vehicle;
        QString parameterName;
        QVariant originalValue;
        ParameterType parameterType = ParameterType::Unknown;
        quint64 parameterRevision = 0;
        // BARO1_GND_PRESS is internal-only in current ArduPilot.  The
        // fallback is offered only when this captured BRD_OPTIONS record has
        // bit 2 set; the service never changes BRD_OPTIONS itself.
        QString capabilityParameterName;
        QVariant capabilityValue;
        ParameterType capabilityType = ParameterType::Unknown;
        quint64 capabilityRevision = 0;

        bool isValid() const noexcept
        {
            return planId != 0 && target.isValid() && vehicle.isValid();
        }
    };

    enum class SubmitResult {
        Started,
        InvalidPlan,
        InvalidValue,
        Busy,
        Unavailable
    };
    Q_ENUM(SubmitResult)

    enum class Outcome {
        Succeeded,
        Rejected,
        OutcomeUncertain
    };
    Q_ENUM(Outcome)

    struct Report
    {
        quint64 operationId = 0;
        Action action = Action::SetQnh;
        VehicleEndpoint endpoint;
        Outcome outcome = Outcome::Rejected;
        QString description;

        bool isValid() const noexcept
        {
            return operationId != 0 && endpoint.isValid();
        }
    };

    using RouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &lease, QString *error)>;

    static constexpr int MaximumHeartbeatAgeMs = 3000;
    static constexpr int MaximumHistoryEntries = 256;
    static constexpr double MinimumPressurePa = 80000.0;
    static constexpr double MaximumPressurePa = 120000.0;
    static constexpr double MaximumAltitudeAdjustmentMetres = 100.0;
    static constexpr double PressurePerMetrePa = 11.1;
    static constexpr float BootloaderMagic = 290876.0F;
    static constexpr int BootloaderAcknowledgementTimeoutMs =
        5 * 60 * 1000;
    static constexpr int BootloaderMaximumLifetimeMs =
        5 * 60 * 1000;

    explicit DeveloperVehicleToolService(
        VehicleTargetManager *targetManager,
        SwarmTelemetryRegistry *telemetryRegistry,
        ParameterService *parameterService,
        VehicleCommandService *commandService,
        RouteValidator routeValidator,
        QObject *parent = nullptr);
    ~DeveloperVehicleToolService() override;

    bool busy() const noexcept { return m_busy; }
    QString status() const { return m_status; }
    QStringList history() const { return m_history; }
    Report lastReport() const { return m_lastReport; }
    quint64 currentOperationId() const noexcept { return m_operationId; }

    /** Reduces only the bootloader command deadline in focused tests. */
    void setBootloaderTimeoutForTesting(int timeoutMs);

    /** Detaches/retires reservations before transport services are torn down. */
    void shutdown();

    /** Read-only eligibility check suitable for periodically refreshing UI. */
    bool canPrepare(Action action, QString *error = nullptr) const;

    /** Captures a new immutable consent plan; supersedes an older idle plan. */
    bool prepare(Action action, Plan *planOut, QString *error = nullptr);

    /** Revalidates without changing the selected target or minting a plan. */
    bool validate(const Plan &plan, QString *error = nullptr) const;

    /**
     * For SetQnh, value is pressure in Pa.  For AdjustBarometerAltitude it is
     * the signed correction in metres.  The remaining actions ignore value.
     */
    SubmitResult execute(const Plan &plan,
                         double value = 0.0,
                         QString *error = nullptr);

signals:
    void stateChanged();
    void operationFinished(DeveloperVehicleToolService::Report report);

private:
    enum class ActiveKind { None, Parameter, Command };

    bool captureVehicle(VehicleTargetLease *target,
                        SwarmVehicleInstanceLease *vehicle,
                        QString *error) const;
    bool validateVehicle(const VehicleTargetLease &target,
                         const SwarmVehicleInstanceLease &vehicle,
                         QString *error) const;
    bool captureParameter(Action action, Plan *plan, QString *error) const;
    bool parameterStillMatches(const Plan &plan, QString *error) const;
    bool planMatchesPending(const Plan &plan) const;
    bool calculateRequestedValue(const Plan &plan, double input,
                                 QVariant *requested,
                                 QString *error) const;
    void handleParameterFinished(
        const ParameterService::ExactOperationReport &report);
    void handleCommandFinished(
        const VehicleCommandService::ExactCommandReport &report);
    void finish(Outcome outcome, const QString &description);
    void appendHistory(const QString &line);
    static QString actionName(Action action);

    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<SwarmTelemetryRegistry> m_telemetryRegistry;
    QPointer<ParameterService> m_parameterService;
    QPointer<VehicleCommandService> m_commandService;
    RouteValidator m_routeValidator;

    Plan m_pendingPlan;
    Plan m_activePlan;
    quint64 m_nextPlanId = 1;
    quint64 m_nextOperationId = 1;
    quint64 m_operationId = 0;
    ActiveKind m_activeKind = ActiveKind::None;
    ParameterService::ExactReservationToken m_parameterReservation;
    ParameterService::ExactOperationToken m_parameterOperation;
    VehicleCommandService::ExactReservationToken m_commandReservation;
    VehicleCommandService::ExactCommandToken m_commandOperation;
    bool m_busy = false;
    bool m_apiInFlight = false;
    bool m_finishing = false;
    bool m_shuttingDown = false;
    int m_bootloaderAcknowledgementTimeoutMs =
        BootloaderAcknowledgementTimeoutMs;
    int m_bootloaderMaximumLifetimeMs = BootloaderMaximumLifetimeMs;
    QString m_status;
    QStringList m_history;
    Report m_lastReport;
};

Q_DECLARE_METATYPE(DeveloperVehicleToolService::Action)
Q_DECLARE_METATYPE(DeveloperVehicleToolService::Plan)
Q_DECLARE_METATYPE(DeveloperVehicleToolService::Outcome)
Q_DECLARE_METATYPE(DeveloperVehicleToolService::Report)

#endif // DEVELOPERVEHICLETOOLSERVICE_H
