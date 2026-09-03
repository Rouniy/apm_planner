#include "TlogReader.h"

namespace {
constexpr int kWindowBytes = 64 * 1024;
}

QDateTime TlogRecord::timestampUtc() const
{
    // Floor division keeps sub-millisecond timestamps monotonic for negative values too.
    const qint64 msec = timestampUsec >= 0 ? timestampUsec / 1000 : -((-timestampUsec + 999) / 1000);
    return QDateTime::fromMSecsSinceEpoch(msec, Qt::UTC);
}

TlogReader::TlogReader(QIODevice *device)
    : m_device(device)
{
    if (!device) {
        finish(Status::Error, QStringLiteral("No log device."));
        return;
    }
    if (!device->isOpen() || !device->isReadable()) {
        finish(Status::Error, QStringLiteral("The log device is not open for reading."));
        return;
    }
    if (device->isSequential()) {
        finish(Status::Error, QStringLiteral("The log device is not seekable."));
        return;
    }
    m_total = device->size();
}

TlogReader::~TlogReader() = default;

bool TlogReader::IsFrameStart(quint8 byte)
{
    return byte == MAVLINK_STX || byte == MAVLINK_STX_MAVLINK1;
}

void TlogReader::setCancelCheck(CancelCheck check)
{
    m_cancel = std::move(check);
}

void TlogReader::setProgress(Progress progress)
{
    m_progress = std::move(progress);
}

TlogReader::Status TlogReader::finish(Status status, const QString &error)
{
    m_status = status;
    if (!error.isEmpty()) {
        m_error = error;
    }
    return status;
}

bool TlogReader::ensure(qint64 absolute, int count)
{
    if (absolute >= m_windowStart
        && absolute + count <= m_windowStart + static_cast<qint64>(m_window.size())) {
        return true;
    }
    if (!m_device->seek(absolute)) {
        return false;
    }
    m_window = m_device->read(qMax<qint64>(count, kWindowBytes));
    m_windowStart = absolute;
    return m_window.size() >= count;
}

quint8 TlogReader::at(qint64 absolute) const
{
    return static_cast<quint8>(m_window.at(static_cast<int>(absolute - m_windowStart)));
}

TlogReader::Status TlogReader::next(TlogRecord *record)
{
    if (m_status != Status::Ok) {
        return m_status;
    }
    if (!record) {
        return finish(Status::Error, QStringLiteral("No record buffer."));
    }
    for (;;) {
        if (m_cancel && m_cancel()) {
            return finish(Status::Cancelled);
        }
        const qint64 remaining = m_total - m_position;
        if (remaining <= 0) {
            return finish(Status::End);
        }
        if (remaining < TimestampBytes + MinimumFrameBytes) {
            m_skipped += remaining;
            m_position = m_total;
            return finish(Status::Truncated,
                          QStringLiteral("%1 trailing byte(s) do not form a timestamped frame.")
                              .arg(remaining));
        }
        if (!ensure(m_position, TimestampBytes + 1)) {
            return finish(Status::Error, QStringLiteral("Reading the log failed: %1")
                                             .arg(m_device->errorString()));
        }

        quint64 timestamp = 0;
        for (int i = 0; i < TimestampBytes; ++i) {
            timestamp = (timestamp << 8) | at(m_position + i);
        }
        const quint8 firstFrameByte = at(m_position + TimestampBytes);
        if (IsFrameStart(firstFrameByte)) {
            // Fresh parser per candidate: no global channel, no state carried
            // over from a rejected frame.
            MAVLinkFrameParser parser;
            mavlink_message_t message{};
            qint64 cursor = m_position + TimestampBytes;
            int consumed = 0;
            unsigned int framing = MAVLINK_FRAMING_INCOMPLETE;
            while (cursor < m_total && consumed < MAVLINK_MAX_PACKET_LEN) {
                if (!ensure(cursor, 1)) {
                    return finish(Status::Error, QStringLiteral("Reading the log failed: %1")
                                                     .arg(m_device->errorString()));
                }
                framing = parser.parseByte(at(cursor), &message);
                ++cursor;
                ++consumed;
                if (framing != MAVLINK_FRAMING_INCOMPLETE) {
                    break;
                }
            }
            if (framing == MAVLINK_FRAMING_OK) {
                record->timestampUsec = static_cast<qint64>(timestamp);
                record->offset = m_position;
                record->message = message;
                m_position = cursor;
                ++m_records;
                if (m_progress) {
                    m_progress(m_position, m_total);
                }
                return Status::Ok;
            }
            if (framing == MAVLINK_FRAMING_BAD_CRC || framing == MAVLINK_FRAMING_BAD_SIGNATURE) {
                ++m_rejected;
            }
        }
        // MP10 MavlinkParse: no usable frame at this offset -> advance one byte.
        ++m_position;
        ++m_skipped;
    }
}
