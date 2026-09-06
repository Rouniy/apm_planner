#include "GeoRefExif.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#endif

namespace {
constexpr qint64 MaximumFile = 0xffffffffLL;
constexpr qint64 MaximumMetadata = 16 * 1024 * 1024;
constexpr int Chunk = 65536;
constexpr int MaximumDirectories = 256;
constexpr int MaximumEntries = 65536;

struct FileIdentity {
    quint64 volume = 0, high = 0, low = 0;
    bool valid = false;
    bool operator==(const FileIdentity &other) const {
        return valid && other.valid && volume == other.volume && high == other.high && low == other.low;
    }
};
FileIdentity identity(QFileDevice *file)
{
    FileIdentity result;
    if (!file || file->handle() < 0) return result;
#ifdef Q_OS_UNIX
    struct stat status;
    if (::fstat(file->handle(), &status) == 0 && S_ISREG(status.st_mode)) {
        result.volume = quint64(status.st_dev); result.low = quint64(status.st_ino); result.valid = true;
    }
#elif defined(Q_OS_WIN)
    const intptr_t native = ::_get_osfhandle(file->handle());
    BY_HANDLE_FILE_INFORMATION info;
    if (native != -1 && ::GetFileInformationByHandle(reinterpret_cast<HANDLE>(native), &info)
            && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.volume = info.dwVolumeSerialNumber; result.high = info.nFileIndexHigh;
        result.low = info.nFileIndexLow; result.valid = true;
    }
#endif
    return result;
}

bool stopped(const GeoRefExif::Cancel &cancel) { return cancel && cancel(); }
quint16 u16(const char *p, bool le)
{ return le ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p); }
quint32 u32(const char *p, bool le)
{ return le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p); }
void put16(QByteArray &b, quint16 n, bool le)
{ char p[2]; if (le) qToLittleEndian(n, p); else qToBigEndian(n, p); b.append(p, 2); }
void put32(QByteArray &b, quint32 n, bool le)
{ char p[4]; if (le) qToLittleEndian(n, p); else qToBigEndian(n, p); b.append(p, 4); }
int width(int type)
{
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: case 13: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
    }
}

struct Entry {
    quint16 tag = 0, type = 0;
    quint32 count = 0;
    qint64 offset = 0, bytes = 0;
    QByteArray raw;
};
struct Directory { QVector<Entry> entries; quint32 next = 0; };

// TIFF offsets remain relative to the ORIGINAL TIFF header. We never relocate
// existing value blocks (including vendor-relative MakerNote and thumbnails).
class Tiff
{
public:
    Tiff(QIODevice *input, qint64 size, const GeoRefExif::Cancel &cancel)
        : input(input), size(size), cancel(cancel) {}
    bool parse()
    {
        QByteArray h;
        if (!read(0, 8, &h)) return false;
        if (h.left(2) != "II" && h.left(2) != "MM") return fail("Invalid TIFF byte order.");
        le = h.left(2) == "II";
        if (u16(h.constData() + 2, le) == 43) return fail("BigTIFF is not supported; classic TIFF is required.");
        if (u16(h.constData() + 2, le) != 42) return fail("Invalid classic TIFF signature.");
        rootOffset = u32(h.constData() + 4, le);
        if (rootOffset < 8) return fail("TIFF has no valid root IFD.");
        return visit(rootOffset, 0);
    }
    bool read(qint64 offset, qint64 length, QByteArray *out)
    {
        if (stopped(cancel)) return fail("Cancelled.");
        if (offset < 0 || length < 0 || offset > size || length > size - offset)
            return fail("TIFF metadata offset/length is outside the source.");
        if (length > MaximumMetadata || consumed > MaximumMetadata - length)
            return fail("TIFF metadata exceeds the 16 MiB read budget.");
        consumed += length;
        if (!input->seek(offset)) return fail("Could not seek TIFF metadata.");
        *out = input->read(length);
        if (out->size() != length) return fail("Could not read complete TIFF metadata.");
        return true;
    }
    bool value(const Entry &e, QByteArray *out) { return read(e.offset, e.bytes, out); }
    const Entry *find(const Directory &d, int tag) const
    { for (const Entry &e : d.entries) if (e.tag == tag) return &e; return nullptr; }
    bool integers(const Entry &e, QVector<quint32> *out)
    {
        if (e.type != 3 && e.type != 4 && e.type != 13)
            return fail("TIFF offset/count field has an unsupported type.");
        QByteArray b; if (!value(e, &b)) return false;
        out->reserve(int(e.count));
        for (quint32 i = 0; i < e.count; ++i)
            out->append(e.type == 3 ? u16(b.constData() + i * 2, le) : u32(b.constData() + i * 4, le));
        return true;
    }
    bool pixelRanges(const Directory &d, int offsetsTag, int lengthsTag)
    {
        const Entry *a = find(d, offsetsTag), *b = find(d, lengthsTag);
        if (!a && !b) return true;
        if (!a || !b || a->count != b->count) return fail("TIFF image offsets and byte counts do not match.");
        QVector<quint32> offsets, lengths;
        if (!integers(*a, &offsets) || !integers(*b, &lengths)) return false;
        for (int i = 0; i < offsets.size(); ++i) {
            if (offsets[i] > size || lengths[i] > size - offsets[i])
                return fail("TIFF strip/tile/thumbnail is outside the source.");
        }
        return true;
    }
    bool visit(quint32 offset, int depth)
    {
        if (depth > 16) return fail("TIFF IFD nesting exceeds 16 levels.");
        if (active.contains(offset)) return fail("Cyclic TIFF IFD offsets.");
        if (directories.contains(offset)) return true;
        if (directories.size() + active.size() >= MaximumDirectories)
            return fail("TIFF contains more than 256 IFDs.");
        if (offset < 8) return fail("Invalid TIFF IFD offset.");
        QByteArray countBytes, table;
        if (!read(offset, 2, &countBytes)) return false;
        const int n = u16(countBytes.constData(), le);
        if (entryCount + n > MaximumEntries) return fail("TIFF contains more than 65536 entries.");
        entryCount += n;
        if (!read(qint64(offset) + 2, qint64(n) * 12 + 4, &table)) return false;
        Directory d;
        QSet<int> tags;
        for (int i = 0; i < n; ++i) {
            if (stopped(cancel)) return fail("Cancelled.");
            Entry e; e.raw = table.mid(i * 12, 12);
            e.tag = u16(e.raw.constData(), le); e.type = u16(e.raw.constData() + 2, le);
            e.count = u32(e.raw.constData() + 4, le);
            if (tags.contains(e.tag)) return fail("Duplicate tag in a TIFF IFD.");
            tags.insert(e.tag);
            const int w = width(e.type);
            if (!w) return fail("Unsupported TIFF field type.");
            e.bytes = qint64(e.count) * w;
            if (e.bytes > MaximumMetadata) return fail("TIFF field exceeds the 16 MiB metadata bound.");
            e.offset = e.bytes <= 4 ? qint64(offset) + 2 + i * 12 + 8 : u32(e.raw.constData() + 8, le);
            if ((e.bytes > 4 && e.offset < 8) || e.offset < 0 || e.offset > size || e.bytes > size - e.offset)
                return fail("TIFF field references bytes outside the source.");
            d.entries.append(e);
        }
        d.next = u32(table.constData() + n * 12, le);
        if (!pixelRanges(d, 273, 279) || !pixelRanges(d, 324, 325) || !pixelRanges(d, 513, 514)) return false;
        active.insert(offset);
        for (const Entry &e : d.entries) {
            if (e.tag != 0x8769 && e.tag != 0x8825 && e.tag != 0xa005 && e.tag != 330 && e.type != 13) continue;
            if (((e.tag == 0x8769 || e.tag == 0x8825 || e.tag == 0xa005) && e.count != 1)
                    || (e.type != 4 && e.type != 13))
                return fail("Invalid TIFF sub-IFD pointer.");
            QVector<quint32> pointers;
            if (!integers(e, &pointers)) return false;
            for (quint32 p : pointers) if (p && !visit(p, depth + 1)) return false;
        }
        // Next-IFD chains are sibling pages, not deeper sub-IFDs. The active
        // set still detects cycles and the independent directory cap bounds it.
        if (d.next && !visit(d.next, depth)) return false;
        active.remove(offset);
        directories.insert(offset, d);
        return true;
    }
    bool child(const Directory &d, int tag, Directory *out)
    {
        const Entry *e = find(d, tag); if (!e) return false;
        QByteArray b; if (!value(*e, &b)) return false;
        const quint32 offset = u32(b.constData(), le);
        if (!offset) return false;
        *out = directories.value(offset); return true;
    }
    QDateTime date(const Directory &d, int tag)
    {
        const Entry *e = find(d, tag);
        if (!e || e->type != 2 || e->count < 19 || e->count > 128) return {};
        QByteArray b; if (!value(*e, &b)) return {};
        const QString value = QString::fromLatin1(b.constData(), 19);
        QDateTime result = QDateTime::fromString(value, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
        result.setTimeSpec(Qt::LocalTime);
        return result;
    }
    bool rational(const Directory &d, int tag, int count, QVector<double> *out)
    {
        const Entry *e = find(d, tag);
        if (!e || e->type != 5 || e->count != quint32(count)) {
            gpsWarning = QStringLiteral("Existing GPS EXIF rational has an invalid type/count or is missing.");
            return false;
        }
        QByteArray b; if (!value(*e, &b)) return false;
        for (int i = 0; i < count; ++i) {
            const quint32 den = u32(b.constData() + i * 8 + 4, le);
            if (!den) {
                gpsWarning = QStringLiteral("Existing GPS EXIF rational has a zero denominator.");
                return false;
            }
            out->append(double(u32(b.constData() + i * 8, le)) / den);
        }
        return true;
    }
    bool rejectGps(GeoRefExif::Metadata *m, const char *why)
    {
        m->hasCoordinates = false;
        m->coordinates = {};
        m->coordinateWarning = QString::fromLatin1(why);
        return error.isEmpty();
    }
    bool metadata(GeoRefExif::Metadata *m)
    {
        const Directory root = directories.value(rootOffset);
        Directory exif;
        if (child(root, 0x8769, &exif)) {
            m->photoTime = date(exif, 0x9003);
            if (!m->photoTime.isValid()) m->photoTime = date(exif, 0x9004);
        }
        if (!error.isEmpty()) return false;
        Directory gps;
        if (child(root, 0x8825, &gps)) {
            QVector<double> lat, lon, alt;
            const Entry *latRef = find(gps, 1), *lonRef = find(gps, 3), *altRef = find(gps, 5);
            if (!latRef || !lonRef || latRef->type != 2 || latRef->count != 2
                    || lonRef->type != 2 || lonRef->count != 2)
                return rejectGps(m, "Existing GPS EXIF hemisphere reference has an invalid type/count or is missing.");
            if (rational(gps, 2, 3, &lat) && rational(gps, 4, 3, &lon)) {
                QByteArray lr, gr, ar;
                if (!value(*latRef, &lr) || !value(*lonRef, &gr)) return false;
                if ((lr != QByteArray("N\0", 2) && lr != QByteArray("S\0", 2))
                        || (gr != QByteArray("E\0", 2) && gr != QByteArray("W\0", 2)))
                    return rejectGps(m, "Invalid existing GPS EXIF hemisphere reference.");
                if (lat[1] >= 60 || lat[2] >= 60 || lon[1] >= 60 || lon[2] >= 60)
                    return rejectGps(m, "Invalid existing GPS EXIF minutes/seconds.");
                m->coordinates.latitude = (lat[0] + lat[1] / 60 + lat[2] / 3600) * (lr[0] == 'S' ? -1 : 1);
                m->coordinates.longitude = (lon[0] + lon[1] / 60 + lon[2] / 3600) * (gr[0] == 'W' ? -1 : 1);
                if (std::abs(m->coordinates.latitude) > 90 || std::abs(m->coordinates.longitude) > 180)
                    return rejectGps(m, "Existing GPS EXIF coordinate is outside latitude/longitude bounds.");
                if (find(gps, 6) && rational(gps, 6, 1, &alt)) {
                    m->coordinates.altitude = alt[0];
                }
                if (altRef) {
                    if (altRef->type != 1 || altRef->count != 1)
                        return rejectGps(m, "Invalid existing GPS altitude reference type/count.");
                    if (!value(*altRef, &ar)) return false;
                    if (quint8(ar[0]) > 1)
                        return rejectGps(m, "Invalid existing GPS altitude reference.");
                    if (ar[0] == 1) m->coordinates.altitude = -m->coordinates.altitude;
                }
                m->hasCoordinates = true;
            }
        }
        if (!gpsWarning.isEmpty()) {
            m->hasCoordinates = false;
            m->coordinates = {};
            m->coordinateWarning = gpsWarning;
        }
        return error.isEmpty();
    }
    bool fail(const char *why) { error = QString::fromLatin1(why); return false; }
    QIODevice *input;
    qint64 size, consumed = 0;
    const GeoRefExif::Cancel &cancel;
    bool le = true;
    quint32 rootOffset = 0;
    int entryCount = 0;
    QMap<quint32, Directory> directories;
    QSet<quint32> active;
    QString error;
    QString gpsWarning;
};

struct Source {
    QFile file;
    QFileInfo original;
    bool jpeg = false;
    qint64 exifStart = -1, exifEnd = -1;
    QByteArray exif;
    QString error;
    bool open(const QString &path)
    {
        original = QFileInfo(path);
        if (!original.isFile() || original.isSymLink() || original.size() < 4 || original.size() > MaximumFile) {
            error = "Source must be a regular non-symlink JPEG/classic TIFF of 4 bytes to 4 GiB minus one byte."; return false;
        }
        file.setFileName(original.canonicalFilePath());
        if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
        const QByteArray h = file.read(4);
        jpeg = h.startsWith(QByteArray::fromHex("ffd8"));
        if (!jpeg && h.left(2) != "II" && h.left(2) != "MM") { error = "Not a JPEG or classic TIFF source."; return false; }
        return current();
    }
    bool current()
    {
        QFileInfo now(original.absoluteFilePath());
        if (!now.isFile() || now.isSymLink() || now.canonicalFilePath() != file.fileName()
                || now.size() != original.size() || file.size() != original.size()
                || now.lastModified() != original.lastModified() || now.birthTime() != original.birthTime()) {
            error = "Source changed while reading/writing EXIF."; return false;
        }
        return true;
    }
    bool jpegMetadata(const GeoRefExif::Cancel &cancel)
    {
        qint64 pos = 2, headerBytes = 0;
        int segments = 0;
        bool scan = false;
        while (pos < original.size()) {
            if (stopped(cancel)) { error = "Cancelled."; return false; }
            const bool fromScan = scan;
            if (scan) {
                // Entropy bytes, restart markers and FF00 stuffing are copied
                // verbatim. Locate the next structural marker in bounded reads.
                if (!file.seek(pos)) break;
                const QByteArray block = file.read(qMin<qint64>(Chunk, original.size() - pos));
                if (block.isEmpty()) break;
                int i = 0;
                for (; i < block.size(); ++i) {
                    if (quint8(block[i]) != 0xff) continue;
                    if (i + 1 == block.size()) break;
                    const quint8 next = quint8(block[i + 1]);
                    if (next == 0 || (next >= 0xd0 && next <= 0xd7)) { ++i; continue; }
                    scan = false; break;
                }
                pos += i;
                if (scan) {
                    if (!i && block.size() == 1) break;
                    continue;
                }
            }
            const qint64 markerStart = pos;
            if (!file.seek(pos)) break;
            char c = 0;
            if (!file.getChar(&c) || quint8(c) != 0xff) break;
            do { if (!file.getChar(&c)) { error = "Truncated JPEG marker."; return false; } ++pos; }
            while (quint8(c) == 0xff && pos - markerStart <= Chunk);
            ++pos;
            const quint8 marker = quint8(c);
            if (++segments > 65536) { error = "JPEG exceeds 65536 structural markers."; return false; }
            if (marker == 0xd9) return true;
            if (fromScan && marker >= 0xd0 && marker <= 0xd7) { scan = true; continue; }
            if (marker == 0 || marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7)) break;
            if (marker == 1) { scan = fromScan; continue; }
            const QByteArray sizeBytes = file.read(2);
            if (sizeBytes.size() != 2) break;
            const int length = u16(sizeBytes.constData(), false);
            if (length < 2 || length > original.size() - pos) break;
            headerBytes += length;
            if (headerBytes > MaximumMetadata) { error = "JPEG headers exceed the 16 MiB metadata bound."; return false; }
            if (marker == 0xe1 && length >= 8) {
                const QByteArray prefix = file.read(6);
                if (prefix == QByteArray("Exif\0\0", 6)) {
                    if (exifStart >= 0) { error = "Multiple JPEG Exif APP1 segments are ambiguous."; return false; }
                    exifStart = markerStart; exifEnd = pos + length;
                    exif = file.read(length - 8);
                    if (exif.size() != length - 8) break;
                }
            }
            pos += length;
            scan = marker == 0xda || (fromScan && marker == 0xdc);
        }
        error = "Malformed or truncated JPEG marker/scan stream.";
        return false;
    }
};

QByteArray entry(int tag, int type, quint32 count, const QByteArray &data, bool le)
{
    QByteArray b; put16(b, quint16(tag), le); put16(b, quint16(type), le); put32(b, count, le);
    b += data.left(4); while (b.size() < 12) b += '\0'; return b;
}
QByteArray longValue(quint32 n, bool le) { QByteArray b; put32(b, n, le); return b; }
void rationalValue(QByteArray &b, quint32 n, quint32 d, bool le) { put32(b, n, le); put32(b, d, le); }
QByteArray dms(double degrees, bool le)
{
    // 1e-7 second precision; carry rounded 60 seconds before encoding.
    const quint64 total = quint64(std::llround(std::abs(degrees) * 3600.0 * 10000000.0));
    const quint64 degree = total / 36000000000ULL;
    const quint64 rest = total % 36000000000ULL;
    QByteArray b;
    rationalValue(b, quint32(degree), 1, le);
    rationalValue(b, quint32(rest / 600000000ULL), 1, le);
    rationalValue(b, quint32(rest % 600000000ULL), 10000000, le);
    return b;
}

bool appendMetadata(Tiff &tiff, const GeoRefExif::Coordinates &coordinates,
                    QByteArray *extra, quint32 *root, QString *error)
{
    const bool le = tiff.le;
    if (tiff.size & 1) extra->append('\0');
    auto offset = [&]() { return quint32(tiff.size + extra->size()); };
    // Fixed new payload plus original root/GPS entries is bounded by graph caps.
    if (tiff.size > MaximumFile - 2 * 1024 * 1024) {
        *error = "Insufficient classic TIFF uint32 offset space for appended EXIF."; return false;
    }
    QMap<int, QByteArray> gps;
    Directory previous;
    const Directory originalRoot = tiff.directories.value(tiff.rootOffset);
    if (tiff.child(originalRoot, 0x8825, &previous))
        for (const Entry &e : previous.entries) if (e.tag > 6) gps.insert(e.tag, e.raw);
    if (!tiff.error.isEmpty()) { *error = tiff.error; return false; }
    gps.insert(0, entry(0, 1, 4, QByteArray::fromHex("02030000"), le));
    gps.insert(1, entry(1, 2, 2, QByteArray(coordinates.latitude < 0 ? "S\0" : "N\0", 2), le));
    gps.insert(2, entry(2, 5, 3, longValue(offset(), le), le)); *extra += dms(coordinates.latitude, le);
    gps.insert(3, entry(3, 2, 2, QByteArray(coordinates.longitude < 0 ? "W\0" : "E\0", 2), le));
    gps.insert(4, entry(4, 5, 3, longValue(offset(), le), le)); *extra += dms(coordinates.longitude, le);
    gps.insert(5, entry(5, 1, 1, QByteArray(1, coordinates.altitude < 0 ? char(1) : char(0)), le));
    gps.insert(6, entry(6, 5, 1, longValue(offset(), le), le));
    const double magnitude = std::abs(coordinates.altitude);
    // Adaptive decimal precision across the full unsigned rational domain.
    quint32 denominator = 1000000;
    while (denominator > 1 && magnitude * denominator > 4294967295.0) denominator /= 10;
    rationalValue(*extra, quint32(std::llround(magnitude * denominator)), denominator, le);
    if (gps.size() > 65535) { *error = "GPS IFD entry count exceeds uint16."; return false; }
    const quint32 gpsOffset = offset();
    put16(*extra, quint16(gps.size()), le);
    for (const QByteArray &e : gps) *extra += e;
    put32(*extra, previous.next, le);
    QMap<int, QByteArray> entries;
    for (const Entry &e : originalRoot.entries) if (e.tag != 0x8825) entries.insert(e.tag, e.raw);
    entries.insert(0x8825, entry(0x8825, 4, 1, longValue(gpsOffset, le), le));
    if (entries.size() > 65535) { *error = "Root IFD entry count exceeds uint16."; return false; }
    *root = offset(); put16(*extra, quint16(entries.size()), le);
    for (const QByteArray &e : entries) *extra += e;
    put32(*extra, originalRoot.next, le);
    return true;
}

bool prepare(Source &source, const GeoRefExif::Cancel &cancel, QByteArray *tiffBytes)
{
    if (!source.jpeg) return true;
    if (!source.jpegMetadata(cancel)) return false;
    *tiffBytes = source.exifStart < 0 ? QByteArray::fromHex("49492a0008000000000000000000") : source.exif;
    return true;
}
}

GeoRefExif::Metadata GeoRefExif::Inspect(const QString &path, const Cancel &cancel)
{
    Metadata result;
    Source source; QByteArray bytes;
    if (stopped(cancel)) { result.error = "Cancelled."; return result; }
    if (!source.open(path) || !prepare(source, cancel, &bytes)) { result.error = source.error; return result; }
    QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    Tiff tiff(source.jpeg ? static_cast<QIODevice *>(&buffer) : &source.file,
              source.jpeg ? bytes.size() : source.original.size(), cancel);
    result.format = source.jpeg ? "JPEG" : "TIFF";
    if (!tiff.parse() || !tiff.metadata(&result)) { result.error = tiff.error; return result; }
    if (stopped(cancel)) { result.error = "Cancelled."; return result; }
    if (!source.current()) { result.error = source.error; return result; }
    result.success = true;
    return result;
}

GeoRefExif::Result GeoRefExif::Write(const QString &path, QIODevice *destination,
                                  const Coordinates &coordinates, const Cancel &cancel,
                                  const Progress &progress)
{
    Result result;
    QPointer<QIODevice> output(destination);
    auto cancelled = [&]() {
        if (result.cancelled) return true;
        if (!stopped(cancel)) return false;
        result.cancelled = true; result.error = "Cancelled."; return true;
    };
    if (cancelled()) return result;
    if (!std::isfinite(coordinates.latitude) || !std::isfinite(coordinates.longitude)
            || !std::isfinite(coordinates.altitude) || std::abs(coordinates.latitude) > 90
            || std::abs(coordinates.longitude) > 180 || std::abs(coordinates.altitude) > 4294967295.0) {
        result.error = "GPS coordinates must be finite, latitude/longitude in range, and altitude representable as an EXIF uint32 rational."; return result;
    }
    if (!output || !output->isOpen() || !output->isWritable()
            || (!output->isSequential() && (output->pos() != 0 || output->size() != 0))) {
        result.error = "Destination must be an empty open writable private device positioned at zero."; return result;
    }
    Source source; QByteArray bytes;
    if (!source.open(path) || !prepare(source, cancel, &bytes)) {
        result.error = source.error; result.cancelled = result.error == QStringLiteral("Cancelled.");
        cancelled(); return result;
    }
    QPointer<QFileDevice> outputFile = qobject_cast<QFileDevice *>(output.data());
    const FileIdentity sourceIdentity = identity(&source.file);
    const FileIdentity outputIdentity = identity(outputFile);
    if (outputFile) {
        if (!sourceIdentity.valid || !outputIdentity.valid) {
            result.error = "Could not establish regular source/destination file identities."; return result;
        }
        if (sourceIdentity == outputIdentity
                || QFileInfo(outputFile->fileName()).canonicalFilePath() == source.file.fileName()) {
            result.error = "Source and destination must be different files."; return result;
        }
    }
    auto outputCurrent = [&]() {
        if (!output || !output->isOpen() || !output->isWritable()) {
            result.error = "Destination was closed or destroyed."; return false;
        }
        if (!output->isSequential() && (output->pos() != result.bytesWritten || output->size() != result.bytesWritten)) {
            result.error = "Destination position or size changed outside the EXIF writer."; return false;
        }
        if (outputFile && !(identity(outputFile) == outputIdentity)) {
            result.error = "Destination file identity changed outside the EXIF writer."; return false;
        }
        return true;
    };
    QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    Tiff tiff(source.jpeg ? static_cast<QIODevice *>(&buffer) : &source.file,
              source.jpeg ? bytes.size() : source.original.size(), cancel);
    QByteArray extra; quint32 root = 0;
    if (!tiff.parse() || !appendMetadata(tiff, coordinates, &extra, &root, &result.error)) {
        if (result.error.isEmpty()) result.error = tiff.error;
        result.cancelled = result.error == QStringLiteral("Cancelled.");
        cancelled(); return result;
    }
    QByteArray replacement;
    if (source.jpeg) {
        bytes.replace(4, 4, longValue(root, tiff.le)); bytes += extra;
        if (bytes.size() + 6 > 65533) { result.error = "Updated JPEG Exif APP1 exceeds 65533 payload bytes."; return result; }
        replacement = QByteArray::fromHex("ffe1");
        put16(replacement, quint16(bytes.size() + 8), false);
        replacement += QByteArray("Exif\0\0", 6); replacement += bytes;
    }
    const qint64 total = source.jpeg
            ? source.original.size() - (source.exifStart < 0 ? 0 : source.exifEnd - source.exifStart) + replacement.size()
            : source.original.size() + extra.size();
    if (progress) progress(0, total);
    if (cancelled()) return result;
    if (!outputCurrent()) return result;
    if (!source.current()) { result.error = source.error; return result; }
    auto write = [&](const QByteArray &data) {
        qint64 position = 0;
        while (position < data.size()) {
            if (cancelled()) return false;
            if (!outputCurrent()) return false;
            const qint64 count = output->write(data.constData() + position, qMin<qint64>(Chunk, data.size() - position));
            if (count <= 0) { result.error = output ? output->errorString() : "Destination was destroyed."; return false; }
            position += count; result.bytesWritten += count;
            if (!outputCurrent()) return false;
            if (progress) progress(result.bytesWritten, total);
            if (cancelled()) return false;
            if (!outputCurrent()) return false;
            if (!source.current()) { result.error = source.error; return false; }
        }
        return true;
    };
    auto copy = [&](qint64 start, qint64 end) {
        if (!source.file.seek(start)) { result.error = source.file.errorString(); return false; }
        while (start < end) {
            if (cancelled()) return false;
            const QByteArray block = source.file.read(qMin<qint64>(Chunk, end - start));
            if (block.isEmpty()) { result.error = "Source read failed or was truncated."; return false; }
            start += block.size();
            if (!write(block)) return false;
        }
        return true;
    };
    bool ok = false;
    if (source.jpeg) {
        const qint64 start = source.exifStart < 0 ? 2 : source.exifStart;
        const qint64 end = source.exifStart < 0 ? 2 : source.exifEnd;
        ok = copy(0, start) && write(replacement) && copy(end, source.original.size());
    } else {
        QByteArray header;
        if (source.file.seek(0)) header = source.file.read(8);
        if (header.size() != 8) result.error = "Could not read TIFF header.";
        else { header.replace(4, 4, longValue(root, tiff.le)); ok = write(header) && copy(8, source.original.size()) && write(extra); }
    }
    if (!ok || cancelled()) return result;
    if (!source.current()) { result.error = source.error; return result; }
    if (!outputCurrent()) return result;
    result.success = true;
    return result;
}
