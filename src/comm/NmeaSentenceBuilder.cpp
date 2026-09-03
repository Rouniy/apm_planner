#include "NmeaSentenceBuilder.h"

#include <QtGlobal>

#include <climits>
#include <cmath>

namespace {

constexpr double kRadToDeg = 180.0 / M_PI;

QString hemisphereLatitude(double latitude)
{
    return latitude < 0.0 ? QStringLiteral("S") : QStringLiteral("N");
}

QString hemisphereLongitude(double longitude)
{
    return longitude < 0.0 ? QStringLiteral("W") : QStringLiteral("E");
}

} // namespace

double NmeaSentenceBuilder::NormaliseDegrees(double degrees)
{
    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

bool NmeaSentenceBuilder::Apply(NmeaVehicleState &state, const mavlink_message_t &message)
{
    switch (message.msgid) {
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t loc;
        mavlink_msg_global_position_int_decode(&message, &loc);
        // CurrentState: useLocation = true unless the dead-reckoning AHRS sends 0/0 or
        // the int.MaxValue sentinel; only a usable fix replaces lat/lng/altasl.
        const bool usable = loc.lat != 0 && loc.lon != 0 && loc.lat != INT_MAX
            && loc.lon != INT_MAX;
        state.globalPositionSeen = usable;
        if (usable) {
            state.latitude = loc.lat / 10000000.0;
            state.longitude = loc.lon / 10000000.0;
            state.altitudeAmsl = static_cast<double>(loc.alt / 1000.0f);
            state.hasPosition = true;
        }
        return true;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        mavlink_gps_raw_int_t gps;
        mavlink_msg_gps_raw_int_decode(&message, &gps);
        if (!state.globalPositionSeen) {
            if (gps.lat != INT_MAX) {
                state.latitude = gps.lat * 1.0e-7;
                state.hasPosition = true;
            }
            if (gps.lon != INT_MAX) {
                state.longitude = gps.lon * 1.0e-7;
                state.hasPosition = true;
            }
            state.altitudeAmsl = static_cast<double>(gps.alt / 1000.0f);
        }
        state.fixType = gps.fix_type;
        if (gps.eph != USHRT_MAX) {
            state.hdop = std::round(gps.eph / 100.0 * 100.0) / 100.0;   // Math.Round(eph/100, 2)
        }
        if (gps.satellites_visible != UCHAR_MAX) {
            state.satellites = gps.satellites_visible;
        }
        if (gps.vel != USHRT_MAX) {
            state.groundSpeedMs = static_cast<double>(gps.vel * 1.0e-2f);
        }
        // The gate reads the speed just assigned above, exactly like CurrentState.
        if (state.groundSpeedMs > 0.5 && gps.cog != USHRT_MAX) {
            state.groundCourseDeg = NormaliseDegrees(static_cast<double>(gps.cog * 1.0e-2f));
        }
        return true;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
        mavlink_vfr_hud_t hud;
        mavlink_msg_vfr_hud_decode(&message, &hud);
        state.groundSpeedMs = hud.groundspeed;   // overrides the GPS value (CurrentState:3872)
        return true;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
        mavlink_attitude_t attitude;
        mavlink_msg_attitude_decode(&message, &attitude);
        state.yawDeg = NormaliseDegrees(static_cast<double>(
            static_cast<float>(attitude.yaw * kRadToDeg)));
        return true;
    }
    default:
        return false;
    }
}

QString NmeaSentenceBuilder::Checksum(const QString &body)
{
    int checksum = 0;
    for (const QChar character : body) {
        if (character == QLatin1Char('$')) {
            continue;
        }
        if (character == QLatin1Char('*')) {
            break;
        }
        checksum ^= static_cast<int>(character.toLatin1()) & 0xFF;
    }
    return QStringLiteral("%1").arg(checksum, 2, 16, QLatin1Char('0')).toUpper();
}

double NmeaSentenceBuilder::DegreesMinutes(double degrees)
{
    const double whole = std::trunc(degrees);
    const double converted = whole + (degrees - whole) * 0.6;
    return std::fabs(converted * 100.0);
}

QString NmeaSentenceBuilder::FormatFixed(double value, int integerDigits, int decimals)
{
    QString text = QString::number(value, 'f', decimals);
    const bool negative = text.startsWith(QLatin1Char('-'));
    if (negative) {
        text.remove(0, 1);
    }
    const int point = text.indexOf(QLatin1Char('.'));
    const int integerLength = point < 0 ? text.size() : point;
    if (integerLength < integerDigits) {
        text.prepend(QString(integerDigits - integerLength, QLatin1Char('0')));
    }
    bool nonZero = false;
    for (const QChar character : text) {
        if (character >= QLatin1Char('1') && character <= QLatin1Char('9')) {
            nonZero = true;
            break;
        }
    }
    if (negative && nonZero) {
        text.prepend(QLatin1Char('-'));   // C# prints no sign for a value that rounds to zero
    }
    return text;
}

QString NmeaSentenceBuilder::FormatFloat(double value)
{
    // C# float.ToString() prints the shortest representation that round-trips a float:
    // try increasing precision until the single-precision value is reproduced.
    const float single = static_cast<float>(value);
    for (int precision = 1; precision <= 9; ++precision) {
        const QString text = QString::number(single, 'g', precision);
        if (static_cast<float>(text.toDouble()) == single) {
            return text;
        }
    }
    return QString::number(single, 'g', 9);
}

QString NmeaSentenceBuilder::FormatTime(const QDateTime &utcNow)
{
    return utcNow.toUTC().toString(QStringLiteral("HHmmss.zzz"));
}

QString NmeaSentenceBuilder::FormatDate(const QDateTime &utcNow)
{
    return utcNow.toUTC().toString(QStringLiteral("ddMMyy"));   // UTC by root decision
}

QString NmeaSentenceBuilder::Gga(const NmeaVehicleState &state, const QDateTime &utcNow)
{
    // $GP{0},{1:HHmmss.fff},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},
    return QStringLiteral("$GPGGA,%1,%2,%3,%4,%5,%6,%7,%8,%9,M,0.0,M,,")
        .arg(FormatTime(utcNow),
             FormatFixed(DegreesMinutes(state.latitude), 4, 5), hemisphereLatitude(state.latitude),
             FormatFixed(DegreesMinutes(state.longitude), 5, 5), hemisphereLongitude(state.longitude),
             QString::number(state.fixType >= 3 ? 1 : 0), QString::number(state.satellites),
             FormatFloat(state.hdop), FormatFloat(state.altitudeAmsl));
}

QString NmeaSentenceBuilder::Gll(const NmeaVehicleState &state, const QDateTime &utcNow)
{
    // $GP{0},{1},{2},{3},{4},{5:HHmmss.fff},{6},{7}
    return QStringLiteral("$GPGLL,%1,%2,%3,%4,%5,A,A")
        .arg(FormatFixed(DegreesMinutes(state.latitude), 4, 2), hemisphereLatitude(state.latitude),
             FormatFixed(DegreesMinutes(state.longitude), 5, 2), hemisphereLongitude(state.longitude),
             FormatTime(utcNow));
}

QString NmeaSentenceBuilder::Hdg(const NmeaVehicleState &state)
{
    // $GP{0},{1:0.0},{2},{3},{4},{5}
    return QStringLiteral("$GPHDG,%1,0,E,0,E").arg(FormatFixed(state.yawDeg, 1, 1));
}

QString NmeaSentenceBuilder::Vtg(const NmeaVehicleState &state)
{
    // $GP{0},{1},{2},{3},{4} - MP10 emits no T/M/N/K unit letters.
    return QStringLiteral("$GPVTG,%1,%2,%3,%4")
        .arg(FormatFixed(state.groundCourseDeg, 3, 0), FormatFixed(state.yawDeg, 3, 0),
             FormatFixed(state.groundSpeedMs * KnotsPerMetrePerSecond, 2, 1),
             FormatFixed(state.groundSpeedMs * KilometresPerHourPerMetrePerSecond, 2, 1));
}

QString NmeaSentenceBuilder::Rmc(const NmeaVehicleState &state, const QDateTime &utcNow)
{
    // $GP{0},{1:HHmmss.fff},{2},{3},{4},{5},{6},{7},{8},{9:ddMMyy},{10},{11},{12}
    return QStringLiteral("$GPRMC,%1,A,%2,%3,%4,%5,%6,%7,%8,0,E,A")
        .arg(FormatTime(utcNow),
             FormatFixed(DegreesMinutes(state.latitude), 1, 5), hemisphereLatitude(state.latitude),
             FormatFixed(DegreesMinutes(state.longitude), 1, 5), hemisphereLongitude(state.longitude),
             FormatFixed(state.groundSpeedMs * KnotsPerMetrePerSecond, 1, 1),
             FormatFixed(state.groundCourseDeg, 1, 1), FormatDate(utcNow));
}

QStringList NmeaSentenceBuilder::Tick(const NmeaVehicleState &state, const QDateTime &utcNow)
{
    return QStringList{Gga(state, utcNow), Gll(state, utcNow), Hdg(state), Vtg(state),
                       Rmc(state, utcNow)};
}

QByteArray NmeaSentenceBuilder::Line(const QString &body)
{
    return (body + QLatin1Char('*') + Checksum(body) + QStringLiteral("\r\n")).toLatin1();
}

QByteArray NmeaSentenceBuilder::TickBytes(const NmeaVehicleState &state, const QDateTime &utcNow)
{
    QByteArray bytes;
    for (const QString &body : Tick(state, utcNow)) {
        bytes += Line(body);
    }
    return bytes;
}
