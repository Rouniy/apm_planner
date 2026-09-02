#include "ui/flightplanner/FlightPlannerRouteMetrics.h"

#include <QtTest>

#include <cmath>
#include <limits>

namespace {
using Metrics = FlightPlannerRouteMetrics;

Metrics::RoutePoint point(double latitude, double longitude, double altitude,
                          bool included = true)
{
    return {{latitude, longitude, altitude}, included};
}

void verifyNear(double actual, double expected, double tolerance)
{
    QVERIFY2(std::abs(actual - expected) <= tolerance,
             qPrintable(QStringLiteral("actual=%1 expected=%2 tolerance=%3")
                            .arg(actual, 0, 'g', 16)
                            .arg(expected, 0, 'g', 16)
                            .arg(tolerance, 0, 'g', 16)));
}
}

class FlightPlannerRouteMetricsTest final : public QObject
{
    Q_OBJECT

private slots:
    void emptyAndInvalidInputsAreSafe();
    void homeAndWaypointLegsMatchMissionPlannerFormulas();
    void excludedPointsDoNotBreakRouteContinuity();
    void routeWithoutHomeStartsAtFirstValidPoint();
    void verticalAndDatelineLegsRemainFinite();
    void loiterTurnsFollowMissionPlannerRadiusRules();
};

void FlightPlannerRouteMetricsTest::emptyAndInvalidInputsAreSafe()
{
    const Metrics::Coordinate invalidHome{
        std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
    const Metrics::Result empty = Metrics::Calculate(invalidHome, true, {});
    QVERIFY(empty.legs.isEmpty());
    QCOMPARE(empty.includedPointCount, 0);
    QCOMPARE(empty.totalDistanceMeters, 0.0);
    QCOMPARE(empty.missionDistanceMeters, 0.0);

    QVector<Metrics::RoutePoint> invalidPoints{
        point(91.0, 0.0, 0.0),
        point(0.0, 181.0, 0.0),
        point(0.0, 0.0, std::numeric_limits<double>::infinity()),
    };
    const Metrics::Result invalid = Metrics::Calculate(
        {0.0, 0.0, 0.0}, true, invalidPoints);
    QCOMPARE(invalid.legs.size(), invalidPoints.size());
    QCOMPARE(invalid.includedPointCount, 0);
    for (const Metrics::Leg &leg : invalid.legs) QVERIFY(!leg.valid);
}

void FlightPlannerRouteMetricsTest::homeAndWaypointLegsMatchMissionPlannerFormulas()
{
    const Metrics::Coordinate home{0.0, 0.0, 0.0};
    const QVector<Metrics::RoutePoint> points{
        point(0.0, 0.001, 100.0),
        point(0.001, 0.001, 100.0),
    };
    const Metrics::Result result = Metrics::Calculate(home, true, points);

    QCOMPARE(result.legs.size(), 2);
    QCOMPARE(result.includedPointCount, 2);
    QVERIFY(result.legs.at(0).valid);
    QVERIFY(result.legs.at(0).startsAtHome);
    QCOMPARE(result.legs.at(0).fromPointIndex, -1);
    QCOMPARE(result.legs.at(0).toPointIndex, 0);
    verifyNear(result.legs.at(0).horizontalDistanceMeters,
               111.1949266, 0.001);
    verifyNear(result.legs.at(0).distanceMeters,
               std::hypot(111.1949266, 100.0), 0.001);
    verifyNear(result.legs.at(0).bearingDegrees, 90.0, 0.0001);
    verifyNear(result.legs.at(0).gradientPercent,
               10000.0 / 111.1949266, 0.001);
    verifyNear(result.legs.at(0).angleDegrees,
               std::atan(100.0 / 111.1949266) * 180.0
                   / 3.14159265358979323846,
               0.001);

    QVERIFY(result.legs.at(1).valid);
    QVERIFY(!result.legs.at(1).startsAtHome);
    QCOMPARE(result.legs.at(1).fromPointIndex, 0);
    verifyNear(result.legs.at(1).horizontalDistanceMeters,
               111.1949266, 0.001);
    verifyNear(result.legs.at(1).distanceMeters, 111.1949266, 0.001);
    verifyNear(result.legs.at(1).bearingDegrees, 0.0, 0.0001);
    QCOMPARE(result.legs.at(1).gradientPercent, 0.0);
    QCOMPARE(result.legs.at(1).angleDegrees, 0.0);

    verifyNear(result.totalHorizontalDistanceMeters,
               2.0 * 111.1949266, 0.002);
    verifyNear(result.totalDistanceMeters,
               std::hypot(111.1949266, 100.0) + 111.1949266, 0.002);
    // MP10 TotalDist excludes Home -> first waypoint.
    verifyNear(result.missionDistanceMeters, 111.1949266, 0.001);
    verifyNear(result.lastLegDistanceMeters, 111.1949266, 0.001);
    verifyNear(result.lastToHomeDistanceMeters, 157.2533733, 0.002);
}

void FlightPlannerRouteMetricsTest::excludedPointsDoNotBreakRouteContinuity()
{
    const Metrics::Coordinate home{10.0, 20.0, 50.0};
    const QVector<Metrics::RoutePoint> points{
        point(10.0, 20.001, 60.0),
        point(80.0, 80.0, 1000.0, false),
        point(10.0, 20.002, 70.0),
    };
    const Metrics::Result result = Metrics::Calculate(home, true, points);

    QCOMPARE(result.includedPointCount, 2);
    QVERIFY(result.legs.at(0).valid);
    QVERIFY(!result.legs.at(1).valid);
    QVERIFY(result.legs.at(2).valid);
    QCOMPARE(result.legs.at(2).fromPointIndex, 0);
    QCOMPARE(result.legs.at(2).toPointIndex, 2);
    QVERIFY(result.missionDistanceMeters > 100.0);
    QVERIFY(result.missionDistanceMeters < 120.0);
}

void FlightPlannerRouteMetricsTest::routeWithoutHomeStartsAtFirstValidPoint()
{
    const QVector<Metrics::RoutePoint> points{
        point(95.0, 0.0, 0.0),
        point(1.0, 1.0, 10.0),
        point(1.0, 1.001, 20.0),
    };
    const Metrics::Result result = Metrics::Calculate(
        {0.0, 0.0, 0.0}, false, points);

    QCOMPARE(result.includedPointCount, 2);
    QVERIFY(!result.legs.at(0).valid);
    QVERIFY(!result.legs.at(1).valid);
    QVERIFY(result.legs.at(2).valid);
    QCOMPARE(result.legs.at(2).fromPointIndex, 1);
    QVERIFY(!result.legs.at(2).startsAtHome);
    QCOMPARE(result.totalDistanceMeters, result.missionDistanceMeters);
    QCOMPARE(result.lastToHomeDistanceMeters, 0.0);
}

void FlightPlannerRouteMetricsTest::verticalAndDatelineLegsRemainFinite()
{
    const Metrics::Coordinate home{0.0, 179.999, 10.0};
    const QVector<Metrics::RoutePoint> points{
        point(0.0, -179.999, 60.0),
        point(0.0, -179.999, 110.0),
    };
    const Metrics::Result result = Metrics::Calculate(home, true, points);

    verifyNear(result.legs.at(0).horizontalDistanceMeters,
               222.3898533, 0.002);
    verifyNear(result.legs.at(0).bearingDegrees, 90.0, 0.0001);
    QCOMPARE(result.legs.at(1).horizontalDistanceMeters, 0.0);
    QCOMPARE(result.legs.at(1).distanceMeters, 50.0);
    QCOMPARE(result.legs.at(1).gradientPercent, 0.0);
    QCOMPARE(result.legs.at(1).angleDegrees, 0.0);
    QCOMPARE(result.legs.at(1).bearingDegrees, 0.0);
    QVERIFY(std::isfinite(result.totalDistanceMeters));
}

void FlightPlannerRouteMetricsTest::loiterTurnsFollowMissionPlannerRadiusRules()
{
    Metrics::RoutePoint commandRadius = point(0.0, 0.001, 0.0);
    commandRadius.loiterTurnsCommand = true;
    commandRadius.loiterTurns = 2.0;
    commandRadius.commandLoiterRadiusMeters = -5.0;
    Metrics::RoutePoint configuredRadius = point(0.0, 0.002, 0.0);
    configuredRadius.loiterTurnsCommand = true;
    configuredRadius.loiterTurns = 1.5;
    configuredRadius.commandLoiterRadiusMeters = 0.0;
    const QVector<Metrics::RoutePoint> points{
        commandRadius, configuredRadius};
    Metrics::Options options;
    options.configuredLoiterRadiusMeters = -10.0;

    const Metrics::Result plane = Metrics::Calculate(
        {0.0, 0.0, 0.0}, true, points, options);
    const double expectedCommand = 2.0 * 3.14159265358979323846 * 5.0 * 2.0;
    const double expectedConfigured =
        2.0 * 3.14159265358979323846 * 10.0 * 1.5;
    verifyNear(plane.legs.at(0).additionalDistanceMeters,
               expectedCommand, 0.000001);
    verifyNear(plane.legs.at(1).additionalDistanceMeters,
               expectedConfigured, 0.000001);
    verifyNear(plane.additionalDistanceMeters,
               expectedCommand + expectedConfigured, 0.000001);
    // First geometric Home leg is omitted from the MP-compatible total, but
    // loiter distance attached to that first item is retained.
    verifyNear(plane.missionDistanceMeters,
               plane.legs.at(1).distanceMeters
                   + expectedCommand + expectedConfigured,
               0.000001);
    verifyNear(plane.lastLegDistanceMeters,
               plane.legs.at(1).distanceMeters, 0.000001);

    options.useConfiguredLoiterRadiusWhenCommandRadiusIsZero = false;
    const Metrics::Result copter = Metrics::Calculate(
        {0.0, 0.0, 0.0}, true, points, options);
    QCOMPARE(copter.legs.at(1).additionalDistanceMeters, 0.0);
    verifyNear(copter.additionalDistanceMeters, expectedCommand, 0.000001);

    configuredRadius.loiterTurns = -1.0;
    QCOMPARE(Metrics::AdditionalLoiterDistanceMeters(
                 configuredRadius, options),
             0.0);
}

QTEST_APPLESS_MAIN(FlightPlannerRouteMetricsTest)
#include "test_flightplannerroutemetrics.moc"
