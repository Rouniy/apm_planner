#include <QtTest>

#include "comm/NmeaSentenceBuilder.h"

#include <QDateTime>

#include <climits>
#include <cmath>

namespace {

// Independent reference checksum for the golden lines (NMEA-0183: XOR between '$' and '*').
QString referenceChecksum(const QString &body)
{
    int checksum = 0;
    for (int i = 1; i < body.size() && body.at(i) != QLatin1Char('*'); ++i) {
        checksum ^= body.at(i).toLatin1();
    }
    return QString::number(checksum, 16).rightJustified(2, QLatin1Char('0')).toUpper();
}

QDateTime instant()
{
    return QDateTime(QDate(2026, 9, 3), QTime(12, 35, 19, 123), Qt::UTC);
}

NmeaVehicleState munich()
{
    NmeaVehicleState state;
    state.globalPositionSeen = true;
    state.hasPosition = true;
    state.latitude = 48.1173;
    state.longitude = 11.5166667;
    state.altitudeAmsl = 545.4;
    state.fixType = 3;
    state.satellites = 8;
    state.hdop = 0.9;
    state.groundSpeedMs = 5.0;
    state.groundCourseDeg = 84.4;
    state.yawDeg = 54.7;
    return state;
}

mavlink_message_t globalPosition(qint32 lat, qint32 lon, qint32 altMm)
{
    mavlink_message_t message{};
    mavlink_msg_global_position_int_pack(1, 1, &message, 0, lat, lon, altMm, altMm, 0, 0, 0, 0);
    return message;
}

mavlink_message_t gpsRaw(quint8 fixType, qint32 lat, qint32 lon, qint32 altMm, quint16 eph,
                         quint16 vel, quint16 cog, quint8 satellites)
{
    mavlink_message_t message{};
    mavlink_msg_gps_raw_int_pack(1, 1, &message, 0, fixType, lat, lon, altMm, eph, 65535, vel, cog,
                                 satellites, 0, 0, 0, 0, 0, 0);
    return message;
}

mavlink_message_t vfrHud(float groundSpeed)
{
    mavlink_message_t message{};
    mavlink_msg_vfr_hud_pack(1, 1, &message, 0.0f, groundSpeed, 0, 0, 0.0f, 0.0f);
    return message;
}

mavlink_message_t attitude(float yawRad)
{
    mavlink_message_t message{};
    mavlink_msg_attitude_pack(1, 1, &message, 0, 0.0f, 0.0f, yawRad, 0.0f, 0.0f, 0.0f);
    return message;
}

} // namespace

class NmeaSentenceBuilderTest final : public QObject
{
    Q_OBJECT

private slots:
    void checksumMatchesMp10Vectors();
    void formattingBlocksMatchCSharpFormats();
    void goldenTickForNorthernEasternFix();
    void southernWesternHemispheresAndUnpaddedRmc();
    void noVehicleAndNoFixSemantics();
    void lineAndTickBytesAppendChecksumAndCrLf();
    void applyIgnoresOtherMessagesAndFollowsPrecedence();
    void applyHonoursSentinelsCourseGateAndNormalisation();
};

void NmeaSentenceBuilderTest::checksumMatchesMp10Vectors()
{
    // MissionPlannerTests NmeaChecksumTests reference vectors.
    QCOMPARE(NmeaSentenceBuilder::Checksum(
                 QStringLiteral("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,")),
             QStringLiteral("47"));
    QCOMPARE(NmeaSentenceBuilder::Checksum(
                 QStringLiteral("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W")),
             QStringLiteral("6A"));
    // '$' is skipped, the result is two uppercase hex digits, and '*' terminates.
    const QString withDollar = NmeaSentenceBuilder::Checksum(
        QStringLiteral("$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K"));
    QCOMPARE(withDollar, NmeaSentenceBuilder::Checksum(
                             QStringLiteral("GPVTG,054.7,T,034.4,M,005.5,N,010.2,K")));
    QCOMPARE(withDollar.size(), 2);
    QCOMPARE(withDollar, withDollar.toUpper());
    QCOMPARE(NmeaSentenceBuilder::Checksum(QStringLiteral("$GPGGA,123519,4807.038,N")),
             NmeaSentenceBuilder::Checksum(QStringLiteral("$GPGGA,123519,4807.038,N*FF")));
    QCOMPARE(NmeaSentenceBuilder::Checksum(QStringLiteral("$")), QStringLiteral("00"));
}

void NmeaSentenceBuilderTest::formattingBlocksMatchCSharpFormats()
{
    // (int)deg + frac * 0.6, times 100, absolute.
    QVERIFY(std::fabs(NmeaSentenceBuilder::DegreesMinutes(48.1173) - 4807.038) < 1e-6);
    QVERIFY(std::fabs(NmeaSentenceBuilder::DegreesMinutes(-33.8688) - 3352.128) < 1e-6);
    QVERIFY(std::fabs(NmeaSentenceBuilder::DegreesMinutes(11.5166667) - 1131.000002) < 1e-6);
    QCOMPARE(NmeaSentenceBuilder::DegreesMinutes(0.0), 0.0);

    QCOMPARE(NmeaSentenceBuilder::FormatFixed(4807.038, 4, 5), QStringLiteral("4807.03800"));   // "0000.00000"
    QCOMPARE(NmeaSentenceBuilder::FormatFixed(1131.000002, 5, 5), QStringLiteral("01131.00000")); // "00000.00000"
    QCOMPARE(NmeaSentenceBuilder::FormatFixed(4807.038, 4, 2), QStringLiteral("4807.04"));       // "0000.00"
    QCOMPARE(NmeaSentenceBuilder::FormatFixed(930.0, 1, 5), QStringLiteral("930.00000"));         // "0.00000" (no padding)
    QCOMPARE(NmeaSentenceBuilder::FormatFixed(84.4, 3, 0), QStringLiteral("084"));                // "000"
    QCOMPARE(NmeaSentenceBuilder::FormatFixed(54.7, 3, 0), QStringLiteral("055"));
    QCOMPARE(NmeaSentenceBuilder::FormatFixed(9.71922, 2, 1), QStringLiteral("09.7"));            // "00.0"
    QCOMPARE(NmeaSentenceBuilder::FormatFixed(0.0, 2, 1), QStringLiteral("00.0"));
    QCOMPARE(NmeaSentenceBuilder::FormatFixed(0.0, 1, 1), QStringLiteral("0.0"));                 // "0.0"
    QCOMPARE(NmeaSentenceBuilder::FormatFixed(359.96, 1, 1), QStringLiteral("360.0"));

    // C# float.ToString(): shortest round-trip of the single-precision value.
    QCOMPARE(NmeaSentenceBuilder::FormatFloat(0.9), QStringLiteral("0.9"));
    QCOMPARE(NmeaSentenceBuilder::FormatFloat(1.23), QStringLiteral("1.23"));
    QCOMPARE(NmeaSentenceBuilder::FormatFloat(545.4), QStringLiteral("545.4"));
    QCOMPARE(NmeaSentenceBuilder::FormatFloat(0.0), QStringLiteral("0"));
    QCOMPARE(NmeaSentenceBuilder::FormatFloat(100.123), QStringLiteral("100.123"));
    QCOMPARE(NmeaSentenceBuilder::FormatFloat(12.0), QStringLiteral("12"));

    QCOMPARE(NmeaSentenceBuilder::FormatTime(instant()), QStringLiteral("123519.123"));
    QCOMPARE(NmeaSentenceBuilder::FormatDate(instant()), QStringLiteral("030926"));
    // Any zone in, UTC out.
    const QDateTime offset(QDate(2026, 9, 3), QTime(23, 30, 0), Qt::OffsetFromUTC, -3 * 3600);
    QCOMPARE(NmeaSentenceBuilder::FormatTime(offset), QStringLiteral("023000.000"));
    QCOMPARE(NmeaSentenceBuilder::FormatDate(offset), QStringLiteral("040926"));   // UTC date, not local

    QCOMPARE(NmeaSentenceBuilder::NormaliseDegrees(-90.0), 270.0);
    QCOMPARE(NmeaSentenceBuilder::NormaliseDegrees(45.0), 45.0);
    QCOMPARE(NmeaSentenceBuilder::NormaliseDegrees(0.0), 0.0);
}

void NmeaSentenceBuilderTest::goldenTickForNorthernEasternFix()
{
    const NmeaVehicleState state = munich();
    const QStringList tick = NmeaSentenceBuilder::Tick(state, instant());
    QCOMPARE(tick.size(), 5);
    QCOMPARE(tick.at(0), QStringLiteral("$GPGGA,123519.123,4807.03800,N,01131.00000,E,1,8,0.9,545.4,M,0.0,M,,"));
    QCOMPARE(tick.at(1), QStringLiteral("$GPGLL,4807.04,N,01131.00,E,123519.123,A,A"));
    QCOMPARE(tick.at(2), QStringLiteral("$GPHDG,54.7,0,E,0,E"));
    QCOMPARE(tick.at(3), QStringLiteral("$GPVTG,084,055,09.7,18.0"));   // no T/M/N/K letters, like MP10
    QCOMPARE(tick.at(4), QStringLiteral("$GPRMC,123519.123,A,4807.03800,N,1131.00000,E,9.7,84.4,030926,0,E,A"));
    QCOMPARE(NmeaSentenceBuilder::Gga(state, instant()), tick.at(0));
    QCOMPARE(NmeaSentenceBuilder::Gll(state, instant()), tick.at(1));
    QCOMPARE(NmeaSentenceBuilder::Hdg(state), tick.at(2));
    QCOMPARE(NmeaSentenceBuilder::Vtg(state), tick.at(3));
    QCOMPARE(NmeaSentenceBuilder::Rmc(state, instant()), tick.at(4));
}

void NmeaSentenceBuilderTest::southernWesternHemispheresAndUnpaddedRmc()
{
    NmeaVehicleState state = munich();
    state.latitude = -33.8688;     // Sydney
    state.longitude = -151.2093;   // (west for the test)
    state.fixType = 6;
    state.satellites = 14;
    state.hdop = 1.23;
    state.altitudeAmsl = 12.0;
    const QStringList tick = NmeaSentenceBuilder::Tick(state, instant());
    QCOMPARE(tick.at(0), QStringLiteral("$GPGGA,123519.123,3352.12800,S,15112.55800,W,1,14,1.23,12,M,0.0,M,,"));
    QCOMPARE(tick.at(1), QStringLiteral("$GPGLL,3352.13,S,15112.56,W,123519.123,A,A"));
    QCOMPARE(tick.at(4), QStringLiteral("$GPRMC,123519.123,A,3352.12800,S,15112.55800,W,9.7,84.4,030926,0,E,A"));

    // Low latitude/longitude: GGA/GLL pad, RMC does not (MP10 "0.00000").
    state.latitude = 9.5;
    state.longitude = 3.25;
    const QStringList low = NmeaSentenceBuilder::Tick(state, instant());
    QVERIFY2(low.at(0).contains(QStringLiteral(",0930.00000,N,00315.00000,E,")), qPrintable(low.at(0)));
    QVERIFY2(low.at(1).startsWith(QStringLiteral("$GPGLL,0930.00,N,00315.00,E,")), qPrintable(low.at(1)));
    QVERIFY2(low.at(4).contains(QStringLiteral(",A,930.00000,N,315.00000,E,")), qPrintable(low.at(4)));
}

void NmeaSentenceBuilderTest::noVehicleAndNoFixSemantics()
{
    // Default state = MP10 with no vehicle: zeros are emitted, quality 0.
    const NmeaVehicleState none;
    const QStringList tick = NmeaSentenceBuilder::Tick(none, instant());
    QCOMPARE(tick.at(0), QStringLiteral("$GPGGA,123519.123,0000.00000,N,00000.00000,E,0,0,0,0,M,0.0,M,,"));
    QCOMPARE(tick.at(1), QStringLiteral("$GPGLL,0000.00,N,00000.00,E,123519.123,A,A"));
    QCOMPARE(tick.at(2), QStringLiteral("$GPHDG,0.0,0,E,0,E"));
    QCOMPARE(tick.at(3), QStringLiteral("$GPVTG,000,000,00.0,00.0"));
    QCOMPARE(tick.at(4), QStringLiteral("$GPRMC,123519.123,A,0.00000,N,0.00000,E,0.0,0.0,030926,0,E,A"));

    // GGA quality is 1 only for fix_type >= 3; RMC/GLL status stays "A" like MP10.
    NmeaVehicleState state = munich();
    for (int fix : {0, 1, 2}) {
        state.fixType = fix;
        QVERIFY2(NmeaSentenceBuilder::Gga(state, instant()).contains(QStringLiteral(",E,0,8,")),
                 qPrintable(NmeaSentenceBuilder::Gga(state, instant())));
        QVERIFY(NmeaSentenceBuilder::Rmc(state, instant()).startsWith(QStringLiteral("$GPRMC,123519.123,A,")));
    }
    for (int fix : {3, 4, 5, 6}) {
        state.fixType = fix;
        QVERIFY(NmeaSentenceBuilder::Gga(state, instant()).contains(QStringLiteral(",E,1,8,")));
    }
}

void NmeaSentenceBuilderTest::lineAndTickBytesAppendChecksumAndCrLf()
{
    const QString body = QStringLiteral("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    QCOMPARE(NmeaSentenceBuilder::Line(body),
             QByteArray("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"));

    const QByteArray bytes = NmeaSentenceBuilder::TickBytes(munich(), instant());
    const QList<QByteArray> lines = bytes.split('\n');
    QCOMPARE(lines.size(), 6);   // five terminated lines + empty tail
    QVERIFY(lines.last().isEmpty());
    const QStringList tick = NmeaSentenceBuilder::Tick(munich(), instant());
    for (int i = 0; i < 5; ++i) {
        const QByteArray expected = tick.at(i).toLatin1() + '*' + referenceChecksum(tick.at(i)).toLatin1() + '\r';
        QCOMPARE(lines.at(i), expected);
        QVERIFY(lines.at(i).startsWith("$GP"));
        QCOMPARE(lines.at(i).indexOf('*'), lines.at(i).size() - 4);   // "*XX\r"
    }
}

void NmeaSentenceBuilderTest::applyIgnoresOtherMessagesAndFollowsPrecedence()
{
    NmeaVehicleState state;
    mavlink_message_t heartbeat{};
    mavlink_msg_heartbeat_pack(1, 1, &heartbeat, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, 0);
    QVERIFY(!NmeaSentenceBuilder::Apply(state, heartbeat));
    QVERIFY(!state.hasPosition);
    QCOMPARE(state.latitude, 0.0);

    // GPS_RAW_INT supplies position/altitude while no GLOBAL_POSITION_INT has been seen.
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(3, 481173000, 115166667, 545400, 90, 500, 8440, 8)));
    QVERIFY(state.hasPosition);
    QVERIFY(!state.globalPositionSeen);
    QVERIFY(std::fabs(state.latitude - 48.1173) < 1e-9);
    QVERIFY(std::fabs(state.longitude - 11.5166667) < 1e-9);
    QVERIFY(std::fabs(state.altitudeAmsl - 545.4) < 1e-3);
    QCOMPARE(state.fixType, 3);
    QCOMPARE(state.satellites, 8);
    QCOMPARE(state.hdop, 0.9);
    QVERIFY(std::fabs(state.groundSpeedMs - 5.0) < 1e-6);
    QVERIFY(std::fabs(state.groundCourseDeg - 84.4) < 1e-4);

    // GLOBAL_POSITION_INT takes over position/altitude ...
    QVERIFY(NmeaSentenceBuilder::Apply(state, globalPosition(-338688000, 1512093000, 12000)));
    QVERIFY(state.globalPositionSeen);
    QVERIFY(std::fabs(state.latitude - -33.8688) < 1e-9);
    QVERIFY(std::fabs(state.longitude - 151.2093) < 1e-9);
    QVERIFY(std::fabs(state.altitudeAmsl - 12.0) < 1e-6);
    // ... and GPS_RAW_INT no longer moves them but still updates fix/hdop/sats/speed/course.
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(4, 100000000, 200000000, 999000, 123, 1000, 27000, 14)));
    QVERIFY(std::fabs(state.latitude - -33.8688) < 1e-9);
    QVERIFY(std::fabs(state.altitudeAmsl - 12.0) < 1e-6);
    QCOMPARE(state.fixType, 4);
    QCOMPARE(state.satellites, 14);
    QCOMPARE(state.hdop, 1.23);
    QVERIFY(std::fabs(state.groundSpeedMs - 10.0) < 1e-6);
    QVERIFY(std::fabs(state.groundCourseDeg - 270.0) < 1e-4);

    // A dead-reckoning 0/0 or int.MaxValue GLOBAL_POSITION_INT clears useLocation and keeps the
    // old values, so GPS_RAW_INT may supply the position again.
    QVERIFY(NmeaSentenceBuilder::Apply(state, globalPosition(0, 1512093000, 5000)));
    QVERIFY(!state.globalPositionSeen);
    QVERIFY(std::fabs(state.latitude - -33.8688) < 1e-9);
    QVERIFY(std::fabs(state.altitudeAmsl - 12.0) < 1e-6);
    QVERIFY(NmeaSentenceBuilder::Apply(state, globalPosition(INT_MAX, 1512093000, 5000)));
    QVERIFY(!state.globalPositionSeen);
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(3, 100000000, 200000000, 77000, 100, 600, 9000, 9)));
    QVERIFY(std::fabs(state.latitude - 10.0) < 1e-9);
    QVERIFY(std::fabs(state.longitude - 20.0) < 1e-9);
    QVERIFY(std::fabs(state.altitudeAmsl - 77.0) < 1e-6);

    // VFR_HUD groundspeed overrides the GPS value; ATTITUDE supplies yaw in degrees.
    QVERIFY(NmeaSentenceBuilder::Apply(state, vfrHud(2.5f)));
    QVERIFY(std::fabs(state.groundSpeedMs - 2.5) < 1e-6);
    QVERIFY(NmeaSentenceBuilder::Apply(state, attitude(static_cast<float>(M_PI / 2.0))));
    QVERIFY(std::fabs(state.yawDeg - 90.0) < 1e-3);
}

void NmeaSentenceBuilderTest::applyHonoursSentinelsCourseGateAndNormalisation()
{
    NmeaVehicleState state;
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(3, 481173000, 115166667, 545400, 90, 500, 8440, 8)));

    // Sentinels: lat/lon int.MaxValue, eph/vel/cog 65535, satellites 255 leave the fields alone.
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(2, INT_MAX, INT_MAX, 100000, 65535, 65535, 65535, 255)));
    QVERIFY(std::fabs(state.latitude - 48.1173) < 1e-9);
    QVERIFY(std::fabs(state.longitude - 11.5166667) < 1e-9);
    QVERIFY(std::fabs(state.altitudeAmsl - 100.0) < 1e-6);   // altitude has no sentinel in CurrentState
    QCOMPARE(state.fixType, 2);                              // fix_type always applied
    QCOMPARE(state.satellites, 8);
    QCOMPARE(state.hdop, 0.9);
    QVERIFY(std::fabs(state.groundSpeedMs - 5.0) < 1e-6);
    QVERIFY(std::fabs(state.groundCourseDeg - 84.4) < 1e-4);

    // Course gate: cog is ignored while the (just updated) ground speed is <= 0.5 m/s.
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(3, 481173000, 115166667, 545400, 90, 50, 18000, 8)));
    QVERIFY(std::fabs(state.groundSpeedMs - 0.5) < 1e-6);
    QVERIFY(std::fabs(state.groundCourseDeg - 84.4) < 1e-4);
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(3, 481173000, 115166667, 545400, 90, 51, 18000, 8)));
    QVERIFY(std::fabs(state.groundCourseDeg - 180.0) < 1e-4);
    // A valid speed with cog sentinel keeps the previous course.
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(3, 481173000, 115166667, 545400, 90, 900, 65535, 8)));
    QVERIFY(std::fabs(state.groundCourseDeg - 180.0) < 1e-4);

    // HDOP rounding to two decimals; satellites as reported.
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(3, 481173000, 115166667, 545400, 9, 900, 65535, 31)));
    QCOMPARE(state.hdop, 0.09);
    QCOMPARE(state.satellites, 31);
    QVERIFY(NmeaSentenceBuilder::Apply(state, gpsRaw(3, 481173000, 115166667, 545400, 12345, 900, 65535, 31)));
    QCOMPARE(state.hdop, 123.45);

    // Yaw normalisation (CurrentState setter adds 360 to negatives).
    QVERIFY(NmeaSentenceBuilder::Apply(state, attitude(static_cast<float>(-M_PI / 2.0))));
    QVERIFY(std::fabs(state.yawDeg - 270.0) < 1e-3);
    QVERIFY(NmeaSentenceBuilder::Apply(state, attitude(0.0f)));
    QCOMPARE(state.yawDeg, 0.0);
    QVERIFY(NmeaSentenceBuilder::Apply(state, attitude(static_cast<float>(M_PI))));
    QVERIFY(std::fabs(state.yawDeg - 180.0) < 1e-3);
    // The VTG "000" field rounds the normalised yaw.
    state.yawDeg = 359.6;
    QVERIFY(NmeaSentenceBuilder::Vtg(state).startsWith(QStringLiteral("$GPVTG,180,360,")));
}

QTEST_GUILESS_MAIN(NmeaSentenceBuilderTest)
#include "test_nmeasentencebuilder.moc"
