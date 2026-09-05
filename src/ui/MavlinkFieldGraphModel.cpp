#include "MavlinkFieldGraphModel.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

MavlinkFieldGraphModel::MavlinkFieldGraphModel(
    const MavlinkGraphSelection &selection, int history)
    : m_selection(selection)
    , m_history(qBound(MinimumHistory, history, MaximumHistory))
{
}

bool MavlinkFieldGraphModel::observe(const mavlink_message_t &message,
                                     double monotonicSeconds)
{
    if (!m_selection.matches(message)
        || !std::isfinite(monotonicSeconds)
        || monotonicSeconds < 0.0
        || (m_hasLastSeconds && monotonicSeconds < m_lastSeconds)) {
        return false;
    }

    QVector<double> values;
    if (!MavlinkGraphSampleExtractor::tryRead(
            message, m_selection.fieldName, &values)) {
        return false;
    }

    if (m_series.empty()) {
        m_series.reserve(static_cast<size_t>(values.size()));
        for (int index = 0; index < values.size(); ++index) {
            StoredSeries series;
            series.label = labelFor(index, values.size());
            m_series.push_back(std::move(series));
        }
    } else if (static_cast<int>(m_series.size()) != values.size()) {
        return false;
    }

    for (int index = 0; index < values.size(); ++index) {
        std::deque<MavlinkGraphPoint> &points =
            m_series[static_cast<size_t>(index)].points;
        points.push_back({monotonicSeconds, values.at(index)});
        if (static_cast<int>(points.size()) > m_history) {
            points.pop_front();
        }
    }
    m_lastSeconds = monotonicSeconds;
    m_hasLastSeconds = true;
    return true;
}

void MavlinkFieldGraphModel::clear()
{
    m_series.clear();
    m_hasLastSeconds = false;
    m_lastSeconds = 0.0;
}

int MavlinkFieldGraphModel::seriesCount() const
{
    return static_cast<int>(m_series.size());
}

int MavlinkFieldGraphModel::storedPointCount(int seriesIndex) const
{
    if (seriesIndex < 0
        || seriesIndex >= static_cast<int>(m_series.size())) {
        return 0;
    }
    return static_cast<int>(
        m_series[static_cast<size_t>(seriesIndex)].points.size());
}

bool MavlinkFieldGraphModel::latestValue(
    int seriesIndex, MavlinkGraphPoint *point) const
{
    if (!point || seriesIndex < 0
        || seriesIndex >= static_cast<int>(m_series.size())) {
        return false;
    }
    const std::deque<MavlinkGraphPoint> &points =
        m_series[static_cast<size_t>(seriesIndex)].points;
    if (points.empty()) {
        return false;
    }
    *point = points.back();
    return true;
}

QVector<MavlinkGraphSeriesSnapshot> MavlinkFieldGraphModel::snapshot(
    int maximumPointsPerSeries) const
{
    const int limit = qBound(
        0, maximumPointsPerSeries, MaximumPlottedPointsPerSeries);
    QVector<MavlinkGraphSeriesSnapshot> result;
    result.reserve(static_cast<int>(m_series.size()));
    for (size_t index = 0; index < m_series.size(); ++index) {
        MavlinkGraphSeriesSnapshot series;
        series.index = static_cast<int>(index);
        series.label = m_series[index].label;
        series.points = downsample(m_series[index].points, limit);
        result.append(std::move(series));
    }
    return result;
}

QVector<MavlinkGraphPoint> MavlinkFieldGraphModel::downsample(
    const std::deque<MavlinkGraphPoint> &points, int maximumPoints)
{
    QVector<MavlinkGraphPoint> result;
    if (maximumPoints <= 0 || points.empty()) {
        return result;
    }
    if (points.size() <= static_cast<size_t>(maximumPoints)) {
        result.reserve(static_cast<int>(points.size()));
        for (const MavlinkGraphPoint &point : points) {
            result.append(point);
        }
        return result;
    }
    if (maximumPoints == 1) {
        result.append(points.back());
        return result;
    }

    result.reserve(maximumPoints);
    result.append(points.front());
    if (maximumPoints == 2) {
        result.append(points.back());
        return result;
    }

    const size_t interiorCount = points.size() - 2U;
    if (maximumPoints == 3) {
        auto minimum = points.begin() + 1;
        auto maximum = minimum;
        for (auto it = minimum; it != points.end() - 1; ++it) {
            if (it->value < minimum->value) {
                minimum = it;
            }
            if (it->value > maximum->value) {
                maximum = it;
            }
        }
        const double firstValue = points.front().value;
        const double lastValue = points.back().value;
        const double center = (firstValue + lastValue) / 2.0;
        result.append(std::abs(minimum->value - center)
                              >= std::abs(maximum->value - center)
                          ? *minimum : *maximum);
        result.append(points.back());
        return result;
    }

    // Two chronological points (minimum and maximum) are retained from each
    // bucket. Consequently both global extrema are retained for every limit
    // of four or more, while first/last remain fixed plot anchors.
    const int bucketCount = (maximumPoints - 2) / 2;
    for (int bucket = 0; bucket < bucketCount; ++bucket) {
        const size_t beginIndex = 1U
            + (interiorCount * static_cast<size_t>(bucket))
                  / static_cast<size_t>(bucketCount);
        const size_t endIndex = 1U
            + (interiorCount * static_cast<size_t>(bucket + 1))
                  / static_cast<size_t>(bucketCount);
        size_t minimumIndex = beginIndex;
        size_t maximumIndex = beginIndex;
        for (size_t index = beginIndex + 1U; index < endIndex; ++index) {
            if (points[index].value < points[minimumIndex].value) {
                minimumIndex = index;
            }
            if (points[index].value > points[maximumIndex].value) {
                maximumIndex = index;
            }
        }
        if (minimumIndex == maximumIndex) {
            result.append(points[minimumIndex]);
        } else if (minimumIndex < maximumIndex) {
            result.append(points[minimumIndex]);
            result.append(points[maximumIndex]);
        } else {
            result.append(points[maximumIndex]);
            result.append(points[minimumIndex]);
        }
    }
    result.append(points.back());
    return result;
}

QString MavlinkFieldGraphModel::labelFor(int index, int count) const
{
    const QString base = QStringLiteral("%1.%2")
        .arg(m_selection.messageName, m_selection.fieldName);
    return count == 1
        ? base
        : QStringLiteral("%1[%2]").arg(base).arg(index);
}
