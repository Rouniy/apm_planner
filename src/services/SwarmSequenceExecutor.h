#ifndef SWARMSEQUENCEEXECUTOR_H
#define SWARMSEQUENCEEXECUTOR_H

#include "comm/SwarmCommandService.h"
#include "comm/VehicleCommandService.h"
#include "ui/tools/SwarmSequenceCore.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>

#include <functional>

class SwarmTelemetryRegistry;

struct SwarmSequenceOrigin
{
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
};

struct SwarmSequenceExactAssignment
{
    int systemId = 0;
    SwarmVehicleInstanceLease lease;
    SwarmSequenceOffset offset;
};

struct SwarmSequenceTakeoffAssignment
{
    int systemId = 0;
    SwarmVehicleInstanceLease lease;
};

struct SwarmSequenceRunStepRequest
{
    QString layoutId;
    SwarmVehicleInstanceLease anchor;
    SwarmSequenceOrigin origin;
    QVector<SwarmSequenceExactAssignment> assignments;
};

struct SwarmSequenceTarget
{
    int systemId = 0;
    SwarmVehicleInstanceLease lease;
    double latitude = 0.0;
    double longitude = 0.0;
    double relativeAltitudeM = 0.0;
};

struct SwarmSequencePreparedRunStep
{
    quint64 preparationId = 0;
    QString layoutId;
    SwarmVehicleInstanceLease anchor;
    SwarmSequenceOrigin origin;
    QVector<SwarmSequenceExactAssignment> assignments;
    QVector<SwarmSequenceTarget> targets;

    bool isValid() const noexcept
    {
        return preparationId != 0 && !layoutId.trimmed().isEmpty()
            && anchor.isValid() && origin.valid && !assignments.isEmpty()
            && assignments.size() == targets.size();
    }
};

struct SwarmSequencePreparedTakeoff
{
    quint64 preparationId = 0;
    QVector<SwarmSequenceTakeoffAssignment> assignments;
    double altitudeM = 2.0;

    bool isValid() const noexcept
    {
        return preparationId != 0 && !assignments.isEmpty();
    }
};

enum class SwarmSequenceOperation
{
    None,
    RunStep,
    Takeoff
};

enum class SwarmSequenceOperationResult
{
    None,
    SentAll,
    Rejected,
    Partial,
    Cancelled,
    OutcomeUncertain
};

struct SwarmSequenceVehicleResult
{
    int systemId = 0;
    SwarmVehicleInstanceLease lease;
    bool targetSent = false;
    bool guidedAccepted = false;
    bool armAccepted = false;
    bool takeoffAccepted = false;
    QString detail;
};

struct SwarmSequenceOperationReport
{
    quint64 operationGeneration = 0;
    SwarmSequenceOperation operation = SwarmSequenceOperation::None;
    SwarmSequenceOperationResult result = SwarmSequenceOperationResult::None;
    SwarmSequenceOrigin origin;
    QVector<SwarmSequenceTarget> targets;
    QVector<SwarmSequenceVehicleResult> vehicles;
    int framesPlanned = 0;
    int framesAttempted = 0;
    int framesSent = 0;
    QString description;
};

/** Fakeable GUI-thread boundary around the three application exact services. */
class SwarmSequenceExecutorBackend : public QObject
{
public:
    struct Callbacks
    {
        std::function<void(VehicleCommandService::ExactCommandReport)>
            commandFinished;
        std::function<void(quint64)> commandReservationReleased;
        std::function<void(quint64, QString)> swarmSessionCancelled;
    };

    explicit SwarmSequenceExecutorBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~SwarmSequenceExecutorBackend() override = default;

    virtual void setCallbacks(Callbacks callbacks) = 0;
    virtual bool ready(QString *error) const = 0;
    virtual qint64 observationClockNowMs() const = 0;
    virtual bool observationIsFresh(qint64 observedMs,
                                    int maximumAgeMs) const = 0;
    virtual bool snapshotForLease(const SwarmVehicleInstanceLease &lease,
                                  SwarmTelemetrySnapshot *snapshot) const = 0;
    virtual bool routeIsEligible(const SwarmVehicleInstanceLease &lease,
                                 QString *error) const = 0;

    virtual SwarmCommandService::Result reserveSwarm(
        QObject *owner, const QVector<SwarmCommandMember> &members,
        int maximumBatchHz, SwarmCommandSessionToken *token,
        QString *error) = 0;
    virtual SwarmCommandService::Result releaseSwarm(
        const SwarmCommandSessionToken &token) = 0;
    virtual SwarmCommandService::BatchReport requestPositionStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds, int rateHz) = 0;
    virtual SwarmCommandService::BatchReport sendPositionTargets(
        const SwarmCommandSessionToken &token,
        const QVector<SwarmPositionTarget> &targets) = 0;

    virtual VehicleCommandService::ExactReservationResult reserveCommands(
        QObject *owner, const QList<SwarmVehicleInstanceLease> &leases,
        VehicleCommandService::ExactReservationToken *token,
        QString *error) = 0;
    virtual bool releaseCommands(
        const VehicleCommandService::ExactReservationToken &token) = 0;
    virtual VehicleCommandService::ExactSubmitResult submitCommand(
        const VehicleCommandService::ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const VehicleCommandService::ExactCommandRequest &request,
        VehicleCommandService::ExactCommandToken *token,
        QString *error) = 0;
};

/**
 * Application-owned exact executor for Mission Planner's Sequence Run Step and
 * Takeoff actions. It owns reservations, ACK correlation and terminal draining;
 * closing the modeless window cannot orphan an exact command waiter.
 */
class SwarmSequenceExecutor final : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Idle,
        RunningStep,
        TakingOff,
        WaitingForHeartbeat,
        Draining,
        PartialEffect,
        OutcomeUncertain
    };
    Q_ENUM(State)

    static constexpr int MaximumHeartbeatAgeMs = 5000;
    static constexpr int MaximumPositionAgeMs = 1500;
    static constexpr int HeartbeatBarrierTimeoutMs = 5000;
    static constexpr int StreamRateHz = 10;
    static constexpr double TakeoffAltitudeM = 2.0;

    SwarmSequenceExecutor(SwarmTelemetryRegistry *registry,
                          SwarmCommandService *swarmCommands,
                          VehicleCommandService *commands,
                          QObject *parent = nullptr);
    explicit SwarmSequenceExecutor(SwarmSequenceExecutorBackend *backend,
                                   QObject *parent = nullptr);
    ~SwarmSequenceExecutor() override;

    bool executorReady(QString *error = nullptr) const;

    /** Read-only pre-confirmation capture; an absent origin is captured exactly. */
    bool prepareRunStep(const SwarmSequenceRunStepRequest &request,
                        SwarmSequencePreparedRunStep *prepared,
                        QString *error = nullptr) const;
    /** Authoritative post-confirmation start; reservations and validation repeat. */
    bool runStep(const SwarmSequencePreparedRunStep &prepared,
                 QString *error = nullptr);

    bool prepareTakeoff(
        const QVector<SwarmSequenceTakeoffAssignment> &assignments,
        SwarmSequencePreparedTakeoff *prepared,
        QString *error = nullptr) const;
    bool startTakeoff(const SwarmSequencePreparedTakeoff &prepared,
                      QString *error = nullptr);

    /** Synchronous no-future-send barrier; pending exact ACKs drain in service. */
    void cancelActiveOperation(const QString &reason = QString());

    State state() const noexcept { return m_state; }
    SwarmSequenceOperation operation() const noexcept { return m_operation; }
    bool isActive() const noexcept;
    QString statusText() const { return m_status; }
    quint64 operationGeneration() const noexcept { return m_generation; }
    SwarmSequenceOperationReport lastReport() const { return m_lastReport; }

signals:
    void changed();
    void stateChanged(SwarmSequenceExecutor::State state);
    void statusChanged(QString status);
    void operationFinished(SwarmSequenceOperationReport report);

private slots:
    void heartbeatPoll();

private:
    enum class TakeoffPhase
    {
        Guided,
        Arm,
        Takeoff
    };

    enum class HeartbeatExpectation
    {
        None,
        Guided,
        Armed
    };

    struct PendingCommand
    {
        quint64 generation = 0;
        VehicleCommandService::ExactCommandToken token;
        TakeoffPhase phase = TakeoffPhase::Guided;
        int vehicleIndex = -1;
    };

    static quint64 nextPreparationId();
    static quint64 nextOperationGeneration();
    static bool sameCommandToken(
        const VehicleCommandService::ExactCommandToken &left,
        const VehicleCommandService::ExactCommandToken &right);
    static bool isCopter(const SwarmTelemetrySnapshot &snapshot);
    static bool batchOutcomeUncertain(
        const SwarmCommandService::BatchReport &report);

    bool validateVehicle(const SwarmVehicleInstanceLease &lease,
                         bool requirePosition, bool requireGuided,
                         SwarmTelemetrySnapshot *snapshot,
                         QString *error) const;
    bool canonicalRunStep(const SwarmSequenceRunStepRequest &request,
                          bool captureOrigin,
                          SwarmSequencePreparedRunStep *prepared,
                          QString *error) const;
    bool canonicalPreparedRunStep(
        const SwarmSequencePreparedRunStep &input,
        SwarmSequencePreparedRunStep *prepared,
        QString *error) const;
    bool canonicalTakeoff(
        const QVector<SwarmSequenceTakeoffAssignment> &assignments,
        SwarmSequencePreparedTakeoff *prepared,
        QString *error) const;
    QVector<SwarmCommandMember> runMembers(
        const SwarmSequencePreparedRunStep &prepared) const;
    QVector<SwarmCommandMember> takeoffMembers(
        const SwarmSequencePreparedTakeoff &prepared) const;
    QList<SwarmVehicleInstanceLease> runLeases(
        const SwarmSequencePreparedRunStep &prepared) const;
    QList<SwarmVehicleInstanceLease> takeoffLeases(
        const SwarmSequencePreparedTakeoff &prepared) const;

    bool beginReservations(const QVector<SwarmCommandMember> &members,
                           const QList<SwarmVehicleInstanceLease> &leases,
                           QString *error);
    void dispatchRunStep();
    void beginTakeoffVehicle();
    void submitTakeoffCommand(TakeoffPhase phase);
    void beginHeartbeatBarrier(HeartbeatExpectation expectation,
                               qint64 baselineHeartbeatMs);
    void rejectCurrentTakeoffVehicle(const QString &detail);

    void commandFinished(
        const VehicleCommandService::ExactCommandReport &report);
    void processCommandFinished(
        const VehicleCommandService::ExactCommandReport &report);
    void commandReservationReleased(quint64 reservationId);
    void swarmSessionCancelled(quint64 sessionId, const QString &reason);
    void drainDeferredCallbacks();

    void accumulateBatch(const SwarmCommandService::BatchReport &report);
    SwarmSequenceOperationResult failedBatchResult(
        const SwarmCommandService::BatchReport &report) const;
    void complete(SwarmSequenceOperationResult result,
                  const QString &description);
    void beginDrain(SwarmSequenceOperationResult result,
                    const QString &description);
    void releaseReservations();
    void finishDrain();
    void setState(State state);
    void setStatus(const QString &status);

    QPointer<SwarmSequenceExecutorBackend> m_backend;
    QTimer m_heartbeatTimer;
    State m_state = State::Idle;
    SwarmSequenceOperation m_operation = SwarmSequenceOperation::None;
    QString m_status = QStringLiteral("Sequence executor is idle.");
    quint64 m_generation = 0;
    bool m_dispatchAllowed = false;
    bool m_swarmReserved = false;
    bool m_commandsReserved = false;
    bool m_commandReleaseRequested = false;
    bool m_destroying = false;
    bool m_completionPending = false;
    bool m_finishing = false;

    SwarmCommandSessionToken m_swarmToken;
    VehicleCommandService::ExactReservationToken m_commandReservation;
    PendingCommand m_pendingCommand;
    SwarmSequencePreparedRunStep m_runPlan;
    SwarmSequencePreparedTakeoff m_takeoffPlan;
    int m_takeoffVehicleIndex = 0;
    TakeoffPhase m_takeoffPhase = TakeoffPhase::Guided;
    HeartbeatExpectation m_heartbeatExpectation =
        HeartbeatExpectation::None;
    qint64 m_heartbeatBaselineMs = -1;
    qint64 m_heartbeatDeadlineMs = -1;
    SwarmSequenceOperationReport m_workingReport;
    SwarmSequenceOperationReport m_lastReport;

    int m_backendDepth = 0;
    QVector<VehicleCommandService::ExactCommandReport> m_deferredCommands;
    QVector<quint64> m_deferredCommandReleases;
    QVector<QPair<quint64, QString>> m_deferredSwarmCancellations;
};

Q_DECLARE_METATYPE(SwarmSequenceOperation)
Q_DECLARE_METATYPE(SwarmSequenceOperationResult)
Q_DECLARE_METATYPE(SwarmSequenceOperationReport)
Q_DECLARE_METATYPE(SwarmSequenceExecutor::State)

#endif // SWARMSEQUENCEEXECUTOR_H
