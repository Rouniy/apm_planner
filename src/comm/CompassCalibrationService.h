#ifndef COMPASSCALIBRATIONSERVICE_H
#define COMPASSCALIBRATIONSERVICE_H

#include "VehicleEndpoint.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

#include <array>

#include <mavlink.h>

class VehicleCommandService;
class VehicleTargetManager;

/**
 * Exact-target onboard compass-calibration state machine.
 *
 * The service owns no link.  Its application owner forwards received MAVLink
 * frames together with their physical link id and forwards link removal to
 * forgetLink().  Every operation pins one immutable VehicleTargetLease.
 */
class CompassCalibrationService final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        StartPending,
        Running,
        AwaitingAccept,
        AcceptPending,
        CancelPending,
        FixedYawPending,
        CompletedNeedsReboot,
        FixedYawCompleted,
        Failed,
        OutcomeUncertain
    };
    Q_ENUM(State)

    enum class RequestResult {
        Started,
        Busy,
        InvalidTarget,
        Armed,
        InvalidHeading,
        RebootRequired,
        InvalidState,
        OutcomeUncertain,
        TransportUnavailable,
        ShuttingDown
    };
    Q_ENUM(RequestResult)

    static constexpr int DefaultCommandTimeoutMs = 10000;
    static constexpr int DefaultActivityTimeoutMs = 30000;
    static constexpr int DefaultTotalTimeoutMs = 10 * 60 * 1000;

    CompassCalibrationService(VehicleTargetManager *targetManager,
                              VehicleCommandService *commandService,
                              QObject *parent = nullptr);
    ~CompassCalibrationService() override;

    void setLocalIdentity(quint8 systemId, quint8 componentId);
    void setTimeoutsForTesting(int commandTimeoutMs,
                               int activityTimeoutMs,
                               int totalTimeoutMs);

    State state() const { return m_state; }
    int progress(int compassIndex) const;
    QString resultText() const { return m_resultText; }
    bool rebootRequired() const { return !m_rebootTargets.isEmpty(); }
    bool rebootRequiredFor(const VehicleTargetLease &lease) const;
    bool isCurrentTarget(const VehicleTargetLease &lease) const;
    VehicleTargetLease activeTarget() const { return m_lease; }
    bool hasActiveTarget() const { return m_lease.isValid(); }
    quint8 calibrationMask() const { return m_calibrationMask; }
    quint8 manualAcceptMask() const { return m_manualAcceptMask; }
    bool isOnboardActive() const;
    bool canCancel() const;
    bool isBusy() const;

    RequestResult start(const VehicleTargetLease &lease, bool armed);
    RequestResult accept(bool armed);
    RequestResult cancel();
    RequestResult fixedYaw(const VehicleTargetLease &lease,
                           double headingDegrees, bool armed);

    void observeMessage(int linkId, const mavlink_message_t &message);
    void forgetLink(int linkId);
    void forgetLink();
    void clearRebootRequired(const VehicleTargetLease &lease);
    void shutdown();

signals:
    /** Any getter-visible state changed. Safe for a UI to refresh atomically. */
    void changed();

private:
    enum class CommandPurpose {
        None,
        Start,
        Accept,
        Cancel,
        FixedYaw
    };

    bool targetIsCurrent(const VehicleTargetLease &lease) const;
    bool mayBeginForLease(const VehicleTargetLease &lease) const;
    RequestResult beginCommand(CommandPurpose purpose, State pendingState,
                               MAV_CMD command,
                               float p1, float p2, float p3, float p4,
                               float p5, float p6, float p7);
    void handleCommandAck(qulonglong generation, int linkId,
                          int systemId, int componentId,
                          int command, int result);
    void handleProgress(const mavlink_message_t &message);
    void handleReport(const mavlink_message_t &message);
    bool acceptTelemetryEnvelope(int linkId,
                                 const mavlink_message_t &message) const;
    bool acceptCalibrationMask(quint8 compassId, quint8 mask);
    void evaluateReports();
    void handleCommandTimeout();
    void handleActivityTimeout();
    void handleTotalTimeout();
    void handleTargetGenerationChanged(qulonglong generation);
    void transitionToFailed(const QString &reason,
                            bool preserveOnboardRecovery = false);
    void transitionToUncertain(const QString &reason);
    void finishCancel();
    void resetSession();
    void stopTimers();
    void appendResult(const QString &line);
    static QString calibrationStatusText(quint8 status);
    static QString commandFailureText(MAV_CMD command, int result);
    static bool isTerminalOperationState(State state);

    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<VehicleCommandService> m_commandService;
    VehicleTargetLease m_lease;
    State m_state = State::Idle;
    State m_stateBeforeCommand = State::Idle;
    CommandPurpose m_pendingPurpose = CommandPurpose::None;
    MAV_CMD m_pendingCommand = static_cast<MAV_CMD>(0);
    quint64 m_operationToken = 0;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    quint8 m_calibrationMask = 0;
    quint8 m_manualAcceptMask = 0;
    std::array<int, 3> m_progress{{0, 0, 0}};
    std::array<quint8, 8> m_reportStatus{{}};
    std::array<bool, 8> m_reportSeen{{}};
    std::array<bool, 8> m_reportAutosaved{{}};
    std::array<QString, 8> m_lastReportLines{{}};
    QString m_resultText;
    QTimer m_commandTimer;
    QTimer m_activityTimer;
    QTimer m_totalTimer;
    int m_commandTimeoutMs = DefaultCommandTimeoutMs;
    int m_activityTimeoutMs = DefaultActivityTimeoutMs;
    int m_totalTimeoutMs = DefaultTotalTimeoutMs;
    bool m_startTelemetryBoundary = false;
    bool m_onboardRecoveryAvailable = false;
    bool m_cancelAttempted = false;
    bool m_shuttingDown = false;
    QSet<VehicleEndpoint> m_rebootTargets;
    QSet<quint64> m_poisonedGenerations;
};

Q_DECLARE_METATYPE(CompassCalibrationService::State)
Q_DECLARE_METATYPE(CompassCalibrationService::RequestResult)

#endif // COMPASSCALIBRATIONSERVICE_H
