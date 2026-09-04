#ifndef NMEALINEFRAMER_H
#define NMEALINEFRAMER_H

#include <QByteArray>
#include <QList>

struct NmeaLineFramerResult
{
    QList<QByteArray> lines;
    int oversizedLines = 0;
};

/**
 * Transport-neutral bounded line framing for ASCII NMEA byte streams.
 *
 * Fragmented and coalesced chunks are accepted. CRLF and LF terminate lines;
 * the optional CR is removed. Once a record exceeds the bound, the complete
 * record is discarded through its LF so its tail cannot become a new NMEA
 * sentence. The default 4096-byte bound is deliberately much larger than a
 * standard NMEA sentence while keeping hostile or broken inputs bounded.
 */
class NmeaLineFramer final
{
public:
    static constexpr int DefaultMaximumLineBytes = 4096;

    explicit NmeaLineFramer(
        int maximumLineBytes = DefaultMaximumLineBytes);

    NmeaLineFramerResult ingest(const QByteArray &bytes);
    void reset();

    int maximumLineBytes() const noexcept { return m_maximumLineBytes; }
    int bufferedBytes() const noexcept { return m_lineBuffer.size(); }
    bool discardingOversizedLine() const noexcept
    {
        return m_discardingOversizedLine;
    }

private:
    int m_maximumLineBytes = DefaultMaximumLineBytes;
    QByteArray m_lineBuffer;
    bool m_discardingOversizedLine = false;
};

#endif // NMEALINEFRAMER_H
