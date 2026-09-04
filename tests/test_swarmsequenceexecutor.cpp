#include "services/SwarmSequenceExecutor.h"

#include <QtTest>

#include <algorithm>
#include <utility>

namespace
{

SwarmVehicleInstanceLease makeLease(int linkId, int systemId,
                                    quint64 instanceEpoch = 1)
{
    SwarmVehicleInstanceLease lease;
    lease.endpoint.linkId = linkId;
    lease.endpoint.systemId = systemId;
    lease.endpoint.componentId = MAV_COMP_ID_AUTOPILOT1;
    lease.endpoint.linkName = QStringLiteral("Link %1").arg(linkId);
    lease.linkSessionEpoch = 100 + linkId;
    lease.instanceEpoch = instanceEpoch;
    return lease;
}

SwarmTelemetrySnapshot makeSnapshot(
    const SwarmVehicleInstanceLease &lease, qint64 observedMs,
    double latitude, double longitude, bool guided = true)
{
    SwarmTelemetrySnapshot snapshot;
    snapshot.lease = lease;
    snapshot.lastMessageMs = observedMs;
    snapshot.heartbeatObservedMs = observedMs;
    snapshot.heartbeatValid = true;
    snapshot.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    snapshot.vehicleType = MAV_TYPE_QUADROTOR;
    snapshot.baseMode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    snapshot.customMode = guided ? 4 : 0;
    snapshot.positionValid = true;
    snapshot.positionObservedMs = observedMs;
    snapshot.latitudeDegrees = latitude;
    snapshot.longitudeDegrees = longitude;
    return snapshot;
}

class FakeBackend final : public SwarmSequenceExecutorBackend
{
public:
    void setCallbacks(Callbacks value) override
    {
        callbacks = std::move(value);
    }

    bool ready(QString *error) const override
    {
        if (error) {
            error->clear();
        }
        return true;
    }

    qint64 observationClockNowMs() const override { return nowMs; }

    bool observationIsFresh(qint64 observedMs,
                            int maximumAgeMs) const override
    {
        return observedMs >= 0 && nowMs >= observedMs
            && nowMs - observedMs <= maximumAgeMs;
    }

    bool snapshotForLease(const SwarmVehicleInstanceLease &lease,
                          SwarmTelemetrySnapshot *snapshot) const override
    {
        for (const auto &candidate : snapshots) {
            if (candidate.lease.sameInstance(lease)) {
                if (snapshot) {
                    *snapshot = candidate;
                }
                return snapshot != nullptr;
            }
        }
        return false;
    }

    bool routeIsEligible(const SwarmVehicleInstanceLease &,
                         QString *error) const override
    {
        if (error) {
            error->clear();
        }
        return routeEligible;
    }

    SwarmCommandService::Result reserveSwarm(
        QObject *, const QVector<SwarmCommandMember> &members,
        int maximumBatchHz, SwarmCommandSessionToken *token,
        QString *error) override
    {
        events.append(QStringLiteral("reserve-swarm"));
        reservedMembers = members;
        streamRateLimit = maximumBatchHz;
        if (swarmReserveResult == SwarmCommandService::Result::Reserved) {
            swarmToken.id = ++nextId;
            *token = swarmToken;
        } else if (error) {
            *error = QStringLiteral("swarm reserve rejected");
        }
        return swarmReserveResult;
    }

    SwarmCommandService::Result releaseSwarm(
        const SwarmCommandSessionToken &token) override
    {
        events.append(QStringLiteral("release-swarm"));
        return token.id == swarmToken.id
            ? SwarmCommandService::Result::Cancelled
            : SwarmCommandService::Result::InvalidSession;
    }

    SwarmCommandService::BatchReport requestPositionStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds, int rateHz) override
    {
        events.append(QStringLiteral("streams"));
        streamSlots = slotIds;
        requestedStreamRate = rateHz;
        return reportFor(token.id, slotIds, streamResult);
    }

    SwarmCommandService::BatchReport sendPositionTargets(
        const SwarmCommandSessionToken &token,
        const QVector<SwarmPositionTarget> &targets) override
    {
        events.append(QStringLiteral("targets"));
        targetBatches.append(targets);
        QVector<int> slotIds;
        for (const auto &target : targets) {
            slotIds.append(target.slotId);
        }
        return reportFor(token.id, slotIds, targetResult);
    }

    VehicleCommandService::ExactReservationResult reserveCommands(
        QObject *owner, const QList<SwarmVehicleInstanceLease> &leases,
        VehicleCommandService::ExactReservationToken *token,
        QString *error) override
    {
        events.append(QStringLiteral("reserve-command"));
        if (mutateAfterCommandReservation && !snapshots.isEmpty()) {
            snapshots[0].heartbeatObservedMs = nowMs - 6000;
        }
        if (commandReserveResult
            == VehicleCommandService::ExactReservationResult::Reserved) {
            commandReservation.owner = owner;
            commandReservation.reservationId = ++nextId;
            commandReservation.leases = leases;
            *token = commandReservation;
            if (cancelSwarmDuringCommandReservation
                && callbacks.swarmSessionCancelled) {
                callbacks.swarmSessionCancelled(
                    swarmToken.id,
                    QStringLiteral("synchronous swarm cancellation"));
            }
        } else if (error) {
            *error = QStringLiteral("command reserve rejected");
        }
        return commandReserveResult;
    }

    bool releaseCommands(
        const VehicleCommandService::ExactReservationToken &token) override
    {
        events.append(QStringLiteral("release-command"));
        if (token.reservationId != commandReservation.reservationId) {
            return false;
        }
        commandClosing = true;
        if (!pendingCommand.isValid()) {
            commandClosing = false;
            if (callbacks.commandReservationReleased) {
                callbacks.commandReservationReleased(token.reservationId);
            }
        }
        return true;
    }

    VehicleCommandService::ExactSubmitResult submitCommand(
        const VehicleCommandService::ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const VehicleCommandService::ExactCommandRequest &request,
        VehicleCommandService::ExactCommandToken *token,
        QString *error) override
    {
        events.append(QStringLiteral("command:%1:%2")
                          .arg(lease.endpoint.systemId)
                          .arg(int(request.command)));
        commandLeases.append(lease);
        commandRequests.append(request);
        if (nextSubmitResult
            != VehicleCommandService::ExactSubmitResult::Started) {
            if (error) {
                *error = QStringLiteral("submit rejected");
            }
            const auto result = nextSubmitResult;
            nextSubmitResult =
                VehicleCommandService::ExactSubmitResult::Started;
            return result;
        }
        pendingCommand.transactionId = ++nextId;
        pendingCommand.reservationId = reservation.reservationId;
        pendingCommand.lease = lease;
        pendingCommand.command = request.command;
        *token = pendingCommand;
        if (completeNextSynchronously) {
            completeNextSynchronously = false;
            VehicleCommandService::ExactCommandReport report;
            report.token = pendingCommand;
            report.terminalResult = synchronousTerminal;
            report.frameAttempted = true;
            pendingCommand = {};
            if (callbacks.commandFinished) {
                callbacks.commandFinished(report);
            }
        }
        return VehicleCommandService::ExactSubmitResult::Started;
    }

    void completeCommand(
        VehicleCommandService::ExactTerminalResult terminal)
    {
        QVERIFY(pendingCommand.isValid());
        VehicleCommandService::ExactCommandReport report;
        report.token = pendingCommand;
        report.terminalResult = terminal;
        report.frameAttempted = true;
        pendingCommand = {};
        if (callbacks.commandFinished) {
            callbacks.commandFinished(report);
        }
        if (commandClosing && callbacks.commandReservationReleased) {
            commandClosing = false;
            callbacks.commandReservationReleased(
                commandReservation.reservationId);
        }
    }

    SwarmTelemetrySnapshot *snapshot(
        const SwarmVehicleInstanceLease &lease)
    {
        for (auto &candidate : snapshots) {
            if (candidate.lease.sameInstance(lease)) {
                return &candidate;
            }
        }
        return nullptr;
    }

    SwarmCommandService::BatchReport reportFor(
        quint64 sessionId, const QVector<int> &slotIds,
        SwarmCommandService::Result result) const
    {
        SwarmCommandService::BatchReport report;
        report.sessionId = sessionId;
        report.result = result;
        for (int index = 0; index < slotIds.size(); ++index) {
            SwarmCommandService::MemberReport member;
            member.slotId = slotIds.at(index);
            const auto found = std::find_if(
                reservedMembers.cbegin(), reservedMembers.cend(),
                [member](const SwarmCommandMember &candidate) {
                    return candidate.slotId == member.slotId;
                });
            if (found != reservedMembers.cend()) {
                member.lease = found->lease;
            }
            member.framesPlanned = 1;
            if (result == SwarmCommandService::Result::SentAll) {
                member.framesAttempted = 1;
                member.framesSent = 1;
                member.result = result;
            } else if (result
                       == SwarmCommandService::Result::PartialSend
                       && index == 0) {
                member.framesAttempted = 1;
                member.framesSent = 1;
                member.result = SwarmCommandService::Result::SentAll;
            } else if (result
                       == SwarmCommandService::Result::
                           TransportOutcomeUncertain
                       && index == 0) {
                member.framesAttempted = 1;
                member.framesSent = 0;
                member.result = result;
            } else {
                member.result = result;
            }
            report.members.append(member);
        }
        return report;
    }

    qint64 nowMs = 1000;
    QVector<SwarmTelemetrySnapshot> snapshots;
    Callbacks callbacks;
    bool routeEligible = true;
    bool mutateAfterCommandReservation = false;
    bool commandClosing = false;
    bool completeNextSynchronously = false;
    bool cancelSwarmDuringCommandReservation = false;
    VehicleCommandService::ExactTerminalResult synchronousTerminal =
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted;
    int streamRateLimit = 0;
    int requestedStreamRate = 0;
    quint64 nextId = 100;
    SwarmCommandService::Result swarmReserveResult =
        SwarmCommandService::Result::Reserved;
    VehicleCommandService::ExactReservationResult commandReserveResult =
        VehicleCommandService::ExactReservationResult::Reserved;
    VehicleCommandService::ExactSubmitResult nextSubmitResult =
        VehicleCommandService::ExactSubmitResult::Started;
    SwarmCommandService::Result streamResult =
        SwarmCommandService::Result::SentAll;
    SwarmCommandService::Result targetResult =
        SwarmCommandService::Result::SentAll;
    QStringList events;
    QVector<SwarmCommandMember> reservedMembers;
    QVector<int> streamSlots;
    QVector<QVector<SwarmPositionTarget>> targetBatches;
    QVector<SwarmVehicleInstanceLease> commandLeases;
    QVector<VehicleCommandService::ExactCommandRequest> commandRequests;
    SwarmCommandSessionToken swarmToken;
    VehicleCommandService::ExactReservationToken commandReservation;
    VehicleCommandService::ExactCommandToken pendingCommand;
};

SwarmSequenceRunStepRequest runRequest(
    const SwarmVehicleInstanceLease &anchor,
    const SwarmVehicleInstanceLease &first,
    const SwarmVehicleInstanceLease &second)
{
    SwarmSequenceRunStepRequest request;
    request.layoutId = QStringLiteral("Line");
    request.anchor = anchor;
    SwarmSequenceExactAssignment high;
    high.systemId = 9;
    high.lease = second;
    high.offset.x = 12.0;
    high.offset.y = 9.0;
    high.offset.z = 20.0;
    SwarmSequenceExactAssignment low;
    low.systemId = 7;
    low.lease = first;
    low.offset.x = -4.0;
    low.offset.y = 3.0;
    low.offset.z = 8.0;
    request.assignments = {high, low};
    return request;
}

void advanceHeartbeat(FakeBackend *backend,
                      const SwarmVehicleInstanceLease &lease,
                      bool guided, bool armed)
{
    ++backend->nowMs;
    SwarmTelemetrySnapshot *snapshot = backend->snapshot(lease);
    QVERIFY(snapshot);
    snapshot->heartbeatObservedMs = backend->nowMs;
    snapshot->lastMessageMs = backend->nowMs;
    snapshot->customMode = guided ? 4 : 0;
    snapshot->armed = armed;
}

} // namespace

class SwarmSequenceExecutorTest final : public QObject
{
    Q_OBJECT

private slots:
    void preparesProjectionAndRunsSortedExactBatch();
    void reservationFailureReturnsToCleanIdleWithoutSending();
    void synchronousSwarmCancellationRollsBackCommandReservation();
    void streamFailureHasNoVehicleEffectAndAllowsRetry();
    void directStateListenerCanCancelBeforeFirstDispatch();
    void terminalSignalsKeepExecutorClosedUntilCompletionReturns();
    void revalidatesAfterBothReservationsBeforeSending();
    void takeoffUsesExactCommandChainAndNewHeartbeatBarriers();
    void knownRejectionSkipsVehicleAndContinuesInSystemOrder();
    void cancellationPreventsFutureCommandsAndDrainsInReverse();
    void uncertainCommandStopsAndBlocksRestart();
    void partialAndUncertainTargetReportsRemainDistinct();
};

void SwarmSequenceExecutorTest::
preparesProjectionAndRunsSortedExactBatch()
{
    const auto anchor = makeLease(1, 1);
    const auto first = makeLease(2, 42);
    const auto second = makeLease(3, 42);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(anchor, backend.nowMs, 35.0, 33.0),
        makeSnapshot(first, backend.nowMs, 35.0001, 33.0001),
        makeSnapshot(second, backend.nowMs, 35.0002, 33.0002)};
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedRunStep prepared;
    QString error;

    QVERIFY2(executor.prepareRunStep(
        runRequest(anchor, first, second), &prepared, &error),
        qPrintable(error));
    QVERIFY(prepared.isValid());
    QCOMPARE(prepared.assignments.at(0).systemId, 7);
    QCOMPARE(prepared.assignments.at(1).systemId, 9);
    QCOMPARE(prepared.origin.latitude, 35.0);
    QCOMPARE(prepared.origin.longitude, 33.0);
    SwarmSequenceGeodeticPoint expected;
    QVERIFY(SwarmSequenceGeometry::projectEastNorth(
        prepared.origin.latitude, prepared.origin.longitude,
        prepared.assignments.at(1).offset, &expected));
    QCOMPARE(prepared.targets.at(1).latitude, expected.latitude);
    QCOMPARE(prepared.targets.at(1).longitude, expected.longitude);
    QCOMPARE(prepared.targets.at(1).relativeAltitudeM, 20.0);

    QVERIFY2(executor.runStep(prepared, &error), qPrintable(error));
    QCOMPARE(executor.state(), SwarmSequenceExecutor::State::Idle);
    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::SentAll);
    QCOMPARE(backend.streamRateLimit, 10);
    QCOMPARE(backend.requestedStreamRate, 10);
    QCOMPARE(backend.streamSlots, QVector<int>({7, 9}));
    QCOMPARE(backend.targetBatches.size(), 1);
    QCOMPARE(backend.targetBatches.first().size(), 2);
    QCOMPARE(backend.targetBatches.first().at(0).slotId, 7);
    QCOMPARE(backend.targetBatches.first().at(1).slotId, 9);
    for (const auto &target : backend.targetBatches.first()) {
        QVERIFY(target.useVelocity);
        QCOMPARE(target.velocityNorthMps, 0.0F);
        QCOMPARE(target.velocityEastMps, 0.0F);
        QCOMPARE(target.velocityDownMps, 0.0F);
    }
    QCOMPARE(backend.events, QStringList({
        QStringLiteral("reserve-swarm"),
        QStringLiteral("reserve-command"),
        QStringLiteral("streams"),
        QStringLiteral("targets"),
        QStringLiteral("release-command"),
        QStringLiteral("release-swarm")}));
}

void SwarmSequenceExecutorTest::
reservationFailureReturnsToCleanIdleWithoutSending()
{
    const auto anchor = makeLease(1, 1);
    const auto first = makeLease(2, 2);
    const auto second = makeLease(3, 3);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(anchor, 1000, 35.0, 33.0),
        makeSnapshot(first, 1000, 35.1, 33.1),
        makeSnapshot(second, 1000, 35.2, 33.2)};
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedRunStep prepared;
    QString error;
    QVERIFY(executor.prepareRunStep(
        runRequest(anchor, first, second), &prepared, &error));

    backend.swarmReserveResult = SwarmCommandService::Result::Busy;
    QVERIFY(!executor.runStep(prepared, &error));
    QCOMPARE(executor.state(), SwarmSequenceExecutor::State::Idle);
    QCOMPARE(executor.operation(), SwarmSequenceOperation::None);
    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::Rejected);
    QVERIFY(!backend.events.contains(QStringLiteral("streams")));
    QVERIFY(!backend.events.contains(QStringLiteral("targets")));
    QVERIFY(executor.executorReady(&error));
}

void SwarmSequenceExecutorTest::
synchronousSwarmCancellationRollsBackCommandReservation()
{
    const auto anchor = makeLease(1, 1);
    const auto first = makeLease(2, 2);
    const auto second = makeLease(3, 3);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(anchor, 1000, 35.0, 33.0),
        makeSnapshot(first, 1000, 35.1, 33.1),
        makeSnapshot(second, 1000, 35.2, 33.2)};
    backend.cancelSwarmDuringCommandReservation = true;
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedRunStep prepared;
    QString error;
    QVERIFY(executor.prepareRunStep(
        runRequest(anchor, first, second), &prepared, &error));

    QVERIFY(!executor.runStep(prepared, &error));
    QCOMPARE(executor.state(), SwarmSequenceExecutor::State::Idle);
    QCOMPARE(executor.operation(), SwarmSequenceOperation::None);
    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::Rejected);
    QCOMPARE(backend.events.count(QStringLiteral("release-command")), 1);
    QVERIFY(!backend.events.contains(QStringLiteral("streams")));
    QVERIFY(!backend.events.contains(QStringLiteral("targets")));
    QVERIFY(executor.executorReady(&error));
}

void SwarmSequenceExecutorTest::
streamFailureHasNoVehicleEffectAndAllowsRetry()
{
    const auto anchor = makeLease(1, 1);
    const auto first = makeLease(2, 2);
    const auto second = makeLease(3, 3);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(anchor, 1000, 35.0, 33.0),
        makeSnapshot(first, 1000, 35.1, 33.1),
        makeSnapshot(second, 1000, 35.2, 33.2)};
    backend.streamResult = SwarmCommandService::Result::PartialSend;
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedRunStep prepared;
    QString error;
    QVERIFY(executor.prepareRunStep(
        runRequest(anchor, first, second), &prepared, &error));
    QVERIFY(executor.runStep(prepared, &error));

    QCOMPARE(executor.state(), SwarmSequenceExecutor::State::Idle);
    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::Rejected);
    QCOMPARE(executor.lastReport().framesPlanned, 0);
    QCOMPARE(executor.lastReport().framesAttempted, 0);
    QCOMPARE(executor.lastReport().framesSent, 0);
    QVERIFY(std::none_of(
        executor.lastReport().vehicles.cbegin(),
        executor.lastReport().vehicles.cend(),
        [](const SwarmSequenceVehicleResult &vehicle) {
            return vehicle.targetSent;
        }));
    QVERIFY(!backend.events.contains(QStringLiteral("targets")));
    QVERIFY(executor.executorReady(&error));
}

void SwarmSequenceExecutorTest::
directStateListenerCanCancelBeforeFirstDispatch()
{
    const auto anchor = makeLease(1, 1);
    const auto first = makeLease(2, 2);
    const auto second = makeLease(3, 3);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(anchor, 1000, 35.0, 33.0),
        makeSnapshot(first, 1000, 35.1, 33.1),
        makeSnapshot(second, 1000, 35.2, 33.2)};
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedRunStep prepared;
    QString error;
    QVERIFY(executor.prepareRunStep(
        runRequest(anchor, first, second), &prepared, &error));
    connect(&executor, &SwarmSequenceExecutor::stateChanged,
            &executor, [&executor](SwarmSequenceExecutor::State state) {
        if (state == SwarmSequenceExecutor::State::RunningStep) {
            executor.cancelActiveOperation(
                QStringLiteral("direct listener cancellation"));
        }
    });

    QVERIFY(!executor.runStep(prepared, &error));
    QCOMPARE(executor.state(), SwarmSequenceExecutor::State::Idle);
    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::Cancelled);
    QVERIFY(!backend.events.contains(QStringLiteral("streams")));
    QVERIFY(!backend.events.contains(QStringLiteral("targets")));
}

void SwarmSequenceExecutorTest::
terminalSignalsKeepExecutorClosedUntilCompletionReturns()
{
    const auto anchor = makeLease(1, 1);
    const auto first = makeLease(2, 2);
    const auto second = makeLease(3, 3);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(anchor, 1000, 35.0, 33.0),
        makeSnapshot(first, 1000, 35.1, 33.1),
        makeSnapshot(second, 1000, 35.2, 33.2)};
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedRunStep prepared;
    QString error;
    QVERIFY(executor.prepareRunStep(
        runRequest(anchor, first, second), &prepared, &error));
    bool sawTerminalState = false;
    bool readyInsideTerminalSignal = true;
    connect(&executor, &SwarmSequenceExecutor::stateChanged,
            &executor, [&](SwarmSequenceExecutor::State state) {
        if (state == SwarmSequenceExecutor::State::Idle) {
            sawTerminalState = true;
            QString nestedError;
            readyInsideTerminalSignal = executor.executorReady(&nestedError);
        }
    });

    QVERIFY(executor.runStep(prepared, &error));
    QVERIFY(sawTerminalState);
    QVERIFY(!readyInsideTerminalSignal);
    QVERIFY(executor.executorReady(&error));
}

void SwarmSequenceExecutorTest::
revalidatesAfterBothReservationsBeforeSending()
{
    const auto anchor = makeLease(1, 1);
    const auto vehicle = makeLease(2, 2);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(anchor, backend.nowMs, 35.0, 33.0),
        makeSnapshot(vehicle, backend.nowMs, 35.1, 33.1)};
    SwarmSequenceExecutor executor(&backend);
    SwarmSequenceRunStepRequest request;
    request.layoutId = QStringLiteral("One");
    request.anchor = anchor;
    request.assignments.append({7, vehicle, {1.0, 2.0, 3.0}});
    SwarmSequencePreparedRunStep prepared;
    QString error;
    QVERIFY(executor.prepareRunStep(request, &prepared, &error));
    backend.mutateAfterCommandReservation = true;

    QVERIFY(!executor.runStep(prepared, &error));
    QVERIFY(error.contains(QStringLiteral("heartbeat"), Qt::CaseInsensitive));
    QVERIFY(!backend.events.contains(QStringLiteral("streams")));
    QVERIFY(!backend.events.contains(QStringLiteral("targets")));
    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::Rejected);
    QVERIFY(backend.events.indexOf(QStringLiteral("release-command"))
            < backend.events.indexOf(QStringLiteral("release-swarm")));
}

void SwarmSequenceExecutorTest::
takeoffUsesExactCommandChainAndNewHeartbeatBarriers()
{
    const auto vehicle = makeLease(2, 9);
    FakeBackend backend;
    auto snapshot = makeSnapshot(
        vehicle, backend.nowMs, 35.0, 33.0, false);
    snapshot.armed = true; // ARM is deliberately sent even when already armed.
    backend.snapshots = {snapshot};
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedTakeoff prepared;
    QString error;
    QVERIFY(executor.prepareTakeoff({{9, vehicle}}, &prepared, &error));
    QVERIFY(executor.startTakeoff(prepared, &error));

    QCOMPARE(backend.commandRequests.size(), 1);
    QCOMPARE(backend.commandRequests.at(0).command, MAV_CMD_DO_SET_MODE);
    QCOMPARE(backend.commandRequests.at(0).params[0],
             float(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED));
    QCOMPARE(backend.commandRequests.at(0).params[1], 4.0F);

    VehicleCommandService::ExactCommandReport wrongReport;
    wrongReport.token = backend.pendingCommand;
    ++wrongReport.token.lease.instanceEpoch;
    wrongReport.terminalResult =
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted;
    wrongReport.frameAttempted = true;
    backend.callbacks.commandFinished(wrongReport);
    QCOMPARE(executor.state(), SwarmSequenceExecutor::State::TakingOff);
    QCOMPARE(backend.commandRequests.size(), 1);

    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted);
    QCOMPARE(executor.state(),
             SwarmSequenceExecutor::State::WaitingForHeartbeat);

    // Changing state on the old heartbeat cannot cross the barrier.
    backend.snapshot(vehicle)->customMode = 4;
    QVERIFY(QMetaObject::invokeMethod(
        &executor, "heartbeatPoll", Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 1);

    advanceHeartbeat(&backend, vehicle, true, true);
    QVERIFY(QMetaObject::invokeMethod(
        &executor, "heartbeatPoll", Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 2);
    QCOMPARE(backend.commandRequests.at(1).command,
             MAV_CMD_COMPONENT_ARM_DISARM);
    QCOMPARE(backend.commandRequests.at(1).params[0], 1.0F);
    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted);

    QVERIFY(QMetaObject::invokeMethod(
        &executor, "heartbeatPoll", Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 2);
    advanceHeartbeat(&backend, vehicle, true, true);
    QVERIFY(QMetaObject::invokeMethod(
        &executor, "heartbeatPoll", Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 3);
    QCOMPARE(backend.commandRequests.at(2).command,
             MAV_CMD_NAV_TAKEOFF);
    QCOMPARE(backend.commandRequests.at(2).params[6], 2.0F);
    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted);

    QCOMPARE(executor.state(), SwarmSequenceExecutor::State::Idle);
    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::SentAll);
    QVERIFY(executor.lastReport().vehicles.first().guidedAccepted);
    QVERIFY(executor.lastReport().vehicles.first().armAccepted);
    QVERIFY(executor.lastReport().vehicles.first().takeoffAccepted);
}

void SwarmSequenceExecutorTest::
knownRejectionSkipsVehicleAndContinuesInSystemOrder()
{
    const auto high = makeLease(4, 40);
    const auto low = makeLease(3, 30);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(high, backend.nowMs, 35.1, 33.1, false),
        makeSnapshot(low, backend.nowMs, 35.0, 33.0, false)};
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedTakeoff prepared;
    QString error;
    QVERIFY(executor.prepareTakeoff(
        {{9, high}, {7, low}}, &prepared, &error));
    backend.completeNextSynchronously = true;
    backend.synchronousTerminal =
        VehicleCommandService::ExactTerminalResult::AcknowledgedRejected;
    QVERIFY(executor.startTakeoff(prepared, &error));
    QVERIFY(backend.commandLeases.first().sameInstance(low));
    QCOMPARE(backend.commandRequests.size(), 2);
    QVERIFY(backend.commandLeases.at(1).sameInstance(high));
    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted);
    advanceHeartbeat(&backend, high, true, false);
    QVERIFY(QMetaObject::invokeMethod(
        &executor, "heartbeatPoll", Qt::DirectConnection));
    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted);
    advanceHeartbeat(&backend, high, true, true);
    QVERIFY(QMetaObject::invokeMethod(
        &executor, "heartbeatPoll", Qt::DirectConnection));
    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted);

    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::Partial);
    QCOMPARE(executor.state(),
             SwarmSequenceExecutor::State::PartialEffect);
    QVERIFY(!executor.lastReport().vehicles.at(0).guidedAccepted);
    QVERIFY(executor.lastReport().vehicles.at(1).takeoffAccepted);
}

void SwarmSequenceExecutorTest::
cancellationPreventsFutureCommandsAndDrainsInReverse()
{
    const auto vehicle = makeLease(2, 9);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(vehicle, backend.nowMs, 35.0, 33.0, false)};
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedTakeoff prepared;
    QString error;
    QVERIFY(executor.prepareTakeoff({{9, vehicle}}, &prepared, &error));
    QVERIFY(executor.startTakeoff(prepared, &error));
    QCOMPARE(backend.commandRequests.size(), 1);

    executor.cancelActiveOperation(QStringLiteral("window closed"));
    QCOMPARE(executor.state(), SwarmSequenceExecutor::State::Draining);
    QVERIFY(backend.events.contains(QStringLiteral("release-command")));
    QVERIFY(!backend.events.contains(QStringLiteral("release-swarm")));

    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted);
    QCOMPARE(backend.commandRequests.size(), 1);
    QCOMPARE(executor.state(),
             SwarmSequenceExecutor::State::PartialEffect);
    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::Partial);
    QVERIFY(backend.events.indexOf(QStringLiteral("release-command"))
            < backend.events.indexOf(QStringLiteral("release-swarm")));
}

void SwarmSequenceExecutorTest::uncertainCommandStopsAndBlocksRestart()
{
    const auto vehicle = makeLease(2, 9);
    FakeBackend backend;
    backend.snapshots = {
        makeSnapshot(vehicle, backend.nowMs, 35.0, 33.0, false)};
    SwarmSequenceExecutor executor(&backend);
    SwarmSequencePreparedTakeoff prepared;
    QString error;
    QVERIFY(executor.prepareTakeoff({{9, vehicle}}, &prepared, &error));
    QVERIFY(executor.startTakeoff(prepared, &error));

    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::TimedOutOutcomeUncertain);
    QCOMPARE(executor.state(),
             SwarmSequenceExecutor::State::OutcomeUncertain);
    QCOMPARE(executor.lastReport().result,
             SwarmSequenceOperationResult::OutcomeUncertain);
    QVERIFY(!executor.executorReady(&error));
    QVERIFY(error.contains(QStringLiteral("uncertain"), Qt::CaseInsensitive));
    QCOMPARE(backend.commandRequests.size(), 1);
}

void SwarmSequenceExecutorTest::
partialAndUncertainTargetReportsRemainDistinct()
{
    const auto anchor = makeLease(1, 1);
    const auto first = makeLease(2, 2);
    const auto second = makeLease(3, 3);
    const auto request = runRequest(anchor, first, second);

    FakeBackend partialBackend;
    partialBackend.snapshots = {
        makeSnapshot(anchor, 1000, 35.0, 33.0),
        makeSnapshot(first, 1000, 35.1, 33.1),
        makeSnapshot(second, 1000, 35.2, 33.2)};
    partialBackend.targetResult = SwarmCommandService::Result::PartialSend;
    SwarmSequenceExecutor partialExecutor(&partialBackend);
    SwarmSequencePreparedRunStep partialPlan;
    QString error;
    QVERIFY(partialExecutor.prepareRunStep(
        request, &partialPlan, &error));
    QVERIFY(partialExecutor.runStep(partialPlan, &error));
    QCOMPARE(partialExecutor.state(),
             SwarmSequenceExecutor::State::PartialEffect);
    QCOMPARE(partialExecutor.lastReport().result,
             SwarmSequenceOperationResult::Partial);
    QVERIFY(!partialExecutor.executorReady(&error));
    QVERIFY(error.contains(QStringLiteral("partial"), Qt::CaseInsensitive));
    QCOMPARE(partialBackend.targetBatches.size(), 1);

    FakeBackend uncertainBackend;
    uncertainBackend.snapshots = partialBackend.snapshots;
    uncertainBackend.targetResult =
        SwarmCommandService::Result::TransportOutcomeUncertain;
    SwarmSequenceExecutor uncertainExecutor(&uncertainBackend);
    SwarmSequencePreparedRunStep uncertainPlan;
    QVERIFY(uncertainExecutor.prepareRunStep(
        request, &uncertainPlan, &error));
    QVERIFY(uncertainExecutor.runStep(uncertainPlan, &error));
    QCOMPARE(uncertainExecutor.state(),
             SwarmSequenceExecutor::State::OutcomeUncertain);
    QCOMPARE(uncertainExecutor.lastReport().result,
             SwarmSequenceOperationResult::OutcomeUncertain);
    QCOMPARE(uncertainBackend.targetBatches.size(), 1);
}

QTEST_MAIN(SwarmSequenceExecutorTest)
#include "test_swarmsequenceexecutor.moc"
