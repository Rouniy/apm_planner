#include <QtTest>

#include "comm/TlogExportService.h"
#include "comm/TlogReader.h"

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstring>

namespace {

QByteArray frameBytes(mavlink_message_t &message)
{
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
    const int size = mavlink_msg_to_send_buffer(buffer, &message);
    return QByteArray(reinterpret_cast<const char *>(buffer), size);
}

QByteArray timestampBytes(qint64 usec)
{
    QByteArray bytes(8, '\0');
    quint64 value = static_cast<quint64>(usec);
    for (int i = 7; i >= 0; --i) {
        bytes[i] = static_cast<char>(value & 0xFF);
        value >>= 8;
    }
    return bytes;
}

QByteArray record(qint64 usec, mavlink_message_t &message)
{
    return timestampBytes(usec) + frameBytes(message);
}

mavlink_message_t heartbeat(quint8 systemId, quint8 autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(systemId, 1, &message, MAV_TYPE_QUADROTOR, autopilot, 0, 3,
                               MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t globalPosition(qint32 lat, qint32 lon, qint32 altMm)
{
    mavlink_message_t message{};
    mavlink_msg_global_position_int_pack(1, 1, &message, 0, lat, lon, altMm, altMm, 0, 0, 0, 0);
    return message;
}

mavlink_message_t paramValue(quint8 systemId, const char *name, float value, quint8 type)
{
    mavlink_message_t message{};
    mavlink_msg_param_value_pack(systemId, 1, &message, name, value, type, 1, 0);
    return message;
}

float bytewiseInt32(qint32 value)
{
    float wire = 0.0f;
    std::memcpy(&wire, &value, sizeof(wire));
    return wire;
}

mavlink_message_t missionCount(quint8 systemId, quint16 count,
                               quint8 missionType = MAV_MISSION_TYPE_MISSION)
{
    mavlink_message_t message{};
    mavlink_msg_mission_count_pack(systemId, 1, &message, 255, 190, count, missionType);
    return message;
}

mavlink_message_t missionItemInt(quint8 systemId, quint16 sequence, qint32 x, qint32 y, quint16 command,
                                 quint8 frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT)
{
    mavlink_message_t message{};
    mavlink_msg_mission_item_int_pack(systemId, 1, &message, 255, 190, sequence, frame, command,
                                      sequence == 0 ? 1 : 0, 1, 0, 0, 0, 0, x, y, 50.0f,
                                      MAV_MISSION_TYPE_MISSION);
    return message;
}

struct OpenBuffer
{
    QByteArray data;
    QBuffer buffer;
    explicit OpenBuffer(const QByteArray &bytes)
        : data(bytes)
        , buffer(&data)
    {
        buffer.open(QIODevice::ReadOnly);
    }
};

constexpr qint64 kBase = 1700000000000000LL; // 2023-11-14T22:13:20Z

QString writeLog(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(bytes);
    file.close();
    return path;
}

QString readAll(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return QString::fromUtf8(file.readAll());
}

} // namespace

class TlogExportServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void formattingHelpersMatchMp10();
    void describePacketListsDialectFields();
    void readTrackFiltersInvalidPositions();
    void extractParametersArduPilotVersusBytewise();
    void extractMissionSnapshotsOutOfOrderAndDeduplicated();
    void exportWritesEveryFormat();
    void emptyResultsCancelAndFailuresCommitNothing();
};

void TlogExportServiceTest::formattingHelpersMatchMp10()
{
    QCOMPARE(TlogExportService::FormatCsvTimestamp(kBase + 123456), QStringLiteral("2023-11-14T22:13:20.123Z"));
    QCOMPARE(TlogExportService::FormatIsoTimestamp(kBase + 123456), QStringLiteral("2023-11-14T22:13:20.1234560Z"));
    QCOMPARE(TlogExportService::FormatIsoTimestamp(kBase), QStringLiteral("2023-11-14T22:13:20.0000000Z"));
    QCOMPARE(TlogExportService::FormatParameterValue(42.0), QStringLiteral("42"));
    QCOMPARE(TlogExportService::FormatParameterValue(1.5), QStringLiteral("1.5"));
    QCOMPARE(TlogExportService::FormatParameterValue(0.1), QStringLiteral("0.10000000000000001"));

    TlogMissionItem item;
    item.sequence = 1;
    item.current = 0;
    item.frame = 3;
    item.command = 16;
    item.param1 = 2.5f;
    item.x = 35.1234567;
    item.y = 33.7654321;
    item.z = 50.0;
    item.autoContinue = 1;
    QCOMPARE(TlogExportService::QgcWplLine(item),
             QStringLiteral("1\t0\t3\t16\t2.500000\t0.000000\t0.000000\t0.000000\t35.1234567\t33.7654321\t50.000000\t1"));

    QCOMPARE(TlogExportService::SnapshotOutputPaths(QStringLiteral("/tmp/x/mission.waypoints"), 3),
             (QStringList{QStringLiteral("/tmp/x/mission.waypoints"),
                          QStringLiteral("/tmp/x/mission-2.waypoints"),
                          QStringLiteral("/tmp/x/mission-3.waypoints")}));
    QCOMPARE(TlogExportService::SnapshotOutputPaths(QStringLiteral("/tmp/x/flight.2024.waypoints"), 2).at(1),
             QStringLiteral("/tmp/x/flight.2024-2.waypoints"));

    for (int frame : {0, 3, 10, 5, 6, 11}) {
        QVERIFY(TlogExportService::IsGlobalFrame(static_cast<quint8>(frame)));
    }
    QVERIFY(!TlogExportService::IsGlobalFrame(MAV_FRAME_LOCAL_NED));
    QVERIFY(TlogExportService::IsValidTrackPosition(35.0, 33.0));
    QVERIFY(!TlogExportService::IsValidTrackPosition(0.0, 0.0));
    QVERIFY(!TlogExportService::IsValidTrackPosition(91.0, 33.0));
    QVERIFY(!TlogExportService::IsValidTrackPosition(35.0, -181.0));

    QCOMPARE(TlogExportService::FormatLabel(TlogExportFormat::Text), QStringLiteral("human-readable text"));
    QCOMPARE(TlogExportService::DefaultExtension(TlogExportFormat::Missions), QStringLiteral("waypoints"));
    QCOMPARE(TlogExportService::DefaultExtension(TlogExportFormat::Parameters), QStringLiteral("param"));
    QCOMPARE(TlogExportService::CancelledText(TlogExportFormat::Kml), QStringLiteral("KML export cancelled."));
}

void TlogExportServiceTest::describePacketListsDialectFields()
{
    mavlink_message_t message = heartbeat(1);
    const QString csv = TlogExportService::DescribePacket(message, QStringLiteral(","));
    QVERIFY2(csv.startsWith(QStringLiteral("HEARTBEAT,type=2,autopilot=3,")), qPrintable(csv));
    QVERIFY(csv.contains(QStringLiteral("custom_mode=3")));
    QVERIFY(csv.contains(QStringLiteral("mavlink_version=3")));
    const QString text = TlogExportService::DescribePacket(message, QStringLiteral(" "));
    QVERIFY(text.startsWith(QStringLiteral("HEARTBEAT type=2 autopilot=3 ")));

    mavlink_message_t param = paramValue(1, "ABC", 1.5f, MAV_PARAM_TYPE_REAL32);
    const QString paramText = TlogExportService::DescribePacket(param, QStringLiteral(","));
    QVERIFY2(paramText.contains(QStringLiteral("param_id=\"ABC\"")), qPrintable(paramText));
    QVERIFY(paramText.contains(QStringLiteral("param_value=1.5")));

    mavlink_message_t unknown{};
    unknown.msgid = 60000;
    QVERIFY(TlogExportService::DescribePacket(unknown, QStringLiteral(",")).isEmpty());

    // MAVLink 2 truncates trailing zero bytes. Preserve the available low byte
    // of a partially present multi-byte extension field and zero-fill the rest.
    mavlink_message_t gps{};
    mavlink_msg_gps_raw_int_pack(1, 1, &gps, 0, 3, 0, 0, 0, 0, 0, 0, 0,
                                 10, 1, 0, 0, 0, 0, 0);
    QCOMPARE(gps.len, quint8(31));
    const QString gpsText = TlogExportService::DescribePacket(
        gps, QStringLiteral(","));
    QVERIFY2(gpsText.contains(QStringLiteral("alt_ellipsoid=1")),
             qPrintable(gpsText));
}

void TlogExportServiceTest::readTrackFiltersInvalidPositions()
{
    mavlink_message_t valid = globalPosition(351234567, 337654321, 123456);
    mavlink_message_t origin = globalPosition(0, 0, 1000);
    mavlink_message_t outOfRange = globalPosition(910000000, 337654321, 1000);
    mavlink_message_t hb = heartbeat(1);
    OpenBuffer log(record(kBase, hb) + record(kBase + 1000, valid) + record(kBase + 2000, origin)
                   + record(kBase + 3000, outOfRange));
    TlogReader reader(&log.buffer);
    const QVector<TlogTrackPoint> track = TlogExportService::ReadTrack(reader);
    QCOMPARE(track.size(), 1);
    QCOMPARE(track.first().latitude, 35.1234567);
    QCOMPARE(track.first().longitude, 33.7654321);
    QCOMPARE(track.first().altitudeMeters, 123.456);
    QCOMPARE(track.first().timestampUsec, kBase + 1000);
    QCOMPARE(reader.recordCount(), qint64(4));

    const QString kml = TlogExportService::KmlDocument(track);
    QVERIFY(kml.contains(QStringLiteral("<altitudeMode>absolute</altitudeMode>")));
    QVERIFY(kml.contains(QStringLiteral("33.7654321,35.1234567,123.46")));
    QVERIFY(kml.contains(QStringLiteral("Flight Path")));
    const QString gpx = TlogExportService::GpxDocument(track);
    QVERIFY(gpx.contains(QStringLiteral("<trkpt lat=\"35.1234567\" lon=\"33.7654321\">")));
    QVERIFY(gpx.contains(QStringLiteral("<time>2023-11-14T22:13:20Z</time>")));
}

void TlogExportServiceTest::extractParametersArduPilotVersusBytewise()
{
    // Sender 1 announces ArduPilot: the float carries the value (41 then 42 -> latest 42).
    // Sender 2 never announces ArduPilot: INT32 is decoded bytewise from the float bits.
    mavlink_message_t hb = heartbeat(1);
    mavlink_message_t first = paramValue(1, "TEST_VALUE", 41.0f, MAV_PARAM_TYPE_INT32);
    mavlink_message_t second = paramValue(1, "TEST_VALUE", 42.0f, MAV_PARAM_TYPE_INT32);
    mavlink_message_t px4 = paramValue(2, "PX_INT", bytewiseInt32(7), MAV_PARAM_TYPE_INT32);
    mavlink_message_t lower = paramValue(1, "abc", 1.0f, MAV_PARAM_TYPE_REAL32);
    mavlink_message_t upper = paramValue(1, "ABC", 2.5f, MAV_PARAM_TYPE_REAL32);
    mavlink_message_t empty = paramValue(1, "", 9.0f, MAV_PARAM_TYPE_REAL32);
    OpenBuffer log(record(kBase, hb) + record(kBase + 1, first) + record(kBase + 2, second)
                   + record(kBase + 3, px4) + record(kBase + 4, lower) + record(kBase + 5, upper)
                   + record(kBase + 6, empty));
    TlogReader reader(&log.buffer);
    const QMap<QString, TlogParameter> values = TlogExportService::ExtractParameters(reader);
    QCOMPARE(values.size(), 3);
    QCOMPARE(values.value(QStringLiteral("TEST_VALUE")).value, 42.0);
    QCOMPARE(values.value(QStringLiteral("PX_INT")).value, 7.0);
    QCOMPARE(values.value(QStringLiteral("ABC")).name, QStringLiteral("abc")); // first spelling
    QCOMPARE(values.value(QStringLiteral("ABC")).value, 2.5);                  // latest value
    QCOMPARE(TlogExportService::ParameterLines(values),
             (QStringList{QStringLiteral("abc\t2.5"), QStringLiteral("PX_INT\t7"),
                          QStringLiteral("TEST_VALUE\t42")}));
}

void TlogExportServiceTest::extractMissionSnapshotsOutOfOrderAndDeduplicated()
{
    // Mirror of MP10 TlogExportServiceTests.ExtractMissionSnapshots_accepts_out_of_order_int_items_and_deduplicates.
    mavlink_message_t count = missionCount(1, 2);
    mavlink_message_t first = missionItemInt(1, 0, 351234567, 337654321, MAV_CMD_NAV_WAYPOINT);
    mavlink_message_t second = missionItemInt(1, 1, 351235000, 337655000, MAV_CMD_NAV_RETURN_TO_LAUNCH);
    mavlink_message_t otherCount = missionCount(7, 1);
    mavlink_message_t otherItem = missionItemInt(7, 0, 100000000, 200000000, MAV_CMD_NAV_TAKEOFF);
    mavlink_message_t fenceCount = missionCount(1, 1, MAV_MISSION_TYPE_FENCE);
    mavlink_message_t orphan = missionItemInt(9, 0, 1, 2, MAV_CMD_NAV_WAYPOINT); // no MISSION_COUNT
    OpenBuffer log(record(kBase, count) + record(kBase + 1, second) + record(kBase + 2, first)
                   + record(kBase + 3, count) + record(kBase + 4, first) + record(kBase + 5, second)
                   + record(kBase + 6, otherCount) + record(kBase + 7, otherItem)
                   + record(kBase + 8, fenceCount) + record(kBase + 9, orphan));
    TlogReader reader(&log.buffer);
    const QVector<QVector<TlogMissionItem>> snapshots = TlogExportService::ExtractMissionSnapshots(reader);
    QCOMPARE(snapshots.size(), 2);
    const QVector<TlogMissionItem> &mission = snapshots.first();
    QCOMPARE(mission.size(), 2);
    QCOMPARE(mission.at(0).sequence, quint16(0));
    QVERIFY(qAbs(mission.at(0).x - 35.1234567) < 1e-7);
    QVERIFY(qAbs(mission.at(0).y - 33.7654321) < 1e-7);
    QCOMPARE(mission.at(0).current, quint8(1));
    QCOMPARE(mission.at(1).command, quint16(MAV_CMD_NAV_RETURN_TO_LAUNCH));
    QCOMPARE(mission.at(1).frame, quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT_INT));
    QCOMPARE(mission.at(1).z, 50.0);
    QCOMPARE(snapshots.at(1).size(), 1);
    QCOMPARE(snapshots.at(1).first().command, quint16(MAV_CMD_NAV_TAKEOFF));
    QCOMPARE(TlogExportService::QgcWplLine(mission.at(0)).section(QLatin1Char('\t'), 0, 3),
             QStringLiteral("0\t1\t6\t16"));

    mavlink_message_t localCount = missionCount(8, 1);
    mavlink_message_t localItem = missionItemInt(
        8, 0, 123456, -78901, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_LOCAL_NED);
    OpenBuffer localLog(record(kBase, localCount)
                        + record(kBase + 1, localItem));
    TlogReader localReader(&localLog.buffer);
    const auto localSnapshots =
        TlogExportService::ExtractMissionSnapshots(localReader);
    QCOMPARE(localSnapshots.size(), 1);
    QVERIFY(qAbs(localSnapshots.first().first().x - 12.3456) < 1e-9);
    QVERIFY(qAbs(localSnapshots.first().first().y - -7.8901) < 1e-9);
}

void TlogExportServiceTest::exportWritesEveryFormat()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    mavlink_message_t hb = heartbeat(1);
    mavlink_message_t gpi = globalPosition(351234567, 337654321, 123456);
    mavlink_message_t param = paramValue(1, "TEST_VALUE", 42.0f, MAV_PARAM_TYPE_INT32);
    mavlink_message_t count = missionCount(1, 1);
    mavlink_message_t item = missionItemInt(1, 0, 351234567, 337654321, MAV_CMD_NAV_WAYPOINT);
    mavlink_message_t count2 = missionCount(1, 1);
    mavlink_message_t item2 = missionItemInt(1, 0, 351235000, 337655000, MAV_CMD_NAV_TAKEOFF);
    const QString input = writeLog(dir, QStringLiteral("flight.tlog"),
                                   record(kBase, hb) + record(kBase + 1000, gpi) + record(kBase + 2000, param)
                                       + record(kBase + 3000, count) + record(kBase + 4000, item)
                                       + record(kBase + 5000, count2) + record(kBase + 6000, item2));

    TlogExportResult kml = TlogExportService::Export(TlogExportFormat::Kml, input, dir.filePath(QStringLiteral("flight.kml")));
    QVERIFY2(kml.success, qPrintable(kml.error));
    QCOMPARE(kml.itemCount, 1);
    QCOMPARE(kml.outputPaths, QStringList{dir.filePath(QStringLiteral("flight.kml"))});
    QVERIFY(readAll(kml.outputPaths.first()).contains(QStringLiteral("33.7654321,35.1234567,123.46")));
    QCOMPARE(kml.message, QStringLiteral("Wrote KML: %1").arg(kml.outputPaths.first()));
    QCOMPARE(kml.recordsRead, qint64(7));

    TlogExportResult gpx = TlogExportService::Export(TlogExportFormat::Gpx, input, dir.filePath(QStringLiteral("flight.gpx")));
    QVERIFY2(gpx.success, qPrintable(gpx.error));
    QVERIFY(readAll(gpx.outputPaths.first()).contains(QStringLiteral("<trkpt lat=\"35.1234567\"")));

    qint64 lastProgress = -1;
    TlogExportResult csv = TlogExportService::Export(
        TlogExportFormat::Csv, input, dir.filePath(QStringLiteral("flight.csv")), {},
        [&lastProgress](qint64 done, qint64) { lastProgress = done; });
    QVERIFY2(csv.success, qPrintable(csv.error));
    QCOMPARE(csv.itemCount, 7);
    QVERIFY(lastProgress > 0);
    const QStringList csvLines = readAll(csv.outputPaths.first()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(csvLines.size(), 7);
    QVERIFY2(csvLines.first().startsWith(QStringLiteral("2023-11-14T22:13:20.000Z,HEARTBEAT,type=2,autopilot=3")),
             qPrintable(csvLines.first()));
    QCOMPARE(csv.message, QStringLiteral("Wrote 7 decoded packets to %1").arg(csv.outputPaths.first()));

    TlogExportResult text = TlogExportService::Export(TlogExportFormat::Text, input, dir.filePath(QStringLiteral("flight.txt")));
    QVERIFY2(text.success, qPrintable(text.error));
    const QStringList textLines = readAll(text.outputPaths.first()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QVERIFY2(textLines.first().startsWith(QStringLiteral("2023-11-14T22:13:20.0000000Z HEARTBEAT type=2 autopilot=3")),
             qPrintable(textLines.first()));

    TlogExportResult params = TlogExportService::Export(TlogExportFormat::Parameters, input, dir.filePath(QStringLiteral("flight.param")));
    QVERIFY2(params.success, qPrintable(params.error));
    QCOMPARE(params.itemCount, 1);
    QCOMPARE(readAll(params.outputPaths.first()), QStringLiteral("TEST_VALUE\t42\n"));
    QCOMPARE(params.message, QStringLiteral("Wrote 1 parameters to %1").arg(params.outputPaths.first()));

    TlogExportResult missions = TlogExportService::Export(TlogExportFormat::Missions, input, dir.filePath(QStringLiteral("flight.waypoints")));
    QVERIFY2(missions.success, qPrintable(missions.error));
    QCOMPARE(missions.itemCount, 2);
    QCOMPARE(missions.outputPaths, (QStringList{dir.filePath(QStringLiteral("flight.waypoints")),
                                                dir.filePath(QStringLiteral("flight-2.waypoints"))}));
    const QString wpl = readAll(missions.outputPaths.first());
    QVERIFY2(wpl.startsWith(QStringLiteral("QGC WPL 110\n0\t1\t6\t16\t0.000000\t0.000000\t0.000000\t0.000000\t35.1234567\t33.7654321\t50.000000\t1\n")),
             qPrintable(wpl));
    QVERIFY(readAll(missions.outputPaths.at(1)).contains(QStringLiteral("\t22\t"))); // MAV_CMD_NAV_TAKEOFF
    QVERIFY(missions.message.startsWith(QStringLiteral("Wrote 2 unique mission snapshot(s): ")));
}

void TlogExportServiceTest::emptyResultsCancelAndFailuresCommitNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    mavlink_message_t hb = heartbeat(1);
    const QString input = writeLog(dir, QStringLiteral("empty.tlog"), record(kBase, hb) + record(kBase + 1, hb));

    const QString kmlPath = dir.filePath(QStringLiteral("empty.kml"));
    TlogExportResult kml = TlogExportService::Export(TlogExportFormat::Kml, input, kmlPath);
    QVERIFY(!kml.success);
    QVERIFY(!kml.cancelled);
    QCOMPARE(kml.error, TlogExportService::NoGpsPositionsText());
    QCOMPARE(kml.recordsRead, qint64(2));
    QVERIFY(kml.outputPaths.isEmpty());
    QVERIFY(!QFile::exists(kmlPath));

    TlogExportResult params = TlogExportService::Export(TlogExportFormat::Parameters, input, dir.filePath(QStringLiteral("empty.param")));
    QCOMPARE(params.error, TlogExportService::NoParametersText());
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("empty.param"))));

    TlogExportResult missions = TlogExportService::Export(TlogExportFormat::Missions, input, dir.filePath(QStringLiteral("empty.waypoints")));
    QCOMPARE(missions.error, TlogExportService::NoMissionText());

    // CSV of heartbeats only is a valid, non-empty export (MP10 writes what it decodes).
    TlogExportResult csv = TlogExportService::Export(TlogExportFormat::Csv, input, dir.filePath(QStringLiteral("empty.csv")));
    QVERIFY(csv.success);
    QCOMPARE(csv.itemCount, 2);

    // Cancellation before the first record: nothing is committed.
    const QString cancelledPath = dir.filePath(QStringLiteral("cancelled.csv"));
    TlogExportResult cancelled = TlogExportService::Export(TlogExportFormat::Csv, input, cancelledPath, []() { return true; });
    QVERIFY(!cancelled.success);
    QVERIFY(cancelled.cancelled);
    QCOMPARE(cancelled.message, QStringLiteral("CSV export cancelled."));
    QVERIFY(!QFile::exists(cancelledPath));

    // Unwritable destination: explicit error, no partial file anywhere.
    TlogExportResult unwritable = TlogExportService::Export(
        TlogExportFormat::Csv, input, dir.filePath(QStringLiteral("missing-dir/out.csv")));
    QVERIFY(!unwritable.success);
    QVERIFY2(unwritable.error.startsWith(QStringLiteral("Cannot write ")), qPrintable(unwritable.error));

    // Missing input / missing output selection.
    TlogExportResult missingInput = TlogExportService::Export(TlogExportFormat::Csv, dir.filePath(QStringLiteral("nope.tlog")), dir.filePath(QStringLiteral("x.csv")));
    QVERIFY(missingInput.error.startsWith(QStringLiteral("Cannot open ")));
    TlogExportResult noOutput = TlogExportService::Export(TlogExportFormat::Csv, input, QString());
    QCOMPARE(noOutput.error, QStringLiteral("No output file selected."));

    // A malicious or accidental Save As selection must never replace the source log.
    QFile original(input);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = original.readAll();
    original.close();
    const TlogExportResult samePath = TlogExportService::Export(
        TlogExportFormat::Csv, input, input);
    QVERIFY(!samePath.success);
    QVERIFY(samePath.error.contains(QStringLiteral("must not replace")));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), originalBytes);

    // Mission siblings are implicit: they may neither replace the input nor
    // overwrite an unrelated pre-existing file that was not selected in Save As.
    mavlink_message_t count = missionCount(1, 1);
    mavlink_message_t item = missionItemInt(
        1, 0, 351234567, 337654321, MAV_CMD_NAV_WAYPOINT);
    mavlink_message_t count2 = missionCount(1, 1);
    mavlink_message_t item2 = missionItemInt(
        1, 0, 351235000, 337655000, MAV_CMD_NAV_TAKEOFF);
    const QByteArray twoMissions = record(kBase, count) + record(kBase + 1, item)
        + record(kBase + 2, count2) + record(kBase + 3, item2);
    const QString siblingInput = writeLog(
        dir, QStringLiteral("mission-2.tlog"), twoMissions);
    const TlogExportResult siblingInputResult = TlogExportService::Export(
        TlogExportFormat::Missions, siblingInput,
        dir.filePath(QStringLiteral("mission.tlog")));
    QVERIFY(!siblingInputResult.success);
    QVERIFY(siblingInputResult.error.contains(QStringLiteral("replace the input")));
    QCOMPARE(readAll(siblingInput), QString::fromUtf8(twoMissions));

    const QString protectedSibling = dir.filePath(
        QStringLiteral("protected-2.waypoints"));
    QFile sentinel(protectedSibling);
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    QCOMPARE(sentinel.write("do not replace"), qint64(14));
    sentinel.close();
    const QString selectedMission = dir.filePath(
        QStringLiteral("protected.waypoints"));
    const TlogExportResult protectedResult = TlogExportService::Export(
        TlogExportFormat::Missions, siblingInput, selectedMission);
    QVERIFY(!protectedResult.success);
    QVERIFY(protectedResult.error.contains(QStringLiteral("already exists")));
    QVERIFY(!QFile::exists(selectedMission));
    QCOMPARE(readAll(protectedSibling), QStringLiteral("do not replace"));
}

QTEST_GUILESS_MAIN(TlogExportServiceTest)
#include "test_tlogexportservice.moc"
