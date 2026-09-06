#include "comm/TlogMatlabExporter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
QByteArray record(qint64 usec, const mavlink_message_t &message) {
    QByteArray bytes(8, '\0'); qToBigEndian<quint64>(quint64(usec), reinterpret_cast<uchar *>(bytes.data()));
    char packet[MAVLINK_MAX_PACKET_LEN]{};
    const int length = mavlink_msg_to_send_buffer(reinterpret_cast<quint8 *>(packet), &message);
    return bytes + QByteArray(packet, length);
}
mavlink_message_t attitude(float roll, quint8 system = 1, quint8 component = 1, bool v1 = false) {
    mavlink_message_t message{};
    mavlink_msg_attitude_pack(system, component, &message, 1234, roll, -2.5f, 3.25f, 4, 5, 6);
    mavlink_status_t status{};
    if (v1) status.flags = MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_finalize_message_buffer(&message, system, component, &status,
        MAVLINK_MSG_ID_ATTITUDE_MIN_LEN, MAVLINK_MSG_ID_ATTITUDE_LEN, MAVLINK_MSG_ID_ATTITUDE_CRC);
    return message;
}
bool save(const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
}
QStringList directoryEntries(const QString &path) {
    return QDir(path).entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}
struct Matrix { int rows = 0, columns = 0; QVector<double> values; };
struct ExtensionFieldFixture {
    const char *name;
    int offset;
    QByteArray wire;
    double expected;
};
struct ExtensionMessageFixture {
    const char *name;
    quint32 id;
    quint8 originalLength, extendedLength, crc;
    QVector<ExtensionFieldFixture> fields;
};
// Independent MP10 wire fixtures: literal little-endian bytes, not the
// exporter's metadata table or generated C structs from the older dialect.
QVector<ExtensionMessageFixture> extensionFixtures() {
    return {
        {"AUTOPILOT_STATE_FOR_GIMBAL_DEVICE", 286, 53, 57, 210,
            {{"angular_velocity_z", 53, QByteArray::fromHex("000048c1"), -12.5}}},
        {"CAMERA_INFORMATION", 259, 235, 236, 92,
            {{"gimbal_device_id", 235, QByteArray::fromHex("d7"), 215}}},
        {"EFI_STATUS", 225, 69, 73, 208,
            {{"fuel_pressure", 69, QByteArray::fromHex("000048c1"), -12.5}}},
        {"GIMBAL_DEVICE_ATTITUDE_STATUS", 285, 40, 49, 137,
            {{"delta_yaw", 40, QByteArray::fromHex("000048c1"), -12.5},
             {"delta_yaw_velocity", 44, QByteArray::fromHex("00009a42"), 77},
             {"gimbal_device_id", 48, QByteArray::fromHex("d7"), 215}}},
        {"GIMBAL_DEVICE_INFORMATION", 283, 144, 145, 74,
            {{"gimbal_device_id", 144, QByteArray::fromHex("d7"), 215}}},
        {"MANUAL_CONTROL", 69, 11, 30, 243,
            {{"buttons2", 11, QByteArray::fromHex("cdab"), 43981},
             {"enabled_extensions", 13, QByteArray::fromHex("ff"), 255},
             {"s", 14, QByteArray::fromHex("2efb"), -1234},
             {"t", 16, QByteArray::fromHex("d204"), 1234},
             {"aux1", 18, QByteArray::fromHex("ffff"), -1},
             {"aux2", 20, QByteArray::fromHex("0080"), -32768},
             {"aux3", 22, QByteArray::fromHex("ff7f"), 32767},
             {"aux4", 24, QByteArray::fromHex("feff"), -2},
             {"aux5", 26, QByteArray::fromHex("fe7f"), 32766},
             {"aux6", 28, QByteArray::fromHex("0180"), -32767}}},
        {"MISSION_CURRENT", 42, 2, 6, 28,
            {{"total", 2, QByteArray::fromHex("cdab"), 43981},
             {"mission_state", 4, QByteArray::fromHex("03"), 3},
             {"mission_mode", 5, QByteArray::fromHex("02"), 2}}},
        {"ODOMETRY", 331, 232, 233, 91,
            {{"quality", 232, QByteArray::fromHex("d6"), -42}}},
        {"SIM_STATE", 108, 84, 92, 32,
            {{"lat_int", 84, QByteArray::fromHex("eb32a4f8"), -123456789},
             {"lon_int", 88, QByteArray::fromHex("15cd5b07"), 123456789}}},
        {"VIDEO_STREAM_INFORMATION", 269, 213, 214, 109,
            {{"encoding", 213, QByteArray::fromHex("02"), 2}}}
    };
}
quint32 u32(const QByteArray &bytes, int at) {
    return at >= 0 && at + 4 <= bytes.size()
        ? qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + at)) : 0;
}
// Independent small-fixture MAT-v5 reader, deliberately not MatFileWriter.
QMap<QString, Matrix> matrices(const QByteArray &bytes) {
    QMap<QString, Matrix> result;
    if (bytes.size() < 128 || bytes.mid(126, 2) != "IM") return {};
    int position = 128;
    while (position < bytes.size()) {
        if (u32(bytes, position) != 14) return {};
        const quint32 count = u32(bytes, position + 4);
        if (count > quint32(bytes.size() - position - 8)) return {};
        const int end = position + 8 + int(count); position += 8;
        QString name; Matrix matrix; bool real = false;
        while (position < end) {
            if (position + 8 > end) return {};
            const quint32 type = u32(bytes, position), length = u32(bytes, position + 4);
            if (length > quint32(end - position - 8)) return {};
            position += 8;
            if (type == 5) {
                if (length != 8) return {};
                matrix.rows = int(u32(bytes, position)); matrix.columns = int(u32(bytes, position + 4));
            } else if (type == 1) name = QString::fromLatin1(bytes.constData() + position, int(length));
            else if (type == 9) {
                if (length % 8 || qint64(matrix.rows) * matrix.columns != length / 8) return {};
                for (quint32 offset = 0; offset < length; offset += 8) {
                    const quint64 bits = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(bytes.constData() + position + offset));
                    double value; std::memcpy(&value, &bits, sizeof(value)); matrix.values.append(value);
                }
                real = true;
            }
            position += int((length + 7) & ~quint32(7));
        }
        if (position != end || name.isEmpty() || !real || result.contains(name)) return {};
        result.insert(name, matrix);
    }
    return result;
}
}

class TlogMatlabExporterTest final : public QObject {
    Q_OBJECT
private slots:
    void localDateQuirkAndMilliseconds_data() {
        QTest::addColumn<QDate>("date"); QTest::addColumn<int>("extraDay");
        QTest::newRow("ordinary") << QDate(2025, 6, 1) << 0;
        QTest::newRow("year-before-leap") << QDate(2023, 6, 1) << 1;
        QTest::newRow("leap-before-february") << QDate(2024, 1, 15) << 1;
        QTest::newRow("leap-after-february") << QDate(2024, 3, 1) << 0;
        QTest::newRow("clamped-february29") << QDate(2028, 2, 29) << 0;
        QTest::newRow("winter-local-offset") << QDate(2025, 1, 10) << 0;
    }
    void localDateQuirkAndMilliseconds() {
        QFETCH(QDate, date); QFETCH(int, extraDay);
        const QDateTime local(date, QTime(12, 0, 0, 123), Qt::LocalTime);
        const qint64 usec = local.toMSecsSinceEpoch() * 1000;
        const double expected = 719529.0 + QDate(1970, 1, 1).daysTo(date)
            + extraDay + (12 * 3600000.0 + 123) / 86400000.0;
        QVERIFY(std::abs(TlogMatlabExporter::mp10SerialDate(usec) - expected) < 2e-10);
        QCOMPARE(TlogMatlabExporter::mp10SerialDate(usec + 999), TlogMatlabExporter::mp10SerialDate(usec));
        QCOMPARE(TlogMatlabExporter::mp10SerialDate(-1), 367.0);
        QCOMPARE(TlogMatlabExporter::mp10SerialDate(9999999LL * 3600 * 1000000), 367.0);
    }
    void scalarSourcesProtocolsArraysAndSigning() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString input = directory.filePath("flight.tlog"), output = directory.filePath("flight.mat");
        const qint64 first = QDateTime(QDate(2025, 6, 1), QTime(12, 0), Qt::UTC).toMSecsSinceEpoch() * 1000;
        QByteArray data = record(first + 999, attitude(1.5f, 1, 1, true));
        data += record(first - 1000000, attitude(-7, 255, 190)); // GCS non-heartbeat retained; raw backwards time.
        mavlink_message_t heartbeat{};
        mavlink_msg_heartbeat_pack(255, 190, &heartbeat, MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
        data += record(first, heartbeat);
        mavlink_message_t named{};
        mavlink_msg_named_value_float_pack(42, 100, &named, 777, "PRIVATE-ID", 8.5f);
        data += record(first, named);
        mavlink_message_t signing{};
        const QByteArray secret("SECRET_SIGNING_KEY_NOT_FOR_EXPORT", 32);
        mavlink_msg_setup_signing_pack(255, 190, &signing, 7, 1,
            reinterpret_cast<const quint8 *>(secret.constData()), 123456);
        data += record(first, signing);
        QVERIFY(save(input, data));
        const auto result = TlogMatlabExporter::Export(input, output);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.recordsRead, qint64(5));
        QCOMPARE(result.outputPaths, QStringList{output});
        const auto bytes = read(output); const auto parsed = matrices(bytes);
        QCOMPARE(parsed.size(), result.itemCount); QCOMPARE(parsed.size(), 12);
        QVERIFY(!bytes.contains(secret)); QVERIFY(!bytes.contains("PRIVATE-ID"));
        QVERIFY(!parsed.contains("name_mavlink_named_value_float_t"));
        QVERIFY(!parsed.contains("secret_key_mavlink_setup_signing_t"));
        QVERIFY(!parsed.contains("type_mavlink_heartbeat_t"));
        const auto roll = parsed.value("roll_mavlink_attitude_t");
        QCOMPARE(roll.rows, 2); QCOMPARE(roll.columns, 2);
        QCOMPARE(roll.values, QVector<double>({TlogMatlabExporter::mp10SerialDate(first),
            TlogMatlabExporter::mp10SerialDate(first - 1000000), 1.5, -7}));
        QCOMPARE(parsed.value("initial_timestamp_mavlink_setup_signing_t").values.last(), 123456.0);
        QCOMPARE(read(input), data);
        QCOMPARE(directoryEntries(directory.path()), QStringList({"flight.mat", "flight.tlog"}));
    }
    void trimmedAndNonfiniteValuesRemainNumeric() {
        QTemporaryDir directory; const auto input = directory.filePath("input.tlog");
        mavlink_message_t zero{}, special{};
        mavlink_msg_attitude_pack(1, 1, &zero, 0, 0, 0, 0, 0, 0, 0);
        QVERIFY(zero.len < MAVLINK_MSG_ID_ATTITUDE_LEN);
        special = attitude(std::numeric_limits<float>::quiet_NaN());
        QVERIFY(save(input, record(1700000000000000LL, zero) + record(1700000001000000LL, special)));
        const auto output = directory.filePath("out.mat");
        const auto result = TlogMatlabExporter::Export(input, output); QVERIFY2(result.success, qPrintable(result.error));
        const auto roll = matrices(read(output)).value("roll_mavlink_attitude_t");
        QCOMPARE(roll.rows, 2); QCOMPARE(roll.values.at(2), 0.0); QVERIFY(std::isnan(roll.values.at(3)));
    }
    void chunkFlushAndCorruptTail() {
        QTemporaryDir directory; const auto input = directory.filePath("input.tlog"), output = directory.filePath("out.mat");
        QByteArray data;
        for (int row = 0; row < 8300; ++row) data += record(1700000000000000LL + row * 1000LL, attitude(float(row)));
        data += "broken trailing bytes";
        QVERIFY(save(input, data));
        const auto result = TlogMatlabExporter::Export(input, output);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.recordsRead, qint64(8300));
        QVERIFY(result.skippedBytes > 0); QVERIFY(result.message.contains("Skipped"));
        const auto roll = matrices(read(output)).value("roll_mavlink_attitude_t");
        QCOMPARE(roll.rows, 8300); QCOMPARE(roll.values.at(8300 + 8191), 8191.0);
        QCOMPARE(roll.values.at(8300 + 8192), 8192.0); QCOMPARE(roll.values.last(), 8299.0);
    }
    void emptyAndGcsOnly() {
        QTemporaryDir directory; const auto input = directory.filePath("empty.tlog"); QVERIFY(save(input, {}));
        const auto output = directory.filePath("out.mat");
        auto result = TlogMatlabExporter::Export(input, output);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.itemCount, 0); QCOMPARE(read(output).size(), 128);
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(255, 190, &message, MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
        QVERIFY(save(input, record(0, message)));
        result = TlogMatlabExporter::Export(input, directory.filePath("gcs.mat"));
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.itemCount, 0); QCOMPARE(result.recordsRead, qint64(1));
    }
    void integerPrecisionAndRejectedCrc() {
        QTemporaryDir directory; const auto input = directory.filePath("input.tlog"), output = directory.filePath("out.mat");
        auto damaged = record(1700000000000000LL, attitude(77));
        damaged[damaged.size() - 1] = char(damaged.at(damaged.size() - 1) ^ 0x55);
        mavlink_message_t message{};
        mavlink_msg_system_time_pack(9, 100, &message, quint64(9007199254740993ULL), 123);
        QVERIFY(save(input, damaged + record(1700000000000000LL, message)));
        const auto result = TlogMatlabExporter::Export(input, output);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.recordsRead, qint64(1));
        QVERIFY(result.skippedBytes >= damaged.size());
        const auto parsed = matrices(read(output)); QCOMPARE(parsed.size(), 2);
        QCOMPARE(parsed.value("time_unix_usec_mavlink_system_time_t").values.last(), 9007199254740992.0);
        QCOMPARE(parsed.value("time_boot_ms_mavlink_system_time_t").values.last(), 123.0);
    }
    void missionCurrentOfflineExtensionSchema() {
        QTemporaryDir directory; const auto input = directory.filePath("input.tlog"), output = directory.filePath("out.mat");
        // Hand-built MP10 wire layout, not the old generated two-byte struct.
        const auto message = [](quint16 sequence, quint16 total, quint8 state,
                                quint8 mode, int length, bool v1) {
            mavlink_message_t wire{}; wire.msgid = MAVLINK_MSG_ID_MISSION_CURRENT;
            auto *bytes = reinterpret_cast<uchar *>(_MAV_PAYLOAD_NON_CONST(&wire));
            qToLittleEndian<quint16>(sequence, bytes);
            qToLittleEndian<quint16>(total, bytes + 2);
            bytes[4] = state; bytes[5] = mode;
            mavlink_status_t status{};
            if (v1) status.flags = MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
            mavlink_finalize_message_buffer(&wire, 8, 1, &status, 2,
                                            quint8(length), MAVLINK_MSG_ID_MISSION_CURRENT_CRC);
            return wire;
        };
        const auto full = message(513, 0x1234, 3, 2, 6, false);
        QCOMPARE(int(full.len), 6);
        const auto shortV2 = message(7, 0, 0, 0, 2, false);
        QVERIFY(shortV2.len <= 2);
        // Bytes outside the v1 two-byte payload must never leak from the
        // constructor's local buffer into extension values in the export.
        const auto v1 = message(9, 0xffff, 4, 2, 6, true);
        QCOMPARE(int(v1.len), 2);
        const auto partial = message(10, 0x78, 0, 0, 3, false);
        QCOMPARE(int(partial.len), 3);
        const qint64 time = 1700000000000000LL;
        QVERIFY(save(input, record(time, full) + record(time, shortV2)
            + record(time, v1) + record(time, partial)));
        const auto result = TlogMatlabExporter::Export(input, output);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.recordsRead, qint64(4));
        QCOMPARE(result.skippedBytes, qint64(0)); QCOMPARE(result.itemCount, 4);
        const auto parsed = matrices(read(output)); QCOMPARE(parsed.size(), 4);
        const auto values = [&](const char *name) { return parsed.value(QLatin1String(name)).values.mid(4); };
        QCOMPARE(values("seq_mavlink_mission_current_t"), QVector<double>({513, 7, 9, 10}));
        QCOMPARE(values("total_mavlink_mission_current_t"), QVector<double>({4660, 0, 0, 120}));
        QCOMPARE(values("mission_state_mavlink_mission_current_t"), QVector<double>({3, 0, 0, 0}));
        QCOMPARE(values("mission_mode_mavlink_mission_current_t"), QVector<double>({2, 0, 0, 0}));
    }
    void gimbalStateOfflineExtensionAndLongName() {
        QTemporaryDir directory; const auto input = directory.filePath("input.tlog"), output = directory.filePath("out.mat");
        const auto message = [](float angularVelocity, int payloadLength) {
            mavlink_message_t wire{};
            const float quaternion[4] = {1, 0, 0, 0};
            mavlink_msg_autopilot_state_for_gimbal_device_pack(1, 1, &wire,
                1, 154, 123456, quaternion, 0, 1, 2, 3, 0, 4.25f, 1, 1);
            quint32 bits; std::memcpy(&bits, &angularVelocity, sizeof(bits));
            qToLittleEndian<quint32>(bits, reinterpret_cast<uchar *>(_MAV_PAYLOAD_NON_CONST(&wire)) + 53);
            mavlink_status_t status{};
            mavlink_finalize_message_buffer(&wire, 1, 1, &status, 53,
                quint8(payloadLength), MAVLINK_MSG_ID_AUTOPILOT_STATE_FOR_GIMBAL_DEVICE_CRC);
            return wire;
        };
        const auto full = message(-12.5f, 57); QCOMPARE(int(full.len), 57);
        const auto zeroTrimmed = message(0, 57); QCOMPARE(int(zeroTrimmed.len), 53);
        const auto oldPayload = message(99, 53); QCOMPARE(int(oldPayload.len), 53);
        const qint64 time = 1700000000000000LL;
        QVERIFY(save(input, record(time, full) + record(time, zeroTrimmed) + record(time, oldPayload)));
        const auto result = TlogMatlabExporter::Export(input, output);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.recordsRead, qint64(3));
        QCOMPARE(result.skippedBytes, qint64(0)); QCOMPARE(result.itemCount, 12);
        const auto parsed = matrices(read(output)); QCOMPARE(parsed.size(), 12);
        const auto angular = parsed.value("angular_velocity_z_mavlink_autopilot_state_for_gimbal_device_t");
        QCOMPARE(angular.rows, 3); QCOMPARE(angular.values.mid(3), QVector<double>({-12.5, 0, 0}));
        const QString longName = QStringLiteral("feed_forward_angular_velocity_z_mavlink_autopilot_state_for_gimbal_device_t");
        QVERIFY(longName.size() > 63); QVERIFY(parsed.contains(longName));
        QCOMPARE(parsed.value(longName).values.mid(3), QVector<double>({4.25, 4.25, 4.25}));
        QVERIFY(!parsed.contains("q_mavlink_autopilot_state_for_gimbal_device_t"));
    }
    void allOfflineExtensionSchemas_data() {
        QTest::addColumn<int>("fixtureIndex");
        const auto fixtures = extensionFixtures();
        for (int i = 0; i < fixtures.size(); ++i) QTest::newRow(fixtures.at(i).name) << i;
    }
    void allOfflineExtensionSchemas() {
        QFETCH(int, fixtureIndex);
        const auto fixtures = extensionFixtures();
        QCOMPARE(fixtures.size(), 10);
        int fieldCount = 0; for (const auto &fixture : fixtures) fieldCount += fixture.fields.size();
        QCOMPARE(fieldCount, 24);
        const auto fixture = fixtures.at(fixtureIndex);
        auto message = [&](bool zeroExtensions, bool oldLength) {
            mavlink_message_t wire{}; wire.msgid = fixture.id;
            auto *bytes = _MAV_PAYLOAD_NON_CONST(&wire);
            bytes[fixture.originalLength - 1] = 1; // Preserve a deterministic old-length payload after trimming.
            if (!zeroExtensions) for (const auto &field : fixture.fields)
                std::memcpy(bytes + field.offset, field.wire.constData(), size_t(field.wire.size()));
            mavlink_status_t status{};
            mavlink_finalize_message_buffer(&wire, 42, 100, &status, fixture.originalLength,
                oldLength ? fixture.originalLength : fixture.extendedLength, fixture.crc);
            return wire;
        };
        const auto full = message(false, false), zeroTrimmed = message(true, false), old = message(false, true);
        QCOMPARE(int(full.len), int(fixture.extendedLength));
        QCOMPARE(int(zeroTrimmed.len), int(fixture.originalLength));
        QCOMPARE(int(old.len), int(fixture.originalLength));
        // old retains nonzero extension bytes in local memory, but none are on
        // wire. Missing bytes must be zero, never copied from struct padding.
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto input = directory.filePath("input.tlog"), output = directory.filePath("out.mat");
        const qint64 time = 1700000000000000LL;
        const auto data = record(time, full) + record(time + 1000000, zeroTrimmed) + record(time + 2000000, old);
        QVERIFY(save(input, data));
        const auto result = TlogMatlabExporter::Export(input, output);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.recordsRead, qint64(3)); QCOMPARE(result.skippedBytes, qint64(0));
        const auto parsed = matrices(read(output)); QCOMPARE(parsed.size(), result.itemCount);
        for (const auto &field : fixture.fields) {
            const QString name = QLatin1String(field.name) + QStringLiteral("_mavlink_")
                + QString::fromLatin1(fixture.name).toLower() + QStringLiteral("_t");
            QVERIFY2(parsed.contains(name), qPrintable(name));
            const auto matrix = parsed.value(name);
            QCOMPARE(matrix.rows, 3); QCOMPARE(matrix.columns, 2);
            QCOMPARE(matrix.values.mid(3), QVector<double>({field.expected, 0, 0}));
            QCOMPARE(matrix.values.mid(0, 3), QVector<double>({TlogMatlabExporter::mp10SerialDate(time),
                TlogMatlabExporter::mp10SerialDate(time + 1000000), TlogMatlabExporter::mp10SerialDate(time + 2000000)}));
        }
        QCOMPARE(read(input), data);
        QCOMPARE(directoryEntries(directory.path()), QStringList({"input.tlog", "out.mat"}));
    }
    void cancellationEveryPhase_data() {
        QTest::addColumn<int>("phase");
        for (int phase : {0, 50, 150, 400, 600, 850, 1000}) QTest::newRow(qPrintable(QString::number(phase))) << phase;
    }
    void cancellationEveryPhase() {
        QFETCH(int, phase);
        QTemporaryDir directory; const auto input = directory.filePath("input.tlog"), output = directory.filePath("out.mat");
        QByteArray data; for (int i = 0; i < 100; ++i) data += record(1700000000000000LL, attitude(float(i)));
        QVERIFY(save(input, data)); bool stop = phase == 0; qint64 previous = 0;
        const auto result = TlogMatlabExporter::Export(input, output, [&] { return stop; }, [&](qint64 done, qint64 total) {
            QVERIFY(done >= previous); QVERIFY(done <= total); previous = done; if (done >= phase) stop = true;
        });
        QVERIFY(result.cancelled); QVERIFY(!result.success); QVERIFY(result.outputPaths.isEmpty());
        QCOMPARE(directoryEntries(directory.path()), QStringList{"input.tlog"}); QCOMPARE(read(input), data);
    }
    void mutationsAndExistingOutputs() {
        QTemporaryDir directory; const auto input = directory.filePath("input.tlog"), output = directory.filePath("out.mat");
        const auto data = record(1700000000000000LL, attitude(1)); QVERIFY(save(input, data));
        QVERIFY(save(output, "old output"));
        QVERIFY(!TlogMatlabExporter::Export(input, output).success); QCOMPARE(read(output), QByteArray("old output"));
        QVERIFY(!TlogMatlabExporter::Export(input, input).success); QCOMPARE(read(input), data);
        const auto changedOutput = directory.filePath("changed.mat"); bool changed = false;
        const auto before = QFileInfo(input).lastModified();
        auto result = TlogMatlabExporter::Export(input, changedOutput, {}, [&](qint64 done, qint64) {
            if (done >= 350 && !changed) {
                changed = true; QVERIFY(save(input, record(1700000000000000LL, attitude(99))));
                QFile file(input); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.setFileTime(before, QFileDevice::FileModificationTime));
            }
        });
        QVERIFY(changed); QVERIFY(!result.success); QVERIFY(!QFileInfo::exists(changedOutput)); QVERIFY(!result.error.isEmpty());
        QVERIFY(save(input, data)); changed = false;
        result = TlogMatlabExporter::Export(input, changedOutput, {}, [&](qint64 done, qint64) {
            if (done == 1000 && !changed) { changed = true; QVERIFY(save(changedOutput, "new arrival")); }
        });
        QVERIFY(!result.success); QCOMPARE(read(changedOutput), QByteArray("new arrival"));
        QCOMPARE(directoryEntries(directory.path()), QStringList({"changed.mat", "input.tlog", "out.mat"}));
    }
    void sourceAndDestinationLinksRefused() {
#ifndef Q_OS_UNIX
        QSKIP("The local symlink fixture uses POSIX QFile::link semantics.");
#else
        QTemporaryDir directory; const auto input = directory.filePath("input.tlog");
        QVERIFY(save(input, record(0, attitude(1))));
        const auto linked = directory.filePath("linked.tlog"), out = directory.filePath("out.mat");
        QVERIFY(QFile::link(input, linked)); QVERIFY(!TlogMatlabExporter::Export(linked, out).success);
        QVERIFY(QFile::link(directory.filePath("missing"), out));
        QVERIFY(!TlogMatlabExporter::Export(input, out).success); QVERIFY(QFileInfo(out).isSymLink());
#endif
    }
};

QTEST_GUILESS_MAIN(TlogMatlabExporterTest)
#include "test_tlogmatlabexporter.moc"
