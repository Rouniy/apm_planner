#ifndef GUIDEDTARGETSERVICE_H
#define GUIDEDTARGETSERVICE_H

#include "VehicleEndpoint.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class VehicleCommandService;
class VehicleTargetManager;

/**
 * Application-owned, exact-link guided-position command serializer.
 *
 * MAVLink COMMAND_ACK has no transaction identifier.  Consequently this
 * service permits one DO_REPOSITION command at a time and one owner session in
 * the process.  Every session pins an immutable VehicleTargetLease; an
 * ambiguous command outcome is quarantined until terminal evidence arrives,
 * a bounded recovery interval elapses, or the physical link epoch is
 * explicitly forgotten.
 */
class GuidedTargetService final : public QObject
{
    Q_OBJECT

public:
    struct Target
    {
        double latitude = 0.0;
        double longitude = 0.0;
        double relativeAltitudeM = 0.0;
    };

    struct SessionToken
    {
        QPointer<QObject> owner;
        quint64 generation = 0;
        VehicleTargetLease target;

        bool isValid() const noexcept
        {
            return !owner.isNull() && generation != 0 && target.isValid();
        }
    };

    enum class State {
        Idle,
        Reserved,
        AwaitingAcknowledgement,
        Active,
        Draining,
        OutcomeUncertain
    };
    Q_ENUM(State)

    enum class RequestResult {
        Started,
        Sent,
        Queued,
        Stopped,
        Busy,
        InvalidOwner,
        InvalidSession,
        InvalidTarget,
        StaleTarget,
        TargetUnsettled,
        HeartbeatStale,
        InvalidPosition,
        TransportUnavailable,
        OutcomeUncertain,
        CommandRejected,
        CommandUnsupported,
        CommandCancelled,
        CommandFailed
    };
    Q_ENUM(RequestResult)

    static constexpr int DefaultHeartbeatMaximumAgeMs = 3000;
    static constexpr int DefaultCommandTimeoutMs = 2000;
    static constexpr int DefaultRecoveryQuarantineMs = 6000;
    static constexpr int MaximumSendAttempts = 3;

    explicit GuidedTargetService(VehicleTargetManager *targetManager,
                                 VehicleCommandService *commandService,
                                 QObject *parent = nullptr);
    ~GuidedTargetService() override;

    void setLocalIdentity(quint8 systemId, quint8 componentId);
    void setCommandTimeoutForTesting(int timeoutMs);
    void setRecoveryQuarantineForTesting(int timeoutMs);

    State state() const noexcept { return m_state; }
    QString statusText() const { return m_statusText; }
    SessionToken activeSession() const { return m_session; }
    bool hasActiveSession() const noexcept
    {
        return m_session.generation != 0;
    }
    bool isOutcomeUncertain(const VehicleEndpoint &endpoint) const;

    RequestResult start(QObject *owner,
                        const VehicleTargetLease &target,
                        const Target &initialTarget,
                        SessionToken *sessionOut = nullptr);
    /**
     * Claims the process-wide guided channel without sending a position.
     *
     * Follow Me uses this while waiting for the first valid NMEA fix.  The
     * reservation is still bound to the exact target generation and owner;
     * submit() sends the first target and stop() releases it.
     */
    RequestResult reserve(QObject *owner,
                          const VehicleTargetLease &target,
                          SessionToken *sessionOut = nullptr);
    RequestResult submit(const SessionToken &session,
                         const Target &target);
    RequestResult stop(const SessionToken &session);
    void forgetLink(int linkId);

    static bool isValidTarget(const Target &target) noexcept;
    static QString stateDescription(State state);
    static QString resultDescription(RequestResult result);

signals:
    void stateChanged(GuidedTargetService::State state);
    void statusChanged(const QString &status);
    void targetAccepted(GuidedTargetService::SessionToken session,
                        GuidedTargetService::Target target);
    void sessionEnded(GuidedTargetService::SessionToken session,
                      GuidedTargetService::RequestResult reason,
                      const QString &description);
    void outcomeBecameUncertain(VehicleEndpoint endpoint,
                                const QString &description);

private slots:
    void handleCommandAck(qulonglong targetGeneration,
                          int linkId, int systemId, int componentId,
                          int command, int result, int progress,
                          int resultParam2, int targetSystem,
                          int targetComponent);
    void handleTargetGenerationChanged(qulonglong generation);
    void handleCommandTimeout();
    void handleRetryDrainExpired();

private:
    bool targetIsCurrent(const VehicleTargetLease &target) const;
    bool sessionMatches(const SessionToken &session) const;
    RequestResult validateReadyTarget(const VehicleTargetLease &target);
    RequestResult sendTarget(const Target &target);
    RequestResult transmitInFlightTarget(bool retry);
    void handleOwnerDestroyed(quint64 sessionGeneration);
    void finishWithUncertainOutcome(RequestResult reason,
                                    const QString &description,
                                    bool commandPending);
    void clearExpiredUncertainEndpoints();
    void scheduleUncertainEndpointExpiry();
    void clearUncertainEndpoint(const VehicleEndpoint &endpoint,
                                quint64 targetGeneration);
    void finishSession(RequestResult reason, const QString &description,
                       State finalState = State::Idle);
    void publishStateAndStatus();
    void resetSessionStorage();
    quint64 nextSessionGeneration();

    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<VehicleCommandService> m_commandService;
    SessionToken m_session;
    State m_state = State::Idle;
    QString m_statusText;
    struct UncertainCommand
    {
        VehicleTargetLease target;
        qint64 expiresAtMs = 0;
        bool commandPending = false;
    };
    QHash<VehicleEndpoint, UncertainCommand> m_uncertainEndpoints;
    Target m_inFlightTarget;
    Target m_queuedTarget;
    bool m_inFlight = false;
    bool m_hasQueuedTarget = false;
    bool m_firstAccepted = false;
    bool m_retryDrain = false;
    int m_sendAttempt = 0;
    quint64 m_nextSessionGeneration = 0;
    quint64 m_transitionGeneration = 0;
    quint64 m_lastFinishedSessionGeneration = 0;
    RequestResult m_lastFinishedResult = RequestResult::InvalidSession;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = 190;
    int m_commandTimeoutMs = DefaultCommandTimeoutMs;
    int m_recoveryQuarantineMs = DefaultRecoveryQuarantineMs;
    QElapsedTimer m_monotonicClock;
    QTimer m_commandTimer;
    QTimer m_retryDrainTimer;
    QTimer m_quarantineTimer;
    QMetaObject::Connection m_ownerDestroyedConnection;
};

Q_DECLARE_METATYPE(GuidedTargetService::Target)
Q_DECLARE_METATYPE(GuidedTargetService::SessionToken)
Q_DECLARE_METATYPE(GuidedTargetService::State)
Q_DECLARE_METATYPE(GuidedTargetService::RequestResult)

#endif // GUIDEDTARGETSERVICE_H
