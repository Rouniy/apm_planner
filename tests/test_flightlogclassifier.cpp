#include "ui/Loghandling/FlightLogClassifier.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cstring>
#include <limits>
#include <mavlink.h>

namespace {
using Classifier = FlightLogClassifier;
bool put(const QString &path, const QByteArray &bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray get(const QString &path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
void le32(QByteArray &bytes, int at, quint32 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + at));
}
void single(QByteArray &bytes, int at, float value)
{
    quint32 bits = 0; std::memcpy(&bits, &value, sizeof(bits)); le32(bytes, at, bits);
}
QByteArray record(quint8 id, int length)
{
    QByteArray bytes(length, '\0'); bytes[0] = char(0xa3); bytes[1] = char(0x95); bytes[2] = char(id); return bytes;
}
QByteArray fmt(quint8 id, int length, const QByteArray &name, const QByteArray &format, const QByteArray &columns)
{
    QByteArray bytes = record(128, 89); bytes[3] = char(id); bytes[4] = char(length);
    std::memcpy(bytes.data() + 5, name.constData(), size_t(name.size()));
    std::memcpy(bytes.data() + 9, format.constData(), size_t(format.size()));
    std::memcpy(bytes.data() + 25, columns.constData(), size_t(columns.size())); return bytes;
}
QByteArray parm(const QByteArray &name, float value)
{
    QByteArray bytes = record(1, 31);
    std::memcpy(bytes.data() + 11, name.constData(), size_t(name.size())); single(bytes, 27, value); return bytes;
}
QByteArray msg(const QByteArray &text)
{
    QByteArray bytes = record(2, 75);
    std::memcpy(bytes.data() + 11, text.constData(), size_t(text.size())); return bytes;
}
QByteArray binaryPrefix()
{
    return fmt(1, 31, "PARM", "QNf", "TimeUS,Name,Value")
        + fmt(2, 75, "MSG", "QZ", "TimeUS,Message")
        + fmt(3, 11, "SIM", "Q", "TimeUS")
        + fmt(4, 7, "PAD", "I", "N") + record(4, 7).repeated(160);
}
QByteArray textPrefix()
{
    return QByteArray("FMT,1,31,PARM,QNf,TimeUS,Name,Value\nFMT,2,75,MSG,QZ,TimeUS,Message\n"
                      "FMT,3,11,SIM,Q,TimeUS\nFMT,4,7,PAD,I,N\n") + QByteArray("PAD,0\n").repeated(180);
}
quint16 x25(quint16 crc, quint8 byte)
{
    quint8 temporary = byte ^ quint8(crc);
    temporary ^= quint8(temporary << 4);
    return quint16((crc >> 8) ^ (quint16(temporary) << 8) ^ (quint16(temporary) << 3) ^ (temporary >> 4));
}
QByteArray frame(int system, int component, int id, const QByteArray &payload, int extra, bool v2 = false)
{
    // Independent wire framing + X25, not mavlink pack/finalize helpers.
    QByteArray bytes;
    if (v2) {
        bytes = QByteArray::fromHex("fd000000000000000000");
        bytes[1] = char(payload.size()); bytes[5] = char(system); bytes[6] = char(component);
        bytes[7] = char(id); bytes[8] = char(id >> 8); bytes[9] = char(id >> 16);
    } else {
        bytes = QByteArray::fromHex("fe0000000000");
        bytes[1] = char(payload.size()); bytes[3] = char(system); bytes[4] = char(component); bytes[5] = char(id);
    }
    bytes += payload;
    quint16 crc = 0xffff;
    for (int at = 1; at < bytes.size(); ++at) crc = x25(crc, quint8(bytes[at]));
    crc = x25(crc, quint8(extra)); bytes += char(crc); bytes += char(crc >> 8); return bytes;
}
QByteArray heartbeat(int system = 42, int component = 1, int type = MAV_TYPE_QUADROTOR, bool v2 = false)
{
    QByteArray payload(9, '\0'); payload[4] = char(type); payload[5] = MAV_AUTOPILOT_ARDUPILOTMEGA;
    payload[7] = MAV_STATE_ACTIVE; payload[8] = 3;
    return frame(system, component, 0, payload, 50, v2);
}
QByteArray serial(int system, int component, float value, bool v2 = false)
{
    QByteArray payload(25, '\0'); single(payload, 0, value);
    std::memcpy(payload.data() + 8, "BRD_SERIAL_NUM", 14); payload[24] = MAV_PARAM_TYPE_REAL32;
    return frame(system, component, 22, payload, 220, v2);
}
QByteArray telemetry(const QList<QByteArray> &frames, bool tlog)
{
    QByteArray bytes;
    for (const auto &frame : frames) {
        if (tlog) {
            QByteArray timestamp(8, '\0'); qToBigEndian<quint64>(1000000, reinterpret_cast<uchar *>(timestamp.data()));
            bytes += timestamp;
        }
        bytes += frame;
    }
    // Valid comp190 traffic pads past SMALL without contributing evidence.
    for (int count = 0; bytes.size() <= 1024; ++count) {
        if (tlog) bytes += QByteArray(8, '\0');
        bytes += heartbeat(255, 190, MAV_TYPE_GCS);
    }
    return bytes;
}
}

class FlightLogClassifierTest final : public QObject
{
    Q_OBJECT
private slots:
    void smallAndEmpty_data();
    void smallAndEmpty();
    void dataFlashContentNotBooleanCount_data();
    void dataFlashContentNotBooleanCount();
    void binaryAndTextGoldenAgreeAndRemainUnmodified();
    void dataFlashFirstOccurrenceAndMsgLimit();
    void telemetrySelectionAndSerialIsolation_data();
    void telemetrySelectionAndSerialIsolation();
    void heartbeatLimitAndAllTypeNames();
    void invalidCrcAndMissingHeartbeats();
    void simulationExclusionAndNonfiniteSerial();
    void malformedDataFlashAndCancellation();
};

void FlightLogClassifierTest::smallAndEmpty_data()
{
    QTest::addColumn<QString>("suffix"); QTest::addColumn<int>("size");
    for (const auto &suffix : {"tlog", "rlog", "bin", "log"})
        for (int size : {0, 1, 1024})
            QTest::newRow(qPrintable(QString("%1-%2").arg(suffix).arg(size))) << QString(suffix) << size;
}
void FlightLogClassifierTest::smallAndEmpty()
{
    QFETCH(QString, suffix); QFETCH(int, size);
    QTemporaryDir dir; const QString path = dir.filePath("in." + suffix); const QByteArray bytes(size, 'X');
    QVERIFY(put(path, bytes)); const auto result = Classifier::Classify(path);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.disposition, size ? Classifier::Disposition::Move : Classifier::Disposition::DeleteEmpty);
    QCOMPARE(result.relativeDirectory, size ? QString("SMALL") : QString()); QCOMPARE(get(path), bytes);
}

void FlightLogClassifierTest::dataFlashContentNotBooleanCount_data()
{
    QTest::addColumn<QByteArray>("messages"); QTest::addColumn<QString>("expected");
    QTest::newRow("unrelated MSG stays unknown") << QByteArray("MSG,0,Storage ready\n") << QString();
    QTest::newRow("copter is not automatically rover") << QByteArray("MSG,0,ArduCopter V4.8\n") << QString("QUADROTOR/7");
    QTest::newRow("hexa frame text") << QByteArray("MSG,0,ArduCopter\nMSG,0,Frame: HEXA/X\n") << QString("HEXAROTOR/7");
    QTest::newRow("octo frame text") << QByteArray("MSG,0,ArduCopter\nMSG,0,Frame: OCTO/X\n") << QString("OCTOROTOR/7");
    QTest::newRow("ArduPilot octa frame text") << QByteArray("MSG,0,ArduCopter\nMSG,0,Frame: OCTA/X\n") << QString("OCTOROTOR/7");
    QTest::newRow("first Frame only") << QByteArray("MSG,0,ArduCopter\nMSG,0,Frame: QUAD\nMSG,0,Frame: OCTO\n") << QString("QUADROTOR/7");
    QTest::newRow("plane") << QByteArray("MSG,0,ArduPlane\n") << QString("FIXED_WING/7");
    QTest::newRow("rover") << QByteArray("MSG,0,ArduRover\n") << QString("GROUND_ROVER/7");
    QTest::newRow("explicit reference precedence") << QByteArray("MSG,0,ArduCopter Plane Rover\n") << QString("GROUND_ROVER/7");
}
void FlightLogClassifierTest::dataFlashContentNotBooleanCount()
{
    QFETCH(QByteArray, messages); QFETCH(QString, expected);
    QTemporaryDir dir; const QString path = dir.filePath("in.LOG");
    const QByteArray bytes = textPrefix() + "PARM,0,SYSID_THISMAV,7\n" + messages;
    QVERIFY(put(path, bytes)); const auto result = Classifier::Classify(path);
    QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.relativeDirectory, expected);
    QCOMPARE(result.disposition, expected.isEmpty() ? Classifier::Disposition::Leave : Classifier::Disposition::Move);
    if (expected.isEmpty()) QVERIFY(!result.warnings.isEmpty());
    QCOMPARE(get(path), bytes); // Declared SIM without data is not SITL.
}

void FlightLogClassifierTest::binaryAndTextGoldenAgreeAndRemainUnmodified()
{
    QTemporaryDir dir;
    const QByteArray binary = binaryPrefix() + parm("SYSID_THISMAV", 3) + parm("BRD_SERIAL_NUM", 123)
        + msg("ArduCopter V4.8") + msg("Frame: OCTO/X") + record(3, 11) + record(4, 7).left(4);
    const QByteArray text = textPrefix() + "PARM,0,SYSID_THISMAV,3\nPARM,0,BRD_SERIAL_NUM,123\n"
        "MSG,0,ArduCopter V4.8\nMSG,0,Frame: OCTO/X\nSIM,0\n";
    for (bool isText : {false, true}) {
        const QString path = dir.filePath(isText ? "in.log" : "in.bin");
        const QByteArray source = isText ? text : binary;
        QVERIFY(put(path, source)); const auto result = Classifier::Classify(path);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.relativeDirectory, QString("SITL/OCTOROTOR/3/123"));
        QCOMPARE(get(path), source);
        if (!isText) QVERIFY(result.warnings.join('\n').contains("4 bytes"));
    }
    const QString missing = dir.filePath("missing-sysid.bin");
    QVERIFY(put(missing, binaryPrefix() + msg("ArduCopter") + msg("Frame: QUAD/PLUS") + record(3, 11)));
    const auto result = Classifier::Classify(missing);
    QVERIFY(result.success); QCOMPARE(result.relativeDirectory, QString("SITL/QUADROTOR/0"));
}

void FlightLogClassifierTest::dataFlashFirstOccurrenceAndMsgLimit()
{
    QTemporaryDir dir; const QString path = dir.filePath("in.log");
    QVERIFY(put(path, textPrefix() + "PARM,0,SYSID_THISMAV,9\nPARM,0,SYSID_THISMAV,10\n"
        "PARM,0,BRD_SERIAL_NUM,-55\nPARM,0,BRD_SERIAL_NUM,66\nMSG,0,ArduPlane\n"));
    auto result = Classifier::Classify(path); QVERIFY(result.success); QCOMPARE(result.relativeDirectory, QString("FIXED_WING/9/-55"));
    QVERIFY(put(path, textPrefix() + "PARM,0,SYSID_THISMAV,NaN\nPARM,0,SYSID_THISMAV,10\nMSG,0,ArduPlane\n"));
    result = Classifier::Classify(path); QVERIFY(result.success); QCOMPARE(result.disposition, Classifier::Disposition::Leave);
    QVERIFY(result.warnings.join('\n').contains("First SYSID"));
    QVERIFY(put(path, textPrefix() + "PARM,0,SYSID_THISMAV,9\n" + QByteArray("MSG,0,Storage ready\n").repeated(100)
        + "MSG,0,ArduCopter\nSIM,0\n"));
    result = Classifier::Classify(path); QVERIFY(result.success); QCOMPARE(result.relativeDirectory, QString("SITL/GENERIC/9"));
}

void FlightLogClassifierTest::telemetrySelectionAndSerialIsolation_data()
{
    QTest::addColumn<bool>("tlog"); QTest::addColumn<bool>("v2");
    QTest::newRow("raw v1") << false << false; QTest::newRow("raw v2") << false << true;
    QTest::newRow("timestamped v1") << true << false; QTest::newRow("timestamped v2") << true << true;
}
void FlightLogClassifierTest::telemetrySelectionAndSerialIsolation()
{
    QFETCH(bool, tlog); QFETCH(bool, v2);
    QTemporaryDir dir; const QString path = dir.filePath(tlog ? "in.tlog" : "in.rlog");
    const QByteArray source = telemetry({serial(44, 2, 333, v2), heartbeat(42, 1, MAV_TYPE_QUADROTOR, v2),
        serial(42, 1, 111, v2), heartbeat(44, 2, MAV_TYPE_FIXED_WING, v2),
        serial(42, 1, 222, v2), serial(44, 1, 999, v2),
        heartbeat(250, 190, MAV_TYPE_GCS, v2), heartbeat(240, 1, MAV_TYPE_ANTENNA_TRACKER, v2),
        heartbeat(241, 1, MAV_TYPE_GCS, v2),
        frame(44, 2, MAVLINK_MSG_ID_HIL_CONTROLS, QByteArray(MAVLINK_MSG_ID_HIL_CONTROLS_LEN, '\0'), MAVLINK_MSG_ID_HIL_CONTROLS_CRC, v2)}, tlog);
    QVERIFY(put(path, source)); const auto result = Classifier::Classify(path);
    QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.relativeDirectory, QString("SITL/FIXED_WING/44/333"));
    QVERIFY(result.warnings.join('\n').contains("Multiple telemetry")); QCOMPARE(get(path), source);
}

void FlightLogClassifierTest::heartbeatLimitAndAllTypeNames()
{
    QTemporaryDir dir; const QString path = dir.filePath("in.rlog");
    QList<QByteArray> packets;
    for (int count = 0; count < 101; ++count) packets.append(heartbeat());
    packets.append(serial(42, 1, 777)); packets.append(heartbeat(50, 1, MAV_TYPE_GROUND_ROVER));
    QVERIFY(put(path, telemetry(packets, false))); auto result = Classifier::Classify(path);
    QVERIFY(result.success); QCOMPARE(result.relativeDirectory, QString("QUADROTOR/42"));
    QVERIFY(result.warnings.join('\n').contains("101 heartbeats"));
    const QList<QByteArray> names = QByteArray("GENERIC FIXED_WING QUADROTOR COAXIAL HELICOPTER ANTENNA_TRACKER GCS AIRSHIP FREE_BALLOON ROCKET GROUND_ROVER SURFACE_BOAT SUBMARINE HEXAROTOR OCTOROTOR TRICOPTER FLAPPING_WING KITE ONBOARD_CONTROLLER VTOL_DUOROTOR VTOL_QUADROTOR VTOL_TILTROTOR VTOL_RESERVED2 VTOL_RESERVED3 VTOL_RESERVED4 VTOL_RESERVED5 GIMBAL ADSB PARAFOIL DODECAROTOR CAMERA CHARGING_STATION FLARM SERVO ODID DECAROTOR BATTERY PARACHUTE LOG OSD IMU GPS WINCH GENERIC_MULTIROTOR ILLUMINATOR SPACECRAFT_ORBITER GROUND_QUADRUPED VTOL_GYRODYNE GRIPPER").split(' ');
    QCOMPARE(names.size(), 49);
    for (int type = 0; type <= 49; ++type) {
        QVERIFY(put(path, telemetry({heartbeat(42, 1, type)}, false)));
        result = Classifier::Classify(path); QVERIFY(result.success);
        QCOMPARE(result.relativeDirectory, (type < names.size() ? QString::fromLatin1(names[type]) : QString::number(type)) + "/42");
    }
    QVERIFY(put(path, telemetry({heartbeat(42, 1, 255), serial(42, 1, 5.75F)}, false)));
    result = Classifier::Classify(path); QVERIFY(result.success); QCOMPARE(result.relativeDirectory, QString("255/42/5"));
}

void FlightLogClassifierTest::invalidCrcAndMissingHeartbeats()
{
    QTemporaryDir dir;
    for (bool tlog : {false, true}) {
        const QString path = dir.filePath(tlog ? "in.tlog" : "in.rlog");
        QByteArray broken = heartbeat(99, 1, MAV_TYPE_FIXED_WING);
        broken[broken.size() - 1] = char(quint8(broken.at(broken.size() - 1)) ^ 1);
        QVERIFY(put(path, telemetry({broken, heartbeat()}, tlog) + "tail"));
        auto result = Classifier::Classify(path); QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.relativeDirectory, QString("QUADROTOR/42")); QVERIFY(result.warnings.join('\n').contains("Rejected"));
        if (tlog) QVERIFY(result.warnings.join('\n').contains("Truncated timestamped telemetry tail"));
        QVERIFY(put(path, telemetry({heartbeat(255, 190, MAV_TYPE_GCS), serial(42, 1, 10)}, tlog)));
        result = Classifier::Classify(path); QVERIFY(result.success); QCOMPARE(result.relativeDirectory, QString("BAD"));
    }
}

void FlightLogClassifierTest::malformedDataFlashAndCancellation()
{
    QTemporaryDir dir; const QString path = dir.filePath("in.log");
    QVERIFY(put(path, QByteArray(1025, 'X')));
    auto result = Classifier::Classify(path); QVERIFY(!result.success); QVERIFY(!result.error.isEmpty());
    QVERIFY(put(path, QByteArray(4 * 1024 * 1024 + 1, 'X')));
    result = Classifier::Classify(path); QVERIFY(!result.success);
    QVERIFY(!Classifier::Classify(dir.filePath("missing.log")).success);
    QVERIFY(put(path, textPrefix() + "MSG,0,ArduCopter\nPARM,0,SYSID_THISMAV,5\n"));
    result = Classifier::Classify(path, [] { return true; }); QVERIFY(result.cancelled); QVERIFY(!result.success);
    bool cancel = false;
    result = Classifier::Classify(path, [&] { return cancel; }, [&](qint64 done, qint64 total) {
        if (done == total) cancel = true;
    });
    QVERIFY(result.cancelled); QVERIFY(!result.success); QCOMPARE(result.disposition, Classifier::Disposition::Leave);
    QVERIFY(result.relativeDirectory.isEmpty());
    const QString raw = dir.filePath("large.rlog"); const QByteArray bytes(512 * 1024, 'X');
    QVERIFY(put(raw, bytes)); cancel = false;
    result = Classifier::Classify(raw, [&] { return cancel; }, [&](qint64 done, qint64) {
        if (done >= 64 * 1024) cancel = true;
    });
    QVERIFY(result.cancelled); QVERIFY(!result.success); QCOMPARE(get(raw), bytes);
}

void FlightLogClassifierTest::simulationExclusionAndNonfiniteSerial()
{
    QTemporaryDir dir;
    for (bool tlog : {false, true}) {
        const QString path = dir.filePath(tlog ? "in.tlog" : "in.rlog");
        const auto simulation = [](int component) {
            return frame(42, component, MAVLINK_MSG_ID_SIMSTATE,
                         QByteArray(MAVLINK_MSG_ID_SIMSTATE_LEN, '\0'), MAVLINK_MSG_ID_SIMSTATE_CRC);
        };
        QVERIFY(put(path, telemetry({simulation(190), heartbeat(),
            serial(42, 1, std::numeric_limits<float>::quiet_NaN())}, tlog)));
        auto result = Classifier::Classify(path); QVERIFY(result.success);
        QCOMPARE(result.relativeDirectory, QString("QUADROTOR/42"));
        QVERIFY(result.warnings.join('\n').contains("invalid telemetry BRD_SERIAL_NUM"));
        QVERIFY(put(path, telemetry({heartbeat(), serial(42, 1, -10), simulation(1),
            serial(42, 1, std::numeric_limits<float>::infinity())}, tlog)));
        result = Classifier::Classify(path); QVERIFY(result.success);
        QCOMPARE(result.relativeDirectory, QString("SITL/QUADROTOR/42/-10"));
        QVERIFY(put(path, telemetry({heartbeat(41, 1, MAV_TYPE_GCS),
            heartbeat(42, 1, MAV_TYPE_ANTENNA_TRACKER)}, tlog)));
        result = Classifier::Classify(path); QVERIFY(result.success);
        QCOMPARE(result.relativeDirectory, QString("ANTENNA_TRACKER/42"));
    }
}

QTEST_GUILESS_MAIN(FlightLogClassifierTest)
#include "test_flightlogclassifier.moc"
