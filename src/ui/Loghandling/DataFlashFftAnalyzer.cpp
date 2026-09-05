#include "DataFlashFftAnalyzer.h"

#include "DataFlashFftCore.h"

#include <QMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
bool cancelled(const DataFlashFftAnalyzer::Options &options)
{
    return options.isCancelled && options.isCancelled();
}

DataFlashFftAnalyzer::Result cancelledResult()
{
    DataFlashFftAnalyzer::Result result;
    result.cancelled = true;
    result.error = QStringLiteral("FFT calculation was cancelled.");
    return result;
}

bool validSegment(const DataFlashFftAnalyzer::Segment &segment)
{
    return std::isfinite(segment.sampleRateHz)
        && segment.sampleRateHz > 0.0
        && segment.x.size() == segment.y.size()
        && segment.x.size() == segment.z.size();
}

bool ratesMatch(double first, double second)
{
    return std::abs(first - second)
        <= std::max(0.01, std::max(first, second) * 1.0e-6);
}

struct RateGroup
{
    double sampleRateHz = 0.0;
    int windowCount = 0;
    QVector<const DataFlashFftAnalyzer::Segment *> segments;
};

RateGroup dominantRateGroup(const DataFlashFftAnalyzer::SensorData &sensor,
                            int fftSize,
                            const DataFlashFftAnalyzer::Options &options,
                            bool *wasCancelled)
{
    QMap<double, RateGroup> groups;
    for (const DataFlashFftAnalyzer::Segment &segment : sensor.segments) {
        if (cancelled(options)) {
            if (wasCancelled) {
                *wasCancelled = true;
            }
            return {};
        }
        if (!validSegment(segment)) {
            continue;
        }
        const int windows = segment.x.size() / fftSize;
        if (windows <= 0) {
            continue;
        }
        auto group = groups.lowerBound(segment.sampleRateHz);
        auto previous = group;
        if (previous != groups.begin()) {
            --previous;
        } else {
            previous = groups.end();
        }
        const bool nextMatches = group != groups.end()
            && ratesMatch(group.key(), segment.sampleRateHz);
        const bool previousMatches = previous != groups.end()
            && ratesMatch(previous.key(), segment.sampleRateHz);
        if (!nextMatches && !previousMatches) {
            RateGroup newGroup;
            newGroup.sampleRateHz = segment.sampleRateHz;
            group = groups.insert(segment.sampleRateHz, std::move(newGroup));
        } else if (!nextMatches
                   || (previousMatches
                       && std::abs(previous.key() - segment.sampleRateHz)
                           <= std::abs(group.key() - segment.sampleRateHz))) {
            group = previous;
        }
        group->segments.append(&segment);
        group->windowCount += windows;
    }
    RateGroup best;
    for (auto group = groups.cbegin(); group != groups.cend(); ++group) {
        if (group->windowCount > best.windowCount) {
            best = group.value();
        }
    }
    return best;
}

bool hasUsableSensor(const QVector<DataFlashFftAnalyzer::SensorData> &sensors,
                     bool batch, int fftSize,
                     const DataFlashFftAnalyzer::Options &options,
                     bool *wasCancelled)
{
    for (const DataFlashFftAnalyzer::SensorData &sensor : sensors) {
        if (sensor.batch == batch
            && dominantRateGroup(sensor, fftSize, options,
                                 wasCancelled).windowCount > 0) {
            return true;
        }
        if (wasCancelled && *wasCancelled) {
            return false;
        }
    }
    return false;
}
}

DataFlashFftAnalyzer::Result DataFlashFftAnalyzer::Analyze(
    const QVector<SensorData> &sensors)
{
    return Analyze(sensors, Options());
}

DataFlashFftAnalyzer::Result DataFlashFftAnalyzer::Analyze(
    const QVector<SensorData> &sensors, const Options &options)
{
    Result result;
    if (cancelled(options)) {
        return cancelledResult();
    }
    if (!DataFlashFftCore::IsValidBins(options.bins)) {
        result.error = QStringLiteral("FFT bins must be between 4 and 14.");
        return result;
    }
    if (!std::isfinite(options.startFrequencyHz)
        || options.startFrequencyHz < 0.0
        || options.startFrequencyHz > 1000.0) {
        result.error = QStringLiteral(
            "FFT start frequency must be between 0 and 1000 Hz.");
        return result;
    }
    if (options.maximumInputSamples <= 0
        || options.maximumInputSamples > MaximumInputSamples) {
        result.error = QStringLiteral("FFT input sample limit is invalid.");
        return result;
    }

    quint64 totalSamples = 0;
    for (const SensorData &sensor : sensors) {
        for (const Segment &segment : sensor.segments) {
            if (cancelled(options)) {
                return cancelledResult();
            }
            if (segment.x.size() != segment.y.size()
                || segment.x.size() != segment.z.size()) {
                result.error = QStringLiteral(
                    "FFT sensor axes contain different sample counts.");
                return result;
            }
            totalSamples += static_cast<quint64>(segment.x.size());
            if (totalSamples
                > static_cast<quint64>(options.maximumInputSamples)) {
                result.error = QStringLiteral(
                    "FFT input exceeds the bounded %1-sample limit.")
                                   .arg(options.maximumInputSamples);
                return result;
            }
        }
    }

    const int fftSize = 1 << options.bins;
    bool scanCancelled = false;
    const bool useBatch = hasUsableSensor(
        sensors, true, fftSize, options, &scanCancelled);
    if (scanCancelled) {
        return cancelledResult();
    }
    if (!useBatch
        && !hasUsableSensor(sensors, false, fftSize, options,
                            &scanCancelled)) {
        if (scanCancelled) {
            return cancelledResult();
        }
        result.error = QStringLiteral(
            "No complete ISBH/ISBD batch or IMU data has %1 contiguous samples. "
            "Lower Bins or increase INS_LOG_BAT_CNT so each logged batch "
            "contains enough samples.")
                           .arg(fftSize);
        return result;
    }
    result.source = useBatch ? QStringLiteral("ISBH/ISBD")
                             : QStringLiteral("IMU");

    double bestGyroValue = -std::numeric_limits<double>::infinity();
    for (const SensorData &sensor : sensors) {
        if (sensor.batch != useBatch || sensor.label.trimmed().isEmpty()) {
            continue;
        }
        if (cancelled(options)) {
            return cancelledResult();
        }
        const RateGroup group = dominantRateGroup(
            sensor, fftSize, options, &scanCancelled);
        if (scanCancelled) {
            return cancelledResult();
        }
        if (group.windowCount <= 0) {
            continue;
        }

        const QVector<double> frequencies =
            DataFlashFftCore::FrequencyTable(fftSize, group.sampleRateHz);
        if (frequencies.isEmpty()) {
            continue;
        }
        QVector<double> sums[3] = {
            QVector<double>(fftSize / 2, 0.0),
            QVector<double>(fftSize / 2, 0.0),
            QVector<double>(fftSize / 2, 0.0)};
        int windowsDone = 0;
        for (const Segment *segment : group.segments) {
            const QVector<double> *axes[] = {
                &segment->x, &segment->y, &segment->z};
            const int segmentWindows = segment->x.size() / fftSize;
            for (int window = 0; window < segmentWindows; ++window) {
                if (cancelled(options)) {
                    return cancelledResult();
                }
                const int offset = window * fftSize;
                for (int axis = 0; axis < 3; ++axis) {
                    QVector<double> input(fftSize);
                    std::copy(axes[axis]->cbegin() + offset,
                              axes[axis]->cbegin() + offset + fftSize,
                              input.begin());
                    QVector<double> transformed;
                    QString transformError;
                    if (!DataFlashFftCore::Transform(
                            input, options.bins, !options.magnitude,
                            &transformed, &transformError)) {
                        result.error = transformError;
                        return result;
                    }
                    for (int bin = 0; bin < transformed.size(); ++bin) {
                        sums[axis][bin] += transformed.at(bin);
                    }
                }
                ++windowsDone;
            }
        }
        if (windowsDone == 0) {
            continue;
        }

        static const char *const axisNames[] = {"x", "y", "z"};
        for (int axis = 0; axis < 3; ++axis) {
            Series series;
            series.label = sensor.label + QLatin1Char(' ')
                + QLatin1String(axisNames[axis]);
            series.frequenciesHz = frequencies;
            series.values = sums[axis];
            series.sampleRateHz = group.sampleRateHz;
            for (int bin = 0; bin < series.values.size(); ++bin) {
                series.values[bin] /= windowsDone;
                if (series.frequenciesHz.at(bin)
                    < options.startFrequencyHz) {
                    series.values[bin] = 0.0;
                }
            }
            if (sensor.label.contains(QStringLiteral("GYR"),
                                      Qt::CaseInsensitive)) {
                for (int bin = 0; bin < series.values.size(); ++bin) {
                    if (series.frequenciesHz.at(bin)
                            >= options.startFrequencyHz
                        && series.values.at(bin) > bestGyroValue) {
                        bestGyroValue = series.values.at(bin);
                        result.suggestedNotchHz =
                            series.frequenciesHz.at(bin);
                    }
                }
            }
            result.series.append(std::move(series));
        }
        if (result.sampleRateHz <= 0.0) {
            result.sampleRateHz = group.sampleRateHz;
        }
    }

    if (result.series.isEmpty()) {
        result.error = QStringLiteral("No usable FFT sensor series were produced.");
        return result;
    }
    result.succeeded = true;
    return result;
}
