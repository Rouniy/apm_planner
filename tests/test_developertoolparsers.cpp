#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QtNumeric>

#include "ui/configuration/DeveloperToolParsers.h"

#include <cstddef>
#include <cstring>
#include <mavlink.h>

namespace {

QByteArray wireBytes(const mavlink_message_t &message)
{
    quint8 buffer[MAVLINK_MAX_PACKET_LEN];
    const quint16 length = mavlink_msg_to_send_buffer(buffer, &message);
    return QByteArray(reinterpret_cast<const char *>(buffer), length);
}

QString hexText(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex());
}

void setOutboundMavlink1(bool mavlink1)
{
    mavlink_status_t *status = mavlink_get_channel_status(MAVLINK_COMM_0);
    if (mavlink1) {
        status->flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    } else {
        status->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    }
}

mavlink_message_t heartbeatMessage(bool mavlink1)
{
    setOutboundMavlink1(mavlink1);
    mavlink_heartbeat_t heartbeat{};
    heartbeat.type = MAV_TYPE_QUADROTOR;
    heartbeat.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    heartbeat.base_mode = 81;
    heartbeat.custom_mode = 3;
    heartbeat.system_status = MAV_STATE_ACTIVE;
    heartbeat.mavlink_version = 3;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(1, 1, &message, &heartbeat);
    setOutboundMavlink1(false);
    return message;
}

// Recomputes the checksum of a MAVLink 2 packet whose header or payload
// bytes were edited by a test.
QByteArray withRecomputedCrc(QByteArray packet, quint8 crcExtra)
{
    const int payloadLength = static_cast<quint8>(packet.at(1));
    const int crcIndex = 1 + MAVLINK_CORE_HEADER_LEN + payloadLength;
    quint16 crc = crc_calculate(
        reinterpret_cast<const quint8 *>(packet.constData()) + 1,
        static_cast<quint16>(MAVLINK_CORE_HEADER_LEN + payloadLength));
    crc_accumulate(crcExtra, &crc);
    packet[crcIndex] = static_cast<char>(crc & 0xff);
    packet[crcIndex + 1] = static_cast<char>(crc >> 8);
    return packet;
}

QByteArray signedHeartbeat()
{
    QByteArray packet = wireBytes(heartbeatMessage(false));
    packet[2] = static_cast<char>(
        static_cast<quint8>(packet.at(2)) | MAVLINK_IFLAG_SIGNED);
    packet = withRecomputedCrc(packet, MAVLINK_MSG_ID_HEARTBEAT_CRC);
    QByteArray signature(MAVLINK_SIGNATURE_BLOCK_LEN, '\0');
    signature[0] = 7;                        // link id
    signature[1] = static_cast<char>(0x10);  // 48-bit timestamp, low byte
    signature[2] = static_cast<char>(0x20);
    for (int i = 7; i < MAVLINK_SIGNATURE_BLOCK_LEN; ++i) {
        signature[i] = static_cast<char>(0xA0 + i);
    }
    return packet + signature;
}

// MAVLink 2 frame for message id 0xFFFFFF (undefined in every dialect) with a
// checksum computed the way the C parser does for unknown ids (crc_extra 0).
QByteArray unknownMessagePacket()
{
    return withRecomputedCrc(
        QByteArray::fromHex("fd010000000101ffffff000000"), 0);
}

// Parses the diagnostic JSON and fails the test on any syntax error.
QJsonObject parsedJson(const QString &json)
{
    QJsonParseError parseError{};
    const QJsonDocument document =
        QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning("JSON parse failed: %s\n%s",
                 qPrintable(parseError.errorString()), qPrintable(json));
        return {};
    }
    return document.object();
}

} // namespace

class DeveloperToolParsersTest final : public QObject
{
    Q_OBJECT

private slots:
    void parseBytesAcceptsCompactHex();
    void parseBytesAcceptsDecimalAndExplicitHexTokens();
    void parseBytesRejectsInvalidInput_data();
    void parseBytesRejectsInvalidInput();
    void decodesMavlink2Heartbeat();
    void decodesMavlink1Heartbeat();
    void rendersTypedFieldsExactly();
    void acceptsSignedPacketWithoutSigningKey();
    void rejectsUnsupportedIncompatFlags();
    void reportsTruncatedPackets();
    void reportsBadCrc();
    void rejectsUnknownMessageId();
    void reportsMissingStartByteAndBadInput();
    void skipsLeadingGarbageAndReportsTrailingBytes();
    void decodeHardwareIdReportsBusFields();
    void decodeHardwareIdResolvesDeviceFamilies();
    void decodeHardwareIdRejectsInvalidInput();
};

void DeveloperToolParsersTest::parseBytesAcceptsCompactHex()
{
    const QByteArray expected = QByteArray::fromHex("fd0500a1");

    DeveloperToolBytes result =
        DeveloperToolParsers::ParseBytes(QStringLiteral("0xfd0500a1"));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.bytes, expected);

    result = DeveloperToolParsers::ParseBytes(QStringLiteral("  FD0500A1 "));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.bytes, expected);

    result = DeveloperToolParsers::ParseBytes(QStringLiteral("0XFD0500A1"));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.bytes, expected);
}

void DeveloperToolParsersTest::parseBytesAcceptsDecimalAndExplicitHexTokens()
{
    DeveloperToolBytes result =
        DeveloperToolParsers::ParseBytes(QStringLiteral("253, 5 0 0xA1"));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.bytes, QByteArray::fromHex("fd0500a1"));

    // Every MP10 separator, bare hex when a token contains a letter, decimal
    // otherwise, and the leading sign that NumberStyles.Integer accepts.
    result = DeveloperToolParsers::ParseBytes(
        QStringLiteral("fd;05:00-a1\n0x0A\t+7,255"));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.bytes, QByteArray::fromHex("fd0500a10a07ff"));
}

void DeveloperToolParsersTest::parseBytesRejectsInvalidInput_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("fragment");

    QTest::newRow("empty") << QString() << QStringLiteral("Enter at least one byte.");
    QTest::newRow("whitespace") << QStringLiteral("   ")
                                << QStringLiteral("Enter at least one byte.");
    QTest::newRow("separators only") << QStringLiteral(",, ;")
                                     << QStringLiteral("Enter at least one byte.");
    QTest::newRow("odd compact") << QStringLiteral("abc")
                                 << QStringLiteral("even number of hex digits");
    QTest::newRow("empty compact prefix") << QStringLiteral("0x")
                                          << QStringLiteral("even number of hex digits");
    QTest::newRow("non-hex compact") << QStringLiteral("zz")
                                     << QStringLiteral("'z' at position 1");
    QTest::newRow("decimal overflow") << QStringLiteral("256 0")
                                      << QStringLiteral("'256' is not a byte (0..255 or 00..FF).");
    QTest::newRow("bad hex token") << QStringLiteral("0xGG 1")
                                   << QStringLiteral("'0xGG' is not a byte");
    QTest::newRow("hex overflow") << QStringLiteral("FFF 1")
                                  << QStringLiteral("'FFF' is not a byte");
    QTest::newRow("empty hex token") << QStringLiteral("1 0x")
                                     << QStringLiteral("'0x' is not a byte");
}

void DeveloperToolParsersTest::parseBytesRejectsInvalidInput()
{
    QFETCH(QString, input);
    QFETCH(QString, fragment);

    const DeveloperToolBytes result = DeveloperToolParsers::ParseBytes(input);
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(fragment), qPrintable(result.error));
    QVERIFY(result.bytes.isEmpty());
}

void DeveloperToolParsersTest::decodesMavlink2Heartbeat()
{
    const QByteArray bytes = wireBytes(heartbeatMessage(false));
    QCOMPARE(bytes.size(), 21);
    QCOMPARE(static_cast<int>(static_cast<quint8>(bytes.at(0))), 0xFD);

    const DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(hexText(bytes));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.name, QStringLiteral("HEARTBEAT"));
    QCOMPARE(result.msgid, static_cast<quint32>(MAVLINK_MSG_ID_HEARTBEAT));
    QCOMPARE(static_cast<int>(result.sysid), 1);
    QCOMPARE(static_cast<int>(result.compid), 1);
    QCOMPARE(static_cast<int>(result.payloadLength), 9);
    QVERIFY(result.mavlink2);
    QVERIFY(!result.signaturePresent);
    QVERIFY(!result.signatureVerified);
    QCOMPARE(result.packet, bytes);
    QCOMPARE(result.leadingBytesSkipped, 0);
    QCOMPARE(result.trailingBytes, 0);
    const quint16 carriedCrc = static_cast<quint16>(
        static_cast<quint8>(bytes.at(19)) |
        (static_cast<quint8>(bytes.at(20)) << 8));
    QCOMPARE(static_cast<int>(result.crc16), static_cast<int>(carriedCrc));

    QCOMPARE(result.summary,
             QStringLiteral("HEARTBEAT (message 0, system 1, component 1, 21 bytes)"));
    QVERIFY(result.json.startsWith(QStringLiteral("{\n")));
    QVERIFY(result.json.endsWith(QStringLiteral("\n}")));
    QVERIFY(result.json.contains(QStringLiteral("\"buffer\": \"") + hexText(bytes) + QStringLiteral("\"")));
    QVERIFY(result.json.contains(QStringLiteral("\"header\": 253")));
    QVERIFY(result.json.contains(QStringLiteral("\"payloadlength\": 9")));
    QVERIFY(result.json.contains(QStringLiteral("\"ismavlink2\": true")));
    QVERIFY(result.json.contains(QStringLiteral("\"msgtypename\": \"HEARTBEAT\"")));
    QVERIFY(result.json.contains(QStringLiteral("\"type\": 2")));
    QVERIFY(result.json.contains(QStringLiteral("\"autopilot\": 3")));
    QVERIFY(result.json.contains(QStringLiteral("\"base_mode\": 81")));
    QVERIFY(result.json.contains(QStringLiteral("\"custom_mode\": 3")));
    QVERIFY(result.json.contains(QStringLiteral("\"system_status\": 4")));
    QVERIFY(result.json.contains(QStringLiteral("\"mavlink_version\": 3")));
    QVERIFY(result.json.contains(QStringLiteral("\"crc16\": ") + QString::number(carriedCrc)));
    QVERIFY(result.json.contains(QStringLiteral("\"sig\": null")));
    QVERIFY(result.json.contains(QStringLiteral("\"signaturePresent\": false")));
    QVERIFY(result.json.contains(QStringLiteral("\"signatureVerified\": false")));
    QVERIFY(result.json.contains(QStringLiteral("\"Length\": 21")));

    // The JSON is well formed and typed, not just textually similar.
    const QJsonObject object = parsedJson(result.json);
    QVERIFY(!object.isEmpty());
    QVERIFY(object.value(QStringLiteral("buffer")).isString());
    QCOMPARE(object.value(QStringLiteral("buffer")).toString(), hexText(bytes));
    QVERIFY(object.value(QStringLiteral("header")).isDouble());
    QCOMPARE(object.value(QStringLiteral("header")).toInt(), 0xFD);
    QVERIFY(object.value(QStringLiteral("ismavlink2")).isBool());
    QVERIFY(object.value(QStringLiteral("ismavlink2")).toBool());
    QVERIFY(object.value(QStringLiteral("msgtypename")).isString());
    QCOMPARE(object.value(QStringLiteral("msgtypename")).toString(), QStringLiteral("HEARTBEAT"));
    QVERIFY(object.value(QStringLiteral("data")).isObject());
    const QJsonObject data = object.value(QStringLiteral("data")).toObject();
    QCOMPARE(data.size(), 6);
    QVERIFY(data.value(QStringLiteral("custom_mode")).isDouble());
    QCOMPARE(data.value(QStringLiteral("custom_mode")).toInt(), 3);
    QCOMPARE(data.value(QStringLiteral("type")).toInt(), 2);
    QVERIFY(object.value(QStringLiteral("crc16")).isDouble());
    QCOMPARE(object.value(QStringLiteral("crc16")).toInt(), static_cast<int>(carriedCrc));
    QVERIFY(object.value(QStringLiteral("sig")).isNull());
    QVERIFY(object.value(QStringLiteral("signaturePresent")).isBool());
    QVERIFY(!object.value(QStringLiteral("signaturePresent")).toBool());
    QVERIFY(object.value(QStringLiteral("signatureVerified")).isBool());
    QVERIFY(!object.value(QStringLiteral("signatureVerified")).toBool(true));
    QCOMPARE(object.value(QStringLiteral("Length")).toInt(), 21);

    QVERIFY(result.text().startsWith(result.summary + QStringLiteral("\n{\n")));
    QVERIFY(result.text().endsWith(QStringLiteral("\n}")));
    QVERIFY(!result.text().contains(QStringLiteral("trailing")));
    // Unsigned packets carry the (false) signature booleans in the JSON but
    // no signature note after it.
    QVERIFY(!result.text().contains(QStringLiteral("NOT verified")));
    QVERIFY(!result.text().contains(QStringLiteral("signature block")));
}

void DeveloperToolParsersTest::decodesMavlink1Heartbeat()
{
    const QByteArray bytes = wireBytes(heartbeatMessage(true));
    QCOMPARE(bytes.size(), 17);
    QCOMPARE(static_cast<int>(static_cast<quint8>(bytes.at(0))), 0xFE);

    const DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(bytes);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.name, QStringLiteral("HEARTBEAT"));
    QVERIFY(!result.mavlink2);
    QVERIFY(!result.signaturePresent);
    QVERIFY(!result.signatureVerified);
    QCOMPARE(static_cast<int>(result.incompatFlags), 0);
    QCOMPARE(static_cast<int>(result.compatFlags), 0);
    QCOMPARE(result.summary,
             QStringLiteral("HEARTBEAT (message 0, system 1, component 1, 17 bytes)"));
    QVERIFY(result.json.contains(QStringLiteral("\"header\": 254")));
    QVERIFY(result.json.contains(QStringLiteral("\"ismavlink2\": false")));
    QVERIFY(result.json.contains(QStringLiteral("\"custom_mode\": 3")));
    QVERIFY(result.json.contains(QStringLiteral("\"Length\": 17")));
}

void DeveloperToolParsersTest::rendersTypedFieldsExactly()
{
    // 64-bit integers must not pass through a double.
    mavlink_system_time_t systemTime{};
    systemTime.time_unix_usec = Q_UINT64_C(1700000000123456789);
    systemTime.time_boot_ms = 4294967295u;
    mavlink_message_t message{};
    mavlink_msg_system_time_encode(42, 200, &message, &systemTime);
    DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(wireBytes(message));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.name, QStringLiteral("SYSTEM_TIME"));
    QCOMPARE(static_cast<int>(result.sysid), 42);
    QCOMPARE(static_cast<int>(result.compid), 200);
    QVERIFY(result.json.contains(QStringLiteral("\"time_unix_usec\": 1700000000123456789")));
    QVERIFY(result.json.contains(QStringLiteral("\"time_boot_ms\": 4294967295")));

    // Floats use the fewest round-tripping digits, keep fixed notation below
    // 1e15, quote NaN, render arrays as lists, and fields trimmed from a
    // MAVLink 2 payload read as zero.
    mavlink_attitude_quaternion_t attitude{};
    attitude.time_boot_ms = 12;
    attitude.q1 = 0.1f;
    attitude.q2 = -2.5f;
    attitude.q3 = 100.0f;
    attitude.q4 = 100000.0f;
    attitude.rollspeed = qQNaN();
    attitude.pitchspeed = 1e10f;
    attitude.yawspeed = 0.00001f;
    attitude.repr_offset_q[0] = 1e15f;
    attitude.repr_offset_q[1] = 0.5f;
    attitude.repr_offset_q[2] = 0.0f;
    attitude.repr_offset_q[3] = 3.0f;
    mavlink_msg_attitude_quaternion_encode(1, 1, &message, &attitude);
    result = DeveloperToolParsers::DecodeMavlinkPacket(wireBytes(message));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.name, QStringLiteral("ATTITUDE_QUATERNION"));
    QCOMPARE(static_cast<int>(result.payloadLength), 48);
    QVERIFY2(result.json.contains(QStringLiteral("\"q1\": 0.1,")), qPrintable(result.json));
    QVERIFY2(result.json.contains(QStringLiteral("\"q2\": -2.5,")), qPrintable(result.json));
    QVERIFY2(result.json.contains(QStringLiteral("\"q3\": 100,")), qPrintable(result.json));
    QVERIFY2(result.json.contains(QStringLiteral("\"q4\": 100000,")), qPrintable(result.json));
    QVERIFY2(result.json.contains(QStringLiteral("\"rollspeed\": \"NaN\",")), qPrintable(result.json));
    QVERIFY2(result.json.contains(QStringLiteral("\"pitchspeed\": 10000000000,")), qPrintable(result.json));
    QVERIFY2(result.json.contains(QStringLiteral("\"yawspeed\": 1E-05,")), qPrintable(result.json));
    QVERIFY2(result.json.contains(QStringLiteral("\"repr_offset_q\": [1E+15, 0.5, 0, 3]")), qPrintable(result.json));
    QJsonObject data = parsedJson(result.json).value(QStringLiteral("data")).toObject();
    QVERIFY(!data.isEmpty());
    QVERIFY(data.value(QStringLiteral("q1")).isDouble());
    QCOMPARE(data.value(QStringLiteral("q3")).toDouble(), 100.0);
    QVERIFY(data.value(QStringLiteral("rollspeed")).isString());
    QCOMPARE(data.value(QStringLiteral("rollspeed")).toString(), QStringLiteral("NaN"));
    QVERIFY(data.value(QStringLiteral("repr_offset_q")).isArray());
    QCOMPARE(data.value(QStringLiteral("repr_offset_q")).toArray().size(), 4);
    QCOMPARE(data.value(QStringLiteral("repr_offset_q")).toArray().at(0).toDouble(), 1e15);

    // A trimmed MAVLink 2 payload (trailing zeros dropped by the encoder)
    // still renders every field, reading the missing bytes as zero.
    mavlink_attitude_quaternion_t trimmed{};
    trimmed.q1 = 1.0f;
    mavlink_msg_attitude_quaternion_encode(1, 1, &message, &trimmed);
    result = DeveloperToolParsers::DecodeMavlinkPacket(wireBytes(message));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QVERIFY(result.payloadLength < 48);
    QVERIFY2(result.json.contains(QStringLiteral("\"q1\": 1,")), qPrintable(result.json));
    QVERIFY2(result.json.contains(QStringLiteral("\"yawspeed\": 0,")), qPrintable(result.json));
    QVERIFY2(result.json.contains(QStringLiteral("\"repr_offset_q\": [0, 0, 0, 0]")), qPrintable(result.json));

    // Doubles follow the same rule with up to 17 significant digits.
    mavlink_wheel_distance_t wheels{};
    wheels.time_usec = 7;
    wheels.count = 6;
    wheels.distance[0] = 1.5;
    wheels.distance[1] = 100000.0;
    wheels.distance[2] = 1e15;
    wheels.distance[3] = 0.1;
    wheels.distance[4] = 0.00001;
    wheels.distance[5] = 123456789.125;
    mavlink_msg_wheel_distance_encode(1, 1, &message, &wheels);
    result = DeveloperToolParsers::DecodeMavlinkPacket(wireBytes(message));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.name, QStringLiteral("WHEEL_DISTANCE"));
    QVERIFY2(result.json.contains(QStringLiteral(
                 "\"distance\": [1.5, 100000, 1E+15, 0.1, 1E-05, 123456789.125, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]")),
             qPrintable(result.json));
    data = parsedJson(result.json).value(QStringLiteral("data")).toObject();
    const QJsonArray distance = data.value(QStringLiteral("distance")).toArray();
    QCOMPARE(distance.size(), 16);
    QCOMPARE(distance.at(0).toDouble(), 1.5);
    QCOMPARE(distance.at(2).toDouble(), 1e15);
    QCOMPARE(distance.at(4).toDouble(), 0.00001);
    QCOMPARE(distance.at(5).toDouble(), 123456789.125);

    // char arrays become JSON strings with escaping.
    mavlink_statustext_t statusText{};
    statusText.severity = MAV_SEVERITY_INFO;
    std::strncpy(statusText.text, "hello \"quoted\"", sizeof(statusText.text));
    mavlink_msg_statustext_encode(1, 1, &message, &statusText);
    result = DeveloperToolParsers::DecodeMavlinkPacket(wireBytes(message));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.name, QStringLiteral("STATUSTEXT"));
    QVERIFY(result.json.contains(QStringLiteral("\"severity\": 6")));
    QVERIFY(result.json.contains(QStringLiteral("\"text\": \"hello \\\"quoted\\\"\"")));
    QVERIFY(result.json.contains(QStringLiteral("\"id\": 0")));
    QVERIFY(result.json.contains(QStringLiteral("\"chunk_seq\": 0")));
    data = parsedJson(result.json).value(QStringLiteral("data")).toObject();
    QVERIFY(data.value(QStringLiteral("text")).isString());
    QCOMPARE(data.value(QStringLiteral("text")).toString(), QStringLiteral("hello \"quoted\""));
}

void DeveloperToolParsersTest::acceptsSignedPacketWithoutSigningKey()
{
    const QByteArray bytes = signedHeartbeat();
    QCOMPARE(bytes.size(), 21 + MAVLINK_SIGNATURE_BLOCK_LEN);

    const DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(hexText(bytes));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.name, QStringLiteral("HEARTBEAT"));
    QVERIFY(result.signaturePresent);
    QVERIFY(!result.signatureVerified);
    QCOMPARE(static_cast<int>(result.incompatFlags), MAVLINK_IFLAG_SIGNED);
    QCOMPARE(result.packet, bytes);
    QCOMPARE(result.summary,
             QStringLiteral("HEARTBEAT (message 0, system 1, component 1, 34 bytes)"));
    QVERIFY(result.json.contains(QStringLiteral("\"incompat_flags\": 1")));
    QVERIFY(result.json.contains(QStringLiteral("\"sig\": [7, 16, 32, 0, 0, 0, 0, 167, 168, 169, 170, 171, 172]")));
    QVERIFY(result.json.contains(QStringLiteral("\"sigLinkid\": 7")));
    QVERIFY(result.json.contains(QStringLiteral("\"sigTimestamp\": 8208")));
    QVERIFY(result.json.contains(QStringLiteral("\"signaturePresent\": true")));
    QVERIFY(result.json.contains(QStringLiteral("\"signatureVerified\": false")));
    QVERIFY(result.json.contains(QStringLiteral("\"Length\": 34")));

    const QJsonObject object = parsedJson(result.json);
    QVERIFY(object.value(QStringLiteral("sig")).isArray());
    QCOMPARE(object.value(QStringLiteral("sig")).toArray().size(), MAVLINK_SIGNATURE_BLOCK_LEN);
    QCOMPARE(object.value(QStringLiteral("sig")).toArray().at(0).toInt(), 7);
    QVERIFY(object.value(QStringLiteral("signaturePresent")).toBool());
    QVERIFY(!object.value(QStringLiteral("signatureVerified")).toBool(true));

    // The log text must never imply cryptographic success.
    QVERIFY(result.text().contains(QStringLiteral("NOT verified")));
    QVERIFY(result.text().contains(QStringLiteral("no signing key")));
}

void DeveloperToolParsersTest::rejectsUnsupportedIncompatFlags()
{
    QByteArray bytes = wireBytes(heartbeatMessage(false));
    bytes[2] = static_cast<char>(0x02); // unknown incompatibility flag
    bytes = withRecomputedCrc(bytes, MAVLINK_MSG_ID_HEARTBEAT_CRC);

    const DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(bytes);
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("incompatibility flags 0x02")), qPrintable(result.error));
    QVERIFY2(!result.error.contains(QStringLiteral("truncated")), qPrintable(result.error));

    // The signed bit combined with an unknown bit is rejected as well.
    bytes[2] = static_cast<char>(0x03);
    const DecodedMavlinkPacket combined =
        DeveloperToolParsers::DecodeMavlinkPacket(bytes);
    QVERIFY(!combined.ok());
    QVERIFY2(combined.error.contains(QStringLiteral("0x03")), qPrintable(combined.error));
}

void DeveloperToolParsersTest::reportsTruncatedPackets()
{
    const QByteArray bytes = wireBytes(heartbeatMessage(false));

    DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(bytes.left(bytes.size() - 3));
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("truncated")), qPrintable(result.error));
    QVERIFY2(result.error.contains(QStringLiteral("HEARTBEAT (message 0)")), qPrintable(result.error));
    QVERIFY2(result.error.contains(QStringLiteral("needs 21 bytes")), qPrintable(result.error));
    QVERIFY2(result.error.contains(QStringLiteral("3 more byte")), qPrintable(result.error));

    result = DeveloperToolParsers::DecodeMavlinkPacket(bytes.left(3));
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("unidentified message")), qPrintable(result.error));
    QVERIFY2(result.error.contains(QStringLiteral("18 more byte")), qPrintable(result.error));

    result = DeveloperToolParsers::DecodeMavlinkPacket(QStringLiteral("fd"));
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("only the start byte")), qPrintable(result.error));

    result = DeveloperToolParsers::DecodeMavlinkPacket(signedHeartbeat().left(21 + 5));
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("signature")), qPrintable(result.error));
    QVERIFY2(result.error.contains(QStringLiteral("8 more byte")), qPrintable(result.error));
}

void DeveloperToolParsersTest::reportsBadCrc()
{
    QByteArray bytes = wireBytes(heartbeatMessage(false));
    bytes[bytes.size() - 1] = static_cast<char>(
        static_cast<quint8>(bytes.at(bytes.size() - 1)) ^ 0xFF);

    DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(hexText(bytes));
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("CRC mismatch")), qPrintable(result.error));
    QVERIFY2(result.error.contains(QStringLiteral("HEARTBEAT (message 0)")), qPrintable(result.error));

    bytes = wireBytes(heartbeatMessage(false));
    bytes[12] = static_cast<char>(static_cast<quint8>(bytes.at(12)) ^ 0x01);
    result = DeveloperToolParsers::DecodeMavlinkPacket(bytes);
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("CRC mismatch")), qPrintable(result.error));
}

void DeveloperToolParsersTest::rejectsUnknownMessageId()
{
    const DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(unknownMessagePacket());
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("16777215")), qPrintable(result.error));
    QVERIFY2(result.error.contains(QStringLiteral("not defined")), qPrintable(result.error));
}

void DeveloperToolParsersTest::reportsMissingStartByteAndBadInput()
{
    DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(QStringLiteral("01 02 03"));
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("start byte")), qPrintable(result.error));
    QVERIFY2(result.error.contains(QStringLiteral("3 byte")), qPrintable(result.error));

    result = DeveloperToolParsers::DecodeMavlinkPacket(QByteArray());
    QVERIFY(!result.ok());
    QCOMPARE(result.error, QStringLiteral("Enter at least one byte."));

    result = DeveloperToolParsers::DecodeMavlinkPacket(QStringLiteral("abc"));
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("even number of hex digits")), qPrintable(result.error));
}

void DeveloperToolParsersTest::skipsLeadingGarbageAndReportsTrailingBytes()
{
    const QByteArray packet = wireBytes(heartbeatMessage(false));
    const QByteArray bytes =
        QByteArray::fromHex("0011") + packet + QByteArray::fromHex("aabb");

    const DecodedMavlinkPacket result =
        DeveloperToolParsers::DecodeMavlinkPacket(hexText(bytes));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.packet, packet);
    QCOMPARE(result.leadingBytesSkipped, 2);
    QCOMPARE(result.trailingBytes, 2);
    QCOMPARE(result.summary,
             QStringLiteral("HEARTBEAT (message 0, system 1, component 1, 21 bytes)"));
    QVERIFY(result.text().contains(QStringLiteral("2 trailing byte(s)")));
}

void DeveloperToolParsersTest::decodeHardwareIdReportsBusFields()
{
    const quint32 id = 2u | (3u << 3) | (42u << 8) | (7u << 16);
    QCOMPARE(id, 469530u);

    DecodedHardwareId result = DeveloperToolParsers::DecodeHardwareId(
        QString::number(id), QStringLiteral("COMPASS_DEV_ID"));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QVERIFY(result.text.contains(QStringLiteral("COMPASS_DEV_ID")));
    QVERIFY(result.text.contains(QStringLiteral("bus type SPI")));
    QVERIFY(result.text.contains(QStringLiteral("bus 3")));
    QVERIFY(result.text.contains(QStringLiteral("address 42")));
    QCOMPARE(result.text,
             QStringLiteral("COMPASS_DEV_ID devid 469530 bus type SPI bus 3 address 42 devtype HMC5883 "));
    QCOMPARE(result.devid, id);
    QCOMPARE(static_cast<int>(result.busType), 2);
    QCOMPARE(static_cast<int>(result.bus), 3);
    QCOMPARE(static_cast<int>(result.address), 42);
    QCOMPARE(static_cast<int>(result.devtype), 7);
    QCOMPARE(result.busTypeName, QStringLiteral("SPI"));
    QCOMPARE(result.devtypeName, QStringLiteral("HMC5883"));

    // Hex input and caller-name normalisation.
    result = DeveloperToolParsers::DecodeHardwareId(
        QStringLiteral(" 0x") + QString::number(id, 16) + QStringLiteral(" "),
        QStringLiteral(" compass_dev_id2 "));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.parameterName, QStringLiteral("COMPASS_DEV_ID2"));
    QCOMPARE(result.text,
             QStringLiteral("COMPASS_DEV_ID2 devid 469530 bus type SPI bus 3 address 42 devtype HMC5883 "));
}

void DeveloperToolParsersTest::decodeHardwareIdResolvesDeviceFamilies()
{
    DecodedHardwareId result = DeveloperToolParsers::DecodeHardwareId(
        (0x0Bu << 16) | 1u, QStringLiteral("BARO1_DEVID"));
    QCOMPARE(result.busTypeName, QStringLiteral("I2C"));
    QCOMPARE(result.devtypeName, QStringLiteral("BARO_MS5611"));

    result = DeveloperToolParsers::DecodeHardwareId(
        (0x13u << 16) | 2u, QStringLiteral("INS_ACC_ID"));
    QCOMPARE(result.devtypeName, QStringLiteral("ACC_MPU6000"));

    result = DeveloperToolParsers::DecodeHardwareId(
        (0x02u << 16) | 3u, QStringLiteral("ASP"));
    QCOMPARE(result.busTypeName, QStringLiteral("UAVCAN"));
    QCOMPARE(result.devtypeName, QStringLiteral("AIRSPEED_MS4525"));

    // Without a recognisable family every table is offered, like MP10.
    result = DeveloperToolParsers::DecodeHardwareId((0x09u << 16) | 4u);
    QCOMPARE(result.busTypeName, QStringLiteral("SITL"));
    QCOMPARE(result.devtypeName,
             QStringLiteral("AK09916 or BMI160 or BARO_KELLERLD or AIRSPEED_NMEA "));
    QVERIFY(result.text.startsWith(QStringLiteral(" devid 589828 bus type SITL")));

    // Unknown enum values print their number, like C# Enum.ToString().
    result = DeveloperToolParsers::DecodeHardwareId(
        (0x99u << 16) | 7u, QStringLiteral("COMPASS_DEV_ID"));
    QCOMPARE(result.busTypeName, QStringLiteral("7"));
    QCOMPARE(result.devtypeName, QStringLiteral("153"));

    QCOMPARE(DeveloperToolParsers::BusTypeName(0), QStringLiteral("UNKNOWN"));
    QCOMPARE(DeveloperToolParsers::BusTypeName(5), QStringLiteral("MSP"));
    QCOMPARE(DeveloperToolParsers::BusTypeName(6), QStringLiteral("SERIAL"));
    QCOMPARE(DeveloperToolParsers::CompassDeviceTypeName(0x19), QStringLiteral("LIS2MDL"));
    QCOMPARE(DeveloperToolParsers::ImuDeviceTypeName(0x3F), QStringLiteral("INS_ASM330"));
    QCOMPARE(DeveloperToolParsers::BaroDeviceTypeName(0x18), QStringLiteral("BARO_MS5837_02BA"));
    QCOMPARE(DeveloperToolParsers::AirspeedDeviceTypeName(0x0A), QStringLiteral("AIRSPEED_ASP5033"));
    QCOMPARE(DeveloperToolParsers::CompassDeviceTypeName(0x03), QStringLiteral("3"));
}

void DeveloperToolParsersTest::decodeHardwareIdRejectsInvalidInput()
{
    DecodedHardwareId result =
        DeveloperToolParsers::DecodeHardwareId(QStringLiteral("abc"));
    QVERIFY(!result.ok());
    QCOMPARE(result.error, QStringLiteral("The hardware ID is not a valid UInt32 value."));

    result = DeveloperToolParsers::DecodeHardwareId(QStringLiteral("-1"));
    QVERIFY(!result.ok());
    QCOMPARE(result.error, QStringLiteral("The hardware ID is not a valid UInt32 value."));

    result = DeveloperToolParsers::DecodeHardwareId(QString());
    QVERIFY(!result.ok());

    result = DeveloperToolParsers::DecodeHardwareId(QStringLiteral("4294967296"));
    QVERIFY(!result.ok());

    result = DeveloperToolParsers::DecodeHardwareId(QStringLiteral("0xZZ"));
    QVERIFY(!result.ok());
    QCOMPARE(result.error,
             QStringLiteral("The hardware ID is not a valid hexadecimal UInt32 value."));

    result = DeveloperToolParsers::DecodeHardwareId(QStringLiteral("0x"));
    QVERIFY(!result.ok());

    result = DeveloperToolParsers::DecodeHardwareId(QStringLiteral("0x1FFFFFFFF"));
    QVERIFY(!result.ok());

    result = DeveloperToolParsers::DecodeHardwareId(QStringLiteral("+5"));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.devid, 5u);

    result = DeveloperToolParsers::DecodeHardwareId(QStringLiteral(" 0xFFFFFFFF "));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.devid, 4294967295u);
}

QTEST_APPLESS_MAIN(DeveloperToolParsersTest)
#include "test_developertoolparsers.moc"
