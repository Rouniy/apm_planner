#include "HeliVisualization.h"

#include <algorithm>
#include <cmath>

namespace {

double finiteOrZero(double value)
{
    return std::isfinite(value) ? value : 0.0;
}

} // namespace

namespace HeliVisualization {

QVector<CurvePoint> BuildStabilizeCurve(double point0, double point40,
                                        double point60, double point100)
{
    return {
        {0.0, finiteOrZero(point0)},
        {40.0, finiteOrZero(point40)},
        {60.0, finiteOrZero(point60)},
        {100.0, finiteOrZero(point100)},
    };
}

QVector<CurvePoint> BuildAcroCurve(double expo)
{
    expo = std::isfinite(expo) ? std::clamp(expo, 0.0, 1.0) : 0.0;

    QVector<CurvePoint> points;
    points.reserve(101);
    for (int input = 0; input <= 100; ++input) {
        const double normalized = (static_cast<double>(input) - 50.0) / 50.0;
        const double shaped = expo * normalized * normalized * normalized
            + (1.0 - expo) * normalized;
        points.append({static_cast<double>(input), 500.0 + shaped * 500.0});
    }
    return points;
}

double MapCollectiveCursor(double pwm, double minimum, double maximum)
{
    if (!std::isfinite(pwm) || !std::isfinite(minimum)
        || !std::isfinite(maximum) || maximum <= minimum) {
        return 0.0;
    }
    return std::clamp((pwm - minimum) * 100.0 / (maximum - minimum),
                      0.0, 100.0);
}

InputRange CaptureRange(const InputRange &current, double pwm,
                        bool manualServoActive)
{
    if (!manualServoActive || !std::isfinite(pwm)
        || pwm < 800.0 || pwm > 2200.0) {
        return current;
    }
    if (!current.hasSamples()) {
        return {pwm, pwm};
    }
    return {std::min(current.minimum, pwm),
            std::max(current.maximum, pwm)};
}

} // namespace HeliVisualization
