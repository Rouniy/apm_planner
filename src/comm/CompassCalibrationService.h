#ifndef COMPASSCALIBRATIONSERVICE_H
#define COMPASSCALIBRATIONSERVICE_H

#include "VehicleEndpoint.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <array>

#include <mavlink.h>

class VehicleCommandService;
class VehicleTargetManager;
class ExactLinkTransmitter;

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
    struct MotorSample {
        double throttlePercent = 0.0;
        double currentAmps = 0.0;
        int interferencePercent = 0;
        double compensationX = 0.0;
        double compensationY = 0.0;
        double compensationZ = 0.0;
    };

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
        MotorStartPending,
        MotorRunning,
        MotorStopPending,
        MotorStopSettling,
        MotorSucceeded,
        MotorFailed,
        MotorCompletedUnverified,
        MotorOutcomeUncertain,
        Failed,
        OutcomeUncertain
    };
    Q_ENUM(State)

    enum class RequestResult {
        Started,
        Busy,
        InvalidTarget,
        Armed,
        UnsupportedVehicle,
        HeartbeatStale,
        UnsafeTransport,
        SharedLinkUnsafe,
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
    static constexpr int DefaultMotorActivityTimeoutMs = 3000;
    static constexpr int DefaultMotorHeartbeatTimeoutMs = 3000;
    static constexpr int DefaultMotorTotalTimeoutMs = 10 * 60 * 1000;
    static constexpr int DefaultMotorSettleTimeoutMs = 1500;
    static constexpr int MaximumMotorSamples = 600;

    CompassCalibrationService(VehicleTargetManager *targetManager,
                              VehicleCommandService *commandService,
                              ExactLinkTransmitter *transmitter,
                              QObject *parent = nullptr);
    ~CompassCalibrationService() override;

    void setLocalIdentity(quint8 systemId, quint8 componentId);
    void setTimeoutsForTesting(int commandTimeoutMs,
                               int activityTimeoutMs,
                               int totalTimeoutMs);
    void setMotorTimeoutsForTesting(int activityTimeoutMs,
                                    int totalTimeoutMs,
                                    int settleTimeoutMs);
    void setMotorHeartbeatTimeoutForTesting(int timeoutMs);

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
    bool blocksDeveloperTools(const VehicleTargetLease &target) const;
    bool canCancel() const;
    bool isBusy() const;
    bool isMotorActive() const;
    bool canStopMotor() const;
    bool canAcknowledgeMotorPowerDisconnected() const;
    bool motorMayBeActive() const { return m_motorMayBeActive; }
    bool motorHasSample() const { return !m_motorSamples.isEmpty(); }
    MotorSample latestMotorSample() const;
    QVector<MotorSample> motorSamples() const { return m_motorSamples; }
    QString motorLog() const { return m_motorLog.join(QLatin1Char('\n')); }
    bool supportsMotorCalibration(const VehicleTargetLease &lease) const;
    bool blocksLegacyCalibrationMessage(
        int linkId, const mavlink_message_t &message) const;

    RequestResult start(const VehicleTargetLease &lease, bool armed);
    RequestResult accept(bool armed);
    RequestResult cancel();
    RequestResult fixedYaw(const VehicleTargetLease &lease,
                           double headingDegrees, bool armed);
    RequestResult startMotor(const VehicleTargetLease &lease,
                             bool armed, bool dedicatedLinkConfirmed);
    RequestResult stopMotor();
    RequestResult acknowledgeMotorPowerDisconnected();

    void observeMessage(int linkId, const mavlink_message_t &message);
    void forgetLink(int linkId);
    void forgetLink();
    void clearRebootRequired(const VehicleTargetLease &lease);
    void shutdown();

signals:
    /** Any getter-visible state changed. Safe for a UI to refresh atomically. */
    void changed();
    /** Application-level warning: a CompassMot stop could not be proven. */
    void motorSafetyWarning(const QString &message);

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
    void handleEndpointRegistryChanged();
    void handleMotorMessage(int linkId, const mavlink_message_t &message);
    void handleMotorAck(const mavlink_message_t &message);
    void handleMotorStatus(const mavlink_message_t &message);
    void handleMotorStatusText(const mavlink_message_t &message);
    RequestResult beginMotorStop(const QString &reason);
    bool sendMotorStopFrame();
    void attemptTeardownMotorStop();
    void scheduleSecondMotorStopFrame(quint64 token);
    void handleMotorActivityTimeout();
    void handleMotorTotalTimeout();
    void handleMotorSettleTimeout();
    void finishMotorFromEvidence(bool allowMissingFinalAck = false);
    void finishMotorUnverified();
    void transitionMotorToUncertain(const QString &reason);
    void appendMotorLog(const QString &line);
    bool acceptMotorEnvelope(int linkId,
                             const mavlink_message_t &message) const;
    bool linkHasSingleAutopilotTarget(
        const VehicleTargetLease &lease) const;
    bool hasSupportedMotorHeartbeat(
        const VehicleTargetLease &lease) const;
    static bool isMotorState(State state);
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
    QPointer<ExactLinkTransmitter> m_transmitter;
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
    QTimer m_motorSettleTimer;
    int m_commandTimeoutMs = DefaultCommandTimeoutMs;
    int m_activityTimeoutMs = DefaultActivityTimeoutMs;
    int m_totalTimeoutMs = DefaultTotalTimeoutMs;
    int m_motorActivityTimeoutMs = DefaultMotorActivityTimeoutMs;
    int m_motorHeartbeatTimeoutMs = DefaultMotorHeartbeatTimeoutMs;
    int m_motorTotalTimeoutMs = DefaultMotorTotalTimeoutMs;
    int m_motorSettleTimeoutMs = DefaultMotorSettleTimeoutMs;
    bool m_startTelemetryBoundary = false;
    bool m_onboardRecoveryAvailable = false;
    bool m_cancelAttempted = false;
    bool m_shuttingDown = false;
    QVector<MotorSample> m_motorSamples;
    QStringList m_motorLog;
    bool m_motorMayBeActive = false;
    bool m_motorStartAckSeen = false;
    bool m_motorStartWireAckSeen = false;
    bool m_motorStatusSeen = false;
    bool m_motorStopAttempted = false;
    bool m_motorSecondStopPending = false;
    bool m_motorAnyStopFrameSent = false;
    bool m_motorRejectedStopAckSeen = false;
    bool m_motorFinalAckSeen = false;
    int m_motorTerminalEvidence = -1;
    QString m_motorTerminalLine;
    QSet<VehicleEndpoint> m_rebootTargets;
    QSet<VehicleEndpoint> m_poisonedEndpoints;
    QSet<VehicleEndpoint> m_motorEpochPoisonedEndpoints;
};

Q_DECLARE_METATYPE(CompassCalibrationService::State)
Q_DECLARE_METATYPE(CompassCalibrationService::RequestResult)

#endif // COMPASSCALIBRATIONSERVICE_H
