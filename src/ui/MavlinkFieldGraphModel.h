#ifndef MAVLINKFIELDGRAPHMODEL_H
#define MAVLINKFIELDGRAPHMODEL_H

#include "MavlinkGraphSampleExtractor.h"

#include <QString>
#include <QVector>

#include <deque>
#include <vector>

struct MavlinkGraphPoint
{
    double seconds = 0.0;
    double value = 0.0;
};

struct MavlinkGraphSeriesSnapshot
{
    int index = 0;
    QString label;
    QVector<MavlinkGraphPoint> points;
};

/** Bounded, transport-independent data model for one selected MAVLink field. */
class MavlinkFieldGraphModel final
{
public:
    static constexpr int MinimumHistory = 10;
    static constexpr int MaximumHistory = 100000;
    static constexpr int DefaultHistory = 500;
    static constexpr int MaximumPlottedPointsPerSeries = 2000;

    explicit MavlinkFieldGraphModel(
        const MavlinkGraphSelection &selection,
        int history = DefaultHistory);

    // Appends one point to every scalar/array series only when the packet and
    // monotonic timestamp are valid. Source identity is pinned by the caller.
    bool observe(const mavlink_message_t &message, double monotonicSeconds);
    void clear();

    const MavlinkGraphSelection &selection() const { return m_selection; }
    int history() const { return m_history; }
    int seriesCount() const;
    int storedPointCount(int seriesIndex) const;
    bool latestValue(int seriesIndex, MavlinkGraphPoint *point) const;

    // Returns chronological plot data. Large histories are projected into
    // bounded buckets which retain their minimum and maximum points; the full
    // configured history remains stored in the model.
    QVector<MavlinkGraphSeriesSnapshot> snapshot(
        int maximumPointsPerSeries = MaximumPlottedPointsPerSeries) const;

private:
    struct StoredSeries
    {
        QString label;
        std::deque<MavlinkGraphPoint> points;
    };

    static QVector<MavlinkGraphPoint> downsample(
        const std::deque<MavlinkGraphPoint> &points, int maximumPoints);
    QString labelFor(int index, int count) const;

    MavlinkGraphSelection m_selection;
    int m_history = DefaultHistory;
    bool m_hasLastSeconds = false;
    double m_lastSeconds = 0.0;
    std::vector<StoredSeries> m_series;
};

#endif // MAVLINKFIELDGRAPHMODEL_H
