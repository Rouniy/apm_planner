#ifndef FLIGHTPLANNERNAVIGATION_H
#define FLIGHTPLANNERNAVIGATION_H

#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

struct FlightPlannerNavigationParameters
{
    double wpAccelerationCms = 100.0;
    double wpSpeedCms = 600.0;
    double wpRadiusMeters = 90.0;
    QString wpAccelerationSource;
    QString wpSpeedSource;
    QString wpRadiusSource;
    QSet<QString> availableParameters;
};

struct FlightPlannerRoutePoint
{
    double latitude = 0.0;
    double longitude = 0.0;
    bool spline = false;
};

class FlightPlannerNavigation final
{
public:
    // The preview is rebuilt on mission/navigation changes and reused during
    // map pan/zoom. Keep a hard expansion bound for pathological vehicle
    // parameters and very long spline missions.
    static constexpr int MaximumSplinePreviewSamples = 20000;

    static QStringList ParameterNames();
    static FlightPlannerNavigationParameters ResolveParameters(
        const QMap<QString, QVariant> &parameters,
        double fallbackRadiusMeters = 90.0);
    static QVector<QPair<QString, double>> WaypointRadiusWrites(
        const QSet<QString> &availableParameters,
        double radiusMeters);
    static QVector<FlightPlannerRoutePoint> BuildSplineRoute(
        const QVector<FlightPlannerRoutePoint> &route,
        const FlightPlannerNavigationParameters &parameters);
};

#endif // FLIGHTPLANNERNAVIGATION_H
