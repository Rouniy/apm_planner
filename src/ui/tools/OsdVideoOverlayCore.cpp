#include "OsdVideoOverlayCore.h"

#include "comm/TlogReader.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
constexpr qint64 kBucketUsec = 100000;
constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;

qint64 saturatingAdd(qint64 left, qint64 right)
{
    if (right > 0 && left > std::numeric_limits<qint64>::max() - right) {
        return std::numeric_limits<qint64>::max();
    }
    if (right < 0 && left < std::numeric_limits<qint64>::min() - right) {
        return std::numeric_limits<qint64>::min();
    }
    return left + right;
}

double finiteOrZero(double value)
{
    return std::isfinite(value) ? value : 0.0;
}

int validPercent(int value)
{
    return value >= 0 && value <= 100 ? value : 0;
}

double normalizedDirection(double value)
{
    value = std::fmod(value, 360.0);
    return value < 0.0 ? value + 360.0 : value;
}

QString modeText(const mavlink_heartbeat_t &heartbeat)
{
    if ((heartbeat.base_mode & MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) == 0) {
        if ((heartbeat.base_mode & MAV_MODE_FLAG_AUTO_ENABLED) != 0) {
            return QStringLiteral("AUTO");
        }
        if ((heartbeat.base_mode & MAV_MODE_FLAG_GUIDED_ENABLED) != 0) {
            return QStringLiteral("GUIDED");
        }
        if ((heartbeat.base_mode & MAV_MODE_FLAG_STABILIZE_ENABLED) != 0) {
            return QStringLiteral("STABILIZE");
        }
        return QStringLiteral("—");
    }

    // Mission Planner displays the firmware-specific label. Cover the common
    // ArduCopter modes here and retain an honest numeric label for every
    // dialect/type not represented by this isolated log reader.
    if (heartbeat.type == MAV_TYPE_QUADROTOR
        || heartbeat.type == MAV_TYPE_HEXAROTOR
        || heartbeat.type == MAV_TYPE_OCTOROTOR
        || heartbeat.type == MAV_TYPE_TRICOPTER
        || heartbeat.type == MAV_TYPE_HELICOPTER
        || heartbeat.type == MAV_TYPE_COAXIAL) {
        static const QHash<quint32, QString> names = {
            {0, QStringLiteral("STABILIZE")}, {1, QStringLiteral("ACRO")},
            {2, QStringLiteral("ALT_HOLD")}, {3, QStringLiteral("AUTO")},
            {4, QStringLiteral("GUIDED")}, {5, QStringLiteral("LOITER")},
            {6, QStringLiteral("RTL")}, {7, QStringLiteral("CIRCLE")},
            {9, QStringLiteral("LAND")}, {11, QStringLiteral("DRIFT")},
            {13, QStringLiteral("SPORT")}, {14, QStringLiteral("FLIP")},
            {15, QStringLiteral("AUTOTUNE")}, {16, QStringLiteral("POSHOLD")},
            {17, QStringLiteral("BRAKE")}, {18, QStringLiteral("THROW")},
            {19, QStringLiteral("AVOID_ADSB")},
            {20, QStringLiteral("GUIDED_NOGPS")},
            {21, QStringLiteral("SMART_RTL")}, {23, QStringLiteral("FOLLOW")},
            {24, QStringLiteral("ZIGZAG")}, {25, QStringLiteral("SYSTEMID")},
            {26, QStringLiteral("AUTOROTATE")}, {27, QStringLiteral("AUTO_RTL")}
        };
        const auto found = names.constFind(heartbeat.custom_mode);
        if (found != names.cend()) {
            return found.value();
        }
    }
    return QStringLiteral("Mode %1").arg(heartbeat.custom_mode);
}

class TelemetryAccumulator
{
public:
    bool hasVehicle() const { return m_systemId >= 0; }

    void observe(const mavlink_message_t &message)
    {
        // Select one autopilot epoch before accepting state. A companion or
        // stale packet at the beginning of a multi-system tlog must not seed
        // the exported HUD and then become mixed with the first vehicle.
        if (m_systemId < 0 && message.msgid != MAVLINK_MSG_ID_HEARTBEAT) {
            return;
        }
        if (m_systemId >= 0 && message.sysid != m_systemId) {
            return;
        }

        switch (message.msgid) {
        case MAVLINK_MSG_ID_HEARTBEAT: {
            mavlink_heartbeat_t value{};
            mavlink_msg_heartbeat_decode(&message, &value);
            if (value.type == MAV_TYPE_GCS
                || message.compid != MAV_COMP_ID_AUTOPILOT1) {
                return;
            }
            if (m_systemId < 0) {
                m_systemId = message.sysid;
            }
            m_sample.armed =
                (value.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
            m_sample.failsafe = value.system_status == MAV_STATE_CRITICAL
                || value.system_status == MAV_STATE_EMERGENCY;
            m_sample.mode = modeText(value);
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_ATTITUDE: {
            mavlink_attitude_t value{};
            mavlink_msg_attitude_decode(&message, &value);
            m_sample.roll = finiteOrZero(value.roll * kRadiansToDegrees);
            m_sample.pitch = finiteOrZero(value.pitch * kRadiansToDegrees);
            m_sample.yaw = finiteOrZero(value.yaw * kRadiansToDegrees);
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_VFR_HUD: {
            mavlink_vfr_hud_t value{};
            mavlink_msg_vfr_hud_decode(&message, &value);
            m_sample.airSpeed = finiteOrZero(value.airspeed);
            m_sample.groundSpeed = finiteOrZero(value.groundspeed);
            m_sample.verticalSpeed = finiteOrZero(value.climb);
            m_sample.throttlePercent = value.throttle;
            m_gotVfr = true;
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
            mavlink_global_position_int_t value{};
            mavlink_msg_global_position_int_decode(&message, &value);
            m_sample.altitude = value.relative_alt / 1000.0;
            if (!m_gotVfr) {
                m_sample.groundSpeed =
                    std::hypot(double(value.vx), double(value.vy)) / 100.0;
                m_sample.verticalSpeed = -value.vz / 100.0;
            }
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_GPS_RAW_INT: {
            mavlink_gps_raw_int_t value{};
            mavlink_msg_gps_raw_int_decode(&message, &value);
            m_sample.gpsFixType = value.fix_type;
            if (value.satellites_visible
                != std::numeric_limits<quint8>::max()) {
                m_sample.satelliteCount = value.satellites_visible;
            }
            if (!m_gotVfr
                && value.vel != std::numeric_limits<quint16>::max()) {
                m_sample.groundSpeed = value.vel / 100.0;
            }
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_SYS_STATUS: {
            mavlink_sys_status_t value{};
            mavlink_msg_sys_status_decode(&message, &value);
            if (value.voltage_battery
                != std::numeric_limits<quint16>::max()) {
                m_sample.batteryVoltage = value.voltage_battery / 1000.0;
            }
            if (value.current_battery != -1) {
                m_sample.currentAmps = value.current_battery / 100.0;
            }
            m_sample.batteryRemaining = validPercent(value.battery_remaining);
            const quint32 prearm = MAV_SYS_STATUS_PREARM_CHECK;
            m_sample.prearmOk = (value.onboard_control_sensors_health & prearm) != 0
                || (value.onboard_control_sensors_enabled & prearm) == 0;
            m_sample.safetyActive =
                (value.onboard_control_sensors_enabled
                 & MAV_SYS_STATUS_SENSOR_MOTOR_OUTPUTS) == 0;
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_BATTERY_STATUS: {
            mavlink_battery_status_t value{};
            mavlink_msg_battery_status_decode(&message, &value);
            // mavlink_battery_status_t is packed. Read each element by value
            // instead of passing its potentially unaligned arrays by pointer.
            double voltage = 0.0;
            for (int index = 0;
                 index < MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_LEN;
                 ++index) {
                const quint16 cell = value.voltages[index];
                if (cell != std::numeric_limits<quint16>::max()) {
                    voltage += cell / 1000.0;
                }
            }
            for (int index = 0;
                 index < MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_EXT_LEN;
                 ++index) {
                const quint16 cell = value.voltages_ext[index];
                if (cell != std::numeric_limits<quint16>::max()) {
                    voltage += cell / 1000.0;
                }
            }
            const double current = value.current_battery == -1
                ? 0.0 : value.current_battery / 100.0;
            if (value.id == 0) {
                if (voltage > 0.0) m_sample.batteryVoltage = voltage;
                if (value.current_battery != -1) m_sample.currentAmps = current;
                m_sample.batteryRemaining = validPercent(value.battery_remaining);
            } else if (value.id == 1) {
                if (voltage > 0.0) m_sample.batteryVoltage2 = voltage;
                if (value.current_battery != -1) m_sample.currentAmps2 = current;
                m_sample.batteryRemaining2 = validPercent(value.battery_remaining);
            }
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_BATTERY2: {
            mavlink_battery2_t value{};
            mavlink_msg_battery2_decode(&message, &value);
            if (value.voltage
                != std::numeric_limits<quint16>::max()) {
                m_sample.batteryVoltage2 = value.voltage / 1000.0;
            }
            if (value.current_battery != -1) {
                m_sample.currentAmps2 = value.current_battery / 100.0;
            }
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_NAV_CONTROLLER_OUTPUT: {
            mavlink_nav_controller_output_t value{};
            mavlink_msg_nav_controller_output_decode(&message, &value);
            m_sample.navBearing = value.nav_bearing;
            m_sample.xtrackError = finiteOrZero(value.xtrack_error);
            m_sample.waypointDistance = value.wp_dist;
            // CurrentState derives these display targets from the controller
            // errors and smooths each new target equally with the old value.
            m_sample.targetAltitude = m_sample.targetAltitude * 0.5
                + std::round(m_sample.altitude + value.alt_error) * 0.5;
            const double airspeedError = value.aspd_error / 100.0;
            m_sample.targetSpeed = m_sample.targetSpeed * 0.5
                + std::round(m_sample.airSpeed + airspeedError) * 0.5;
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_WIND: {
            mavlink_wind_t value{};
            mavlink_msg_wind_decode(&message, &value);
            m_sample.windDirection = normalizedDirection(value.direction);
            m_sample.windSpeed = finiteOrZero(value.speed);
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_AOA_SSA: {
            mavlink_aoa_ssa_t value{};
            mavlink_msg_aoa_ssa_decode(&message, &value);
            m_sample.aoa = finiteOrZero(value.AOA);
            m_sample.ssa = finiteOrZero(value.SSA);
            updateSequence(message);
            break;
        }
        case MAVLINK_MSG_ID_MISSION_CURRENT: {
            mavlink_mission_current_t value{};
            mavlink_msg_mission_current_decode(&message, &value);
            m_sample.waypointNumber = value.seq;
            updateSequence(message);
            break;
        }
        default:
            updateSequence(message);
            break;
        }
    }

    OsdVideoTelemetrySample snapshot(qint64 timestampUsec) const
    {
        OsdVideoTelemetrySample result = m_sample;
        result.timestampUsec = timestampUsec;
        result.turnRate = result.groundSpeed > 1.0
            ? result.roll * 9.80665 / result.groundSpeed : 0.0;
        return result;
    }

private:
    void updateSequence(const mavlink_message_t &message)
    {
        if (!m_haveSequence) {
            m_haveSequence = true;
            m_lastSequence = message.seq;
            m_receivedPackets = 1;
            m_sample.linkQuality = 100.0;
            return;
        }
        const int missing = (int(message.seq) - int(m_lastSequence) - 1 + 256)
            % 256;
        m_lastSequence = message.seq;
        ++m_receivedPackets;
        m_lostPackets += missing;
        const quint64 total = m_receivedPackets + m_lostPackets;
        m_sample.linkQuality = total > 0
            ? 100.0 * m_receivedPackets / total : 0.0;
    }

    OsdVideoTelemetrySample m_sample;
    int m_systemId = -1;
    bool m_gotVfr = false;
    bool m_haveSequence = false;
    quint8 m_lastSequence = 0;
    quint64 m_receivedPackets = 0;
    quint64 m_lostPackets = 0;
};

QString pathIdentity(const QFileInfo &file)
{
    QString path = file.exists() ? file.canonicalFilePath()
                                 : file.absoluteFilePath();
    path = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return path;
}
}

OsdVideoTelemetryTimeline::OsdVideoTelemetryTimeline(
    QVector<OsdVideoTelemetrySample> samples)
    : m_samples(std::move(samples))
{
    std::stable_sort(m_samples.begin(), m_samples.end(),
                     [](const OsdVideoTelemetrySample &left,
                        const OsdVideoTelemetrySample &right) {
        return left.timestampUsec < right.timestampUsec;
    });
    QVector<OsdVideoTelemetrySample> unique;
    unique.reserve(m_samples.size());
    for (const OsdVideoTelemetrySample &sample : m_samples) {
        if (!unique.isEmpty()
            && unique.last().timestampUsec == sample.timestampUsec) {
            unique.last() = sample;
        } else {
            unique.append(sample);
        }
    }
    m_samples = std::move(unique);
}

OsdVideoTelemetryTimeline OsdVideoTelemetryTimeline::Load(
    QIODevice *device, LoadStatus *status, QString *errorMessage,
    const CancelRequested &cancel, const Progress &progress)
{
    if (status) *status = LoadStatus::Error;
    if (errorMessage) errorMessage->clear();

    TlogReader reader(device);
    reader.setCancelCheck(cancel);
    reader.setProgress(progress);
    if (reader.status() == TlogReader::Status::Error) {
        if (errorMessage) *errorMessage = reader.errorString();
        return OsdVideoTelemetryTimeline();
    }

    TelemetryAccumulator accumulator;
    QVector<OsdVideoTelemetrySample> samples;
    TlogRecord record;
    TlogReader::Status readerStatus = TlogReader::Status::Ok;
    while ((readerStatus = reader.next(&record)) == TlogReader::Status::Ok) {
        accumulator.observe(record.message);
        if (!accumulator.hasVehicle()) {
            continue;
        }
        const qint64 bucket = BucketStartUsec(record.timestampUsec);
        const OsdVideoTelemetrySample sample = accumulator.snapshot(bucket);
        if (!samples.isEmpty() && samples.last().timestampUsec == bucket) {
            samples.last() = sample;
        } else {
            samples.append(sample);
        }
    }

    if (readerStatus == TlogReader::Status::Cancelled) {
        if (status) *status = LoadStatus::Cancelled;
        return OsdVideoTelemetryTimeline();
    }
    if (readerStatus != TlogReader::Status::End) {
        if (errorMessage) {
            *errorMessage = reader.errorString().isEmpty()
                ? QStringLiteral("The telemetry log is truncated or unreadable.")
                : reader.errorString();
        }
        return OsdVideoTelemetryTimeline();
    }
    if (samples.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The telemetry log contains no readable MAVLink state.");
        }
        return OsdVideoTelemetryTimeline();
    }
    if (status) *status = LoadStatus::Ok;
    return OsdVideoTelemetryTimeline(std::move(samples));
}

qint64 OsdVideoTelemetryTimeline::startTimeUsec() const
{
    return m_samples.isEmpty() ? 0 : m_samples.first().timestampUsec;
}

qint64 OsdVideoTelemetryTimeline::endTimeUsec() const
{
    return m_samples.isEmpty() ? 0 : m_samples.last().timestampUsec;
}

OsdVideoTelemetrySample OsdVideoTelemetryTimeline::sampleAt(
    qint64 videoPositionUsec, qint64 offsetUsec) const
{
    if (m_samples.isEmpty()) {
        return {};
    }
    videoPositionUsec = std::max<qint64>(0, videoPositionUsec);
    const qint64 target = saturatingAdd(
        saturatingAdd(startTimeUsec(), videoPositionUsec), offsetUsec);
    const auto found = std::upper_bound(
        m_samples.cbegin(), m_samples.cend(), target,
        [](qint64 value, const OsdVideoTelemetrySample &sample) {
            return value < sample.timestampUsec;
        });
    if (found == m_samples.cbegin()) {
        return m_samples.first();
    }
    return *(found - 1);
}

qint64 OsdVideoTelemetryTimeline::BucketStartUsec(qint64 timestampUsec)
{
    qint64 remainder = timestampUsec % kBucketUsec;
    if (remainder < 0) remainder += kBucketUsec;
    return timestampUsec - remainder;
}

QString OsdVideoOverlayCore::Validate(const OsdVideoExportOptions &options)
{
    if (options.videoPath.trimmed().isEmpty()) {
        return QStringLiteral("Choose a source video.");
    }
    if (options.tlogPath.trimmed().isEmpty()) {
        return QStringLiteral("Choose a telemetry log.");
    }
    if (options.outputPath.trimmed().isEmpty()) {
        return QStringLiteral("Choose an output AVI.");
    }

    const QFileInfo video(options.videoPath);
    const QFileInfo tlog(options.tlogPath);
    const QFileInfo output(options.outputPath);
    if (!video.exists() || !video.isFile()) {
        return QStringLiteral("The source video does not exist.");
    }
    if (!tlog.exists() || !tlog.isFile()) {
        return QStringLiteral("The telemetry log does not exist.");
    }
    if (tlog.suffix().compare(QStringLiteral("tlog"),
                              Qt::CaseInsensitive) != 0) {
        return QStringLiteral(
            "OSD video synchronization requires a .tlog telemetry log.");
    }
    const QString outputIdentity = pathIdentity(output);
    if (outputIdentity == pathIdentity(video)
        || outputIdentity == pathIdentity(tlog)) {
        return QStringLiteral(
            "The output path must differ from both input files.");
    }
    if (output.suffix().compare(QStringLiteral("avi"),
                                Qt::CaseInsensitive) != 0) {
        return QStringLiteral("The OSD video output must use the .avi extension.");
    }
    if (output.exists()) {
        return QStringLiteral(
            "The output file already exists. Choose a new file name.");
    }
    if (options.timeOffsetSeconds < MinimumOffsetSeconds
        || options.timeOffsetSeconds > MaximumOffsetSeconds) {
        return QStringLiteral("The time offset must be between %1 and %2 seconds.")
            .arg(MinimumOffsetSeconds).arg(MaximumOffsetSeconds);
    }
    if (options.previewWidth < 160 || options.previewWidth > 8192) {
        return QStringLiteral("The preview width is invalid.");
    }
    if (options.jpegQuality < 1 || options.jpegQuality > 100) {
        return QStringLiteral("JPEG quality must be between 1 and 100.");
    }
    return {};
}

QString OsdVideoOverlayCore::DefaultOutputPath(const QString &videoPath)
{
    if (videoPath.trimmed().isEmpty()) {
        return {};
    }
    const QFileInfo input(videoPath);
    const QString directory = input.absolutePath();
    const QString stem = input.completeBaseName() + QStringLiteral("-overlay");
    QString candidate = QDir(directory).filePath(stem + QStringLiteral(".avi"));
    for (int suffix = 2; QFileInfo::exists(candidate); ++suffix) {
        candidate = QDir(directory).filePath(
            QStringLiteral("%1-%2.avi").arg(stem).arg(suffix));
    }
    return QDir::cleanPath(candidate);
}

QSize OsdVideoOverlayCore::OutputSize(int sourceWidth, int sourceHeight,
                                      bool fullResolution, int previewWidth)
{
    if (sourceWidth < 1 || sourceHeight < 1 || previewWidth < 1) {
        return {};
    }
    if (fullResolution || sourceWidth <= previewWidth) {
        return QSize(sourceWidth, sourceHeight);
    }
    // MP10 truncates the scaled height, then the MJPEG path requires an even
    // scanline count.
    int height = std::max(
        2, int(double(sourceHeight) * previewWidth / sourceWidth));
    if ((height & 1) != 0) ++height;
    return QSize(previewWidth, height);
}

int OsdVideoOverlayCore::ClampFramesPerSecond(double framesPerSecond)
{
    if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0) {
        return DefaultFramesPerSecond;
    }
    return std::max(1, std::min(120, int(std::lround(framesPerSecond))));
}

qint64 OsdVideoOverlayCore::FrameIndexForTimestamp(
    qint64 timestampUsec, int framesPerSecond)
{
    if (timestampUsec <= 0) {
        return 0;
    }
    framesPerSecond = ClampFramesPerSecond(framesPerSecond);
    constexpr qint64 usecPerSecond = 1000000;
    const qint64 wholeSeconds = timestampUsec / usecPerSecond;
    const qint64 remainderUsec = timestampUsec % usecPerSecond;
    const qint64 roundedRemainder =
        (remainderUsec * framesPerSecond + usecPerSecond / 2)
        / usecPerSecond;
    if (wholeSeconds
        > (std::numeric_limits<qint64>::max() - roundedRemainder)
            / framesPerSecond) {
        return std::numeric_limits<qint64>::max();
    }
    return wholeSeconds * framesPerSecond + roundedRemainder;
}
