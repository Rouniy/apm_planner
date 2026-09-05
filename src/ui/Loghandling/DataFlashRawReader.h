#ifndef DATAFLASHRAWREADER_H
#define DATAFLASHRAWREADER_H

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QList>
#include <QSet>
#include <QVector>
#include <QtEndian>
#include <cmath>
#include <cstring>

// Shared bounded raw reader for file tools. Unlike plot-oriented log parsers,
// it never synthesizes columns, rewrites timestamps or applies display units.
// The caller owns the QFile and cancellation/publication policy. kind values:
// 0 FMT; 1 FMTU; 2 UNIT; 3 MULT; -1 ordinary; -2 ignored blank/known EOF tail.
namespace DataFlashRaw {
constexpr qint64 TextLineLimit = 4 * 1024 * 1024;
constexpr int FmtId = 128;
constexpr int FmtLength = 89;

inline int width(char type)
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

inline QByteArray ascii(const QByteArray &bytes)
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

inline bool parseDefinition(const QByteArray &raw, bool text, Definition *out, QString *error)
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

inline int metadataKind(const QByteArray &name)
{
    if (name == "FMT") return 0;
    if (name == "FMTU") return 1;
    if (name == "UNIT") return 2;
    if (name == "MULT") return 3;
    return -1;
}

inline QList<QByteArray> textValues(const QByteArray &raw, const Definition &definition)
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
    QList<int> definitionOrder;
    Definition currentDefinition;
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
            if (!definitions.contains(d.id)) definitionOrder.append(d.id);
            definitions.insert(d.id, d); names.insert(d.name, d.id);
        } else if (*kind > 0 && !checkMetadata(*raw, d)) {
            return false;
        }
        currentDefinition = d;
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

} // namespace DataFlashRaw

#endif // DATAFLASHRAWREADER_H
