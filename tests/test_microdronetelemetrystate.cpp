#include <QtTest/QtTest>

#include "comm/MicrodroneTelemetryState.h"

#include <cmath>
#include <limits>

namespace {

#define DEFINE_MESSAGE_ENCODER(Type, Name)                                      \
    mavlink_message_t makeMessage(const Type &value)                            \
    {                                                                            \
        mavlink_message_t message{};                                              \
        mavlink_msg_##Name##_encode(42, 1, &message, &value);                    \
        return message;                                                           \
    }

DEFINE_MESSAGE_ENCODER(mavlink_high_latency_t, high_latency)
DEFINE_MESSAGE_ENCODER(mavlink_high_latency2_t, high_latency2)
DEFINE_MESSAGE_ENCODER(mavlink_home_position_t, home_position)
DEFINE_MESSAGE_ENCODER(mavlink_attitude_t, attitude)
DEFINE_MESSAGE_ENCODER(mavlink_global_position_int_t, global_position_int)
DEFINE_MESSAGE_ENCODER(mavlink_gps_raw_int_t, gps_raw_int)
DEFINE_MESSAGE_ENCODER(mavlink_gps_status_t, gps_status)
DEFINE_MESSAGE_ENCODER(mavlink_vfr_hud_t, vfr_hud)
DEFINE_MESSAGE_ENCODER(mavlink_scaled_pressure_t, scaled_pressure)
DEFINE_MESSAGE_ENCODER(mavlink_raw_imu_t, raw_imu)
DEFINE_MESSAGE_ENCODER(mavlink_scaled_imu_t, scaled_imu)
DEFINE_MESSAGE_ENCODER(mavlink_highres_imu_t, highres_imu)
DEFINE_MESSAGE_ENCODER(mavlink_heartbeat_t, heartbeat)

#undef DEFINE_MESSAGE_ENCODER

QDateTime at(int milliseconds)
{
    return QDateTime(QDate(2026, 9, 6), QTime(12, 0), Qt::UTC)
        .addMSecs(milliseconds);
}

void near(double actual, double expected, double tolerance = 1.0e-6)
{
    QVERIFY2(std::abs(actual - expected) <= tolerance,
             qPrintable(QStringLiteral("actual=%1 expected=%2")
                 .arg(actual, 0, 'g', 17).arg(expected, 0, 'g', 17)));
}

} // namespace

class MicrodroneTelemetryStateTest : public QObject
{
    Q_OBJECT
private slots:
    void globalPositionFallbackAndSentinels();
    void altitudeEmaAndClockRollback();
    void gpsStatusCourseAndVfrSources();
    void attitudeAndHighLatencyVariants();
    void primaryMagnetometerAndTemperatureSources();
    void clearResetsAllState();
};

void MicrodroneTelemetryStateTest::globalPositionFallbackAndSentinels()
{
    MicrodroneTelemetryState state;
    mavlink_global_position_int_t global{};
    global.lat = 123456789;
    global.lon = -234567890;
    global.relative_alt = 100000;
    QVERIFY(state.Apply(makeMessage(global), at(0)));
    near(state.snapshot().latitude, 12.3456789, 1.0e-12);
    near(state.snapshot().longitude, -23.456789, 1.0e-12);
    QCOMPARE(state.snapshot().altitude, 100.0);

    mavlink_gps_raw_int_t gps{};
    gps.lat = 450000000;
    gps.lon = 900000000;
    gps.eph = std::numeric_limits<quint16>::max();
    gps.vel = std::numeric_limits<quint16>::max();
    gps.cog = std::numeric_limits<quint16>::max();
    gps.satellites_visible = std::numeric_limits<quint8>::max();
    QVERIFY(state.Apply(makeMessage(gps), at(10)));
    near(state.snapshot().latitude, 12.3456789, 1.0e-12);
    near(state.snapshot().longitude, -23.456789, 1.0e-12);

    // A bad GLOBAL_POSITION_INT clears CurrentState.useLocation, while still
    // updating relative altitude. GPS_RAW_INT can then update each valid axis.
    global.lat = 0;
    global.lon = 10000000;
    global.relative_alt = 101000;
    QVERIFY(state.Apply(makeMessage(global), at(20)));
    gps.lat = std::numeric_limits<qint32>::max();
    gps.lon = 345678901;
    QVERIFY(state.Apply(makeMessage(gps), at(30)));
    near(state.snapshot().latitude, 12.3456789, 1.0e-12);
    near(state.snapshot().longitude, 34.5678901, 1.0e-12);
}

void MicrodroneTelemetryStateTest::altitudeEmaAndClockRollback()
{
    MicrodroneTelemetryState state;
    mavlink_global_position_int_t global{};
    global.lat = 10000000;
    global.lon = 20000000;

    global.relative_alt = 100000;
    QVERIFY(state.Apply(makeMessage(global), at(0)));
    const double initialVerticalSpeed = state.snapshot().verticalSpeed;
    QVERIFY(std::abs(initialVerticalSpeed) < 1.0e-6);

    global.relative_alt = 110000;
    QVERIFY(state.Apply(makeMessage(global), at(100)));
    QCOMPARE(state.snapshot().verticalSpeed, initialVerticalSpeed);

    global.relative_alt = 120000;
    QVERIFY(state.Apply(makeMessage(global), at(200)));
    near(state.snapshot().verticalSpeed, 60.0, 1.0e-4);

    // CurrentState deliberately accepts a rollback immediately, using the
    // negative interval, then applies its 0.4/0.6 float EMA.
    global.relative_alt = 110000;
    QVERIFY(state.Apply(makeMessage(global), at(100)));
    near(state.snapshot().verticalSpeed, 84.0, 1.0e-4);
}

void MicrodroneTelemetryStateTest::gpsStatusCourseAndVfrSources()
{
    MicrodroneTelemetryState state;
    mavlink_gps_raw_int_t gps{};
    gps.lat = 10000000;
    gps.lon = 20000000;
    gps.eph = 123;
    gps.satellites_visible = 12;
    gps.vel = 40;
    gps.cog = 1234;
    QVERIFY(state.Apply(makeMessage(gps), at(0)));
    QCOMPARE(state.snapshot().gpsHdop,
             static_cast<double>(static_cast<float>(1.23)));
    QCOMPARE(state.snapshot().satelliteCount, 12.0);
    QCOMPARE(state.snapshot().groundSpeed,
             static_cast<double>(40 * 1.0e-2f));
    QCOMPARE(state.snapshot().groundCourse, 0.0);

    gps.eph = std::numeric_limits<quint16>::max();
    gps.satellites_visible = std::numeric_limits<quint8>::max();
    gps.vel = std::numeric_limits<quint16>::max();
    gps.cog = 2500;
    QVERIFY(state.Apply(makeMessage(gps), at(10)));
    QCOMPARE(state.snapshot().gpsHdop,
             static_cast<double>(static_cast<float>(1.23)));
    QCOMPARE(state.snapshot().satelliteCount, 12.0);
    QCOMPARE(state.snapshot().groundCourse, 0.0);

    mavlink_vfr_hud_t hud{};
    hud.groundspeed = 3.25f;
    QVERIFY(state.Apply(makeMessage(hud), at(20)));
    QCOMPARE(state.snapshot().groundSpeed, static_cast<double>(3.25f));

    QVERIFY(state.Apply(makeMessage(gps), at(30)));
    QCOMPARE(state.snapshot().groundCourse,
             static_cast<double>(static_cast<float>(25.0f)));

    mavlink_gps_status_t status{};
    status.satellites_visible = 255;
    QVERIFY(state.Apply(makeMessage(status), at(40)));
    QCOMPARE(state.snapshot().satelliteCount, 255.0);
}

void MicrodroneTelemetryStateTest::attitudeAndHighLatencyVariants()
{
    MicrodroneTelemetryState state;
    mavlink_attitude_t attitude{};
    attitude.roll = 0.1f;
    attitude.pitch = -0.2f;
    attitude.yaw = static_cast<float>(-3.14159265358979323846 / 2.0);
    QVERIFY(state.Apply(makeMessage(attitude), at(0)));
    QCOMPARE(state.snapshot().roll, static_cast<double>(
        static_cast<float>(attitude.roll * (180.0 / 3.14159265358979323846))));
    QCOMPARE(state.snapshot().pitch, static_cast<double>(
        static_cast<float>(attitude.pitch * (180.0 / 3.14159265358979323846))));
    QCOMPARE(state.snapshot().yaw, static_cast<double>(
        static_cast<float>(attitude.yaw * (180.0 / 3.14159265358979323846))
        + 360.0f));

    mavlink_home_position_t home{};
    home.altitude = 123456;
    QVERIFY(state.Apply(makeMessage(home), at(10)));

    mavlink_high_latency_t high{};
    high.latitude = -353629380;
    high.longitude = 1491650850;
    high.altitude_amsl = 584;
    high.roll = 123;
    high.pitch = -456;
    high.heading = 35000;
    high.groundspeed = 9;
    high.gps_nsat = 255;
    high.temperature = -5;
    QVERIFY(state.Apply(makeMessage(high), at(1000)));
    near(state.snapshot().latitude, -35.362938, 1.0e-12);
    near(state.snapshot().longitude, 149.165085, 1.0e-12);
    QCOMPARE(state.snapshot().altitude, static_cast<double>(
        584.0f - static_cast<float>(123456 / 1000.0)));
    QCOMPARE(state.snapshot().roll,
             static_cast<double>(static_cast<float>(1.23f)));
    QCOMPARE(state.snapshot().pitch,
             static_cast<double>(static_cast<float>(-4.56f)));
    QCOMPARE(state.snapshot().yaw, 350.0);
    QCOMPARE(state.snapshot().groundSpeed, 9.0);
    QCOMPARE(state.snapshot().satelliteCount, 255.0);
    QCOMPARE(state.snapshot().pressureTemperature, -5.0);

    mavlink_high_latency2_t high2{};
    high2.latitude = 515000000;
    high2.longitude = -11400000;
    high2.altitude = 300;
    high2.heading = 200;
    high2.groundspeed = 51;
    high2.eph = 17;
    QVERIFY(state.Apply(makeMessage(high2), at(2000)));
    near(state.snapshot().latitude, 51.5, 1.0e-12);
    near(state.snapshot().longitude, -1.14, 1.0e-12);
    QCOMPARE(state.snapshot().altitude, static_cast<double>(
        300.0f - static_cast<float>(123456 / 1000.0)));
    QCOMPARE(state.snapshot().yaw, 400.0); // CurrentState does not wrap >360.
    QCOMPARE(state.snapshot().groundSpeed,
             static_cast<double>(static_cast<float>(51 / 5.0f)));
    QCOMPARE(state.snapshot().gpsHdop, 17.0);
    QCOMPARE(state.snapshot().satelliteCount, 255.0);
    QCOMPARE(state.snapshot().pressureTemperature, -5.0);
}

void MicrodroneTelemetryStateTest::primaryMagnetometerAndTemperatureSources()
{
    MicrodroneTelemetryState state;
    mavlink_raw_imu_t raw{};
    raw.xmag = -139;
    raw.ymag = 12;
    raw.zmag = 431;
    QVERIFY(state.Apply(makeMessage(raw), at(0)));
    QCOMPARE(state.snapshot().magnetometerX, -139.0);
    QCOMPARE(state.snapshot().magnetometerY, 12.0);
    QCOMPARE(state.snapshot().magnetometerZ, 431.0);

    mavlink_scaled_imu_t scaled{};
    scaled.xmag = 4;
    scaled.ymag = 5;
    scaled.zmag = 6;
    QVERIFY(state.Apply(makeMessage(scaled), at(10)));
    QCOMPARE(state.snapshot().magnetometerX, 4.0);
    QCOMPARE(state.snapshot().magnetometerY, 5.0);
    QCOMPARE(state.snapshot().magnetometerZ, 6.0);

    mavlink_scaled_pressure_t pressure{};
    pressure.temperature = 2450;
    QVERIFY(state.Apply(makeMessage(pressure), at(20)));
    QCOMPARE(state.snapshot().pressureTemperature, 2450.0);

    mavlink_highres_imu_t high{};
    high.id = 1;
    high.fields_updated = 0x40 | 0x1000;
    high.xmag = 100.5f;
    high.ymag = 101.5f;
    high.zmag = 102.5f;
    high.temperature = 23.9f;
    QVERIFY(state.Apply(makeMessage(high), at(30)));
    QCOMPARE(state.snapshot().magnetometerX, 4.0); // id 1 is secondary.
    QCOMPARE(state.snapshot().pressureTemperature, 23.0);

    high.id = 0;
    high.fields_updated = 0x40;
    high.xmag = 0.1f;
    high.ymag = -0.2f;
    high.zmag = 0.3f;
    high.temperature = 99.9f;
    QVERIFY(state.Apply(makeMessage(high), at(40)));
    QCOMPARE(state.snapshot().magnetometerX, static_cast<double>(0.1f));
    QCOMPARE(state.snapshot().magnetometerY, static_cast<double>(-0.2f));
    QCOMPARE(state.snapshot().magnetometerZ, static_cast<double>(0.3f));
    QCOMPARE(state.snapshot().pressureTemperature, 23.0);

    high.id = 2;
    high.fields_updated = 0x1000;
    high.temperature = -8.9f;
    QVERIFY(state.Apply(makeMessage(high), at(50)));
    QCOMPARE(state.snapshot().pressureTemperature, -8.0);

    high.id = 0;
    high.temperature = std::numeric_limits<float>::quiet_NaN();
    QVERIFY(state.Apply(makeMessage(high), at(60)));
    QCOMPARE(state.snapshot().pressureTemperature, 0.0);

    high.temperature = std::numeric_limits<float>::infinity();
    QVERIFY(state.Apply(makeMessage(high), at(70)));
    QCOMPARE(state.snapshot().pressureTemperature,
             static_cast<double>(std::numeric_limits<int>::max()));

    high.temperature = -std::numeric_limits<float>::infinity();
    QVERIFY(state.Apply(makeMessage(high), at(80)));
    QCOMPARE(state.snapshot().pressureTemperature,
             static_cast<double>(std::numeric_limits<int>::min()));

    high.temperature = 3.0e20f;
    QVERIFY(state.Apply(makeMessage(high), at(90)));
    QCOMPARE(state.snapshot().pressureTemperature,
             static_cast<double>(std::numeric_limits<int>::max()));

    high.temperature = -3.0e20f;
    QVERIFY(state.Apply(makeMessage(high), at(100)));
    QCOMPARE(state.snapshot().pressureTemperature,
             static_cast<double>(std::numeric_limits<int>::min()));
}

void MicrodroneTelemetryStateTest::clearResetsAllState()
{
    MicrodroneTelemetryState state;
    mavlink_global_position_int_t global{};
    global.lat = 10000000;
    global.lon = 20000000;
    global.relative_alt = 3000;
    QVERIFY(state.Apply(makeMessage(global), at(0)));

    state.clear();
    const MicrodroneTelemetry cleared = state.snapshot();
    QCOMPARE(cleared.latitude, 0.0);
    QCOMPARE(cleared.longitude, 0.0);
    QCOMPARE(cleared.altitude, 0.0);
    QCOMPARE(cleared.verticalSpeed, 0.0);

    // clear also drops the HOME_POSITION offset used only by high-latency
    // absolute-altitude reports.
    mavlink_high_latency_t high{};
    high.altitude_amsl = 10;
    QVERIFY(state.Apply(makeMessage(high), at(5)));
    QCOMPARE(state.snapshot().altitude, 10.0);

    mavlink_gps_raw_int_t gps{};
    gps.lat = 30000000;
    gps.lon = 40000000;
    gps.eph = std::numeric_limits<quint16>::max();
    gps.vel = std::numeric_limits<quint16>::max();
    gps.cog = std::numeric_limits<quint16>::max();
    gps.satellites_visible = std::numeric_limits<quint8>::max();
    QVERIFY(state.Apply(makeMessage(gps), at(10)));
    QCOMPARE(state.snapshot().latitude, 3.0);
    QCOMPARE(state.snapshot().longitude, 4.0);

    mavlink_heartbeat_t heartbeat{};
    QVERIFY(!state.Apply(makeMessage(heartbeat), at(20)));
}

QTEST_GUILESS_MAIN(MicrodroneTelemetryStateTest)
#include "test_microdronetelemetrystate.moc"
