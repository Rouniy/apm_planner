#include "DataFlashBinToLogConverter.h"

#include "DataFlashModeNames.h"
#include "DataFlashRawReader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QtEndian>

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>

#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {

using Result = DataFlashBinToLogConverter::Result;

constexpr qint64 ReadChunkBytes = 64 * 1024;

struct FormatDefinition {
    bool present = false;
    quint8 type = 0;
    quint8 length = 0;
    QByteArray name;
    QByteArray format;
};

bool pathExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool publishNoReplace(const QString &stagedPath, const QString &destination,
                      QString *error)
{
#ifdef Q_OS_UNIX
    // The private stage and destination are deliberately siblings. link(2)
    // publishes the already-complete inode atomically and fails with EEXIST
    // rather than replacing a destination installed by a racer.
    const QByteArray sourceName = QFile::encodeName(stagedPath);
    const QByteArray destinationName = QFile::encodeName(destination);
    if (::link(sourceName.constData(), destinationName.constData()) != 0) {
        *error = QStringLiteral(
            "Could not publish the ASCII log without overwriting an existing file.");
        return false;
    }
    // Publication is already complete if removal of this private sibling
    // fails. QTemporaryFile will retry that removal at destruction.
    QFile::remove(stagedPath);
    return true;
#elif defined(Q_OS_WIN)
    // Omitting MOVEFILE_REPLACE_EXISTING makes this a same-volume,
    // atomic no-replace move.
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(stagedPath.utf16()),
                     reinterpret_cast<LPCWSTR>(destination.utf16()),
                     MOVEFILE_WRITE_THROUGH)) {
        *error = QStringLiteral(
            "Could not publish the ASCII log without overwriting an existing file.");
        return false;
    }
    return true;
#else
    Q_UNUSED(stagedPath)
    Q_UNUSED(destination)
    *error = QStringLiteral(
        "Atomic no-overwrite publication is unavailable on this platform.");
    return false;
#endif
}

bool cancelRequested(const DataFlashBinToLogConverter::CancelCheck &cancel,
                     Result *result)
{
    if (cancel && cancel()) {
        result->cancelled = true;
        return true;
    }
    return false;
}

bool writeAll(QFile *file, const QByteArray &bytes, Result *result)
{
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = file->write(
            bytes.constData() + offset, bytes.size() - offset);
        if (written <= 0) {
            result->error = QStringLiteral("Could not write the staged ASCII log: %1")
                                .arg(file->errorString());
            return false;
        }
        offset += written;
    }
    return true;
}

QByteArray dotNetAscii(QByteArray bytes)
{
    for (char &byte : bytes) {
        if (quint8(byte) > 127) {
            byte = '?';
        }
    }
    while (!bytes.isEmpty() && bytes.front() == '\0') {
        bytes.remove(0, 1);
    }
    while (!bytes.isEmpty() && bytes.back() == '\0') {
        bytes.chop(1);
    }
    return bytes;
}

bool roundTrips(const QString &text, double value, bool singlePrecision)
{
    const QByteArray bytes = text.toLatin1();
    if (singlePrecision) {
        float parsed = 0.0f;
        const auto converted = std::from_chars(
            bytes.constData(), bytes.constData() + bytes.size(), parsed,
            std::chars_format::general);
        return converted.ec == std::errc()
            && converted.ptr == bytes.constData() + bytes.size()
            && parsed == static_cast<float>(value);
    }
    double parsed = 0.0;
    const auto converted = std::from_chars(
        bytes.constData(), bytes.constData() + bytes.size(), parsed,
        std::chars_format::general);
    return converted.ec == std::errc()
        && converted.ptr == bytes.constData() + bytes.size()
        && parsed == value;
}

QByteArray normalizeDotNetExponent(const QByteArray &input)
{
    int marker = input.indexOf('e');
    if (marker < 0) {
        marker = input.indexOf('E');
    }
    if (marker < 0) {
        return input;
    }
    int digit = marker + 1;
    char sign = '+';
    if (digit < input.size()
        && (input.at(digit) == '+' || input.at(digit) == '-')) {
        sign = input.at(digit++);
    }
    return input.left(marker) + 'E' + sign
        + input.mid(digit).rightJustified(2, '0');
}

QByteArray dotNetSingle(float value)
{
    std::array<char, 128> storage{};
    auto converted = std::to_chars(storage.data(), storage.data() + storage.size(),
                                   value, std::chars_format::general);
    if (converted.ec != std::errc()) {
        return QString::number(value, 'g', 9).toUpper().toLatin1();
    }
    QByteArray general(storage.data(), int(converted.ptr - storage.data()));
    int marker = general.indexOf('e');
    if (marker < 0) {
        marker = general.indexOf('E');
    }
    if (marker < 0) {
        return general;
    }
    const int exponent = general.mid(marker + 1).toInt();
    if (exponent >= 0 && exponent < 9) {
        converted = std::to_chars(storage.data(), storage.data() + storage.size(),
                                  value, std::chars_format::fixed);
        if (converted.ec == std::errc()) {
            return QByteArray(storage.data(), int(converted.ptr - storage.data()));
        }
    }
    return normalizeDotNetExponent(general);
}

// .NET 10's default Single/Double ToString uses the shortest round-tripping
// representation. Single uses std::to_chars' correctly-rounded shortest
// conversion; the QString precision search remains for Double. General format
// retains fixed notation below the type's round-trip precision and uses an
// upper-case exponent marker.
QByteArray dotNetNumber(double value, bool singlePrecision)
{
    if (std::isnan(value)) {
        return QByteArrayLiteral("NaN");
    }
    if (std::isinf(value)) {
        return value < 0.0 ? QByteArrayLiteral("-Infinity")
                           : QByteArrayLiteral("Infinity");
    }
    if (value == 0.0) {
        return std::signbit(value) ? QByteArrayLiteral("-0")
                                   : QByteArrayLiteral("0");
    }

    if (singlePrecision) {
        return dotNetSingle(static_cast<float>(value));
    }

    constexpr int maximum = 17;
    for (int precision = 1; precision <= maximum; ++precision) {
        const QString general = QString::number(value, 'g', precision);
        if (!roundTrips(general, value, singlePrecision)) {
            continue;
        }
        const int marker = general.indexOf(QLatin1Char('e'));
        if (marker < 0) {
            return general.toLatin1();
        }
        const int exponent = general.mid(marker + 1).toInt();
        if (exponent >= 0 && exponent < maximum) {
            const QString fixed = QString::number(
                value, 'f', qMax(0, precision - 1 - exponent));
            if (roundTrips(fixed, value, singlePrecision)) {
                return fixed.toLatin1();
            }
        }
        return general.toUpper().toLatin1();
    }
    return QString::number(value, 'g', maximum).toUpper().toLatin1();
}

float halfToFloat(quint16 bits)
{
    const quint32 sign = quint32(bits & 0x8000u) << 16;
    quint32 exponent = (bits >> 10) & 0x1fu;
    quint32 fraction = bits & 0x03ffu;
    quint32 output = 0;
    if (exponent == 0) {
        if (fraction == 0) {
            output = sign;
        } else {
            int shift = 0;
            while ((fraction & 0x0400u) == 0) {
                fraction <<= 1;
                ++shift;
            }
            fraction &= 0x03ffu;
            output = sign | (quint32(127 - 14 - shift) << 23)
                | (fraction << 13);
        }
    } else if (exponent == 0x1fu) {
        output = sign | 0x7f800000u | (fraction << 13);
    } else {
        output = sign | ((exponent + (127 - 15)) << 23)
            | (fraction << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &output, sizeof(value));
    return value;
}

template<typename Integer>
Integer littleEndian(const QByteArray &payload, int offset)
{
    return qFromLittleEndian<Integer>(
        reinterpret_cast<const uchar *>(payload.constData() + offset));
}

QByteArray floatValue(const QByteArray &payload, int offset)
{
    const quint32 bits = littleEndian<quint32>(payload, offset);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return dotNetNumber(value, true);
}

QByteArray doubleValue(const QByteArray &payload, int offset)
{
    const quint64 bits = littleEndian<quint64>(payload, offset);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return dotNetNumber(value, false);
}

QByteArray escapedBlob(const QByteArray &payload, bool *empty)
{
    const QByteArray text = dotNetAscii(payload);
    *empty = text.isEmpty();
    QByteArray escaped;
    escaped.reserve(text.size());
    for (const char byte : text) {
        switch (byte) {
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (quint8(byte) < 32 || quint8(byte) > 127) {
                escaped += QByteArrayLiteral("\\x")
                    + QByteArray::number(quint8(byte), 16)
                          .rightJustified(2, '0').toUpper();
            } else {
                escaped += byte;
            }
            break;
        }
    }
    return escaped;
}

void detectFirmware(const QByteArray &line, QByteArray *firmware)
{
    if (line.contains("PARM, RATE_RLL_P") || line.contains("ArduCopter")
        || line.contains("Copter")) {
        *firmware = QByteArrayLiteral("ArduCopter2");
    } else if (line.contains("PARM, H_SWASH_PLATE")) {
        *firmware = QByteArrayLiteral("ArduCopter2");
    } else if (line.contains("PARM, PTCH2SRV_P")
               || line.contains("ArduPlane") || line.contains("Plane")) {
        *firmware = QByteArrayLiteral("ArduPlane");
    } else if (line.contains("PARM, SKID_STEER_OUT")
               || line.contains("ArduRover") || line.contains("Rover")) {
        *firmware = QByteArrayLiteral("ArduRover");
    } else if (line.contains("AntennaTracker")
               || line.contains("Tracker")) {
        *firmware = QByteArrayLiteral("ArduTracker");
    }
}

class BoundedInput final
{
public:
    BoundedInput(QFile *file, qint64 limit,
                 const DataFlashBinToLogConverter::CancelCheck &cancel,
                 const DataFlashBinToLogConverter::Progress &progress,
                 Result *result)
        : m_file(file), m_limit(limit), m_cancel(cancel), m_progress(progress),
          m_result(result)
    {
    }

    bool atEnd() const { return m_consumed >= m_limit; }
    qint64 consumed() const { return m_consumed; }
    QByteArray digest() { return m_hash.result(); }

    bool byte(char *value)
    {
        if (!ensure()) {
            return false;
        }
        *value = m_buffer.at(m_offset++);
        ++m_consumed;
        return true;
    }

    bool bytes(int count, QByteArray *value)
    {
        value->clear();
        value->reserve(count);
        while (value->size() < count) {
            if (!ensure()) {
                return false;
            }
            const int available = m_buffer.size() - m_offset;
            const int take = qMin(available, count - value->size());
            value->append(m_buffer.constData() + m_offset, take);
            m_offset += take;
            m_consumed += take;
        }
        return true;
    }

private:
    bool ensure()
    {
        if (m_offset < m_buffer.size()) {
            return true;
        }
        if (m_loaded >= m_limit) {
            return false;
        }
        if (cancelRequested(m_cancel, m_result)) {
            return false;
        }
        const qint64 wanted = qMin(ReadChunkBytes, m_limit - m_loaded);
        m_buffer = m_file->read(wanted);
        m_offset = 0;
        if (m_buffer.size() != wanted) {
            m_result->error = QStringLiteral(
                "The DataFlash input ended or failed while it was being read: %1")
                                  .arg(m_file->errorString());
            return false;
        }
        m_hash.addData(m_buffer);
        m_loaded += m_buffer.size();
        m_result->bytesRead = m_loaded;
        if (m_progress) {
            m_progress(m_loaded, m_limit);
        }
        return true;
    }

    QFile *m_file = nullptr;
    qint64 m_limit = 0;
    const DataFlashBinToLogConverter::CancelCheck &m_cancel;
    const DataFlashBinToLogConverter::Progress &m_progress;
    Result *m_result = nullptr;
    QByteArray m_buffer;
    int m_offset = 0;
    qint64 m_loaded = 0;
    qint64 m_consumed = 0;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
};

enum class DecodeResult { Complete, Malformed, EmptyBlob };

DecodeResult decodeRecord(const FormatDefinition &definition,
                          const QByteArray &payload,
                          const QByteArray &firmware, QByteArray *line)
{
    QList<QByteArray> values;
    values.reserve(definition.format.size() + 1);
    values.append(definition.name);
    int offset = 0;
    for (const char type : definition.format) {
        const int width = DataFlashRaw::width(type);
        // Unknown encodings have width zero in BinaryLog.GetObjectFromMessage;
        // String.Join consequently emits an empty field without consuming data.
        // `A` is a newer DataFlash encoding absent from MP10 BinaryLog and has
        // that same behavior even though the shared modern reader knows it.
        if (width == 0 || type == 'A') {
            values.append(QByteArray());
            continue;
        }
        if (offset > payload.size() - width) {
            return DecodeResult::Malformed;
        }
        QByteArray value;
        switch (type) {
        case 'b': value = QByteArray::number(qint8(payload.at(offset))); break;
        case 'B': value = QByteArray::number(quint8(payload.at(offset))); break;
        case 'h': value = QByteArray::number(littleEndian<qint16>(payload, offset)); break;
        case 'H': value = QByteArray::number(littleEndian<quint16>(payload, offset)); break;
        case 'i': value = QByteArray::number(littleEndian<qint32>(payload, offset)); break;
        case 'I': value = QByteArray::number(littleEndian<quint32>(payload, offset)); break;
        case 'q': value = QByteArray::number(littleEndian<qint64>(payload, offset)); break;
        case 'Q': value = QByteArray::number(littleEndian<quint64>(payload, offset)); break;
        case 'g':
            value = dotNetNumber(halfToFloat(
                littleEndian<quint16>(payload, offset)), true);
            break;
        case 'f': value = floatValue(payload, offset); break;
        case 'd': value = doubleValue(payload, offset); break;
        case 'c':
            value = dotNetNumber(
                double(littleEndian<qint16>(payload, offset)) / 100.0, false);
            break;
        case 'C':
            value = dotNetNumber(
                double(littleEndian<quint16>(payload, offset)) / 100.0, false);
            break;
        case 'e':
            value = dotNetNumber(
                double(littleEndian<qint32>(payload, offset)) / 100.0, false);
            break;
        case 'E':
            value = dotNetNumber(
                double(littleEndian<quint32>(payload, offset)) / 100.0, false);
            break;
        case 'L':
            value = dotNetNumber(
                double(littleEndian<qint32>(payload, offset)) / 10000000.0,
                false);
            break;
        case 'n': case 'N': value = dotNetAscii(payload.mid(offset, width)); break;
        case 'M': {
            const int mode = quint8(payload.at(offset));
            const QString resolved = DataFlashModeNames::resolve(firmware, mode);
            value = resolved.isEmpty() ? QByteArray::number(mode)
                                       : resolved.toLatin1();
            break;
        }
        case 'Z': {
            bool empty = false;
            value = escapedBlob(payload.mid(offset, width), &empty);
            // BinaryLog's Aggregate() throws for an empty Z string and the
            // surrounding reader drops that complete record.
            if (empty) {
                return DecodeResult::EmptyBlob;
            }
            break;
        }
        case 'a': {
            value = "[";
            for (int index = 0; index < 32; ++index) {
                if (index) {
                    value += ' ';
                }
                value += QByteArray::number(
                    littleEndian<qint16>(payload, offset + index * 2));
            }
            value += ']';
            break;
        }
        default:
            values.append(QByteArray());
            continue;
        }
        values.append(value);
        offset += width;
    }

    *line = values.join(QByteArrayLiteral(", ")) + QByteArrayLiteral("\r\n");
    return DecodeResult::Complete;
}

bool sourceStillMatches(const QString &requestedPath, const QString &canonical,
                        qint64 size, const QDateTime &modifiedUtc)
{
    const QFileInfo current(requestedPath);
    return !current.isSymLink() && current.isFile()
        && current.canonicalFilePath() == canonical
        && current.size() == size && current.lastModified().toUTC() == modifiedUtc;
}

bool hashFile(const QString &path, qint64 expectedSize,
              const DataFlashBinToLogConverter::CancelCheck &cancel,
              QByteArray *digest, Result *result)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result->error = QStringLiteral("Could not reopen the DataFlash input for final verification: %1")
                            .arg(file.errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 read = 0;
    while (read < expectedSize) {
        if (cancelRequested(cancel, result)) {
            return false;
        }
        const QByteArray bytes = file.read(qMin(ReadChunkBytes, expectedSize - read));
        if (bytes.isEmpty()) {
            result->error = QStringLiteral(
                "The DataFlash input changed or failed during final verification.");
            return false;
        }
        hash.addData(bytes);
        read += bytes.size();
    }
    if (file.size() != expectedSize) {
        result->error = QStringLiteral(
            "The DataFlash input changed during conversion; no ASCII log was published.");
        return false;
    }
    *digest = hash.result();
    return true;
}

void appendWarnings(Result *result, qint64 ignoredBytes,
                    qint64 missingDefinitions, qint64 malformedRecords,
                    qint64 emptyBlobs, qint64 trailingBytes, int trailingType)
{
    if (ignoredBytes > 0) {
        result->warnings.append(QStringLiteral(
            "Ignored %1 byte(s) outside complete DataFlash records while resynchronizing.")
                                    .arg(ignoredBytes));
    }
    if (missingDefinitions > 0) {
        result->warnings.append(QStringLiteral(
            "Skipped %1 record marker(s) whose type had no preceding usable FMT definition.")
                                    .arg(missingDefinitions));
    }
    if (malformedRecords > 0) {
        result->warnings.append(QStringLiteral(
            "Skipped %1 malformed DataFlash record(s) that could not be decoded safely.")
                                    .arg(malformedRecords));
    }
    if (emptyBlobs > 0) {
        result->warnings.append(QStringLiteral(
            "Skipped %1 record(s) with an empty Z field, matching Mission Planner's BinaryLog behavior.")
                                    .arg(emptyBlobs));
    }
    if (trailingBytes > 0) {
        result->warnings.append(QStringLiteral(
            "Dropped %1 trailing byte(s) from an incomplete final record of type %2.")
                                    .arg(trailingBytes).arg(trailingType));
    }
    if (result->recordsWritten == 0) {
        result->warnings.append(QStringLiteral(
            "The input contained no complete DataFlash records that could be converted."));
    }
}

} // namespace

DataFlashBinToLogConverter::Result DataFlashBinToLogConverter::Convert(
    const QString &inputPath, const QString &outputPath,
    const CancelCheck &cancel, const Progress &progress)
{
    Result result;
    try {
        if (inputPath.isEmpty() || outputPath.isEmpty()
            || QFileInfo(inputPath).suffix().compare(
                   QLatin1String("bin"), Qt::CaseInsensitive) != 0
            || QFileInfo(outputPath).suffix().compare(
                   QLatin1String("log"), Qt::CaseInsensitive) != 0) {
            result.error = QStringLiteral(
                "Select a DataFlash .bin input and a new .log output path.");
            return result;
        }

        const QFileInfo requestedSource(inputPath);
        if (requestedSource.isSymLink() || !requestedSource.isFile()
            || !requestedSource.isReadable()) {
            result.error = QStringLiteral(
                "The DataFlash input must be a readable regular, non-symlink .bin file.");
            return result;
        }
        const QString sourcePath = requestedSource.canonicalFilePath();
        const qint64 sourceSize = requestedSource.size();
        const QDateTime sourceModified = requestedSource.lastModified().toUTC();
        if (sourcePath.isEmpty() || sourceSize < 0) {
            result.error = QStringLiteral("Could not resolve the DataFlash input file.");
            return result;
        }

        const QFileInfo requestedOutput(outputPath);
        if (pathExists(outputPath)) {
            result.error = QStringLiteral(
                "The ASCII log output already exists; it was not overwritten.");
            return result;
        }
        const QString parentPath = QFileInfo(requestedOutput.absolutePath())
                                       .canonicalFilePath();
        const QFileInfo parent(parentPath);
        if (parentPath.isEmpty() || !parent.isDir() || !parent.isWritable()
            || requestedOutput.fileName().isEmpty()) {
            result.error = QStringLiteral(
                "The ASCII log output directory must already exist and be writable.");
            return result;
        }
        const QString destination = QDir(parentPath).filePath(
            requestedOutput.fileName());
        if (destination == sourcePath) {
            result.error = QStringLiteral(
                "The ASCII log output must be different from the DataFlash input.");
            return result;
        }
        if (cancelRequested(cancel, &result)) {
            return result;
        }

        QFile source(sourcePath);
        if (!source.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Could not open the DataFlash input: %1")
                               .arg(source.errorString());
            return result;
        }
        QTemporaryFile staged(QDir(parentPath).filePath(
            QStringLiteral(".apm-bin-to-log-XXXXXX.partial")));
        staged.setAutoRemove(true);
        if (!staged.open()) {
            result.error = QStringLiteral("Could not create a private staged ASCII log: %1")
                               .arg(staged.errorString());
            return result;
        }

        BoundedInput input(&source, sourceSize, cancel, progress, &result);
        std::array<FormatDefinition, 256> definitions{};
        QByteArray firmware;
        qint64 ignoredBytes = 0;
        qint64 missingDefinitions = 0;
        qint64 malformedRecords = 0;
        qint64 emptyBlobs = 0;
        qint64 trailingBytes = 0;
        int trailingType = -1;
        int framingState = 0;

        while (!input.atEnd()) {
            if (cancelRequested(cancel, &result)) {
                return result;
            }
            char rawByte = 0;
            if (!input.byte(&rawByte)) {
                if (!result.cancelled && result.error.isEmpty()) {
                    result.error = QStringLiteral(
                        "Could not read the complete DataFlash input.");
                }
                return result;
            }
            const quint8 byte = quint8(rawByte);
            if (framingState == 0) {
                if (byte == 0xa3u) {
                    framingState = 1;
                } else {
                    ++ignoredBytes;
                }
                continue;
            }
            if (framingState == 1) {
                if (byte == 0x95u) {
                    framingState = 2;
                } else {
                    ignoredBytes += 2;
                    framingState = 0;
                }
                continue;
            }

            framingState = 0;
            const int type = byte;
            QByteArray line;
            if (type == DataFlashRaw::FmtId) {
                QByteArray payload;
                if (!input.bytes(DataFlashRaw::FmtLength - 3, &payload)) {
                    if (result.cancelled || !result.error.isEmpty()) {
                        return result;
                    }
                    trailingBytes = 3 + payload.size();
                    trailingType = type;
                    ++result.recordsSkipped;
                    break;
                }
                FormatDefinition definition;
                definition.present = true;
                definition.type = quint8(payload.at(0));
                definition.length = quint8(payload.at(1));
                definition.name = dotNetAscii(payload.mid(2, 4));
                definition.format = dotNetAscii(payload.mid(6, 16));
                const QByteArray labels = dotNetAscii(payload.mid(22, 64));
                line = QByteArrayLiteral("FMT, ")
                    + QByteArray::number(definition.type) + QByteArrayLiteral(", ")
                    + QByteArray::number(definition.length) + QByteArrayLiteral(", ")
                    + definition.name + QByteArrayLiteral(", ")
                    + definition.format + QByteArrayLiteral(", ")
                    + labels + QByteArrayLiteral("\r\n");
                definitions[definition.type] = definition;
            } else {
                const FormatDefinition definition = definitions[type];
                if (!definition.present || definition.length < 3) {
                    ++missingDefinitions;
                    ++result.recordsSkipped;
                    continue;
                }
                QByteArray payload;
                const int payloadSize = int(definition.length) - 3;
                if (!input.bytes(payloadSize, &payload)) {
                    if (result.cancelled || !result.error.isEmpty()) {
                        return result;
                    }
                    trailingBytes = 3 + payload.size();
                    trailingType = type;
                    ++result.recordsSkipped;
                    break;
                }
                const DecodeResult decoded = decodeRecord(
                    definition, payload, firmware, &line);
                if (decoded != DecodeResult::Complete) {
                    ++result.recordsSkipped;
                    if (decoded == DecodeResult::EmptyBlob) {
                        ++emptyBlobs;
                    } else {
                        ++malformedRecords;
                    }
                    continue;
                }
            }

            if (!writeAll(&staged, line, &result)) {
                return result;
            }
            ++result.recordsWritten;
            detectFirmware(line, &firmware);
        }
        if (framingState != 0) {
            ignoredBytes += framingState;
        }

        const QByteArray sourceHash = input.digest();
        if (result.bytesRead != sourceSize || source.size() != sourceSize
            || !sourceStillMatches(inputPath, sourcePath, sourceSize,
                                   sourceModified)) {
            result.error = QStringLiteral(
                "The DataFlash input changed during conversion; no ASCII log was published.");
            return result;
        }
        source.close();
        appendWarnings(&result, ignoredBytes, missingDefinitions,
                       malformedRecords, emptyBlobs, trailingBytes,
                       trailingType);

        if (!staged.flush()) {
            result.error = QStringLiteral("Could not flush the staged ASCII log: %1")
                               .arg(staged.errorString());
            return result;
        }
        const qint64 stagedBytes = staged.size();
        staged.close();
        if (cancelRequested(cancel, &result)) {
            return result;
        }

        QByteArray finalHash;
        if (!hashFile(sourcePath, sourceSize, cancel, &finalHash, &result)) {
            return result;
        }
        if (finalHash != sourceHash
            || !sourceStillMatches(inputPath, sourcePath, sourceSize,
                                   sourceModified)) {
            result.error = QStringLiteral(
                "The DataFlash input changed before publication; no ASCII log was published.");
            return result;
        }
        const QString finalParent = QFileInfo(requestedOutput.absolutePath())
                                        .canonicalFilePath();
        if (finalParent != parentPath || pathExists(destination)) {
            result.error = QStringLiteral(
                "The ASCII log destination changed before publication; nothing was overwritten.");
            return result;
        }
        if (cancelRequested(cancel, &result)) {
            return result;
        }
        if (pathExists(destination)
            || !publishNoReplace(staged.fileName(), destination,
                                 &result.error)) {
            return result;
        }

        result.success = true;
        result.outputPath = destination;
        result.bytesWritten = stagedBytes;
        // No caller callback follows publication; published output is never
        // retracted or deleted by this converter.
    } catch (const std::exception &error) {
        result.error = QStringLiteral("DataFlash BIN conversion failed: %1")
                           .arg(QString::fromUtf8(error.what()));
    } catch (...) {
        result.error = QStringLiteral(
            "Unexpected DataFlash BIN conversion failure.");
    }
    return result;
}
