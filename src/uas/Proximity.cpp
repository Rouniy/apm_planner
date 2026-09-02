#include "Proximity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr auto kDirectionalSampleAge = std::chrono::milliseconds(3000);
constexpr auto kObstacleSampleAge = std::chrono::milliseconds(200);
constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
}

void Proximity::DirectionState::Add(
    quint32 sensorId, MAV_SENSOR_ORIENTATION orientation,
    double distanceCm, TimePoint received, std::chrono::milliseconds age)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_samples.erase(std::remove_if(m_samples.begin(), m_samples.end(),
        [sensorId, orientation](const Sample &sample) {
            return sample.SensorId == sensorId
                && sample.Orientation == orientation;
        }), m_samples.end());

    Sample sample;
    sample.SensorId = sensorId;
    sample.Orientation = orientation;
    // Match Mission Planner's directional sample record: Angle is only
    // populated for custom/array samples. The radar derives cardinal angles
    // from Orientation when it renders a directional sensor.
    sample.Angle = 0.0;
    sample.Size = 45.0;
    sample.Distance = distanceCm;
    sample.Received = received;
    sample.Age = age;
    m_samples.append(sample);
    ExpireLocked(received);
}

void Proximity::DirectionState::Add(
    quint32 sensorId, double angleDegrees, double sizeDegrees,
    double distanceCm, TimePoint received, std::chrono::milliseconds age)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_samples.erase(std::remove_if(m_samples.begin(), m_samples.end(),
        [sensorId, angleDegrees](const Sample &sample) {
            return sample.SensorId == sensorId
                && sample.Angle == angleDegrees;
        }), m_samples.end());

    Sample sample;
    sample.SensorId = sensorId;
    sample.Orientation = MAV_SENSOR_ROTATION_CUSTOM;
    sample.Angle = angleDegrees;
    sample.Size = sizeDegrees;
    sample.Distance = distanceCm;
    sample.Received = received;
    sample.Age = age;
    m_samples.append(sample);
    ExpireLocked(received);
}

void Proximity::DirectionState::ExpireLocked(TimePoint now) const
{
    m_samples.erase(std::remove_if(m_samples.begin(), m_samples.end(),
        [now](const Sample &sample) {
            return sample.ExpireTime() < now;
        }), m_samples.end());
}

QVector<Proximity::Sample> Proximity::DirectionState::GetRaw(
    TimePoint now) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ExpireLocked(now);
    return m_samples;
}

double Proximity::DirectionState::GetClosest(TimePoint now) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ExpireLocked(now);
    double closest = std::numeric_limits<double>::max();
    for (const Sample &sample : m_samples) {
        closest = std::min(closest, sample.Distance);
    }
    return closest;
}

QVector<MAV_SENSOR_ORIENTATION> Proximity::DirectionState::GetWarnings(
    double minDistanceCm, TimePoint now) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ExpireLocked(now);
    QVector<MAV_SENSOR_ORIENTATION> result;
    for (const Sample &sample : m_samples) {
        if (sample.Distance < minDistanceCm) {
            result.append(sample.Orientation);
        }
    }
    return result;
}

void Proximity::DirectionState::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_samples.clear();
}

Proximity::Proximity(quint8 systemId)
    : m_systemId(systemId)
{
}

bool Proximity::observeMessage(const mavlink_message_t &message,
                               double vehicleYawRadians,
                               TimePoint received)
{
    if (message.sysid != m_systemId) {
        return false;
    }

    if (message.msgid == MAVLINK_MSG_ID_DISTANCE_SENSOR) {
        mavlink_distance_sensor_t distance{};
        mavlink_msg_distance_sensor_decode(&message, &distance);
        if (distance.current_distance >= distance.max_distance
            || distance.current_distance <= distance.min_distance) {
            return true;
        }

        m_directionState.Add(
            distance.id,
            static_cast<MAV_SENSOR_ORIENTATION>(distance.orientation),
            distance.current_distance, received, kDirectionalSampleAge);
        m_dataAvailable.store(true);
        return true;
    }

    if (message.msgid != MAVLINK_MSG_ID_OBSTACLE_DISTANCE) {
        return false;
    }

    mavlink_obstacle_distance_t obstacle{};
    mavlink_msg_obstacle_distance_decode(&message, &obstacle);
    const double increment = obstacle.increment == 0
        ? obstacle.increment_f : obstacle.increment;
    double rangeStart = obstacle.angle_offset;
    if (obstacle.frame == MAV_FRAME_GLOBAL
        && std::isfinite(vehicleYawRadians)) {
        rangeStart += vehicleYawRadians * kRadiansToDegrees;
    }

    for (int index = 0;
         index < MAVLINK_MSG_OBSTACLE_DISTANCE_FIELD_DISTANCES_LEN;
         ++index) {
        const quint16 rawDistance = obstacle.distances[index];
        if (rawDistance == std::numeric_limits<quint16>::max()
            || rawDistance > obstacle.max_distance
            || rawDistance < obstacle.min_distance) {
            continue;
        }
        const double distance = std::min(
            std::max(double(rawDistance), double(obstacle.min_distance)),
            double(obstacle.max_distance));
        m_directionState.Add(obstacle.sensor_type,
                             rangeStart + increment * index,
                             increment, distance, received,
                             kObstacleSampleAge);
    }
    // Mission Planner records that a supported array packet arrived even if
    // every bin in it is marked unused.
    m_dataAvailable.store(true);
    return true;
}

void Proximity::Clear()
{
    m_directionState.Clear();
    m_dataAvailable.store(false);
}

double Proximity::OrientationAngle(MAV_SENSOR_ORIENTATION orientation)
{
    switch (orientation) {
    case MAV_SENSOR_ROTATION_NONE: return 0.0;
    case MAV_SENSOR_ROTATION_YAW_45: return 45.0;
    case MAV_SENSOR_ROTATION_YAW_90: return 90.0;
    case MAV_SENSOR_ROTATION_YAW_135: return 135.0;
    case MAV_SENSOR_ROTATION_YAW_180: return 180.0;
    case MAV_SENSOR_ROTATION_YAW_225: return 225.0;
    case MAV_SENSOR_ROTATION_YAW_270: return 270.0;
    case MAV_SENSOR_ROTATION_YAW_315: return 315.0;
    default: return 0.0;
    }
}
