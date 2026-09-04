#include "PropagationDistanceEstimator.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double EarthRadiusMetres = 6378137.0;
constexpr qint64 DistanceSampleIntervalMs = 1000;
constexpr double UnknownCurrentAmps = -0.01;
constexpr double UsedMahEpsilon = 1.0e-9;
}

void PropagationDistanceEstimator::reset()
{
    m_connected = false;
    m_armed = false;
    m_gpsFixType = 0;
    m_havePosition = false;
    m_lastLatitude = 0.0;
    m_lastLongitude = 0.0;
    m_haveDistanceTimestamp = false;
    m_lastDistanceTimestampMs = 0;
    m_travelledMetres = 0.0;
    m_remainingPercent = qQNaN();
    m_usedMahValid = false;
    m_usedMah = 0.0;
    m_haveCurrentTimestamp = false;
    m_lastCurrentTimestampMs = 0;
}

void PropagationDistanceEstimator::setConnected(bool connected)
{
    if (!connected) {
        reset();
        return;
    }
    m_connected = true;
}

void PropagationDistanceEstimator::observeBattery(
    qint64 monotonicMs, double batteryRemainingPercent,
    double currentAmps, double consumedMah)
{
    if (!m_connected) {
        return;
    }

    m_remainingPercent = std::isfinite(batteryRemainingPercent)
            && batteryRemainingPercent >= 0.0
            && batteryRemainingPercent <= 100.0
        ? batteryRemainingPercent : qQNaN();

    const bool authoritativeConsumption = std::isfinite(consumedMah)
        && consumedMah >= 0.0;
    if (authoritativeConsumption) {
        m_usedMah = consumedMah;
        m_usedMahValid = true;
    }

    if (monotonicMs < 0) {
        return;
    }

    if (!m_haveCurrentTimestamp) {
        m_haveCurrentTimestamp = true;
        m_lastCurrentTimestampMs = monotonicMs;
        return;
    }
    if (monotonicMs < m_lastCurrentTimestampMs) {
        m_lastCurrentTimestampMs = monotonicMs;
        return;
    }

    if (authoritativeConsumption) {
        // The reported counter is already authoritative for the elapsed
        // interval. Establish a new integration baseline for a possible
        // SYS_STATUS fallback without counting that interval twice.
        m_lastCurrentTimestampMs = monotonicMs;
        return;
    }
    if (!validCurrent(currentAmps)) {
        return;
    }

    const qint64 elapsedMs = monotonicMs - m_lastCurrentTimestampMs;
    m_lastCurrentTimestampMs = monotonicMs;
    if (elapsedMs <= 0) {
        return;
    }
    if (!m_usedMahValid) {
        m_usedMah = 0.0;
        m_usedMahValid = true;
    }
    // A * ms / 3600 is mAh.
    m_usedMah += currentAmps * double(elapsedMs) / 3600.0;
    if (!std::isfinite(m_usedMah)) {
        m_usedMah = 0.0;
        m_usedMahValid = false;
    }
}

double PropagationDistanceEstimator::observePosition(
    qint64 monotonicMs, double latitude, double longitude)
{
    if (!m_connected || monotonicMs < 0) {
        return kilometresLeft();
    }

    const bool positionValid = validCoordinate(latitude, longitude);
    if (!m_haveDistanceTimestamp) {
        m_haveDistanceTimestamp = true;
        m_lastDistanceTimestampMs = monotonicMs;
        m_havePosition = positionValid;
        if (positionValid) {
            m_lastLatitude = latitude;
            m_lastLongitude = longitude;
        }
        return kilometresLeft();
    }

    if (monotonicMs < m_lastDistanceTimestampMs) {
        m_lastDistanceTimestampMs = monotonicMs;
        m_havePosition = positionValid;
        if (positionValid) {
            m_lastLatitude = latitude;
            m_lastLongitude = longitude;
        }
        return kilometresLeft();
    }
    if (monotonicMs - m_lastDistanceTimestampMs
        < DistanceSampleIntervalMs) {
        return kilometresLeft();
    }
    m_lastDistanceTimestampMs = monotonicMs;

    if (m_havePosition && positionValid && m_armed && m_gpsFixType >= 3) {
        const double increment = distanceMetres(
            m_lastLatitude, m_lastLongitude, latitude, longitude);
        if (std::isfinite(increment) && increment >= 0.0) {
            // MP10 intentionally has no teleport filter here. Preserve that
            // observable contract rather than silently changing range.
            m_travelledMetres += increment;
        }
    }

    m_havePosition = positionValid;
    if (positionValid) {
        m_lastLatitude = latitude;
        m_lastLongitude = longitude;
    }
    return kilometresLeft();
}

bool PropagationDistanceEstimator::batteryRemainingValid() const
{
    return std::isfinite(m_remainingPercent)
        && m_remainingPercent >= 0.0 && m_remainingPercent <= 100.0;
}

double PropagationDistanceEstimator::kilometresLeft() const
{
    // MP10 computes remaining capacity divided by mAh/km. Once usedMah is
    // non-zero this reduces exactly to travelledKm * remaining / consumed%.
    // Keep the used-mAh gate so absent battery-consumption telemetry cannot
    // manufacture an apparently valid range.
    if (!m_connected || !batteryRemainingValid() || !m_usedMahValid
        || std::abs(m_usedMah) <= UsedMahEpsilon
        || m_remainingPercent <= 0.0 || m_remainingPercent >= 100.0
        || !std::isfinite(m_travelledMetres) || m_travelledMetres <= 0.0) {
        return qQNaN();
    }
    return (m_travelledMetres / 1000.0)
        * m_remainingPercent / (100.0 - m_remainingPercent);
}

bool PropagationDistanceEstimator::validCoordinate(
    double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0
        && (latitude != 0.0 || longitude != 0.0);
}

bool PropagationDistanceEstimator::validCurrent(double currentAmps)
{
    return std::isfinite(currentAmps)
        && std::abs(currentAmps - UnknownCurrentAmps) > 1.0e-9;
}

double PropagationDistanceEstimator::distanceMetres(
    double firstLatitude, double firstLongitude,
    double secondLatitude, double secondLongitude)
{
    const double firstLatitudeRadians = qDegreesToRadians(firstLatitude);
    const double secondLatitudeRadians = qDegreesToRadians(secondLatitude);
    const double latitudeDelta = secondLatitudeRadians
        - firstLatitudeRadians;
    const double longitudeDelta = qDegreesToRadians(
        secondLongitude - firstLongitude);
    const double haversine = std::pow(std::sin(latitudeDelta / 2.0), 2.0)
        + std::cos(firstLatitudeRadians)
            * std::cos(secondLatitudeRadians)
            * std::pow(std::sin(longitudeDelta / 2.0), 2.0);
    return 2.0 * EarthRadiusMetres
        * std::asin(std::min(1.0, std::sqrt(haversine)));
}
