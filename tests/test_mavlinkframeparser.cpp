#include "comm/MAVLinkFrameParser.h"

#include <QByteArray>
#include <QtTest>

namespace {
QByteArray heartbeatFrame(quint8 systemId, quint8 componentId)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, MAV_TYPE_QUADROTOR,
        MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
        3, MAV_STATE_ACTIVE);
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
    const int size = mavlink_msg_to_send_buffer(buffer, &message);
    return QByteArray(reinterpret_cast<const char *>(buffer), size);
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
};

void MAVLinkFrameParserTest::interleavedLinksKeepIndependentPartialFrames()
{
    MAVLinkFrameParser first;
    MAVLinkFrameParser second;
    const QByteArray firstFrame = heartbeatFrame(41, 1);
    const QByteArray secondFrame = heartbeatFrame(42, 154);
    const int split = firstFrame.size() / 2;

    QVERIFY(feed(first, firstFrame.left(split)).isEmpty());
    const QList<mavlink_message_t> secondMessages = feed(second, secondFrame);
    QCOMPARE(secondMessages.size(), 1);
    QCOMPARE(secondMessages.first().sysid, quint8(42));
    QCOMPARE(secondMessages.first().compid, quint8(154));

    const QList<mavlink_message_t> firstMessages =
        feed(first, firstFrame.mid(split));
    QCOMPARE(firstMessages.size(), 1);
    QCOMPARE(firstMessages.first().sysid, quint8(41));
    QCOMPARE(firstMessages.first().compid, quint8(1));
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

QTEST_APPLESS_MAIN(MAVLinkFrameParserTest)
#include "test_mavlinkframeparser.moc"
