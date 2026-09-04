#include "ui/tools/PropagationDistanceEstimator.h"

#include <QtTest/QtTest>

#include <cmath>
#include <limits>

namespace
{
double missingValue()
{
    return std::numeric_limits<double>::quiet_NaN();
}
}

class PropagationDistanceEstimatorTest : public QObject
{
    Q_OBJECT

private slots:
    void requiresUsedMahMovementAndUsablePercentage();
    void matchesMissionPlannerEstimate();
    void samplesPositionAtMostOncePerSecond();
    void onlyAccumulatesArmedThreeDimensionalFixes();
    void integratesCurrentAndAcceptsAuthoritativeConsumption();
    void disconnectResetsTheWholeEpoch();
};

void PropagationDistanceEstimatorTest::
requiresUsedMahMovementAndUsablePercentage()
{
    PropagationDistanceEstimator estimator;
    estimator.setConnected(true);
    estimator.setArmed(true);
    estimator.setGpsFixType(3);
    estimator.observeBattery(0, 80.0, 10.0, missingValue());
    estimator.observePosition(0, 35.0, 33.0);
    estimator.observePosition(1000, 35.0, 33.001);

    QVERIFY(estimator.travelledMetres() > 0.0);
    QVERIFY(!estimator.usedMahValid());
    QVERIFY(qIsNaN(estimator.kilometresLeft()));

    estimator.observeBattery(1000, 80.0, 10.0, missingValue());
    QVERIFY(estimator.usedMahValid());
    QVERIFY(qIsFinite(estimator.kilometresLeft()));

    estimator.observeBattery(2000, 100.0, 10.0, missingValue());
    QVERIFY(qIsNaN(estimator.kilometresLeft()));
    estimator.observeBattery(3000, -1.0, 10.0, missingValue());
    QVERIFY(qIsNaN(estimator.kilometresLeft()));
}

void PropagationDistanceEstimatorTest::matchesMissionPlannerEstimate()
{
    PropagationDistanceEstimator estimator;
    estimator.setConnected(true);
    estimator.setArmed(true);
    estimator.setGpsFixType(3);
    estimator.observeBattery(0, 80.0, missingValue(), 120.0);
    estimator.observePosition(0, 35.0, 33.0);
    const double estimate = estimator.observePosition(
        1000, 35.0, 33.001);

    QVERIFY(qIsFinite(estimate));
    const double expected = estimator.travelledMetres()
        / 1000.0 * 80.0 / 20.0;
    QVERIFY(std::abs(estimate - expected) < 1.0e-12);
}

void PropagationDistanceEstimatorTest::samplesPositionAtMostOncePerSecond()
{
    PropagationDistanceEstimator estimator;
    estimator.setConnected(true);
    estimator.setArmed(true);
    estimator.setGpsFixType(3);
    estimator.observeBattery(0, 50.0, missingValue(), 1.0);
    estimator.observePosition(0, 35.0, 33.0);
    estimator.observePosition(500, 35.0, 33.001);
    estimator.observePosition(999, 35.0, 33.002);
    QCOMPARE(estimator.travelledMetres(), 0.0);

    estimator.observePosition(1000, 35.0, 33.003);
    const double firstDistance = estimator.travelledMetres();
    QVERIFY(firstDistance > 0.0);
    estimator.observePosition(1500, 35.0, 33.004);
    QCOMPARE(estimator.travelledMetres(), firstDistance);
    estimator.observePosition(2000, 35.0, 33.005);
    QVERIFY(estimator.travelledMetres() > firstDistance);
}

void PropagationDistanceEstimatorTest::
onlyAccumulatesArmedThreeDimensionalFixes()
{
    PropagationDistanceEstimator estimator;
    estimator.setConnected(true);
    estimator.observeBattery(0, 50.0, missingValue(), 50.0);
    estimator.setGpsFixType(3);
    estimator.observePosition(0, 35.0, 33.0);
    estimator.observePosition(1000, 35.0, 33.001);
    QCOMPARE(estimator.travelledMetres(), 0.0);

    estimator.setArmed(true);
    estimator.setGpsFixType(2);
    estimator.observePosition(2000, 35.0, 33.002);
    QCOMPARE(estimator.travelledMetres(), 0.0);

    estimator.setGpsFixType(3);
    estimator.observePosition(3000, 35.0, 33.003);
    QVERIFY(estimator.travelledMetres() > 0.0);
}

void PropagationDistanceEstimatorTest::
integratesCurrentAndAcceptsAuthoritativeConsumption()
{
    PropagationDistanceEstimator estimator;
    estimator.setConnected(true);
    estimator.observeBattery(0, 60.0, 5.0, missingValue());
    QVERIFY(!estimator.usedMahValid());
    estimator.observeBattery(3600, 60.0, 5.0, missingValue());
    QVERIFY(estimator.usedMahValid());
    QVERIFY(std::abs(estimator.usedMah() - 5.0) < 1.0e-12);

    estimator.observeBattery(4000, 60.0, missingValue(), 250.0);
    QCOMPARE(estimator.usedMah(), 250.0);
    estimator.observeBattery(5000, 60.0, -0.01, missingValue());
    QCOMPARE(estimator.usedMah(), 250.0);
}

void PropagationDistanceEstimatorTest::disconnectResetsTheWholeEpoch()
{
    PropagationDistanceEstimator estimator;
    estimator.setConnected(true);
    estimator.setArmed(true);
    estimator.setGpsFixType(3);
    estimator.observeBattery(0, 50.0, missingValue(), 100.0);
    estimator.observePosition(0, 35.0, 33.0);
    estimator.observePosition(1000, 35.0, 33.001);
    QVERIFY(estimator.travelledMetres() > 0.0);
    QVERIFY(qIsFinite(estimator.kilometresLeft()));

    estimator.setConnected(false);
    QVERIFY(!estimator.connected());
    QCOMPARE(estimator.travelledMetres(), 0.0);
    QVERIFY(!estimator.usedMahValid());
    QVERIFY(qIsNaN(estimator.kilometresLeft()));
}

QTEST_APPLESS_MAIN(PropagationDistanceEstimatorTest)
#include "test_propagationdistanceestimator.moc"
