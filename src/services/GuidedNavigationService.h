#ifndef GUIDEDNAVIGATIONSERVICE_H
#define GUIDEDNAVIGATIONSERVICE_H

#include "comm/GuidedTargetService.h"
#include "comm/VehicleCommandService.h"
#include "services/GuidedAltitudeStore.h"

#include <QObject>
#include <QList>
#include <QPointer>
#include <QSharedDataPointer>
#include <QString>
#include <QTimer>

/**
 * Application-owned coordinator for one consented, no-session
 * MAV_CMD_DO_REPOSITION transaction. It reserves the legacy guided lane but
 * delegates all wire, retry, COMMAND_ACK and quarantine ownership to
 * VehicleCommandService.
 */
class GuidedNavigationService final : public QObject
{
    Q_OBJECT

public:
    enum class Purpose {
        FlyToHere,
        TerrainClick,
        AltitudeUpdate,
        Coordinates
    };
    Q_ENUM(Purpose)

    class Plan
    {
    public:
        Plan();
        Plan(const Plan &);
        Plan &operator=(const Plan &);
        ~Plan();

        bool isValid() const noexcept;
        quint64 planId() const noexcept;
        Purpose purpose() const noexcept;
        GuidedAltitudeStore::Context context() const;
        VehicleTargetLease target() const;
        SwarmVehicleInstanceLease vehicle() const;
        double latitude() const noexcept;
        double longitude() const noexcept;
        double altitudeM() const noexcept;
        MAV_FRAME frame() const noexcept;
        bool changeMode() const noexcept;
        QString description() const;

    private:
        struct Data;
        QSharedDataPointer<Data> d;
        friend class GuidedNavigationService;
    };

    enum class SubmitResult {
        Started,
        InvalidOwner,
        InvalidPlan,
        Busy,
        Unavailable
    };
    Q_ENUM(SubmitResult)

    enum class Outcome {
        Accepted,
        Rejected,
        Cancelled,
        OutcomeUncertain
    };
    Q_ENUM(Outcome)

    struct Report
    {
        quint64 operationId = 0;
        Plan plan;
        Outcome outcome = Outcome::Rejected;
        int mavResult = -1;
        int transmissionAttempts = 0;
        bool frameAttempted = false;
        bool cancellationRequested = false;
        bool targetRecorded = false;
        QString description;

        bool isValid() const noexcept
        {
            return operationId != 0 && plan.isValid();
        }
    };

    static constexpr int DefaultAcknowledgementTimeoutMs = 2000;
    static constexpr int DefaultMaximumLifetimeMs = 30000;
    // MP10 sends the original COMMAND_INT plus three retries.
    static constexpr int DefaultMaximumRetries = 3;
    static constexpr int MaximumPreparedPlans = 16;

    explicit GuidedNavigationService(
        GuidedAltitudeStore *altitudeStore,
        GuidedTargetService *guidedTargetService,
        VehicleCommandService *commandService,
        QObject *parent = nullptr);
    ~GuidedNavigationService() override;

    bool busy() const noexcept;
    quint64 currentOperationId() const noexcept;
    QString status() const;
    Plan activePlan() const;
    Report lastReport() const;

    bool prepare(const GuidedAltitudeStore::Context &context,
                 double latitude, double longitude, double altitudeM,
                 MAV_FRAME frame, bool changeMode, Purpose purpose,
                 Plan *planOut, QString *error = nullptr);
    bool discardPlan(const Plan &plan);
    bool validate(const Plan &plan, QString *error = nullptr) const;
    SubmitResult execute(QObject *uiOwner, const Plan &plan,
                         quint64 *operationIdOut,
                         QString *error = nullptr);
    bool cancel(quint64 operationId);
    void shutdown();
    void setTimeoutsForTesting(int acknowledgementMs,
                               int maximumLifetimeMs,
                               int maximumRetries = DefaultMaximumRetries);

signals:
    void stateChanged();
    void operationFinished(GuidedNavigationService::Report report);

private:
    bool planMatchesPrepared(const Plan &plan) const noexcept;
    bool operationIsCurrent(quint64 operationId,
                            const Plan &plan) const noexcept;
    bool validatePlan(const Plan &plan, bool requirePrepared,
                      QString *error) const;
    bool finalWriteGate(quint64 operationId, const Plan &plan,
                        QString *error);
    void handleCommandFinished(
        const VehicleCommandService::ExactCommandReport &report);
    void processCommandReport(
        const VehicleCommandService::ExactCommandReport &report);
    void pollQuarantine();
    void finish(Outcome outcome, const QString &description);
    void releaseLanes();
    void handleUiOwnerDestroyed(quint64 operationId);
    quint64 nextPlanId();
    quint64 nextOperationId();

    QPointer<GuidedAltitudeStore> m_altitudeStore;
    QPointer<GuidedTargetService> m_guidedTargetService;
    QPointer<VehicleCommandService> m_commandService;
    QList<Plan> m_preparedPlans;
    Plan m_activePlan;
    Report m_report;
    Report m_lastReport;
    VehicleCommandService::ExactReservationToken m_commandReservation;
    VehicleCommandService::ExactCommandToken m_commandToken;
    GuidedTargetService::SessionToken m_guidedSession;
    QPointer<QObject> m_uiOwner;
    QMetaObject::Connection m_uiOwnerDestroyedConnection;
    QList<VehicleCommandService::ExactCommandReport> m_deferredReports;
    QTimer m_quarantinePoll;
    QString m_status;
    quint64 m_nextPlanId = 0;
    quint64 m_nextOperationId = 0;
    bool m_busy = false;
    bool m_finishing = false;
    bool m_apiInFlight = false;
    bool m_submitting = false;
    bool m_processingCommandReport = false;
    bool m_cancelRequested = false;
    bool m_shuttingDown = false;
    bool m_waitingForQuarantine = false;
    Outcome m_pendingOutcome = Outcome::Rejected;
    QString m_pendingDescription;
    int m_acknowledgementTimeoutMs = DefaultAcknowledgementTimeoutMs;
    int m_maximumLifetimeMs = DefaultMaximumLifetimeMs;
    int m_maximumRetries = DefaultMaximumRetries;
};

Q_DECLARE_METATYPE(GuidedNavigationService::Purpose)
Q_DECLARE_METATYPE(GuidedNavigationService::Plan)
Q_DECLARE_METATYPE(GuidedNavigationService::SubmitResult)
Q_DECLARE_METATYPE(GuidedNavigationService::Outcome)
Q_DECLARE_METATYPE(GuidedNavigationService::Report)

#endif // GUIDEDNAVIGATIONSERVICE_H
