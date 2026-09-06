#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"

#include <QtTest>

#include <QPointer>

#include <cstring>

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

QByteArray frameBytes(const mavlink_message_t &message)
{
    uint8_t bytes[MAVLINK_MAX_PACKET_LEN]{};
    const auto length = mavlink_msg_to_send_buffer(bytes, &message);
    return QByteArray(reinterpret_cast<const char *>(bytes), length);
}

mavlink_signing_t signingFixture()
{
    mavlink_signing_t signing{};
    signing.flags = MAVLINK_SIGNING_FLAG_SIGN_OUTGOING;
    signing.link_id = 9;
    signing.timestamp = 0x102030405060ULL;
    std::memset(signing.secret_key, 0x5a, sizeof(signing.secret_key));
    return signing;
}

// Sign the supplied v2 header/payload in place, without re-finalizing or changing
// its sequence/zero-truncation. Also supports deliberate payload mutation tests.
QByteArray attachSignature(QByteArray frame)
{
    if (frame.size() < MAVLINK_NUM_NON_PAYLOAD_BYTES
        || quint8(frame[0]) != MAVLINK_STX
        || frame.size() != quint8(frame[1]) + MAVLINK_NUM_NON_PAYLOAD_BYTES)
        return {};
    const quint32 messageId = quint8(frame[7]) | (quint32(quint8(frame[8])) << 8)
        | (quint32(quint8(frame[9])) << 16);
    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(messageId);
    if (!entry) return {};
    frame[2] = char(MAVLINK_IFLAG_SIGNED);
    const auto *bytes = reinterpret_cast<const uint8_t *>(frame.constData());
    quint16 crc = crc_calculate(bytes + 1, frame.size() - 3);
    crc_accumulate(entry->crc_extra, &crc);
    frame[frame.size() - 2] = char(crc & 255);
    frame[frame.size() - 1] = char(crc >> 8);
    bytes = reinterpret_cast<const uint8_t *>(frame.constData());
    mavlink_signing_t signing = signingFixture();
    uint8_t signature[MAVLINK_SIGNATURE_BLOCK_LEN]{};
    if (mavlink_sign_packet(&signing, signature, bytes, MAVLINK_NUM_HEADER_BYTES,
                           bytes + MAVLINK_NUM_HEADER_BYTES, quint8(frame[1]),
                           bytes + frame.size() - 2) != MAVLINK_SIGNATURE_BLOCK_LEN)
        return {};
    frame.append(reinterpret_cast<const char *>(signature), sizeof(signature));
    return frame;
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
    void signingFailuresNeverInvokeWriter_data();
    void signingFailuresNeverInvokeWriter();
    void signingRequirementPinsV2AndSurvivesForget();
    void signedSubmissionContainsActualWireSignature();
    void setupSigningNeverPublishesKeyMaterial();
    void remoteLogControlRequiresExactSessionOwner();
    void serialControlRequiresDedicatedSessionOwner();
    void reentrantSignerChangesAbortBeforeWrite_data();
    void reentrantSignerChangesAbortBeforeWrite();
    void reentrantSignerDestructionIsSafe();
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

void ExactLinkTransmitterTest::signingFailuresNeverInvokeWriter_data()
{
    QTest::addColumn<QString>("scenario");
    for (const char *scenario : {"no-hook", "failure", "unsigned", "empty", "truncated",
                                 "extra-byte", "invalid-crc", "retarget", "resequence"})
        QTest::newRow(scenario) << QString::fromLatin1(scenario);
}

void ExactLinkTransmitterTest::signingFailuresNeverInvokeWriter()
{
    QFETCH(QString, scenario);
    int writes = 0;
    int submitted = 0;
    ExactLinkTransmitter transmitter([&writes](int, const QByteArray &) {
        ++writes;
        return true;
    });
    transmitter.setLinkSessionEpoch(11, 123);
    transmitter.setSigningRequired(11, true);
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted, this,
            [&submitted](int, quint64, mavlink_message_t) { ++submitted; });
    if (scenario != QStringLiteral("no-hook")) {
        transmitter.setFrameSigner([scenario](int, const QByteArray &frame, QByteArray *out) {
            if (scenario == QStringLiteral("failure")) return false;
            if (scenario == QStringLiteral("unsigned")) { *out = frame; return true; }
            if (scenario == QStringLiteral("empty")) { out->clear(); return true; }
            QByteArray toSign = frame;
            if (scenario == QStringLiteral("retarget"))
                toSign[MAVLINK_NUM_HEADER_BYTES + 30] = char(43); // COMMAND_LONG target_system
            if (scenario == QStringLiteral("resequence"))
                toSign[4] = char(quint8(toSign[4]) + 1);
            *out = attachSignature(toSign);
            if (scenario == QStringLiteral("truncated")) out->chop(1);
            if (scenario == QStringLiteral("extra-byte")) out->append('\0');
            if (scenario == QStringLiteral("invalid-crc"))
                (*out)[frame.size() - 1] = char(quint8((*out)[frame.size() - 1]) ^ 1);
            return true;
        });
    }
    bool writerInvoked = true;
    QCOMPARE(transmitter.sendMessage(11, 250, 190, commandMessage(MAV_CMD_MISSION_START),
                                     &writerInvoked),
             ExactLinkTransmitter::SendResult::SigningUnavailable);
    QVERIFY(!writerInvoked);
    QCOMPARE(writes, 0);
    QCOMPARE(submitted, 0);
}

void ExactLinkTransmitterTest::signingRequirementPinsV2AndSurvivesForget()
{
    int writes = 0;
    ExactLinkTransmitter transmitter([&writes](int, const QByteArray &) {
        ++writes;
        return true;
    });
    transmitter.setOutboundVersion(11, 1);
    QCOMPARE(transmitter.outboundVersion(11), 1U);
    transmitter.setSigningRequired(11, true);
    QCOMPARE(transmitter.outboundVersion(11), 2U);
    transmitter.setOutboundVersion(11, 1);
    QCOMPARE(transmitter.outboundVersion(11), 2U);
    transmitter.forgetLink(11);
    transmitter.setOutboundVersion(11, 1);
    QCOMPARE(transmitter.outboundVersion(11), 2U);
    bool invoked = true;
    QCOMPARE(transmitter.sendMessage(11, 250, 190, commandMessage(MAV_CMD_MISSION_START), &invoked),
             ExactLinkTransmitter::SendResult::SigningUnavailable);
    QVERIFY(!invoked);
    QCOMPARE(writes, 0);

    // The policy is link-local; explicitly clearing it is required to permit v1.
    transmitter.setOutboundVersion(12, 1);
    QCOMPARE(transmitter.outboundVersion(12), 1U);
    transmitter.setSigningRequired(11, false);
    transmitter.setOutboundVersion(11, 1);
    QCOMPARE(transmitter.outboundVersion(11), 1U);
    QCOMPARE(transmitter.sendMessage(11, 250, 190, commandMessage(MAV_CMD_MISSION_START)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(writes, 1);
}

void ExactLinkTransmitterTest::signedSubmissionContainsActualWireSignature()
{
    QByteArray written;
    QByteArray unsignedFrame;
    QStringList order;
    ExactLinkTransmitter transmitter([&](int linkId, const QByteArray &frame) {
        if (linkId != 11) return false;
        order.append(QStringLiteral("writer"));
        written = frame;
        return true;
    });
    transmitter.setLinkSessionEpoch(11, 456);
    transmitter.setSigningRequired(11, true);
    transmitter.setFrameSigner([&](int linkId, const QByteArray &frame, QByteArray *out) {
        if (linkId != 11) return false;
        order.append(QStringLiteral("signer"));
        unsignedFrame = frame;
        *out = attachSignature(frame);
        return !out->isEmpty();
    });
    int submitted = 0;
    mavlink_message_t published{};
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted, this,
            [&](int linkId, quint64 epoch, mavlink_message_t message) {
                QCOMPARE(linkId, 11);
                QCOMPARE(epoch, quint64(456));
                order.append(QStringLiteral("published"));
                ++submitted;
                published = message;
            });
    bool invoked = false;
    QCOMPARE(transmitter.sendMessage(11, 91, 192, commandMessage(MAV_CMD_MISSION_START), &invoked),
             ExactLinkTransmitter::SendResult::Sent);
    QVERIFY(invoked);
    QCOMPARE(submitted, 1);
    QCOMPARE(order, (QStringList{QStringLiteral("signer"), QStringLiteral("writer"),
                                 QStringLiteral("published")}));
    QCOMPARE(written.size(), unsignedFrame.size() + MAVLINK_SIGNATURE_BLOCK_LEN);
    QCOMPARE(written, attachSignature(unsignedFrame));
    QCOMPARE(published.incompat_flags, quint8(MAVLINK_IFLAG_SIGNED));
    QCOMPARE(published.sysid, quint8(91));
    QCOMPARE(published.compid, quint8(192));
    QCOMPARE(published.seq, quint8(0));
    QCOMPARE(frameBytes(published), written);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(published.signature),
                        MAVLINK_SIGNATURE_BLOCK_LEN), written.right(MAVLINK_SIGNATURE_BLOCK_LEN));
    const mavlink_message_t decoded = decodeFrame(written);
    QCOMPARE(frameBytes(decoded), written);
    mavlink_signing_t signing = signingFixture();
    mavlink_signing_streams_t streams{};
    QVERIFY(mavlink_signature_check(&signing, &streams, &published));
}

void ExactLinkTransmitterTest::remoteLogControlRequiresExactSessionOwner()
{
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter([&](int link, const QByteArray &frame) {
        frames.append({link, frame});
        return true;
    });
    transmitter.setLinkSessionEpoch(11, 7);
    int submitted = 0;
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted,
            &transmitter, [&](int, quint64, mavlink_message_t) { ++submitted; });
    for (quint32 sequence : {quint32(MAV_REMOTE_LOG_DATA_BLOCK_START),
                            quint32(MAV_REMOTE_LOG_DATA_BLOCK_STOP), quint32(0)}) {
        mavlink_message_t message{};
        mavlink_msg_remote_log_block_status_pack(250, 190, &message,
            42, 1, sequence, MAV_REMOTE_LOG_DATA_BLOCK_ACK);
        bool attempted = true;
        QCOMPARE(transmitter.sendMessage(11, 250, 190, message, &attempted),
                 ExactLinkTransmitter::SendResult::RestrictedMessage);
        QVERIFY(!attempted);
    }
    QVERIFY(frames.isEmpty());
    QCOMPARE(submitted, 0);
    QCOMPARE(transmitter.sendMessage(11, 250, 190,
                 commandMessage(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(decodeFrame(frames.first().bytes).seq, quint8(0));
}

void ExactLinkTransmitterTest::serialControlRequiresDedicatedSessionOwner()
{
    int writes = 0;
    ExactLinkTransmitter transmitter([&](int, const QByteArray &) {
        ++writes;
        return true;
    });
    transmitter.setLinkSessionEpoch(11, 7);
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_SERIAL_CONTROL;
    message.len = MAVLINK_MSG_ID_SERIAL_CONTROL_LEN;
    bool attempted = true;
    QCOMPARE(transmitter.sendMessage(11, 250, 190, message, &attempted),
             ExactLinkTransmitter::SendResult::RestrictedMessage);
    QVERIFY(!attempted);
    QCOMPARE(writes, 0);
}

void ExactLinkTransmitterTest::setupSigningNeverPublishesKeyMaterial()
{
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter([&frames](int linkId, const QByteArray &frame) {
        frames.append({linkId, frame});
        return true;
    });
    transmitter.setLinkSessionEpoch(11, 456);
    int submitted = 0;
    int signerCalls = 0;
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted, this,
            [&submitted](int, quint64, mavlink_message_t) { ++submitted; });
    uint8_t secret[32];
    std::memset(secret, 0x5a, sizeof(secret));
    mavlink_message_t setup{};
    mavlink_msg_setup_signing_pack(250, 190, &setup, 42, 1, secret, 123456789);
    for (const bool protectedLink : {false, true}) {
        if (protectedLink) {
            transmitter.setSigningRequired(11, true);
            transmitter.setFrameSigner([&signerCalls](
                    int, const QByteArray &frame, QByteArray *out) {
                ++signerCalls;
                *out = attachSignature(frame);
                return !out->isEmpty();
            });
        }
        bool invoked = true;
        QCOMPARE(transmitter.sendMessage(11, 250, 190, setup, &invoked),
                 ExactLinkTransmitter::SendResult::RestrictedMessage);
        QVERIFY(!invoked);
    }
    QCOMPARE(frames.size(), 0);
    QCOMPARE(signerCalls, 0);
    QCOMPARE(submitted, 0);

    // A rejected secret must not consume the physical link's sequence. Only
    // LinkManager's private provisioning primitive may put SETUP_SIGNING on
    // the wire.
    transmitter.setSigningRequired(11, false);
    transmitter.setFrameSigner({});
    QCOMPARE(transmitter.sendMessage(
                 11, 250, 190,
                 commandMessage(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             ExactLinkTransmitter::SendResult::Sent);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(decodeFrame(frames.first().bytes).seq, quint8(0));
    QCOMPARE(submitted, 1);
}

void ExactLinkTransmitterTest::reentrantSignerChangesAbortBeforeWrite_data()
{
    QTest::addColumn<bool>("replaceSigner");
    QTest::newRow("forgotten-epoch") << false;
    QTest::newRow("replaced-signer") << true;
}

void ExactLinkTransmitterTest::reentrantSignerChangesAbortBeforeWrite()
{
    QFETCH(bool, replaceSigner);
    int writes = 0;
    int submitted = 0;
    ExactLinkTransmitter transmitter([&writes](int, const QByteArray &) {
        ++writes;
        return true;
    });
    transmitter.setLinkSessionEpoch(11, 456);
    transmitter.setSigningRequired(11, true);
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted, this,
            [&submitted](int, quint64, mavlink_message_t) { ++submitted; });
    transmitter.setFrameSigner([&](int linkId, const QByteArray &frame, QByteArray *out) {
        *out = attachSignature(frame);
        if (replaceSigner)
            transmitter.setFrameSigner([](int, const QByteArray &, QByteArray *) { return false; });
        else
            transmitter.forgetLink(linkId);
        return true;
    });
    bool invoked = true;
    QCOMPARE(transmitter.sendMessage(11, 250, 190, commandMessage(MAV_CMD_MISSION_START), &invoked),
             ExactLinkTransmitter::SendResult::SigningUnavailable);
    QVERIFY(!invoked);
    QCOMPARE(writes, 0);
    QCOMPARE(submitted, 0);
}

void ExactLinkTransmitterTest::reentrantSignerDestructionIsSafe()
{
    int writes = 0;
    int submitted = 0;
    auto *transmitter = new ExactLinkTransmitter([&writes](int, const QByteArray &) {
        ++writes;
        return true;
    });
    QPointer<ExactLinkTransmitter> guarded = transmitter;
    transmitter->setLinkSessionEpoch(11, 456);
    transmitter->setSigningRequired(11, true);
    connect(transmitter, &ExactLinkTransmitter::messageSubmitted, this,
            [&submitted](int, quint64, mavlink_message_t) { ++submitted; });
    transmitter->setFrameSigner([&](int, const QByteArray &frame, QByteArray *out) {
        *out = attachSignature(frame);
        delete transmitter;
        transmitter = nullptr;
        return true;
    });
    bool invoked = true;
    const auto result = transmitter->sendMessage(
        11, 250, 190, commandMessage(MAV_CMD_MISSION_START), &invoked);
    QCOMPARE(result, ExactLinkTransmitter::SendResult::SigningUnavailable);
    QVERIFY(guarded.isNull());
    QVERIFY(!transmitter);
    QVERIFY(!invoked);
    QCOMPARE(writes, 0);
    QCOMPARE(submitted, 0);
}

QTEST_APPLESS_MAIN(ExactLinkTransmitterTest)

#include "test_exactlinktransmitter.moc"
