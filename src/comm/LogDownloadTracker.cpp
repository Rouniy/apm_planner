#include "LogDownloadTracker.h"

#include <algorithm>
#include <limits>

LogDownloadTracker::LogDownloadTracker(
        std::optional<quint32> trustedTotalLength)
{
    reset(trustedTotalLength);
}

LogDownloadTracker::AddResult LogDownloadTracker::add(
        quint32 offset, quint8 count, bool inferTotalLength)
{
    if (count > PacketSize) {
        return AddResult::InvalidRange;
    }

    const quint64 end = quint64(offset) + count;
    if (end > std::numeric_limits<quint32>::max()) {
        return AddResult::InvalidRange;
    }

    std::size_t first = 0;
    std::size_t last = 0;
    if (count != 0
            && !canMerge(ByteRange{offset, end}, &first, &last)) {
        return AddResult::RangeCapacityExceeded;
    }

    const bool nearFrontier = quint64(offset) <= frontierEnd() + InferenceSlack;
    if (inferTotalLength && count < PacketSize && end >= m_highestEnd) {
        if (nearFrontier) {
            m_totalLength = static_cast<quint32>(end);
        } else if (!m_pendingTotalLength
                   || end > *m_pendingTotalLength) {
            // Keep the largest of a final run of candidates. A smaller corrupt
            // candidate arriving afterwards must not truncate the download.
            m_pendingTotalLength = static_cast<quint32>(end);
        }
    } else {
        // The stream continued, so a previous far short candidate was corrupt.
        m_pendingTotalLength.reset();
    }

    // Far corrupt offsets do not raise the bar that the real end must clear.
    if (nearFrontier) {
        m_highestEnd = std::max(m_highestEnd, end);
    }

    if (count != 0) {
        merge(ByteRange{offset, end}, first, last);
    }
    return AddResult::Accepted;
}

bool LogDownloadTracker::acceptPendingTotalLength()
{
    if (m_totalLength || !m_pendingTotalLength) {
        return false;
    }

    m_totalLength = m_pendingTotalLength;
    m_pendingTotalLength.reset();
    return true;
}

LogDownloadRequest LogDownloadTracker::nextRequest(
        quint32 maximumKnownLength) const
{
    const quint64 limit = m_totalLength
            ? quint64(*m_totalLength)
            : quint64(std::numeric_limits<quint32>::max());
    quint64 cursor = 0;
    quint64 missingEnd = limit;

    for (const ByteRange &range : m_ranges) {
        if (range.start > cursor) {
            missingEnd = std::min(range.start, limit);
            break;
        }

        cursor = std::max(cursor, range.end);
        if (cursor >= limit) {
            cursor = limit;
            break;
        }
    }

    const quint32 offset = static_cast<quint32>(
            std::min(cursor, quint64(std::numeric_limits<quint32>::max())));
    if (!m_totalLength) {
        return {offset, std::numeric_limits<quint32>::max()};
    }

    const quint64 remaining = missingEnd > cursor ? missingEnd - cursor : 0;
    return {offset, static_cast<quint32>(
                    std::min(remaining, quint64(maximumKnownLength)))};
}

bool LogDownloadTracker::complete() const
{
    return m_totalLength && coveredBytes() >= *m_totalLength;
}

quint64 LogDownloadTracker::coveredBytes() const
{
    const quint64 limit = m_totalLength
            ? quint64(*m_totalLength)
            : std::numeric_limits<quint64>::max();
    quint64 covered = 0;

    for (const ByteRange &range : m_ranges) {
        if (range.start >= limit) {
            break;
        }
        covered += std::min(range.end, limit) - range.start;
    }
    return covered;
}

void LogDownloadTracker::reset(
        std::optional<quint32> trustedTotalLength)
{
    m_ranges.clear();
    m_highestEnd = 0;
    m_pendingTotalLength.reset();
    m_totalLength = trustedTotalLength;
}

quint32 LogDownloadTracker::frontierEnd() const
{
    if (m_ranges.empty() || m_ranges.front().start != 0) {
        return 0;
    }
    return static_cast<quint32>(m_ranges.front().end);
}

bool LogDownloadTracker::canMerge(const ByteRange &incoming,
                                  std::size_t *first,
                                  std::size_t *last) const
{
    std::size_t begin = 0;
    while (begin < m_ranges.size()
           && m_ranges[begin].end < incoming.start) {
        ++begin;
    }

    std::size_t end = begin;
    while (end < m_ranges.size()
           && m_ranges[end].start <= incoming.end) {
        ++end;
    }

    const std::size_t mergedCount = m_ranges.size() - (end - begin) + 1;
    if (mergedCount > MaximumRanges) {
        return false;
    }

    *first = begin;
    *last = end;
    return true;
}

void LogDownloadTracker::merge(ByteRange incoming, std::size_t first,
                               std::size_t last)
{
    for (std::size_t index = first; index < last; ++index) {
        incoming.start = std::min(incoming.start, m_ranges[index].start);
        incoming.end = std::max(incoming.end, m_ranges[index].end);
    }

    const auto firstIterator = m_ranges.begin()
            + static_cast<std::ptrdiff_t>(first);
    const auto lastIterator = m_ranges.begin()
            + static_cast<std::ptrdiff_t>(last);
    const auto insertionPoint = m_ranges.erase(firstIterator, lastIterator);
    m_ranges.insert(insertionPoint, incoming);
}
