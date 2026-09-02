#include "comm/MAVLinkFrameParser.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"

#include <QtTest>

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
    int sourceSystem, int sourceComponent, MAV_CMD command,
    MAV_RESULT result, int targetSystem = 250, int targetComponent = 190,
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

} // namespace

class VehicleCommandServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void commandLongUsesOnlyTheExactSelectedEndpoint();
    void currentCommandUsesConfiguredIdentityAndExactTarget();
    void commandIntPreservesCoordinatePayload();
    void invalidStaleAndUnavailableTargetsFailClosed();
    void transmitVersionAndSequenceAreIndependentPerLink();
    void acknowledgementRequiresExactLeaseEnvelopeAndCommand();
    void targetChangeInvalidatesPendingAcknowledgement();
};

void VehicleCommandServiceTest::commandLongUsesOnlyTheExactSelectedEndpoint()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);

    QVERIFY(targets.observeEndpoint(endpoint(4, 42, 1), true));
    QVERIFY(targets.observeEndpoint(endpoint(9, 42, 100)));
    QVERIFY(targets.selectTarget(9, 42, 100));
    const VehicleTargetLease lease = targets.acquireTarget();

    QCOMPARE(service.sendCommandLong(
                 lease, 250, 190, MAV_CMD_DO_CHANGE_SPEED, 3,
                 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F),
             VehicleCommandService::SendResult::Sent);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().linkId, 9);

    const mavlink_message_t message = decodeFrame(frames.first().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_COMMAND_LONG));
    QCOMPARE(message.sysid, quint8(250));
    QCOMPARE(message.compid, quint8(190));
    mavlink_command_long_t payload{};
    mavlink_msg_command_long_decode(&message, &payload);
    QCOMPARE(payload.target_system, quint8(42));
    QCOMPARE(payload.target_component, quint8(100));
    QCOMPARE(payload.command, quint16(MAV_CMD_DO_CHANGE_SPEED));
    QCOMPARE(payload.confirmation, quint8(3));
    QCOMPARE(payload.param1, 1.0F);
    QCOMPARE(payload.param7, 7.0F);
}

void VehicleCommandServiceTest::currentCommandUsesConfiguredIdentityAndExactTarget()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(251, 190);
    QVERIFY(targets.observeEndpoint(endpoint(17, 77, 42), true));

    QCOMPARE(service.sendCurrentCommandLong(
                 MAV_CMD_PREFLIGHT_STORAGE, 0,
                 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F),
             int(VehicleCommandService::SendResult::Sent));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.constFirst().linkId, 17);
    const mavlink_message_t message = decodeFrame(frames.constFirst().bytes);
    QCOMPARE(message.sysid, quint8(251));
    QCOMPARE(message.compid, quint8(190));
    mavlink_command_long_t payload{};
    mavlink_msg_command_long_decode(&message, &payload);
    QCOMPARE(payload.target_system, quint8(77));
    QCOMPARE(payload.target_component, quint8(42));
    QCOMPARE(payload.command, quint16(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(payload.param1, 1.0F);
}

void VehicleCommandServiceTest::commandIntPreservesCoordinatePayload()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(7, 71, 154), true));

    QCOMPARE(service.sendCommandInt(
                 targets.acquireTarget(), 251, 190,
                 MAV_CMD_DO_SET_ROI_LOCATION,
                 MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
                 1.0F, 2.0F, 3.0F, 4.0F,
                 -353632610, 1491652300, 85.5F, 1, 1),
             VehicleCommandService::SendResult::Sent);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().linkId, 7);

    const mavlink_message_t message = decodeFrame(frames.first().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_COMMAND_INT));
    mavlink_command_int_t payload{};
    mavlink_msg_command_int_decode(&message, &payload);
    QCOMPARE(payload.target_system, quint8(71));
    QCOMPARE(payload.target_component, quint8(154));
    QCOMPARE(payload.frame,
             quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT_INT));
    QCOMPARE(payload.x, qint32(-353632610));
    QCOMPARE(payload.y, qint32(1491652300));
    QCOMPARE(payload.z, 85.5F);
    QCOMPARE(payload.current, quint8(1));
    QCOMPARE(payload.autocontinue, quint8(1));
}

void VehicleCommandServiceTest::invalidStaleAndUnavailableTargetsFailClosed()
{
    VehicleTargetManager targets;
    int writes = 0;
    bool writerSucceeds = true;
    ExactLinkTransmitter transmitter(
        [&writes, &writerSucceeds](int, const QByteArray &) {
            ++writes;
            return writerSucceeds;
        });
    VehicleCommandService service(&targets, &transmitter);

    QCOMPARE(service.sendCommandLong(
                 {}, 250, 190, MAV_CMD_NAV_RETURN_TO_LAUNCH, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::InvalidTarget);
    QCOMPARE(writes, 0);

    QVERIFY(targets.observeEndpoint(endpoint(1), true));
    QVERIFY(targets.observeEndpoint(endpoint(2)));
    const VehicleTargetLease stale = targets.acquireTarget();
    QVERIFY(targets.selectTarget(2, 42, 1));
    QCOMPARE(service.sendCommandLong(
                 stale, 250, 190, MAV_CMD_NAV_RETURN_TO_LAUNCH, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::StaleTarget);
    QCOMPARE(writes, 0);

    writerSucceeds = false;
    QCOMPARE(service.sendCommandLong(
                 targets.acquireTarget(), 250, 190,
                 MAV_CMD_NAV_RETURN_TO_LAUNCH, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::TransportUnavailable);
    QCOMPARE(writes, 1);

    QSignalSpy acknowledgements(
        &service, &VehicleCommandService::commandAckReceived);
    service.observeMessage(
        2, commandAck(42, 1, MAV_CMD_NAV_RETURN_TO_LAUNCH,
                      MAV_RESULT_ACCEPTED));
    QCOMPARE(acknowledgements.count(), 0);
}

void VehicleCommandServiceTest::transmitVersionAndSequenceAreIndependentPerLink()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(11), true));
    QVERIFY(targets.observeEndpoint(endpoint(22)));
    transmitter.setOutboundVersion(11, 1);
    transmitter.setOutboundVersion(22, 2);

    const auto send = [&service, &targets](MAV_CMD command) {
        return service.sendCommandLong(
            targets.acquireTarget(), 250, 190, command, 0,
            0, 0, 0, 0, 0, 0, 0);
    };
    QCOMPARE(send(MAV_CMD_NAV_RETURN_TO_LAUNCH),
             VehicleCommandService::SendResult::Sent);
    QVERIFY(targets.selectTarget(22, 42, 1));
    QCOMPARE(send(MAV_CMD_MISSION_START),
             VehicleCommandService::SendResult::Sent);
    QVERIFY(targets.selectTarget(11, 42, 1));
    QCOMPARE(send(MAV_CMD_DO_CHANGE_SPEED),
             VehicleCommandService::SendResult::Sent);

    QCOMPARE(frames.size(), 3);
    const mavlink_message_t first = decodeFrame(frames.at(0).bytes);
    const mavlink_message_t second = decodeFrame(frames.at(1).bytes);
    const mavlink_message_t third = decodeFrame(frames.at(2).bytes);
    QCOMPARE(first.magic, quint8(MAVLINK_STX_MAVLINK1));
    QCOMPARE(second.magic, quint8(MAVLINK_STX));
    QCOMPARE(third.magic, quint8(MAVLINK_STX_MAVLINK1));
    QCOMPARE(first.seq, quint8(0));
    QCOMPARE(second.seq, quint8(0));
    QCOMPARE(third.seq, quint8(1));

    service.forgetLink(11);
    transmitter.forgetLink(11);
    transmitter.setOutboundVersion(11, 2);
    QCOMPARE(send(MAV_CMD_DO_GO_AROUND),
             VehicleCommandService::SendResult::Sent);
    const mavlink_message_t reset = decodeFrame(frames.last().bytes);
    QCOMPARE(reset.magic, quint8(MAVLINK_STX));
    QCOMPARE(reset.seq, quint8(0));
}

void VehicleCommandServiceTest::acknowledgementRequiresExactLeaseEnvelopeAndCommand()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    VehicleCommandService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(9, 42, 100), true));
    const VehicleTargetLease lease = targets.acquireTarget();
    QCOMPARE(service.sendCommandLong(
                 lease, 250, 190, MAV_CMD_DO_CHANGE_SPEED, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::Sent);

    QSignalSpy acknowledgements(
        &service, &VehicleCommandService::commandAckReceived);
    service.observeMessage(
        8, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED));
    service.observeMessage(
        9, commandAck(41, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED));
    service.observeMessage(
        9, commandAck(42, 1, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED));
    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_MISSION_START,
                      MAV_RESULT_ACCEPTED));
    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED, 249, 190));
    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED, 250, 191));
    QCOMPARE(acknowledgements.count(), 0);

    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_IN_PROGRESS, 250, 190, 37));
    QCOMPARE(acknowledgements.count(), 1);
    QCOMPARE(acknowledgements.first().at(0).toULongLong(),
             lease.generation);
    QCOMPARE(acknowledgements.first().at(1).toInt(), 9);
    QCOMPARE(acknowledgements.first().at(2).toInt(), 42);
    QCOMPARE(acknowledgements.first().at(3).toInt(), 100);
    QCOMPARE(acknowledgements.first().at(4).toInt(),
             int(MAV_CMD_DO_CHANGE_SPEED));
    QCOMPARE(acknowledgements.first().at(6).toInt(), 37);

    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED, 0, 0));
    QCOMPARE(acknowledgements.count(), 2);
    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED, 0, 0));
    QCOMPARE(acknowledgements.count(), 2);
}

void VehicleCommandServiceTest::targetChangeInvalidatesPendingAcknowledgement()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    VehicleCommandService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    QVERIFY(targets.observeEndpoint(endpoint(4)));
    const VehicleTargetLease oldTarget = targets.acquireTarget();
    QCOMPARE(service.sendCommandLong(
                 oldTarget, 250, 190, MAV_CMD_MISSION_START, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::Sent);

    QVERIFY(targets.selectTarget(4, 42, 1));
    QVERIFY(targets.selectTarget(3, 42, 1));
    QSignalSpy acknowledgements(
        &service, &VehicleCommandService::commandAckReceived);
    service.observeMessage(
        3, commandAck(42, 1, MAV_CMD_MISSION_START,
                      MAV_RESULT_ACCEPTED));
    QCOMPARE(acknowledgements.count(), 0);
}

QTEST_APPLESS_MAIN(VehicleCommandServiceTest)

#include "test_vehiclecommandservice.moc"
