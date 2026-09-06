#include "ShapefileGeometryReader.h"

#include "NativeGdalLibrary.h"

#include <QDate>
#include <QLibrary>
#include <QRegularExpression>
#include <QSet>
#include <QTextCodec>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace {
constexpr int ShpHeaderBytes = 100;
constexpr quint32 ShpFileCode = 9994;
constexpr quint32 ShpVersion = 1000;
constexpr quint32 NullShape = 0;
constexpr quint32 Point = 1;
constexpr quint32 PolyLine = 3;
constexpr quint32 Polygon = 5;
constexpr quint32 MultiPoint = 8;
constexpr quint32 PointZ = 11;
constexpr quint32 PolyLineZ = 13;
constexpr quint32 PolygonZ = 15;
constexpr quint32 MultiPointZ = 18;
constexpr quint32 PointM = 21;
constexpr quint32 PolyLineM = 23;
constexpr quint32 PolygonM = 25;
constexpr quint32 MultiPointM = 28;
constexpr quint32 MultiPatch = 31;

void assignError(QString *error, const QString &message)
{
    if (error) *error = message;
}

class CancellationProbe final
{
public:
    explicit CancellationProbe(const Shapefile::Cancel &cancel)
        : m_cancel(cancel)
    {
    }

    bool poll(QString *error)
    {
        if (m_cancel && m_cancel()) {
            m_cancelled = true;
            assignError(error, QStringLiteral("Shapefile reading was cancelled."));
            return true;
        }
        return false;
    }

    bool cancelled() const { return m_cancelled; }

private:
    const Shapefile::Cancel &m_cancel;
    bool m_cancelled = false;
};

bool checkedAdd(qint64 left, qint64 right, qint64 *value)
{
    if (!value || left < 0 || right < 0
        || left > std::numeric_limits<qint64>::max() - right) {
        return false;
    }
    *value = left + right;
    return true;
}

bool checkedMultiply(qint64 left, qint64 right, qint64 *value)
{
    if (!value || left < 0 || right < 0
        || (right != 0 && left > std::numeric_limits<qint64>::max() / right)) {
        return false;
    }
    *value = left * right;
    return true;
}

class Cursor final
{
public:
    Cursor(const QByteArray &bytes, qint64 begin = 0, qint64 end = -1)
        : m_bytes(bytes), m_position(begin),
          m_end(end < 0 ? bytes.size() : end)
    {
    }

    qint64 position() const { return m_position; }
    qint64 remaining() const { return m_end - m_position; }
    bool atEnd() const { return m_position == m_end; }

    bool skip(qint64 count)
    {
        if (!available(count)) return false;
        m_position += count;
        return true;
    }

    bool readU8(quint8 *value)
    {
        if (!value || !available(1)) return false;
        *value = static_cast<quint8>(m_bytes.at(static_cast<int>(m_position)));
        ++m_position;
        return true;
    }

    bool readU16Le(quint16 *value)
    {
        if (!value || !available(2)) return false;
        const uchar *data = reinterpret_cast<const uchar *>(m_bytes.constData())
            + m_position;
        *value = quint16(data[0]) | (quint16(data[1]) << 8);
        m_position += 2;
        return true;
    }

    bool readU32Le(quint32 *value)
    {
        if (!value || !available(4)) return false;
        const uchar *data = reinterpret_cast<const uchar *>(m_bytes.constData())
            + m_position;
        *value = quint32(data[0]) | (quint32(data[1]) << 8)
            | (quint32(data[2]) << 16) | (quint32(data[3]) << 24);
        m_position += 4;
        return true;
    }

    bool readU32Be(quint32 *value)
    {
        if (!value || !available(4)) return false;
        const uchar *data = reinterpret_cast<const uchar *>(m_bytes.constData())
            + m_position;
        *value = (quint32(data[0]) << 24) | (quint32(data[1]) << 16)
            | (quint32(data[2]) << 8) | quint32(data[3]);
        m_position += 4;
        return true;
    }

    bool readDoubleLe(double *value)
    {
        quint64 bits = 0;
        if (!value || !available(8)) return false;
        const uchar *data = reinterpret_cast<const uchar *>(m_bytes.constData())
            + m_position;
        for (int index = 0; index < 8; ++index)
            bits |= quint64(data[index]) << (8 * index);
        std::memcpy(value, &bits, sizeof(bits));
        m_position += 8;
        return true;
    }

    QByteArray readBytes(int count)
    {
        if (count < 0 || !available(count)) return {};
        const QByteArray result = m_bytes.mid(static_cast<int>(m_position), count);
        m_position += count;
        return result;
    }

private:
    bool available(qint64 count) const
    {
        return count >= 0 && m_position >= 0 && m_position <= m_end
            && count <= m_end - m_position;
    }

    const QByteArray &m_bytes;
    qint64 m_position = 0;
    qint64 m_end = 0;
};

bool isPointType(quint32 type)
{
    return type == Point || type == PointM || type == PointZ;
}

bool isMultiPointType(quint32 type)
{
    return type == MultiPoint || type == MultiPointM || type == MultiPointZ;
}

bool isPolyLineType(quint32 type)
{
    return type == PolyLine || type == PolyLineM || type == PolyLineZ;
}

bool isPolygonType(quint32 type)
{
    return type == Polygon || type == PolygonM || type == PolygonZ;
}

bool isSupportedType(quint32 type)
{
    return isPointType(type) || isMultiPointType(type)
        || isPolyLineType(type) || isPolygonType(type);
}

bool hasZ(quint32 type)
{
    return type == PointZ || type == MultiPointZ
        || type == PolyLineZ || type == PolygonZ;
}

bool hasRequiredM(quint32 type)
{
    return type == PointM || type == MultiPointM
        || type == PolyLineM || type == PolygonM;
}

QTextCodec *dbfCodec(const QByteArray &cpg)
{
    QByteArray name = cpg;
    if (name.startsWith("\xef\xbb\xbf")) name.remove(0, 3);
    name = name.trimmed();
    if (!name.isEmpty()) {
        if (QTextCodec *codec = QTextCodec::codecForName(name)) return codec;
        bool numeric = false;
        const int codePage = QString::fromLatin1(name).toInt(&numeric);
        if (numeric) {
            if (QTextCodec *codec = QTextCodec::codecForName(
                    QByteArray("Windows-") + QByteArray::number(codePage))) {
                return codec;
            }
            if (QTextCodec *codec = QTextCodec::codecForName(
                    QByteArray("IBM ") + QByteArray::number(codePage))) {
                return codec;
            }
        }
    }
    return QTextCodec::codecForName("UTF-8");
}

struct DbfField
{
    char type = 0;
    int length = 0;
    int scale = 0;
};

struct DbfTable
{
    QVector<bool> deleted;
};

bool validInteger(const QString &text, int bits)
{
    static const QRegularExpression expression(
        QStringLiteral("^[+-]?[0-9]+$"));
    if (!expression.match(text).hasMatch()) return false;
    bool ok = false;
    if (bits == 32) (void)text.toInt(&ok, 10);
    else (void)text.toLongLong(&ok, 10);
    return ok;
}

bool validDecimal(const QString &text)
{
    static const QRegularExpression expression(QStringLiteral(
        "^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$"));
    if (!expression.match(text).hasMatch()) return false;
    bool ok = false;
    (void)text.toDouble(&ok);
    return ok;
}

bool validateDbfValue(const DbfField &field, const QByteArray &bytes,
                      QString *error)
{
    const QByteArray trimmedBytes = bytes.trimmed();
    const QString text = QString::fromLatin1(trimmedBytes);
    switch (field.type) {
    case 'C':
    case 'L':
        return true;
    case 'D': {
        if (text.isEmpty() || text == QStringLiteral("00000000")) return true;
        if (text.size() != 8) {
            assignError(error, QStringLiteral("Invalid dBASE date field value."));
            return false;
        }
        bool yearOk = false, monthOk = false, dayOk = false;
        const int year = text.left(4).toInt(&yearOk);
        const int month = text.mid(4, 2).toInt(&monthOk);
        const int day = text.mid(6, 2).toInt(&dayOk);
        if (!yearOk || !monthOk || !dayOk || !QDate(year, month, day).isValid()) {
            assignError(error, QStringLiteral("Invalid dBASE date field value."));
            return false;
        }
        return true;
    }
    case 'N':
    case 'F': {
        QString number = text;
        while (number.startsWith(QLatin1Char('*'))) number.remove(0, 1);
        while (number.endsWith(QLatin1Char('*'))) number.chop(1);
        if (number.trimmed().isEmpty()) return true;
        bool valid = false;
        if (field.type == 'F' || field.scale > 0) valid = validDecimal(number);
        else if (field.length <= 10) valid = validInteger(number, 32);
        else if (field.length <= 19) valid = validInteger(number, 64);
        else valid = validDecimal(number);
        if (!valid) {
            assignError(error, QStringLiteral("Invalid numeric dBASE field value."));
            return false;
        }
        return true;
    }
    default:
        assignError(error, QStringLiteral("Unsupported dBASE III field type."));
        return false;
    }
}

bool parseDbf(const QByteArray &dbf, const QByteArray &cpg,
              DbfTable *table, CancellationProbe *probe, QString *error)
{
    if (!table) return false;
    table->deleted.clear();
    if (dbf.isEmpty()) {
        assignError(error, QStringLiteral("The dBASE sidecar is empty."));
        return false;
    }
    if (dbf.size() > Shapefile::MaximumDbfBytes) {
        assignError(error, QStringLiteral("The dBASE sidecar exceeds the safety limit."));
        return false;
    }
    if (cpg.size() > Shapefile::MaximumCpgBytes) {
        assignError(error, QStringLiteral("The CPG sidecar exceeds the safety limit."));
        return false;
    }
    if (dbf.size() < 32) {
        assignError(error, QStringLiteral("The dBASE III header is truncated."));
        return false;
    }

    Cursor cursor(dbf);
    quint8 version = 0, yearOffset = 0, month = 0, day = 0;
    quint32 recordCount = 0;
    quint16 headerSize = 0, recordSize = 0;
    if (!cursor.readU8(&version) || !cursor.readU8(&yearOffset)
        || !cursor.readU8(&month) || !cursor.readU8(&day)
        || !cursor.readU32Le(&recordCount) || !cursor.readU16Le(&headerSize)
        || !cursor.readU16Le(&recordSize) || !cursor.skip(20)) {
        assignError(error, QStringLiteral("The dBASE III header is truncated."));
        return false;
    }
    if (version != 3) {
        assignError(error, QStringLiteral("Only dBASE III version 3 is supported."));
        return false;
    }
    const int safeMonth = qMax<int>(month, 1);
    const int safeDay = qMax<int>(day, 1);
    if (!QDate(1900 + yearOffset, safeMonth, safeDay).isValid()) {
        assignError(error, QStringLiteral("The dBASE update date is invalid."));
        return false;
    }
    if (recordCount > quint32(Shapefile::MaximumFeatures)) {
        assignError(error, QStringLiteral("The dBASE record count exceeds the safety limit."));
        return false;
    }
    if (headerSize < 33 || (headerSize - 33) % 32 != 0
        || headerSize > dbf.size()) {
        assignError(error, QStringLiteral("The dBASE III header size is invalid."));
        return false;
    }
    const int fieldCount = (headerSize - 33) / 32;
    if (fieldCount > 255) {
        assignError(error, QStringLiteral("The dBASE field count exceeds the safety limit."));
        return false;
    }

    QTextCodec *codec = dbfCodec(cpg);
    QVector<DbfField> fields;
    fields.reserve(fieldCount);
    QSet<QString> names;
    qint64 fieldsBytes = 1;
    for (int index = 0; index < fieldCount; ++index) {
        if (index != 0 && index % 64 == 0 && probe && probe->poll(error))
            return false;
        const QByteArray descriptor = cursor.readBytes(32);
        if (descriptor.size() != 32) {
            assignError(error, QStringLiteral("The dBASE field table is truncated."));
            return false;
        }
        QByteArray nameBytes = descriptor.left(10);
        while (!nameBytes.isEmpty() && nameBytes.endsWith('\0')) nameBytes.chop(1);
        QString name = codec ? codec->toUnicode(nameBytes).trimmed()
                             : QString::fromUtf8(nameBytes).trimmed();
        if (name.isEmpty() || name.size() > 10) {
            assignError(error, QStringLiteral("The dBASE field name is invalid."));
            return false;
        }
        const QString folded = name.toCaseFolded();
        if (names.contains(folded)) {
            assignError(error, QStringLiteral("The dBASE field names are not unique."));
            return false;
        }
        names.insert(folded);

        char type = static_cast<char>(descriptor.at(11));
        if (type >= 'a' && type <= 'z') type = char(type - 'a' + 'A');
        if (type == 'S') type = 'C';
        const int length = static_cast<uchar>(descriptor.at(16));
        const int scale = static_cast<uchar>(descriptor.at(17));
        if ((type != 'C' && type != 'D' && type != 'N'
             && type != 'F' && type != 'L') || length < 1) {
            assignError(error, QStringLiteral("The dBASE field definition is invalid."));
            return false;
        }
        if (!checkedAdd(fieldsBytes, length, &fieldsBytes)) {
            assignError(error, QStringLiteral("The dBASE record layout overflows."));
            return false;
        }
        fields.append({type, length, scale});
    }
    quint8 terminator = 0;
    if (!cursor.readU8(&terminator) || terminator != 0x0d
        || cursor.position() != headerSize) {
        assignError(error, QStringLiteral("The dBASE III header terminator is invalid."));
        return false;
    }
    if (recordSize < fieldsBytes) {
        assignError(error, QStringLiteral("The dBASE record size is smaller than its fields."));
        return false;
    }
    qint64 recordsBytes = 0, expectedEnd = 0;
    if (!checkedMultiply(recordCount, recordSize, &recordsBytes)
        || !checkedAdd(headerSize, recordsBytes, &expectedEnd)
        || expectedEnd > dbf.size()) {
        assignError(error, QStringLiteral("The dBASE record data is truncated."));
        return false;
    }

    table->deleted.reserve(static_cast<int>(recordCount));
    for (quint32 record = 0; record < recordCount; ++record) {
        if (record != 0 && probe && probe->poll(error)) return false;
        Cursor row(dbf, headerSize + qint64(record) * recordSize,
                   headerSize + qint64(record + 1) * recordSize);
        quint8 marker = 0;
        if (!row.readU8(&marker)) {
            assignError(error, QStringLiteral("The dBASE record is truncated."));
            return false;
        }
        int fieldIndex = 0;
        for (const DbfField &field : fields) {
            if (fieldIndex != 0 && fieldIndex % 64 == 0
                && probe && probe->poll(error)) {
                return false;
            }
            const QByteArray value = row.readBytes(field.length);
            if (value.size() != field.length
                || !validateDbfValue(field, value, error)) {
                return false;
            }
            ++fieldIndex;
        }
        table->deleted.append(marker == 0x2a);
    }
    return true;
}

void appendU32Le(QByteArray *bytes, quint32 value)
{
    bytes->append(char(value & 0xff));
    bytes->append(char((value >> 8) & 0xff));
    bytes->append(char((value >> 16) & 0xff));
    bytes->append(char((value >> 24) & 0xff));
}

void appendDoubleLe(QByteArray *bytes, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int index = 0; index < 8; ++index)
        bytes->append(char((bits >> (8 * index)) & 0xff));
}

bool same2d(const Shapefile::Coordinate &left,
            const Shapefile::Coordinate &right)
{
    return left.x == right.x && left.y == right.y;
}

int orientationIndex(const Shapefile::Coordinate &first,
                     const Shapefile::Coordinate &second,
                     const Shapefile::Coordinate &third)
{
    const long double dx1 = static_cast<long double>(second.x) - first.x;
    const long double dy1 = static_cast<long double>(second.y) - first.y;
    const long double dx2 = static_cast<long double>(third.x) - second.x;
    const long double dy2 = static_cast<long double>(third.y) - second.y;
    const long double determinant = dx1 * dy2 - dy1 * dx2;
    return determinant > 0 ? 1 : determinant < 0 ? -1 : 0;
}

// Mirrors NetTopologySuite.Algorithm.Orientation.IsCCW. Its top-edge method,
// rather than a signed-area shortcut, determines the strict ESRI ring groups.
bool isCcw(const QVector<Shapefile::Coordinate> &ring, bool *ccw,
           CancellationProbe *probe, QString *error)
{
    if (!ccw) return false;
    *ccw = false;
    const int countWithoutClose = ring.size() - 1;
    if (countWithoutClose < 3) return true;
    Shapefile::Coordinate upHi = ring.first();
    double previousY = upHi.y;
    Shapefile::Coordinate upLow;
    bool hasUpLow = false;
    int upHiIndex = 0;
    for (int index = 1; index <= countWithoutClose; ++index) {
        if (index % 1024 == 0 && probe && probe->poll(error)) return false;
        const double y = ring.at(index).y;
        if (y > previousY && y >= upHi.y) {
            upHi = ring.at(index);
            upHiIndex = index;
            upLow = ring.at(index - 1);
            hasUpLow = true;
        }
        previousY = y;
    }
    if (upHiIndex == 0 || !hasUpLow) return true;

    int downHiIndex = upHiIndex;
    do {
        downHiIndex = (downHiIndex + 1) % countWithoutClose;
    } while (downHiIndex != upHiIndex
             && ring.at(downHiIndex).y == upHi.y);
    const Shapefile::Coordinate downHi = ring.at(downHiIndex);
    const int downLowIndex = downHiIndex > 0
        ? downHiIndex - 1 : countWithoutClose - 1;
    const Shapefile::Coordinate downLow = ring.at(downLowIndex);
    if (same2d(upHi, downLow)) {
        if (same2d(upLow, upHi) || same2d(downHi, upHi)
            || same2d(upLow, downHi)) {
            return true;
        }
        *ccw = orientationIndex(upLow, upHi, downHi) > 0;
        return true;
    }
    *ccw = downLow.x - upHi.x < 0.0;
    return true;
}

bool polygonWkb(const QVector<QVector<Shapefile::Coordinate>> &parts,
                QByteArray *bytes, CancellationProbe *probe, QString *error)
{
    if (!bytes) return false;
    bytes->clear();
    struct PolygonRings {
        QVector<QVector<Shapefile::Coordinate>> rings;
    };
    QVector<PolygonRings> polygons;
    if (!parts.isEmpty()) {
        PolygonRings current;
        current.rings.append(parts.first());
        for (int index = 1; index < parts.size(); ++index) {
            if (index % 256 == 0 && probe && probe->poll(error)) return false;
            bool ccw = false;
            if (!isCcw(parts.at(index), &ccw, probe, error)) return false;
            if (ccw) {
                current.rings.append(parts.at(index));
            } else {
                polygons.append(current);
                current.rings.clear();
                current.rings.append(parts.at(index));
            }
        }
        polygons.append(current);
    }

    bytes->reserve(9 + parts.size() * 9);
    bytes->append(char(1));
    appendU32Le(bytes, 6); // OGC WKB MultiPolygon.
    appendU32Le(bytes, static_cast<quint32>(polygons.size()));
    int polygonIndex = 0;
    for (const PolygonRings &polygon : polygons) {
        if (polygonIndex != 0 && polygonIndex % 64 == 0
            && probe && probe->poll(error)) {
            return false;
        }
        bytes->append(char(1));
        appendU32Le(bytes, 3); // OGC WKB Polygon.
        appendU32Le(bytes, static_cast<quint32>(polygon.rings.size()));
        for (const QVector<Shapefile::Coordinate> &ring : polygon.rings) {
            appendU32Le(bytes, static_cast<quint32>(ring.size()));
            int coordinateIndex = 0;
            for (const Shapefile::Coordinate &coordinate : ring) {
                if (coordinateIndex != 0 && coordinateIndex % 1024 == 0
                    && probe && probe->poll(error)) {
                    return false;
                }
                appendDoubleLe(bytes, coordinate.x);
                appendDoubleLe(bytes, coordinate.y);
                ++coordinateIndex;
            }
        }
        ++polygonIndex;
    }
    return true;
}

class GdalPolygonValidator final
{
public:
    // ogr_api.h declares these CPL_DLL functions with the default C calling
    // convention (unlike the small set of OSR/CPLError CPL_STDCALL APIs).
    using CreateFromWkb = int (*)(const void *, void *, void **, int);
    using IsValid = int (*)(void *);
    using DestroyGeometry = void (*)(void *);
    using GetGeosVersion = bool (*)(int *, int *, int *);

    static const GdalPolygonValidator &instance()
    {
        static const GdalPolygonValidator validator;
        return validator;
    }

    bool available() const { return m_available; }
    QString error() const { return m_error; }

    bool validate(const QByteArray &wkb) const
    {
        return m_available && nativeValidate(wkb);
    }

private:
    bool nativeValidate(const QByteArray &wkb) const
    {
        if (!m_create || !m_isValid || !m_destroy || wkb.isEmpty()) return false;
        void *geometry = nullptr;
        const int result = m_create(
            static_cast<const void *>(wkb.constData()),
            nullptr, &geometry, wkb.size());
        if (result != 0 || !geometry) return false;
        const bool valid = m_isValid(geometry) != 0;
        m_destroy(geometry);
        return valid;
    }

    GdalPolygonValidator()
    {
        for (const QString &candidate : nativeGdalLibraryCandidates()) {
            std::unique_ptr<QLibrary> library(new QLibrary(candidate));
            library->setLoadHints(QLibrary::ResolveAllSymbolsHint
                                  | QLibrary::PreventUnloadHint);
            if (!library->load()) continue;
            m_create = reinterpret_cast<CreateFromWkb>(
                library->resolve("OGR_G_CreateFromWkb"));
            m_isValid = reinterpret_cast<IsValid>(
                library->resolve("OGR_G_IsValid"));
            m_destroy = reinterpret_cast<DestroyGeometry>(
                library->resolve("OGR_G_DestroyGeometry"));
            m_getGeosVersion = reinterpret_cast<GetGeosVersion>(
                library->resolve("OGRGetGEOSVersion"));
            if (m_create && m_isValid && m_destroy) {
                int major = 0, minor = 0, patch = 0;
                if (m_getGeosVersion
                    && !m_getGeosVersion(&major, &minor, &patch)) {
                    m_error = QStringLiteral(
                        "Native GDAL/GEOS polygon validation is unavailable: this GDAL runtime has no usable GEOS support.");
                    continue;
                }
                const QVector<Shapefile::Coordinate> ring{
                    {0, 0, 0}, {4, 0, 0}, {4, 4, 0},
                    {0, 4, 0}, {0, 0, 0}};
                QByteArray square;
                if (!polygonWkb({ring}, &square, nullptr, nullptr)
                    || !nativeValidate(square)) {
                    m_error = QStringLiteral(
                        "Native GDAL/GEOS polygon validation is unavailable: the runtime failed its known-valid square self-test.");
                    continue;
                }
                m_available = true;
                // PreventUnloadHint keeps the native module resident after the
                // QObject wrapper is destroyed, so the function pointers stay valid.
                library.reset();
                return;
            }
        }
        if (m_error.isEmpty()) {
            m_error = QStringLiteral(
                "Native GDAL/GEOS polygon validation is unavailable; install a current "
                "GDAL runtime or set MISSIONPLANNER_GDAL_LIBRARY to its exact path.");
        }
    }

    bool m_available = false;
    QString m_error;
    CreateFromWkb m_create = nullptr;
    IsValid m_isValid = nullptr;
    DestroyGeometry m_destroy = nullptr;
    GetGeosVersion m_getGeosVersion = nullptr;
};

bool checkExactSize(qint64 actual, qint64 mandatory, qint64 optional,
                    bool optionalAllowed, QString *error)
{
    if (actual == mandatory) return true;
    if (optionalAllowed && optional > 0 && actual == mandatory + optional)
        return true;
    assignError(error, QStringLiteral("The shape record has an invalid or partial Z/M layout."));
    return false;
}

bool parsePoint(const QByteArray &record, quint32 type,
                Shapefile::Feature *feature, qint64 *pointTotal,
                QString *error)
{
    if (*pointTotal == Shapefile::MaximumPoints) {
        assignError(error, QStringLiteral("The shape point count exceeds the safety limit."));
        return false;
    }
    const qint64 mandatory = hasZ(type) ? 28 : hasRequiredM(type) ? 28 : 20;
    const bool optionalM = hasZ(type);
    if (!checkExactSize(record.size(), mandatory, 8, optionalM, error)) return false;
    Cursor cursor(record, 4);
    Shapefile::Coordinate coordinate;
    if (!cursor.readDoubleLe(&coordinate.x) || !cursor.readDoubleLe(&coordinate.y)) {
        assignError(error, QStringLiteral("The point record is truncated."));
        return false;
    }
    coordinate.z = 0.0;
    if (hasZ(type) && !cursor.readDoubleLe(&coordinate.z)) {
        assignError(error, QStringLiteral("The PointZ record is truncated."));
        return false;
    }
    feature->points.append(coordinate);
    ++*pointTotal;
    return true;
}

bool readCoordinates(Cursor *cursor, quint32 pointCount,
                     QVector<Shapefile::Coordinate> *points,
                     CancellationProbe *probe, QString *error)
{
    if (!cursor || !points) return false;
    points->resize(static_cast<int>(pointCount));
    for (quint32 index = 0; index < pointCount; ++index) {
        if (index != 0 && index % 1024 == 0
            && probe && probe->poll(error)) {
            return false;
        }
        if (!cursor->readDoubleLe(&(*points)[static_cast<int>(index)].x)
            || !cursor->readDoubleLe(&(*points)[static_cast<int>(index)].y)) {
            assignError(error, QStringLiteral("The shape coordinate array is truncated."));
            return false;
        }
    }
    return true;
}

bool applyZ(Cursor *cursor, QVector<Shapefile::Coordinate> *points,
            CancellationProbe *probe, QString *error)
{
    double ignored = 0.0;
    if (!cursor->readDoubleLe(&ignored) || !cursor->readDoubleLe(&ignored)) {
        assignError(error, QStringLiteral("The Z range is truncated."));
        return false;
    }
    int index = 0;
    for (Shapefile::Coordinate &coordinate : *points) {
        if (index != 0 && index % 1024 == 0
            && probe && probe->poll(error)) {
            return false;
        }
        if (!cursor->readDoubleLe(&coordinate.z)) {
            assignError(error, QStringLiteral("The Z coordinate array is truncated."));
            return false;
        }
        ++index;
    }
    return true;
}

bool parseMultiPoint(const QByteArray &record, quint32 type,
                     Shapefile::Feature *feature, qint64 *pointTotal,
                     CancellationProbe *probe, QString *error)
{
    if (record.size() < 40) {
        assignError(error, QStringLiteral("The MultiPoint record is truncated."));
        return false;
    }
    Cursor countCursor(record, 36);
    quint32 pointCount = 0;
    if (!countCursor.readU32Le(&pointCount)
        || pointCount > quint32(Shapefile::MaximumPoints)
        || *pointTotal > Shapefile::MaximumPoints - qint64(pointCount)) {
        assignError(error, QStringLiteral("The shape point count exceeds the safety limit."));
        return false;
    }
    qint64 xyBytes = 0, baseSize = 0;
    if (!checkedMultiply(pointCount, 16, &xyBytes)
        || !checkedAdd(40, xyBytes, &baseSize)) {
        assignError(error, QStringLiteral("The MultiPoint record size overflows."));
        return false;
    }
    const qint64 axisBlock = 16 + qint64(pointCount) * 8;
    const qint64 mandatory = baseSize + (hasZ(type) ? axisBlock
                                      : hasRequiredM(type) ? axisBlock : 0);
    if (!checkExactSize(record.size(), mandatory, axisBlock, hasZ(type), error))
        return false;

    Cursor cursor(record, 4);
    if (!cursor.skip(32) || !cursor.skip(4)
        || !readCoordinates(&cursor, pointCount, &feature->points,
                            probe, error)) {
        return false;
    }
    if (hasZ(type) && !applyZ(&cursor, &feature->points, probe, error))
        return false;
    *pointTotal += pointCount;
    return true;
}

bool parseMultiPart(const QByteArray &record, quint32 type,
                    Shapefile::Feature *feature, qint64 *pointTotal,
                    CancellationProbe *probe, QString *error)
{
    if (record.size() < 44) {
        assignError(error, QStringLiteral("The PolyLine/Polygon record is truncated."));
        return false;
    }
    Cursor countCursor(record, 36);
    quint32 partCount = 0, pointCount = 0;
    if (!countCursor.readU32Le(&partCount)
        || !countCursor.readU32Le(&pointCount)
        || partCount > quint32(Shapefile::MaximumPoints)
        || pointCount > quint32(Shapefile::MaximumPoints)
        || *pointTotal > Shapefile::MaximumPoints - qint64(pointCount)) {
        assignError(error, QStringLiteral("The shape part/point count exceeds the safety limit."));
        return false;
    }
    if ((partCount == 0) != (pointCount == 0) || partCount > pointCount) {
        assignError(error, QStringLiteral("The shape part/point counts are inconsistent."));
        return false;
    }
    qint64 offsetsBytes = 0, xyBytes = 0, baseSize = 0;
    if (!checkedMultiply(partCount, 4, &offsetsBytes)
        || !checkedMultiply(pointCount, 16, &xyBytes)
        || !checkedAdd(44, offsetsBytes, &baseSize)
        || !checkedAdd(baseSize, xyBytes, &baseSize)) {
        assignError(error, QStringLiteral("The PolyLine/Polygon record size overflows."));
        return false;
    }
    const qint64 axisBlock = 16 + qint64(pointCount) * 8;
    const qint64 mandatory = baseSize + (hasZ(type) ? axisBlock
                                      : hasRequiredM(type) ? axisBlock : 0);
    if (!checkExactSize(record.size(), mandatory, axisBlock, hasZ(type), error))
        return false;

    Cursor cursor(record, 4);
    if (!cursor.skip(32) || !cursor.skip(8)) {
        assignError(error, QStringLiteral("The shape part table is truncated."));
        return false;
    }
    QVector<quint32> offsets;
    offsets.reserve(static_cast<int>(partCount));
    for (quint32 index = 0; index < partCount; ++index) {
        if (index != 0 && index % 1024 == 0
            && probe && probe->poll(error)) {
            return false;
        }
        quint32 offset = 0;
        if (!cursor.readU32Le(&offset)) {
            assignError(error, QStringLiteral("The shape part table is truncated."));
            return false;
        }
        if ((index == 0 && offset != 0)
            || (index > 0 && offset <= offsets.constLast())
            || offset >= pointCount) {
            assignError(error, QStringLiteral("The shape part offsets are invalid."));
            return false;
        }
        offsets.append(offset);
    }
    if (!readCoordinates(&cursor, pointCount, &feature->points, probe, error))
        return false;
    if (hasZ(type) && !applyZ(&cursor, &feature->points, probe, error))
        return false;
    *pointTotal += pointCount;

    QVector<QVector<Shapefile::Coordinate>> parts;
    parts.reserve(static_cast<int>(partCount));
    for (quint32 index = 0; index < partCount; ++index) {
        if (index != 0 && index % 256 == 0
            && probe && probe->poll(error)) {
            return false;
        }
        const quint32 end = index + 1 < partCount
            ? offsets.at(static_cast<int>(index + 1)) : pointCount;
        const quint32 begin = offsets.at(static_cast<int>(index));
        const quint32 minimum = isPolygonType(type) ? 4u : 2u;
        if (end - begin < minimum) {
            assignError(error, isPolygonType(type)
                ? QStringLiteral("A polygon ring has fewer than four coordinates.")
                : QStringLiteral("A polyline part has fewer than two coordinates."));
            return false;
        }
        QVector<Shapefile::Coordinate> part;
        part.reserve(static_cast<int>(end - begin));
        for (quint32 point = begin; point < end; ++point) {
            if (point != begin && (point - begin) % 1024 == 0
                && probe && probe->poll(error)) {
                return false;
            }
            part.append(feature->points.at(static_cast<int>(point)));
        }
        if (isPolygonType(type) && !same2d(part.first(), part.last())) {
            assignError(error, QStringLiteral("A polygon ring is not closed."));
            return false;
        }
        parts.append(part);
    }

    if (isPolygonType(type) && !parts.isEmpty()) {
        const GdalPolygonValidator &validator = GdalPolygonValidator::instance();
        if (!validator.available()) {
            assignError(error, validator.error());
            return false;
        }
        QByteArray wkb;
        if (!polygonWkb(parts, &wkb, probe, error)) return false;
        if (probe && probe->poll(error)) return false;
        if (!validator.validate(wkb)) {
            assignError(error, QStringLiteral("Invalid polygon geometry."));
            return false;
        }
        if (probe && probe->poll(error)) return false;
    }
    return true;
}

bool parseRecord(const QByteArray &record, quint32 fileType,
                 Shapefile::Feature *feature, bool *skipped,
                 qint64 *pointTotal, CancellationProbe *probe,
                 QString *error)
{
    if (!feature || !skipped || !pointTotal || record.size() < 4) {
        assignError(error, QStringLiteral("The shape record is truncated."));
        return false;
    }
    *skipped = false;
    Cursor cursor(record);
    quint32 recordType = 0;
    if (!cursor.readU32Le(&recordType)) return false;
    if (recordType == NullShape) {
        if (record.size() != 4) {
            assignError(error, QStringLiteral("The null-shape record size is invalid."));
            return false;
        }
        return true;
    }
    if (recordType != fileType) {
        // NTS records and skips an unexpected per-record shape type even in
        // Strict mode. The paired DBF row is still consumed.
        *skipped = true;
        return true;
    }
    if (isPointType(recordType))
        return parsePoint(record, recordType, feature, pointTotal, error);
    if (isMultiPointType(recordType))
        return parseMultiPoint(record, recordType, feature, pointTotal,
                               probe, error);
    return parseMultiPart(record, recordType, feature, pointTotal,
                          probe, error);
}
}

Shapefile::GeometryResult ShapefileGeometryReader::read(
    const QByteArray &shp, bool hasDbf, const QByteArray &dbf,
    const QByteArray &cpg, const Shapefile::Cancel &cancel,
    const Shapefile::Progress &progress)
{
    // Pin all caller-owned value inputs before the first cancellation/progress
    // callback. A callback may otherwise detach or clear the original arrays.
    const QByteArray sourceShp = shp;
    const QByteArray sourceDbf = dbf;
    const QByteArray sourceCpg = cpg;
    const Shapefile::Cancel stopped = cancel;
    const Shapefile::Progress notify = progress;
    Shapefile::GeometryResult result;
    CancellationProbe probe(stopped);
    if (sourceShp.size() > Shapefile::MaximumShpBytes) {
        result.error = QStringLiteral("The SHP file exceeds the safety limit.");
        return result;
    }
    if (sourceShp.size() < ShpHeaderBytes) {
        result.error = QStringLiteral("The SHP header is truncated.");
        return result;
    }
    if (probe.poll(&result.error)) {
        result.cancelled = true;
        return result;
    }

    DbfTable table;
    if (hasDbf && !parseDbf(sourceDbf, sourceCpg, &table,
                            &probe, &result.error)) {
        result.cancelled = probe.cancelled();
        return result;
    }

    Cursor header(sourceShp);
    quint32 fileCode = 0, declaredWords = 0, version = 0, fileType = 0;
    if (!header.readU32Be(&fileCode) || !header.skip(20)
        || !header.readU32Be(&declaredWords) || !header.readU32Le(&version)
        || !header.readU32Le(&fileType) || !header.skip(64)) {
        result.error = QStringLiteral("The SHP header is truncated.");
        return result;
    }
    if (fileCode != ShpFileCode || version != ShpVersion) {
        result.error = QStringLiteral("The SHP header code or version is invalid.");
        return result;
    }
    if (fileType == MultiPatch) {
        result.error = QStringLiteral("MultiPatch shapefiles are unsupported.");
        return result;
    }
    if (!isSupportedType(fileType)) {
        result.error = QStringLiteral("The SHP geometry type is unsupported.");
        return result;
    }
    const qint64 declaredBytes = qint64(declaredWords) * 2;
    if (declaredBytes < ShpHeaderBytes || declaredBytes > sourceShp.size()) {
        result.error = QStringLiteral("The SHP declared file length is invalid or truncated.");
        return result;
    }
    if (declaredBytes < sourceShp.size()) {
        result.warnings.append(QStringLiteral(
            "Bytes after the declared SHP file length were ignored, as by the reference reader."));
    }

    Cursor records(sourceShp, ShpHeaderBytes, declaredBytes);
    int sourceRecord = 0;
    int mismatchedTypes = 0;
    qint64 pointTotal = 0;
    while (!records.atEnd()
           && (!hasDbf || sourceRecord < table.deleted.size())) {
        if (sourceRecord == Shapefile::MaximumFeatures) {
            result.error = QStringLiteral("The SHP feature count exceeds the safety limit.");
            result.features.clear();
            return result;
        }
        if (probe.poll(&result.error)) {
            result.cancelled = true;
            result.features.clear();
            return result;
        }
        if (records.remaining() < 8) {
            result.error = QStringLiteral("The SHP record header is truncated.");
            result.features.clear();
            return result;
        }
        quint32 ignoredNumber = 0, contentWords = 0;
        if (!records.readU32Be(&ignoredNumber) || !records.readU32Be(&contentWords)) {
            result.error = QStringLiteral("The SHP record header is truncated.");
            result.features.clear();
            return result;
        }
        const qint64 contentBytes = qint64(contentWords) * 2;
        if (contentBytes < 4 || contentBytes > records.remaining()
            || contentBytes > std::numeric_limits<int>::max()) {
            result.error = QStringLiteral("The SHP record content length is invalid or truncated.");
            result.features.clear();
            return result;
        }
        const QByteArray record = records.readBytes(static_cast<int>(contentBytes));
        Shapefile::Feature feature;
        bool skipped = false;
        if (!parseRecord(record, fileType, &feature, &skipped,
                         &pointTotal, &probe, &result.error)) {
            result.cancelled = probe.cancelled();
            result.features.clear();
            return result;
        }
        const bool deleted = hasDbf && table.deleted.at(sourceRecord);
        ++sourceRecord;
        if (skipped) {
            ++mismatchedTypes;
        } else if (!deleted) {
            if (result.features.size() == Shapefile::MaximumFeatures) {
                result.error = QStringLiteral("The SHP feature count exceeds the safety limit.");
                result.features.clear();
                return result;
            }
            result.features.append(feature);
        }
        if (notify) {
            notify(records.position(), declaredBytes,
                   QStringLiteral("Reading shapefile geometry"));
            if (probe.poll(&result.error)) {
                result.cancelled = true;
                result.features.clear();
                return result;
            }
        }
    }

    if (hasDbf && sourceRecord < table.deleted.size()) {
        result.error = QStringLiteral(
            "Corrupted shapefile data: the dBASE table has more records than the SHP file.");
        result.features.clear();
        return result;
    }
    if (hasDbf && sourceRecord == table.deleted.size() && !records.atEnd()) {
        result.warnings.append(QStringLiteral(
            "The dBASE table ended before the declared SHP data; remaining SHP records "
            "were ignored, matching the reference reader."));
    }
    if (mismatchedTypes > 0) {
        result.warnings.append(QStringLiteral(
            "%1 record(s) with a shape type different from the SHP header were skipped.")
            .arg(mismatchedTypes));
    }
    result.success = true;
    if (notify) {
        notify(declaredBytes, declaredBytes,
               QStringLiteral("Shapefile geometry ready"));
        if (probe.poll(&result.error)) {
            result.success = false;
            result.cancelled = true;
            result.features.clear();
        }
    }
    return result;
}
