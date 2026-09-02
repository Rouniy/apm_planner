#include "FlightPlannerMeasurement.h"

#include <QtGlobal>

#include <cmath>

namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr double DegreesToRadians = Pi / 180.0;
constexpr double RadiansToDegrees = 180.0 / Pi;

// GMap.NET MercatorProjection.Axis, used by the original ContextMeasure.
constexpr double MercatorAxisMeters = 6378137.0;
}

FlightPlannerMeasurement::Step FlightPlannerMeasurement::AddPoint(
        double latitude, double longitude, Result *completedResult)
{
    const Point point{latitude, longitude};
    if (!IsValidPoint(point)) {
        if (completedResult) *completedResult = {};
        return Step::Rejected;
    }

    if (!m_active) {
        m_start = point;
        m_active = true;
        if (completedResult) *completedResult = {};
        return Step::Started;
    }

    const Result result = Calculate(m_start, point);
    Reset();
    if (completedResult) *completedResult = result;
    return result.valid ? Step::Completed : Step::Rejected;
}

void FlightPlannerMeasurement::Reset()
{
    m_active = false;
    m_start = {};
}

bool FlightPlannerMeasurement::IsActive() const { return m_active; }

FlightPlannerMeasurement::Point FlightPlannerMeasurement::Start() const
{
    return m_start;
}

bool FlightPlannerMeasurement::IsValidPoint(const Point &point)
{
    return qIsFinite(point.latitude) && qIsFinite(point.longitude)
        && point.latitude >= -90.0 && point.latitude <= 90.0
        && point.longitude >= -180.0 && point.longitude <= 180.0;
}

FlightPlannerMeasurement::Result FlightPlannerMeasurement::Calculate(
        const Point &start, const Point &end)
{
    Result result;
    result.start = start;
    result.end = end;
    if (!IsValidPoint(start) || !IsValidPoint(end)) return result;

    const double latitude1 = start.latitude * DegreesToRadians;
    const double latitude2 = end.latitude * DegreesToRadians;
    const double deltaLatitude = latitude2 - latitude1;
    const double deltaLongitude =
        std::remainder((end.longitude - start.longitude)
                           * DegreesToRadians,
                       2.0 * Pi);

    const double sinHalfLatitude = std::sin(deltaLatitude / 2.0);
    const double sinHalfLongitude = std::sin(deltaLongitude / 2.0);
    const double haversine = qBound(
        0.0,
        sinHalfLatitude * sinHalfLatitude
            + std::cos(latitude1) * std::cos(latitude2)
                * sinHalfLongitude * sinHalfLongitude,
        1.0);
    const double centralAngle = 2.0 * std::atan2(
        std::sqrt(haversine), std::sqrt(1.0 - haversine));

    const double y = std::sin(deltaLongitude) * std::cos(latitude2);
    const double x = std::cos(latitude1) * std::sin(latitude2)
        - std::sin(latitude1) * std::cos(latitude2)
            * std::cos(deltaLongitude);
    double bearing = std::atan2(y, x) * RadiansToDegrees;
    bearing = std::fmod(bearing + 360.0, 360.0);

    result.distanceMeters = MercatorAxisMeters * centralAngle;
    result.bearingDegrees = bearing;
    result.valid = qIsFinite(result.distanceMeters)
        && qIsFinite(result.bearingDegrees);
    return result;
}

QString FlightPlannerMeasurement::FormatDistance(
        double distanceMeters, double displayMultiplier,
        const QString &unit)
{
    if (!qIsFinite(distanceMeters) || distanceMeters < 0.0
        || !qIsFinite(displayMultiplier) || displayMultiplier <= 0.0
        || unit.trimmed().isEmpty()) {
        return {};
    }
    return QStringLiteral("%1 %2")
        .arg(distanceMeters * displayMultiplier, 0, 'f', 2)
        .arg(unit.trimmed());
}
