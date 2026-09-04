#include "comm/ExactLinkTransmitter.h"
#include "comm/GuidedTargetService.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"

#include <QSignalSpy>
#include <QtTest>

#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace
{

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("Link %1").arg(linkId);
    result.componentName = QStringLiteral("Component %1").arg(componentId);
    return result;
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    if (state != MAVLINK_FRAMING_OK) {
        return {};
    }
    return message;
}

mavlink_message_t commandAck(
    int sourceSystem, int sourceComponent, MAV_CMD command, int result,
    int targetSystem = 250, int targetComponent = 190,
    int progress = 255)
{
    mavlink_command_ack_t payload{};
    payload.command = static_cast<quint16>(command);
    payload.result = static_cast<quint8>(result);
    payload.progress = static_cast<quint8>(progress);
    payload.target_system = static_cast<quint8>(targetSystem);
    payload.target_component = static_cast<quint8>(targetComponent);
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(
        static_cast<quint8>(sourceSystem),
        static_cast<quint8>(sourceComponent), &message, &payload);
    return message;
}

class Fixture
{
public:
    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            if (writeHook) {
                std::function<void()> hook = std::move(writeHook);
                writeHook = {};
                hook();
            }
            return writerSucceeds;
        })
        , commands(&targets, &transmitter)
        , service(&targets, &commands)
    {
        service.setLocalIdentity(250, 190);
    }

    VehicleTargetLease selectFresh(const VehicleEndpoint &value)
    {
        targets.observeEndpoint(value, true);
        if (!targets.acquireTarget().endpoint.sameIdentity(value)) {
            targets.selectTarget(value.linkId, value.systemId,
                                 value.componentId);
        }
        const VehicleTargetLease lease = targets.acquireTarget();
        targets.observeHeartbeat(lease.endpoint, false,
                                 MAV_AUTOPILOT_ARDUPILOTMEGA,
                                 MAV_TYPE_QUADROTOR);
        return lease;
    }

    void acknowledge(int result, int linkId = 9, int systemId = 42,
                     int componentId = 1, int targetSystem = 250,
                     int targetComponent = 190, int progress = 255,
                     MAV_CMD command = MAV_CMD_DO_REPOSITION)
    {
        commands.observeMessage(
            linkId,
            commandAck(systemId, componentId, command, result,
                       targetSystem, targetComponent, progress));
    }

    mavlink_command_int_t commandAt(int index) const
    {
        mavlink_command_int_t command{};
        const mavlink_message_t message = decodeFrame(frames.at(index).bytes);
        if (message.msgid == MAVLINK_MSG_ID_COMMAND_INT) {
            mavlink_msg_command_int_decode(&message, &command);
        }
        return command;
    }

    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    bool writerSucceeds = true;
    std::function<void()> writeHook;
    ExactLinkTransmitter transmitter;
    VehicleCommandService commands;
    GuidedTargetService service;
};

const GuidedTargetService::Target kInitialTarget{
    34.1234567, 33.1234567, 25.0};

} // namespace

class GuidedTargetServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void wirePayloadRejectsNullIslandAndTruncatesCoordinates();
    void reservationClaimsOwnerWithoutSendingUntilSubmit();
    void reservationPublishIsReentrancySafe();
    void positionAndHeartbeatValidationFailClosed();
    void unsettledAndStaleLeasesFailClosed();
    void exactAckAndInProgressExtendTimeout();
    void ownerIsExclusiveAndOnlyLatestTargetQueues();
    void retryAcknowledgementDrainsBeforeQueuedTarget();
    void firstAcceptedClearsChangeModeFlag();
    void terminalResultsEndTheSession_data();
    void terminalResultsEndTheSession();
    void timeoutRetriesThenRecoversAfterIsolation();
    void stopPendingReleasesOwnerAndLateAckClearsIsolation();
    void staleTokenCannotStopANewerSession();
    void targetChangeIsolationExpires();
    void awayAndBackRequiresANewHeartbeat();
    void ownerDestructionIsSafe();
    void transportFailureQuarantinesTheEndpoint();
    void reentrantTargetChangeDuringWriteFailsUncertain();
    void reentrantSessionEndedHandlerMayStartANewSession();
};

void GuidedTargetServiceTest::
wirePayloadRejectsNullIslandAndTruncatesCoordinates()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject owner;
    GuidedTargetService::SessionToken session;

    QCOMPARE(fixture.service.start(
                 &owner, lease, kInitialTarget, &session),
             GuidedTargetService::RequestResult::Started);
    QVERIFY(session.isValid());
    QCOMPARE(fixture.service.state(),
             GuidedTargetService::State::AwaitingAcknowledgement);
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(fixture.frames.constFirst().linkId, 9);

    const mavlink_message_t message =
        decodeFrame(fixture.frames.constFirst().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_COMMAND_INT));
    QCOMPARE(message.sysid, quint8(250));
    QCOMPARE(message.compid, quint8(190));
    const mavlink_command_int_t first = fixture.commandAt(0);
    QCOMPARE(first.target_system, quint8(42));
    QCOMPARE(first.target_component, quint8(1));
    QCOMPARE(first.command, quint16(MAV_CMD_DO_REPOSITION));
    QCOMPARE(first.frame, quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QCOMPARE(first.param1, -1.0F);
    QCOMPARE(first.param2,
             float(MAV_DO_REPOSITION_FLAGS_CHANGE_MODE));
    QCOMPARE(first.param3, 0.0F);
    QVERIFY(std::isnan(first.param4));
    QCOMPARE(first.x, qint32(341234567));
    QCOMPARE(first.y, qint32(331234567));
    QCOMPARE(first.z, 25.0F);
    QCOMPARE(first.current, quint8(0));
    QCOMPARE(first.autocontinue, quint8(0));

    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.state(), GuidedTargetService::State::Active);

    const GuidedTargetService::Target precise{
        12.34567899, -45.67891239, 51.25};
    QCOMPARE(fixture.service.submit(session, precise),
             GuidedTargetService::RequestResult::Sent);
    QCOMPARE(fixture.frames.size(), 2);
    const mavlink_command_int_t second = fixture.commandAt(1);
    // Mission Planner casts the scaled coordinate and therefore truncates
    // toward zero rather than rounding the eighth decimal place.
    QCOMPARE(second.x, qint32(123456789));
    QCOMPARE(second.y, qint32(-456789123));
    QCOMPARE(second.z, 51.25F);
}

void GuidedTargetServiceTest::
reservationClaimsOwnerWithoutSendingUntilSubmit()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject owner;
    QObject competingOwner;
    GuidedTargetService::SessionToken session;

    QCOMPARE(fixture.service.reserve(&owner, lease, &session),
             GuidedTargetService::RequestResult::Started);
    QVERIFY(session.isValid());
    QCOMPARE(fixture.service.state(), GuidedTargetService::State::Reserved);
    QCOMPARE(fixture.frames.size(), 0);
    QCOMPARE(fixture.service.reserve(&competingOwner, lease),
             GuidedTargetService::RequestResult::Busy);

    QCOMPARE(fixture.service.submit(session, kInitialTarget),
             GuidedTargetService::RequestResult::Sent);
    QCOMPARE(fixture.service.state(),
             GuidedTargetService::State::AwaitingAcknowledgement);
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(fixture.commandAt(0).param2,
             float(MAV_DO_REPOSITION_FLAGS_CHANGE_MODE));

    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.state(), GuidedTargetService::State::Active);
    QCOMPARE(fixture.service.stop(session),
             GuidedTargetService::RequestResult::Stopped);
    QCOMPARE(fixture.service.state(), GuidedTargetService::State::Idle);
}

void GuidedTargetServiceTest::reservationPublishIsReentrancySafe()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject *owner = new QObject;
    GuidedTargetService::SessionToken session;
    const QMetaObject::Connection connection = QObject::connect(
        &fixture.service, &GuidedTargetService::stateChanged,
        &fixture.service, [&](GuidedTargetService::State state) {
            if (state == GuidedTargetService::State::Reserved) {
                delete owner;
                owner = nullptr;
            }
        });

    QCOMPARE(fixture.service.reserve(owner, lease, &session),
             GuidedTargetService::RequestResult::Stopped);
    QObject::disconnect(connection);
    QVERIFY(!session.isValid());
    QVERIFY(!fixture.service.hasActiveSession());
    QCOMPARE(fixture.frames.size(), 0);
    QCOMPARE(fixture.service.state(), GuidedTargetService::State::Idle);
}

void GuidedTargetServiceTest::positionAndHeartbeatValidationFailClosed()
{
    Fixture fixture;
    const VehicleEndpoint selected = endpoint(9);
    fixture.targets.observeEndpoint(selected, true);
    const VehicleTargetLease lease = fixture.targets.acquireTarget();
    QObject owner;

    QCOMPARE(fixture.service.start(&owner, lease, kInitialTarget),
             GuidedTargetService::RequestResult::HeartbeatStale);
    QCOMPARE(fixture.frames.size(), 0);

    const QList<GuidedTargetService::Target> invalidTargets{
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 10.0},
        {91.0, 0.0, 10.0},
        {0.0, -181.0, 10.0},
        {0.0, 0.0, 10.0},
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 10000.01}
    };
    for (const GuidedTargetService::Target &target : invalidTargets) {
        QCOMPARE(fixture.service.start(&owner, lease, target),
                 GuidedTargetService::RequestResult::InvalidPosition);
    }
    QCOMPARE(fixture.frames.size(), 0);

    fixture.targets.observeHeartbeat(selected, false,
                                     MAV_AUTOPILOT_ARDUPILOTMEGA,
                                     MAV_TYPE_QUADROTOR);
    GuidedTargetService::SessionToken session;
    QCOMPARE(fixture.service.start(
                 &owner, lease, {90.0, 180.0, 10000.0}, &session),
             GuidedTargetService::RequestResult::Started);
    QCOMPARE(fixture.frames.size(), 1);
}

void GuidedTargetServiceTest::unsettledAndStaleLeasesFailClosed()
{
    Fixture fixture;
    const VehicleTargetLease stale = fixture.selectFresh(endpoint(9));
    fixture.targets.observeEndpoint(endpoint(10));
    QVERIFY(fixture.targets.selectTarget(10, 42, 1));
    fixture.targets.observeHeartbeat(endpoint(10), false,
                                     MAV_AUTOPILOT_ARDUPILOTMEGA,
                                     MAV_TYPE_QUADROTOR);
    QObject owner;
    QCOMPARE(fixture.service.start(&owner, stale, kInitialTarget),
             GuidedTargetService::RequestResult::StaleTarget);

    GuidedTargetService::RequestResult nestedResult =
        GuidedTargetService::RequestResult::Sent;
    QObject nestedOwner;
    QMetaObject::Connection connection = QObject::connect(
        &fixture.targets, &VehicleTargetManager::targetGenerationChanged,
        &fixture.targets, [&](qulonglong) {
            nestedResult = fixture.service.start(
                &nestedOwner, fixture.targets.acquireTarget(),
                kInitialTarget);
        });
    QVERIFY(fixture.targets.selectTarget(9, 42, 1));
    QObject::disconnect(connection);
    QCOMPARE(nestedResult,
             GuidedTargetService::RequestResult::TargetUnsettled);
    QCOMPARE(fixture.frames.size(), 0);
}

void GuidedTargetServiceTest::exactAckAndInProgressExtendTimeout()
{
    Fixture fixture;
    fixture.service.setCommandTimeoutForTesting(80);
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject owner;
    GuidedTargetService::SessionToken session;
    QCOMPARE(fixture.service.start(
                 &owner, lease, kInitialTarget, &session),
             GuidedTargetService::RequestResult::Started);

    fixture.acknowledge(MAV_RESULT_ACCEPTED, 8);
    fixture.acknowledge(MAV_RESULT_ACCEPTED, 9, 43, 1);
    fixture.acknowledge(MAV_RESULT_ACCEPTED, 9, 42, 2);
    fixture.acknowledge(MAV_RESULT_ACCEPTED, 9, 42, 1, 251, 190);
    fixture.acknowledge(MAV_RESULT_ACCEPTED, 9, 42, 1, 250, 191);
    fixture.acknowledge(MAV_RESULT_ACCEPTED, 9, 42, 1, 250, 190,
                        255, MAV_CMD_DO_CHANGE_SPEED);
    QCOMPARE(fixture.service.state(),
             GuidedTargetService::State::AwaitingAcknowledgement);

    QTest::qWait(50);
    fixture.acknowledge(MAV_RESULT_IN_PROGRESS, 9, 42, 1,
                        250, 190, 37);
    QTest::qWait(50);
    QCOMPARE(fixture.service.state(),
             GuidedTargetService::State::AwaitingAcknowledgement);
    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.state(), GuidedTargetService::State::Active);
}

void GuidedTargetServiceTest::ownerIsExclusiveAndOnlyLatestTargetQueues()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject firstOwner;
    QObject secondOwner;
    GuidedTargetService::SessionToken firstSession;
    QCOMPARE(fixture.service.start(
                 &firstOwner, lease, kInitialTarget, &firstSession),
             GuidedTargetService::RequestResult::Started);
    QCOMPARE(fixture.service.start(
                 &secondOwner, lease, {1.0, 1.0, 10.0}),
             GuidedTargetService::RequestResult::Busy);

    QCOMPARE(fixture.service.submit(
                 firstSession, {1.0, 2.0, 30.0}),
             GuidedTargetService::RequestResult::Queued);
    QCOMPARE(fixture.service.submit(
                 firstSession, {3.0, 4.0, 40.0}),
             GuidedTargetService::RequestResult::Queued);
    QCOMPARE(fixture.frames.size(), 1);
    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.frames.size(), 2);
    const mavlink_command_int_t queued = fixture.commandAt(1);
    QCOMPARE(queued.x, qint32(30000000));
    QCOMPARE(queued.y, qint32(40000000));
    QCOMPARE(queued.z, 40.0F);
}

void GuidedTargetServiceTest::
retryAcknowledgementDrainsBeforeQueuedTarget()
{
    Fixture fixture;
    fixture.service.setCommandTimeoutForTesting(30);
    fixture.service.setRecoveryQuarantineForTesting(50);
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject owner;
    GuidedTargetService::SessionToken session;
    QSignalSpy accepted(&fixture.service,
                        &GuidedTargetService::targetAccepted);
    QCOMPARE(fixture.service.start(
                 &owner, lease, kInitialTarget, &session),
             GuidedTargetService::RequestResult::Started);
    QCOMPARE(fixture.service.submit(session, {1.0, 2.0, 30.0}),
             GuidedTargetService::RequestResult::Queued);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 2, 100);

    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.state(), GuidedTargetService::State::Draining);
    QCOMPARE(accepted.count(), 1);
    QCOMPARE(fixture.frames.size(), 2);

    // Another terminal ACK from the identical retry is ignored while no new
    // command is registered with VehicleCommandService.
    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QCOMPARE(accepted.count(), 1);
    QCOMPARE(fixture.frames.size(), 2);

    QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 3, 200);
    QCOMPARE(fixture.service.state(),
             GuidedTargetService::State::AwaitingAcknowledgement);
    const mavlink_command_int_t queued = fixture.commandAt(2);
    QCOMPARE(queued.x, qint32(10000000));
    QCOMPARE(queued.y, qint32(20000000));
    QCOMPARE(queued.z, 30.0F);
}

void GuidedTargetServiceTest::firstAcceptedClearsChangeModeFlag()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject owner;
    GuidedTargetService::SessionToken session;
    QCOMPARE(fixture.service.start(
                 &owner, lease, kInitialTarget, &session),
             GuidedTargetService::RequestResult::Started);
    QCOMPARE(fixture.commandAt(0).param2,
             float(MAV_DO_REPOSITION_FLAGS_CHANGE_MODE));
    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.submit(session, {2.0, 3.0, 20.0}),
             GuidedTargetService::RequestResult::Sent);
    QCOMPARE(fixture.commandAt(1).param2, 0.0F);
}

void GuidedTargetServiceTest::terminalResultsEndTheSession_data()
{
    QTest::addColumn<int>("ackResult");
    QTest::addColumn<GuidedTargetService::RequestResult>("serviceResult");

    QTest::newRow("temporary rejection")
        << int(MAV_RESULT_TEMPORARILY_REJECTED)
        << GuidedTargetService::RequestResult::CommandRejected;
    QTest::newRow("denied") << int(MAV_RESULT_DENIED)
        << GuidedTargetService::RequestResult::CommandRejected;
    QTest::newRow("unsupported") << int(MAV_RESULT_UNSUPPORTED)
        << GuidedTargetService::RequestResult::CommandUnsupported;
    QTest::newRow("failed") << int(MAV_RESULT_FAILED)
        << GuidedTargetService::RequestResult::CommandFailed;
    QTest::newRow("cancelled") << 6
        << GuidedTargetService::RequestResult::CommandCancelled;
}

void GuidedTargetServiceTest::terminalResultsEndTheSession()
{
    QFETCH(int, ackResult);
    QFETCH(GuidedTargetService::RequestResult, serviceResult);
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject owner;
    QSignalSpy ended(&fixture.service,
                     &GuidedTargetService::sessionEnded);
    QCOMPARE(fixture.service.start(&owner, lease, kInitialTarget),
             GuidedTargetService::RequestResult::Started);
    fixture.acknowledge(ackResult);
    QCOMPARE(fixture.service.state(), GuidedTargetService::State::Idle);
    QVERIFY(!fixture.service.hasActiveSession());
    QCOMPARE(ended.count(), 1);
    QCOMPARE(qvariant_cast<GuidedTargetService::RequestResult>(
                 ended.constFirst().at(1)), serviceResult);
}

void GuidedTargetServiceTest::timeoutRetriesThenRecoversAfterIsolation()
{
    Fixture fixture;
    fixture.service.setCommandTimeoutForTesting(20);
    fixture.service.setRecoveryQuarantineForTesting(100);
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject owner;
    QObject otherOwner;
    GuidedTargetService::SessionToken session;
    QCOMPARE(fixture.service.start(
                 &owner, lease, kInitialTarget, &session),
             GuidedTargetService::RequestResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(
        fixture.service.state(),
        GuidedTargetService::State::OutcomeUncertain, 200);
    QVERIFY(fixture.service.isOutcomeUncertain(lease.endpoint));
    QVERIFY(!fixture.service.hasActiveSession());
    QCOMPARE(fixture.service.submit(session, {1.0, 1.0, 20.0}),
             GuidedTargetService::RequestResult::InvalidSession);
    QCOMPARE(fixture.service.start(
                 &otherOwner, lease, {1.0, 1.0, 20.0}),
             GuidedTargetService::RequestResult::OutcomeUncertain);
    QCOMPARE(fixture.frames.size(),
             GuidedTargetService::MaximumSendAttempts);

    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QVERIFY(fixture.service.isOutcomeUncertain(lease.endpoint));
    QTRY_VERIFY_WITH_TIMEOUT(
        !fixture.service.isOutcomeUncertain(lease.endpoint), 250);
    QTRY_COMPARE_WITH_TIMEOUT(
        fixture.service.state(), GuidedTargetService::State::Idle, 250);
    QCOMPARE(fixture.service.start(
                 &otherOwner, lease, {1.0, 1.0, 20.0}),
             GuidedTargetService::RequestResult::Started);
}

void GuidedTargetServiceTest::
stopPendingReleasesOwnerAndLateAckClearsIsolation()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject owner;
    GuidedTargetService::SessionToken session;
    QCOMPARE(fixture.service.start(
                 &owner, lease, kInitialTarget, &session),
             GuidedTargetService::RequestResult::Started);
    QCOMPARE(fixture.service.stop(session),
             GuidedTargetService::RequestResult::Stopped);
    QCOMPARE(fixture.service.state(),
             GuidedTargetService::State::OutcomeUncertain);
    QVERIFY(!fixture.service.hasActiveSession());
    QCOMPARE(fixture.service.submit(session, {1.0, 2.0, 30.0}),
             GuidedTargetService::RequestResult::InvalidSession);
    QCOMPARE(fixture.frames.size(), 1);
    fixture.acknowledge(MAV_RESULT_DENIED);
    QCOMPARE(fixture.service.state(), GuidedTargetService::State::Idle);
    QVERIFY(!fixture.service.isOutcomeUncertain(lease.endpoint));
}

void GuidedTargetServiceTest::staleTokenCannotStopANewerSession()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    QObject firstOwner;
    GuidedTargetService::SessionToken first;
    QCOMPARE(fixture.service.start(
                 &firstOwner, lease, kInitialTarget, &first),
             GuidedTargetService::RequestResult::Started);
    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.stop(first),
             GuidedTargetService::RequestResult::Stopped);

    QObject secondOwner;
    GuidedTargetService::SessionToken second;
    QCOMPARE(fixture.service.start(
                 &secondOwner, lease, {1.0, 1.0, 20.0}, &second),
             GuidedTargetService::RequestResult::Started);
    QVERIFY(second.generation > first.generation);
    QCOMPARE(fixture.service.stop(first),
             GuidedTargetService::RequestResult::InvalidSession);
    QCOMPARE(fixture.service.activeSession().generation,
             second.generation);
    QCOMPARE(fixture.service.state(),
             GuidedTargetService::State::AwaitingAcknowledgement);
}

void GuidedTargetServiceTest::targetChangeIsolationExpires()
{
    Fixture fixture;
    fixture.service.setRecoveryQuarantineForTesting(30);
    const VehicleEndpoint firstEndpoint = endpoint(9);
    const VehicleTargetLease first = fixture.selectFresh(firstEndpoint);
    fixture.targets.observeEndpoint(endpoint(10));
    QObject owner;
    QCOMPARE(fixture.service.start(&owner, first, kInitialTarget),
             GuidedTargetService::RequestResult::Started);
    QVERIFY(fixture.targets.selectTarget(10, 42, 1));
    QCOMPARE(fixture.service.state(),
             GuidedTargetService::State::OutcomeUncertain);
    QVERIFY(!fixture.service.hasActiveSession());
    QVERIFY(fixture.service.isOutcomeUncertain(firstEndpoint));

    QVERIFY(fixture.targets.selectTarget(9, 42, 1));
    const VehicleTargetLease selectedAgain = fixture.targets.acquireTarget();
    fixture.targets.observeHeartbeat(firstEndpoint, false,
                                     MAV_AUTOPILOT_ARDUPILOTMEGA,
                                     MAV_TYPE_QUADROTOR);
    QCOMPARE(fixture.service.start(&owner, selectedAgain, kInitialTarget),
             GuidedTargetService::RequestResult::OutcomeUncertain);
    QTRY_VERIFY_WITH_TIMEOUT(
        !fixture.service.isOutcomeUncertain(firstEndpoint), 200);
    QTRY_COMPARE_WITH_TIMEOUT(
        fixture.service.state(), GuidedTargetService::State::Idle, 200);
    QCOMPARE(fixture.service.start(&owner, selectedAgain, kInitialTarget),
             GuidedTargetService::RequestResult::Started);
}

void GuidedTargetServiceTest::awayAndBackRequiresANewHeartbeat()
{
    Fixture fixture;
    const VehicleEndpoint first = endpoint(9);
    fixture.selectFresh(first);
    fixture.targets.observeEndpoint(endpoint(10));
    QVERIFY(fixture.targets.selectTarget(10, 42, 1));
    QVERIFY(fixture.targets.selectTarget(9, 42, 1));
    const VehicleTargetLease newEpoch = fixture.targets.acquireTarget();
    QObject owner;
    QCOMPARE(fixture.service.start(&owner, newEpoch, kInitialTarget),
             GuidedTargetService::RequestResult::HeartbeatStale);
    fixture.targets.observeHeartbeat(first, false,
                                     MAV_AUTOPILOT_ARDUPILOTMEGA,
                                     MAV_TYPE_QUADROTOR);
    QCOMPARE(fixture.service.start(&owner, newEpoch, kInitialTarget),
             GuidedTargetService::RequestResult::Started);
}

void GuidedTargetServiceTest::ownerDestructionIsSafe()
{
    {
        Fixture fixture;
        const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
        QObject *owner = new QObject;
        QCOMPARE(fixture.service.start(owner, lease, kInitialTarget),
                 GuidedTargetService::RequestResult::Started);
        delete owner;
        QCOMPARE(fixture.service.state(),
                 GuidedTargetService::State::OutcomeUncertain);
        QVERIFY(!fixture.service.hasActiveSession());
        QVERIFY(fixture.service.isOutcomeUncertain(lease.endpoint));
        fixture.acknowledge(MAV_RESULT_ACCEPTED);
        QCOMPARE(fixture.service.state(), GuidedTargetService::State::Idle);
    }
    {
        Fixture fixture;
        const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
        QObject *owner = new QObject;
        QCOMPARE(fixture.service.start(owner, lease, kInitialTarget),
                 GuidedTargetService::RequestResult::Started);
        fixture.acknowledge(MAV_RESULT_ACCEPTED);
        delete owner;
        QCOMPARE(fixture.service.state(), GuidedTargetService::State::Idle);
        QVERIFY(!fixture.service.isOutcomeUncertain(lease.endpoint));
    }
}

void GuidedTargetServiceTest::transportFailureQuarantinesTheEndpoint()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectFresh(endpoint(9));
    fixture.writerSucceeds = false;
    QObject owner;
    QCOMPARE(fixture.service.start(&owner, lease, kInitialTarget),
             GuidedTargetService::RequestResult::TransportUnavailable);
    QVERIFY(!fixture.service.hasActiveSession());
    QVERIFY(fixture.service.isOutcomeUncertain(lease.endpoint));
    QCOMPARE(fixture.service.start(&owner, lease, kInitialTarget),
             GuidedTargetService::RequestResult::OutcomeUncertain);
    fixture.service.forgetLink(9);
    fixture.writerSucceeds = true;
    QCOMPARE(fixture.service.start(&owner, lease, kInitialTarget),
             GuidedTargetService::RequestResult::Started);
}

void GuidedTargetServiceTest::reentrantTargetChangeDuringWriteFailsUncertain()
{
    Fixture fixture;
    const VehicleEndpoint firstEndpoint = endpoint(9);
    const VehicleTargetLease first = fixture.selectFresh(firstEndpoint);
    fixture.targets.observeEndpoint(endpoint(10));
    fixture.writeHook = [&fixture]() {
        QVERIFY(fixture.targets.selectTarget(10, 42, 1));
    };
    QObject owner;
    GuidedTargetService::SessionToken token;
    QCOMPARE(fixture.service.start(
                 &owner, first, kInitialTarget, &token),
             GuidedTargetService::RequestResult::StaleTarget);
    QVERIFY(!token.isValid());
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(fixture.service.isOutcomeUncertain(firstEndpoint));
    QVERIFY(!fixture.service.hasActiveSession());
}

void GuidedTargetServiceTest::reentrantSessionEndedHandlerMayStartANewSession()
{
    Fixture fixture;
    const VehicleTargetLease first = fixture.selectFresh(endpoint(9));
    QObject firstOwner;
    GuidedTargetService::SessionToken firstSession;
    QCOMPARE(fixture.service.start(
                 &firstOwner, first, kInitialTarget, &firstSession),
             GuidedTargetService::RequestResult::Started);
    fixture.acknowledge(MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.submit(
                 firstSession, {0.5, 0.5, 20.0}),
             GuidedTargetService::RequestResult::Sent);

    QObject secondOwner;
    GuidedTargetService::SessionToken secondSession;
    GuidedTargetService::RequestResult nestedResult =
        GuidedTargetService::RequestResult::InvalidSession;
    QObject::connect(
        &fixture.service, &GuidedTargetService::sessionEnded,
        &fixture.service,
        [&](GuidedTargetService::SessionToken,
            GuidedTargetService::RequestResult, const QString &) {
            const VehicleTargetLease second =
                fixture.targets.acquireTarget();
            nestedResult = fixture.service.start(
                &secondOwner, second, {1.0, 2.0, 30.0}, &secondSession);
        });
    fixture.acknowledge(MAV_RESULT_DENIED);
    QCOMPARE(nestedResult, GuidedTargetService::RequestResult::Started);
    QVERIFY(secondSession.isValid());
    QCOMPARE(fixture.service.activeSession().generation,
             secondSession.generation);
    QCOMPARE(fixture.service.state(),
             GuidedTargetService::State::AwaitingAcknowledgement);
    QCOMPARE(fixture.frames.size(), 3);
}

QTEST_GUILESS_MAIN(GuidedTargetServiceTest)

#include "test_guidedtargetservice.moc"
