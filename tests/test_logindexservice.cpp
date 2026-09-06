#include "ui/Loghandling/LogIndexService.h"

#include <QtTest>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <atomic>
#include <cstring>

#include <mavlink.h>

namespace {
constexpr qint64 BaseTimestampUsec = 1700000000000000LL;

bool put(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size() && file.flush();
}

bool append(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::Append)
        && file.write(bytes) == bytes.size() && file.flush();
}

bool setModified(const QString &path, const QDateTime &when)
{
    QFile file(path);
    return file.open(QIODevice::ReadWrite)
        && file.setFileTime(when, QFileDevice::FileModificationTime);
}

LogIndex::Entry source(const QString &path, const QString &root = {})
{
    LogIndex::Entry entry;
    entry.fullPath = path;
    entry.rootPath = root;
    return entry;
}

QByteArray textFixture(bool absoluteTime = true)
{
    QByteArray bytes;
    bytes += "FMT,129,75,MSG,QZ,TimeUS,Message\n";
    bytes += "FMT,130,31,PARM,QNf,TimeUS,Name,Value\n";
    bytes += "FMT,131,11,CAM,Q,TimeUS\n";
    if (absoluteTime) {
        bytes += "FMT,132,34,GPS,QBHIffff,TimeUS,Status,GWk,GMS,Lat,Lng,Alt,Spd\n";
    } else {
        bytes += "FMT,132,28,GPS,QBffff,TimeUS,Status,Lat,Lng,Alt,Spd\n";
    }
    bytes += "MSG,0,ArduCopter V4.5\n";
    bytes += "PARM,0,SYSID_THISMAV,42\n";
    bytes += "CAM,500000\n";
    if (absoluteTime) {
        bytes += "GPS,0,3,2200,1000,35.000000,33.000000,100,1\n";
        bytes += "GPS,500000,3,2200,1500,45.000000,43.000000,120,1\n";
        bytes += "GPS,1000000,3,2200,2000,35.000100,33.000000,103,1\n";
        bytes += "GPS,2000000,3,2200,3000,35.000200,33.000000,104,1\n";
    } else {
        bytes += "GPS,0,3,35.000000,33.000000,100,1\n";
        bytes += "GPS,1000000,3,35.000100,33.000000,103,1\n";
        bytes += "GPS,2000000,3,35.000200,33.000000,104,1\n";
    }
    return bytes;
}

QByteArray rawRecord(quint8 type, int length)
{
    QByteArray result(length, '\0');
    result[0] = char(0xa3);
    result[1] = char(0x95);
    result[2] = char(type);
    return result;
}

QByteArray rawFmt(quint8 type, int length, const QByteArray &name,
                  const QByteArray &format, const QByteArray &columns)
{
    QByteArray result = rawRecord(128, 89);
    result[3] = char(type);
    result[4] = char(length);
    std::memcpy(result.data() + 5, name.constData(), size_t(name.size()));
    std::memcpy(result.data() + 9, format.constData(), size_t(format.size()));
    std::memcpy(result.data() + 25, columns.constData(), size_t(columns.size()));
    return result;
}

void little64(QByteArray *bytes, int offset, quint64 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes->data() + offset));
}

void little32(QByteArray *bytes, int offset, qint32 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes->data() + offset));
}

void single(QByteArray *bytes, int offset, float value)
{
    quint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    qToLittleEndian(bits, reinterpret_cast<uchar *>(bytes->data() + offset));
}

QByteArray rawGps(quint64 timeUsec, quint8 status, qint32 latitude,
                  qint32 longitude, float altitude, float speed)
{
    QByteArray result = rawRecord(1, 28);
    little64(&result, 3, timeUsec);
    result[11] = char(status);
    little32(&result, 12, latitude);
    little32(&result, 16, longitude);
    single(&result, 20, altitude);
    single(&result, 24, speed);
    return result;
}

QByteArray binaryFixture()
{
    return rawFmt(1, 28, "GPS", "QBLLff",
                  "TimeUS,Status,Lat,Lng,Alt,Spd")
        + rawGps(0, 3, 350000000, 330000000, 100.0f, 1.0f)
        + rawGps(1000000, 3, 350001000, 330000000, 103.0f, 1.0f)
        + rawGps(2000000, 3, 350002000, 330000000, 104.0f, 1.0f);
}

QByteArray frameBytes(mavlink_message_t message)
{
    quint8 bytes[MAVLINK_MAX_PACKET_LEN]{};
    const int size = mavlink_msg_to_send_buffer(bytes, &message);
    return QByteArray(reinterpret_cast<const char *>(bytes), size);
}

QByteArray timestamp(qint64 usec)
{
    QByteArray result(8, '\0');
    qToBigEndian(static_cast<quint64>(usec),
                 reinterpret_cast<uchar *>(result.data()));
    return result;
}

QByteArray record(qint64 usec, mavlink_message_t message)
{
    return timestamp(usec) + frameBytes(message);
}

mavlink_message_t heartbeat(quint8 system, quint8 type, bool isArmed)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        system, MAV_COMP_ID_AUTOPILOT1, &message, type,
        MAV_AUTOPILOT_ARDUPILOTMEGA,
        isArmed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        0, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t gpsRaw(quint8 system, quint8 fix)
{
    mavlink_message_t message{};
    mavlink_gps_raw_int_t gps{};
    gps.fix_type = fix;
    mavlink_msg_gps_raw_int_encode(system, MAV_COMP_ID_GPS, &message, &gps);
    return message;
}

mavlink_message_t globalPosition(quint8 system, qint32 latitude,
                                 qint32 longitude, qint32 altitudeMm)
{
    mavlink_message_t message{};
    mavlink_msg_global_position_int_pack(
        system, MAV_COMP_ID_AUTOPILOT1, &message, 0, latitude,
        longitude, altitudeMm, altitudeMm, 100, 0, 0, 0);
    return message;
}

mavlink_message_t emptyMessage(quint8 system, quint8 component,
                               quint32 id, quint8 minimumLength,
                               quint8 length, quint8 crcExtra)
{
    mavlink_message_t message{};
    message.msgid = id;
    std::memset(_MAV_PAYLOAD_NON_CONST(&message), 0, length);
    mavlink_finalize_message(&message, system, component,
                             minimumLength, length, crcExtra);
    return message;
}

mavlink_message_t camera(quint8 system)
{
    return emptyMessage(system, MAV_COMP_ID_CAMERA,
                        MAVLINK_MSG_ID_CAMERA_FEEDBACK,
                        MAVLINK_MSG_ID_CAMERA_FEEDBACK_MIN_LEN,
                        MAVLINK_MSG_ID_CAMERA_FEEDBACK_LEN,
                        MAVLINK_MSG_ID_CAMERA_FEEDBACK_CRC);
}

mavlink_message_t simulation(quint8 system)
{
    return emptyMessage(system, MAV_COMP_ID_AUTOPILOT1,
                        MAVLINK_MSG_ID_SIM_STATE,
                        MAVLINK_MSG_ID_SIM_STATE_MIN_LEN,
                        MAVLINK_MSG_ID_SIM_STATE_LEN,
                        MAVLINK_MSG_ID_SIM_STATE_CRC);
}

QByteArray telemetryFixture()
{
    QByteArray bytes;
    bytes += record(BaseTimestampUsec,
                    heartbeat(42, MAV_TYPE_FIXED_WING, true));
    bytes += record(BaseTimestampUsec + 1000000,
                    heartbeat(255, MAV_TYPE_GCS, false));
    bytes += record(BaseTimestampUsec + 2000000, gpsRaw(42, 2));
    bytes += record(BaseTimestampUsec + 3000000,
                    globalPosition(42, 350000000, 330000000, 100000));
    bytes += record(BaseTimestampUsec + 4000000,
                    globalPosition(77, 360000000, 340000000, 100000));
    bytes += record(BaseTimestampUsec + 5000000, gpsRaw(42, 3));
    bytes += record(BaseTimestampUsec + 6000000,
                    globalPosition(42, 350000000, 330000000, 100000));
    bytes += record(BaseTimestampUsec + 7000000,
                    globalPosition(42, 350001000, 330000000, 103000));
    bytes += record(BaseTimestampUsec + 8000000, camera(77));
    bytes += record(BaseTimestampUsec + 9000000, camera(255));
    bytes += record(BaseTimestampUsec + 10000000, simulation(42));
    return bytes;
}

void verifyThreePointMetrics(const LogIndex::Analysis &analysis)
{
    QVERIFY(analysis.entry.home.valid);
    QCOMPARE(analysis.entry.home.latitude, 35.0);
    QCOMPARE(analysis.entry.home.longitude, 33.0);
    QCOMPARE(analysis.entry.home.altitudeMeters, 100.0);
    QCOMPARE(analysis.entry.durationSeconds, 2.0);
    QCOMPARE(analysis.entry.timeInAirSeconds, 2.0);
    QVERIFY(analysis.entry.distanceMeters > 21.0);
    QVERIFY(analysis.entry.distanceMeters < 23.5);
    QCOMPARE(analysis.track.size(), 3);
}
}

class LogIndexServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void textDataFlashMetricsMatchReference();
    void dataFlashClockUsesFirstGpsAnchorAndBootDeltas();
    void binaryDataFlashMetricsMatchReference();
    void telemetryMetricsFilterSystemsAndFixes();
    void trackSamplingIsUniformAndBounded();
    void malformedTailRetainsRecoveredMetrics();
    void cancellationStopsLargeDataFlashPass();
    void scanSortsAndIsolatesPerFileErrors();
    void scanBoundsWorkersAndSerializesCancellation();
    void scanCancellationReturnsCompletedRows();
    void scanRechecksAfterProgressCallback();
};

void LogIndexServiceTest::textDataFlashMetricsMatchReference()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("flight.log"));
    QVERIFY(put(path, textFixture()));

    const LogIndex::Analysis analysis = LogIndexService::analyzeFile(
        source(path, directory.path()));
    QVERIFY(!analysis.cancelled);
    QVERIFY2(analysis.entry.error.isEmpty(), qPrintable(analysis.entry.error));
    QCOMPARE(analysis.entry.frame, QStringLiteral("ArduCopter"));
    QCOMPARE(analysis.entry.systemId, 42);
    QCOMPARE(analysis.entry.cameraMessages, quint64(1));
    QCOMPARE(analysis.entry.dateUtc,
             QDateTime(QDate(2022, 3, 5), QTime(23, 59, 43), Qt::UTC));
    verifyThreePointMetrics(analysis);
}

void LogIndexServiceTest::dataFlashClockUsesFirstGpsAnchorAndBootDeltas()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("gps-jitter.log"));
    QByteArray bytes =
        "FMT,133,34,GPS2,QBHIffff,TimeUS,Status,GWk,GMS,Lat,Lng,Alt,Spd\n"
        "GPS2,10199252,3,2434,143088000,34.000000,32.000000,580,0\n"
        "FMT,132,34,GPS,QBHIffff,TimeUS,Status,GWk,GMS,Lat,Lng,Alt,Spd\n"
        "GPS,10199252,3,2434,90000000,35.000000,33.000000,584.09,1\n"
        "GPS,11199593,3,2434,500000000,35.000100,33.000000,585.00,1\n"
        "GPS,12199593,3,2434,1000,35.000200,33.000000,586.00,1\n";
    QVERIFY(put(path, bytes));

    const LogIndex::Analysis analysis = LogIndexService::analyzeFile(
        source(path, directory.path()));
    QVERIFY2(analysis.entry.error.isEmpty(), qPrintable(analysis.entry.error));
    // GPS2 supplies the first clock anchor. GPS GMS then deliberately jumps
    // forward and backward; MP10 still uses TimeUS for flight intervals.
    QVERIFY(qAbs(analysis.entry.durationSeconds - 2.000341) < 1e-9);
    QVERIFY(qAbs(analysis.entry.timeInAirSeconds - 2.000341) < 1e-9);
    QCOMPARE(analysis.entry.dateUtc,
             QDateTime(QDate(2026, 8, 31), QTime(15, 44, 30), Qt::UTC));
    QCOMPARE(analysis.track.size(), 3);
}

void LogIndexServiceTest::binaryDataFlashMetricsMatchReference()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("flight.bin"));
    QVERIFY(put(path, binaryFixture()));

    const LogIndex::Analysis analysis = LogIndexService::analyzeFile(
        source(path, directory.path()));
    QVERIFY(!analysis.cancelled);
    QVERIFY2(analysis.entry.error.isEmpty(), qPrintable(analysis.entry.error));
    QCOMPARE(analysis.entry.frame, QStringLiteral("DFLog Unknown"));
    QVERIFY(!analysis.entry.dateUtc.isValid());
    verifyThreePointMetrics(analysis);
}

void LogIndexServiceTest::telemetryMetricsFilterSystemsAndFixes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("flight.tlog"));
    QVERIFY(put(path, telemetryFixture()));

    const LogIndex::Analysis analysis = LogIndexService::analyzeFile(
        source(path, directory.path()));
    QVERIFY(!analysis.cancelled);
    QVERIFY2(analysis.entry.error.isEmpty(), qPrintable(analysis.entry.error));
    QCOMPARE(analysis.entry.frame, QStringLiteral("FIXED_WING (SITL)"));
    QCOMPARE(analysis.entry.systemId, 42);
    QCOMPARE(analysis.entry.cameraMessages, quint64(1));
    QCOMPARE(analysis.entry.dateUtc,
             QDateTime::fromMSecsSinceEpoch(BaseTimestampUsec / 1000, Qt::UTC));
    QCOMPARE(analysis.entry.durationSeconds, 10.0);
    QCOMPARE(analysis.entry.timeInAirSeconds, 1.0);
    QCOMPARE(analysis.track.size(), 2);
    QVERIFY(analysis.entry.distanceMeters > 10.0);
    QVERIFY(analysis.entry.distanceMeters < 12.5);
}

void LogIndexServiceTest::trackSamplingIsUniformAndBounded()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("large.log"));
    QByteArray bytes = "FMT,132,28,GPS,QBffff,TimeUS,Status,Lat,Lng,Alt,Spd\n";
    constexpr int Points = 5001;
    for (int index = 0; index < Points; ++index) {
        bytes += QByteArray("GPS,") + QByteArray::number(qint64(index) * 1000000)
            + ",3," + QByteArray::number(35.0 + index * 0.000001, 'f', 6)
            + ",33.000000,100,1\n";
    }
    QVERIFY(put(path, bytes));

    const LogIndex::Analysis analysis = LogIndexService::analyzeFile(
        source(path, directory.path()));
    QVERIFY2(analysis.entry.error.isEmpty(), qPrintable(analysis.entry.error));
    QCOMPARE(analysis.track.size(), LogIndex::MaximumTrackPoints);
    QCOMPARE(analysis.track.first().latitude, 35.0);
    QCOMPARE(analysis.track.last().latitude, 35.005);
    QVERIFY(analysis.track.at(2000).latitude > 35.00249);
    QVERIFY(analysis.track.at(2000).latitude < 35.00252);
}

void LogIndexServiceTest::malformedTailRetainsRecoveredMetrics()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("partial.tlog"));
    const QByteArray first = record(BaseTimestampUsec,
                                    heartbeat(19, MAV_TYPE_QUADROTOR, false));
    QVERIFY(put(path, first + QByteArray::fromHex("0102030405")));

    const LogIndex::Analysis analysis = LogIndexService::analyzeFile(
        source(path, directory.path()));
    QVERIFY(!analysis.cancelled);
    QCOMPARE(analysis.entry.systemId, 19);
    QCOMPARE(analysis.entry.frame, QStringLiteral("QUADROTOR"));
    QCOMPARE(analysis.entry.dateUtc,
             QDateTime::fromMSecsSinceEpoch(BaseTimestampUsec / 1000, Qt::UTC));
    QVERIFY(!analysis.entry.error.isEmpty());
}

void LogIndexServiceTest::cancellationStopsLargeDataFlashPass()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("cancel.log"));
    QByteArray bytes = "FMT,132,28,GPS,QBffff,TimeUS,Status,Lat,Lng,Alt,Spd\n";
    bytes += QByteArray("GPS,0,3,35,33,100,1\n").repeated(1000);
    QVERIFY(put(path, bytes));
    int calls = 0;
    const LogIndex::Analysis analysis = LogIndexService::analyzeFile(
        source(path, directory.path()), [&] { return ++calls > 20; });
    QVERIFY(analysis.cancelled);
    QVERIFY(calls <= 22);
}

void LogIndexServiceTest::scanSortsAndIsolatesPerFileErrors()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString older = directory.filePath(QStringLiteral("older.log"));
    const QString newer = directory.filePath(QStringLiteral("newer.bin"));
    const QString broken = directory.filePath(QStringLiteral("broken.tlog"));
    QVERIFY(put(older, QByteArray("FMT,129,75,MSG,QZ,TimeUS,Message\n")));
    QVERIFY(put(newer, rawFmt(1, 28, "GPS", "QBLLff",
                             "TimeUS,Status,Lat,Lng,Alt,Spd")));
    QVERIFY(put(broken, QByteArray::fromHex("0001020304050607fd")));
    const QDateTime base(QDate(2024, 1, 1), QTime(0, 0), Qt::UTC);
    QVERIFY(setModified(older, base));
    QVERIFY(setModified(newer, base.addSecs(10)));
    QVERIFY(setModified(broken, base.addSecs(-10)));
    int progressCalls = 0;

    const LogIndex::ScanResult scan = LogIndexService::scan(
        directory.path(), {},
        [&](int completed, int total, const QString &) {
            ++progressCalls;
            QCOMPARE(completed, progressCalls);
            QCOMPARE(total, 3);
        });
    QVERIFY2(scan.success, qPrintable(scan.error));
    QVERIFY(!scan.cancelled);
    QCOMPARE(scan.entries.size(), 3);
    QCOMPARE(progressCalls, 3);
    QCOMPARE(QFileInfo(scan.entries.at(0).fullPath).fileName(),
             QStringLiteral("newer.bin"));
    QCOMPARE(QFileInfo(scan.entries.at(1).fullPath).fileName(),
             QStringLiteral("older.log"));
    QCOMPARE(QFileInfo(scan.entries.at(2).fullPath).fileName(),
             QStringLiteral("broken.tlog"));
    QVERIFY(!scan.entries.at(2).error.isEmpty());
    for (const LogIndex::Entry &entry : scan.entries) {
        QVERIFY(!entry.thumbnailJpeg.isEmpty());
        QVERIFY(entry.thumbnail.exists);
    }
}

void LogIndexServiceTest::scanBoundsWorkersAndSerializesCancellation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (int index = 0; index < 8; ++index) {
        QVERIFY(put(directory.filePath(QStringLiteral("%1.log").arg(index)),
                    textFixture(false)));
    }
    std::atomic_int activeCancel{0}, maximumCancel{0};
    std::atomic_int activeTiles{0}, maximumTiles{0};
    const auto enter = [](std::atomic_int *active, std::atomic_int *maximum) {
        const int now = active->fetch_add(1) + 1;
        int seen = maximum->load();
        while (seen < now && !maximum->compare_exchange_weak(seen, now)) {}
    };
    const LogIndex::ScanResult scan = LogIndexService::scan(
        directory.path(),
        [&] {
            enter(&activeCancel, &maximumCancel);
            QThread::usleep(50);
            activeCancel.fetch_sub(1);
            return false;
        }, {},
        [&](int, int, int) {
            enter(&activeTiles, &maximumTiles);
            QThread::msleep(2);
            activeTiles.fetch_sub(1);
            return QByteArray();
        });
    QVERIFY2(scan.success, qPrintable(scan.error));
    QCOMPARE(scan.entries.size(), 8);
    QCOMPARE(maximumCancel.load(), 1);
    QVERIFY(maximumTiles.load() >= 1);
    QVERIFY(maximumTiles.load() <= 4);
}

void LogIndexServiceTest::scanCancellationReturnsCompletedRows()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (int index = 0; index < 8; ++index) {
        QVERIFY(put(directory.filePath(QStringLiteral("%1.log").arg(index)),
                    QByteArray("FMT,129,75,MSG,QZ,TimeUS,Message\n")));
    }
    bool cancel = false;
    int progressCalls = 0;
    const LogIndex::ScanResult scan = LogIndexService::scan(
        directory.path(), [&] { return cancel; },
        [&](int completed, int total, const QString &) {
            ++progressCalls;
            QCOMPARE(completed, progressCalls);
            QCOMPARE(total, 8);
            cancel = true;
        });
    QVERIFY(!scan.success);
    QVERIFY(scan.cancelled);
    QVERIFY(!scan.error.isEmpty());
    QCOMPARE(scan.entries.size(), progressCalls);
    QVERIFY(progressCalls >= 1);
    QVERIFY(progressCalls <= 4);
}

void LogIndexServiceTest::scanRechecksAfterProgressCallback()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("changing.log"));
    QVERIFY(put(path, textFixture(false)));
    bool changed = false;

    const LogIndex::ScanResult scan = LogIndexService::scan(
        directory.path(), {},
        [&](int, int, const QString &reported) {
            QCOMPARE(reported, path);
            QVERIFY(append(path, QByteArray("\n")));
            changed = true;
        });
    QVERIFY(scan.success);
    QVERIFY(changed);
    QCOMPARE(scan.entries.size(), 1);
    QVERIFY2(scan.entries.first().error.contains(
                 QStringLiteral("changed"), Qt::CaseInsensitive),
             qPrintable(scan.entries.first().error));
}

QTEST_MAIN(LogIndexServiceTest)
#include "test_logindexservice.moc"
