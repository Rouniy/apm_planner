#ifndef MAVLINK_USE_MESSAGE_INFO
#define MAVLINK_USE_MESSAGE_INFO
#endif
#include "WarningTelemetrySource.h"
#include "comm/VehicleTargetManager.h"

#include <QtEndian>
#include <QCoreApplication>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Degrees = 180.0 / 3.14159265358979323846;
enum Invalid { None, U16Max, U8Max, I16Max, U32Max, I32Max, MinusOne, Percentage, Negative, NonPositive, Zero, CoordinateLat, CoordinateLon, GpsYaw };
struct Mapping {
    quint32 message;
    QString wire, name, unit;
    double scale = 1;
    Invalid invalid = None;
    int index = 0;
    int instance = -1;
};

const QVector<Mapping> &mappings()
{
    static const QVector<Mapping> result = [] {
        QVector<Mapping> v;
        auto add = [&](quint32 msg, const QString &wire, const QString &name,
                       const QString &unit = QString(), double scale = 1,
                       Invalid invalid = None, int index = 0, int instance = -1) {
            v.append({msg, wire, name, unit, scale, invalid, index, instance});
        };
#define ADD(MSG, WIRE, NAME, UNIT, SCALE, INVALID) \
        add(MAVLINK_MSG_ID_##MSG, QStringLiteral(#WIRE), QStringLiteral(#NAME), QStringLiteral(UNIT), SCALE, INVALID)
        ADD(ATTITUDE, roll, roll, "deg", Degrees, None);
        ADD(ATTITUDE, pitch, pitch, "deg", Degrees, None);
        ADD(ATTITUDE, yaw, yaw, "deg", Degrees, None);
        ADD(AOA_SSA, AOA, AOA, "deg", 1, None);
        ADD(AOA_SSA, SSA, SSA, "deg", 1, None);
        ADD(GLOBAL_POSITION_INT, lat, lat, "deg", 1e-7, CoordinateLat);
        ADD(GLOBAL_POSITION_INT, lon, lng, "deg", 1e-7, CoordinateLon);
        ADD(GLOBAL_POSITION_INT, alt, altasl, "m", .001, None);
        ADD(GLOBAL_POSITION_INT, relative_alt, alt, "m", .001, None);
        ADD(GLOBAL_POSITION_INT, vx, vx, "m/s", .01, I16Max);
        ADD(GLOBAL_POSITION_INT, vy, vy, "m/s", .01, I16Max);
        ADD(GLOBAL_POSITION_INT, vz, vz, "m/s", .01, I16Max);
        ADD(VFR_HUD, airspeed, airspeed, "m/s", 1, Negative);
        ADD(VFR_HUD, groundspeed, groundspeed, "m/s", 1, Negative);
        ADD(VFR_HUD, climb, climbrate, "m/s", 1, None);
        ADD(VFR_HUD, throttle, ch3percent, "%", 1, U16Max);
        ADD(NAV_CONTROLLER_OUTPUT, nav_roll, nav_roll, "deg", 1, None);
        ADD(NAV_CONTROLLER_OUTPUT, nav_pitch, nav_pitch, "deg", 1, None);
        ADD(NAV_CONTROLLER_OUTPUT, nav_bearing, nav_bearing, "deg", 1, None);
        ADD(NAV_CONTROLLER_OUTPUT, target_bearing, target_bearing, "deg", 1, None);
        ADD(NAV_CONTROLLER_OUTPUT, wp_dist, wp_dist, "m", 1, None);
        ADD(NAV_CONTROLLER_OUTPUT, alt_error, alt_error, "m", 1, None);
        ADD(NAV_CONTROLLER_OUTPUT, aspd_error, aspd_error, "m/s", .01, None);
        ADD(NAV_CONTROLLER_OUTPUT, xtrack_error, xtrack_error, "m", 1, None);
        ADD(MISSION_CURRENT, seq, wpno, "", 1, U16Max);
        ADD(SYS_STATUS, load, load, "%", .1, U16Max);
        ADD(SYS_STATUS, voltage_battery, battery_voltage, "V", .001, U16Max);
        ADD(SYS_STATUS, current_battery, current, "A", .01, MinusOne);
        ADD(SYS_STATUS, battery_remaining, battery_remaining, "%", 1, Percentage);
        ADD(SYS_STATUS, drop_rate_comm, packetdropremote, "c%", 1, None);
        ADD(SYS_STATUS, errors_count1, errors_count1, "", 1, None);
        ADD(SYS_STATUS, errors_count2, errors_count2, "", 1, None);
        ADD(SYS_STATUS, errors_count3, errors_count3, "", 1, None);
        ADD(SYS_STATUS, errors_count4, errors_count4, "", 1, None);
        ADD(BATTERY2, voltage, battery_voltage2, "V", .001, U16Max);
        ADD(BATTERY2, current_battery, current2, "A", .01, MinusOne);
        for (int id = 0; id < 9; ++id) {
            const QString suffix = id == 0 ? QString() : QString::number(id + 1);
            add(MAVLINK_MSG_ID_BATTERY_STATUS, "current_battery", "current" + suffix, "A", .01, MinusOne, 0, id);
            add(MAVLINK_MSG_ID_BATTERY_STATUS, "battery_remaining", "battery_remaining" + suffix, "%", 1, Percentage, 0, id);
            add(MAVLINK_MSG_ID_BATTERY_STATUS, "current_consumed", "battery_usedmah" + suffix, "mAh", 1, Negative, 0, id);
            add(MAVLINK_MSG_ID_BATTERY_STATUS, "temperature", "battery_temp" + suffix, "degC", .01, I16Max, 0, id);
            add(MAVLINK_MSG_ID_BATTERY_STATUS, "time_remaining", "battery_remainmin" + suffix, "min", 1.0/60, NonPositive, 0, id);
        }
        for (int n = 1; n <= 14; ++n)
            add(MAVLINK_MSG_ID_BATTERY_STATUS, n <= 10 ? "voltages" : "voltages_ext", QString("battery_cell%1").arg(n), "V", .001, n <= 10 ? U16Max : Zero, n <= 10 ? n-1 : n-11, 0);
        for (int gps = 0; gps < 2; ++gps) {
            const auto msg = gps ? MAVLINK_MSG_ID_GPS2_RAW : MAVLINK_MSG_ID_GPS_RAW_INT;
            const QString suffix = gps ? "2" : "";
            add(msg, "lat", "lat" + suffix, "deg", 1e-7, CoordinateLat);
            add(msg, "lon", "lng" + suffix, "deg", 1e-7, CoordinateLon);
            add(msg, "alt", "altasl" + suffix, "m", .001);
            add(msg, "fix_type", "gpsstatus" + suffix);
            add(msg, "eph", "gpshdop" + suffix, "", .01, U16Max);
            add(msg, "satellites_visible", "satcount" + suffix, "", 1, U8Max);
            add(msg, "vel", "groundspeed" + suffix, "m/s", .01, U16Max);
            add(msg, "cog", "groundcourse" + suffix, "deg", .01, U16Max);
            add(msg, "h_acc", "gpsh_acc" + suffix, "m", .001, Zero);
            add(msg, "v_acc", "gpsv_acc" + suffix, "m", .001, Zero);
            add(msg, "vel_acc", "gpsvel_acc" + suffix, "m/s", .001, Zero);
            add(msg, "hdg_acc", "gpshdg_acc" + suffix, "deg", 1e-5, Zero);
            add(msg, "yaw", "gpsyaw" + suffix, "deg", .01, GpsYaw);
        }
        for (int n = 1; n <= 16; ++n) {
            add(MAVLINK_MSG_ID_RC_CHANNELS, QString("chan%1_raw").arg(n), QString("ch%1in").arg(n), "us", 1, U16Max);
            if (n <= 8)
                add(MAVLINK_MSG_ID_RC_CHANNELS_RAW, QString("chan%1_raw").arg(n), QString("ch%1in").arg(n), "us", 1, U16Max);
            for (int port = 0; port < 2; ++port)
                add(MAVLINK_MSG_ID_SERVO_OUTPUT_RAW, QString("servo%1_raw").arg(n), QString("ch%1out").arg(port*16+n), "us", 1, Zero, 0, port);
        }
        const quint32 imus[] = {MAVLINK_MSG_ID_RAW_IMU, MAVLINK_MSG_ID_SCALED_IMU, MAVLINK_MSG_ID_SCALED_IMU2, MAVLINK_MSG_ID_SCALED_IMU3};
        for (int i = 0; i < 4; ++i) {
            const QString suffix = i < 2 ? "" : QString::number(i);
            for (const auto &axis : {QString("x"), QString("y"), QString("z")}) {
                add(imus[i], axis + "acc", "a" + axis + suffix, "mg");
                add(imus[i], axis + "gyro", "g" + axis + suffix, "mrad/s");
                add(imus[i], axis + "mag", "m" + axis + suffix, "mG");
            }
            add(imus[i], "temperature", QString("imu%1_temp").arg(i < 2 ? 1 : i), "degC", .01, Zero);
        }
        ADD(SCALED_PRESSURE, press_abs, press_abs, "hPa", 1, NonPositive);
        ADD(SCALED_PRESSURE, temperature, press_temp, "cdegC", 1, None);
        ADD(SCALED_PRESSURE, temperature_press_diff, airspeed1_temp, "cdegC", 1, Zero);
        ADD(SCALED_PRESSURE2, press_abs, press_abs2, "hPa", 1, NonPositive);
        ADD(SCALED_PRESSURE2, temperature, press_temp2, "cdegC", 1, None);
        ADD(SCALED_PRESSURE2, temperature_press_diff, airspeed2_temp, "cdegC", 1, Zero);
        ADD(AIRSPEED_AUTOCAL, ratio, asratio, "", 1, None);
        ADD(HWSTATUS, Vcc, hwvoltage, "V", .001, None);
        ADD(HWSTATUS, I2Cerr, i2cerrors, "", 1, None);
        ADD(POWER_STATUS, Vcc, boardvoltage, "mV", 1, None);
        ADD(POWER_STATUS, Vservo, servovoltage, "mV", 1, None);
        ADD(POWER_STATUS, flags, voltageflag, "bitmask", 1, None);
        ADD(MEMINFO, brkval, brklevel, "byte", 1, None);
        ADD(MEMINFO, freemem, freemem, "byte", 1, None);
        ADD(EXTENDED_SYS_STATE, vtol_state, vtol_state, "enum", 1, Zero);
        ADD(EXTENDED_SYS_STATE, landed_state, landed_state, "enum", 1, Zero);
        ADD(EKF_STATUS_REPORT, flags, ekfflags, "bitmask", 1, None);
        ADD(EKF_STATUS_REPORT, velocity_variance, ekfvelv, "", 1, Negative);
        ADD(EKF_STATUS_REPORT, compass_variance, ekfcompv, "", 1, Negative);
        ADD(EKF_STATUS_REPORT, pos_horiz_variance, ekfposhor, "", 1, Negative);
        ADD(EKF_STATUS_REPORT, pos_vert_variance, ekfposvert, "", 1, Negative);
        ADD(EKF_STATUS_REPORT, terrain_alt_variance, ekfteralt, "", 1, Negative);
        ADD(VIBRATION, vibration_x, vibex, "m/s2", 1, Negative);
        ADD(VIBRATION, vibration_y, vibey, "m/s2", 1, Negative);
        ADD(VIBRATION, vibration_z, vibez, "m/s2", 1, Negative);
        ADD(VIBRATION, clipping_0, vibeclip0, "", 1, None);
        ADD(VIBRATION, clipping_1, vibeclip1, "", 1, None);
        ADD(VIBRATION, clipping_2, vibeclip2, "", 1, None);
        ADD(RANGEFINDER, distance, sonarrange, "m", 1, Negative);
        ADD(RANGEFINDER, voltage, sonarvoltage, "V", 1, Negative);
        for (int i = 0; i < 10; ++i)
            add(MAVLINK_MSG_ID_DISTANCE_SENSOR, "current_distance", QString("rangefinder%1").arg(i+1), "cm", 1, U16Max, 0, i);
        ADD(RPM, rpm1, rpm1, "rpm", 1, None);
        ADD(RPM, rpm2, rpm2, "rpm", 1, None);
        ADD(WIND, direction, wind_dir, "deg", 1, None);
        ADD(WIND, speed, wind_vel, "m/s", 1, Negative);
        ADD(TERRAIN_REPORT, current_height, ter_curalt, "m", 1, None);
        ADD(TERRAIN_REPORT, terrain_height, ter_alt, "m", 1, None);
        ADD(TERRAIN_REPORT, loaded, ter_load, "", 1, None);
        ADD(TERRAIN_REPORT, pending, ter_pend, "", 1, None);
        ADD(TERRAIN_REPORT, spacing, ter_space, "m", 1, None);
        ADD(OPTICAL_FLOW, flow_comp_m_x, opt_m_x, "m/s", 1, None);
        ADD(OPTICAL_FLOW, flow_comp_m_y, opt_m_y, "m/s", 1, None);
        ADD(OPTICAL_FLOW, flow_x, opt_x, "pixel", 1, None);
        ADD(OPTICAL_FLOW, flow_y, opt_y, "pixel", 1, None);
        ADD(OPTICAL_FLOW, quality, opt_qua, "", 1, None);
        ADD(AHRS2, roll, ahrs2_roll, "deg", Degrees, None);
        ADD(AHRS2, pitch, ahrs2_pitch, "deg", Degrees, None);
        ADD(AHRS2, yaw, ahrs2_yaw, "deg", Degrees, None);
        ADD(AHRS2, altitude, ahrs2_alt, "m", 1, None);
        ADD(AHRS2, lat, ahrs2_lat, "deg", 1e-7, CoordinateLat);
        ADD(AHRS2, lng, ahrs2_lng, "deg", 1e-7, CoordinateLon);
        ADD(LOCAL_POSITION_NED, x, posn, "m", 1, None);
        ADD(LOCAL_POSITION_NED, y, pose, "m", 1, None);
        ADD(LOCAL_POSITION_NED, z, posd, "m", 1, None);
        ADD(FENCE_STATUS, breach_count, fenceb_count, "", 1, None);
        ADD(FENCE_STATUS, breach_status, fenceb_status, "", 1, None);
        ADD(FENCE_STATUS, breach_type, fenceb_type, "enum", 1, None);
        ADD(PID_TUNING, FF, pidff, "", 1, None);
        ADD(PID_TUNING, P, pidP, "", 1, None);
        ADD(PID_TUNING, I, pidI, "", 1, None);
        ADD(PID_TUNING, D, pidD, "", 1, None);
        ADD(PID_TUNING, axis, pidaxis, "enum", 1, None);
        ADD(PID_TUNING, desired, piddesired, "axis dependent", 1, None);
        ADD(PID_TUNING, achieved, pidachieved, "axis dependent", 1, None);
        ADD(PID_TUNING, SRate, pidSRate, "axis dependent", 1, None);
        ADD(PID_TUNING, PDmod, pidPDmod, "", 1, None);
        for (int id=0; id<2; ++id) {
            add(MAVLINK_MSG_ID_HYGROMETER_SENSOR,"temperature",QString("hygrotemp%1").arg(id+1),"cdegC",1,None,0,id);
            add(MAVLINK_MSG_ID_HYGROMETER_SENSOR,"humidity",QString("hygrohumi%1").arg(id+1),"c%",1,None,0,id);
        }
        add(MAVLINK_MSG_ID_MCU_STATUS,"MCU_temperature","mcutemp","degC",.01,None,0,0);
        add(MAVLINK_MSG_ID_MCU_STATUS,"MCU_voltage","mcuvoltage","V",.001,None,0,0);
        add(MAVLINK_MSG_ID_MCU_STATUS,"MCU_voltage_min","mcuminvolt","V",.001,None,0,0);
        add(MAVLINK_MSG_ID_MCU_STATUS,"MCU_voltage_max","mcumaxvolt","V",.001,None,0,0);
        ADD(GENERATOR_STATUS, status, gen_status, "bitmask", 1, None);
        ADD(GENERATOR_STATUS, generator_speed, gen_speed, "rpm", 1, U16Max);
        ADD(GENERATOR_STATUS, load_current, gen_current, "A", 1, None);
        ADD(GENERATOR_STATUS, bus_voltage, gen_voltage, "V", 1, None);
        ADD(GENERATOR_STATUS, runtime, gen_runtime, "s", 1, U32Max);
        ADD(GENERATOR_STATUS, time_until_maintenance, gen_maint_time, "s", 1, I32Max);
        add(MAVLINK_MSG_ID_EFI_STATUS,"barometric_pressure","efi_baro","kPa",1,None,0,0);
        add(MAVLINK_MSG_ID_EFI_STATUS,"cylinder_head_temperature","efi_headtemp","degC",1,None,0,0);
        add(MAVLINK_MSG_ID_EFI_STATUS,"engine_load","efi_load","%",1,None,0,0);
        add(MAVLINK_MSG_ID_EFI_STATUS,"health","efi_health","enum",1,None,0,0);
        add(MAVLINK_MSG_ID_EFI_STATUS,"exhaust_gas_temperature","efi_exhasttemp","degC",1,None,0,0);
        add(MAVLINK_MSG_ID_EFI_STATUS,"intake_manifold_temperature","efi_intaketemp","degC",1,None,0,0);
        add(MAVLINK_MSG_ID_EFI_STATUS,"rpm","efi_rpm","rpm",1,None,0,0);
        add(MAVLINK_MSG_ID_EFI_STATUS,"fuel_flow","efi_fuelflow","cm3/min",1,None,0,0);
        add(MAVLINK_MSG_ID_EFI_STATUS,"fuel_consumed","efi_fuelconsumed","cm3",1,None,0,0);
        for (quint32 msg : {quint32(MAVLINK_MSG_ID_RADIO), quint32(MAVLINK_MSG_ID_RADIO_STATUS)}) {
            add(msg, "rssi", "rssi", "raw"); add(msg, "remrssi", "remrssi", "raw");
            add(msg, "noise", "noise", "raw"); add(msg, "remnoise", "remnoise", "raw");
            add(msg, "txbuf", "txbuffer", "%"); add(msg, "rxerrors", "rxerrors");
            add(msg, "fixed", "fixedp");
        }
        const quint32 escs[] = {MAVLINK_MSG_ID_ESC_TELEMETRY_1_TO_4, MAVLINK_MSG_ID_ESC_TELEMETRY_5_TO_8, MAVLINK_MSG_ID_ESC_TELEMETRY_9_TO_12, MAVLINK_MSG_ID_ESC_TELEMETRY_13_TO_16};
        for (int block = 0; block < 4; ++block)
            for (int i = 0; i < 4; ++i) {
                const auto name = QString("esc%1_").arg(block*4+i+1);
                add(escs[block], "voltage", name+"volt", "V", .01, None, i);
                add(escs[block], "current", name+"curr", "A", .01, None, i);
                add(escs[block], "rpm", name+"rpm", "rpm", 1, None, i);
                add(escs[block], "temperature", name+"temp", "degC", 1, None, i);
            }
#undef ADD
        return v;
    }();
    return result;
}

const QHash<QString, QString> &catalog()
{
    static const QHash<QString, QString> result = [] {
        QHash<QString, QString> c;
        for (const auto &m : mappings()) c.insert(m.name, m.unit);
        for (int id = 1; id <= 9; ++id)
            c.insert("battery_voltage" + (id == 1 ? QString() : QString::number(id)), "V");
        for (int i = 1; i <= 3; ++i) {
            const auto suffix = i == 1 ? QString() : QString::number(i);
            c.insert("accelsq"+suffix, "g"); c.insert("gyrosq"+suffix, "mrad/s"); c.insert("magfield"+suffix, "mG");
        }
        c.insert("armed", "bool"); c.insert("failsafe", "bool"); c.insert("landed", "bool");
        c.insert("connected", "bool"); c.insert("safetyactive", "bool"); c.insert("terrainactive", "bool"); c.insert("prearmstatus", "bool");
        c.insert("rxrssi", "%"); c.insert("watts", "W"); c.insert("vlen", "m/s"); c.insert("satcountB", "");
        c.insert("verticalspeed_fpm", "ft/min"); c.insert("ber_error", "deg"); c.insert("ekfstatus", "");
        c.insert("DistToHome", "m"); c.insert("HomeAlt", "m");
        c.insert("verticalspeed", "m/s");
        return c;
    }();
    return result;
}

// MAVLink 2 permits trimming any trailing zero byte, even inside a scalar.
// Copy only actual payload bytes; generated encoders can leave checksum bytes
// after len. Extension fields in MAVLink 1 are absent, never zero samples.
double raw(const mavlink_message_t &message, const QString &name, int index = 0)
{
    const auto *info = mavlink_get_message_info(&message);
    const auto *entry = mavlink_get_msg_entry(message.msgid);
    if (!info || !entry) return NaN;
    for (unsigned i = 0; i < info->num_fields; ++i) {
        const auto &f = info->fields[i];
        if (name != QLatin1String(f.name)) continue;
        if (index < 0 || index >= std::max(1, int(f.array_length))) return NaN;
        int width = 0;
        switch (f.type) {
        case MAVLINK_TYPE_UINT8_T: case MAVLINK_TYPE_INT8_T: width=1; break;
        case MAVLINK_TYPE_UINT16_T: case MAVLINK_TYPE_INT16_T: width=2; break;
        case MAVLINK_TYPE_UINT32_T: case MAVLINK_TYPE_INT32_T: case MAVLINK_TYPE_FLOAT: width=4; break;
        case MAVLINK_TYPE_UINT64_T: case MAVLINK_TYPE_INT64_T: case MAVLINK_TYPE_DOUBLE: width=8; break;
        default: return NaN;
        }
        const int offset = f.wire_offset + index*width;
        if (offset+width > entry->max_msg_len || (message.magic == MAVLINK_STX_MAVLINK1 && offset+width > entry->min_msg_len)) return NaN;
        quint8 bytes[8]{};
        if (offset < message.len)
            std::memcpy(bytes, _MAV_PAYLOAD(&message)+offset, size_t(std::min(width, int(message.len)-offset)));
        switch (f.type) {
        case MAVLINK_TYPE_UINT8_T: return bytes[0];
        case MAVLINK_TYPE_INT8_T: return qint8(bytes[0]);
        case MAVLINK_TYPE_UINT16_T: return qFromLittleEndian<quint16>(bytes);
        case MAVLINK_TYPE_INT16_T: return qFromLittleEndian<qint16>(bytes);
        case MAVLINK_TYPE_UINT32_T: return qFromLittleEndian<quint32>(bytes);
        case MAVLINK_TYPE_INT32_T: return qFromLittleEndian<qint32>(bytes);
        case MAVLINK_TYPE_UINT64_T: return double(qFromLittleEndian<quint64>(bytes));
        case MAVLINK_TYPE_INT64_T: return double(qFromLittleEndian<qint64>(bytes));
        case MAVLINK_TYPE_FLOAT: { const quint32 bits = qFromLittleEndian<quint32>(bytes); float value; std::memcpy(&value, &bits, 4); return value; }
        case MAVLINK_TYPE_DOUBLE: { const quint64 bits = qFromLittleEndian<quint64>(bytes); double value; std::memcpy(&value, &bits, 8); return value; }
        default: return NaN;
        }
    }
    return NaN;
}

bool validRaw(double value, Invalid invalid)
{
    if (!std::isfinite(value)) return false;
    switch (invalid) {
    case U16Max: return value != 65535;
    case U8Max: return value != 255;
    case I16Max: return value != 32767;
    case U32Max: return value != 4294967295.0;
    case I32Max: return value != 2147483647.0;
    case MinusOne: return value != -1;
    case Percentage: return value >= 0 && value <= 100;
    case Negative: return value >= 0;
    case NonPositive: return value > 0;
    case Zero: return value != 0;
    case CoordinateLat: return std::abs(value) <= 900000000;
    case CoordinateLon: return std::abs(value) <= 1800000000;
    case GpsYaw: return value > 0 && value <= 36000;
    default: return true;
    }
}

bool freshAt(qint64 now, qint64 at)
{
    return at >= 0 && now >= at
        && quint64(now)-quint64(at) <= quint64(WarningTelemetrySource::FreshnessMs);
}
}

WarningTelemetrySource::WarningTelemetrySource(VehicleTargetManager *targets, Clock clock, QObject *parent)
    : QObject(parent), m_targets(targets), m_clock(std::move(clock))
{
    m_elapsed.start();
    if (targets) {
        connect(targets, &VehicleTargetManager::targetGenerationChanged, this, [this] { clearEpoch(false); });
        connect(targets, &VehicleTargetManager::targetGenerationSettled, this, [this] { clearEpoch(true); });
        connect(targets, &QObject::destroyed, this, [this] { m_targets = nullptr; clearEpoch(false); });
    }
    clearEpoch(true);
}

QStringList WarningTelemetrySource::fieldNames() { auto names = catalog().keys(); names.sort(Qt::CaseInsensitive); return names; }
QString WarningTelemetrySource::fieldUnits(const QString &field) { return catalog().value(field); }
qint64 WarningTelemetrySource::nowMs() const
{
    // A trusted injected callback may synchronously delete its owning source.
    // Keep the callable alive independently and access no members afterward.
    const Clock clock=m_clock;
    return clock ? clock() : m_elapsed.elapsed();
}

bool WarningTelemetrySource::current() const
{
    return m_targets && m_lease.isValid() && m_targets->isTargetGenerationSettled()
        && m_targets->isCurrentTarget(m_lease.endpoint.linkId, m_lease.endpoint.systemId, m_lease.endpoint.componentId, m_lease.generation);
}

bool WarningTelemetrySource::isCurrentLease(const VehicleTargetLease &lease) const
{
    return current() && lease.isValid() && lease.generation == m_lease.generation
        && lease.endpoint.sameIdentity(m_lease.endpoint);
}

void WarningTelemetrySource::clearEpoch(bool acquire)
{
    m_samples.clear(); m_lease = {}; m_homeValid = false;
    if (acquire && m_targets && m_targets->isTargetGenerationSettled()) m_lease = m_targets->acquireTarget();
    ++m_epoch;
    // The legacy LinkManager singleton may be destroyed after QApplication.
    // Invalidate state, but never send a late repaint into a dead Qt style.
    if (!QCoreApplication::closingDown())
        emit epochChanged(); // Last operation: listeners may change the target or delete this source.
}

void WarningTelemetrySource::invalidateSourceEpoch() { clearEpoch(true); }

void WarningTelemetrySource::put(const QString &name, double value, qint64 at, bool valid)
{
    if (valid && std::isfinite(value) && at >= 0) m_samples.insert(name, {value, at});
    else m_samples.remove(name);
}

QHash<QString, double> WarningTelemetrySource::values() const
{
    QHash<QString, double> out;
    if (!current()) return out;
    const auto capturedEpoch = m_epoch;
    const QPointer<const WarningTelemetrySource> guard(this);
    const auto now = nowMs();
    if (!guard || capturedEpoch != m_epoch || !current()) return out;
    for (auto i=m_samples.cbegin(); i!=m_samples.cend(); ++i)
        if (freshAt(now,i->at) && catalog().contains(i.key())) out.insert(i.key(), i->value);
    auto combine = [&](const QString &name, const QStringList &inputs, const std::function<double(const QVector<double>&)> &fn) {
        QVector<double> values;
        for (const auto &key : inputs) { if (!out.contains(key)) return; values.append(out.value(key)); }
        const double value = fn(values); if (std::isfinite(value)) out.insert(name, value);
    };
    combine("watts", {"battery_voltage", "current"}, [](const QVector<double> &v){ return v[0]*v[1]; });
    combine("vlen", {"vx", "vy", "vz"}, [](const QVector<double> &v){ return std::hypot(std::hypot(v[0],v[1]),v[2]); });
    combine("verticalspeed_fpm", {"vz"}, [](const QVector<double> &v){ return v[0]*-3.28084*60; });
    combine("satcountB", {"satcount", "satcount2"}, [](const QVector<double> &v){ return v[0]+v[1]; });
    combine("ber_error", {"target_bearing", "yaw"}, [](const QVector<double> &v){ return v[0]-v[1]; });
    if (m_homeValid) {
        // Home is a session-scoped reference, not a streaming sample. It may
        // be sent once. Distance still requires fresh position coordinates.
        out.insert("HomeAlt", m_homeAltitude);
        combine("DistToHome", {"lat", "lng"}, [this](const QVector<double> &v){
            const double lat1=m_homeLatitude/Degrees, lat2=v[0]/Degrees;
            const double dlat=(v[0]-m_homeLatitude)/Degrees;
            const double dlon=(v[1]-m_homeLongitude)/Degrees;
            const double a=std::pow(std::sin(dlat/2),2)+std::cos(lat1)*std::cos(lat2)*std::pow(std::sin(dlon/2),2);
            return 6371000.0*2*std::asin(std::sqrt(std::max(0.0,std::min(1.0,a))));
        });
    }
    for (int i=1; i<=3; ++i) {
        const QString suffix = i==1 ? QString() : QString::number(i);
        for (const QString &prefix : {QString("a"),QString("g"),QString("m")}) {
            const QString name = (prefix=="a" ? "accelsq" : prefix=="g" ? "gyrosq" : "magfield") + suffix;
            combine(name, {prefix+"x"+suffix,prefix+"y"+suffix,prefix+"z"+suffix}, [prefix](const QVector<double> &v){ return std::hypot(std::hypot(v[0],v[1]),v[2])/(prefix=="a" ? 1000.0 : 1.0); });
        }
    }
    combine("ekfstatus", {"ekfvelv","ekfcompv","ekfposhor","ekfposvert","ekfteralt","ekfflags"}, [&out](const QVector<double> &v){
        const auto flags = quint32(v[5]);
        if (!(flags & EKF_ATTITUDE) || (flags & EKF_UNINITIALIZED)
            || (out.value("gpsstatus", 0)>0 && !(flags & EKF_VELOCITY_HORIZ))) return 1.0;
        return *std::max_element(v.cbegin(), v.cbegin()+5);
    });
    return out;
}

void WarningTelemetrySource::observeMessage(int linkId, const mavlink_message_t &message)
{
    if (!current() || linkId != m_lease.endpoint.linkId
        || message.sysid != m_lease.endpoint.systemId || message.compid != m_lease.endpoint.componentId) return;
    const auto capturedEpoch = m_epoch;
    const QPointer<WarningTelemetrySource> guard(this);
    const auto at = nowMs();
    if (!guard || capturedEpoch != m_epoch || !current()) return;
    const auto *entry = mavlink_get_msg_entry(message.msgid);
    if (!entry || message.len > entry->max_msg_len
        || (message.magic == MAVLINK_STX_MAVLINK1 && message.len != entry->min_msg_len)
        || (message.magic != MAVLINK_STX_MAVLINK1 && message.magic != MAVLINK_STX)) return;
    // A RAW_IMU extension names its sensor instance. Do not let another IMU
    // overwrite the primary fields. The explicit SCALED_IMU2/3 routes supply
    // the other instances. RC_CHANNELS_RAW has only the first eight channels.
    if ((message.msgid == MAVLINK_MSG_ID_RAW_IMU && raw(message,"id") > 0)
        || (message.msgid == MAVLINK_MSG_ID_RC_CHANNELS_RAW && raw(message,"port") != 0)) return;
    for (const auto &mapping : mappings()) {
        if (mapping.message != message.msgid) continue;
        if (mapping.instance >= 0) {
            const auto discriminator = message.msgid == MAVLINK_MSG_ID_SERVO_OUTPUT_RAW ? "port"
                : message.msgid == MAVLINK_MSG_ID_EFI_STATUS ? "ecu_index" : "id";
            if (raw(message, discriminator) != mapping.instance) continue;
        }
        double value = raw(message, mapping.wire, mapping.index);
        bool valid = validRaw(value, mapping.invalid);
        if ((message.msgid == MAVLINK_MSG_ID_GPS_RAW_INT || message.msgid == MAVLINK_MSG_ID_GPS2_RAW)
            && (mapping.wire == "lat" || mapping.wire == "lon" || mapping.wire == "alt"
                || mapping.wire == "vel" || mapping.wire == "cog"))
            valid = valid && raw(message,"fix_type") >= 2;
        if (message.msgid == MAVLINK_MSG_ID_RC_CHANNELS) {
            const int channel = mapping.wire.mid(4).section('_',0,0).toInt();
            valid = valid && channel <= raw(message, "chancount");
        }
        // Prefer the fused global position while fresh, as CurrentState does.
        if (message.msgid == MAVLINK_MSG_ID_GPS_RAW_INT
            && (mapping.name == "lat" || mapping.name == "lng" || mapping.name == "altasl")) {
            const auto fused = m_samples.constFind("_fusedPosition");
            if (fused != m_samples.cend() && freshAt(at,fused->at)) continue;
        }
        value *= mapping.scale;
        if (mapping.name == "yaw" && value < 0) value += 360;
        if (mapping.name == "ch3percent") {
            const auto reverse=m_samples.constFind("_reverseThrottle");
            if (reverse!=m_samples.cend() && freshAt(at,reverse->at) && reverse->value!=0) value=-value;
        }
        put(mapping.name, value, at, valid);
    }
    if (message.msgid == MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
        put("_fusedPosition", 1, at, validRaw(raw(message,"lat"),CoordinateLat) && validRaw(raw(message,"lon"),CoordinateLon));
        const double altitude=raw(message,"relative_alt")*.001;
        const auto previous=m_samples.constFind("_altReference");
        if (previous!=m_samples.cend() && at>previous->at && freshAt(at,previous->at)) {
            if (at-previous->at>=200) {
                double rate=(altitude-previous->value)*1000.0/(at-previous->at);
                const auto oldRate=m_samples.constFind("verticalspeed");
                if (oldRate!=m_samples.cend() && freshAt(at,oldRate->at)) rate=oldRate->value*.4+rate*.6;
                put("verticalspeed",rate,at); put("_altReference",altitude,at);
            }
        } else { m_samples.remove("verticalspeed"); put("_altReference",altitude,at); }
    }
    if (message.msgid == MAVLINK_MSG_ID_HOME_POSITION) {
        const double lat=raw(message,"latitude"), lon=raw(message,"longitude"), alt=raw(message,"altitude");
        m_homeValid = validRaw(lat,CoordinateLat) && validRaw(lon,CoordinateLon) && std::isfinite(alt);
        if (m_homeValid) { m_homeLatitude=lat*1e-7; m_homeLongitude=lon*1e-7; m_homeAltitude=alt*.001; }
    }
    if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        const auto status = raw(message,"system_status");
        put("armed", (quint8(raw(message,"base_mode")) & MAV_MODE_FLAG_SAFETY_ARMED) != 0, at);
        put("failsafe", status == MAV_STATE_CRITICAL, at);
        put("landed", status == MAV_STATE_STANDBY, at);
        put("connected", 1, at);
    }
    if (message.msgid == MAVLINK_MSG_ID_SYS_STATUS) {
        const quint32 enabled = quint32(raw(message,"onboard_control_sensors_enabled"));
        const quint32 health = quint32(raw(message,"onboard_control_sensors_health"));
        const quint32 present = quint32(raw(message,"onboard_control_sensors_present"));
        put("safetyactive", !(enabled & MAV_SYS_STATUS_SENSOR_MOTOR_OUTPUTS), at);
        put("terrainactive", bool(enabled & health & present & MAV_SYS_STATUS_TERRAIN), at);
        put("prearmstatus", !(enabled & MAV_SYS_STATUS_PREARM_CHECK) || (health & MAV_SYS_STATUS_PREARM_CHECK), at);
        put("_reverseThrottle",bool(enabled & health & present & MAV_SYS_STATUS_REVERSE_MOTOR),at);
    }
    if (message.msgid == MAVLINK_MSG_ID_RC_CHANNELS || message.msgid == MAVLINK_MSG_ID_RC_CHANNELS_RAW) {
        const double rssi = raw(message,"rssi");
        put("rxrssi", std::floor(rssi/(message.msgid==MAVLINK_MSG_ID_RC_CHANNELS ? 254.0 : 255.0)*100), at, validRaw(rssi,U8Max));
    }
    if (message.msgid == MAVLINK_MSG_ID_BATTERY_STATUS) {
        const int id = int(raw(message,"id"));
        if (id >= 0 && id < 9) {
            double voltage = 0; bool found = false;
            for (int n=0; n<14; ++n) {
                const double cell = raw(message,n<10 ? "voltages" : "voltages_ext",n<10 ? n : n-10);
                if (validRaw(cell,n<10 ? U16Max : Zero)) { voltage += cell*.001; found=true; }
                // MAVLink battery extension zero means unsupported, not 0 V.
                if (id==0 && n>=10 && cell==0) m_samples.remove(QString("battery_cell%1").arg(n+1));
            }
            put("battery_voltage"+(id==0 ? QString() : QString::number(id+1)),voltage,at,found);
        }
    }
    if (message.msgid == MAVLINK_MSG_ID_MEMINFO) {
        const double wide = raw(message,"freemem32");
        if (std::isfinite(wide) && wide > 0) put("freemem",wide,at);
    }
}
