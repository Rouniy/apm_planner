#include "ui/flightplanner/FlightPlannerNavigation.h"

#include <QtTest>

#include <cmath>

class FlightPlannerNavigationTest final : public QObject
{
    Q_OBJECT

private slots:
    void resolvesArduPilot47AliasesAndRadiusPrecedence();
    void convertsRadiusWritesToFirmwareUnits();
    void samplesSplineSegmentsUsingResolvedDynamics();
    void boundsSplinePreviewAcrossEntireRoute();
};

void FlightPlannerNavigationTest::resolvesArduPilot47AliasesAndRadiusPrecedence()
{
    const QMap<QString, QVariant> parameters{
        {QStringLiteral("WPNAV_ACCEL"), 250.0},
        {QStringLiteral("WP_ACC"), 3.0},
        {QStringLiteral("WP_SPD"), 7.5},
        {QStringLiteral("WP_RADIUS"), 10.0},
        {QStringLiteral("WPNAV_RADIUS"), 2500.0},
        {QStringLiteral("WP_RADIUS_M"), 42.0},
    };
    const FlightPlannerNavigationParameters resolved =
        FlightPlannerNavigation::ResolveParameters(parameters, 90.0);

    QCOMPARE(resolved.wpAccelerationCms, 250.0);
    QCOMPARE(resolved.wpAccelerationSource,
             QStringLiteral("WPNAV_ACCEL"));
    QCOMPARE(resolved.wpSpeedCms, 750.0);
    QCOMPARE(resolved.wpSpeedSource, QStringLiteral("WP_SPD"));
    QCOMPARE(resolved.wpRadiusMeters, 42.0);
    QCOMPARE(resolved.wpRadiusSource, QStringLiteral("WP_RADIUS_M"));

    const FlightPlannerNavigationParameters defaults =
        FlightPlannerNavigation::ResolveParameters({}, 35.0);
    QCOMPARE(defaults.wpAccelerationCms, 100.0);
    QCOMPARE(defaults.wpSpeedCms, 600.0);
    QCOMPARE(defaults.wpRadiusMeters, 35.0);
}

void FlightPlannerNavigationTest::convertsRadiusWritesToFirmwareUnits()
{
    const QSet<QString> available{
        QStringLiteral("WP_RADIUS"),
        QStringLiteral("WP_RADIUS_M"),
        QStringLiteral("WPNAV_RADIUS"),
    };
    const QVector<QPair<QString, double>> writes =
        FlightPlannerNavigation::WaypointRadiusWrites(available, 12.5);
    QCOMPARE(writes.size(), 3);
    QCOMPARE(writes.at(0), qMakePair(QStringLiteral("WP_RADIUS"), 12.5));
    QCOMPARE(writes.at(1), qMakePair(QStringLiteral("WP_RADIUS_M"), 12.5));
    QCOMPARE(writes.at(2),
             qMakePair(QStringLiteral("WPNAV_RADIUS"), 1250.0));
}

void FlightPlannerNavigationTest::samplesSplineSegmentsUsingResolvedDynamics()
{
    const QVector<FlightPlannerRoutePoint> route{
        {47.0000, 8.0000, false},
        {47.0010, 8.0000, false},
        {47.0020, 8.0010, true},
        {47.0020, 8.0020, false},
    };
    FlightPlannerNavigationParameters slow;
    slow.wpAccelerationCms = 10000.0;
    slow.wpSpeedCms = 100.0;
    FlightPlannerNavigationParameters fast = slow;
    fast.wpSpeedCms = 1000.0;

    const QVector<FlightPlannerRoutePoint> slowRoute =
        FlightPlannerNavigation::BuildSplineRoute(route, slow);
    const QVector<FlightPlannerRoutePoint> fastRoute =
        FlightPlannerNavigation::BuildSplineRoute(route, fast);

    QVERIFY(slowRoute.size() > route.size());
    QVERIFY(slowRoute.size() > fastRoute.size());
    QCOMPARE(slowRoute.first().latitude, route.first().latitude);
    QCOMPARE(slowRoute.first().longitude, route.first().longitude);
    QCOMPARE(slowRoute.last().latitude, route.last().latitude);
    QCOMPARE(slowRoute.last().longitude, route.last().longitude);
    for (const FlightPlannerRoutePoint &point : slowRoute) {
        QVERIFY(std::isfinite(point.latitude));
        QVERIFY(std::isfinite(point.longitude));
    }
}

void FlightPlannerNavigationTest::boundsSplinePreviewAcrossEntireRoute()
{
    QVector<FlightPlannerRoutePoint> route;
    for (int index = 0; index < 12; ++index) {
        route.append({47.0 + index * 0.001,
                      8.0 + index * 0.001,
                      index != 0});
    }
    FlightPlannerNavigationParameters parameters;
    parameters.wpAccelerationCms = 0.001;
    parameters.wpSpeedCms = 0.001;

    const QVector<FlightPlannerRoutePoint> preview =
        FlightPlannerNavigation::BuildSplineRoute(route, parameters);
    QVERIFY(preview.size()
            <= route.size()
                + FlightPlannerNavigation::MaximumSplinePreviewSamples);
    QCOMPARE(preview.first().latitude, route.first().latitude);
    QCOMPARE(preview.last().latitude, route.last().latitude);
    QCOMPARE(preview.last().longitude, route.last().longitude);
}

QTEST_APPLESS_MAIN(FlightPlannerNavigationTest)
#include "test_flightplannernavigation.moc"
