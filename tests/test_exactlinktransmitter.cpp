#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"

#include <QtTest>

#include <QPointer>

namespace
{

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

mavlink_message_t commandMessage(MAV_CMD command)
{
    mavlink_command_long_t payload{};
    payload.target_system = 42;
    payload.target_component = 1;
    payload.command = static_cast<quint16>(command);
    mavlink_message_t message{};
    mavlink_msg_command_long_encode(250, 190, &message, &payload);
    return message;
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

} // namespace

class ExactLinkTransmitterTest final : public QObject
{
    Q_OBJECT

private slots:
    void sequenceAndVersionBelongToEachLink();
    void targetedCommandAckCapabilityTracksLinkVersion();
    void prefinalizedV1ExtensionsAreRejectedButTypedAckIsExact();
    void invalidAndV2OnlyMessagesDoNotConsumeV1Sequence();
    void writerFailureConsumesSequence();
    void forgettingLinkResetsSequenceAndVersion();
    void successfulSubmissionReportsFinalizedMessageAndEpoch();
    void failedZeroAndForgottenEpochsDoNotReportSubmission();
    void reentrantEpochChangesNeverRelabelSubmission();
    void reentrantDestructionDuringWriteIsSafe();
};

void ExactLinkTransmitterTest::targetedCommandAckCapabilityTracksLinkVersion()
{
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });

    QCOMPARE(transmitter.outboundVersion(17), 2U);
    QVERIFY(transmitter.supportsTargetedCommandAck(17));
    transmitter.setOutboundVersion(17, 1);
    QCOMPARE(transmitter.outboundVersion(17), 1U);
    QVERIFY(!transmitter.supportsTargetedCommandAck(17));
    transmitter.setOutboundVersion(17, 2);
    QVERIFY(transmitter.supportsTargetedCommandAck(17));
    transmitter.forgetLink(17);
    QCOMPARE(transmitter.outboundVersion(17), 2U);
}

void ExactLinkTransmitterTest::prefinalizedV1ExtensionsAreRejectedButTypedAckIsExact()
{
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    transmitter.setOutboundVersion(22, 2);

    mavlink_status_t *const global =
        mavlink_get_channel_status(MAVLINK_COMM_0);
    const quint8 previousFlags = global->flags;
    global->flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_message_t prematurelyFinalized{};
    mavlink_msg_command_ack_pack(
        250, MAV_COMP_ID_MISSIONPLANNER, &prematurelyFinalized,
        MAV_CMD_PREFLIGHT_CALIBRATION, MAV_RESULT_ACCEPTED,
        255, 0, 42, 1);
    global->flags = previousFlags;
    QCOMPARE(prematurelyFinalized.len,
             quint8(MAVLINK_MSG_ID_COMMAND_ACK_MIN_LEN));

    QCOMPARE(transmitter.sendMessage(
                 22, 250, MAV_COMP_ID_MISSIONPLANNER,
                 prematurelyFinalized),
             ExactLinkTransmitter::SendResult::IncompatibleVersion);
    QCOMPARE(frames.size(), 0);

    QCOMPARE(transmitter.sendCommandAck(
                 22, 250, MAV_COMP_ID_MISSIONPLANNER,
                 MAV_CMD_PREFLIGHT_CALIBRATION, MAV_RESULT_ACCEPTED,
                 77, 1, 63, -123456),
             ExactLinkTransmitter::SendResult::Sent);
    const mavlink_message_t typedDecoded = decodeFrame(frames.first().bytes);
    mavlink_command_ack_t acknowledgement{};
    mavlink_msg_command_ack_decode(
        &typedDecoded, &acknowledgement);
    QCOMPARE(typedDecoded.magic, quint8(MAVLINK_STX));
    QCOMPARE(acknowledgement.target_system, quint8(77));
    QCOMPARE(acknowledgement.target_component, quint8(1));
    QCOMPARE(acknowledgement.progress, quint8(63));
    QCOMPARE(acknowledgement.result_param2, qint32(-123456));
}

void ExactLinkTransmitterTest::sequenceAndVersionBelongToEachLink()
{
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    transmitter.setOutboundVersion(11, 1);
    transmitter.setOutboundVersion(22, 2);

    QCOMPARE(transmitter.sendMessage(
                 11, 250, 190,
                 commandMessage(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(transmitter.sendMessage(
                 22, 250, 190, commandMessage(MAV_CMD_MISSION_START)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(transmitter.sendMessage(
                 11, 250, 190, commandMessage(MAV_CMD_DO_CHANGE_SPEED)),
             ExactLinkTransmitter::SendResult::Sent);

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
}

void ExactLinkTransmitterTest::invalidAndV2OnlyMessagesDoNotConsumeV1Sequence()
{
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    transmitter.setOutboundVersion(7, 1);

    mavlink_message_t unknown{};
    unknown.msgid = 0x00ffffffU;
    bool frameWriterInvoked = true;
    QCOMPARE(transmitter.sendMessage(
                 7, 250, 190, unknown, &frameWriterInvoked),
             ExactLinkTransmitter::SendResult::InvalidMessage);
    QVERIFY(!frameWriterInvoked);

    mavlink_message_t v2Only{};
    mavlink_msg_param_ext_request_list_pack(
        250, 190, &v2Only, 42, 1);
    frameWriterInvoked = true;
    QCOMPARE(transmitter.sendMessage(
                 7, 250, 190, v2Only, &frameWriterInvoked),
             ExactLinkTransmitter::SendResult::IncompatibleVersion);
    QVERIFY(!frameWriterInvoked);
    QCOMPARE(frames.size(), 0);

    QCOMPARE(transmitter.sendMessage(
                 7, 250, 190,
                 commandMessage(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(decodeFrame(frames.first().bytes).seq, quint8(0));
}

void ExactLinkTransmitterTest::writerFailureConsumesSequence()
{
    QVector<CapturedFrame> frames;
    bool succeed = false;
    ExactLinkTransmitter transmitter(
        [&frames, &succeed](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return succeed;
        });

    bool frameWriterInvoked = false;
    QCOMPARE(transmitter.sendMessage(
                 3, 250, 190, commandMessage(MAV_CMD_MISSION_START),
                 &frameWriterInvoked),
             ExactLinkTransmitter::SendResult::TransportUnavailable);
    QVERIFY(frameWriterInvoked);
    succeed = true;
    frameWriterInvoked = false;
    QCOMPARE(transmitter.sendMessage(
                 3, 250, 190, commandMessage(MAV_CMD_DO_CHANGE_SPEED),
                 &frameWriterInvoked),
             ExactLinkTransmitter::SendResult::Sent);
    QVERIFY(frameWriterInvoked);

    QCOMPARE(frames.size(), 2);
    QCOMPARE(decodeFrame(frames.at(0).bytes).seq, quint8(0));
    QCOMPARE(decodeFrame(frames.at(1).bytes).seq, quint8(1));
}

void ExactLinkTransmitterTest::forgettingLinkResetsSequenceAndVersion()
{
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    transmitter.setOutboundVersion(5, 1);
    QCOMPARE(transmitter.sendMessage(
                 5, 250, 190, commandMessage(MAV_CMD_MISSION_START)),
             ExactLinkTransmitter::SendResult::Sent);

    transmitter.forgetLink(5);
    QCOMPARE(transmitter.sendMessage(
                 5, 250, 190, commandMessage(MAV_CMD_DO_CHANGE_SPEED)),
             ExactLinkTransmitter::SendResult::Sent);

    QCOMPARE(decodeFrame(frames.at(0).bytes).magic,
             quint8(MAVLINK_STX_MAVLINK1));
    const mavlink_message_t reset = decodeFrame(frames.at(1).bytes);
    QCOMPARE(reset.magic, quint8(MAVLINK_STX));
    QCOMPARE(reset.seq, quint8(0));
}

void ExactLinkTransmitterTest::
successfulSubmissionReportsFinalizedMessageAndEpoch()
{
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    transmitter.setLinkSessionEpoch(37, 9001);

    int submittedCount = 0;
    int submittedLinkId = -1;
    quint64 submittedEpoch = 0;
    mavlink_message_t submittedMessage{};
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted,
            this,
            [&submittedCount, &submittedLinkId, &submittedEpoch,
             &submittedMessage](int linkId, quint64 epoch,
                                mavlink_message_t message) {
                ++submittedCount;
                submittedLinkId = linkId;
                submittedEpoch = epoch;
                submittedMessage = message;
            });

    const mavlink_message_t input =
        commandMessage(MAV_CMD_NAV_RETURN_TO_LAUNCH);
    QCOMPARE(transmitter.sendMessage(37, 91, 192, input),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(submittedCount, 1);
    QCOMPARE(submittedLinkId, 37);
    QCOMPARE(submittedEpoch, quint64(9001));
    QCOMPARE(submittedMessage.sysid, quint8(91));
    QCOMPARE(submittedMessage.compid, quint8(192));
    QCOMPARE(submittedMessage.seq, quint8(0));
    QCOMPARE(submittedMessage.magic, quint8(MAVLINK_STX));
    QCOMPARE(submittedMessage.msgid,
             quint32(MAVLINK_MSG_ID_COMMAND_LONG));

    QCOMPARE(frames.size(), 1);
    const mavlink_message_t decoded = decodeFrame(frames.constFirst().bytes);
    QCOMPARE(decoded.sysid, submittedMessage.sysid);
    QCOMPARE(decoded.compid, submittedMessage.compid);
    QCOMPARE(decoded.seq, submittedMessage.seq);
    QCOMPARE(decoded.checksum, submittedMessage.checksum);
}

void ExactLinkTransmitterTest::
failedZeroAndForgottenEpochsDoNotReportSubmission()
{
    bool writerSucceeds = false;
    ExactLinkTransmitter transmitter(
        [&writerSucceeds](int, const QByteArray &) {
            return writerSucceeds;
        });
    int submittedCount = 0;
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted,
            this, [&submittedCount](int, quint64, mavlink_message_t) {
                ++submittedCount;
            });

    transmitter.setLinkSessionEpoch(4, 100);
    QCOMPARE(transmitter.sendMessage(
                 4, 250, 190, commandMessage(MAV_CMD_MISSION_START)),
             ExactLinkTransmitter::SendResult::TransportUnavailable);
    QCOMPARE(submittedCount, 0);

    writerSucceeds = true;
    transmitter.setLinkSessionEpoch(4, 0);
    QCOMPARE(transmitter.sendMessage(
                 4, 250, 190, commandMessage(MAV_CMD_MISSION_START)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(submittedCount, 0);

    transmitter.setLinkSessionEpoch(4, 101);
    transmitter.forgetLink(4);
    QCOMPARE(transmitter.sendMessage(
                 4, 250, 190, commandMessage(MAV_CMD_MISSION_START)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(submittedCount, 0);
}

void ExactLinkTransmitterTest::
reentrantEpochChangesNeverRelabelSubmission()
{
    enum class WriterAction {
        RollEpoch,
        ForgetLink,
        Stable
    };
    WriterAction action = WriterAction::RollEpoch;
    ExactLinkTransmitter *transmitterPointer = nullptr;
    ExactLinkTransmitter transmitter(
        [&action, &transmitterPointer](int linkId, const QByteArray &) {
            if (action == WriterAction::RollEpoch) {
                transmitterPointer->setLinkSessionEpoch(linkId, 202);
            } else if (action == WriterAction::ForgetLink) {
                transmitterPointer->forgetLink(linkId);
            }
            return true;
        });
    transmitterPointer = &transmitter;
    QVector<quint64> submittedEpochs;
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted,
            this,
            [&submittedEpochs](int, quint64 epoch, mavlink_message_t) {
                submittedEpochs.append(epoch);
            });

    transmitter.setLinkSessionEpoch(8, 201);
    QCOMPARE(transmitter.sendMessage(
                 8, 250, 190, commandMessage(MAV_CMD_MISSION_START)),
             ExactLinkTransmitter::SendResult::Sent);
    QVERIFY(submittedEpochs.isEmpty());

    action = WriterAction::Stable;
    QCOMPARE(transmitter.sendMessage(
                 8, 250, 190, commandMessage(MAV_CMD_MISSION_START)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(submittedEpochs, QVector<quint64>({202}));

    action = WriterAction::ForgetLink;
    QCOMPARE(transmitter.sendMessage(
                 8, 250, 190, commandMessage(MAV_CMD_MISSION_START)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(submittedEpochs, QVector<quint64>({202}));
}

void ExactLinkTransmitterTest::reentrantDestructionDuringWriteIsSafe()
{
    ExactLinkTransmitter *transmitter = nullptr;
    QPointer<ExactLinkTransmitter> guardedTransmitter;
    transmitter = new ExactLinkTransmitter(
        [&transmitter](int, const QByteArray &) {
            delete transmitter;
            transmitter = nullptr;
            return true;
        });
    guardedTransmitter = transmitter;
    int submittedCount = 0;
    connect(transmitter, &ExactLinkTransmitter::messageSubmitted,
            this, [&submittedCount](int, quint64, mavlink_message_t) {
                ++submittedCount;
            });
    transmitter->setLinkSessionEpoch(12, 300);

    const ExactLinkTransmitter::SendResult result =
        transmitter->sendMessage(
            12, 250, 190, commandMessage(MAV_CMD_MISSION_START));
    QCOMPARE(result, ExactLinkTransmitter::SendResult::Sent);
    QVERIFY(!transmitter);
    QVERIFY(guardedTransmitter.isNull());
    QCOMPARE(submittedCount, 0);
}

QTEST_APPLESS_MAIN(ExactLinkTransmitterTest)

#include "test_exactlinktransmitter.moc"
