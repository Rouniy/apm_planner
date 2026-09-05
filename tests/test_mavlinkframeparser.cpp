#include "comm/MAVLinkFrameParser.h"

#include <QByteArray>
#include <QtTest>

namespace {
QByteArray frameBytes(const mavlink_message_t &message)
{
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
    const int size = mavlink_msg_to_send_buffer(buffer, &message);
    return QByteArray(reinterpret_cast<const char *>(buffer), size);
}

QByteArray heartbeatFrame(quint8 systemId, quint8 componentId)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, MAV_TYPE_QUADROTOR,
        MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
        3, MAV_STATE_ACTIVE);
    return frameBytes(message);
}

QByteArray mavlink1HeartbeatFrame(quint8 systemId, quint8 componentId)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, MAV_TYPE_FIXED_WING,
        MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
        7, MAV_STATE_STANDBY);
    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(message.msgid);
    Q_ASSERT(entry);
    mavlink_status_t status{};
    status.flags = MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    status.current_tx_seq = 17;
    mavlink_finalize_message_buffer(
        &message, systemId, componentId, &status,
        entry->min_msg_len, entry->max_msg_len, entry->crc_extra);
    return frameBytes(message);
}

QByteArray trimmedPayloadFrame()
{
    // COMMAND_ACK has ten payload bytes, but this payload ends after byte two.
    // Its first byte is deliberately zero, proving that capture follows the
    // received length rather than reconstructing a decoded payload buffer.
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_COMMAND_ACK;
    auto *payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint16_t(payload, 0, 256);
    _mav_put_uint8_t(payload, 2, MAV_RESULT_ACCEPTED);
    mavlink_status_t status{};
    status.current_tx_seq = 23;
    mavlink_finalize_message_buffer(
        &message, 42, 1, &status,
        MAVLINK_MSG_ID_COMMAND_ACK_MIN_LEN,
        MAVLINK_MSG_ID_COMMAND_ACK_LEN,
        MAVLINK_MSG_ID_COMMAND_ACK_CRC);
    Q_ASSERT(message.len == 2);
    return frameBytes(message);
}

QByteArray payloadWithMarkersFrame()
{
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_STATUSTEXT;
    auto *payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint8_t(payload, 0, MAV_SEVERITY_INFO);
    payload[1] = static_cast<char>(MAVLINK_STX);
    payload[2] = static_cast<char>(MAVLINK_STX_MAVLINK1);
    mavlink_status_t status{};
    status.current_tx_seq = 31;
    mavlink_finalize_message_buffer(
        &message, 42, 1, &status,
        MAVLINK_MSG_ID_STATUSTEXT_MIN_LEN,
        MAVLINK_MSG_ID_STATUSTEXT_LEN,
        MAVLINK_MSG_ID_STATUSTEXT_CRC);
    Q_ASSERT(message.len == 3);
    return frameBytes(message);
}

QByteArray appendSignatureTrailer(const QByteArray &unsignedFrame,
                                  const QByteArray &signature)
{
    Q_ASSERT(unsignedFrame.size() >= MAVLINK_NUM_HEADER_BYTES + 2);
    Q_ASSERT(quint8(unsignedFrame.at(0)) == MAVLINK_STX);
    Q_ASSERT(signature.size() == MAVLINK_SIGNATURE_BLOCK_LEN);
    QByteArray result = unsignedFrame;
    auto *bytes = reinterpret_cast<quint8 *>(result.data());
    const int payloadLength = bytes[1];
    const int crcOffset = MAVLINK_NUM_HEADER_BYTES + payloadLength;
    const quint32 messageId = quint32(bytes[7])
        | (quint32(bytes[8]) << 8) | (quint32(bytes[9]) << 16);
    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(messageId);
    Q_ASSERT(entry);
    bytes[2] |= MAVLINK_IFLAG_SIGNED;
    quint16 crc = crc_calculate(bytes + 1, crcOffset - 1);
    crc_accumulate(entry->crc_extra, &crc);
    bytes[crcOffset] = static_cast<quint8>(crc & 0xff);
    bytes[crcOffset + 1] = static_cast<quint8>(crc >> 8);
    result.append(signature);
    return result;
}

QList<mavlink_message_t> feed(MAVLinkFrameParser &parser,
                              const QByteArray &bytes)
{
    QList<mavlink_message_t> result;
    for (char byte : bytes) {
        mavlink_message_t message{};
        if (parser.parseByte(static_cast<quint8>(byte), &message)
            == MAVLINK_FRAMING_OK) {
            result.append(message);
        }
    }
    return result;
}

}

class MAVLinkFrameParserTest final : public QObject
{
    Q_OBJECT

private slots:
    void interleavedLinksKeepIndependentPartialFrames();
    void protocolVersionStateIsPerParser();
    void signedTrailerNeverRehabilitatesBadCrc();
    void exactFrameClearsOnlyWhenTheNextFrameStarts();
    void markersInsidePayloadAndSignatureDoNotResynchronize();
    void rejectedAndTruncatedFramesRecoverWithoutFabricatingBytes();
    void mavlink1FrameIsCapturedExactly();
};

void MAVLinkFrameParserTest::interleavedLinksKeepIndependentPartialFrames()
{
    MAVLinkFrameParser first;
    MAVLinkFrameParser second;
    const QByteArray firstFrame = heartbeatFrame(41, 1);
    const QByteArray secondFrame = heartbeatFrame(42, 154);
    const int split = firstFrame.size() / 2;

    QVERIFY(feed(first, firstFrame.left(split)).isEmpty());
    QVERIFY(first.lastFrame().isEmpty());
    const QList<mavlink_message_t> secondMessages = feed(second, secondFrame);
    QCOMPARE(secondMessages.size(), 1);
    QCOMPARE(secondMessages.first().sysid, quint8(42));
    QCOMPARE(secondMessages.first().compid, quint8(154));
    QCOMPARE(second.lastFrame(), secondFrame);
    QVERIFY(first.lastFrame().isEmpty());

    const QList<mavlink_message_t> firstMessages =
        feed(first, firstFrame.mid(split));
    QCOMPARE(firstMessages.size(), 1);
    QCOMPARE(firstMessages.first().sysid, quint8(41));
    QCOMPARE(firstMessages.first().compid, quint8(1));
    QCOMPARE(first.lastFrame(), firstFrame);
    QCOMPARE(second.lastFrame(), secondFrame);
}

void MAVLinkFrameParserTest::protocolVersionStateIsPerParser()
{
    MAVLinkFrameParser mavlink1;
    MAVLinkFrameParser mavlink2;
    mavlink1.setOutboundVersion(1);
    mavlink2.setOutboundVersion(2);

    QVERIFY(mavlink1.status().flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1);
    QVERIFY(!(mavlink2.status().flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1));

    mavlink2.setOutboundVersion(1);
    QVERIFY(mavlink1.status().flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1);
    QVERIFY(mavlink2.status().flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1);
}

void MAVLinkFrameParserTest::signedTrailerNeverRehabilitatesBadCrc()
{
    // Independent pymavlink frame, key bytes1..32, timestamp27992960000000.
    const QByteArray valid = QByteArray::fromHex(
        "fd0901004d2a01000000000000000203000303c9070900e06f9e7519f130582f8758");
    for (bool configured : {false, true}) {
        MAVLinkFrameParser parser;
        mavlink_signing_t signing{};
        for (int i = 0; i < 32; ++i) signing.secret_key[i] = i + 1;
        signing.timestamp = 27992960000000ULL;
        mavlink_signing_streams_t streams{};
        if (configured) {
            parser.status().signing = &signing;
            parser.status().signing_streams = &streams;
        }
        for (int offset : {10, 19, 20}) {
            QByteArray corrupt = valid;
            corrupt[offset] = char(quint8(corrupt[offset]) ^ 1);
            // Even a genuinely valid MAC covering an invalid CRC is not an
            // acceptable frame and must not allocate replay state.
            auto signer = signing;
            signer.flags = MAVLINK_SIGNING_FLAG_SIGN_OUTGOING;
            signer.link_id = 9;
            auto *bytes = reinterpret_cast<unsigned char *>(corrupt.data());
            mavlink_sign_packet(&signer, bytes + 21, bytes, 10, bytes + 10, 9, bytes + 19);
            QList<unsigned int> outcomes;
            for (int index = 0; index < corrupt.size(); ++index) {
                mavlink_message_t message{};
                const unsigned int framing = parser.parseByte(
                    quint8(corrupt.at(index)), &message);
                if (framing != MAVLINK_FRAMING_INCOMPLETE) {
                    outcomes.append(framing);
                    if (index != corrupt.size() - 1) {
                        // The native helper reports BAD_CRC at CRC2 even
                        // though a signed frame still has 13 bytes to consume.
                        QVERIFY(parser.lastFrame().isEmpty());
                    }
                }
            }
            QCOMPARE(outcomes.size(), 2);
            QCOMPARE(outcomes.first(), unsigned(MAVLINK_FRAMING_BAD_CRC));
            QCOMPARE(outcomes.last(), unsigned(MAVLINK_FRAMING_BAD_CRC));
            QCOMPARE(parser.lastFrame(), corrupt);
            QCOMPARE(parser.status().msg_received, quint8(MAVLINK_FRAMING_BAD_CRC));
            QCOMPARE(parser.status().packet_rx_success_count, quint16(0));
            QCOMPARE(streams.num_signing_streams, quint16(0));
            QCOMPARE(signing.timestamp, 27992960000000ULL);
        }
        QCOMPARE(feed(parser, valid).size(), 1);
        QCOMPARE(parser.lastFrame(), valid);
        QCOMPARE(parser.status().packet_rx_success_count, quint16(1));
        if (configured) QCOMPARE(streams.num_signing_streams, quint16(1));
    }
}

void MAVLinkFrameParserTest::exactFrameClearsOnlyWhenTheNextFrameStarts()
{
    MAVLinkFrameParser parser;
    const QByteArray first = trimmedPayloadFrame();
    const QByteArray second = heartbeatFrame(43, 2);
    QCOMPARE(feed(parser, first).size(), 1);
    QCOMPARE(parser.lastFrame(), first);

    // Non-frame transport noise does not invalidate the last complete frame.
    QVERIFY(feed(parser, QByteArray::fromHex("0011227f")).isEmpty());
    QCOMPARE(parser.lastFrame(), first);

    mavlink_message_t message{};
    QCOMPARE(parser.parseByte(quint8(second.at(0)), &message),
             unsigned(MAVLINK_FRAMING_INCOMPLETE));
    QVERIFY(parser.lastFrame().isEmpty());
    QVERIFY(feed(parser, second.mid(1, second.size() - 2)).isEmpty());
    QVERIFY(parser.lastFrame().isEmpty());
    QCOMPARE(parser.parseByte(quint8(second.back()), &message),
             unsigned(MAVLINK_FRAMING_OK));
    QCOMPARE(parser.lastFrame(), second);
}

void MAVLinkFrameParserTest::markersInsidePayloadAndSignatureDoNotResynchronize()
{
    MAVLinkFrameParser parser;
    const QByteArray payloadMarkers = payloadWithMarkersFrame();
    QCOMPARE(feed(parser, payloadMarkers).size(), 1);
    QCOMPARE(parser.lastFrame(), payloadMarkers);

    QByteArray trailer(MAVLINK_SIGNATURE_BLOCK_LEN, '\0');
    trailer[0] = 9;
    trailer[2] = static_cast<char>(MAVLINK_STX);
    trailer[6] = static_cast<char>(MAVLINK_STX_MAVLINK1);
    trailer[12] = static_cast<char>(MAVLINK_STX);
    const QByteArray signedMarkers = appendSignatureTrailer(
        heartbeatFrame(44, 3), trailer);
    QCOMPARE(feed(parser, signedMarkers).size(), 1);
    QCOMPARE(parser.lastFrame(), signedMarkers);

    MAVLinkFrameParser rejectingParser;
    mavlink_signing_t wrongSigning{};
    wrongSigning.secret_key[0] = 1;
    mavlink_signing_streams_t streams{};
    rejectingParser.status().signing = &wrongSigning;
    rejectingParser.status().signing_streams = &streams;
    unsigned int terminal = MAVLINK_FRAMING_INCOMPLETE;
    for (char byte : signedMarkers) {
        mavlink_message_t message{};
        const unsigned int framing = rejectingParser.parseByte(
            quint8(byte), &message);
        if (framing != MAVLINK_FRAMING_INCOMPLETE) {
            terminal = framing;
        }
    }
    QCOMPARE(terminal, unsigned(MAVLINK_FRAMING_BAD_SIGNATURE));
    QCOMPARE(rejectingParser.lastFrame(), signedMarkers);
    QCOMPARE(streams.num_signing_streams, quint16(0));
}

void MAVLinkFrameParserTest::rejectedAndTruncatedFramesRecoverWithoutFabricatingBytes()
{
    MAVLinkFrameParser parser;
    const QByteArray first = heartbeatFrame(45, 4);
    const QByteArray recovery = heartbeatFrame(46, 5);
    QCOMPARE(feed(parser, first).size(), 1);

    // The unsupported incompatibility bit retires this candidate immediately.
    QVERIFY(feed(parser, QByteArray::fromHex("fd0980")).isEmpty());
    QVERIFY(parser.lastFrame().isEmpty());
    QVERIFY(feed(parser, QByteArray::fromHex("112233")).isEmpty());
    QVERIFY(parser.lastFrame().isEmpty());
    QCOMPARE(feed(parser, recovery).size(), 1);
    QCOMPARE(parser.lastFrame(), recovery);

    QByteArray trailer(MAVLINK_SIGNATURE_BLOCK_LEN, '\0');
    trailer[0] = 7;
    const QByteArray completeSigned = appendSignatureTrailer(first, trailer);
    const QByteArray truncated = completeSigned.left(completeSigned.size() - 1);
    QVERIFY(feed(parser, truncated).isEmpty());
    QVERIFY(parser.lastFrame().isEmpty());

    // An STX while waiting for a signature byte is data, not a new frame. The
    // native no-key parser consequently completes this exact contiguous frame.
    mavlink_message_t message{};
    QCOMPARE(parser.parseByte(quint8(recovery.at(0)), &message),
             unsigned(MAVLINK_FRAMING_OK));
    QCOMPARE(parser.lastFrame(), truncated + recovery.left(1));
    QVERIFY(feed(parser, recovery.mid(1)).isEmpty());
    QCOMPARE(parser.lastFrame(), truncated + recovery.left(1));

    // A subsequent complete STX-delimited packet recovers normally.
    QCOMPARE(feed(parser, recovery).size(), 1);
    QCOMPARE(parser.lastFrame(), recovery);
}

void MAVLinkFrameParserTest::mavlink1FrameIsCapturedExactly()
{
    MAVLinkFrameParser parser;
    const QByteArray frame = mavlink1HeartbeatFrame(47, 6);
    QCOMPARE(quint8(frame.at(0)), quint8(MAVLINK_STX_MAVLINK1));
    QCOMPARE(feed(parser, frame).size(), 1);
    QCOMPARE(parser.lastFrame(), frame);
}

QTEST_APPLESS_MAIN(MAVLinkFrameParserTest)
#include "test_mavlinkframeparser.moc"
