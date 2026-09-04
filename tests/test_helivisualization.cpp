#include <QtTest>

#include "ui/configuration/HeliVisualization.h"

#include <cmath>
#include <limits>

namespace {

using HeliVisualization::CurvePoint;
using HeliVisualization::InputRange;

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

bool near(double actual, double expected, double tolerance = 1.0e-9)
{
    return std::fabs(actual - expected) <= tolerance;
}

void compareRange(const InputRange &actual, double minimum, double maximum)
{
    QCOMPARE(actual.minimum, minimum);
    QCOMPARE(actual.maximum, maximum);
}

} // namespace

class HeliVisualizationTest final : public QObject
{
    Q_OBJECT

private slots:
    void stabilizeCurveUsesOfficialControlPoints();
    void stabilizeCurveReplacesNonFiniteOutputs();
    void acroCurveMatchesMpFormula();
    void acroCurveClampsInvalidExpo();
    void collectiveCursorMapsAndClamps();
    void collectiveCursorRejectsInvalidInput();
    void rangeCaptureRequiresActiveValidManualInput();
    void rangeCaptureStartsAndExpandsInclusively();
};

void HeliVisualizationTest::stabilizeCurveUsesOfficialControlPoints()
{
    const QVector<CurvePoint> curve =
        HeliVisualization::BuildStabilizeCurve(110.0, 420.0, 610.0, 930.0);

    QCOMPARE(curve.size(), 4);
    QCOMPARE(curve.at(0).inputPercent, 0.0);
    QCOMPARE(curve.at(0).output, 110.0);
    QCOMPARE(curve.at(1).inputPercent, 40.0);
    QCOMPARE(curve.at(1).output, 420.0);
    QCOMPARE(curve.at(2).inputPercent, 60.0);
    QCOMPARE(curve.at(2).output, 610.0);
    QCOMPARE(curve.at(3).inputPercent, 100.0);
    QCOMPARE(curve.at(3).output, 930.0);
}

void HeliVisualizationTest::stabilizeCurveReplacesNonFiniteOutputs()
{
    const QVector<CurvePoint> curve =
        HeliVisualization::BuildStabilizeCurve(kNaN, kInf, -kInf, -25.0);

    QCOMPARE(curve.at(0).output, 0.0);
    QCOMPARE(curve.at(1).output, 0.0);
    QCOMPARE(curve.at(2).output, 0.0);
    QCOMPARE(curve.at(3).output, -25.0);
}

void HeliVisualizationTest::acroCurveMatchesMpFormula()
{
    const QVector<CurvePoint> linear = HeliVisualization::BuildAcroCurve(0.0);
    QCOMPARE(linear.size(), 101);
    QCOMPARE(linear.at(0).inputPercent, 0.0);
    QCOMPARE(linear.at(0).output, 0.0);
    QCOMPARE(linear.at(25).output, 250.0);
    QCOMPARE(linear.at(50).output, 500.0);
    QCOMPARE(linear.at(75).output, 750.0);
    QCOMPARE(linear.at(100).inputPercent, 100.0);
    QCOMPARE(linear.at(100).output, 1000.0);

    const QVector<CurvePoint> cubic = HeliVisualization::BuildAcroCurve(1.0);
    QCOMPARE(cubic.size(), 101);
    QCOMPARE(cubic.at(25).output, 437.5);
    QCOMPARE(cubic.at(50).output, 500.0);
    QCOMPARE(cubic.at(75).output, 562.5);

    const QVector<CurvePoint> mixed = HeliVisualization::BuildAcroCurve(0.35);
    const double normalized = (37.0 - 50.0) / 50.0;
    const double expected = 500.0
        + (0.35 * normalized * normalized * normalized
           + 0.65 * normalized) * 500.0;
    QVERIFY(near(mixed.at(37).output, expected));
}

void HeliVisualizationTest::acroCurveClampsInvalidExpo()
{
    const QVector<CurvePoint> below = HeliVisualization::BuildAcroCurve(-1.0);
    const QVector<CurvePoint> above = HeliVisualization::BuildAcroCurve(2.0);
    const QVector<CurvePoint> nan = HeliVisualization::BuildAcroCurve(kNaN);
    const QVector<CurvePoint> inf = HeliVisualization::BuildAcroCurve(kInf);

    QCOMPARE(below.at(25).output, 250.0);
    QCOMPARE(nan.at(25).output, 250.0);
    QCOMPARE(inf.at(25).output, 250.0);
    QCOMPARE(above.at(25).output, 437.5);
}

void HeliVisualizationTest::collectiveCursorMapsAndClamps()
{
    QCOMPARE(HeliVisualization::MapCollectiveCursor(1000.0, 1000.0, 2000.0),
             0.0);
    QCOMPARE(HeliVisualization::MapCollectiveCursor(1500.0, 1000.0, 2000.0),
             50.0);
    QCOMPARE(HeliVisualization::MapCollectiveCursor(2000.0, 1000.0, 2000.0),
             100.0);
    QCOMPARE(HeliVisualization::MapCollectiveCursor(500.0, 1000.0, 2000.0),
             0.0);
    QCOMPARE(HeliVisualization::MapCollectiveCursor(2400.0, 1000.0, 2000.0),
             100.0);
}

void HeliVisualizationTest::collectiveCursorRejectsInvalidInput()
{
    QCOMPARE(HeliVisualization::MapCollectiveCursor(kNaN, 1000.0, 2000.0),
             0.0);
    QCOMPARE(HeliVisualization::MapCollectiveCursor(1500.0, kInf, 2000.0),
             0.0);
    QCOMPARE(HeliVisualization::MapCollectiveCursor(1500.0, 1000.0, -kInf),
             0.0);
    QCOMPARE(HeliVisualization::MapCollectiveCursor(1500.0, 1000.0, 1000.0),
             0.0);
    QCOMPARE(HeliVisualization::MapCollectiveCursor(1500.0, 2000.0, 1000.0),
             0.0);
}

void HeliVisualizationTest::rangeCaptureRequiresActiveValidManualInput()
{
    const InputRange empty;
    QVERIFY(!empty.hasSamples());

    compareRange(HeliVisualization::CaptureRange(empty, 1500.0, false),
                 2200.0, 800.0);
    compareRange(HeliVisualization::CaptureRange(empty, kNaN, true),
                 2200.0, 800.0);
    compareRange(HeliVisualization::CaptureRange(empty, kInf, true),
                 2200.0, 800.0);
    compareRange(HeliVisualization::CaptureRange(empty, 799.0, true),
                 2200.0, 800.0);
    compareRange(HeliVisualization::CaptureRange(empty, 2201.0, true),
                 2200.0, 800.0);
}

void HeliVisualizationTest::rangeCaptureStartsAndExpandsInclusively()
{
    InputRange range = HeliVisualization::CaptureRange(InputRange{}, 1500.0,
                                                       true);
    QVERIFY(range.hasSamples());
    compareRange(range, 1500.0, 1500.0);

    range = HeliVisualization::CaptureRange(range, 1900.0, true);
    range = HeliVisualization::CaptureRange(range, 1100.0, true);
    compareRange(range, 1100.0, 1900.0);

    range = HeliVisualization::CaptureRange(range, 800.0, true);
    range = HeliVisualization::CaptureRange(range, 2200.0, true);
    compareRange(range, 800.0, 2200.0);
}

QTEST_APPLESS_MAIN(HeliVisualizationTest)
#include "test_helivisualization.moc"
