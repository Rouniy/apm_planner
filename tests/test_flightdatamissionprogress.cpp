#include "ui/flightdata/FlightDataMissionProgress.h"

#include <QtTest/QTest>

class FlightDataMissionProgressTest final : public QObject
{
    Q_OBJECT

private slots:
    void emptyMissionHasNoProgress();
    void pathProgressIsSortedAndDistanceBased();
    void completedMissionIsClamped();
};

void FlightDataMissionProgressTest::emptyMissionHasNoProgress()
{
    const FlightDataMissionProgress result =
        FlightDataMissionProgressCalculator::Calculate(
            47.0, 8.0, {}, 0, 0.0);
    QCOMPARE(result.itemCount, 0);
    QCOMPARE(result.totalDistanceMeters, 0.0);
    QCOMPARE(result.travelledDistanceMeters, 0.0);
    QCOMPARE(result.percent, 0.0);
}

void FlightDataMissionProgressTest::pathProgressIsSortedAndDistanceBased()
{
    // Deliberately reverse the input: sequence, not container order, defines
    // the mission path.
    const QVector<FlightDataMissionPoint> points{
        {2, 47.002, 8.0},
        {1, 47.001, 8.0},
        {0, 91.0, 8.0},
    };
    const FlightDataMissionProgress result =
        FlightDataMissionProgressCalculator::Calculate(
            47.0, 8.0, points, 1, 0.0);
    QCOMPARE(result.itemCount, 2);
    QVERIFY(result.totalDistanceMeters > 200.0);
    QVERIFY(result.travelledDistanceMeters > 100.0);
    QVERIFY(qAbs(result.percent - 50.0) < 0.1);
}

void FlightDataMissionProgressTest::completedMissionIsClamped()
{
    const QVector<FlightDataMissionPoint> points{
        {1, 47.001, 8.0},
        {2, 47.002, 8.0},
    };
    const FlightDataMissionProgress result =
        FlightDataMissionProgressCalculator::Calculate(
            47.0, 8.0, points, 99, -50.0);
    QCOMPARE(result.itemCount, 2);
    QCOMPARE(result.travelledDistanceMeters, result.totalDistanceMeters);
    QCOMPARE(result.percent, 100.0);
}

QTEST_APPLESS_MAIN(FlightDataMissionProgressTest)
#include "test_flightdatamissionprogress.moc"
