#ifndef HELIVISUALIZATION_H
#define HELIVISUALIZATION_H

#include <QVector>

/**
 * Pure calculations used by Mission Planner's traditional-helicopter setup
 * display. Values and formulas intentionally follow MP10 HeliVisualization;
 * no vehicle, parameter-cache or widget state is owned here.
 */
namespace HeliVisualization {

struct CurvePoint
{
    double inputPercent = 0.0;
    double output = 0.0;
};

struct InputRange
{
    // This reversed default is MP10's empty range sentinel.
    double minimum = 2200.0;
    double maximum = 800.0;

    bool hasSamples() const { return minimum <= maximum; }
};

QVector<CurvePoint> BuildStabilizeCurve(double point0, double point40,
                                        double point60, double point100);
QVector<CurvePoint> BuildAcroCurve(double expo);

double MapCollectiveCursor(double pwm, double minimum, double maximum);
InputRange CaptureRange(const InputRange &current, double pwm,
                        bool manualServoActive);

} // namespace HeliVisualization

#endif // HELIVISUALIZATION_H
