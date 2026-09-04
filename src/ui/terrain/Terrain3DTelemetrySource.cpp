#include "Terrain3DTelemetrySource.h"

#include "comm/VehicleTargetManager.h"

#include <cmath>
#include <utility>

namespace {
constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;

double normalizedHeading(double degrees)
{
    degrees = std::fmod(degrees, 360.0);
    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

bool validPosition(double latitude, double longitude, double altitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && std::isfinite(altitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0
        && (std::abs(latitude) > 1.0e-9
            || std::abs(longitude) > 1.0e-9);
}
}

Terrain3DTelemetrySource::Terrain3DTelemetrySource(
    VehicleTargetManager *targets, Clock clock,
    ModeFormatter modeFormatter, QObject *parent)
    : QObject(parent)
    , m_targets(targets)
    , m_clock(std::move(clock))
    , m_modeFormatter(std::move(modeFormatter))
{
    m_elapsedClock.start();
    if (!m_targets) {
        return;
    }
    connect(m_targets, &VehicleTargetManager::targetGenerationChanged,
            this, &Terrain3DTelemetrySource::invalidateTargetEpoch);
    connect(m_targets, &VehicleTargetManager::targetGenerationSettled,
            this, &Terrain3DTelemetrySource::resetTargetEpoch);
    resetTargetEpoch();
}

qint64 Terrain3DTelemetrySource::monotonicTimeMs() const
{
    return m_clock ? m_clock() : m_elapsedClock.elapsed();
}

bool Terrain3DTelemetrySource::accepts(
    int linkId, const mavlink_message_t &message) const
{
    const VehicleTargetLease &lease = m_snapshot.lease;
    return m_targets && lease.isValid()
        && linkId == lease.endpoint.linkId
        && message.sysid == lease.endpoint.systemId
        && message.compid == lease.endpoint.componentId
        && m_targets->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation);
}

bool Terrain3DTelemetrySource::isCurrentLease(
    const VehicleTargetLease &lease) const
{
    return m_targets && lease.isValid() && m_snapshot.lease.isValid()
        && lease.generation == m_snapshot.lease.generation
        && lease.endpoint.sameIdentity(m_snapshot.lease.endpoint)
        && m_targets->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation);
}

void Terrain3DTelemetrySource::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (!accepts(linkId, message)) {
        return;
    }
    const VehicleTargetLease capturedLease = m_snapshot.lease;
    Terrain3DTelemetrySnapshot next = m_snapshot;
    bool changed = false;

    switch (message.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
        mavlink_heartbeat_t heartbeat{};
        mavlink_msg_heartbeat_decode(&message, &heartbeat);
        const bool armed =
            (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
        const QString mode = m_modeFormatter
            ? m_modeFormatter(heartbeat.autopilot, heartbeat.type,
                              heartbeat.custom_mode, heartbeat.base_mode)
            : fallbackModeText(heartbeat.autopilot, heartbeat.type,
                               heartbeat.custom_mode, heartbeat.base_mode);
        changed = !next.heartbeatValid
            || next.armed != armed || next.mode != mode;
        next.heartbeatValid = true;
        next.armed = armed;
        next.mode = mode;
        break;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t position{};
        mavlink_msg_global_position_int_decode(&message, &position);
        const double latitude = position.lat / 1.0e7;
        const double longitude = position.lon / 1.0e7;
        const double altitude = position.alt / 1000.0;
        const double relativeAltitude = position.relative_alt / 1000.0;
        const bool valid = validPosition(latitude, longitude, altitude)
            && std::isfinite(relativeAltitude);
        changed = next.positionValid != valid;
        if (valid) {
            changed = changed || next.latitude != latitude
                || next.longitude != longitude
                || next.altitudeAmslM != altitude
                || next.altitudeRelativeM != relativeAltitude
                || next.velocityNorthMps != position.vx / 100.0
                || next.velocityEastMps != position.vy / 100.0
                || next.velocityUpMps != -position.vz / 100.0;
            next.latitude = latitude;
            next.longitude = longitude;
            next.altitudeAmslM = altitude;
            next.altitudeRelativeM = relativeAltitude;
            next.velocityNorthMps = position.vx / 100.0;
            next.velocityEastMps = position.vy / 100.0;
            next.velocityUpMps = -position.vz / 100.0;
            const qint64 observedMs = monotonicTimeMs();
            changed = changed || next.positionObservedMs != observedMs;
            next.positionObservedMs = observedMs;
        } else {
            next.positionObservedMs = -1;
        }
        next.positionValid = valid;
        break;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
        mavlink_attitude_t attitude{};
        mavlink_msg_attitude_decode(&message, &attitude);
        const double roll = attitude.roll * kRadiansToDegrees;
        const double pitch = attitude.pitch * kRadiansToDegrees;
        const double yaw = normalizedHeading(
            attitude.yaw * kRadiansToDegrees);
        const bool valid = std::isfinite(roll) && std::isfinite(pitch)
            && std::isfinite(yaw);
        changed = next.attitudeValid != valid;
        if (valid) {
            changed = changed || next.rollDeg != roll
                || next.pitchDeg != pitch
                || next.yawDeg != yaw;
            next.rollDeg = roll;
            next.pitchDeg = pitch;
            next.yawDeg = yaw;
        }
        next.attitudeValid = valid;
        break;
    }
    default:
        return;
    }

    // External clocks/formatters and snapshot listeners may synchronously
    // select another vehicle. Publish the value copy only while the captured
    // physical-target epoch is still authoritative.
    if (changed && isCurrentLease(capturedLease)) {
        m_snapshot = std::move(next);
        emit snapshotChanged();
    }
}

void Terrain3DTelemetrySource::invalidateTargetEpoch()
{
    m_snapshot = Terrain3DTelemetrySnapshot{};
    emit snapshotChanged();
}

void Terrain3DTelemetrySource::resetTargetEpoch()
{
    Terrain3DTelemetrySnapshot next;
    if (m_targets) {
        next.lease = m_targets->acquireTarget();
    }
    m_snapshot = next;
    emit snapshotChanged();
    emit targetEpochChanged();
}

QString Terrain3DTelemetrySource::fallbackModeText(
    int autopilot, int vehicleType, quint32 customMode, quint8 baseMode)
{
    Q_UNUSED(autopilot)
    Q_UNUSED(vehicleType)
    if (baseMode & MAV_MODE_FLAG_AUTO_ENABLED) {
        return QStringLiteral("Auto");
    }
    if (baseMode & MAV_MODE_FLAG_GUIDED_ENABLED) {
        return QStringLiteral("Guided");
    }
    if (baseMode & MAV_MODE_FLAG_STABILIZE_ENABLED) {
        return QStringLiteral("Stabilize");
    }
    if (baseMode & MAV_MODE_FLAG_MANUAL_INPUT_ENABLED) {
        return QStringLiteral("Manual");
    }
    return QStringLiteral("Mode %1").arg(customMode);
}
