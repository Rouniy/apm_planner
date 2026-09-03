#ifndef TLOGREADER_H
#define TLOGREADER_H

#include "MAVLinkFrameParser.h"

#include <QByteArray>
#include <QDateTime>
#include <QIODevice>
#include <QString>

#include <functional>
#include <memory>

/*
 * Streaming reader for Mission Planner telemetry logs (.tlog): a sequence of
 * [8-byte big-endian UNIX microseconds][one MAVLink v1 or v2 frame]. This is
 * the same layout TLogReplayLink replays (TLogReplayLink.cc:184-215) and that
 * MP10 reads with MavlinkParse(true) (TlogExportService.cs:221-240).
 *
 * The reader never loads the whole file: it keeps a small window over a
 * seekable QIODevice (QFile or QBuffer) and decodes one frame per next().
 * Every candidate frame is parsed with a private MAVLinkFrameParser (no
 * global MAVLink channel state). Malformed data is handled like MP10: when a
 * frame does not decode at the expected offset the reader advances one byte
 * and tries again, so progress is guaranteed and the scan per attempt is
 * bounded by 8 + MAVLINK_MAX_PACKET_LEN bytes. A truncated tail (fewer bytes
 * than a timestamp plus a minimal frame) ends the stream.
 */
struct TlogRecord
{
    qint64 timestampUsec = 0;   // UNIX microseconds from the 8-byte prefix
    qint64 offset = 0;          // byte offset of the timestamp in the device
    mavlink_message_t message{};

    QDateTime timestampUtc() const;
};

class TlogReader
{
public:
    using CancelCheck = std::function<bool()>;
    using Progress = std::function<void(qint64 bytesProcessed, qint64 bytesTotal)>;

    enum class Status {
        Ok,          // a record was produced
        End,         // clean end of data
        Truncated,   // trailing bytes could not form a timestamp + frame
        Cancelled,   // CancelCheck returned true
        Error        // device error (not open / not seekable / read failure)
    };

    static constexpr int TimestampBytes = 8;
    static constexpr int MinimumFrameBytes = 8; // v1: STX + 5 header bytes + 2 CRC bytes, empty payload

    // The device must be open for reading and seekable; the reader does not own it.
    explicit TlogReader(QIODevice *device);
    ~TlogReader();

    static bool IsFrameStart(quint8 byte); // MAVLINK_STX (v2) or MAVLINK_STX_MAVLINK1

    void setCancelCheck(CancelCheck check);
    void setProgress(Progress progress);

    // Decodes the next record. Returns Status::Ok and fills *record, or the
    // reason the stream stopped. After End/Truncated/Cancelled/Error every call
    // returns the same status.
    Status next(TlogRecord *record);

    Status status() const { return m_status; }
    QString errorString() const { return m_error; }
    qint64 recordCount() const { return m_records; }
    qint64 skippedBytes() const { return m_skipped; }        // bytes discarded while resyncing
    qint64 bytesProcessed() const { return m_position; }
    qint64 bytesTotal() const { return m_total; }
    // Number of frames whose CRC or signature check failed at a candidate offset.
    qint64 rejectedFrames() const { return m_rejected; }

private:
    bool ensure(qint64 absolute, int count);       // window contains [absolute, absolute+count)
    quint8 at(qint64 absolute) const;
    Status finish(Status status, const QString &error = QString());

    QIODevice *m_device = nullptr;
    QByteArray m_window;
    qint64 m_windowStart = 0;
    qint64 m_position = 0;
    qint64 m_total = 0;
    qint64 m_records = 0;
    qint64 m_skipped = 0;
    qint64 m_rejected = 0;
    Status m_status = Status::Ok;
    QString m_error;
    CancelCheck m_cancel;
    Progress m_progress;
};

#endif // TLOGREADER_H
