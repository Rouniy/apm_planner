#include "FlightPlannerRouteMetrics.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double DegreesToRadians = Pi / 180.0;
constexpr double RadiansToDegrees = 180.0 / Pi;
constexpr double EarthRadiusMeters = 6371000.0;

double normalizedDegrees(double value)
{
    value = std::fmod(value, 360.0);
    return value < 0.0 ? value + 360.0 : value;
}
}

bool FlightPlannerRouteMetrics::Coordinate::IsValid() const
{
    return std::isfinite(latitude) && latitude >= -90.0 && latitude <= 90.0
            && std::isfinite(longitude) && longitude >= -180.0
            && longitude <= 180.0 && std::isfinite(altitude);
}

double FlightPlannerRouteMetrics::HorizontalDistanceMeters(
        const Coordinate &from, const Coordinate &to)
{
    if (!from.IsValid() || !to.IsValid()) return 0.0;

    const double latitude1 = from.latitude * DegreesToRadians;
    const double latitude2 = to.latitude * DegreesToRadians;
    const double deltaLatitude = latitude2 - latitude1;
    const double deltaLongitude = (to.longitude - from.longitude)
            * DegreesToRadians;
    const double sinLatitude = std::sin(deltaLatitude * 0.5);
    const double sinLongitude = std::sin(deltaLongitude * 0.5);
    const double haversine = std::clamp(
        sinLatitude * sinLatitude
            + std::cos(latitude1) * std::cos(latitude2)
                * sinLongitude * sinLongitude,
        0.0, 1.0);
    const double angularDistance = 2.0 * std::atan2(
        std::sqrt(haversine), std::sqrt(1.0 - haversine));
    return EarthRadiusMeters * angularDistance;
}

double FlightPlannerRouteMetrics::BearingDegrees(
        const Coordinate &from, const Coordinate &to)
{
    if (!from.IsValid() || !to.IsValid()) return 0.0;

    const double latitude1 = from.latitude * DegreesToRadians;
    const double latitude2 = to.latitude * DegreesToRadians;
    const double deltaLongitude = (to.longitude - from.longitude)
            * DegreesToRadians;
    const double y = std::sin(deltaLongitude) * std::cos(latitude2);
    const double x = std::cos(latitude1) * std::sin(latitude2)
            - std::sin(latitude1) * std::cos(latitude2)
                * std::cos(deltaLongitude);
    if (x == 0.0 && y == 0.0) return 0.0;
    return normalizedDegrees(std::atan2(y, x) * RadiansToDegrees);
}

double FlightPlannerRouteMetrics::AdditionalLoiterDistanceMeters(
        const RoutePoint &point, const Options &options)
{
    if (!point.loiterTurnsCommand || !std::isfinite(point.loiterTurns)
        || point.loiterTurns <= 0.0
        || !std::isfinite(point.commandLoiterRadiusMeters)) {
        return 0.0;
    }
    double radius = std::abs(point.commandLoiterRadiusMeters);
    if (radius == 0.0) {
        if (!options.useConfiguredLoiterRadiusWhenCommandRadiusIsZero)
            return 0.0;
        if (!std::isfinite(options.configuredLoiterRadiusMeters)) return 0.0;
        radius = std::abs(options.configuredLoiterRadiusMeters);
    }
    return 2.0 * Pi * radius * point.loiterTurns;
}

FlightPlannerRouteMetrics::Result FlightPlannerRouteMetrics::Calculate(
        const Coordinate &home, bool homeValid,
        const QVector<RoutePoint> &points)
{
    return Calculate(home, homeValid, points, Options{});
}

FlightPlannerRouteMetrics::Result FlightPlannerRouteMetrics::Calculate(
        const Coordinate &home, bool homeValid,
        const QVector<RoutePoint> &points, const Options &options)
{
    Result result;
    result.legs.resize(points.size());
    const bool useHome = homeValid && home.IsValid();
    Coordinate previous = home;
    int previousPointIndex = -1;
    bool havePrevious = useHome;
    int lastIncludedPointIndex = -1;

    for (int index = 0; index < points.size(); ++index) {
        Leg &leg = result.legs[index];
        leg.toPointIndex = index;
        const RoutePoint &point = points.at(index);
        if (!point.included || !point.coordinate.IsValid()) continue;

        ++result.includedPointCount;
        leg.additionalDistanceMeters = AdditionalLoiterDistanceMeters(
            point, options);
        result.additionalDistanceMeters += leg.additionalDistanceMeters;
        result.totalDistanceMeters += leg.additionalDistanceMeters;
        result.missionDistanceMeters += leg.additionalDistanceMeters;
        if (havePrevious) {
            leg.valid = true;
            leg.fromPointIndex = previousPointIndex;
            leg.startsAtHome = previousPointIndex < 0;
            leg.horizontalDistanceMeters = HorizontalDistanceMeters(
                previous, point.coordinate);
            const double altitudeDifference = point.coordinate.altitude
                    - previous.altitude;
            leg.distanceMeters = std::hypot(
                leg.horizontalDistanceMeters, altitudeDifference);
            // Match Mission Planner's historical grid formula exactly. A
            // geodesic's reciprocal initial bearing is not generally equal
            // to the forward initial bearing on long/high-latitude legs.
            leg.bearingDegrees = leg.horizontalDistanceMeters > 0.0
                ? normalizedDegrees(
                    BearingDegrees(point.coordinate, previous) + 180.0)
                : 0.0;
            const double gradient = leg.horizontalDistanceMeters > 0.0
                    ? altitudeDifference / leg.horizontalDistanceMeters : 0.0;
            leg.gradientPercent = gradient * 100.0;
            leg.angleDegrees = std::atan(gradient) * RadiansToDegrees;

            result.totalHorizontalDistanceMeters
                    += leg.horizontalDistanceMeters;
            result.totalDistanceMeters += leg.distanceMeters;
            if (!leg.startsAtHome)
                result.missionDistanceMeters += leg.distanceMeters;
            result.lastLegDistanceMeters = leg.distanceMeters;
        }

        previous = point.coordinate;
        previousPointIndex = index;
        lastIncludedPointIndex = index;
        havePrevious = true;
    }

    if (useHome && lastIncludedPointIndex >= 0) {
        result.lastToHomeDistanceMeters = HorizontalDistanceMeters(
            points.at(lastIncludedPointIndex).coordinate, home);
    }
    return result;
}
