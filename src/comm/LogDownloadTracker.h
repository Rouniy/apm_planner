#ifndef LOGDOWNLOADTRACKER_H
#define LOGDOWNLOADTRACKER_H

#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <vector>

struct LogDownloadRequest
{
    quint32 offset = 0;
    quint32 count = 0;
};

/**
 * Tracks byte coverage for one MAVLink LOG_DATA download.
 *
 * LOG_DATA packets may overlap, arrive out of order, or be duplicated. This
 * transport-free helper keeps disjoint byte ranges and reproduces Mission
 * Planner's conservative short-packet end inference.
 */
class LogDownloadTracker final
{
public:
    static constexpr quint32 PacketSize = 90;
    static constexpr quint64 InferenceSlack = quint64(PacketSize) * 100;
    static constexpr std::size_t MaximumRanges = 4096;

    enum class AddResult
    {
        Accepted,
        InvalidRange,
        RangeCapacityExceeded
    };

    explicit LogDownloadTracker(
            std::optional<quint32> trustedTotalLength = std::nullopt);

    /**
     * Record a valid payload range. Counts over the MAVLink payload capacity
     * and ranges whose exclusive end is not representable by quint32 fail
     * without changing tracker state.
     */
    AddResult add(quint32 offset, quint8 count, bool inferTotalLength);

    /** Promote a far short-packet candidate after the stream goes quiet. */
    bool acceptPendingTotalLength();

    /**
     * Return the first missing request. Unknown-length streams use an
     * unbounded count; known-length repairs are capped by maximumKnownLength.
     */
    LogDownloadRequest nextRequest(quint32 maximumKnownLength) const;

    bool complete() const;
    quint64 coveredBytes() const;
    /** End of the contiguous trusted range beginning at byte zero. */
    quint32 frontierEnd() const;
    std::optional<quint32> totalLength() const { return m_totalLength; }
    int rangeCount() const { return static_cast<int>(m_ranges.size()); }

    /**
     * Clear all coverage and inference state. A supplied length must be from
     * an explicitly trusted source; a LOG_ENTRY advertised size is only a
     * progress hint and must not silently replace short-packet inference.
     */
    void reset(std::optional<quint32> trustedTotalLength = std::nullopt);

private:
    struct ByteRange
    {
        quint64 start = 0;
        quint64 end = 0;
    };

    bool canMerge(const ByteRange &incoming, std::size_t *first,
                  std::size_t *last) const;
    void merge(ByteRange incoming, std::size_t first, std::size_t last);

    std::vector<ByteRange> m_ranges;
    quint64 m_highestEnd = 0;
    std::optional<quint32> m_pendingTotalLength;
    std::optional<quint32> m_totalLength;
};

#endif // LOGDOWNLOADTRACKER_H
