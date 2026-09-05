#include "DataFlashLogSplitter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QVector>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace {
constexpr qint64 MetadataLimit = 16 * 1024 * 1024;
constexpr qint64 TextLineLimit = 4 * 1024 * 1024;
constexpr int FmtId = 128;
constexpr int FmtLength = 89;

int width(char type)
{
    switch (type) {
    case 'b': case 'B': case 'M': return 1;
    case 'c': case 'C': case 'h': case 'H': case 'g': return 2;
    case 'e': case 'E': case 'f': case 'i': case 'I': case 'L': case 'n': return 4;
    case 'd': case 'q': case 'Q': return 8;
    case 'N': return 16;
    case 'Z': case 'a': return 64;
    case 'A': return 128;
    default: return 0;
    }
}

QByteArray ascii(const QByteArray &bytes)
{
    const int end = bytes.indexOf('\0');
    return (end < 0 ? bytes : bytes.left(end)).trimmed();
}

struct Definition {
    int id = -1, length = 0;
    QByteArray name, format;
    QList<QByteArray> columns;
    QVector<int> offsets;
    bool same(const Definition &other) const {
        return id == other.id && length == other.length && name == other.name
            && format == other.format && columns == other.columns;
    }
};

bool parseDefinition(const QByteArray &raw, bool text, Definition *out, QString *error)
{
    Definition d;
    if (text) {
        auto fields = raw.trimmed().split(',');
        if (fields.size() < 6) { *error = QStringLiteral("Malformed text FMT record."); return false; }
        bool idOk = false, lengthOk = false;
        d.id = fields[1].trimmed().toInt(&idOk);
        d.length = fields[2].trimmed().toInt(&lengthOk);
        if (!idOk || !lengthOk) { *error = QStringLiteral("Invalid text FMT type or length."); return false; }
        d.name = fields[3].trimmed(); d.format = fields[4].trimmed();
        d.columns = fields.mid(5);
    } else {
        if (raw.size() != FmtLength) { *error = QStringLiteral("Truncated binary FMT record."); return false; }
        d.id = quint8(raw[3]); d.length = quint8(raw[4]);
        d.name = ascii(raw.mid(5, 4)); d.format = ascii(raw.mid(9, 16));
        d.columns = ascii(raw.mid(25, 64)).split(',');
    }
    QSet<QByteArray> names;
    for (auto &column : d.columns) {
        column = column.trimmed();
        if (column.isEmpty() || names.contains(column)) {
            *error = QStringLiteral("Empty or repeated FMT column."); return false;
        }
        names.insert(column);
    }
    if (d.id < 0 || d.id > 255 || d.length < 3 || d.length > 255
        || d.name.isEmpty() || d.name.size() > 4 || d.format.isEmpty()
        || d.format.size() > 16 || d.columns.size() != d.format.size()) {
        *error = QStringLiteral("Invalid DataFlash FMT definition."); return false;
    }
    int size = 3;
    for (char type : d.format) {
        const int fieldWidth = width(type);
        if (!fieldWidth) { *error = QStringLiteral("Unsupported DataFlash FMT encoding."); return false; }
        d.offsets.append(size); size += fieldWidth;
    }
    if (size != d.length || ((d.id == FmtId) != (d.name == "FMT"))
        || (d.id == FmtId && (d.length != FmtLength || d.format != "BBnNZ"))) {
        *error = QStringLiteral("Conflicting reserved FMT layout or incorrect record length."); return false;
    }
    *out = d;
    return true;
}

int metadataKind(const QByteArray &name)
{
    if (name == "FMT") return 0;
    if (name == "FMTU") return 1;
    if (name == "UNIT") return 2;
    if (name == "MULT") return 3;
    return -1;
}

QList<QByteArray> textValues(const QByteArray &raw, const Definition &definition)
{
    QList<QByteArray> values = raw.trimmed().split(',').mid(1);
    // Text DataFlash does not quote MSG/Z strings. A trailing string or array
    // may contain commas; preserving the full raw line keeps its bytes intact.
    if (values.size() > definition.columns.size()
        && QByteArray("nNZaA").contains(definition.format.back())) {
        const int tail = definition.columns.size() - 1;
        QByteArray joined;
        for (int index = tail; index < values.size(); ++index) {
            if (index != tail) joined += ',';
            joined += values[index];
        }
        values = values.mid(0, tail); values.append(joined);
    }
    return values;
}

// One raw record at a time; no indexes proportional to log size.
class Reader {
public:
    Reader(QFile *file, bool text) : file(file), text(text) {}
    QFile *file;
    bool text;
    QString error;
    QHash<int, Definition> definitions;
    QHash<QByteArray, int> names;
    QHash<QByteArray, QByteArray> metadataValues;
    qint64 blankLines = 0;
    int trailingBytes = 0, trailingType = -1;
    QCryptographicHash hash{QCryptographicHash::Sha256};

    bool next(QByteArray *raw, int *kind)
    {
        raw->clear();
        if (file->atEnd()) return false;
        Definition d;
        if (text) {
            *raw = file->readLine(TextLineLimit + 1);
            if (raw->isEmpty() || raw->size() > TextLineLimit || raw->contains('\0')) {
                error = QStringLiteral("Invalid or oversized DataFlash text record."); return false;
            }
            if (raw->trimmed().isEmpty()) {
                ++blankLines; *kind = -2; hash.addData(*raw); return true;
            }
            const QByteArray name = raw->left(raw->indexOf(',') < 0 ? raw->size() : raw->indexOf(',')).trimmed();
            if (name == "FMT") {
                if (!parseDefinition(*raw, true, &d, &error)) return false;
            } else {
                if (!names.contains(name)) { error = QStringLiteral("Text record has no preceding FMT definition."); return false; }
                d = definitions.value(names.value(name));
                if (textValues(*raw, d).size() != d.columns.size()) {
                    error = QStringLiteral("Text record does not match its FMT column count."); return false;
                }
            }
            *kind = metadataKind(name);
        } else {
            *raw = file->read(3);
            if (raw->size() != 3 || quint8(raw->at(0)) != 0xa3 || quint8(raw->at(1)) != 0x95) {
                error = QStringLiteral("Corrupt or truncated binary DataFlash framing."); return false;
            }
            const int id = quint8(raw->at(2));
            const int length = id == FmtId ? FmtLength : definitions.value(id).length;
            if (length < 3) { error = QStringLiteral("Binary record has no preceding FMT definition."); return false; }
            raw->append(file->read(length - 3));
            if (raw->size() != length) {
                if (id != FmtId && metadataKind(definitions.value(id).name) < 0
                    && file->atEnd() && file->error() == QFileDevice::NoError) {
                    trailingBytes = raw->size(); trailingType = id;
                    *kind = -2; hash.addData(*raw); return true;
                }
                error = QStringLiteral("Truncated binary metadata record; no parts were published."); return false;
            }
            if (id == FmtId) {
                if (!parseDefinition(*raw, false, &d, &error)) return false;
                *kind = 0;
            } else {
                d = definitions.value(id);
                *kind = metadataKind(d.name);
            }
        }
        if (*kind == 0) {
            if ((definitions.contains(d.id) && !definitions.value(d.id).same(d))
                || (names.contains(d.name) && names.value(d.name) != d.id)) {
                error = QStringLiteral("Conflicting FMT definitions cannot form self-contained pieces."); return false;
            }
            definitions.insert(d.id, d); names.insert(d.name, d.id);
        } else if (*kind > 0 && !checkMetadata(*raw, d)) {
            return false;
        }
        hash.addData(*raw);
        return true;
    }

    bool checkMetadata(const QByteArray &raw, const Definition &d)
    {
        const QByteArray idName = d.name == "FMTU" ? QByteArray("FmtType") : QByteArray("Id");
        const int keyIndex = d.columns.indexOf(idName);
        const QList<QByteArray> values = text ? textValues(raw, d) : QList<QByteArray>();
        if (keyIndex < 0 || (d.format[keyIndex] != 'B' && d.format[keyIndex] != 'b')
            || (d.name == "FMTU" && d.format[keyIndex] != 'B')) {
            error = QStringLiteral("Metadata has no byte-sized identity field."); return false;
        }
        QByteArray keyValue;
        if (text) {
            bool ok = false;
            const int id = values[keyIndex].trimmed().toInt(&ok);
            if (!ok || id < -128 || id > 255) { error = QStringLiteral("Invalid text metadata identity."); return false; }
            keyValue = QByteArray::number(quint8(id));
        } else {
            keyValue = QByteArray::number(quint8(raw[d.offsets[keyIndex]]));
        }
        const QList<QByteArray> required = d.name == "FMTU"
            ? QList<QByteArray>{"UnitIds", "MultIds"}
            : (d.name == "UNIT" ? QList<QByteArray>{"Label"} : QList<QByteArray>{"Mult"});
        for (int index = 0; index < d.columns.size(); ++index) {
            const QByteArray &name = d.columns[index];
            if (name == idName || required.contains(name)) continue;
            if ((name == "TimeUS" && d.format[index] == 'Q')
                || (name == "TimeMS" && d.format[index] == 'I')) continue;
            error = QStringLiteral("Unsupported metadata field layout."); return false;
        }
        QByteArray semanticValue;
        for (const QByteArray &name : required) {
            const int index = d.columns.indexOf(name);
            if (index < 0) { error = QStringLiteral("Incomplete FMTU/UNIT/MULT metadata definition."); return false; }
            const char type = d.format[index];
            QByteArray value;
            if (d.name == "MULT") {
                if (type != 'd') { error = QStringLiteral("MULT value must use double encoding."); return false; }
                double number = 0;
                if (text) {
                    bool ok = false; number = values[index].trimmed().toDouble(&ok);
                    if (!ok) { error = QStringLiteral("Invalid text MULT value."); return false; }
                } else {
                    const quint64 bits = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(raw.constData() + d.offsets[index]));
                    std::memcpy(&number, &bits, sizeof(number));
                }
                if (!std::isfinite(number)) { error = QStringLiteral("Non-finite MULT value."); return false; }
                value = QByteArray::number(number, 'g', 17);
            } else {
                if ((d.name == "FMTU" && type != 'N') || (d.name == "UNIT" && type != 'Z')) {
                    error = QStringLiteral("Unsupported metadata string encoding."); return false;
                }
                value = text ? values[index].trimmed() : ascii(raw.mid(d.offsets[index], width(type)));
            }
            semanticValue += QByteArray::number(value.size()) + ':' + value;
        }
        const QByteArray key = d.name + ':' + keyValue;
        if (metadataValues.contains(key) && metadataValues.value(key) != semanticValue) {
            error = QStringLiteral("Conflicting FMTU/UNIT/MULT definitions cannot be prepended safely."); return false;
        }
        metadataValues.insert(key, semanticValue);
        return true;
    }
};

bool exists(const QString &path)
{
    const QFileInfo file(path);
    return file.exists() || file.isSymLink();
}

bool write(QFile *file, const QByteArray &bytes, QString *error)
{
    qint64 position = 0;
    while (position < bytes.size()) {
        const qint64 count = file->write(bytes.constData() + position, bytes.size() - position);
        if (count <= 0) { *error = QStringLiteral("Could not write staged part: %1").arg(file->errorString()); return false; }
        position += count;
    }
    return true;
}

bool cancelled(const DataFlashLogSplitter::CancelCheck &cancel, DataFlashLogSplitter::Result *result)
{
    if (cancel && cancel()) { result->cancelled = true; return true; }
    return false;
}

bool hashFile(const QString &path, QByteArray *digest, const DataFlashLogSplitter::CancelCheck &cancel,
              DataFlashLogSplitter::Result *result)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { result->error = QStringLiteral("Could not verify file: %1").arg(file.errorString()); return false; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled(cancel, result)) return false;
        const QByteArray bytes = file.read(64 * 1024);
        if (bytes.isEmpty()) { result->error = QStringLiteral("Could not read file during verification."); return false; }
        hash.addData(bytes);
    }
    *digest = hash.result();
    return true;
}
}

QStringList DataFlashLogSplitter::OutputPaths(const QString &inputPath, int pieces)
{
    const QFileInfo input(inputPath);
    const QString suffix = input.suffix().toLower();
    if (inputPath.isEmpty() || pieces < 2 || pieces > 1000 || (suffix != "bin" && suffix != "log")) return {};
    const QString parent = QFileInfo(input.absolutePath()).canonicalFilePath();
    if (parent.isEmpty()) return {};
    const QString base = QDir(parent).filePath(input.fileName());
    QStringList paths;
    for (int piece = 0; piece < pieces; ++piece)
        paths.append(base + QStringLiteral("_split%1.%2").arg(piece).arg(suffix));
    return paths;
}

DataFlashLogSplitter::Result DataFlashLogSplitter::Split(
    const QString &inputPath, int pieces, CancelCheck cancel, Progress progress)
{
    Result result;
    try {
        const QStringList destinations = OutputPaths(inputPath, pieces);
        if (destinations.isEmpty()) { result.error = QStringLiteral("Select a .bin/.log file and between 2 and 1000 pieces."); return result; }
        for (const auto &path : destinations)
            if (exists(path)) { result.error = QStringLiteral("Output already exists; nothing was overwritten: %1").arg(path); return result; }
        const QString sourcePath = QFileInfo(inputPath).canonicalFilePath();
        QFile source(sourcePath);
        if (sourcePath.isEmpty() || !QFileInfo(sourcePath).isFile() || !source.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Could not open the input DataFlash file."); return result;
        }
        const qint64 sourceSize = source.size();
        const bool text = QFileInfo(inputPath).suffix().compare("log", Qt::CaseInsensitive) == 0;
        if (progress) progress(0, 1000);
        Reader first(&source, text);
        QByteArray metadata[4];
        qint64 metadataBytes = 0, dataBytes = 0, lastProgress = -65536;
        QByteArray raw;
        int kind = -1;
        while (first.next(&raw, &kind)) {
            if (cancelled(cancel, &result)) return result;
            if (kind != -2) ++result.recordsRead;
            if (kind >= 0) {
                QByteArray prefixRecord = raw;
                if (text && !prefixRecord.endsWith('\n')) prefixRecord += '\n';
                metadataBytes += prefixRecord.size();
                if (metadataBytes > MetadataLimit) { result.error = QStringLiteral("Metadata exceeds the 16 MiB safety limit."); return result; }
                metadata[kind] += prefixRecord;
            } else if (kind == -1) {
                ++result.dataRecords; dataBytes += raw.size();
            }
            if (source.pos() - lastProgress >= 65536) {
                lastProgress = source.pos();
                if (progress) progress(sourceSize ? qint64(250.0L * source.pos() / sourceSize) : 250, 1000);
            }
        }
        if (!first.error.isEmpty()) { result.error = first.error; return result; }
        if (first.blankLines)
            result.warnings.append(QStringLiteral("Ignored %1 blank text line(s).").arg(first.blankLines));
        if (first.trailingBytes)
            result.warnings.append(QStringLiteral("Dropped incomplete final binary %1 record (type %2): %3 trailing bytes.")
                .arg(QString::fromLatin1(first.definitions.value(first.trailingType).name))
                .arg(first.trailingType).arg(first.trailingBytes));
        if (source.size() != sourceSize || source.pos() != sourceSize) { result.error = QStringLiteral("Input changed during the metadata pass."); return result; }
        if (result.dataRecords < pieces) { result.error = QStringLiteral("There are fewer complete data records than requested pieces."); return result; }
        const QByteArray sourceHash = first.hash.result();
        source.close();
        QByteArray prefix;
        for (const auto &group : metadata) prefix += group;
        result.warnings.append(QStringLiteral("Parts preserve ordinary records once, but are not complete flight-state snapshots. Publication is not group-atomic; cancellation stops before publication, not between published files."));
        if (progress) progress(250, 1000);
        if (cancelled(cancel, &result)) return result;
        if (QFileInfo(inputPath).canonicalFilePath() != sourcePath || !source.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Input path changed before the splitting pass."); return result;
        }
        QTemporaryDir staging(QFileInfo(destinations.first()).absolutePath() + QStringLiteral("/.apm-dataflash-split-XXXXXX"));
        if (!staging.isValid()) { result.error = QStringLiteral("Could not create private sibling staging directory."); return result; }
        QStringList staged;
        QList<QByteArray> stageHashes;
        QList<qint64> stageSizes;
        QFile part;
        QCryptographicHash partHash(QCryptographicHash::Sha256);
        int index = 0;
        qint64 currentBytes = 0, writtenBytes = 0, writtenRecords = 0;
        long double targetBytes = static_cast<long double>(dataBytes) / pieces;
        const auto openPart = [&]() {
            const QString path = staging.filePath(QStringLiteral("part-%1").arg(index));
            part.setFileName(path);
            if (!part.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { result.error = QStringLiteral("Could not create a staged part."); return false; }
            staged.append(path); partHash.reset(); partHash.addData(prefix);
            return write(&part, prefix, &result.error);
        };
        const auto closePart = [&]() {
            if (!part.flush()) { result.error = QStringLiteral("Could not flush a staged part."); return false; }
            stageSizes.append(part.size()); stageHashes.append(partHash.result()); part.close(); return true;
        };
        if (!openPart()) return result;
        Reader second(&source, text);
        lastProgress = -65536;
        while (second.next(&raw, &kind)) {
            if (cancelled(cancel, &result)) return result;
            if (kind == -1) {
                const qint64 recordsLeft = result.dataRecords - writtenRecords;
                const int partsLeft = pieces - index;
                const bool closerBefore = std::abs(currentBytes - targetBytes)
                    <= std::abs(currentBytes + raw.size() - targetBytes);
                if (currentBytes > 0 && partsLeft > 1 && recordsLeft >= partsLeft - 1
                    && (recordsLeft == partsLeft - 1 || closerBefore)) {
                    if (!closePart()) return result;
                    ++index; currentBytes = 0;
                    targetBytes = static_cast<long double>(dataBytes - writtenBytes) / (pieces - index);
                    if (!openPart()) return result;
                }
                if (!write(&part, raw, &result.error)) return result;
                partHash.addData(raw); currentBytes += raw.size(); writtenBytes += raw.size(); ++writtenRecords;
            }
            if (source.pos() - lastProgress >= 65536) {
                lastProgress = source.pos();
                if (progress) progress(250 + qint64(250.0L * source.pos() / sourceSize), 1000);
            }
        }
        if (!second.error.isEmpty()) { result.error = second.error; return result; }
        if (second.hash.result() != sourceHash || source.size() != sourceSize
            || writtenRecords != result.dataRecords || index != pieces - 1) {
            result.error = QStringLiteral("Input changed between splitting passes."); return result;
        }
        source.close();
        if (!closePart()) return result;
        for (int piece = 0; piece < staged.size(); ++piece) {
            if (progress) progress(500 + (250 * piece) / pieces, 1000);
            QByteArray digest;
            if (!hashFile(staged[piece], &digest, cancel, &result)) return result;
            if (digest != stageHashes[piece] || QFileInfo(staged[piece]).size() != stageSizes[piece]) {
                result.error = QStringLiteral("A staged part changed before publication."); return result;
            }
        }
        if (progress) progress(750, 1000);
        if (cancelled(cancel, &result)) return result;
        QByteArray finalHash;
        if (QFileInfo(inputPath).canonicalFilePath() != sourcePath || !hashFile(sourcePath, &finalHash, cancel, &result)) {
            if (result.error.isEmpty() && !result.cancelled) result.error = QStringLiteral("Input path changed before publication.");
            return result;
        }
        if (finalHash != sourceHash) { result.error = QStringLiteral("Input changed before publication."); return result; }
        for (const auto &path : destinations)
            if (exists(path)) { result.error = QStringLiteral("Output appeared before publication: %1").arg(path); return result; }
        if (cancelled(cancel, &result)) return result;
        // Deliberate cancellation boundary. Never roll back or delete a published
        // file: another actor might already have opened or modified it.
        for (int piece = 0; piece < staged.size(); ++piece) {
            if (exists(destinations[piece]) || !QFile::rename(staged[piece], destinations[piece])) {
                result.error = QStringLiteral("Could not publish %1 without overwriting. %2 earlier part(s) remain published.")
                    .arg(destinations[piece]).arg(result.outputs.size()); return result;
            }
            result.outputs.append(destinations[piece]); result.bytesWritten += stageSizes[piece];
            if (progress) progress(750 + (250 * (piece + 1)) / pieces, 1000);
        }
        result.success = true;
    } catch (const std::exception &error) {
        result.error = QStringLiteral("DataFlash split failed: %1").arg(QString::fromUtf8(error.what()));
    } catch (...) {
        result.error = QStringLiteral("Unexpected DataFlash split failure.");
    }
    return result;
}
