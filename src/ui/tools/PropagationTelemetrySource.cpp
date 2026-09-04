#include "PropagationTelemetrySource.h"

#include "comm/VehicleTargetManager.h"

#include <QtGlobal>

#include <cmath>
#include <limits>
#include <utility>

namespace
{
double batteryRemaining(int value)
{
    return value >= 0 && value <= 100
        ? double(value) : std::numeric_limits<double>::quiet_NaN();
}

double batteryCurrentAmps(int valueCentiAmps)
{
    return valueCentiAmps == -1
        ? std::numeric_limits<double>::quiet_NaN()
        : valueCentiAmps / 100.0;
}

double consumedMilliampHours(qint32 value)
{
    return value >= 0
        ? double(value) : std::numeric_limits<double>::quiet_NaN();
}
}

PropagationTelemetrySource::PropagationTelemetrySource(
    VehicleTargetManager *targets, Clock clock, QObject *parent)
    : QObject(parent)
    , m_targets(targets)
    , m_clock(std::move(clock))
{
    m_elapsedClock.start();
    if (m_targets) {
        connect(m_targets, &VehicleTargetManager::targetGenerationChanged,
                this, &PropagationTelemetrySource::invalidateTargetEpoch);
        connect(m_targets, &VehicleTargetManager::targetGenerationSettled,
                this, &PropagationTelemetrySource::resetTargetEpoch);
    }
    resetTargetEpoch();
}

qint64 PropagationTelemetrySource::monotonicTimeMs() const
{
    return m_clock ? m_clock() : m_elapsedClock.elapsed();
}

bool PropagationTelemetrySource::accepts(
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

bool PropagationTelemetrySource::isCurrentLease(
    const VehicleTargetLease &lease) const
{
    return m_targets && lease.isValid() && m_snapshot.lease.isValid()
        && lease.generation == m_snapshot.lease.generation
        && lease.endpoint.sameIdentity(m_snapshot.lease.endpoint)
        && m_targets->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation);
}

bool PropagationTelemetrySource::capturedEpochIsCurrent(
    const VehicleTargetLease &lease, quint64 telemetryEpoch) const
{
    return telemetryEpoch == m_snapshot.telemetryEpoch
        && isCurrentLease(lease);
}

void PropagationTelemetrySource::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (!accepts(linkId, message)) {
        return;
    }

    const VehicleTargetLease capturedLease = m_snapshot.lease;
    const quint64 capturedEpoch = m_snapshot.telemetryEpoch;
    const qint64 now = monotonicTimeMs();
    // An injected clock or a synchronous target callback may replace the
    // physical target. Never mutate the new epoch with the old message.
    if (!capturedEpochIsCurrent(capturedLease, capturedEpoch)) {
        return;
    }

    PropagationTelemetrySnapshot next = m_snapshot;
    switch (message.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
        mavlink_heartbeat_t heartbeat{};
        mavlink_msg_heartbeat_decode(&message, &heartbeat);
        next.heartbeatValid = true;
        next.armed =
            (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
        next.heartbeatObservedMs = now;
        m_distanceEstimator.setConnected(true);
        m_distanceEstimator.setArmed(next.armed);
        break;
    }
    case MAVLINK_MSG_ID_HOME_POSITION: {
        mavlink_home_position_t home{};
        mavlink_msg_home_position_decode(&message, &home);
        const double latitude = home.latitude / 1.0e7;
        const double longitude = home.longitude / 1.0e7;
        const double altitude = home.altitude / 1000.0;
        next.homeValid = validCoordinate(latitude, longitude, altitude);
        next.homeObservedMs = next.homeValid ? now : -1;
        if (next.homeValid) {
            next.homeLatitude = latitude;
            next.homeLongitude = longitude;
            next.homeAltitudeAmslM = altitude;
        } else {
            next.homeLatitude = 0.0;
            next.homeLongitude = 0.0;
            next.homeAltitudeAmslM = 0.0;
        }
        break;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t position{};
        mavlink_msg_global_position_int_decode(&message, &position);
        const double latitude = position.lat / 1.0e7;
        const double longitude = position.lon / 1.0e7;
        const double altitude = position.alt / 1000.0;
        next.positionValid = validCoordinate(
            latitude, longitude, altitude);
        next.positionObservedMs = next.positionValid ? now : -1;
        if (next.positionValid) {
            next.latitude = latitude;
            next.longitude = longitude;
            next.altitudeAmsl = altitude;
        } else {
            next.latitude = 0.0;
            next.longitude = 0.0;
            next.altitudeAmsl = 0.0;
        }
        m_distanceEstimator.observePosition(now, latitude, longitude);
        break;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        mavlink_gps_raw_int_t gps{};
        mavlink_msg_gps_raw_int_decode(&message, &gps);
        next.gpsFixType = gps.fix_type;
        next.gpsObservedMs = now;
        m_distanceEstimator.setGpsFixType(next.gpsFixType);
        break;
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
        mavlink_sys_status_t status{};
        mavlink_msg_sys_status_decode(&message, &status);
        m_distanceEstimator.observeBattery(
            now, batteryRemaining(status.battery_remaining),
            batteryCurrentAmps(status.current_battery),
            std::numeric_limits<double>::quiet_NaN());
        next.batteryObservedMs = now;
        break;
    }
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
        mavlink_battery_status_t status{};
        mavlink_msg_battery_status_decode(&message, &status);
        if (status.id != 0) {
            return;
        }
        m_distanceEstimator.observeBattery(
            now, batteryRemaining(status.battery_remaining),
            batteryCurrentAmps(status.current_battery),
            consumedMilliampHours(status.current_consumed));
        next.batteryObservedMs = now;
        break;
    }
    default:
        return;
    }

    copyBatteryEstimate(&next);
    if (!capturedEpochIsCurrent(capturedLease, capturedEpoch)) {
        return;
    }
    m_snapshot = std::move(next);
    emit snapshotChanged();
}

void PropagationTelemetrySource::observeLinkDisconnected(int linkId)
{
    if (!m_snapshot.lease.isValid()
        || m_snapshot.lease.endpoint.linkId != linkId) {
        return;
    }
    publishFreshEpoch(m_snapshot.lease);
}

void PropagationTelemetrySource::invalidateTargetEpoch()
{
    publishFreshEpoch({});
}

void PropagationTelemetrySource::resetTargetEpoch()
{
    publishFreshEpoch(
        m_targets ? m_targets->acquireTarget() : VehicleTargetLease{});
}

void PropagationTelemetrySource::publishFreshEpoch(
    const VehicleTargetLease &lease)
{
    m_distanceEstimator.reset();
    PropagationTelemetrySnapshot next;
    next.lease = lease;
    next.telemetryEpoch = ++m_epochCounter;
    next.vehicleIdentity = lease.isValid() ? lease.endpoint.systemId : -1;
    m_snapshot = next;
    emit telemetryEpochChanged(m_snapshot.telemetryEpoch);
    emit snapshotChanged();
}

bool PropagationTelemetrySource::validCoordinate(
    double latitude, double longitude, double altitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && std::isfinite(altitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0
        && (latitude != 0.0 || longitude != 0.0);
}

void PropagationTelemetrySource::copyBatteryEstimate(
    PropagationTelemetrySnapshot *snapshot) const
{
    if (!snapshot) {
        return;
    }
    snapshot->batteryRemainingValid =
        m_distanceEstimator.batteryRemainingValid();
    snapshot->batteryRemainingPercent =
        snapshot->batteryRemainingValid
            ? m_distanceEstimator.batteryRemainingPercent() : 0.0;
    snapshot->usedMahValid = m_distanceEstimator.usedMahValid();
    snapshot->usedMah = snapshot->usedMahValid
        ? m_distanceEstimator.usedMah() : 0.0;
    const double range = m_distanceEstimator.kilometresLeft();
    snapshot->batteryKilometresLeftValid = std::isfinite(range) && range > 0.0;
    snapshot->batteryKilometresLeft =
        snapshot->batteryKilometresLeftValid ? range : qQNaN();
}
