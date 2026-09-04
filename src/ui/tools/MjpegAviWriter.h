#ifndef MJPEGAVIWRITER_H
#define MJPEGAVIWRITER_H

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QVector>

#include <limits>

/** Cross-platform single-stream MJPEG RIFF/AVI writer. */
class MjpegAviWriter
{
public:
    MjpegAviWriter(const QString &path, int width, int height, int fps);
    ~MjpegAviWriter();

    MjpegAviWriter(const MjpegAviWriter &) = delete;
    MjpegAviWriter &operator=(const MjpegAviWriter &) = delete;

    bool isOpen() const { return m_file.isOpen() && !m_finalized; }
    QString path() const { return m_file.fileName(); }
    QString errorString() const { return m_error; }
    int frameCount() const { return m_index.size(); }

    bool writeJpeg(const QByteArray &jpeg);
    /** Flushes current headers so a non-empty interrupted file is playable. */
    bool checkpoint();
    /** Writes idx1, patches headers and closes. Idempotent. */
    bool finalize();

    static bool FitsRiffCapacity(
        quint64 currentPosition, quint64 additionalBytes,
        quint64 riffSizeLimit = std::numeric_limits<quint32>::max());

private:
    struct IndexEntry {
        quint32 offset = 0;
        quint32 size = 0;
    };

    bool initialize();
    bool patchHeaders(qint64 moviEnd, qint64 fileEnd);
    bool writeMainHeader();
    bool writeStreamHeader();
    bool writeBitmapHeader();
    bool writeFourCc(const char value[5]);
    bool writeUInt16(quint16 value);
    bool writeUInt32(quint32 value);
    bool patchUInt32(qint64 position, quint32 value);
    bool writeBytes(const char *data, qint64 size);
    bool fail(const QString &message);
    bool ensureCapacity(quint64 additionalBytes);
    static bool checkedUInt32(qint64 value, quint32 *result);

    QFile m_file;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;
    QVector<IndexEntry> m_index;
    qint64 m_aviHeaderPosition = -1;
    qint64 m_streamHeaderPosition = -1;
    qint64 m_moviSizePosition = -1;
    qint64 m_moviDataPosition = -1;
    quint32 m_largestFrame = 0;
    QString m_error;
    bool m_finalized = false;
};

#endif // MJPEGAVIWRITER_H
