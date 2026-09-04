#ifndef MOVINGBASESERVICE_H
#define MOVINGBASESERVICE_H

#include "MovingBasePositionStore.h"
#include "NmeaGgaParser.h"
#include "VehicleEndpoint.h"

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>

class VehicleTargetManager;

/**
 * Application-owned, transport-independent Moving Base session controller.
 *
 * The service admits one owner at a time and binds that owner to one immutable
 * VehicleTargetLease. Parsed GGA fixes enter through submitFix(); serial,
 * TCP/UDP and UI policy deliberately remain outside this class. The first fix
 * is published immediately and later input is coalesced at the selected rate.
 */
class MovingBaseService final : public QObject
{
    Q_OBJECT

public:
    using Clock = std::function<qint64()>;

    /**
     * Optional deterministic timing hooks. Production leaves them empty and
     * uses the owned QTimer. Tests inject a clock and observe/disarm wakeups,
     * then call processTimersForTesting() at the chosen monotonic instant.
     */
    struct TimingSeams
    {
        Clock monotonicMs;
        std::function<void(int delayMs)> armOneShot;
        std::function<void()> disarm;
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
        Active
    };
    Q_ENUM(State)

    enum class RequestResult {
        Started,
        Published,
        Queued,
        Cleared,
        Stopped,
        Busy,
        InvalidOwner,
        InvalidSession,
        InvalidTarget,
        StaleTarget,
        TargetUnsettled,
        InvalidRate,
        InvalidFix,
        StorageUnavailable
    };
    Q_ENUM(RequestResult)

    static constexpr double DefaultRateHz = 0.5;
    static constexpr int MinimumStaleTimeoutMs = 5000;

    explicit MovingBaseService(VehicleTargetManager *targetManager,
                               MovingBasePositionStore *positionStore,
                               QObject *parent = nullptr);
    MovingBaseService(VehicleTargetManager *targetManager,
                      MovingBasePositionStore *positionStore,
                      TimingSeams timingSeams,
                      QObject *parent = nullptr);
    ~MovingBaseService() override;

    State state() const noexcept { return m_state; }
    QString statusText() const { return m_statusText; }
    bool hasActiveSession() const noexcept
    {
        return m_session.generation != 0;
    }
    SessionToken activeSession() const { return m_session; }
    double rateHz() const noexcept { return m_rateHz; }
    int staleTimeoutMs() const noexcept;

    static QList<double> supportedRates();
    static bool isSupportedRate(double rateHz) noexcept;
    static QString stateDescription(State state);
    static QString resultDescription(RequestResult result);

    RequestResult start(QObject *owner,
                        const VehicleTargetLease &target,
                        double rateHz,
                        SessionToken *sessionOut = nullptr);

public slots:
    RequestResult submitFix(const MovingBaseService::SessionToken &session,
                            const NmeaGgaFix &fix);
    RequestResult reportNoFix(
        const MovingBaseService::SessionToken &session,
        const QString &description = QString());
    RequestResult stop(const MovingBaseService::SessionToken &session);
    void forgetLink(int linkId);

    /** Runs the same due-work path as the production QTimer. */
    void processTimersForTesting();

signals:
    void stateChanged(MovingBaseService::State state);
    void statusChanged(const QString &status);
    void fixPublished(MovingBaseService::SessionToken session,
                      MovingBasePositionSnapshot snapshot);
    void positionCleared(MovingBaseService::SessionToken session,
                         const QString &reason);
    void sessionEnded(MovingBaseService::SessionToken session,
                      MovingBaseService::RequestResult reason,
                      const QString &description);

private:
    bool targetIsCurrent(const VehicleTargetLease &target) const;
    bool sessionMatches(const SessionToken &session) const;
    RequestResult validateStartTarget(
        const VehicleTargetLease &target) const;
    static bool isValidFix(const NmeaGgaFix &fix) noexcept;
    MovingBasePositionFix positionFix(const NmeaGgaFix &fix,
                                      qint64 observedMs) const;
    RequestResult publishPendingFix();
    RequestResult clearPosition(const QString &reason);
    void handleTargetGenerationChanged(qulonglong generation);
    void handleOwnerDestroyed(quint64 sessionGeneration);
    void finishSession(RequestResult reason, const QString &description);
    void resetFixState();
    void scheduleNextWakeup();
    void armWakeup(int delayMs);
    void disarmWakeup();
    qint64 nowMs() const;
    int rateIntervalMs() const noexcept;
    void publishStateAndStatus(quint64 transition);
    quint64 nextSessionGeneration();

    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<MovingBasePositionStore> m_positionStore;
    TimingSeams m_timingSeams;
    QTimer m_wakeupTimer;
    QElapsedTimer m_monotonicClock;
    SessionToken m_session;
    State m_state = State::Idle;
    QString m_statusText;
    NmeaGgaFix m_pendingFix;
    bool m_hasPendingFix = false;
    qint64 m_pendingObservedMs = -1;
    qint64 m_lastValidFixMs = -1;
    qint64 m_lastPublishedMs = -1;
    double m_rateHz = DefaultRateHz;
    quint64 m_nextSessionGeneration = 0;
    quint64 m_transitionGeneration = 0;
    quint64 m_lastFinishedSessionGeneration = 0;
    RequestResult m_lastFinishedResult = RequestResult::InvalidSession;
    bool m_finishing = false;
    QMetaObject::Connection m_ownerDestroyedConnection;
};

Q_DECLARE_METATYPE(MovingBaseService::SessionToken)
Q_DECLARE_METATYPE(MovingBaseService::State)
Q_DECLARE_METATYPE(MovingBaseService::RequestResult)

#endif // MOVINGBASESERVICE_H
