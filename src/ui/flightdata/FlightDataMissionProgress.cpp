#include "FlightDataMissionProgress.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

bool validCoordinate(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0
        && (latitude != 0.0 || longitude != 0.0);
}

double distanceMeters(double latitude1, double longitude1,
                      double latitude2, double longitude2)
{
    const double deltaLatitude = (latitude2 - latitude1) * kDegreesToRadians;
    const double deltaLongitude = (longitude2 - longitude1) * kDegreesToRadians;
    const double latitude1Radians = latitude1 * kDegreesToRadians;
    const double latitude2Radians = latitude2 * kDegreesToRadians;
    const double sinLatitude = std::sin(deltaLatitude / 2.0);
    const double sinLongitude = std::sin(deltaLongitude / 2.0);
    const double a = sinLatitude * sinLatitude
        + std::cos(latitude1Radians) * std::cos(latitude2Radians)
            * sinLongitude * sinLongitude;
    return kEarthRadiusMeters * 2.0
        * std::atan2(std::sqrt(a), std::sqrt(qMax(0.0, 1.0 - a)));
}
}

FlightDataMissionProgress FlightDataMissionProgressCalculator::Calculate(
    double homeLatitude,
    double homeLongitude,
    const QVector<FlightDataMissionPoint> &missionPoints,
    int currentWaypoint,
    double distanceToCurrentWaypoint)
{
    QVector<FlightDataMissionPoint> points;
    points.reserve(missionPoints.size());
    for (const FlightDataMissionPoint &point : missionPoints) {
        if (point.sequence > 0
            && validCoordinate(point.latitude, point.longitude)) {
            points.append(point);
        }
    }
    std::sort(points.begin(), points.end(),
              [](const FlightDataMissionPoint &left,
                 const FlightDataMissionPoint &right) {
        return left.sequence < right.sequence;
    });

    FlightDataMissionProgress result;
    result.itemCount = points.size();
    if (points.isEmpty()) {
        return result;
    }

    bool havePrevious = validCoordinate(homeLatitude, homeLongitude);
    double previousLatitude = homeLatitude;
    double previousLongitude = homeLongitude;
    for (const FlightDataMissionPoint &point : points) {
        if (havePrevious) {
            const double legDistance = distanceMeters(
                previousLatitude, previousLongitude,
                point.latitude, point.longitude);
            result.totalDistanceMeters += legDistance;
            if (point.sequence <= currentWaypoint) {
                result.travelledDistanceMeters += legDistance;
            }
        }
        previousLatitude = point.latitude;
        previousLongitude = point.longitude;
        havePrevious = true;
    }

    if (result.travelledDistanceMeters > 0.0
        && std::isfinite(distanceToCurrentWaypoint)) {
        result.travelledDistanceMeters -= qMax(0.0, distanceToCurrentWaypoint);
    }
    result.travelledDistanceMeters = qBound(
        0.0, result.travelledDistanceMeters, result.totalDistanceMeters);
    if (result.totalDistanceMeters > 0.0) {
        result.percent = qBound(
            0.0,
            result.travelledDistanceMeters / result.totalDistanceMeters * 100.0,
            100.0);
    }
    return result;
}
