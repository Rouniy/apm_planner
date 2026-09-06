#include "MicrodroneTelemetryState.h"

#include <QtGlobal>

#include <cmath>
#include <limits>

namespace {

constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr double RadiansToDegrees = 180.0 / Pi;

float normalizePositiveDegrees(float value)
{
    return value < 0.0f ? value + 360.0f : value;
}

int truncateTemperature(float value)
{
    constexpr float IntUpperExclusive = 2147483648.0f;
    constexpr float IntLowerInclusive = -2147483648.0f;
    if (qIsNaN(value)) {
        return 0;
    }
    if (value >= IntUpperExclusive) {
        return std::numeric_limits<int>::max();
    }
    if (value <= IntLowerInclusive) {
        return std::numeric_limits<int>::min();
    }
    // .NET 10 saturates float-to-int conversion at both bounds and maps NaN
    // to zero. Handle those cases above to avoid undefined C++ conversion.
    return static_cast<int>(value);
}

} // namespace

bool MicrodroneTelemetryState::Apply(const mavlink_message_t &message,
                                     const QDateTime &receivedUtc)
{
    switch (message.msgid) {
    case MAVLINK_MSG_ID_HOME_POSITION: {
        mavlink_home_position_t value{};
        mavlink_msg_home_position_decode(&message, &value);
        m_homeAltitude = value.altitude / 1000.0;
        return true;
    }
    case MAVLINK_MSG_ID_HIGH_LATENCY: {
        mavlink_high_latency_t value{};
        mavlink_msg_high_latency_decode(&message, &value);
        m_values.roll = static_cast<double>(static_cast<float>(value.roll / 100.0f));
        m_values.pitch = static_cast<double>(static_cast<float>(value.pitch / 100.0f));
        m_values.yaw = static_cast<double>(normalizePositiveDegrees(
            static_cast<float>(value.heading / 100.0f)));
        m_values.latitude = value.latitude / 1.0e7;
        m_values.longitude = value.longitude / 1.0e7;
        setAltitude(static_cast<float>(value.altitude_amsl)
            - static_cast<float>(m_homeAltitude), receivedUtc);
        m_values.groundSpeed = static_cast<double>(static_cast<float>(value.groundspeed));
        m_values.satelliteCount = static_cast<double>(static_cast<float>(value.gps_nsat));
        m_values.pressureTemperature = static_cast<double>(value.temperature);
        return true;
    }
    case MAVLINK_MSG_ID_HIGH_LATENCY2: {
        mavlink_high_latency2_t value{};
        mavlink_msg_high_latency2_decode(&message, &value);
        m_values.latitude = value.latitude / 1.0e7;
        m_values.longitude = value.longitude / 1.0e7;
        setAltitude(static_cast<float>(value.altitude)
            - static_cast<float>(m_homeAltitude), receivedUtc);
        m_values.yaw = static_cast<double>(normalizePositiveDegrees(
            static_cast<float>(value.heading * 2)));
        m_values.groundSpeed = static_cast<double>(
            static_cast<float>(value.groundspeed / 5.0f));
        m_values.gpsHdop = static_cast<double>(static_cast<float>(value.eph));
        return true;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
        mavlink_attitude_t value{};
        mavlink_msg_attitude_decode(&message, &value);
        m_values.roll = static_cast<double>(
            static_cast<float>(value.roll * RadiansToDegrees));
        m_values.pitch = static_cast<double>(
            static_cast<float>(value.pitch * RadiansToDegrees));
        m_values.yaw = static_cast<double>(normalizePositiveDegrees(
            static_cast<float>(value.yaw * RadiansToDegrees)));
        return true;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t value{};
        mavlink_msg_global_position_int_decode(&message, &value);
        setAltitude(static_cast<float>(value.relative_alt / 1000.0f), receivedUtc);
        m_globalPosition = value.lat != 0 && value.lon != 0
            && value.lat != std::numeric_limits<qint32>::max()
            && value.lon != std::numeric_limits<qint32>::max();
        if (m_globalPosition) {
            m_values.latitude = value.lat / 10000000.0;
            m_values.longitude = value.lon / 10000000.0;
        }
        return true;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        mavlink_gps_raw_int_t value{};
        mavlink_msg_gps_raw_int_decode(&message, &value);
        if (!m_globalPosition) {
            if (value.lat != std::numeric_limits<qint32>::max()) {
                m_values.latitude = value.lat * 1.0e-7;
            }
            if (value.lon != std::numeric_limits<qint32>::max()) {
                m_values.longitude = value.lon * 1.0e-7;
            }
        }
        if (value.eph != std::numeric_limits<quint16>::max()) {
            // eph has centi-unit granularity already; MP10's Round(..., 2)
            // therefore leaves the double unchanged before the float cast.
            m_values.gpsHdop = static_cast<double>(
                static_cast<float>(value.eph / 100.0));
        }
        if (value.satellites_visible != std::numeric_limits<quint8>::max()) {
            m_values.satelliteCount = static_cast<double>(
                static_cast<float>(value.satellites_visible));
        }
        if (value.vel != std::numeric_limits<quint16>::max()) {
            m_values.groundSpeed = static_cast<double>(
                static_cast<float>(value.vel * 1.0e-2f));
        }
        if (static_cast<float>(m_values.groundSpeed) > 0.5f
            && value.cog != std::numeric_limits<quint16>::max()) {
            m_values.groundCourse = static_cast<double>(normalizePositiveDegrees(
                static_cast<float>(value.cog * 1.0e-2f)));
        }
        return true;
    }
    case MAVLINK_MSG_ID_GPS_STATUS: {
        mavlink_gps_status_t value{};
        mavlink_msg_gps_status_decode(&message, &value);
        m_values.satelliteCount = static_cast<double>(
            static_cast<float>(value.satellites_visible));
        return true;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
        mavlink_vfr_hud_t value{};
        mavlink_msg_vfr_hud_decode(&message, &value);
        m_values.groundSpeed = static_cast<double>(value.groundspeed);
        return true;
    }
    case MAVLINK_MSG_ID_SCALED_PRESSURE: {
        mavlink_scaled_pressure_t value{};
        mavlink_msg_scaled_pressure_decode(&message, &value);
        // CurrentState exposes this raw centi-degree integer to the encoder.
        m_values.pressureTemperature = static_cast<double>(value.temperature);
        return true;
    }
    case MAVLINK_MSG_ID_RAW_IMU: {
        mavlink_raw_imu_t value{};
        mavlink_msg_raw_imu_decode(&message, &value);
        m_values.magnetometerX = static_cast<double>(static_cast<float>(value.xmag));
        m_values.magnetometerY = static_cast<double>(static_cast<float>(value.ymag));
        m_values.magnetometerZ = static_cast<double>(static_cast<float>(value.zmag));
        return true;
    }
    case MAVLINK_MSG_ID_SCALED_IMU: {
        mavlink_scaled_imu_t value{};
        mavlink_msg_scaled_imu_decode(&message, &value);
        m_values.magnetometerX = static_cast<double>(static_cast<float>(value.xmag));
        m_values.magnetometerY = static_cast<double>(static_cast<float>(value.ymag));
        m_values.magnetometerZ = static_cast<double>(static_cast<float>(value.zmag));
        return true;
    }
    case MAVLINK_MSG_ID_HIGHRES_IMU: {
        mavlink_highres_imu_t value{};
        mavlink_msg_highres_imu_decode(&message, &value);
        constexpr quint16 UpdatedMagnetometer = 0x40;
        constexpr quint16 UpdatedTemperature = 0x1000;
        if (value.id == 0 && (value.fields_updated & UpdatedMagnetometer) != 0) {
            m_values.magnetometerX = static_cast<double>(value.xmag);
            m_values.magnetometerY = static_cast<double>(value.ymag);
            m_values.magnetometerZ = static_cast<double>(value.zmag);
        }
        // CurrentState has one press_temp field: temperature from IMU ids 0,
        // 1 and 2 all replace it when the update bit is present.
        if (value.id <= 2 && (value.fields_updated & UpdatedTemperature) != 0) {
            m_values.pressureTemperature = static_cast<double>(
                truncateTemperature(value.temperature));
        }
        return true;
    }
    default:
        return false;
    }
}

void MicrodroneTelemetryState::clear()
{
    m_values = {};
    m_globalPosition = false;
    m_homeAltitude = 0.0;
    m_oldAltitude = 0.0f;
    m_verticalSpeed = 0.0f;
    m_lastAltitude = QDateTime(QDate(1, 1, 1), QTime(0, 0), Qt::UTC);
}

void MicrodroneTelemetryState::setAltitude(float metres,
                                           const QDateTime &receivedUtc)
{
    // All arithmetic through this routine deliberately remains float before
    // widening into MicrodroneTelemetry, matching CurrentState.alt and its EMA.
    m_values.altitude = static_cast<double>(metres);
    if (!receivedUtc.isValid()) {
        return;
    }

    const QDateTime timestamp = receivedUtc.toUTC();
    const double elapsedSeconds = m_lastAltitude.msecsTo(timestamp) / 1000.0;
    const bool clockRolledBack = m_lastAltitude > timestamp;
    if ((elapsedSeconds >= 0.2 && m_oldAltitude != metres)
        || clockRolledBack) {
        const float instantaneous = (metres - m_oldAltitude)
            / static_cast<float>(elapsedSeconds);
        m_verticalSpeed = m_verticalSpeed * 0.4f + instantaneous * 0.6f;
        if (qIsInf(m_verticalSpeed)) {
            m_verticalSpeed = 0.0f;
        }
        if (qIsNaN(m_verticalSpeed)) {
            // CurrentState's verticalspeed getter normalizes NaN when captured.
            m_verticalSpeed = 0.0f;
        }
        m_lastAltitude = timestamp;
        m_oldAltitude = metres;
    }
    m_values.verticalSpeed = static_cast<double>(m_verticalSpeed);
}
