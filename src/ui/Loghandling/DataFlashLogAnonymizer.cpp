#include "DataFlashLogAnonymizer.h"

#include <QCryptographicHash>
#include <QHash>
#include <QIODevice>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>

namespace
{
constexpr char Head1 = static_cast<char>(0xa3);
constexpr char Head2 = static_cast<char>(0x95);
constexpr int HeaderSize = 3;
constexpr int FmtMessageId = 128;
constexpr int FmtMessageLength = 89;
constexpr qint64 ProgressByteInterval = 64 * 1024;

struct Field
{
    int offset = 0;
    int size = 0;
    char format = 0;
    QString name;
};

struct Definition
{
    int typeId = -1;
    int length = 0;
    QString name;
    QString format;
    QStringList columns;
    QVector<Field> fields;
};

struct Patch
{
    int offset = 0;
    int size = 0;
    char format = 0;
    bool latitude = false;
    QString fieldName;
};

struct FmtuMapping
{
    QString units;
    QString multipliers;
};

struct Metadata
{
    QHash<int, Definition> definitions;
    QHash<int, FmtuMapping> fmtu;
    QHash<int, QVector<Patch>> patches;
    int trailingType = -1;
    qint64 trailingOffset = -1;
    QByteArray trailingBytes;
    qint64 records = 0;
};

int formatSize(char format)
{
    switch (format) {
    case 'a': return 64;
    case 'b':
    case 'B':
    case 'M': return 1;
    case 'c':
    case 'C':
    case 'h':
    case 'H': return 2;
    case 'e':
    case 'E':
    case 'f':
    case 'i':
    case 'I':
    case 'L':
    case 'n': return 4;
    case 'd':
    case 'q':
    case 'Q': return 8;
    case 'N': return 16;
    case 'Z': return 64;
    case 'A': return 128;
    default: return 0;
    }
}

QString readAscii(const QByteArray &bytes, int offset, int length)
{
    if (offset < 0 || length <= 0 || offset > bytes.size() - length) {
        return {};
    }
    const int terminator = bytes.indexOf('\0', offset);
    const int end = terminator >= offset && terminator < offset + length
        ? terminator : offset + length;
    return QString::fromLatin1(bytes.constData() + offset, end - offset);
}

bool definitionsEqual(const Definition &left, const Definition &right)
{
    return left.typeId == right.typeId
        && left.length == right.length
        && left.name == right.name
        && left.format == right.format
        && left.columns == right.columns;
}

bool parseDefinition(const QByteArray &record, Definition *definition,
                     QString *error)
{
    if (!definition || record.size() != FmtMessageLength) {
        if (error) *error = QStringLiteral("A binary FMT record is truncated.");
        return false;
    }
    Definition parsed;
    parsed.typeId = static_cast<quint8>(record.at(3));
    parsed.length = static_cast<quint8>(record.at(4));
    parsed.name = readAscii(record, 5, 4).trimmed();
    parsed.format = readAscii(record, 9, 16);
    const QString columnsText = readAscii(record, 25, 64);
    if (!columnsText.isEmpty()) {
        parsed.columns = columnsText.split(QLatin1Char(','),
                                           Qt::KeepEmptyParts);
        for (QString &column : parsed.columns) {
            column = column.trimmed();
        }
    }
    if (parsed.name.isEmpty()) {
        if (error) *error = QStringLiteral("A binary FMT record has no message name.");
        return false;
    }

    int offset = HeaderSize;
    parsed.fields.reserve(parsed.format.size());
    for (int index = 0; index < parsed.format.size(); ++index) {
        const char format = parsed.format.at(index).toLatin1();
        const int size = formatSize(format);
        if (size == 0) {
            if (error) {
                *error = QStringLiteral(
                    "FMT %1 uses unsupported format character '%2'.")
                    .arg(parsed.name, QString(parsed.format.at(index)));
            }
            return false;
        }
        if (offset > std::numeric_limits<int>::max() - size) {
            if (error) *error = QStringLiteral("A binary FMT field layout overflowed.");
            return false;
        }
        Field field;
        field.offset = offset;
        field.size = size;
        field.format = format;
        parsed.fields.append(field);
        offset += size;
    }
    if (parsed.length < HeaderSize || parsed.length != offset) {
        if (error) {
            *error = QStringLiteral(
                "FMT %1 declares length %2 but its format requires %3 bytes.")
                .arg(parsed.name).arg(parsed.length).arg(offset);
        }
        return false;
    }
    if (parsed.columns.size() != parsed.fields.size()) {
        if (error) {
            *error = QStringLiteral(
                "FMT %1 has %2 fields but %3 column names.")
                .arg(parsed.name).arg(parsed.fields.size())
                .arg(parsed.columns.size());
        }
        return false;
    }
    for (int index = 0; index < parsed.fields.size(); ++index) {
        if (parsed.columns.at(index).isEmpty()) {
            if (error) {
                *error = QStringLiteral("FMT %1 has an empty column name.")
                    .arg(parsed.name);
            }
            return false;
        }
        parsed.fields[index].name = parsed.columns.at(index);
    }
    if ((parsed.typeId == FmtMessageId
         && (parsed.name.compare(QStringLiteral("FMT"), Qt::CaseInsensitive)
                 != 0
             || parsed.length != FmtMessageLength))
        || (parsed.name.compare(QStringLiteral("FMT"), Qt::CaseInsensitive)
                == 0
            && parsed.typeId != FmtMessageId)) {
        if (error) *error = QStringLiteral("The reserved FMT message definition is invalid.");
        return false;
    }
    *definition = std::move(parsed);
    return true;
}

bool fmtuFields(const Definition &definition,
                const Field **typeField, const Field **unitsField,
                const Field **multipliersField,
                QString *error)
{
    *typeField = nullptr;
    *unitsField = nullptr;
    *multipliersField = nullptr;
    for (const Field &field : definition.fields) {
        if (field.name.compare(QStringLiteral("FmtType"),
                               Qt::CaseInsensitive) == 0) {
            *typeField = &field;
        } else if (field.name.compare(QStringLiteral("UnitIds"),
                                      Qt::CaseInsensitive) == 0) {
            *unitsField = &field;
        } else if (field.name.compare(QStringLiteral("MultIds"),
                                      Qt::CaseInsensitive) == 0) {
            *multipliersField = &field;
        }
    }
    if (!*typeField || !*unitsField || !*multipliersField
        || (*typeField)->format != 'B'
        || (*typeField)->size != 1 || (*unitsField)->format != 'N'
        || (*unitsField)->size != 16
        || (*multipliersField)->format != 'N'
        || (*multipliersField)->size != 16) {
        if (error) {
            *error = QStringLiteral(
                "FMTU must contain byte FmtType plus 16-byte N UnitIds and MultIds fields.");
        }
        return false;
    }
    return true;
}

bool addDefinition(const Definition &definition, Metadata *metadata,
                   QString *error)
{
    const auto existing = metadata->definitions.constFind(definition.typeId);
    if (existing != metadata->definitions.constEnd()) {
        if (definitionsEqual(*existing, definition)) {
            return true;
        }
        if (error) {
            *error = QStringLiteral("Conflicting FMT redefinition for message type %1.")
                .arg(definition.typeId);
        }
        return false;
    }
    if (definition.name.compare(QStringLiteral("FMTU"),
                                Qt::CaseInsensitive) == 0) {
        for (auto candidate = metadata->definitions.constBegin();
             candidate != metadata->definitions.constEnd(); ++candidate) {
            if (candidate.key() != definition.typeId
                && candidate->name.compare(QStringLiteral("FMTU"),
                                           Qt::CaseInsensitive) == 0) {
                if (error) {
                    *error = QStringLiteral(
                        "More than one binary message type is defined as FMTU.");
                }
                return false;
            }
        }
        const Field *typeField = nullptr;
        const Field *unitsField = nullptr;
        const Field *multipliersField = nullptr;
        if (!fmtuFields(definition, &typeField, &unitsField,
                        &multipliersField, error)) {
            return false;
        }
    }
    metadata->definitions.insert(definition.typeId, definition);
    return true;
}

bool addFmtuRecord(const QByteArray &record, const Definition &definition,
                   Metadata *metadata, QString *error)
{
    const Field *typeField = nullptr;
    const Field *unitsField = nullptr;
    const Field *multipliersField = nullptr;
    if (!fmtuFields(definition, &typeField, &unitsField,
                    &multipliersField, error)) {
        return false;
    }
    if (typeField->offset + typeField->size > record.size()
        || unitsField->offset + unitsField->size > record.size()
        || multipliersField->offset + multipliersField->size > record.size()) {
        if (error) *error = QStringLiteral("A binary FMTU record is truncated.");
        return false;
    }
    const int describedType = static_cast<quint8>(record.at(typeField->offset));
    const QString units = readAscii(record, unitsField->offset,
                                    unitsField->size);
    const QString multipliers = readAscii(
        record, multipliersField->offset, multipliersField->size);
    const auto existing = metadata->fmtu.constFind(describedType);
    if (existing != metadata->fmtu.constEnd()
        && (existing->units != units
            || existing->multipliers != multipliers)) {
        if (error) {
            *error = QStringLiteral(
                "Conflicting FMTU redefinition for message type %1.")
                .arg(describedType);
        }
        return false;
    }
    metadata->fmtu.insert(describedType,
                          FmtuMapping{units, multipliers});
    return true;
}

enum class CancelState { Continue, Cancelled, Failed };

CancelState checkCancellation(const LogAnonymizeCancel &cancel,
                              QString *error)
{
    if (!cancel) {
        return CancelState::Continue;
    }
    try {
        return cancel() ? CancelState::Cancelled : CancelState::Continue;
    } catch (const std::exception &exception) {
        if (error) {
            *error = QStringLiteral("The cancellation callback failed: %1")
                .arg(QString::fromLocal8Bit(exception.what()));
        }
    } catch (...) {
        if (error) *error = QStringLiteral("The cancellation callback failed.");
    }
    return CancelState::Failed;
}

bool readExactly(QIODevice *device, qint64 count, QByteArray *bytes)
{
    bytes->resize(static_cast<int>(count));
    qint64 read = 0;
    while (read < count) {
        const qint64 amount = device->read(bytes->data() + read, count - read);
        if (amount <= 0) {
            return false;
        }
        read += amount;
    }
    return true;
}

bool scanMetadata(const QPointer<QIODevice> &input,
                  const QPointer<QIODevice> &output,
                  qint64 expectedSize,
                  const LogAnonymizeCancel &cancel,
                  Metadata *metadata, QByteArray *digest,
                  bool *cancelled, QString *error)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 position = 0;
    while (position < expectedSize) {
        const CancelState cancellation = checkCancellation(cancel, error);
        if (cancellation != CancelState::Continue) {
            *cancelled = cancellation == CancelState::Cancelled;
            return false;
        }
        if (!input || !output || !input->isReadable()
            || !output->isWritable() || input->pos() != position
            || output->pos() != 0) {
            if (error) {
                *error = QStringLiteral(
                    "An anonymization callback invalidated an I/O device.");
            }
            return false;
        }
        if (expectedSize - position < HeaderSize) {
            if (error) {
                *error = QStringLiteral(
                    "The binary DataFlash log ends with a truncated record header at byte %1.")
                    .arg(position);
            }
            return false;
        }
        QByteArray header;
        if (!readExactly(input.data(), HeaderSize, &header)) {
            if (error) *error = QStringLiteral("Reading the binary DataFlash log failed.");
            return false;
        }
        if (header.at(0) != Head1 || header.at(1) != Head2) {
            if (error) {
                *error = QStringLiteral(
                    "Unrecognized binary DataFlash framing at byte %1.")
                    .arg(position);
            }
            return false;
        }
        const int messageId = static_cast<quint8>(header.at(2));
        int length = FmtMessageLength;
        if (messageId != FmtMessageId) {
            const auto definition = metadata->definitions.constFind(messageId);
            if (definition == metadata->definitions.constEnd()) {
                if (error) {
                    *error = QStringLiteral(
                        "Message type %1 at byte %2 has no prior FMT definition.")
                        .arg(messageId).arg(position);
                }
                return false;
            }
            length = definition->length;
        }
        if (length < HeaderSize) {
            if (error) {
                *error = QStringLiteral(
                    "Message type %1 has an invalid length at byte %2.")
                    .arg(messageId).arg(position);
            }
            return false;
        }
        if (expectedSize - position < length) {
            // A power loss or logger close can leave one final known record
            // incomplete. Its safety cannot be decided until FMTU and the
            // complete coordinate patch map have been collected.
            QByteArray remainder;
            if (!readExactly(input.data(),
                             expectedSize - position - HeaderSize,
                             &remainder)) {
                if (error) *error = QStringLiteral("Reading the binary DataFlash tail failed.");
                return false;
            }
            metadata->trailingType = messageId;
            metadata->trailingOffset = position;
            metadata->trailingBytes = header;
            metadata->trailingBytes.append(remainder);
            hash.addData(metadata->trailingBytes);
            position = expectedSize;
            break;
        }
        QByteArray remainder;
        if (!readExactly(input.data(), length - HeaderSize, &remainder)) {
            if (error) *error = QStringLiteral("Reading the binary DataFlash log failed.");
            return false;
        }
        QByteArray record = header;
        record.append(remainder);
        hash.addData(record);

        if (messageId == FmtMessageId) {
            Definition definition;
            if (!parseDefinition(record, &definition, error)
                || !addDefinition(definition, metadata, error)) {
                return false;
            }
        } else {
            const Definition &definition = metadata->definitions[messageId];
            if (definition.name.compare(QStringLiteral("FMTU"),
                                        Qt::CaseInsensitive) == 0
                && !addFmtuRecord(record, definition, metadata, error)) {
                return false;
            }
        }
        position += length;
        ++metadata->records;
    }
    if (!input || !output || input->size() != expectedSize
        || output->pos() != 0 || output->size() != 0) {
        if (error) *error = QStringLiteral("The input changed during its metadata scan.");
        return false;
    }
    *digest = hash.result();
    return true;
}

bool fallbackCoordinate(const QString &name, char format, bool *latitude)
{
    static const QSet<QString> latitudeNames = {
        QStringLiteral("lat"), QStringLiteral("hlat"),
        QStringLiteral("dlat"), QStringLiteral("oalat"),
        QStringLiteral("dlt"), QStringLiteral("olt"),
        QStringLiteral("elat"), QStringLiteral("olat"),
        QStringLiteral("clat"), QStringLiteral("trlat"),
        QStringLiteral("wplat"), QStringLiteral("rlat"),
        QStringLiteral("tp_lat")};
    static const QSet<QString> longitudeNames = {
        QStringLiteral("lng"), QStringLiteral("lon"),
        QStringLiteral("hlon"), QStringLiteral("hlng"),
        QStringLiteral("dlng"), QStringLiteral("oalng"),
        QStringLiteral("dlg"), QStringLiteral("olg"),
        QStringLiteral("elng"), QStringLiteral("olng"),
        QStringLiteral("clng"), QStringLiteral("trlng"),
        QStringLiteral("wplng"), QStringLiteral("rlng"),
        QStringLiteral("tp_lng")};
    const QString lower = name.trimmed().toLower();
    if (latitudeNames.contains(lower)) {
        *latitude = true;
        return true;
    }
    if (longitudeNames.contains(lower)) {
        *latitude = false;
        return true;
    }
    if (format != 'L' && format != 'i' && format != 'I') {
        return false;
    }
    // The reference's broad "lt"/"lg" substring heuristic also matches
    // altitude-like names. Retain only unambiguous coordinate suffixes; the
    // full historical aliases above remain exact and case-insensitive.
    if (lower.endsWith(QStringLiteral("lat"))) {
        *latitude = true;
        return true;
    }
    if (lower.endsWith(QStringLiteral("lng"))
        || lower.endsWith(QStringLiteral("lon"))) {
        *latitude = false;
        return true;
    }
    return false;
}

bool supportedCoordinateFormat(char format)
{
    return format == 'L' || format == 'i' || format == 'I'
        || format == 'f' || format == 'd';
}

bool identifyPatches(Metadata *metadata, qint64 *coordinateFields,
                     QStringList *warnings, QString *error)
{
    *coordinateFields = 0;
    for (auto item = metadata->definitions.constBegin();
         item != metadata->definitions.constEnd(); ++item) {
        const Definition &definition = item.value();
        const QString upperName = definition.name.toUpper();
        if (upperName == QStringLiteral("FMT")
            || upperName == QStringLiteral("FMTU")
            || upperName == QStringLiteral("MULT")
            || upperName == QStringLiteral("UNIT")) {
            continue;
        }
        QVector<Patch> patches;
        const auto fmtu = metadata->fmtu.constFind(definition.typeId);
        if (fmtu != metadata->fmtu.constEnd()) {
            for (int index = definition.fields.size();
                 index < fmtu->units.size(); ++index) {
                const QChar unit = fmtu->units.at(index);
                if (unit == QLatin1Char('D') || unit == QLatin1Char('U')) {
                    if (error) {
                        *error = QStringLiteral(
                            "FMTU marks a missing field as a coordinate in %1.")
                            .arg(definition.name);
                    }
                    return false;
                }
            }
        }
        for (int index = 0; index < definition.fields.size(); ++index) {
            const Field &field = definition.fields.at(index);
            const bool hasFmtu = fmtu != metadata->fmtu.constEnd();
            const bool hasUnit = hasFmtu && index < fmtu->units.size();
            const QChar unit = hasUnit ? fmtu->units.at(index) : QChar();
            if (unit == QLatin1Char('D') || unit == QLatin1Char('U')) {
                if (!supportedCoordinateFormat(field.format)) {
                    if (error) {
                        *error = QStringLiteral(
                            "Coordinate field %1.%2 uses unsupported format '%3'.")
                            .arg(definition.name, field.name,
                                 QString(QLatin1Char(field.format)));
                    }
                    return false;
                }
                patches.append(Patch{field.offset, field.size, field.format,
                                     unit == QLatin1Char('D'), field.name});
                continue;
            }

            bool latitude = false;
            if (!fallbackCoordinate(field.name, field.format, &latitude)) {
                continue;
            }
            if (hasFmtu && hasUnit && unit != QLatin1Char('-')
                && unit != QLatin1Char('?')) {
                if (error) {
                    *error = QStringLiteral(
                        "Coordinate field %1.%2 has explicit non-coordinate FMTU unit '%3'.")
                        .arg(definition.name, field.name, QString(unit));
                }
                return false;
            }
            if (!supportedCoordinateFormat(field.format)) {
                if (error) {
                    *error = QStringLiteral(
                        "Coordinate field %1.%2 uses unsupported format '%3'.")
                        .arg(definition.name, field.name,
                             QString(QLatin1Char(field.format)));
                }
                return false;
            }
            patches.append(Patch{field.offset, field.size, field.format,
                                 latitude, field.name});
            if (hasFmtu && warnings) {
                const QString annotation = hasUnit ? QString(unit)
                                                   : QStringLiteral("missing");
                warnings->append(QStringLiteral(
                    "FMTU has no D/U annotation for %1.%2 (unit %3); patched by coordinate name.")
                    .arg(definition.name, field.name, annotation));
            }
        }
        if (!patches.isEmpty()) {
            *coordinateFields += patches.size();
            metadata->patches.insert(definition.typeId, patches);
        }
    }
    if (*coordinateFields == 0) {
        if (error) {
            *error = QStringLiteral(
                "No coordinate fields were found in the binary DataFlash metadata.");
        }
        return false;
    }
    return true;
}

qint32 readI32(const QByteArray &record, int offset)
{
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(record.constData() + offset));
}

quint32 readU32(const QByteArray &record, int offset)
{
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(record.constData() + offset));
}

quint64 readU64(const QByteArray &record, int offset)
{
    return qFromLittleEndian<quint64>(
        reinterpret_cast<const uchar *>(record.constData() + offset));
}

void writeI32(QByteArray *record, int offset, qint32 value)
{
    qToLittleEndian<qint32>(
        value, reinterpret_cast<uchar *>(record->data() + offset));
}

void writeU32(QByteArray *record, int offset, quint32 value)
{
    qToLittleEndian<quint32>(
        value, reinterpret_cast<uchar *>(record->data() + offset));
}

void writeU64(QByteArray *record, int offset, quint64 value)
{
    qToLittleEndian<quint64>(
        value, reinterpret_cast<uchar *>(record->data() + offset));
}

bool patchValue(QByteArray *record, const Patch &patch, double degreeOffset,
                bool *patched, QString *error)
{
    *patched = false;
    if (patch.offset < HeaderSize || patch.offset > record->size() - patch.size) {
        if (error) *error = QStringLiteral("A coordinate field lies outside its record.");
        return false;
    }
    if (!std::isfinite(degreeOffset)) {
        if (error) *error = QStringLiteral("Coordinate offsets must be finite numbers.");
        return false;
    }
    switch (patch.format) {
    case 'L':
    case 'i': {
        const qint32 oldValue = readI32(*record, patch.offset);
        if (oldValue == 0) return true;
        const double shifted = static_cast<double>(oldValue)
            + degreeOffset * 10000000.0;
        if (!std::isfinite(shifted)
            || shifted < std::numeric_limits<qint32>::min()
            || shifted > std::numeric_limits<qint32>::max()) {
            if (error) *error = QStringLiteral("A signed coordinate offset overflowed.");
            return false;
        }
        writeI32(record, patch.offset, static_cast<qint32>(shifted));
        break;
    }
    case 'I': {
        // ArduPilot uses I for several semantically signed latitude and
        // longitude fields. Preserve the wire bits while applying signed
        // arithmetic so an ordinary crossing of zero remains valid.
        const qint32 oldValue = readI32(*record, patch.offset);
        if (oldValue == 0) return true;
        const double shifted = static_cast<double>(oldValue)
            + degreeOffset * 10000000.0;
        if (!std::isfinite(shifted)
            || shifted < std::numeric_limits<qint32>::min()
            || shifted > std::numeric_limits<qint32>::max()) {
            if (error) *error = QStringLiteral("A signed coordinate offset overflowed.");
            return false;
        }
        writeI32(record, patch.offset, static_cast<qint32>(shifted));
        break;
    }
    case 'f': {
        const quint32 bits = readU32(*record, patch.offset);
        float oldValue = 0.0F;
        std::memcpy(&oldValue, &bits, sizeof(oldValue));
        if (!std::isfinite(oldValue)) {
            if (error) *error = QStringLiteral("A floating-point coordinate is not finite.");
            return false;
        }
        if (oldValue == 0.0F) return true;
        const double shifted = static_cast<double>(oldValue)
            + degreeOffset * (std::fabs(oldValue) > 1000.0F
                                  ? 10000000.0 : 1.0);
        if (!std::isfinite(shifted)
            || std::fabs(shifted) > std::numeric_limits<float>::max()) {
            if (error) *error = QStringLiteral("A floating-point coordinate overflowed.");
            return false;
        }
        const float stored = static_cast<float>(shifted);
        quint32 storedBits = 0;
        std::memcpy(&storedBits, &stored, sizeof(storedBits));
        writeU32(record, patch.offset, storedBits);
        break;
    }
    case 'd': {
        const quint64 bits = readU64(*record, patch.offset);
        double oldValue = 0.0;
        std::memcpy(&oldValue, &bits, sizeof(oldValue));
        if (!std::isfinite(oldValue)) {
            if (error) *error = QStringLiteral("A double coordinate is not finite.");
            return false;
        }
        if (oldValue == 0.0) return true;
        const double shifted = oldValue + degreeOffset;
        if (!std::isfinite(shifted)) {
            if (error) *error = QStringLiteral("A double coordinate overflowed.");
            return false;
        }
        quint64 storedBits = 0;
        std::memcpy(&storedBits, &shifted, sizeof(storedBits));
        writeU64(record, patch.offset, storedBits);
        break;
    }
    default:
        if (error) {
            *error = QStringLiteral("Coordinate field %1 has an unsupported format.")
                .arg(patch.fieldName);
        }
        return false;
    }
    *patched = true;
    return true;
}

bool writeExactly(QIODevice *output, const QByteArray &bytes)
{
    qint64 written = 0;
    while (written < bytes.size()) {
        const qint64 amount = output->write(
            bytes.constData() + written, bytes.size() - written);
        if (amount <= 0) {
            return false;
        }
        written += amount;
    }
    return true;
}

bool reportProgress(const LogAnonymizeProgress &progress,
                    qint64 completed, qint64 total, QString *error)
{
    if (!progress) return true;
    try {
        progress(completed, total);
        return true;
    } catch (const std::exception &exception) {
        if (error) {
            *error = QStringLiteral("The progress callback failed: %1")
                .arg(QString::fromLocal8Bit(exception.what()));
        }
    } catch (...) {
        if (error) *error = QStringLiteral("The progress callback failed.");
    }
    return false;
}

bool rewriteIoStateIsValid(const QPointer<QIODevice> &input,
                           const QPointer<QIODevice> &output,
                           qint64 expectedInputPosition,
                           qint64 expectedOutputBytes,
                           bool validateOutputSize)
{
    return input && output && input->isReadable() && output->isWritable()
        && input->pos() == expectedInputPosition
        && output->pos() == expectedOutputBytes
        && (!validateOutputSize || output->size() == expectedOutputBytes);
}

} // namespace

LogAnonymizeResult DataFlashLogAnonymizer::anonymize(
    QIODevice *input, QIODevice *output,
    const LogAnonymizeOptions &options,
    const LogAnonymizeCancel &cancel,
    const LogAnonymizeProgress &progress)
{
    LogAnonymizeResult result;
    if (!input || !output || input == output) {
        result.error = QStringLiteral("Separate input and staging-output devices are required.");
        return result;
    }
    QPointer<QIODevice> inputGuard(input);
    QPointer<QIODevice> outputGuard(output);
    if (!input->isOpen() || !input->isReadable() || input->isSequential()) {
        result.error = QStringLiteral("The binary DataFlash input must be open, readable and seekable.");
        return result;
    }
    if (!output->isOpen() || !output->isWritable()
        || output->pos() != 0 || output->size() != 0) {
        result.error = QStringLiteral("The staging output must be open, writable and empty.");
        return result;
    }
    if (!std::isfinite(options.latitudeOffset)
        || !std::isfinite(options.longitudeOffset)) {
        result.error = QStringLiteral("Coordinate offsets must be finite numbers.");
        return result;
    }
    const qint64 inputSize = input->size();
    result.inputBytes = inputSize;
    if (inputSize < HeaderSize || !input->seek(0)) {
        result.error = QStringLiteral("The binary DataFlash input is empty or cannot be rewound.");
        return result;
    }

    Metadata metadata;
    QByteArray originalDigest;
    bool cancelled = false;
    if (!scanMetadata(inputGuard, outputGuard, inputSize, cancel, &metadata,
                      &originalDigest, &cancelled, &result.error)) {
        result.cancelled = cancelled;
        if (cancelled) {
            result.error = QStringLiteral("DataFlash anonymization was cancelled.");
        }
        return result;
    }
    if (!identifyPatches(&metadata, &result.coordinateFields,
                         &result.warnings, &result.error)) {
        return result;
    }
    if (metadata.trailingType >= 0) {
        const auto definition = metadata.definitions.constFind(
            metadata.trailingType);
        if (metadata.trailingType == FmtMessageId
            || definition == metadata.definitions.constEnd()
            || definition->name.compare(QStringLiteral("FMTU"),
                                        Qt::CaseInsensitive) == 0) {
            result.error = QStringLiteral(
                "The binary DataFlash log ends with incomplete metadata at byte %1.")
                .arg(metadata.trailingOffset);
            return result;
        }
        if (metadata.patches.contains(metadata.trailingType)) {
            result.error = QStringLiteral(
                "The binary DataFlash log ends with an incomplete coordinate record %1 at byte %2.")
                .arg(definition->name).arg(metadata.trailingOffset);
            return result;
        }
        result.warnings.append(QStringLiteral(
            "Preserved %1 trailing bytes of incomplete non-coordinate %2 (type %3) record.")
            .arg(metadata.trailingBytes.size()).arg(definition->name)
            .arg(metadata.trailingType));
    }
    if (!inputGuard || !outputGuard || !input->seek(0)) {
        result.error = QStringLiteral("The input device disappeared or could not be rewound.");
        return result;
    }
    if (!reportProgress(progress, 0, inputSize, &result.error)) {
        return result;
    }
    if (!rewriteIoStateIsValid(inputGuard, outputGuard, 0, 0, true)) {
        result.error = QStringLiteral("An anonymization callback invalidated an I/O device.");
        return result;
    }

    QCryptographicHash rewriteHash(QCryptographicHash::Sha256);
    QHash<int, Definition> seenDefinitions;
    QHash<int, FmtuMapping> seenFmtu;
    qint64 position = 0;
    qint64 lastReportedPosition = 0;
    while (position < inputSize) {
        const CancelState cancellation = checkCancellation(cancel, &result.error);
        if (cancellation != CancelState::Continue) {
            result.cancelled = cancellation == CancelState::Cancelled;
            if (result.cancelled) {
                result.error = QStringLiteral("DataFlash anonymization was cancelled.");
            }
            return result;
        }
        if (!rewriteIoStateIsValid(inputGuard, outputGuard, position,
                                   result.outputBytes, false)
            || inputSize - position < HeaderSize) {
            result.error = QStringLiteral("The input changed during DataFlash rewriting.");
            return result;
        }
        QByteArray header;
        if (!readExactly(input, HeaderSize, &header)
            || header.at(0) != Head1 || header.at(1) != Head2) {
            result.error = QStringLiteral("The input changed or has invalid framing during rewriting.");
            return result;
        }
        const int messageId = static_cast<quint8>(header.at(2));
        int length = FmtMessageLength;
        if (messageId != FmtMessageId) {
            const auto definition = seenDefinitions.constFind(messageId);
            if (definition == seenDefinitions.constEnd()) {
                result.error = QStringLiteral("The input message ordering changed during rewriting.");
                return result;
            }
            length = definition->length;
        }
        if (length < HeaderSize) {
            result.error = QStringLiteral("The input record length changed during rewriting.");
            return result;
        }
        if (inputSize - position < length) {
            QByteArray remainder;
            if (!readExactly(input, inputSize - position - HeaderSize,
                             &remainder)) {
                result.error = QStringLiteral(
                    "Reading the input tail during DataFlash rewriting failed.");
                return result;
            }
            QByteArray original = header;
            original.append(remainder);
            rewriteHash.addData(original);
            if (metadata.trailingType != messageId
                || metadata.trailingOffset != position
                || metadata.trailingBytes != original) {
                result.error = QStringLiteral(
                    "The incomplete input tail changed during DataFlash rewriting.");
                return result;
            }
            if (!writeExactly(output, original)) {
                result.error = QStringLiteral(
                    "Writing the preserved DataFlash tail failed.");
                return result;
            }
            position = inputSize;
            result.outputBytes += original.size();
            if (!reportProgress(progress, position, inputSize,
                                &result.error)) {
                return result;
            }
            if (!rewriteIoStateIsValid(inputGuard, outputGuard, position,
                                       result.outputBytes, true)) {
                result.error = QStringLiteral(
                    "An anonymization callback invalidated an I/O device.");
                return result;
            }
            break;
        }
        QByteArray remainder;
        if (!readExactly(input, length - HeaderSize, &remainder)) {
            result.error = QStringLiteral("Reading the input during DataFlash rewriting failed.");
            return result;
        }
        QByteArray original = header;
        original.append(remainder);
        rewriteHash.addData(original);
        QByteArray rewritten = original;

        if (messageId == FmtMessageId) {
            Definition definition;
            QString definitionError;
            if (!parseDefinition(original, &definition, &definitionError)) {
                result.error = QStringLiteral("The input FMT changed during rewriting: %1")
                    .arg(definitionError);
                return result;
            }
            const auto prior = seenDefinitions.constFind(definition.typeId);
            if (prior != seenDefinitions.constEnd()
                && !definitionsEqual(*prior, definition)) {
                result.error = QStringLiteral("The input FMT changed during rewriting.");
                return result;
            }
            seenDefinitions.insert(definition.typeId, definition);
        } else {
            const Definition &definition = seenDefinitions[messageId];
            if (definition.name.compare(QStringLiteral("FMTU"),
                                        Qt::CaseInsensitive) == 0) {
                Metadata observed;
                observed.fmtu = seenFmtu;
                QString unitError;
                if (!addFmtuRecord(original, definition, &observed, &unitError)) {
                    result.error = QStringLiteral("The input FMTU changed during rewriting: %1")
                        .arg(unitError);
                    return result;
                }
                seenFmtu = observed.fmtu;
            }
            const auto patches = metadata.patches.constFind(messageId);
            if (patches != metadata.patches.constEnd()) {
                for (const Patch &patch : *patches) {
                    bool didPatch = false;
                    if (!patchValue(&rewritten, patch,
                                    patch.latitude
                                        ? options.latitudeOffset
                                        : options.longitudeOffset,
                                    &didPatch, &result.error)) {
                        result.error = QStringLiteral("Cannot anonymize %1.%2: %3")
                            .arg(definition.name, patch.fieldName, result.error);
                        return result;
                    }
                    if (didPatch) ++result.patchedValues;
                }
            }
        }

        if (!writeExactly(output, rewritten)) {
            result.error = QStringLiteral("Writing the anonymized staging output failed.");
            return result;
        }
        position += length;
        result.outputBytes += length;
        ++result.records;
        if (progress && (position == inputSize
                         || position - lastReportedPosition
                             >= ProgressByteInterval)) {
            if (!reportProgress(progress, position, inputSize,
                                &result.error)) {
                return result;
            }
            if (!rewriteIoStateIsValid(inputGuard, outputGuard, position,
                                       result.outputBytes, true)) {
                result.error = QStringLiteral(
                    "An anonymization callback invalidated an I/O device.");
                return result;
            }
            lastReportedPosition = position;
        }
    }
    if (!rewriteIoStateIsValid(inputGuard, outputGuard, inputSize,
                               result.outputBytes, true)
        || input->size() != inputSize
        || rewriteHash.result() != originalDigest) {
        result.error = QStringLiteral("The input changed between metadata scan and rewriting.");
        return result;
    }
    result.success = true;
    return result;
}
