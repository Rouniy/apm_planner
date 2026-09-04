#ifndef SWARMWAYPOINTLEADEREXECUTOR_H
#define SWARMWAYPOINTLEADEREXECUTOR_H

#include "comm/ExactMissionSnapshotService.h"
#include "comm/ParameterService.h"
#include "comm/SwarmCommandService.h"
#include "comm/VehicleCommandService.h"
#include "ui/tools/SwarmWaypointLeaderCore.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>

#include <functional>

class SwarmTelemetryRegistry;

/** Fakeable, GUI-thread boundary around the five application exact services. */
class SwarmWaypointLeaderExecutorBackend : public QObject
{
public:
    struct Snapshot
    {
        QList<SwarmWaypointLeaderVehicleState> vehicles;
        SwarmWaypointLeaderMissionSnapshot mission;
        qint64 observationClockNowMs = -1;
    };

    struct Callbacks
    {
        std::function<void(ParameterService::ExactOperationReport)>
            parameterFinished;
        std::function<void(VehicleCommandService::ExactCommandReport)>
            commandFinished;
        std::function<void(quint64)> parameterReservationReleased;
        std::function<void(quint64)> commandReservationReleased;
        std::function<void(quint64, QString)> swarmSessionCancelled;
    };

    explicit SwarmWaypointLeaderExecutorBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~SwarmWaypointLeaderExecutorBackend() override = default;

    virtual void setCallbacks(Callbacks callbacks) = 0;
    virtual bool ready(QString *error) const = 0;
    virtual bool capture(const SwarmWaypointLeaderPlan &plan,
                         Snapshot *snapshot, QString *error) const = 0;

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
        const QVector<SwarmPositionTarget> &targets,
        SwarmCommandService::PositionTargetPriority priority) = 0;

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

    virtual ParameterService::ExactReservationResult reserveParameters(
        QObject *owner, const QList<SwarmVehicleInstanceLease> &leases,
        ParameterService::ExactReservationToken *token,
        QString *error) = 0;
    virtual bool cancelParameterOperation(
        const ParameterService::ExactReservationToken &reservation,
        const ParameterService::ExactOperationToken &operation,
        const QString &reason) = 0;
    virtual bool releaseParameters(
        const ParameterService::ExactReservationToken &token) = 0;
    virtual ParameterService::ExactSubmitResult submitParameterRead(
        const ParameterService::ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const ParameterService::ExactReadRequest &request,
        ParameterService::ExactOperationToken *token,
        QString *error) = 0;
    virtual ParameterService::ExactSubmitResult submitParameterWrite(
        const ParameterService::ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const ParameterService::ExactWriteRequest &request,
        ParameterService::ExactOperationToken *token,
        QString *error) = 0;
};

/**
 * Application-owned exact executor for Mission Planner's Waypoint Leader.
 *
 * start() reserves SwarmCommand -> VehicleCommand -> Parameter atomically,
 * revalidates the exact plan, then discovers every runtime parameter type by
 * exact read. cancelActiveRun() disables dispatch and synchronously cancels a
 * pending exact parameter operation before it returns; ACK waiters may remain
 * in Draining but cannot emit another command.
 */
class SwarmWaypointLeaderExecutor final : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Idle,
        DiscoveringParameters,
        Running,
        WaitingForCompletion,
        Draining,
        OutcomeUncertain
    };
    Q_ENUM(State)

    static constexpr int ControlIntervalMs =
        1000 / SwarmWaypointLeaderCore::ControlRateHz;

    SwarmWaypointLeaderExecutor(
        SwarmTelemetryRegistry *registry,
        ExactMissionSnapshotService *missions,
        SwarmCommandService *swarmCommands,
        VehicleCommandService *commands,
        ParameterService *parameters,
        QObject *parent = nullptr);
    explicit SwarmWaypointLeaderExecutor(
        SwarmWaypointLeaderExecutorBackend *backend,
        QObject *parent = nullptr);
    ~SwarmWaypointLeaderExecutor() override;

    bool executorReady(QString *error = nullptr) const;
    bool validatePlan(const SwarmWaypointLeaderPlan &plan,
                      QString *error = nullptr) const;
    bool start(const SwarmWaypointLeaderPlan &plan,
               QString *error = nullptr);
    void cancelActiveRun(const QString &reason = QString());
    bool requestMode(SwarmWaypointLeaderMode mode,
                     QString *error = nullptr);

    bool isRunning() const noexcept;
    State state() const noexcept { return m_state; }
    SwarmWaypointLeaderMode mode() const noexcept { return m_core.mode(); }
    QString statusText() const { return m_status; }
    quint64 runGeneration() const noexcept { return m_runGeneration; }

signals:
    void changed();
    void runningChanged(bool running);
    void stateChanged(SwarmWaypointLeaderExecutor::State state);
    void statusChanged(QString status);

private slots:
    void controlTick();

private:
    struct Capability
    {
        SwarmVehicleInstanceLease lease;
        QHash<QString, ParameterType> types;
    };

    struct AliasProbe
    {
        SwarmVehicleInstanceLease lease;
        QVector<QString> aliases;
        int nextAlias = 0;
    };

    struct Batch
    {
        quint64 runGeneration = 0;
        quint64 coreBatchId = 0;
        QVector<SwarmWaypointLeaderCommandIntent> intents;
        int nextIntent = 0;
        bool cancellationRequested = false;
        bool anyFrameAttempted = false;
    };

    struct PendingParameter
    {
        quint64 runGeneration = 0;
        ParameterService::ExactOperationToken token;
        bool discovery = false;
    };

    struct PendingCommand
    {
        quint64 runGeneration = 0;
        VehicleCommandService::ExactCommandToken token;
    };

    static quint64 nextRunGeneration();
    static bool sameToken(
        const ParameterService::ExactOperationToken &left,
        const ParameterService::ExactOperationToken &right);
    static bool sameToken(
        const VehicleCommandService::ExactCommandToken &left,
        const VehicleCommandService::ExactCommandToken &right);
    static bool swarmOutcomeUncertain(
        const SwarmCommandService::BatchReport &report);

    bool capture(const SwarmWaypointLeaderPlan &plan,
                 SwarmWaypointLeaderExecutorBackend::Snapshot *snapshot,
                 QString *error) const;
    bool probePlan(
        const SwarmWaypointLeaderPlan &plan,
        const SwarmWaypointLeaderExecutorBackend::Snapshot &snapshot,
        QString *error) const;
    QVector<SwarmCommandMember> swarmMembers(
        const SwarmWaypointLeaderPlan &plan) const;
    QList<SwarmVehicleInstanceLease> flightLeases(
        const SwarmWaypointLeaderPlan &plan) const;
    int slotFor(const SwarmVehicleInstanceLease &lease) const;
    Capability *capabilityFor(const SwarmVehicleInstanceLease &lease);
    const Capability *capabilityFor(
        const SwarmVehicleInstanceLease &lease) const;

    void beginDiscovery();
    void submitNextRead();
    void finishDiscovery();
    void handleTick(const SwarmWaypointLeaderTick &tick);
    void beginBatch(const SwarmWaypointLeaderTick &tick);
    void dispatchNext();
    void dispatchUrgent(const SwarmWaypointLeaderTick &tick);
    void completeBatch(SwarmWaypointLeaderBatchResult result,
                       const QString &detail);
    void fail(const QString &detail,
              SwarmWaypointLeaderBatchResult result,
              bool uncertain = false);

    void parameterFinished(
        const ParameterService::ExactOperationReport &report);
    void processParameterFinished(
        const ParameterService::ExactOperationReport &report);
    void commandFinished(
        const VehicleCommandService::ExactCommandReport &report);
    void processCommandFinished(
        const VehicleCommandService::ExactCommandReport &report);
    void parameterReservationReleased(quint64 reservationId);
    void commandReservationReleased(quint64 reservationId);
    void swarmSessionCancelled(quint64 sessionId, const QString &reason);
    void drainDeferredCallbacks();

    void beginDrain(const QString &reason, bool uncertain);
    void cancelPendingParameter(const QString &reason);
    void releaseReservations();
    void finishDrain();
    void setState(State state);
    void setStatus(const QString &status);

    QPointer<SwarmWaypointLeaderExecutorBackend> m_backend;
    SwarmWaypointLeaderCore m_core;
    QTimer m_timer;
    SwarmWaypointLeaderPlan m_plan;
    QVector<SwarmVehicleInstanceLease> m_groupLeases;
    QList<SwarmVehicleInstanceLease> m_flightLeases;
    QVector<Capability> m_capabilities;
    QVector<AliasProbe> m_probes;
    int m_probeIndex = 0;
    Batch m_batch;
    PendingParameter m_pendingParameter;
    PendingCommand m_pendingCommand;
    SwarmCommandSessionToken m_swarmToken;
    VehicleCommandService::ExactReservationToken m_commandReservation;
    ParameterService::ExactReservationToken m_parameterReservation;
    State m_state = State::Idle;
    QString m_status = QStringLiteral("Waypoint Leader executor is idle.");
    QString m_drainReason;
    quint64 m_runGeneration = 0;
    bool m_dispatchAllowed = false;
    bool m_swarmReserved = false;
    bool m_commandsReserved = false;
    bool m_parametersReserved = false;
    bool m_uncertain = false;
    bool m_destroying = false;
    int m_backendDepth = 0;
    QVector<ParameterService::ExactOperationReport> m_deferredParameters;
    QVector<VehicleCommandService::ExactCommandReport> m_deferredCommands;
    QVector<quint64> m_deferredParameterReleases;
    QVector<quint64> m_deferredCommandReleases;
    QVector<QPair<quint64, QString>> m_deferredSwarmCancellations;
};

Q_DECLARE_METATYPE(SwarmWaypointLeaderExecutor::State)

#endif // SWARMWAYPOINTLEADEREXECUTOR_H
