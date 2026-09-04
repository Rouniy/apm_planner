#include "services/SwarmWaypointLeaderExecutor.h"

#include <QtTest>

#include <algorithm>
#include <utility>

namespace
{

SwarmVehicleInstanceLease makeLease(int link, int system,
                                    quint64 instance = 1)
{
    SwarmVehicleInstanceLease result;
    result.endpoint.linkId = link;
    result.endpoint.systemId = system;
    result.endpoint.componentId = MAV_COMP_ID_AUTOPILOT1;
    result.endpoint.linkName = QStringLiteral("Link %1").arg(link);
    result.linkSessionEpoch = 10 + link;
    result.instanceEpoch = instance;
    return result;
}

SwarmWaypointLeaderVehicleState makeVehicle(
    const SwarmVehicleInstanceLease &lease, double longitude,
    bool copter)
{
    SwarmWaypointLeaderVehicleState result;
    auto &telemetry = result.telemetry;
    telemetry.lease = lease;
    telemetry.heartbeatValid = true;
    telemetry.heartbeatObservedMs = 1000;
    telemetry.lastMessageMs = 1000;
    telemetry.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    telemetry.vehicleType = copter
        ? MAV_TYPE_QUADROTOR : MAV_TYPE_GROUND_ROVER;
    telemetry.baseMode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    telemetry.customMode = 0;
    telemetry.positionValid = true;
    telemetry.positionObservedMs = 1000;
    telemetry.latitudeDegrees = 35.0;
    telemetry.longitudeDegrees = longitude;
    telemetry.relativeAltitudeM = 0.0;
    telemetry.velocityValid = true;
    telemetry.velocityNorthMps = 1.0;
    telemetry.velocityEastMps = 0.0;
    telemetry.velocityDownMps = 0.0;
    return result;
}

SwarmWaypointLeaderMissionItem missionItem(
    int sequence, qint32 longitudeE7, float altitude)
{
    SwarmWaypointLeaderMissionItem result;
    result.sequence = sequence;
    result.command = MAV_CMD_NAV_WAYPOINT;
    result.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    result.latitudeE7 = 350000000;
    result.longitudeE7 = longitudeE7;
    result.relativeAltitudeM = altitude;
    return result;
}

struct Scenario
{
    SwarmVehicleInstanceLease ground = makeLease(1, 21);
    SwarmVehicleInstanceLease air = makeLease(2, 22);
    SwarmVehicleInstanceLease follower = makeLease(3, 23);
    SwarmWaypointLeaderPlan plan;
    SwarmWaypointLeaderExecutorBackend::Snapshot snapshot;

    explicit Scenario(bool withFollower = true)
    {
        snapshot.vehicles = {
            makeVehicle(ground, 33.0000, false),
            makeVehicle(air, 33.0002, true)};
        if (withFollower) {
            snapshot.vehicles.append(makeVehicle(follower, 33.0001, true));
            plan.followers.append({follower, 1});
        }
        snapshot.observationClockNowMs = 1000;
        snapshot.mission.airMaster = air;
        snapshot.mission.missionType = MAV_MISSION_TYPE_MISSION;
        snapshot.mission.contentGeneration = 7;
        snapshot.mission.contentDigest = QByteArrayLiteral("mission-digest");
        snapshot.mission.items = {
            missionItem(0, 330000000, 0.0F),
            missionItem(1, 330020000, 15.0F),
            missionItem(2, 330040000, 20.0F)};

        SwarmWaypointLeaderMissionPath path;
        QString error;
        Q_ASSERT(SwarmWaypointLeaderMissionPath::build(
            snapshot.mission, &path, &error));
        plan.groundMaster = ground;
        plan.airMaster = air;
        plan.missionSignature = path.signature();
        plan.missionContentGeneration = snapshot.mission.contentGeneration;
        plan.missionContentDigest = snapshot.mission.contentDigest;
    }

    SwarmWaypointLeaderVehicleState &airState()
    {
        return snapshot.vehicles[1];
    }
};

class FakeBackend final : public SwarmWaypointLeaderExecutorBackend
{
public:
    struct ReadRule
    {
        ParameterService::ExactTerminalResult result =
            ParameterService::ExactTerminalResult::ReadSucceeded;
        ParameterType type = ParameterType::Int16;
    };

    explicit FakeBackend(Scenario scenario)
        : data(std::move(scenario))
    {
        readRules.insert(QStringLiteral("RTL_ALT"),
                         {ParameterService::ExactTerminalResult::ReadSucceeded,
                          ParameterType::Int16});
        readRules.insert(QStringLiteral("WPNAV_ACCEL"),
                         {ParameterService::ExactTerminalResult::ReadSucceeded,
                          ParameterType::Int32});
    }

    void setCallbacks(Callbacks value) override
    {
        callbacks = std::move(value);
    }

    bool ready(QString *error) const override
    {
        if (error) {
            error->clear();
        }
        if (!isReady && error) {
            *error = QStringLiteral("fake unavailable");
        }
        return isReady;
    }

    bool capture(const SwarmWaypointLeaderPlan &,
                 Snapshot *result, QString *error) const override
    {
        ++captureCalls;
        if (failCaptureAt > 0 && captureCalls >= failCaptureAt) {
            if (error) {
                *error = QStringLiteral("capture changed");
            }
            return false;
        }
        if (result) {
            *result = data.snapshot;
        }
        if (error) {
            error->clear();
        }
        return result != nullptr;
    }

    SwarmCommandService::Result reserveSwarm(
        QObject *owner, const QVector<SwarmCommandMember> &members,
        int maximumBatchHz, SwarmCommandSessionToken *token,
        QString *error) override
    {
        events.append(QStringLiteral("reserve-swarm"));
        reservedMembers = members;
        Q_ASSERT(maximumBatchHz == 10);
        if (deleteOwnerDuringSwarmReserve) {
            delete owner;
            return SwarmCommandService::Result::Busy;
        }
        if (swarmReserveResult == SwarmCommandService::Result::Reserved) {
            swarmToken.id = ++nextReservationId;
            *token = swarmToken;
        } else if (error) {
            *error = QStringLiteral("swarm busy");
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
        streamRateHz = rateHz;
        return swarmReport(token.id);
    }

    SwarmCommandService::BatchReport sendPositionTargets(
        const SwarmCommandSessionToken &token,
        const QVector<SwarmPositionTarget> &targets,
        SwarmCommandService::PositionTargetPriority priority) override
    {
        events.append(priority
            == SwarmCommandService::PositionTargetPriority::Urgent
                ? QStringLiteral("targets-urgent")
                : QStringLiteral("targets-normal"));
        targetBatches.append(targets);
        return swarmReport(token.id);
    }

    VehicleCommandService::ExactReservationResult reserveCommands(
        QObject *owner, const QList<SwarmVehicleInstanceLease> &leases,
        VehicleCommandService::ExactReservationToken *token,
        QString *error) override
    {
        events.append(QStringLiteral("reserve-command"));
        if (commandReserveResult
            == VehicleCommandService::ExactReservationResult::Reserved) {
            commandReservation.owner = owner;
            commandReservation.reservationId = ++nextReservationId;
            commandReservation.leases = leases;
            *token = commandReservation;
        } else if (error) {
            *error = QStringLiteral("command busy");
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
        QString *) override
    {
        events.append(QStringLiteral("command:%1")
                          .arg(int(request.command)));
        commandRequests.append(request);
        pendingCommand.transactionId = ++nextOperationId;
        pendingCommand.reservationId = reservation.reservationId;
        pendingCommand.lease = lease;
        pendingCommand.command = request.command;
        *token = pendingCommand;
        if (autoCommands) {
            completeCommand(
                VehicleCommandService::ExactTerminalResult::
                    AcknowledgedAccepted);
        }
        return VehicleCommandService::ExactSubmitResult::Started;
    }

    ParameterService::ExactReservationResult reserveParameters(
        QObject *owner, const QList<SwarmVehicleInstanceLease> &leases,
        ParameterService::ExactReservationToken *token,
        QString *error) override
    {
        events.append(QStringLiteral("reserve-parameter"));
        if (parameterReserveResult
            == ParameterService::ExactReservationResult::Reserved) {
            parameterReservation.owner = owner;
            parameterReservation.reservationId = ++nextReservationId;
            parameterReservation.leases = leases;
            *token = parameterReservation;
        } else if (error) {
            *error = QStringLiteral("parameter busy");
        }
        return parameterReserveResult;
    }

    bool cancelParameterOperation(
        const ParameterService::ExactReservationToken &reservation,
        const ParameterService::ExactOperationToken &operation,
        const QString &) override
    {
        events.append(QStringLiteral("cancel-parameter"));
        if (reservation.reservationId != parameterReservation.reservationId
            || !sameParameter(operation, pendingParameter)) {
            return false;
        }
        ParameterService::ExactOperationReport report;
        report.token = pendingParameter;
        report.attempts = 1;
        report.frameAttempted = parameterFrameAttempted;
        report.terminalResult = pendingParameter.kind
                == ParameterService::ExactOperationKind::Read
            ? ParameterService::ExactTerminalResult::ReadCancelled
            : (parameterFrameAttempted
                   ? ParameterService::ExactTerminalResult::
                         WriteCancelledOutcomeUncertain
                   : ParameterService::ExactTerminalResult::WriteCancelled);
        pendingParameter = {};
        ++cancelCalls;
        if (callbacks.parameterFinished) {
            callbacks.parameterFinished(report);
        }
        return true;
    }

    bool releaseParameters(
        const ParameterService::ExactReservationToken &token) override
    {
        events.append(QStringLiteral("release-parameter"));
        if (token.reservationId != parameterReservation.reservationId) {
            return false;
        }
        parameterClosing = true;
        if (!pendingParameter.isValid()) {
            parameterClosing = false;
            if (callbacks.parameterReservationReleased) {
                callbacks.parameterReservationReleased(token.reservationId);
            }
        }
        return true;
    }

    ParameterService::ExactSubmitResult submitParameterRead(
        const ParameterService::ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const ParameterService::ExactReadRequest &request,
        ParameterService::ExactOperationToken *token,
        QString *) override
    {
        events.append(QStringLiteral("read:%1:%2")
                          .arg(lease.endpoint.systemId)
                          .arg(request.name));
        pendingParameter.operationId = ++nextOperationId;
        pendingParameter.reservationId = reservation.reservationId;
        pendingParameter.lease = lease;
        pendingParameter.kind = ParameterService::ExactOperationKind::Read;
        pendingParameter.name = request.name;
        pendingParameter.type = ParameterType::Unknown;
        *token = pendingParameter;
        parameterFrameAttempted = true;
        if (autoReads) {
            const ReadRule rule = readRules.value(request.name, ReadRule{
                ParameterService::ExactTerminalResult::ReadTimedOut,
                ParameterType::Unknown});
            completeParameter(rule.result, rule.type);
        }
        return ParameterService::ExactSubmitResult::Started;
    }

    ParameterService::ExactSubmitResult submitParameterWrite(
        const ParameterService::ExactReservationToken &reservation,
        const SwarmVehicleInstanceLease &lease,
        const ParameterService::ExactWriteRequest &request,
        ParameterService::ExactOperationToken *token,
        QString *) override
    {
        events.append(QStringLiteral("write:%1:%2")
                          .arg(lease.endpoint.systemId)
                          .arg(request.name));
        writeRequests.append(request);
        writeLeases.append(lease);
        pendingParameter.operationId = ++nextOperationId;
        pendingParameter.reservationId = reservation.reservationId;
        pendingParameter.lease = lease;
        pendingParameter.kind = ParameterService::ExactOperationKind::Write;
        pendingParameter.name = request.name;
        pendingParameter.type = request.type;
        pendingParameter.normalizedValue = request.value;
        *token = pendingParameter;
        parameterFrameAttempted = true;
        if (autoWrites) {
            completeParameter(
                ParameterService::ExactTerminalResult::WriteSucceeded,
                request.type);
        }
        return ParameterService::ExactSubmitResult::Started;
    }

    void completeParameter(
        ParameterService::ExactTerminalResult terminal,
        ParameterType type = ParameterType::Unknown)
    {
        Q_ASSERT(pendingParameter.isValid());
        ParameterService::ExactOperationReport report;
        report.token = pendingParameter;
        report.terminalResult = terminal;
        report.type = type;
        report.attempts = 1;
        report.frameAttempted = parameterFrameAttempted;
        pendingParameter = {};
        parameterFrameAttempted = false;
        if (callbacks.parameterFinished) {
            callbacks.parameterFinished(report);
        }
        if (parameterClosing && callbacks.parameterReservationReleased) {
            parameterClosing = false;
            callbacks.parameterReservationReleased(
                parameterReservation.reservationId);
        }
    }

    void completeCommand(
        VehicleCommandService::ExactTerminalResult terminal)
    {
        Q_ASSERT(pendingCommand.isValid());
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

    void publishParameterReport(
        const ParameterService::ExactOperationReport &report)
    {
        if (callbacks.parameterFinished) {
            callbacks.parameterFinished(report);
        }
    }

    static bool sameParameter(
        const ParameterService::ExactOperationToken &left,
        const ParameterService::ExactOperationToken &right)
    {
        return left.operationId == right.operationId
            && left.reservationId == right.reservationId
            && left.lease.sameInstance(right.lease)
            && left.kind == right.kind && left.name == right.name
            && left.type == right.type
            && left.normalizedValue == right.normalizedValue;
    }

    SwarmCommandService::BatchReport swarmReport(quint64 sessionId)
    {
        SwarmCommandService::BatchReport report;
        report.sessionId = sessionId;
        report.result = nextSwarmResult;
        if (nextSwarmResult
            == SwarmCommandService::Result::TransportOutcomeUncertain) {
            SwarmCommandService::MemberReport member;
            member.framesPlanned = 1;
            member.framesAttempted = 1;
            member.framesSent = 0;
            member.result = nextSwarmResult;
            report.members.append(member);
            report.detail = QStringLiteral("writer outcome uncertain");
        }
        nextSwarmResult = SwarmCommandService::Result::SentAll;
        return report;
    }

    Scenario data;
    Callbacks callbacks;
    mutable int captureCalls = 0;
    int failCaptureAt = 0;
    bool isReady = true;
    bool autoReads = false;
    bool autoWrites = false;
    bool autoCommands = false;
    bool deleteOwnerDuringSwarmReserve = false;
    bool parameterFrameAttempted = false;
    bool parameterClosing = false;
    bool commandClosing = false;
    int cancelCalls = 0;
    quint64 nextReservationId = 100;
    quint64 nextOperationId = 1000;
    SwarmCommandService::Result swarmReserveResult =
        SwarmCommandService::Result::Reserved;
    VehicleCommandService::ExactReservationResult commandReserveResult =
        VehicleCommandService::ExactReservationResult::Reserved;
    ParameterService::ExactReservationResult parameterReserveResult =
        ParameterService::ExactReservationResult::Reserved;
    SwarmCommandService::Result nextSwarmResult =
        SwarmCommandService::Result::SentAll;
    QHash<QString, ReadRule> readRules;
    QStringList events;
    QVector<SwarmCommandMember> reservedMembers;
    QVector<int> streamSlots;
    int streamRateHz = 0;
    QVector<QVector<SwarmPositionTarget>> targetBatches;
    QVector<ParameterService::ExactWriteRequest> writeRequests;
    QVector<SwarmVehicleInstanceLease> writeLeases;
    QVector<VehicleCommandService::ExactCommandRequest> commandRequests;
    SwarmCommandSessionToken swarmToken;
    VehicleCommandService::ExactReservationToken commandReservation;
    ParameterService::ExactReservationToken parameterReservation;
    ParameterService::ExactOperationToken pendingParameter;
    VehicleCommandService::ExactCommandToken pendingCommand;
};

int eventIndex(const QStringList &events, const QString &value)
{
    return events.indexOf(value);
}

} // namespace

class SwarmWaypointLeaderExecutorTest final : public QObject
{
    Q_OBJECT

private slots:
    void reservesInOrderAndRollsBackInReverse();
    void revalidatesAfterAllReservations();
    void discoversAliasesAndUsesExactRuntimeTypesInStrictOrder();
    void fallsBackToMp10AliasesWithoutGuessingTypes();
    void commandAcksStillRequireNewerHeartbeatEvidence();
    void stopCancelsParameterBeforeReleaseAndPreventsNewSends();
    void ignoresStaleTerminalTokensAcrossRuns();
    void writerAttemptFailureIsOutcomeUncertain();
    void timerContractIsTenHertzWithoutCatchUpLoop();
    void backendReentrancyCanDeleteExecutorDuringReserve();
};

void SwarmWaypointLeaderExecutorTest::
reservesInOrderAndRollsBackInReverse()
{
    FakeBackend backend{Scenario()};
    backend.parameterReserveResult =
        ParameterService::ExactReservationResult::Busy;
    SwarmWaypointLeaderExecutor executor(&backend);
    QString error;

    QVERIFY(!executor.start(backend.data.plan, &error));
    QVERIFY(error.contains(QStringLiteral("parameter"),
                           Qt::CaseInsensitive));
    const QStringList expected{
        QStringLiteral("reserve-swarm"),
        QStringLiteral("reserve-command"),
        QStringLiteral("reserve-parameter"),
        QStringLiteral("release-command"),
        QStringLiteral("release-swarm")};
    QCOMPARE(backend.events, expected);
    QCOMPARE(executor.state(), SwarmWaypointLeaderExecutor::State::Idle);
    QVERIFY(!executor.isRunning());
}

void SwarmWaypointLeaderExecutorTest::revalidatesAfterAllReservations()
{
    FakeBackend backend{Scenario()};
    backend.failCaptureAt = 2;
    SwarmWaypointLeaderExecutor executor(&backend);
    QString error;

    QVERIFY(!executor.start(backend.data.plan, &error));
    QCOMPARE(backend.captureCalls, 2);
    QVERIFY(error.contains(QStringLiteral("capture")));
    const QStringList expected{
        QStringLiteral("reserve-swarm"),
        QStringLiteral("reserve-command"),
        QStringLiteral("reserve-parameter"),
        QStringLiteral("release-parameter"),
        QStringLiteral("release-command"),
        QStringLiteral("release-swarm")};
    QCOMPARE(backend.events, expected);
    QVERIFY(std::none_of(
        backend.events.cbegin(), backend.events.cend(),
        [](const QString &event) { return event.startsWith("read:"); }));
}

void SwarmWaypointLeaderExecutorTest::
discoversAliasesAndUsesExactRuntimeTypesInStrictOrder()
{
    FakeBackend backend{Scenario()};
    backend.autoReads = true;
    SwarmWaypointLeaderExecutor executor(&backend);
    QString error;

    QVERIFY2(executor.start(backend.data.plan, &error), qPrintable(error));
    QCOMPARE(backend.streamRateHz, 5);
    QCOMPARE(backend.streamSlots, QVector<int>({0, 1, 2}));
    QCOMPARE(backend.writeRequests.size(), 1);
    QCOMPARE(backend.writeRequests.at(0).name, QStringLiteral("RTL_ALT"));
    QCOMPARE(backend.writeRequests.at(0).type, ParameterType::Int16);

    backend.completeParameter(
        ParameterService::ExactTerminalResult::WriteSucceeded,
        ParameterType::Int16);
    QCOMPARE(backend.writeRequests.size(), 2);
    QCOMPARE(backend.writeRequests.at(1).name,
             QStringLiteral("WPNAV_ACCEL"));
    QCOMPARE(backend.writeRequests.at(1).type, ParameterType::Int32);
    backend.completeParameter(
        ParameterService::ExactTerminalResult::WriteSucceeded,
        ParameterType::Int32);
    QCOMPARE(backend.writeRequests.size(), 3);
    QVERIFY(backend.writeLeases.at(2).sameInstance(backend.data.follower));
    QCOMPARE(backend.writeRequests.at(2).name, QStringLiteral("RTL_ALT"));
    backend.completeParameter(
        ParameterService::ExactTerminalResult::WriteSucceeded,
        ParameterType::Int16);
    QCOMPARE(backend.writeRequests.size(), 4);
    QCOMPARE(backend.writeRequests.at(3).name,
             QStringLiteral("WPNAV_ACCEL"));
    backend.completeParameter(
        ParameterService::ExactTerminalResult::WriteSucceeded,
        ParameterType::Int32);

    QCOMPARE(executor.mode(), SwarmWaypointLeaderMode::Takeoff);
    QCOMPARE(executor.state(), SwarmWaypointLeaderExecutor::State::Running);
    const QStringList expectedReads{
        QStringLiteral("read:22:RTL_ALT"),
        QStringLiteral("read:22:WPNAV_ACCEL"),
        QStringLiteral("read:23:RTL_ALT"),
        QStringLiteral("read:23:WPNAV_ACCEL")};
    QStringList actualReads;
    for (const QString &event : backend.events) {
        if (event.startsWith(QStringLiteral("read:"))) {
            actualReads.append(event);
        }
    }
    QCOMPARE(actualReads, expectedReads);
}

void SwarmWaypointLeaderExecutorTest::
fallsBackToMp10AliasesWithoutGuessingTypes()
{
    FakeBackend backend{Scenario(false)};
    backend.readRules.insert(
        QStringLiteral("RTL_ALT"),
        {ParameterService::ExactTerminalResult::ReadTimedOut,
         ParameterType::Unknown});
    backend.readRules.insert(
        QStringLiteral("RTL_ALT_M"),
        {ParameterService::ExactTerminalResult::ReadSucceeded,
         ParameterType::Real32});
    backend.readRules.insert(
        QStringLiteral("WPNAV_ACCEL"),
        {ParameterService::ExactTerminalResult::ReadTimedOut,
         ParameterType::Unknown});
    backend.readRules.insert(
        QStringLiteral("WP_ACC"),
        {ParameterService::ExactTerminalResult::ReadSucceeded,
         ParameterType::UInt16});
    backend.autoReads = true;
    SwarmWaypointLeaderExecutor executor(&backend);
    QString error;

    QVERIFY2(executor.start(backend.data.plan, &error), qPrintable(error));
    QCOMPARE(backend.writeRequests.size(), 1);
    QCOMPARE(backend.writeRequests.at(0).name,
             QStringLiteral("RTL_ALT_M"));
    QCOMPARE(backend.writeRequests.at(0).type, ParameterType::Real32);
    backend.completeParameter(
        ParameterService::ExactTerminalResult::WriteSucceeded,
        ParameterType::Real32);
    QCOMPARE(backend.writeRequests.size(), 2);
    QCOMPARE(backend.writeRequests.at(1).name, QStringLiteral("WP_ACC"));
    QCOMPARE(backend.writeRequests.at(1).type, ParameterType::UInt16);

    const QStringList expectedReads{
        QStringLiteral("read:22:RTL_ALT"),
        QStringLiteral("read:22:RTL_ALT_M"),
        QStringLiteral("read:22:WPNAV_ACCEL"),
        QStringLiteral("read:22:WP_ACC")};
    QStringList actualReads;
    for (const QString &event : backend.events) {
        if (event.startsWith(QStringLiteral("read:"))) {
            actualReads.append(event);
        }
    }
    QCOMPARE(actualReads, expectedReads);
}

void SwarmWaypointLeaderExecutorTest::
commandAcksStillRequireNewerHeartbeatEvidence()
{
    FakeBackend backend{Scenario(false)};
    backend.autoReads = true;
    backend.autoWrites = true;
    SwarmWaypointLeaderExecutor executor(&backend);
    QString error;
    QVERIFY2(executor.start(backend.data.plan, &error), qPrintable(error));
    QCOMPARE(executor.mode(), SwarmWaypointLeaderMode::Takeoff);

    QVERIFY(QMetaObject::invokeMethod(&executor, "controlTick",
                                      Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 1);
    QCOMPARE(backend.commandRequests.constLast().command,
             MAV_CMD_DO_SET_MODE);
    QCOMPARE(backend.commandRequests.constLast().params[1], 4.0F);
    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted);

    // ACK alone and even mode mutation on the old heartbeat cannot advance.
    backend.data.airState().telemetry.customMode = 4;
    QVERIFY(QMetaObject::invokeMethod(&executor, "controlTick",
                                      Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 1);
    ++backend.data.airState().telemetry.heartbeatObservedMs;
    ++backend.data.snapshot.observationClockNowMs;
    QVERIFY(QMetaObject::invokeMethod(&executor, "controlTick",
                                      Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 2);
    QCOMPARE(backend.commandRequests.constLast().command,
             MAV_CMD_COMPONENT_ARM_DISARM);
    backend.completeCommand(
        VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted);

    backend.data.airState().telemetry.armed = true;
    QVERIFY(QMetaObject::invokeMethod(&executor, "controlTick",
                                      Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 2);
    ++backend.data.airState().telemetry.heartbeatObservedMs;
    ++backend.data.snapshot.observationClockNowMs;
    QVERIFY(QMetaObject::invokeMethod(&executor, "controlTick",
                                      Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 3);
    QCOMPARE(backend.commandRequests.constLast().command,
             MAV_CMD_NAV_TAKEOFF);
}

void SwarmWaypointLeaderExecutorTest::
stopCancelsParameterBeforeReleaseAndPreventsNewSends()
{
    FakeBackend backend{Scenario(false)};
    backend.autoReads = true;
    SwarmWaypointLeaderExecutor executor(&backend);
    QString error;
    QVERIFY2(executor.start(backend.data.plan, &error), qPrintable(error));
    QCOMPARE(backend.writeRequests.size(), 1);
    QVERIFY(backend.pendingParameter.isValid());

    executor.cancelActiveRun(QStringLiteral("window close"));
    QCOMPARE(backend.cancelCalls, 1);
    QCOMPARE(backend.writeRequests.size(), 1);
    QVERIFY(eventIndex(backend.events, QStringLiteral("cancel-parameter"))
            < eventIndex(backend.events, QStringLiteral("release-parameter")));
    QVERIFY(eventIndex(backend.events, QStringLiteral("release-parameter"))
            < eventIndex(backend.events, QStringLiteral("release-command")));
    QVERIFY(eventIndex(backend.events, QStringLiteral("release-command"))
            < eventIndex(backend.events, QStringLiteral("release-swarm")));
    QCOMPARE(executor.state(),
             SwarmWaypointLeaderExecutor::State::OutcomeUncertain);
    QVERIFY(!executor.isRunning());
    QVERIFY(!executor.executorReady(&error));
    QVERIFY(error.contains(QStringLiteral("uncertain"),
                           Qt::CaseInsensitive));

    QVERIFY(QMetaObject::invokeMethod(&executor, "controlTick",
                                      Qt::DirectConnection));
    QCOMPARE(backend.writeRequests.size(), 1);
    QCOMPARE(backend.commandRequests.size(), 0);
    QCOMPARE(backend.targetBatches.size(), 0);
}

void SwarmWaypointLeaderExecutorTest::ignoresStaleTerminalTokensAcrossRuns()
{
    FakeBackend backend{Scenario(false)};
    SwarmWaypointLeaderExecutor executor(&backend);
    QString error;
    QVERIFY(executor.start(backend.data.plan, &error));
    const auto oldToken = backend.pendingParameter;
    const quint64 firstGeneration = executor.runGeneration();
    executor.cancelActiveRun(QStringLiteral("first stop"));
    QCOMPARE(executor.state(), SwarmWaypointLeaderExecutor::State::Idle);

    QVERIFY(executor.start(backend.data.plan, &error));
    QVERIFY(executor.runGeneration() > firstGeneration);
    const auto currentToken = backend.pendingParameter;
    ParameterService::ExactOperationReport stale;
    stale.token = oldToken;
    stale.terminalResult =
        ParameterService::ExactTerminalResult::ReadSucceeded;
    stale.type = ParameterType::Int16;
    backend.publishParameterReport(stale);
    QVERIFY(FakeBackend::sameParameter(
        backend.pendingParameter, currentToken));
    QCOMPARE(int(std::count_if(
        backend.events.cbegin(), backend.events.cend(),
        [](const QString &event) {
            return event.startsWith(QStringLiteral("read:"));
        })), 2);
}

void SwarmWaypointLeaderExecutorTest::writerAttemptFailureIsOutcomeUncertain()
{
    FakeBackend backend{Scenario(false)};
    backend.autoReads = true;
    backend.autoWrites = true;
    backend.nextSwarmResult =
        SwarmCommandService::Result::TransportOutcomeUncertain;
    SwarmWaypointLeaderExecutor executor(&backend);
    QString error;

    QVERIFY(!executor.start(backend.data.plan, &error));
    QCOMPARE(executor.state(),
             SwarmWaypointLeaderExecutor::State::OutcomeUncertain);
    QVERIFY(!executor.executorReady(&error));
    QVERIFY(executor.statusText().contains(
        QStringLiteral("uncertain"), Qt::CaseInsensitive));
}

void SwarmWaypointLeaderExecutorTest::
timerContractIsTenHertzWithoutCatchUpLoop()
{
    QCOMPARE(SwarmWaypointLeaderExecutor::ControlIntervalMs, 100);
    FakeBackend backend{Scenario(false)};
    backend.autoReads = true;
    backend.autoWrites = true;
    SwarmWaypointLeaderExecutor executor(&backend);
    QString error;
    QVERIFY(executor.start(backend.data.plan, &error));

    QVERIFY(QMetaObject::invokeMethod(&executor, "controlTick",
                                      Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 1);
    // A second delivered timer event observes the outstanding batch and never
    // catches up by submitting another command.
    QVERIFY(QMetaObject::invokeMethod(&executor, "controlTick",
                                      Qt::DirectConnection));
    QCOMPARE(backend.commandRequests.size(), 1);
}

void SwarmWaypointLeaderExecutorTest::
backendReentrancyCanDeleteExecutorDuringReserve()
{
    FakeBackend backend{Scenario(false)};
    backend.deleteOwnerDuringSwarmReserve = true;
    QPointer<SwarmWaypointLeaderExecutor> executor =
        new SwarmWaypointLeaderExecutor(&backend);
    QString error;

    QVERIFY(!executor->start(backend.data.plan, &error));
    QVERIFY(executor.isNull());
    // The backend retained guarded callbacks; invoking an obsolete one is safe.
    ParameterService::ExactOperationReport obsolete;
    backend.publishParameterReport(obsolete);
}

QTEST_GUILESS_MAIN(SwarmWaypointLeaderExecutorTest)

#include "test_swarmwaypointleaderexecutor.moc"
