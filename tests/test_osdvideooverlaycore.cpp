#include "ui/tools/OsdVideoOverlayCore.h"

#include <QBuffer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <mavlink.h>

#include <algorithm>
#include <iterator>
#include <limits>

namespace
{
constexpr qint64 kBase = 1700000000000000LL;

QByteArray timestampBytes(qint64 usec)
{
    QByteArray bytes(8, '\0');
    quint64 value = quint64(usec);
    for (int index = 7; index >= 0; --index) {
        bytes[index] = char(value & 0xff);
        value >>= 8;
    }
    return bytes;
}

QByteArray record(qint64 usec, const mavlink_message_t &message)
{
    quint8 bytes[MAVLINK_MAX_PACKET_LEN]{};
    const int size = mavlink_msg_to_send_buffer(bytes, &message);
    return timestampBytes(usec)
        + QByteArray(reinterpret_cast<const char *>(bytes), size);
}

template<typename Payload>
mavlink_message_t encoded(
    const Payload &payload,
    uint16_t (*encode)(uint8_t, uint8_t, mavlink_message_t *, const Payload *))
{
    mavlink_message_t message{};
    encode(1, 1, &message, &payload);
    return message;
}

void createFile(const QString &path, const QByteArray &contents = QByteArray("x"))
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), qint64(contents.size()));
}
}

class OsdVideoOverlayCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void timelineSortsDeduplicatesAndClamps();
    void bucketUsesFloorForPositiveAndNegativeTimes();
    void loadsMavlinkStateIntoLastSampleOfEachBucket();
    void ignoresStateUntilAnAutopilotHeartbeatSelectsTheLogVehicle();
    void cancelledLoadReturnsNoPartialTimeline();
    void validatesPathsOffsetsAndEncodingOptions();
    void defaultOutputNeverOverwrites();
    void outputSizeAndFrameRateMatchMissionPlanner();
    void cfrFrameIndexRoundsPresentationTimestamps();
};

void OsdVideoOverlayCoreTest::timelineSortsDeduplicatesAndClamps()
{
    OsdVideoTelemetrySample first;
    first.timestampUsec = kBase;
    first.roll = 0;
    OsdVideoTelemetrySample duplicate = first;
    duplicate.timestampUsec += 1000000;
    duplicate.roll = 10;
    OsdVideoTelemetrySample replacement = duplicate;
    replacement.roll = 11;
    OsdVideoTelemetrySample last = first;
    last.timestampUsec += 3000000;
    last.roll = 30;

    const OsdVideoTelemetryTimeline timeline(
        {last, first, duplicate, replacement});

    QCOMPARE(timeline.count(), 3);
    QCOMPARE(timeline.startTimeUsec(), kBase);
    QCOMPARE(timeline.endTimeUsec(), kBase + 3000000);
    QCOMPARE(timeline.sampleAt(-5000000, 0).roll, 0.0);
    QCOMPARE(timeline.sampleAt(1900000, 0).roll, 11.0);
    QCOMPARE(timeline.sampleAt(1000000, 2000000).roll, 30.0);
    QCOMPARE(timeline.sampleAt(2000000, -10000000).roll, 0.0);
    QCOMPARE(timeline.sampleAt(std::numeric_limits<qint64>::max(), 1).roll,
             30.0);
}

void OsdVideoOverlayCoreTest::bucketUsesFloorForPositiveAndNegativeTimes()
{
    QCOMPARE(OsdVideoTelemetryTimeline::BucketStartUsec(1987654), qint64(1900000));
    QCOMPARE(OsdVideoTelemetryTimeline::BucketStartUsec(-1), qint64(-100000));
    QCOMPARE(OsdVideoTelemetryTimeline::BucketStartUsec(-100000), qint64(-100000));
}

void OsdVideoOverlayCoreTest::loadsMavlinkStateIntoLastSampleOfEachBucket()
{
    mavlink_heartbeat_t heartbeat{};
    heartbeat.type = MAV_TYPE_QUADROTOR;
    heartbeat.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    heartbeat.base_mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED
        | MAV_MODE_FLAG_SAFETY_ARMED;
    heartbeat.custom_mode = 2;
    heartbeat.system_status = MAV_STATE_ACTIVE;

    mavlink_attitude_t attitude{};
    attitude.roll = float(10.0 / 180.0 * 3.14159265358979323846);
    attitude.pitch = float(-5.0 / 180.0 * 3.14159265358979323846);
    attitude.yaw = float(90.0 / 180.0 * 3.14159265358979323846);

    mavlink_vfr_hud_t hud{};
    hud.airspeed = 21.5f;
    hud.groundspeed = 18.25f;
    hud.climb = -2.75f;
    hud.throttle = 47;

    mavlink_global_position_int_t position{};
    position.relative_alt = 123500;

    mavlink_gps_raw_int_t gps{};
    gps.fix_type = 3;
    gps.satellites_visible = 17;
    gps.vel = 1825;

    mavlink_sys_status_t system{};
    system.voltage_battery = 15800;
    system.current_battery = 420;
    system.battery_remaining = 73;
    system.onboard_control_sensors_enabled = MAV_SYS_STATUS_PREARM_CHECK
        | MAV_SYS_STATUS_SENSOR_MOTOR_OUTPUTS;
    system.onboard_control_sensors_health = MAV_SYS_STATUS_PREARM_CHECK;

    mavlink_battery_status_t battery2{};
    for (int index = 0;
         index < MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_LEN; ++index) {
        battery2.voltages[index] = std::numeric_limits<quint16>::max();
    }
    for (int index = 0;
         index < MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_EXT_LEN; ++index) {
        battery2.voltages_ext[index] = std::numeric_limits<quint16>::max();
    }
    battery2.id = 1;
    battery2.voltages[0] = 4000;
    battery2.voltages[1] = 3900;
    battery2.current_battery = 250;
    battery2.battery_remaining = 64;

    mavlink_nav_controller_output_t navigation{};
    navigation.nav_bearing = 123;
    navigation.wp_dist = 456;
    navigation.xtrack_error = -7.5f;
    navigation.alt_error = 27.0f;
    navigation.aspd_error = 350.0f;

    mavlink_wind_t wind{};
    wind.direction = -10.0f;
    wind.speed = 8.5f;

    mavlink_aoa_ssa_t aoa{};
    aoa.AOA = 12.25f;
    aoa.SSA = -1.75f;

    mavlink_mission_current_t mission{};
    mission.seq = 9;

    QByteArray bytes;
    bytes += record(kBase + 1, encoded(heartbeat, mavlink_msg_heartbeat_encode));
    bytes += record(kBase + 50000, encoded(attitude, mavlink_msg_attitude_encode));
    bytes += record(kBase + 120000, encoded(hud, mavlink_msg_vfr_hud_encode));
    bytes += record(kBase + 220000,
                    encoded(position, mavlink_msg_global_position_int_encode));
    bytes += record(kBase + 320000, encoded(gps, mavlink_msg_gps_raw_int_encode));
    bytes += record(kBase + 420000, encoded(system, mavlink_msg_sys_status_encode));
    bytes += record(kBase + 520000,
                    encoded(battery2, mavlink_msg_battery_status_encode));
    bytes += record(kBase + 620000,
                    encoded(navigation, mavlink_msg_nav_controller_output_encode));
    bytes += record(kBase + 720000, encoded(wind, mavlink_msg_wind_encode));
    bytes += record(kBase + 820000, encoded(aoa, mavlink_msg_aoa_ssa_encode));
    bytes += record(kBase + 920000,
                    encoded(mission, mavlink_msg_mission_current_encode));
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    OsdVideoTelemetryTimeline::LoadStatus status;
    QString error;
    qint64 lastProgress = 0;
    const OsdVideoTelemetryTimeline timeline = OsdVideoTelemetryTimeline::Load(
        &buffer, &status, &error, {},
        [&lastProgress](qint64 done, qint64) { lastProgress = done; });

    QCOMPARE(status, OsdVideoTelemetryTimeline::LoadStatus::Ok);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(timeline.count(), 10); // heartbeat + attitude share the first bucket
    QCOMPARE(lastProgress, qint64(bytes.size()));
    const OsdVideoTelemetrySample sample = timeline.samples().last();
    QVERIFY(sample.armed);
    QCOMPARE(sample.mode, QStringLiteral("ALT_HOLD"));
    QVERIFY(qAbs(sample.roll - 10.0) < 0.01);
    QVERIFY(qAbs(sample.pitch + 5.0) < 0.01);
    QVERIFY(qAbs(sample.yaw - 90.0) < 0.01);
    QCOMPARE(sample.altitude, 123.5);
    QCOMPARE(sample.airSpeed, 21.5);
    QCOMPARE(sample.groundSpeed, 18.25);
    QCOMPARE(sample.verticalSpeed, -2.75);
    QCOMPARE(sample.throttlePercent, 47.0);
    QCOMPARE(sample.gpsFixType, 3);
    QCOMPARE(sample.satelliteCount, 17.0);
    QCOMPARE(sample.batteryVoltage, 15.8);
    QCOMPARE(sample.batteryRemaining, 73);
    QCOMPARE(sample.currentAmps, 4.2);
    QVERIFY(sample.prearmOk);
    QVERIFY(!sample.safetyActive);
    QCOMPARE(sample.batteryVoltage2, 7.9);
    QCOMPARE(sample.batteryRemaining2, 64);
    QCOMPARE(sample.currentAmps2, 2.5);
    QCOMPARE(sample.navBearing, 123.0);
    QCOMPARE(sample.waypointDistance, 456.0);
    QCOMPARE(sample.xtrackError, -7.5);
    QCOMPARE(sample.targetAltitude, 75.5);
    QCOMPARE(sample.targetSpeed, 12.5);
    QCOMPARE(sample.windDirection, 350.0);
    QCOMPARE(sample.windSpeed, 8.5);
    QCOMPARE(sample.aoa, 12.25);
    QCOMPARE(sample.ssa, -1.75);
    QCOMPARE(sample.waypointNumber, 9);
    QVERIFY(sample.turnRate > 5.3 && sample.turnRate < 5.4);
    QVERIFY(sample.linkQuality > 0.0 && sample.linkQuality <= 100.0);
}

void OsdVideoOverlayCoreTest::ignoresStateUntilAnAutopilotHeartbeatSelectsTheLogVehicle()
{
    mavlink_attitude_t foreignAttitude{};
    foreignAttitude.roll = float(42.0 / 180.0 * 3.14159265358979323846);
    mavlink_message_t attitudeMessage{};
    mavlink_msg_attitude_encode(2, MAV_COMP_ID_AUTOPILOT1,
                                &attitudeMessage, &foreignAttitude);

    mavlink_heartbeat_t companion{};
    companion.type = MAV_TYPE_ONBOARD_CONTROLLER;
    companion.autopilot = MAV_AUTOPILOT_INVALID;
    mavlink_message_t companionHeartbeat{};
    mavlink_msg_heartbeat_encode(1, MAV_COMP_ID_ONBOARD_COMPUTER,
                                 &companionHeartbeat, &companion);

    mavlink_heartbeat_t autopilot{};
    autopilot.type = MAV_TYPE_QUADROTOR;
    autopilot.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    mavlink_message_t autopilotHeartbeat{};
    mavlink_msg_heartbeat_encode(1, MAV_COMP_ID_AUTOPILOT1,
                                 &autopilotHeartbeat, &autopilot);

    QByteArray bytes;
    bytes += record(kBase, attitudeMessage);
    bytes += record(kBase + 100000, companionHeartbeat);
    bytes += record(kBase + 200000, autopilotHeartbeat);
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    OsdVideoTelemetryTimeline::LoadStatus status;
    QString error;

    const OsdVideoTelemetryTimeline timeline =
        OsdVideoTelemetryTimeline::Load(&buffer, &status, &error);

    QCOMPARE(status, OsdVideoTelemetryTimeline::LoadStatus::Ok);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(timeline.count(), 1);
    QCOMPARE(timeline.startTimeUsec(),
             OsdVideoTelemetryTimeline::BucketStartUsec(kBase + 200000));
    QCOMPARE(timeline.samples().first().roll, 0.0);
}

void OsdVideoOverlayCoreTest::cancelledLoadReturnsNoPartialTimeline()
{
    mavlink_heartbeat_t heartbeat{};
    heartbeat.type = MAV_TYPE_QUADROTOR;
    const mavlink_message_t message =
        encoded(heartbeat, mavlink_msg_heartbeat_encode);
    QByteArray bytes = record(kBase, message) + record(kBase + 100000, message);
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    int checks = 0;
    OsdVideoTelemetryTimeline::LoadStatus status;
    const OsdVideoTelemetryTimeline timeline = OsdVideoTelemetryTimeline::Load(
        &buffer, &status, nullptr, [&checks]() { return ++checks > 1; });

    QCOMPARE(status, OsdVideoTelemetryTimeline::LoadStatus::Cancelled);
    QVERIFY(timeline.isEmpty());
}

void OsdVideoOverlayCoreTest::validatesPathsOffsetsAndEncodingOptions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString video = temporary.filePath(QStringLiteral("camera.mp4"));
    const QString tlog = temporary.filePath(QStringLiteral("flight.tlog"));
    const QString output = temporary.filePath(QStringLiteral("overlay.avi"));
    createFile(video);
    createFile(tlog);

    OsdVideoExportOptions options{video, tlog, output};
    QVERIFY2(OsdVideoOverlayCore::Validate(options).isEmpty(),
             qPrintable(OsdVideoOverlayCore::Validate(options)));

    options.outputPath = video;
    QVERIFY(OsdVideoOverlayCore::Validate(options).contains(QStringLiteral("differ")));
    options.outputPath = output;
    options.timeOffsetSeconds = 901;
    QVERIFY(OsdVideoOverlayCore::Validate(options).contains(QStringLiteral("offset")));
    options.timeOffsetSeconds = 0;
    options.jpegQuality = 0;
    QVERIFY(OsdVideoOverlayCore::Validate(options).contains(QStringLiteral("quality"),
                                                            Qt::CaseInsensitive));
    options.jpegQuality = 85;
    options.tlogPath = temporary.filePath(QStringLiteral("flight.bin"));
    createFile(options.tlogPath);
    QVERIFY(OsdVideoOverlayCore::Validate(options).contains(QStringLiteral(".tlog")));
    options.tlogPath = tlog;
    createFile(output);
    QVERIFY(OsdVideoOverlayCore::Validate(options).contains(QStringLiteral("already")));
}

void OsdVideoOverlayCoreTest::defaultOutputNeverOverwrites()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString video = temporary.filePath(QStringLiteral("camera.test.avi"));
    createFile(video);
    QCOMPARE(OsdVideoOverlayCore::DefaultOutputPath(video),
             temporary.filePath(QStringLiteral("camera.test-overlay.avi")));
    createFile(temporary.filePath(QStringLiteral("camera.test-overlay.avi")));
    QCOMPARE(OsdVideoOverlayCore::DefaultOutputPath(video),
             temporary.filePath(QStringLiteral("camera.test-overlay-2.avi")));
}

void OsdVideoOverlayCoreTest::outputSizeAndFrameRateMatchMissionPlanner()
{
    QCOMPARE(OsdVideoOverlayCore::OutputSize(1920, 1080, false), QSize(960, 540));
    QCOMPARE(OsdVideoOverlayCore::OutputSize(1920, 1081, false), QSize(960, 540));
    QCOMPARE(OsdVideoOverlayCore::OutputSize(640, 480, false), QSize(640, 480));
    QCOMPARE(OsdVideoOverlayCore::OutputSize(1920, 1081, true), QSize(1920, 1081));
    QVERIFY(!OsdVideoOverlayCore::OutputSize(0, 1080, false).isValid());

    QCOMPARE(OsdVideoOverlayCore::ClampFramesPerSecond(0), 25);
    QCOMPARE(OsdVideoOverlayCore::ClampFramesPerSecond(
                 std::numeric_limits<double>::quiet_NaN()), 25);
    QCOMPARE(OsdVideoOverlayCore::ClampFramesPerSecond(0.6), 1);
    QCOMPARE(OsdVideoOverlayCore::ClampFramesPerSecond(29.6), 30);
    QCOMPARE(OsdVideoOverlayCore::ClampFramesPerSecond(500), 120);
}

void OsdVideoOverlayCoreTest::cfrFrameIndexRoundsPresentationTimestamps()
{
    QCOMPARE(OsdVideoOverlayCore::FrameIndexForTimestamp(0, 30), qint64(0));
    QCOMPARE(OsdVideoOverlayCore::FrameIndexForTimestamp(33333, 30), qint64(1));
    QCOMPARE(OsdVideoOverlayCore::FrameIndexForTimestamp(66667, 30), qint64(2));
    QCOMPARE(OsdVideoOverlayCore::FrameIndexForTimestamp(40000, 25), qint64(1));
    QCOMPARE(OsdVideoOverlayCore::FrameIndexForTimestamp(-1, 25), qint64(0));
}

QTEST_GUILESS_MAIN(OsdVideoOverlayCoreTest)
#include "test_osdvideooverlaycore.moc"
