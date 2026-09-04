#include "MjpegAviWriter.h"

#include <QFileInfo>

#include <algorithm>

namespace
{
quint32 saturatingUInt32(quint64 value)
{
    return value > std::numeric_limits<quint32>::max()
        ? std::numeric_limits<quint32>::max()
        : quint32(value);
}
}

MjpegAviWriter::MjpegAviWriter(const QString &path, int width, int height,
                               int fps)
    : m_file(path)
    , m_width(width)
    , m_height(height)
    , m_fps(fps)
{
    initialize();
}

MjpegAviWriter::~MjpegAviWriter()
{
    if (m_file.isOpen()) {
        finalize();
    }
}

bool MjpegAviWriter::initialize()
{
    if (m_file.fileName().trimmed().isEmpty()) {
        return fail(QStringLiteral("The AVI output path is empty."));
    }
    if (m_width < 1 || m_width > std::numeric_limits<qint16>::max()
        || m_height < 1 || m_height > std::numeric_limits<qint16>::max()) {
        return fail(QStringLiteral("The AVI dimensions are invalid."));
    }
    if (m_fps < 1) {
        return fail(QStringLiteral("The AVI frame rate is invalid."));
    }
    if (!m_file.open(QIODevice::ReadWrite | QIODevice::NewOnly)) {
        return fail(QStringLiteral("Could not create AVI '%1': %2")
                    .arg(QFileInfo(m_file.fileName()).absoluteFilePath(),
                         m_file.errorString()));
    }

    bool ok = writeFourCc("RIFF") && writeUInt32(0)
        && writeFourCc("AVI ") && writeFourCc("LIST");
    const qint64 headerListSizePosition = m_file.pos();
    ok = ok && writeUInt32(0) && writeFourCc("hdrl")
        && writeFourCc("avih") && writeUInt32(56);
    m_aviHeaderPosition = m_file.pos();
    ok = ok && writeMainHeader() && writeFourCc("LIST");
    const qint64 streamListSizePosition = m_file.pos();
    ok = ok && writeUInt32(0) && writeFourCc("strl")
        && writeFourCc("strh") && writeUInt32(56);
    m_streamHeaderPosition = m_file.pos();
    ok = ok && writeStreamHeader() && writeFourCc("strf")
        && writeUInt32(40) && writeBitmapHeader();
    const qint64 endOfHeader = m_file.pos();

    quint32 streamListSize = 0;
    quint32 headerListSize = 0;
    ok = ok
        && checkedUInt32(endOfHeader
                         - (streamListSizePosition + qint64(sizeof(quint32))),
                         &streamListSize)
        && checkedUInt32(endOfHeader
                         - (headerListSizePosition + qint64(sizeof(quint32))),
                         &headerListSize)
        && patchUInt32(streamListSizePosition, streamListSize)
        && patchUInt32(headerListSizePosition, headerListSize)
        && m_file.seek(endOfHeader)
        && writeFourCc("LIST");
    m_moviSizePosition = m_file.pos();
    ok = ok && writeUInt32(4) && writeFourCc("movi");
    m_moviDataPosition = m_file.pos();
    ok = ok && checkpoint();
    if (ok) {
        return true;
    }

    const QString path = m_file.fileName();
    if (m_error.isEmpty()) {
        fail(QStringLiteral("Could not initialize AVI '%1': %2")
             .arg(QFileInfo(m_file.fileName()).absoluteFilePath(),
                  m_file.errorString()));
    }
    m_file.close();
    QFile::remove(path);
    m_finalized = true;
    return false;
}

bool MjpegAviWriter::writeJpeg(const QByteArray &jpeg)
{
    if (!isOpen()) {
        return fail(m_error.isEmpty()
            ? QStringLiteral("The AVI is not open.") : m_error);
    }
    if (jpeg.size() < 4
        || quint8(jpeg.at(0)) != 0xff || quint8(jpeg.at(1)) != 0xd8) {
        return fail(QStringLiteral("The AVI frame is not a JPEG image."));
    }

    const quint64 padded = quint64(jpeg.size()) + quint64(jpeg.size() & 1);
    const quint64 frameChunkBytes = 8 + padded;
    const quint64 eventualIndexBytes =
        8 + quint64(m_index.size() + 1) * 16;
    if (!ensureCapacity(frameChunkBytes + eventualIndexBytes)) {
        return false;
    }

    const qint64 chunkPosition = m_file.pos();
    if (!writeFourCc("00dc") || !writeUInt32(quint32(jpeg.size()))
        || !writeBytes(jpeg.constData(), jpeg.size())) {
        return false;
    }
    if ((jpeg.size() & 1) != 0) {
        const char padding = 0;
        if (!writeBytes(&padding, 1)) return false;
    }

    quint32 offset = 0;
    if (!checkedUInt32(chunkPosition - m_moviDataPosition + 4, &offset)) {
        return fail(QStringLiteral("The AVI frame offset exceeds RIFF limits."));
    }
    m_index.append({offset, quint32(jpeg.size())});
    m_largestFrame = std::max(m_largestFrame, quint32(jpeg.size()));
    return true;
}

bool MjpegAviWriter::checkpoint()
{
    if (!isOpen()) {
        return fail(m_error.isEmpty()
            ? QStringLiteral("The AVI is not open.") : m_error);
    }
    const qint64 end = m_file.pos();
    if (!m_file.flush() || !patchHeaders(end, end)
        || !m_file.seek(end) || !m_file.flush()) {
        return fail(QStringLiteral("Could not checkpoint AVI '%1': %2")
                    .arg(QFileInfo(m_file.fileName()).absoluteFilePath(),
                         m_file.errorString()));
    }
    return true;
}

bool MjpegAviWriter::finalize()
{
    if (m_finalized) {
        return m_error.isEmpty();
    }
    if (!m_file.isOpen()) {
        m_finalized = true;
        return m_error.isEmpty();
    }

    const qint64 moviEnd = m_file.pos();
    // Make the movi section independently readable before appending idx1.
    if (!patchHeaders(moviEnd, moviEnd) || !m_file.seek(moviEnd)) {
        m_file.close();
        m_finalized = true;
        return fail(QStringLiteral("Could not finalize AVI headers."));
    }

    const quint64 indexBytes = 8 + quint64(m_index.size()) * 16;
    if (!ensureCapacity(indexBytes)
        || !writeFourCc("idx1")
        || !writeUInt32(quint32(m_index.size()) * 16u)) {
        m_file.seek(moviEnd);
        m_file.resize(moviEnd);
        m_file.flush();
        m_file.close();
        m_finalized = true;
        return false;
    }
    for (const IndexEntry &entry : m_index) {
        if (!writeFourCc("00dc") || !writeUInt32(0x10)
            || !writeUInt32(entry.offset) || !writeUInt32(entry.size)) {
            m_file.seek(moviEnd);
            m_file.resize(moviEnd);
            patchHeaders(moviEnd, moviEnd);
            m_file.flush();
            m_file.close();
            m_finalized = true;
            return false;
        }
    }

    const qint64 fileEnd = m_file.pos();
    const bool ok = patchHeaders(moviEnd, fileEnd)
        && m_file.seek(fileEnd) && m_file.flush();
    if (!ok && m_error.isEmpty()) {
        fail(QStringLiteral("Could not finalize AVI '%1': %2")
             .arg(QFileInfo(m_file.fileName()).absoluteFilePath(),
                  m_file.errorString()));
    }
    m_file.close();
    m_finalized = true;
    return ok;
}

bool MjpegAviWriter::FitsRiffCapacity(quint64 currentPosition,
                                      quint64 additionalBytes,
                                      quint64 riffSizeLimit)
{
    const quint64 maximumFileSize = riffSizeLimit > std::numeric_limits<quint64>::max() - 8
        ? std::numeric_limits<quint64>::max() : riffSizeLimit + 8;
    return currentPosition <= maximumFileSize
        && additionalBytes <= maximumFileSize - currentPosition;
}

bool MjpegAviWriter::patchHeaders(qint64 moviEnd, qint64 fileEnd)
{
    quint32 riffSize = 0;
    quint32 moviSize = 0;
    if (!checkedUInt32(fileEnd - 8, &riffSize)
        || !checkedUInt32(
            moviEnd - (m_moviSizePosition + qint64(sizeof(quint32))),
            &moviSize)) {
        return fail(QStringLiteral("The AVI exceeded the RIFF size limit."));
    }

    const qint64 returnPosition = m_file.pos();
    bool ok = patchUInt32(4, riffSize)
        && patchUInt32(m_moviSizePosition, moviSize)
        && m_file.seek(m_aviHeaderPosition) && writeMainHeader()
        && m_file.seek(m_streamHeaderPosition) && writeStreamHeader()
        && m_file.seek(returnPosition);
    if (!ok && m_error.isEmpty()) {
        fail(QStringLiteral("Could not patch AVI headers: %1")
             .arg(m_file.errorString()));
    }
    return ok;
}

bool MjpegAviWriter::writeMainHeader()
{
    return writeUInt32(quint32(1000000 / m_fps))
        && writeUInt32(saturatingUInt32(quint64(m_largestFrame) * quint64(m_fps)))
        && writeUInt32(0)
        && writeUInt32(m_index.isEmpty() ? 0u : 0x10u)
        && writeUInt32(quint32(m_index.size()))
        && writeUInt32(0) && writeUInt32(1)
        && writeUInt32(m_largestFrame)
        && writeUInt32(quint32(m_width)) && writeUInt32(quint32(m_height))
        && writeUInt32(0) && writeUInt32(0)
        && writeUInt32(0) && writeUInt32(0);
}

bool MjpegAviWriter::writeStreamHeader()
{
    return writeFourCc("vids") && writeFourCc("MJPG")
        && writeUInt32(0) && writeUInt16(0) && writeUInt16(0)
        && writeUInt32(0) && writeUInt32(1) && writeUInt32(quint32(m_fps))
        && writeUInt32(0) && writeUInt32(quint32(m_index.size()))
        && writeUInt32(m_largestFrame)
        && writeUInt32(std::numeric_limits<quint32>::max())
        && writeUInt32(0)
        && writeUInt16(0) && writeUInt16(0)
        && writeUInt16(quint16(m_width)) && writeUInt16(quint16(m_height));
}

bool MjpegAviWriter::writeBitmapHeader()
{
    return writeUInt32(40) && writeUInt32(quint32(m_width))
        && writeUInt32(quint32(m_height)) && writeUInt16(1)
        && writeUInt16(24) && writeFourCc("MJPG")
        && writeUInt32(saturatingUInt32(
            quint64(m_width) * quint64(m_height) * 3))
        && writeUInt32(0) && writeUInt32(0)
        && writeUInt32(0) && writeUInt32(0);
}

bool MjpegAviWriter::writeFourCc(const char value[5])
{
    return writeBytes(value, 4);
}

bool MjpegAviWriter::writeUInt16(quint16 value)
{
    const char bytes[] = {
        char(value & 0xff), char((value >> 8) & 0xff)
    };
    return writeBytes(bytes, 2);
}

bool MjpegAviWriter::writeUInt32(quint32 value)
{
    const char bytes[] = {
        char(value & 0xff), char((value >> 8) & 0xff),
        char((value >> 16) & 0xff), char((value >> 24) & 0xff)
    };
    return writeBytes(bytes, 4);
}

bool MjpegAviWriter::patchUInt32(qint64 position, quint32 value)
{
    const qint64 returnPosition = m_file.pos();
    return m_file.seek(position) && writeUInt32(value)
        && m_file.seek(returnPosition);
}

bool MjpegAviWriter::writeBytes(const char *data, qint64 size)
{
    if (size < 0 || m_file.write(data, size) != size) {
        return fail(QStringLiteral("Writing AVI '%1' failed: %2")
                    .arg(QFileInfo(m_file.fileName()).absoluteFilePath(),
                         m_file.errorString()));
    }
    return true;
}

bool MjpegAviWriter::fail(const QString &message)
{
    if (m_error.isEmpty()) m_error = message;
    return false;
}

bool MjpegAviWriter::ensureCapacity(quint64 additionalBytes)
{
    if (m_file.pos() < 0
        || !FitsRiffCapacity(quint64(m_file.pos()), additionalBytes)) {
        return fail(QStringLiteral(
            "The AVI reached the 4 GiB RIFF size limit. Start a new export."));
    }
    return true;
}

bool MjpegAviWriter::checkedUInt32(qint64 value, quint32 *result)
{
    if (!result || value < 0
        || quint64(value) > std::numeric_limits<quint32>::max()) {
        return false;
    }
    *result = quint32(value);
    return true;
}
