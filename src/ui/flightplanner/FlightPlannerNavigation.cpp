#include "FlightPlannerNavigation.h"

#include <QtGlobal>

#include <cmath>

namespace {
constexpr double kLatitudeToCentimeters = 1.113195;
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
constexpr int kMaximumSamplesPerSegment = 10000;

struct Vector2
{
    double x = 0.0;
    double y = 0.0;

    Vector2 operator+(const Vector2 &other) const
    {
        return {x + other.x, y + other.y};
    }
    Vector2 operator-(const Vector2 &other) const
    {
        return {x - other.x, y - other.y};
    }
    Vector2 operator*(double factor) const
    {
        return {x * factor, y * factor};
    }
    double length() const { return std::hypot(x, y); }
};

bool positiveParameter(const QMap<QString, QVariant> &parameters,
                       const QString &name, double *value)
{
    const auto found = parameters.constFind(name);
    if (found == parameters.constEnd()) {
        return false;
    }
    bool ok = false;
    const double converted = found.value().toDouble(&ok);
    if (!ok || !std::isfinite(converted) || converted <= 0.0) {
        return false;
    }
    *value = converted;
    return true;
}

bool nonNegativeParameter(const QMap<QString, QVariant> &parameters,
                          const QString &name, double *value)
{
    const auto found = parameters.constFind(name);
    if (found == parameters.constEnd()) {
        return false;
    }
    bool ok = false;
    const double converted = found.value().toDouble(&ok);
    if (!ok || !std::isfinite(converted) || converted < 0.0) {
        return false;
    }
    *value = converted;
    return true;
}

bool validRoutePoint(const FlightPlannerRoutePoint &point)
{
    return std::isfinite(point.latitude)
        && std::isfinite(point.longitude)
        && point.latitude >= -90.0 && point.latitude <= 90.0
        && point.longitude >= -180.0 && point.longitude <= 180.0;
}

bool appendUnique(QVector<FlightPlannerRoutePoint> *route,
                  const FlightPlannerRoutePoint &point)
{
    if (!route || !validRoutePoint(point)) {
        return false;
    }
    if (!route->isEmpty()) {
        const FlightPlannerRoutePoint &last = route->constLast();
        if (qAbs(last.latitude - point.latitude) < 1.0e-10
            && qAbs(last.longitude - point.longitude) < 1.0e-10) {
            return false;
        }
    }
    route->append(point);
    return true;
}
}

QStringList FlightPlannerNavigation::ParameterNames()
{
    return {
        QStringLiteral("WPNAV_ACCEL"),
        QStringLiteral("WP_ACC"),
        QStringLiteral("WPNAV_SPEED"),
        QStringLiteral("WP_SPD"),
        QStringLiteral("WP_RADIUS"),
        QStringLiteral("WPNAV_RADIUS"),
        QStringLiteral("WP_RADIUS_M"),
    };
}

FlightPlannerNavigationParameters
FlightPlannerNavigation::ResolveParameters(
    const QMap<QString, QVariant> &parameters,
    double fallbackRadiusMeters)
{
    FlightPlannerNavigationParameters result;
    if (std::isfinite(fallbackRadiusMeters)
        && fallbackRadiusMeters >= 0.0) {
        result.wpRadiusMeters = fallbackRadiusMeters;
    }
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
        bool ok = false;
        const double value = it.value().toDouble(&ok);
        if (ok && std::isfinite(value)) {
            result.availableParameters.insert(it.key());
        }
    }

    double value = 0.0;
    if (positiveParameter(parameters, QStringLiteral("WPNAV_ACCEL"), &value)) {
        result.wpAccelerationCms = value;
        result.wpAccelerationSource = QStringLiteral("WPNAV_ACCEL");
    } else if (positiveParameter(parameters, QStringLiteral("WP_ACC"), &value)) {
        result.wpAccelerationCms = value * 100.0;
        result.wpAccelerationSource = QStringLiteral("WP_ACC");
    }

    if (positiveParameter(parameters, QStringLiteral("WPNAV_SPEED"), &value)) {
        result.wpSpeedCms = value;
        result.wpSpeedSource = QStringLiteral("WPNAV_SPEED");
    } else if (positiveParameter(parameters, QStringLiteral("WP_SPD"), &value)) {
        result.wpSpeedCms = value * 100.0;
        result.wpSpeedSource = QStringLiteral("WP_SPD");
    }

    // Match FlightPlanner.setWPParams ordering: the 4.7+ meter parameter
    // deliberately wins over both older spellings when all are present.
    if (nonNegativeParameter(parameters, QStringLiteral("WP_RADIUS"), &value)) {
        result.wpRadiusMeters = value;
        result.wpRadiusSource = QStringLiteral("WP_RADIUS");
    }
    if (nonNegativeParameter(parameters, QStringLiteral("WPNAV_RADIUS"), &value)) {
        result.wpRadiusMeters = value / 100.0;
        result.wpRadiusSource = QStringLiteral("WPNAV_RADIUS");
    }
    if (nonNegativeParameter(parameters, QStringLiteral("WP_RADIUS_M"), &value)) {
        result.wpRadiusMeters = value;
        result.wpRadiusSource = QStringLiteral("WP_RADIUS_M");
    }
    return result;
}

QVector<QPair<QString, double>> FlightPlannerNavigation::WaypointRadiusWrites(
    const QSet<QString> &availableParameters,
    double radiusMeters)
{
    QVector<QPair<QString, double>> result;
    if (!std::isfinite(radiusMeters) || radiusMeters < 0.0) {
        return result;
    }
    if (availableParameters.contains(QStringLiteral("WP_RADIUS"))) {
        result.append(qMakePair(QStringLiteral("WP_RADIUS"), radiusMeters));
    }
    if (availableParameters.contains(QStringLiteral("WP_RADIUS_M"))) {
        result.append(qMakePair(QStringLiteral("WP_RADIUS_M"), radiusMeters));
    }
    if (availableParameters.contains(QStringLiteral("WPNAV_RADIUS"))) {
        result.append(qMakePair(QStringLiteral("WPNAV_RADIUS"),
                                radiusMeters * 100.0));
    }
    return result;
}

QVector<FlightPlannerRoutePoint> FlightPlannerNavigation::BuildSplineRoute(
    const QVector<FlightPlannerRoutePoint> &route,
    const FlightPlannerNavigationParameters &parameters)
{
    QVector<FlightPlannerRoutePoint> result;
    if (route.isEmpty()) {
        return result;
    }
    for (const FlightPlannerRoutePoint &point : route) {
        if (!validRoutePoint(point)) {
            return result;
        }
    }

    const double referenceLatitude = route.first().latitude;
    const double referenceLongitude = route.first().longitude;
    const double longitudeScale = qMax(
        0.01, qAbs(std::cos(referenceLatitude * kDegreesToRadians)));
    const auto toVector = [&](const FlightPlannerRoutePoint &point) {
        return Vector2{
            (point.latitude - referenceLatitude) * 1.0e7
                * kLatitudeToCentimeters,
            (point.longitude - referenceLongitude) * 1.0e7
                * kLatitudeToCentimeters * longitudeScale,
        };
    };
    const auto toPoint = [&](const Vector2 &value) {
        return FlightPlannerRoutePoint{
            referenceLatitude
                + value.x / (1.0e7 * kLatitudeToCentimeters),
            referenceLongitude
                + value.y
                    / (1.0e7 * kLatitudeToCentimeters * longitudeScale),
            true,
        };
    };

    appendUnique(&result, route.first());
    double velocityScaler = 0.0;
    double previousSplineTime = 0.0;
    bool previousWasSpline = false;
    Vector2 previousDestinationVelocity;
    int previewSamples = 0;

    for (int index = 1; index < route.size(); ++index) {
        const FlightPlannerRoutePoint &destinationPoint = route.at(index);
        if (!destinationPoint.spline) {
            appendUnique(&result, destinationPoint);
            previousWasSpline = false;
            velocityScaler = 0.0;
            previousSplineTime = 0.0;
            continue;
        }

        const FlightPlannerRoutePoint &originPoint = route.at(index - 1);
        const FlightPlannerRoutePoint &previousPoint =
            route.at(qMax(0, index - 2));
        const FlightPlannerRoutePoint &nextPoint =
            route.at(qMin(route.size() - 1, index + 1));
        const Vector2 origin = toVector(originPoint);
        const Vector2 destination = toVector(destinationPoint);
        const Vector2 nextDestination = toVector(nextPoint);
        Vector2 originVelocity = previousWasSpline
            ? previousDestinationVelocity
            : origin - toVector(previousPoint);
        Vector2 destinationVelocity = index + 1 < route.size()
            ? (route.at(index + 1).spline
                ? nextDestination - origin
                : nextDestination - destination)
            : Vector2{};

        const double velocityLength =
            (originVelocity + destinationVelocity).length();
        const double maximumVelocityLength =
            (destination - origin).length() * 4.0;
        if (velocityLength > maximumVelocityLength
            && velocityLength > 0.0) {
            const double scale = maximumVelocityLength / velocityLength;
            originVelocity = originVelocity * scale;
            destinationVelocity = destinationVelocity * scale;
        }

        const Vector2 c0 = origin;
        const Vector2 c1 = originVelocity;
        const Vector2 c2 = origin * -3.0 + originVelocity * -2.0
            + destination * 3.0 - destinationVelocity;
        const Vector2 c3 = origin * 2.0 + originVelocity
            - destination * 2.0 + destinationVelocity;
        double splineTime = previousWasSpline
            && previousSplineTime > 1.0 && previousSplineTime < 1.1
            ? previousSplineTime - 1.0 : 0.0;
        // Spline2::set_spline_origin_and_destination resets acceleration for
        // every segment, including consecutive spline waypoints.
        velocityScaler = 0.0;
        const bool fastWaypoint = index + 1 < route.size();
        const double slowDownDistance = parameters.wpAccelerationCms > 0.0
            ? parameters.wpSpeedCms * parameters.wpSpeedCms
                / (4.0 * parameters.wpAccelerationCms)
            : 0.0;

        for (int sample = 0;
             sample < kMaximumSamplesPerSegment && splineTime < 1.0
                 && previewSamples < MaximumSplinePreviewSamples;
             ++sample) {
            const double squared = splineTime * splineTime;
            const double cubed = squared * splineTime;
            const Vector2 position = c0 + c1 * splineTime
                + c2 * squared + c3 * cubed;
            if (appendUnique(&result, toPoint(position))) {
                ++previewSamples;
            }

            const Vector2 velocity = c1 + c2 * (2.0 * splineTime)
                + c3 * (3.0 * squared);
            const double targetVelocity = velocity.length();
            if (targetVelocity <= 1.0e-6) {
                break;
            }
            const double distanceToWaypoint =
                (destination - position).length();
            if (!fastWaypoint && distanceToWaypoint < slowDownDistance) {
                velocityScaler = std::sqrt(
                    distanceToWaypoint * 2.0
                    * parameters.wpAccelerationCms);
            } else if (velocityScaler < parameters.wpSpeedCms) {
                // Mission Planner samples the preview with dt=1 second.
                velocityScaler += parameters.wpAccelerationCms;
            }
            velocityScaler = qMin(parameters.wpSpeedCms, velocityScaler);
            if (velocityScaler <= 0.0) {
                break;
            }
            splineTime += velocityScaler / targetVelocity;
        }
        appendUnique(&result, destinationPoint);
        previousDestinationVelocity = destinationVelocity;
        previousSplineTime = splineTime;
        previousWasSpline = true;
    }
    return result;
}
