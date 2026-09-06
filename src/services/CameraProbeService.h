#ifndef CAMERAPROBESERVICE_H
#define CAMERAPROBESERVICE_H

#include "comm/MavlinkComponentInstanceLease.h"
#include "comm/VehicleCommandService.h"
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

class VehicleTargetManager;
class MavlinkComponentRegistry;

// Application-owned implementation of MP10 Camera.test: six commands to one
// consented external component100 on the active physical link, never autopilot
// substitution. UI closure cancels remaining work, not already sent effects.
class CameraProbeService final : public QObject
{
    Q_OBJECT
public:
    struct Plan {
        quint64 planId = 0;
        VehicleTargetLease selection;
        MavlinkComponentInstanceLease camera;
        bool isValid() const {
            return planId && selection.isValid() && camera.isValid()
                && camera.endpoint.componentId == MAV_COMP_ID_CAMERA
                && selection.endpoint.linkId == camera.endpoint.linkId;
        }
    };
    enum class StepOutcome { NotSent, Accepted, Rejected, OutcomeUncertain };
    Q_ENUM(StepOutcome)
    struct StepResult {
        MAV_CMD command = static_cast<MAV_CMD>(0);
        StepOutcome outcome = StepOutcome::NotSent;
        int attempts = 0;
        int mavResult = -1;
        QString description;
    };
    struct Report {
        quint64 operationId = 0;
        Plan plan;
        QList<StepResult> steps;
        bool cancelled = false;
        QString description;
    };
    using RouteValidator = std::function<bool(const MavlinkComponentInstanceLease &, QString *)>;
    explicit CameraProbeService(VehicleTargetManager *targets,
        MavlinkComponentRegistry *components, VehicleCommandService *commands,
        RouteValidator validateRoute, QObject *parent = nullptr);
    ~CameraProbeService() override;
    static QList<MAV_CMD> Commands();
    static QString CommandName(MAV_CMD command);
    static QString ConfirmationText(const Plan &plan);
    bool busy() const { return m_busy || m_finishing; }
    quint64 currentOperationId() const { return m_report.operationId; }
    QStringList history() const { return m_history; }
    Report lastReport() const { return m_lastReport; }
    Report currentReport() const { return m_report; }
    bool prepare(Plan *plan, QString *error = nullptr);
    bool validate(const Plan &plan, QString *error = nullptr) const;
    bool execute(const Plan &plan, QString *error = nullptr) {
        return execute(plan, nullptr, error);
    }
    // Publishes the operation ID before validation/state/transport callbacks,
    // allowing a closing controller to cancel exactly its own admitted run.
    bool execute(const Plan &plan, quint64 *operationIdOut, QString *error);
    void cancel(quint64 operationId);
    void shutdown();
    void setTimeoutsForTesting(int acknowledgementMs, int maximumLifetimeMs);
signals:
    void stateChanged();
    void logMessage(QString message);
    void operationFinished(CameraProbeService::Report report);
private:
    bool capture(Plan *plan, QString *error) const;
    bool validateIdentity(const Plan &plan, QString *error) const;
    bool samePlan(const Plan &left, const Plan &right) const;
    void nextStep(quint64 revision);
    void commandFinished(const VehicleCommandService::ExactCommandReport &report);
    void invalidateSource();
    void finish(const QString &description, bool cancelled = false);
    void appendLog(const QString &message);
    QPointer<VehicleTargetManager> m_targets;
    QPointer<MavlinkComponentRegistry> m_components;
    QPointer<VehicleCommandService> m_commands;
    RouteValidator m_validateRoute;
    Plan m_prepared;
    Report m_report, m_lastReport;
    VehicleCommandService::ExactReservationToken m_reservation;
    VehicleCommandService::ExactCommandToken m_command;
    QStringList m_history;
    QString m_stopReason;
    quint64 m_nextPlan = 0, m_nextOperation = 0, m_revision = 0;
    int m_step = 0, m_acknowledgementMs = 2000, m_maximumLifetimeMs = 30000;
    bool m_busy = false, m_apiInFlight = false, m_shuttingDown = false;
    bool m_cancelRequested = false;
    bool m_finishing = false;
};
Q_DECLARE_METATYPE(CameraProbeService::Report)
#endif
