#include "comm/TlogReader.h"
#include "ui/Loghandling/TelemetryLogAnonymizer.h"

#include <QBuffer>
#include <QtEndian>
#include <QtTest/QTest>

#include <mavlink_helpers.h>

#include <cstring>
#include <limits>

namespace
{

void checksum(mavlink_message_t *message, quint8 crcExtra)
{
    uchar header[9]{};
    int count = 0;
    header[count++] = message->len;
    if (message->magic == MAVLINK_STX_MAVLINK1) {
        header[count++] = message->seq;
        header[count++] = message->sysid;
        header[count++] = message->compid;
        header[count++] = static_cast<uchar>(message->msgid);
    } else {
        header[count++] = message->incompat_flags;
        header[count++] = message->compat_flags;
        header[count++] = message->seq;
        header[count++] = message->sysid;
        header[count++] = message->compid;
        header[count++] = static_cast<uchar>(message->msgid);
        header[count++] = static_cast<uchar>(message->msgid >> 8);
        header[count++] = static_cast<uchar>(message->msgid >> 16);
    }
    quint16 value = crc_calculate(header, count);
    crc_accumulate_buffer(&value, _MAV_PAYLOAD(message), message->len);
    crc_accumulate(crcExtra, &value);
    message->checksum = value;
}

mavlink_message_t framed(mavlink_message_t message, bool mavlink1,
                         quint8 sequence, bool signedFrame = false)
{
    message.magic = mavlink1 ? MAVLINK_STX_MAVLINK1 : MAVLINK_STX;
    message.seq = sequence;
    message.incompat_flags = !mavlink1 && signedFrame
        ? MAVLINK_IFLAG_SIGNED : 0;
    message.compat_flags = 0;
    if (mavlink1) {
        message.len = mavlink_min_message_length(&message);
    }
    checksum(&message, mavlink_get_crc_extra(&message));
    if (signedFrame) {
        for (int index = 0; index < MAVLINK_SIGNATURE_BLOCK_LEN; ++index) {
            message.signature[index] = static_cast<quint8>(0x70 + index);
        }
    }
    return message;
}

QByteArray frameBytes(const mavlink_message_t &message)
{
    const bool mavlink1 = message.magic == MAVLINK_STX_MAVLINK1;
    const bool signedFrame = !mavlink1
        && (message.incompat_flags & MAVLINK_IFLAG_SIGNED);
    const int header = mavlink1 ? 6 : 10;
    QByteArray bytes(header + message.len + 2
                         + (signedFrame ? MAVLINK_SIGNATURE_BLOCK_LEN : 0),
                     char(0));
    uchar *out = reinterpret_cast<uchar *>(bytes.data());
    out[0] = message.magic;
    out[1] = message.len;
    if (mavlink1) {
        out[2] = message.seq;
        out[3] = message.sysid;
        out[4] = message.compid;
        out[5] = static_cast<uchar>(message.msgid);
    } else {
        out[2] = message.incompat_flags;
        out[3] = message.compat_flags;
        out[4] = message.seq;
        out[5] = message.sysid;
        out[6] = message.compid;
        out[7] = static_cast<uchar>(message.msgid);
        out[8] = static_cast<uchar>(message.msgid >> 8);
        out[9] = static_cast<uchar>(message.msgid >> 16);
    }
    std::memcpy(out + header, _MAV_PAYLOAD(&message), message.len);
    out[header + message.len] = static_cast<uchar>(message.checksum);
    out[header + message.len + 1] =
        static_cast<uchar>(message.checksum >> 8);
    if (signedFrame) {
        std::memcpy(out + header + message.len + 2,
                    message.signature, MAVLINK_SIGNATURE_BLOCK_LEN);
    }
    return bytes;
}

QByteArray timestampBytes(quint64 timestamp)
{
    QByteArray bytes(8, char(0));
    qToBigEndian<quint64>(
        timestamp, reinterpret_cast<uchar *>(bytes.data()));
    return bytes;
}

QByteArray recordBytes(quint64 timestamp, const mavlink_message_t &message)
{
    return timestampBytes(timestamp) + frameBytes(message);
}

mavlink_message_t globalPosition(qint32 latitude, qint32 longitude,
                                 bool mavlink1, quint8 sequence,
                                 bool signedFrame = false)
{
    mavlink_global_position_int_t payload{};
    payload.time_boot_ms = 1234;
    payload.lat = latitude;
    payload.lon = longitude;
    payload.alt = 12000;
    payload.relative_alt = 3000;
    mavlink_message_t message{};
    mavlink_msg_global_position_int_encode(17, 42, &message, &payload);
    return framed(message, mavlink1, sequence, signedFrame);
}

mavlink_message_t simState(float latitude, float longitude,
                           quint8 sequence)
{
    mavlink_sim_state_t payload{};
    payload.lat = latitude;
    payload.lon = longitude;
    mavlink_message_t message{};
    mavlink_msg_sim_state_encode(23, 9, &message, &payload);
    return framed(message, false, sequence);
}

mavlink_message_t fencePoint(float latitude, float longitude,
                             quint8 sequence)
{
    mavlink_fence_point_t payload{};
    payload.lat = latitude;
    payload.lng = longitude;
    payload.target_system = 1;
    payload.target_component = 1;
    mavlink_message_t message{};
    mavlink_msg_fence_point_encode(31, 7, &message, &payload);
    return framed(message, false, sequence);
}

mavlink_message_t heartbeat(quint8 sequence, bool signedFrame = false)
{
    mavlink_heartbeat_t payload{};
    payload.type = MAV_TYPE_GENERIC;
    payload.autopilot = MAV_AUTOPILOT_GENERIC;
    payload.system_status = MAV_STATE_ACTIVE;
    payload.mavlink_version = 3;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(1, 1, &message, &payload);
    return framed(message, false, sequence, signedFrame);
}

mavlink_message_t missionItem(quint8 frame, quint16 command,
                              float latitudeOrX, float longitudeOrY,
                              quint8 sequence, float param1 = 0.0F)
{
    mavlink_mission_item_t payload{};
    payload.frame = frame;
    payload.command = command;
    payload.param1 = param1;
    payload.x = latitudeOrX;
    payload.y = longitudeOrY;
    payload.target_system = 1;
    payload.target_component = 1;
    mavlink_message_t message{};
    mavlink_msg_mission_item_encode(7, 8, &message, &payload);
    return framed(message, false, sequence);
}

mavlink_message_t missionItemInt(quint8 frame, quint16 command,
                                 qint32 latitudeOrX, qint32 longitudeOrY,
                                 quint8 sequence, float param1 = 0.0F)
{
    mavlink_mission_item_int_t payload{};
    payload.frame = frame;
    payload.command = command;
    payload.param1 = param1;
    payload.x = latitudeOrX;
    payload.y = longitudeOrY;
    payload.target_system = 1;
    payload.target_component = 1;
    mavlink_message_t message{};
    mavlink_msg_mission_item_int_encode(7, 8, &message, &payload);
    return framed(message, false, sequence);
}

mavlink_message_t partiallyTrimmedAhrs2(quint8 sequence)
{
    mavlink_ahrs2_t payload{};
    payload.lng = 1;
    mavlink_message_t message{};
    mavlink_msg_ahrs2_encode(9, 10, &message, &payload);
    message = framed(message, false, sequence);
    // lng starts at byte 20.  A MAVLink 2 sender legitimately trims its three
    // zero high bytes while retaining the non-zero low byte.
    message.len = 21;
    checksum(&message, mavlink_get_crc_extra(&message));
    return message;
}

LogAnonymizeResult anonymizeTlog(
    const QByteArray &source, QByteArray *destination,
    const LogAnonymizeOptions &options,
    const LogAnonymizeCancel &cancel = {})
{
    QBuffer input;
    input.setData(source);
    input.open(QIODevice::ReadOnly);
    QBuffer output(destination);
    output.open(QIODevice::WriteOnly);
    return TelemetryLogAnonymizer::anonymizeTlog(
        &input, &output, options, cancel);
}

LogAnonymizeResult anonymizeText(
    const QByteArray &source, QByteArray *destination,
    const LogAnonymizeOptions &options,
    const LogAnonymizeCancel &cancel = {})
{
    QBuffer input;
    input.setData(source);
    input.open(QIODevice::ReadOnly);
    QBuffer output(destination);
    output.open(QIODevice::WriteOnly);
    return TelemetryLogAnonymizer::anonymizeText(
        &input, &output, options, cancel);
}

} // namespace

class TelemetryLogAnonymizerTest final : public QObject
{
    Q_OBJECT

private slots:
    void textUsesFmtColumnsAndPreservesLineEndings();
    void textRejectsMalformedCoordinatesAndCancelsBoundedly();
    void tlogPatchesIndependentIntegerOffsetsInV1AndV2();
    void tlogPatchesReferenceFloatAndLngFields();
    void globalLocationMissionItemsPatchButLocalAndNonLocationStayUnchanged();
    void truncatedMavlink2CoordinateScalarIsMaterialized();
    void zeroAndUnblockedFramesRemainByteIdentical();
    void modifiedSignedFramesAreStrippedButUnchangedSignaturesRemain();
    void corruptTruncatedAndUnknownDialectInputsFailClosed();
    void rejectsInvalidDevicesAndNonFiniteOffsets();
};

void TelemetryLogAnonymizerTest::textUsesFmtColumnsAndPreservesLineEndings()
{
    const QByteArray source =
        "FMT, 1, 24, GPS, Qdd, TimeUS, Lat, Lng\r\n"
        "GPS, 100, 34.25, -117.5\r\n"
        "GPS, 200, 0, 0\n"
        "OTHER,keep,bytes";
    QByteArray output;
    const LogAnonymizeResult result = anonymizeText(
        source, &output, {1.5, -2.25});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.records, qint64(4));
    QCOMPARE(result.coordinateFields, qint64(4));
    QCOMPARE(result.patchedValues, qint64(2));
    QCOMPARE(output,
        QByteArray("FMT, 1, 24, GPS, Qdd, TimeUS, Lat, Lng\r\n"
                   "GPS, 100, 35.75, -119.75\r\n"
                   "GPS, 200, 0, 0\n"
                   "OTHER,keep,bytes"));
}

void TelemetryLogAnonymizerTest::textRejectsMalformedCoordinatesAndCancelsBoundedly()
{
    const QByteArray malformed =
        "FMT,1,20,GPS,Qd,TimeUS,Lat\nGPS,1,not-a-number\n";
    QByteArray output;
    const LogAnonymizeResult failed = anonymizeText(
        malformed, &output, {1.0, 1.0});
    QVERIFY(!failed.success);
    QVERIFY(!failed.cancelled);
    QVERIFY(failed.error.contains(QStringLiteral("malformed"),
                                  Qt::CaseInsensitive));

    int checks = 0;
    output.clear();
    const QByteArray cancellable =
        "FMT,1,20,GPS,Qd,TimeUS,Lat\nGPS,1,12.0\nGPS,2,13.0\n";
    const LogAnonymizeResult cancelled = anonymizeText(
        cancellable, &output, {1.0, 1.0}, [&checks]() {
            return ++checks >= 3;
        });
    QVERIFY(!cancelled.success);
    QVERIFY(cancelled.cancelled);
    QVERIFY(output.size() < cancellable.size());
}

void TelemetryLogAnonymizerTest::tlogPatchesIndependentIntegerOffsetsInV1AndV2()
{
    const mavlink_message_t v1 = globalPosition(
        340000000, -1170000000, true, 77);
    const mavlink_message_t v2 = globalPosition(
        -120000000, 450000000, false, 201);
    const QByteArray source = recordBytes(0x0102030405060708ULL, v1)
        + recordBytes(0x1112131415161718ULL, v2);
    QByteArray output;
    const LogAnonymizeResult result = anonymizeTlog(
        source, &output, {1.25, -2.5});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.records, qint64(2));
    QCOMPARE(result.coordinateFields, qint64(4));
    QCOMPARE(result.patchedValues, qint64(4));

    QBuffer buffer(&output);
    buffer.open(QIODevice::ReadOnly);
    TlogReader reader(&buffer);
    TlogRecord first;
    QCOMPARE(reader.next(&first), TlogReader::Status::Ok);
    QCOMPARE(first.timestampUsec, qint64(0x0102030405060708ULL));
    QCOMPARE(first.message.magic, quint8(MAVLINK_STX_MAVLINK1));
    QCOMPARE(first.message.seq, quint8(77));
    QCOMPARE(first.message.sysid, quint8(17));
    QCOMPARE(first.message.compid, quint8(42));
    mavlink_global_position_int_t firstPayload{};
    mavlink_msg_global_position_int_decode(&first.message, &firstPayload);
    QCOMPARE(firstPayload.lat, qint32(352500000));
    QCOMPARE(firstPayload.lon, qint32(-1195000000));

    TlogRecord second;
    QCOMPARE(reader.next(&second), TlogReader::Status::Ok);
    QCOMPARE(second.timestampUsec, qint64(0x1112131415161718ULL));
    QCOMPARE(second.message.magic, quint8(MAVLINK_STX));
    QCOMPARE(second.message.seq, quint8(201));
    mavlink_global_position_int_t secondPayload{};
    mavlink_msg_global_position_int_decode(&second.message, &secondPayload);
    QCOMPARE(secondPayload.lat, qint32(-107500000));
    QCOMPARE(secondPayload.lon, qint32(425000000));
    QCOMPARE(reader.next(&second), TlogReader::Status::End);
}

void TelemetryLogAnonymizerTest::tlogPatchesReferenceFloatAndLngFields()
{
    const mavlink_message_t sim = simState(12.5F, -44.25F, 9);
    const mavlink_message_t fence = fencePoint(8.0F, 19.0F, 10);
    const QByteArray source = recordBytes(100, sim) + recordBytes(200, fence);
    QByteArray output;
    const LogAnonymizeResult result = anonymizeTlog(
        source, &output, {0.75, -1.5});
    QVERIFY2(result.success, qPrintable(result.error));
    // Qt intentionally closes Privacy.cs's legacy "lng" coverage hole.
    QCOMPARE(result.coordinateFields, qint64(4));
    QCOMPARE(result.patchedValues, qint64(4));

    QBuffer buffer(&output);
    buffer.open(QIODevice::ReadOnly);
    TlogReader reader(&buffer);
    TlogRecord record;
    QCOMPARE(reader.next(&record), TlogReader::Status::Ok);
    mavlink_sim_state_t simPayload{};
    mavlink_msg_sim_state_decode(&record.message, &simPayload);
    QCOMPARE(simPayload.lat, 13.25F);
    QCOMPARE(simPayload.lon, -45.75F);
    QCOMPARE(reader.next(&record), TlogReader::Status::Ok);
    mavlink_fence_point_t fencePayload{};
    mavlink_msg_fence_point_decode(&record.message, &fencePayload);
    QCOMPARE(fencePayload.lat, 8.75F);
    QCOMPARE(fencePayload.lng, 17.5F);
}

void TelemetryLogAnonymizerTest::globalLocationMissionItemsPatchButLocalAndNonLocationStayUnchanged()
{
    const mavlink_message_t globalFloat = missionItem(
        MAV_FRAME_GLOBAL_RELATIVE_ALT, MAV_CMD_NAV_WAYPOINT,
        34.5F, -117.25F, 20);
    const mavlink_message_t globalInt = missionItemInt(
        MAV_FRAME_GLOBAL_RELATIVE_ALT_INT, MAV_CMD_NAV_WAYPOINT,
        345000000, -1172500000, 21);
    const mavlink_message_t local = missionItemInt(
        MAV_FRAME_LOCAL_NED, MAV_CMD_NAV_WAYPOINT,
        123400, -567800, 22);
    const mavlink_message_t nonLocation = missionItemInt(
        MAV_FRAME_GLOBAL_INT, MAV_CMD_NAV_RETURN_TO_LAUNCH,
        111000000, 222000000, 23);
    const QByteArray source = recordBytes(100, globalFloat)
        + recordBytes(200, globalInt) + recordBytes(300, local)
        + recordBytes(400, nonLocation);
    QByteArray output;
    const LogAnonymizeResult result = anonymizeTlog(
        source, &output, {1.0, -2.0});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.coordinateFields, qint64(4));
    QCOMPARE(result.patchedValues, qint64(4));

    QBuffer buffer(&output);
    buffer.open(QIODevice::ReadOnly);
    TlogReader reader(&buffer);
    TlogRecord record;
    QCOMPARE(reader.next(&record), TlogReader::Status::Ok);
    mavlink_mission_item_t decodedFloat{};
    mavlink_msg_mission_item_decode(&record.message, &decodedFloat);
    QCOMPARE(decodedFloat.x, 35.5F);
    QCOMPARE(decodedFloat.y, -119.25F);

    QCOMPARE(reader.next(&record), TlogReader::Status::Ok);
    mavlink_mission_item_int_t decodedInt{};
    mavlink_msg_mission_item_int_decode(&record.message, &decodedInt);
    QCOMPARE(decodedInt.x, qint32(355000000));
    QCOMPARE(decodedInt.y, qint32(-1192500000));

    QCOMPARE(reader.next(&record), TlogReader::Status::Ok);
    mavlink_msg_mission_item_int_decode(&record.message, &decodedInt);
    QCOMPARE(decodedInt.x, qint32(123400));
    QCOMPARE(decodedInt.y, qint32(-567800));

    QCOMPARE(reader.next(&record), TlogReader::Status::Ok);
    mavlink_msg_mission_item_int_decode(&record.message, &decodedInt);
    QCOMPARE(decodedInt.x, qint32(111000000));
    QCOMPARE(decodedInt.y, qint32(222000000));
}

void TelemetryLogAnonymizerTest::truncatedMavlink2CoordinateScalarIsMaterialized()
{
    const mavlink_message_t inputMessage = partiallyTrimmedAhrs2(31);
    QCOMPARE(inputMessage.len, quint8(21));
    QByteArray output;
    const LogAnonymizeResult result = anonymizeTlog(
        recordBytes(0x0102030405060708ULL, inputMessage),
        &output, {0.0, 1.0});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.coordinateFields, qint64(2));
    QCOMPARE(result.patchedValues, qint64(1));

    QBuffer buffer(&output);
    buffer.open(QIODevice::ReadOnly);
    TlogReader reader(&buffer);
    TlogRecord record;
    QCOMPARE(reader.next(&record), TlogReader::Status::Ok);
    QCOMPARE(record.timestampUsec, qint64(0x0102030405060708ULL));
    QCOMPARE(record.message.seq, quint8(31));
    QCOMPARE(record.message.len, quint8(23));
    mavlink_ahrs2_t decoded{};
    mavlink_msg_ahrs2_decode(&record.message, &decoded);
    QCOMPARE(decoded.lat, qint32(0));
    QCOMPARE(decoded.lng, qint32(10000001));
}

void TelemetryLogAnonymizerTest::zeroAndUnblockedFramesRemainByteIdentical()
{
    const mavlink_message_t zero = globalPosition(0, 0, false, 5);
    mavlink_position_target_global_int_t target{};
    target.lat_int = 350000000;
    target.lon_int = -1180000000;
    mavlink_message_t unblocked{};
    mavlink_msg_position_target_global_int_encode(2, 3, &unblocked, &target);
    unblocked = framed(unblocked, false, 6);
    const mavlink_message_t ordinary = heartbeat(7);
    const QByteArray source = recordBytes(10, zero)
        + recordBytes(20, unblocked) + recordBytes(30, ordinary);
    QByteArray output;
    const LogAnonymizeResult result = anonymizeTlog(
        source, &output, {1.0, 2.0});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.coordinateFields, qint64(2)); // Only the in-scope zero GPS pair.
    QVERIFY(result.warnings.join(QLatin1Char('\n')).contains(QStringLiteral("POSITION_TARGET_GLOBAL_INT")));
    QCOMPARE(result.patchedValues, qint64(0));
    QCOMPARE(output, source);
}

void TelemetryLogAnonymizerTest::modifiedSignedFramesAreStrippedButUnchangedSignaturesRemain()
{
    const mavlink_message_t unchanged = heartbeat(41, true);
    const mavlink_message_t modified = globalPosition(
        100000000, 200000000, false, 42, true);
    const QByteArray unchangedRecord = recordBytes(123, unchanged);
    const QByteArray source = unchangedRecord + recordBytes(456, modified);
    QByteArray output;
    const LogAnonymizeResult result = anonymizeTlog(
        source, &output, {1.0, -1.0});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.strippedSignatures, qint64(1));
    QCOMPARE(result.warnings.size(), 1);
    QVERIFY(result.warnings.first().contains(
        QStringLiteral("not verified"), Qt::CaseInsensitive));
    QVERIFY(output.startsWith(unchangedRecord));
    QCOMPARE(output.size(), source.size() - MAVLINK_SIGNATURE_BLOCK_LEN);

    QBuffer buffer(&output);
    buffer.open(QIODevice::ReadOnly);
    TlogReader reader(&buffer);
    TlogRecord record;
    QCOMPARE(reader.next(&record), TlogReader::Status::Ok);
    QVERIFY(record.message.incompat_flags & MAVLINK_IFLAG_SIGNED);
    QCOMPARE(reader.next(&record), TlogReader::Status::Ok);
    QVERIFY(!(record.message.incompat_flags & MAVLINK_IFLAG_SIGNED));
    QCOMPARE(record.message.seq, quint8(42));
}

void TelemetryLogAnonymizerTest::corruptTruncatedAndUnknownDialectInputsFailClosed()
{
    QByteArray corrupt = recordBytes(
        12, globalPosition(100000000, 200000000, false, 1));
    const int corruptIndex = corrupt.size() - 1;
    corrupt[corruptIndex] = char(corrupt.at(corruptIndex) ^ 0x55);
    QByteArray output;
    LogAnonymizeResult result = anonymizeTlog(
        corrupt, &output, {1.0, 1.0});
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("corrupt"),
                                  Qt::CaseInsensitive)
            || result.error.contains(QStringLiteral("trailing"),
                                     Qt::CaseInsensitive));

    output.clear();
    result = anonymizeTlog(corrupt.left(12), &output, {1.0, 1.0});
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());

    mavlink_message_t unknown{};
    unknown.magic = MAVLINK_STX;
    unknown.len = 1;
    unknown.seq = 4;
    unknown.sysid = 5;
    unknown.compid = 6;
    unknown.msgid = 41999;
    _MAV_PAYLOAD_NON_CONST(&unknown)[0] = char(0x33);
    checksum(&unknown, 0);
    output.clear();
    result = anonymizeTlog(recordBytes(99, unknown), &output, {1.0, 1.0});
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("dialect"),
                                  Qt::CaseInsensitive));

    mavlink_message_t shortV1 = heartbeat(5);
    shortV1.magic = MAVLINK_STX_MAVLINK1;
    shortV1.incompat_flags = 0;
    shortV1.compat_flags = 0;
    shortV1.len = mavlink_min_message_length(&shortV1) - 1;
    checksum(&shortV1, mavlink_get_crc_extra(&shortV1));
    output.clear();
    result = anonymizeTlog(recordBytes(100, shortV1), &output, {});
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());

    mavlink_message_t longV2 = heartbeat(6);
    longV2.len = mavlink_max_message_length(&longV2) + 1;
    _MAV_PAYLOAD_NON_CONST(&longV2)[longV2.len - 1] = char(0x42);
    checksum(&longV2, mavlink_get_crc_extra(&longV2));
    output.clear();
    result = anonymizeTlog(recordBytes(101, longV2), &output, {});
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
}

void TelemetryLogAnonymizerTest::rejectsInvalidDevicesAndNonFiniteOffsets()
{
    QByteArray source("FMT,1,1,X,B,A\n");
    QBuffer input(&source);
    input.open(QIODevice::ReadOnly);
    QByteArray destination;
    QBuffer output(&destination);
    output.open(QIODevice::WriteOnly);

    LogAnonymizeOptions invalid;
    invalid.latitudeOffset = std::numeric_limits<double>::infinity();
    LogAnonymizeResult result = TelemetryLogAnonymizer::anonymizeText(
        &input, &output, invalid);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("finite"),
                                  Qt::CaseInsensitive));

    result = TelemetryLogAnonymizer::anonymizeText(
        &input, &input, {});
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("different"),
                                  Qt::CaseInsensitive));
}

QTEST_MAIN(TelemetryLogAnonymizerTest)
#include "test_telemetryloganonymizer.moc"
