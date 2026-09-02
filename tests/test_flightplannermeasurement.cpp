#include "ui/flightplanner/FlightPlannerMeasurement.h"
#include "ui/map/PlannerMeasurementOverlay.h"

#include <QGraphicsItemGroup>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QtTest>

#include <cmath>
#include <limits>

namespace
{
void verifyNear(double actual, double expected, double tolerance)
{
    QVERIFY2(std::abs(actual - expected) <= tolerance,
             qPrintable(QStringLiteral("actual=%1 expected=%2 tolerance=%3")
                            .arg(actual, 0, 'g', 16)
                            .arg(expected, 0, 'g', 16)
                            .arg(tolerance, 0, 'g', 16)));
}
}

class FlightPlannerMeasurementTest final : public QObject
{
    Q_OBJECT

private slots:
    void stateMachineRejectsInvalidPointsAndCompletesTwoClicks();
    void formulasMatchOriginalMercatorProjection();
    void antimeridianAndCoincidentPointsRemainStable();
    void presentationUsesLiveMissionPlannerUnits();
    void overlayShowsOriginalMarkerLineLifecycle();
};

void FlightPlannerMeasurementTest::
stateMachineRejectsInvalidPointsAndCompletesTwoClicks()
{
    FlightPlannerMeasurement measurement;
    FlightPlannerMeasurement::Result result;
    QCOMPARE(measurement.AddPoint(
                 std::numeric_limits<double>::quiet_NaN(), 10.0, &result),
             FlightPlannerMeasurement::Step::Rejected);
    QVERIFY(!measurement.IsActive());
    QVERIFY(!result.valid);

    // Unlike PointLatLng.IsEmpty in the legacy implementation, (0, 0) is a
    // valid geographic start and must not be mistaken for no measurement.
    QCOMPARE(measurement.AddPoint(0.0, 0.0, &result),
             FlightPlannerMeasurement::Step::Started);
    QVERIFY(measurement.IsActive());
    QCOMPARE(measurement.Start().latitude, 0.0);
    QCOMPARE(measurement.Start().longitude, 0.0);

    QCOMPARE(measurement.AddPoint(0.0, 1.0, &result),
             FlightPlannerMeasurement::Step::Completed);
    QVERIFY(!measurement.IsActive());
    QVERIFY(result.valid);
    QCOMPARE(result.start.latitude, 0.0);
    QCOMPARE(result.end.longitude, 1.0);

    measurement.AddPoint(10.0, 20.0);
    QVERIFY(measurement.IsActive());
    measurement.Reset();
    QVERIFY(!measurement.IsActive());
}

void FlightPlannerMeasurementTest::
formulasMatchOriginalMercatorProjection()
{
    const auto east = FlightPlannerMeasurement::Calculate(
        {0.0, 0.0}, {0.0, 1.0});
    QVERIFY(east.valid);
    verifyNear(east.distanceMeters, 111319.49079327357, 0.000001);
    verifyNear(east.bearingDegrees, 90.0, 0.000001);

    const auto north = FlightPlannerMeasurement::Calculate(
        {10.0, 20.0}, {11.0, 20.0});
    QVERIFY(north.valid);
    verifyNear(north.bearingDegrees, 0.0, 0.000001);

    const auto west = FlightPlannerMeasurement::Calculate(
        {0.0, 1.0}, {0.0, 0.0});
    QVERIFY(west.valid);
    verifyNear(west.bearingDegrees, 270.0, 0.000001);
}

void FlightPlannerMeasurementTest::
antimeridianAndCoincidentPointsRemainStable()
{
    const auto wrapped = FlightPlannerMeasurement::Calculate(
        {0.0, 179.9}, {0.0, -179.9});
    QVERIFY(wrapped.valid);
    verifyNear(wrapped.distanceMeters, 22263.89815865, 0.0001);
    verifyNear(wrapped.bearingDegrees, 90.0, 0.000001);

    const auto same = FlightPlannerMeasurement::Calculate(
        {-35.0, 120.0}, {-35.0, 120.0});
    QVERIFY(same.valid);
    QCOMPARE(same.distanceMeters, 0.0);
    QCOMPARE(same.bearingDegrees, 0.0);

    const auto invalid = FlightPlannerMeasurement::Calculate(
        {91.0, 0.0}, {0.0, 0.0});
    QVERIFY(!invalid.valid);
}

void FlightPlannerMeasurementTest::presentationUsesLiveMissionPlannerUnits()
{
    QCOMPARE(FlightPlannerMeasurement::FormatDistance(111.319, 1.0, "m"),
             QStringLiteral("111.32 m"));
    QCOMPARE(FlightPlannerMeasurement::FormatDistance(
                 111.319, 3.280839895013123, "ft"),
             QStringLiteral("365.22 ft"));
    QVERIFY(FlightPlannerMeasurement::FormatDistance(
                -1.0, 1.0, "m").isEmpty());
    QVERIFY(FlightPlannerMeasurement::FormatDistance(
                1.0, 0.0, "m").isEmpty());
}

void FlightPlannerMeasurementTest::
overlayShowsOriginalMarkerLineLifecycle()
{
    QGraphicsScene scene;
    auto *group = new QGraphicsItemGroup;
    scene.addItem(group);

    MissionPlanner::PlannerMeasurementOverlay::Rebuild(
        group, {QPointF(10.0, 20.0)});
    QCOMPARE(group->childItems().size(), 1);
    auto *startMarker = dynamic_cast<QGraphicsPathItem *>(
        group->childItems().constFirst());
    QVERIFY(startMarker);
    QCOMPARE(startMarker->brush().color(), QColor(220, 32, 32));

    MissionPlanner::PlannerMeasurementOverlay::Rebuild(
        group, {QPointF(10.0, 20.0), QPointF(110.0, 70.0)});
    QCOMPARE(group->childItems().size(), 3);
    int redMarkers = 0;
    int greenLines = 0;
    for (QGraphicsItem *item : group->childItems()) {
        auto *path = dynamic_cast<QGraphicsPathItem *>(item);
        QVERIFY(path);
        if (path->brush().color() == QColor(220, 32, 32)) ++redMarkers;
        if (path->pen().color() == QColor(0, 160, 0)) ++greenLines;
    }
    QCOMPARE(redMarkers, 2);
    QCOMPARE(greenLines, 1);

    MissionPlanner::PlannerMeasurementOverlay::Rebuild(group, {});
    QVERIFY(group->childItems().isEmpty());
}

QTEST_MAIN(FlightPlannerMeasurementTest)
#include "test_flightplannermeasurement.moc"
