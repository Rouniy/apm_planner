#include "GeoRefService.h"

#include "DataFlashRawReader.h"
#include "GeoRefExif.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QTemporaryFile>
#include <QXmlStreamWriter>
#include <QtEndian>

#include <array>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {

constexpr qint64 MaximumLogBytes = 1024LL * 1024 * 1024;
constexpr qint64 MaximumPhotoBytes = 4LL * 1024 * 1024 * 1024;
constexpr qint64 MaximumPhotoBytesTotal = 16LL * 1024 * 1024 * 1024;
constexpr int MaximumPhotos = 20000;
constexpr int MaximumDirectoryEntries = 200000;
constexpr int MaximumRelevantRecords = 2000000;
constexpr qint64 ReadChunkBytes = 64 * 1024;
constexpr qint64 TicksPerMillisecond = 10000;
constexpr int CurrentGpsUtcOffsetSeconds = 18;
constexpr int CameraGpsUtcOffsetSeconds = 17;
constexpr int InstanceClockRecordLimit = 2001;

struct FileSnapshot {
    QString requestedPath;
    QString canonicalPath;
    qint64 size = -1;
    QDateTime modifiedUtc;
    QDateTime born;
    QByteArray digest;
#ifdef Q_OS_UNIX
    quint64 device = 0;
    quint64 inode = 0;
#endif
};

struct DirectorySnapshot {
    QString path;
    QString canonicalPath;
    QString parentCanonical;
    QString name;
    bool existed = false;
#ifdef Q_OS_UNIX
    quint64 device = 0;
    quint64 inode = 0;
    quint64 parentDevice = 0;
    quint64 parentInode = 0;
#endif
};

struct PhotoInfo {
    FileSnapshot snapshot;
    QDateTime localTime;
    QDateTime utcTime;
    QString outputPath;
};

struct Location {
    qint64 keyMilliseconds = 0;
    qint64 unixTicks = 0;
    QDateTime timeUtc;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double relativeAltitude = 0.0;
    double gpsAltitude = 0.0;
    double roll = 0.0;
    double pitch = 0.0;
    double yaw = 0.0;
};

struct Clock {
    bool valid = false;
    qint64 anchorUnixTicks = 0;
    qint64 offsetMilliseconds = 0;
};

struct SchemaFacts {
    DataFlashRaw::Definition primaryGpsLayout;
    bool hasPrimaryGpsLayout = false;
    QSet<int> instanceTypes;
    QHash<int, Clock> instanceClocks;
    Clock lazyGpsClock;
    QByteArray digest;
    qint64 blankLines = 0;
    int trailingBytes = 0;
    int trailingType = -1;
};

struct ParsedLog {
    QMap<qint64, Location> gps;
    QMap<qint64, Location> cam;
    QMap<qint64, Location> trig;
    QByteArray digest;
    QStringList warnings;
};

bool pathExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool listPhotoFiles(const QString &directory, QVector<QFileInfo> *photos,
                    QString *error)
{
    photos->clear();
    QDirIterator iterator(directory,
                          QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden
                              | QDir::System,
                          QDirIterator::NoIteratorFlags);
    int scanned = 0;
    while (iterator.hasNext()) {
        iterator.next();
        if (++scanned > MaximumDirectoryEntries) {
            *error = QStringLiteral("The photo directory exceeds the 200,000-entry scan bound.");
            photos->clear();
            return false;
        }
        const QFileInfo entry = iterator.fileInfo();
        const QString extension = entry.suffix().toLower();
        if (extension != QLatin1String("jpg")
            && extension != QLatin1String("jpeg")
            && extension != QLatin1String("tif")
            && extension != QLatin1String("tiff")) {
            continue;
        }
        if (photos->size() >= MaximumPhotos) {
            *error = QStringLiteral("The photo directory exceeds the 20,000-photo bound.");
            photos->clear();
            return false;
        }
        photos->append(entry);
    }
    std::sort(photos->begin(), photos->end(),
              [](const QFileInfo &a, const QFileInfo &b) {
        return a.absoluteFilePath() < b.absoluteFilePath();
    });
    return true;
}

bool photoSetStillMatches(const QString &directory,
                          const QVector<PhotoInfo> &expected, QString *error)
{
    QVector<QFileInfo> current;
    if (!listPhotoFiles(directory, &current, error))
        return false;
    if (current.size() != expected.size()) {
        *error = QStringLiteral("The set of photos changed after it was snapshotted.");
        return false;
    }
    QSet<QString> paths;
    paths.reserve(expected.size());
    for (const PhotoInfo &photo : expected)
        paths.insert(photo.snapshot.canonicalPath.toCaseFolded());
    for (const QFileInfo &photo : current) {
        if (photo.isSymLink()
            || !paths.contains(photo.canonicalFilePath().toCaseFolded())) {
            *error = QStringLiteral("The set of photos changed after it was snapshotted.");
            return false;
        }
    }
    return true;
}

bool isCancelled(const GeoRefService::Cancel &cancel)
{
    return cancel && cancel();
}

void reportProgress(const GeoRefService::Progress &progress,
                    qint64 completed, qint64 total)
{
    if (progress) {
        progress(qBound<qint64>(0, completed, qMax<qint64>(1, total)),
                 qMax<qint64>(1, total));
    }
}

bool statRegular(const QString &path, quint64 *device, quint64 *inode)
{
#ifdef Q_OS_UNIX
    struct stat status {};
    const QByteArray encoded = QFile::encodeName(path);
    if (::lstat(encoded.constData(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return false;
    }
    if (device)
        *device = quint64(status.st_dev);
    if (inode)
        *inode = quint64(status.st_ino);
#else
    Q_UNUSED(device)
    Q_UNUSED(inode)
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink())
        return false;
#endif
    return true;
}

bool statDirectory(const QString &path, quint64 *device, quint64 *inode)
{
#ifdef Q_OS_UNIX
    struct stat status {};
    const QByteArray encoded = QFile::encodeName(path);
    if (::lstat(encoded.constData(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        return false;
    }
    if (device)
        *device = quint64(status.st_dev);
    if (inode)
        *inode = quint64(status.st_ino);
#else
    Q_UNUSED(device)
    Q_UNUSED(inode)
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink())
        return false;
#endif
    return true;
}

bool captureDirectory(const QString &requested, DirectorySnapshot *snapshot,
                      QString *error)
{
    const QFileInfo info(requested);
    DirectorySnapshot out;
    out.path = QDir::cleanPath(info.absoluteFilePath());
    out.name = QFileInfo(out.path).fileName();
    if (info.isSymLink()) {
        *error = QStringLiteral("The GeoRef output directory cannot be a symbolic link.");
        return false;
    }
    if (info.exists()) {
        if (!info.isDir() || !info.isWritable()) {
            *error = QStringLiteral("The GeoRef output path must be a writable directory.");
            return false;
        }
        out.canonicalPath = info.canonicalFilePath();
        out.path = out.canonicalPath;
        out.existed = true;
        if (out.canonicalPath.isEmpty()
            || !statDirectory(out.canonicalPath, &out.device, &out.inode)) {
            *error = QStringLiteral("Could not freeze the GeoRef output directory identity.");
            return false;
        }
        out.parentCanonical = QFileInfo(out.canonicalPath).dir().canonicalPath();
    } else {
        const QFileInfo parent(info.absolutePath());
        out.parentCanonical = parent.canonicalFilePath();
        if (out.name.isEmpty() || out.parentCanonical.isEmpty() || parent.isSymLink()
            || !parent.isDir() || !parent.isWritable()
            || !statDirectory(out.parentCanonical, &out.parentDevice,
                              &out.parentInode)) {
            *error = QStringLiteral(
                "The parent of the new GeoRef output directory must be a writable regular directory.");
            return false;
        }
        out.path = QDir(out.parentCanonical).filePath(out.name);
    }
    *snapshot = out;
    return true;
}

bool directoryMatches(const DirectorySnapshot &snapshot)
{
    if (snapshot.existed) {
        const QFileInfo current(snapshot.path);
        if (current.isSymLink() || !current.isDir()
            || current.canonicalFilePath() != snapshot.canonicalPath) {
            return false;
        }
#ifdef Q_OS_UNIX
        quint64 device = 0, inode = 0;
        return statDirectory(snapshot.path, &device, &inode)
            && device == snapshot.device && inode == snapshot.inode;
#else
        return true;
#endif
    }
    if (pathExists(snapshot.path))
        return false;
    const QFileInfo parent(snapshot.parentCanonical);
    if (parent.isSymLink() || !parent.isDir()
        || parent.canonicalFilePath() != snapshot.parentCanonical) {
        return false;
    }
#ifdef Q_OS_UNIX
    quint64 device = 0, inode = 0;
    return statDirectory(snapshot.parentCanonical, &device, &inode)
        && device == snapshot.parentDevice && inode == snapshot.parentInode;
#else
    return true;
#endif
}

bool hashFile(const QString &path, qint64 expectedSize,
              const GeoRefService::Cancel &cancel,
              QByteArray *digest, QString *error,
              const GeoRefService::Progress &progress = {},
              qint64 base = 0, qint64 total = 1)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not open %1: %2")
                     .arg(path, file.errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 read = 0;
    while (read < expectedSize) {
        if (isCancelled(cancel))
            return false;
        const QByteArray bytes = file.read(qMin(ReadChunkBytes, expectedSize - read));
        if (bytes.isEmpty()) {
            *error = QStringLiteral("A GeoRef input changed or failed while it was read.");
            return false;
        }
        hash.addData(bytes);
        read += bytes.size();
        reportProgress(progress, base + read, total);
    }
    if (file.size() != expectedSize || file.pos() != expectedSize) {
        *error = QStringLiteral("A GeoRef input changed while it was read.");
        return false;
    }
    *digest = hash.result();
    return true;
}

bool captureFile(const QString &requested, qint64 maximumBytes,
                 const GeoRefService::Cancel &cancel, FileSnapshot *snapshot,
                 QString *error, const GeoRefService::Progress &progress = {},
                 qint64 base = 0, qint64 total = 1)
{
    const QFileInfo before(requested);
    if (before.isSymLink() || !before.isFile() || !before.isReadable()) {
        *error = QStringLiteral("GeoRef inputs must be readable regular, non-symlink files.");
        return false;
    }
    if (before.size() < 0 || before.size() > maximumBytes) {
        *error = QStringLiteral("A GeoRef input exceeds its bounded size limit.");
        return false;
    }
    FileSnapshot out;
    out.requestedPath = before.absoluteFilePath();
    out.canonicalPath = before.canonicalFilePath();
    out.size = before.size();
    out.modifiedUtc = before.lastModified().toUTC();
    out.born = before.birthTime().toUTC();
    if (out.canonicalPath.isEmpty()
        || !statRegular(out.canonicalPath, &out.device, &out.inode)) {
        *error = QStringLiteral("Could not freeze a GeoRef input file identity.");
        return false;
    }
    if (!hashFile(out.canonicalPath, out.size, cancel, &out.digest, error,
                  progress, base, total)) {
        return false;
    }
    const QFileInfo after(out.requestedPath);
    if (after.isSymLink() || !after.isFile()
        || after.canonicalFilePath() != out.canonicalPath
        || after.size() != out.size
        || after.lastModified().toUTC() != out.modifiedUtc) {
        *error = QStringLiteral("A GeoRef input changed while its snapshot was captured.");
        return false;
    }
#ifdef Q_OS_UNIX
    quint64 device = 0, inode = 0;
    if (!statRegular(out.canonicalPath, &device, &inode)
        || device != out.device || inode != out.inode) {
        *error = QStringLiteral("A GeoRef input identity changed while it was read.");
        return false;
    }
#endif
    *snapshot = out;
    return true;
}

bool fileMatches(const FileSnapshot &snapshot,
                 const GeoRefService::Cancel &cancel, QString *error)
{
    const QFileInfo current(snapshot.requestedPath);
    if (current.isSymLink() || !current.isFile()
        || current.canonicalFilePath() != snapshot.canonicalPath
        || current.size() != snapshot.size
        || current.lastModified().toUTC() != snapshot.modifiedUtc) {
        *error = QStringLiteral("A snapshotted GeoRef input changed.");
        return false;
    }
#ifdef Q_OS_UNIX
    quint64 device = 0, inode = 0;
    if (!statRegular(snapshot.canonicalPath, &device, &inode)
        || device != snapshot.device || inode != snapshot.inode) {
        *error = QStringLiteral("A snapshotted GeoRef input was replaced.");
        return false;
    }
#endif
    QByteArray digest;
    if (!hashFile(snapshot.canonicalPath, snapshot.size, cancel, &digest, error))
        return false;
    if (digest != snapshot.digest) {
        *error = QStringLiteral("A snapshotted GeoRef input content changed.");
        return false;
    }
    return true;
}

int fieldIndex(const DataFlashRaw::Definition &definition,
               const QList<QByteArray> &names)
{
    for (const QByteArray &name : names) {
        const int index = definition.columns.indexOf(name);
        if (index >= 0)
            return index;
    }
    return -1;
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
            output = sign | (quint32(113 - shift) << 23) | (fraction << 13);
        }
    } else if (exponent == 0x1fu) {
        output = sign | 0x7f800000u | (fraction << 13);
    } else {
        output = sign | ((exponent + 112) << 23) | (fraction << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &output, sizeof(value));
    return value;
}

double binaryScalar(const QByteArray &raw,
                    const DataFlashRaw::Definition &definition, int index)
{
    if (index < 0 || index >= definition.format.size())
        return std::numeric_limits<double>::quiet_NaN();
    const int offset = definition.offsets.at(index);
    const int width = DataFlashRaw::width(definition.format.at(index));
    if (offset < 0 || width <= 0 || offset + width > raw.size())
        return std::numeric_limits<double>::quiet_NaN();
    const auto *at = reinterpret_cast<const uchar *>(raw.constData() + offset);
    switch (definition.format.at(index)) {
    case 'b': return qint8(*at);
    case 'B': case 'M': return *at;
    case 'h': return qFromLittleEndian<qint16>(at);
    case 'H': return qFromLittleEndian<quint16>(at);
    case 'i': return qFromLittleEndian<qint32>(at);
    case 'I': return qFromLittleEndian<quint32>(at);
    case 'q': return static_cast<double>(qFromLittleEndian<qint64>(at));
    case 'Q': return static_cast<double>(qFromLittleEndian<quint64>(at));
    case 'c': return qFromLittleEndian<qint16>(at) / 100.0;
    case 'C': return qFromLittleEndian<quint16>(at) / 100.0;
    case 'e': return qFromLittleEndian<qint32>(at) / 100.0;
    case 'E': return qFromLittleEndian<quint32>(at) / 100.0;
    case 'L': return qFromLittleEndian<qint32>(at) / 10000000.0;
    case 'f': {
        const quint32 bits = qFromLittleEndian<quint32>(at);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    case 'd': {
        const quint64 bits = qFromLittleEndian<quint64>(at);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    case 'g': return halfToFloat(qFromLittleEndian<quint16>(at));
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}

bool fieldNumber(const QByteArray &raw,
                 const DataFlashRaw::Definition &definition, bool text,
                 const QList<QByteArray> &names, double *value)
{
    const int index = fieldIndex(definition, names);
    if (index < 0)
        return false;
    if (!text) {
        *value = binaryScalar(raw, definition, index);
        return !std::isnan(*value);
    }
    const QList<QByteArray> values = DataFlashRaw::textValues(raw, definition);
    if (index >= values.size())
        return false;
    bool ok = false;
    const double parsed = values.at(index).trimmed().toDouble(&ok);
    if (!ok)
        return false;
    *value = parsed;
    return true;
}

bool binaryIntegral(const QByteArray &raw,
                    const DataFlashRaw::Definition &definition, int index,
                    qint64 *value)
{
    const double number = binaryScalar(raw, definition, index);
    if (!std::isfinite(number) || number != std::trunc(number)
        || number < static_cast<double>(std::numeric_limits<qint64>::min())
        || number >= -static_cast<double>(std::numeric_limits<qint64>::min())) {
        return false;
    }
    *value = static_cast<qint64>(number);
    return true;
}

bool fieldIntegralAt(const QByteArray &raw,
                     const DataFlashRaw::Definition &definition, bool text,
                     int index, qint64 *value)
{
    if (index < 0 || index >= definition.columns.size())
        return false;
    if (!text)
        return binaryIntegral(raw, definition, index, value);
    const QList<QByteArray> values = DataFlashRaw::textValues(raw, definition);
    if (index >= values.size())
        return false;
    bool ok = false;
    const qint64 parsed = values.at(index).trimmed().toLongLong(&ok, 10);
    if (!ok)
        return false;
    *value = parsed;
    return true;
}

bool fieldIntegral(const QByteArray &raw,
                   const DataFlashRaw::Definition &definition, bool text,
                   const QList<QByteArray> &names, qint64 *value)
{
    return fieldIntegralAt(raw, definition, text, fieldIndex(definition, names),
                           value);
}

bool clockCandidate(const QByteArray &raw,
                    const DataFlashRaw::Definition &definition, bool text,
                    const DataFlashRaw::Definition &primaryGpsLayout,
                    Clock *clock)
{
    qint64 status = 0;
    const int statusIndex = primaryGpsLayout.columns.indexOf("Status");
    if (statusIndex >= 0
        && fieldIntegralAt(raw, definition, text, statusIndex, &status)
        && status < 3) {
        return false;
    }
    int weekIndex = primaryGpsLayout.columns.indexOf("Week");
    if (weekIndex < 0)
        weekIndex = primaryGpsLayout.columns.indexOf("GWk");
    int millisecondsIndex = primaryGpsLayout.columns.indexOf("TimeMS");
    if (millisecondsIndex < 0)
        millisecondsIndex = primaryGpsLayout.columns.indexOf("GMS");
    qint64 week = 0, milliseconds = 0;
    if (!fieldIntegralAt(raw, definition, text, weekIndex, &week)
        || !fieldIntegralAt(raw, definition, text, millisecondsIndex,
                            &milliseconds)
        || week < 0 || week > 5000 || milliseconds < 0
        || milliseconds > 7LL * 24 * 60 * 60 * 1000) {
        return false;
    }
    qint64 offset = 0, value = 0;
    if (definition.columns.contains("T")) {
        if (!fieldIntegral(raw, definition, text, {"T"}, &value)
            || value < std::numeric_limits<int>::min()
            || value > std::numeric_limits<int>::max()) {
            return false;
        }
        offset = value;
    }
    if (definition.columns.contains("TimeUS")) {
        if (!fieldIntegral(raw, definition, text, {"TimeUS"}, &value))
            return false;
        offset = value / 1000;
    }
    const QDateTime epoch(QDate(1980, 1, 6), QTime(0, 0), Qt::UTC);
    const QDateTime anchor = epoch.addDays(week * 7).addMSecs(milliseconds)
                                 .addSecs(-CurrentGpsUtcOffsetSeconds);
    if (!anchor.isValid())
        return false;
    clock->valid = true;
    clock->anchorUnixTicks = anchor.toMSecsSinceEpoch() * TicksPerMillisecond;
    clock->offsetMilliseconds = offset;
    return true;
}

bool messageMilliseconds(const QByteArray &raw,
                         const DataFlashRaw::Definition &definition,
                         bool text, double *milliseconds)
{
    qint64 value = 0;
    if (definition.columns.contains("TimeMS")) {
        if (!fieldIntegral(raw, definition, text, {"TimeMS"}, &value))
            return false;
        *milliseconds = double(value);
    } else if (definition.columns.contains("TimeUS")) {
        if (!fieldIntegral(raw, definition, text, {"TimeUS"}, &value))
            return false;
        *milliseconds = double(value) / 1000.0;
    } else if (definition.columns.contains("T")) {
        if (!fieldIntegral(raw, definition, text, {"T"}, &value))
            return false;
        *milliseconds = double(value);
    } else {
        *milliseconds = 0.0;
    }
    return true;
}

qint64 floorDivide(qint64 numerator, qint64 denominator)
{
    qint64 result = numerator / denominator;
    if (numerator % denominator < 0)
        --result;
    return result;
}

qint64 millisecondsKey(qint64 unixTicks)
{
    const qint64 floor = floorDivide(unixTicks, TicksPerMillisecond);
    const qint64 remainder = unixTicks - floor * TicksPerMillisecond;
    if (remainder < TicksPerMillisecond / 2)
        return floor;
    if (remainder > TicksPerMillisecond / 2)
        return floor + 1;
    return (floor & 1) == 0 ? floor : floor + 1;
}

bool shiftedMillisecondsKey(const QDateTime &utc, double seconds,
                            qint64 *key)
{
    const double deltaTicks = seconds * 1000.0 * TicksPerMillisecond;
    if (!utc.isValid() || !std::isfinite(deltaTicks)
        || deltaTicks < double(std::numeric_limits<qint64>::min())
        || deltaTicks >= -double(std::numeric_limits<qint64>::min())) {
        return false;
    }
    const qint64 base = utc.toMSecsSinceEpoch() * TicksPerMillisecond;
    const qint64 delta = static_cast<qint64>(std::round(deltaTicks));
    if ((delta > 0 && base > std::numeric_limits<qint64>::max() - delta)
        || (delta < 0 && base < std::numeric_limits<qint64>::min() - delta)) {
        return false;
    }
    *key = millisecondsKey(base + delta);
    return true;
}

bool timestampFromMessage(const Clock &clock, double messageMs,
                          qint64 *unixTicks, QDateTime *utc)
{
    if (!std::isfinite(messageMs))
        return false;
    const double relative =
        (messageMs - double(clock.offsetMilliseconds)) * TicksPerMillisecond;
    if (!std::isfinite(relative)
        || relative < double(std::numeric_limits<qint64>::min())
        || relative >= -double(std::numeric_limits<qint64>::min())) {
        return false;
    }
    const qint64 relativeTicks = static_cast<qint64>(relative);
    qint64 ticks = relativeTicks;
    if (clock.valid) {
        if ((relativeTicks > 0
             && clock.anchorUnixTicks > std::numeric_limits<qint64>::max()
                                            - relativeTicks)
            || (relativeTicks < 0
                && clock.anchorUnixTicks < std::numeric_limits<qint64>::min()
                                             - relativeTicks)) {
            return false;
        }
        ticks += clock.anchorUnixTicks;
        *utc = QDateTime::fromMSecsSinceEpoch(
            floorDivide(ticks, TicksPerMillisecond), Qt::UTC);
    } else {
        const QDateTime minimum(QDate(1, 1, 1), QTime(0, 0), Qt::LocalTime);
        *utc = minimum.addMSecs(floorDivide(relativeTicks, TicksPerMillisecond))
                   .toUTC();
        ticks = utc->toMSecsSinceEpoch() * TicksPerMillisecond
            + (relativeTicks % TicksPerMillisecond);
    }
    *unixTicks = ticks;
    return utc->isValid();
}

bool directGpsTimestamp(qint64 week, qint64 milliseconds,
                        qint64 *unixTicks, QDateTime *utc)
{
    if (week < 0 || week > 5000 || milliseconds < 0
        || milliseconds > 7LL * 24 * 60 * 60 * 1000) {
        return false;
    }
    const QDateTime epoch(QDate(1980, 1, 6), QTime(0, 0), Qt::UTC);
    *utc = epoch.addDays(week * 7).addMSecs(milliseconds)
               .addSecs(-CameraGpsUtcOffsetSeconds);
    if (!utc->isValid())
        return false;
    *unixTicks = utc->toMSecsSinceEpoch() * TicksPerMillisecond;
    return true;
}

template<typename Handler>
bool rawPass(const FileSnapshot &source, bool text,
             const GeoRefService::Cancel &cancel,
             QString *error, QByteArray *digest, Handler handler)
{
    QFile file(source.canonicalPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not open the DataFlash log: %1")
                     .arg(file.errorString());
        return false;
    }
    DataFlashRaw::Reader reader(&file, text);
    QByteArray raw;
    int kind = -1;
    while (file.pos() < source.size) {
        if (isCancelled(cancel))
            return false;
        if (!reader.next(&raw, &kind)) {
            *error = reader.error.isEmpty()
                ? QStringLiteral("The DataFlash log ended before its snapshotted size.")
                : reader.error;
            return false;
        }
        if (file.pos() > source.size) {
            *error = QStringLiteral("The DataFlash log grew while it was parsed.");
            return false;
        }
        if (!handler(raw, kind, reader))
            return false;
    }
    if (file.pos() != source.size || file.size() != source.size) {
        *error = QStringLiteral("The DataFlash log changed while it was parsed.");
        return false;
    }
    if (digest)
        *digest = reader.hash.result();
    return true;
}

Clock clockFor(const SchemaFacts &facts,
               const DataFlashRaw::Definition &definition)
{
    const Clock instance = facts.instanceClocks.value(definition.id);
    return instance.valid ? instance : facts.lazyGpsClock;
}

bool parseLog(const FileSnapshot &source, bool useGps2,
              bool needGps, bool needCam, bool needTrig,
              const GeoRefService::Cancel &cancel,
              ParsedLog *parsed, QString *error)
{
    const bool text = QFileInfo(source.canonicalPath).suffix().compare(
                          QLatin1String("log"), Qt::CaseInsensitive) == 0;
    SchemaFacts facts;
    QByteArray schemaDigest;
    if (!rawPass(source, text, cancel, error, &schemaDigest,
                 [&facts, text](const QByteArray &raw, int kind,
                                DataFlashRaw::Reader &reader) {
        if (kind == 0 && reader.currentDefinition.name == "GPS") {
            facts.primaryGpsLayout = reader.currentDefinition;
            facts.hasPrimaryGpsLayout = true;
        }
        if (kind == 1) {
            qint64 type = -1;
            const int unitsIndex = reader.currentDefinition.columns.indexOf("UnitIds");
            if (fieldIntegral(raw, reader.currentDefinition, text,
                              {"FmtType"}, &type)
                && type >= 0 && type <= 255 && unitsIndex >= 0) {
                QByteArray unitIds;
                if (text) {
                    const QList<QByteArray> values =
                        DataFlashRaw::textValues(raw, reader.currentDefinition);
                    if (unitsIndex < values.size())
                        unitIds = values.at(unitsIndex).trimmed();
                } else {
                    const int offset =
                        reader.currentDefinition.offsets.at(unitsIndex);
                    unitIds = DataFlashRaw::ascii(raw.mid(
                        offset, DataFlashRaw::width(
                                    reader.currentDefinition.format.at(unitsIndex))));
                }
                if (unitIds.contains('#'))
                    facts.instanceTypes.insert(int(type));
            }
        }
        facts.blankLines = reader.blankLines;
        facts.trailingBytes = reader.trailingBytes;
        facts.trailingType = reader.trailingType;
        return true;
    })) {
        return false;
    }
    QHash<int, int> seen;
    QByteArray clockDigest;
    if (!rawPass(source, text, cancel, error, &clockDigest,
                 [&facts, &seen, text](const QByteArray &raw, int kind,
                                      DataFlashRaw::Reader &reader) {
        if (kind != -1 || !facts.hasPrimaryGpsLayout
            || !reader.currentDefinition.name.startsWith("GPS")) {
            return true;
        }
        if (reader.currentDefinition.name == "GPS" && !facts.lazyGpsClock.valid) {
            Clock candidate;
            if (clockCandidate(raw, reader.currentDefinition, text,
                               facts.primaryGpsLayout, &candidate)) {
                facts.lazyGpsClock = candidate;
            }
        }
        if (!facts.instanceTypes.contains(reader.currentDefinition.id))
            return true;
        int &count = seen[reader.currentDefinition.id];
        if (count++ >= InstanceClockRecordLimit
            || facts.instanceClocks.value(reader.currentDefinition.id).valid) {
            return true;
        }
        Clock candidate;
        if (clockCandidate(raw, reader.currentDefinition, text,
                           facts.primaryGpsLayout, &candidate)) {
            facts.instanceClocks.insert(reader.currentDefinition.id, candidate);
        }
        return true;
    })) {
        return false;
    }
    if (schemaDigest != source.digest || clockDigest != source.digest) {
        *error = QStringLiteral("The DataFlash log changed between parsing passes.");
        return false;
    }

    const QByteArray gpsName = useGps2 ? QByteArrayLiteral("GPS2")
                                       : QByteArrayLiteral("GPS");
    double roll = 0.0, pitch = 0.0, yaw = 0.0;
    int relevant = 0;
    QByteArray dataDigest;
    if (!rawPass(source, text, cancel, error, &dataDigest,
                 [&](const QByteArray &raw, int kind,
                     DataFlashRaw::Reader &reader) {
        if (kind != -1)
            return true;
        const auto &definition = reader.currentDefinition;
        const QByteArray name = definition.name;
        if (name == "ATT" && needGps) {
            double value = 0.0;
            if (fieldNumber(raw, definition, text, {"Roll"}, &value)
                && std::isfinite(value)) roll = static_cast<float>(value);
            if (fieldNumber(raw, definition, text, {"Pitch"}, &value)
                && std::isfinite(value)) pitch = static_cast<float>(value);
            if (fieldNumber(raw, definition, text, {"Yaw"}, &value)
                && std::isfinite(value)) yaw = static_cast<float>(value);
            return true;
        }
        const bool selectedGps = needGps && name == gpsName;
        const bool selectedCam = needCam && name == "CAM";
        const bool selectedTrig = needTrig && name == "TRIG";
        if (!selectedGps && !selectedCam && !selectedTrig)
            return true;
        if (++relevant > MaximumRelevantRecords) {
            *error = QStringLiteral("The DataFlash log contains too many GeoRef records.");
            return false;
        }

        Location point;
        qint64 ticks = 0;
        bool direct = false;
        qint64 week = 0, gpsMilliseconds = 0;
        if ((selectedCam || selectedTrig)
            && fieldIntegral(raw, definition, text, {"GPSWeek"}, &week)
            && fieldIntegral(raw, definition, text, {"GPSTime"}, &gpsMilliseconds)) {
            direct = directGpsTimestamp(week, gpsMilliseconds, &ticks,
                                        &point.timeUtc);
        }
        if (!direct) {
            double milliseconds = 0.0;
            if (!messageMilliseconds(raw, definition, text, &milliseconds)
                || !timestampFromMessage(clockFor(facts, definition), milliseconds,
                                         &ticks, &point.timeUtc)) {
                return true;
            }
        }
        point.keyMilliseconds = millisecondsKey(ticks);
        point.unixTicks = ticks;
        if (!fieldNumber(raw, definition, text, {"Lat"}, &point.latitude)
            || !fieldNumber(raw, definition, text, {"Lng"}, &point.longitude)) {
            if (selectedGps || selectedTrig)
                return true;
            point.latitude = 0.0;
            point.longitude = 0.0;
        }
        if (!std::isfinite(point.latitude) || !std::isfinite(point.longitude)
            || point.latitude < -90.0 || point.latitude > 90.0
            || point.longitude < -180.0 || point.longitude > 180.0
            || ((selectedGps || selectedTrig)
                && point.latitude == 0.0 && point.longitude == 0.0)) {
            return true;
        }
        if (selectedGps) {
            qint64 status = 3;
            if (definition.columns.contains("Status")
                && fieldIntegral(raw, definition, text, {"Status"}, &status)
                && status < 3) {
                return true;
            }
            double value = 0.0;
            if (fieldNumber(raw, definition, text, {"Alt"}, &value)
                && std::isfinite(value)) {
                point.altitude = value;
                point.gpsAltitude = value;
            }
            if (fieldNumber(raw, definition, text, {"RAlt", "RelAlt"}, &value)
                && std::isfinite(value)) point.relativeAltitude = value;
            point.roll = roll;
            point.pitch = pitch;
            point.yaw = yaw;
            if (!parsed->gps.contains(point.keyMilliseconds))
                parsed->gps.insert(point.keyMilliseconds, point);
            return true;
        }
        double value = 0.0;
        if (fieldNumber(raw, definition, text, {"Alt"}, &value)
            && std::isfinite(value)) point.altitude = value;
        if (fieldNumber(raw, definition, text, {"RelAlt"}, &value)
            && std::isfinite(value)) point.relativeAltitude = value;
        if (fieldNumber(raw, definition, text, {"GPSAlt"}, &value)
            && std::isfinite(value)) point.gpsAltitude = value;
        if (fieldNumber(raw, definition, text, {"Roll", "R"}, &value)
            && std::isfinite(value)) point.roll = static_cast<float>(value);
        if (fieldNumber(raw, definition, text, {"Pitch", "P"}, &value)
            && std::isfinite(value)) point.pitch = static_cast<float>(value);
        if (fieldNumber(raw, definition, text, {"Yaw", "Y"}, &value)
            && std::isfinite(value)) point.yaw = static_cast<float>(value);
        if (selectedCam)
            parsed->cam.insert(point.keyMilliseconds, point); // Last wins.
        else
            parsed->trig.insert(point.keyMilliseconds, point); // Last wins.
        return true;
    })) {
        return false;
    }
    if (dataDigest != source.digest) {
        *error = QStringLiteral("The DataFlash log changed during GeoRef parsing.");
        return false;
    }
    parsed->digest = dataDigest;
    if (facts.blankLines > 0) {
        parsed->warnings.append(QStringLiteral("Ignored %1 blank DataFlash text line(s).")
                                    .arg(facts.blankLines));
    }
    if (facts.trailingBytes > 0) {
        parsed->warnings.append(QStringLiteral(
            "Dropped %1 trailing byte(s) from incomplete record type %2.")
                                    .arg(facts.trailingBytes)
                                    .arg(facts.trailingType));
    }
    if (needGps && parsed->gps.isEmpty()) {
        parsed->warnings.append(QStringLiteral(
            "No usable %1 positions were found in the DataFlash log.")
                                    .arg(QString::fromLatin1(gpsName)));
    }
    return true;
}

const Location *nearest(const QMap<qint64, Location> &locations,
                        qint64 key, qint64 window)
{
    if (locations.isEmpty())
        return nullptr;
    auto upper = locations.lowerBound(key);
    if (upper != locations.end() && upper.key() == key)
        return &upper.value();
    const Location *later = upper == locations.end() ? nullptr : &upper.value();
    const qint64 laterDistance = upper == locations.end()
        ? std::numeric_limits<qint64>::max() : upper.key() - key;
    const Location *earlier = nullptr;
    qint64 earlierDistance = std::numeric_limits<qint64>::max();
    if (upper != locations.begin()) {
        --upper;
        earlier = &upper.value();
        earlierDistance = key - upper.key();
    }
    const qint64 best = qMin(laterDistance, earlierDistance);
    if (best > window)
        return nullptr;
    // Mission Planner probes +i before -i, so a later point wins a tie.
    return laterDistance <= earlierDistance ? later : earlier;
}

GeoRefService::Match makeMatch(const PhotoInfo &photo, const Location &location)
{
    GeoRefService::Match match;
    match.sourcePath = photo.snapshot.canonicalPath;
    match.outputPath = photo.outputPath;
    match.timeUtc = location.timeUtc;
    match.latitude = location.latitude;
    match.longitude = location.longitude;
    match.altitude = location.altitude;
    match.roll = location.roll;
    match.pitch = location.pitch;
    match.yaw = location.yaw;
    return match;
}

QString photoOutputName(const QString &source)
{
    const QString name = QFileInfo(source).fileName();
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    return (dot > 0 ? name.left(dot) : name) + QStringLiteral("_geotag")
        + (dot > 0 ? name.mid(dot) : QString());
}

QByteArray dotNetNumber(double value)
{
    if (std::isnan(value)) return QByteArrayLiteral("NaN");
    if (std::isinf(value)) return value < 0 ? QByteArrayLiteral("-Infinity")
                                            : QByteArrayLiteral("Infinity");
    if (value == 0.0) return std::signbit(value) ? QByteArrayLiteral("-0")
                                                 : QByteArrayLiteral("0");
    const auto normalizeExponent = [](const QByteArray &input) {
        int marker = input.indexOf('e');
        if (marker < 0) marker = input.indexOf('E');
        if (marker < 0) return input;
        int digit = marker + 1;
        char sign = '+';
        if (digit < input.size()
            && (input.at(digit) == '+' || input.at(digit) == '-')) {
            sign = input.at(digit++);
        }
        return input.left(marker) + 'E' + sign
            + input.mid(digit).rightJustified(2, '0');
    };
    const auto roundTrips = [value](const QString &text) {
        const QByteArray bytes = text.toLatin1();
        double parsed = 0.0;
        const auto converted = std::from_chars(
            bytes.constData(), bytes.constData() + bytes.size(), parsed,
            std::chars_format::general);
        return converted.ec == std::errc()
            && converted.ptr == bytes.constData() + bytes.size()
            && parsed == value;
    };
    for (int precision = 1; precision <= 17; ++precision) {
        const QString general = QString::number(value, 'g', precision);
        if (!roundTrips(general)) continue;
        const int marker = general.indexOf(QLatin1Char('e'));
        if (marker < 0) return general.toLatin1();
        const int exponent = general.mid(marker + 1).toInt();
        if (exponent >= 0 && exponent < 17) {
            const QString fixed = QString::number(
                value, 'f', qMax(0, precision - 1 - exponent));
            if (roundTrips(fixed)) return fixed.toLatin1();
        }
        return normalizeExponent(general.toLatin1());
    }
    return normalizeExponent(QString::number(value, 'g', 17).toLatin1());
}

struct StageIdentity {
    bool valid = false;
#ifdef Q_OS_UNIX
    quint64 device = 0;
    quint64 inode = 0;
#else
    QDateTime born;
    QString canonical;
#endif

    static StageIdentity capture(QFileDevice *file, const QString &path)
    {
        StageIdentity out;
#ifdef Q_OS_UNIX
        struct stat byDescriptor {}, byPath {};
        const QByteArray encoded = QFile::encodeName(path);
        if (file && file->handle() >= 0
            && ::fstat(file->handle(), &byDescriptor) == 0
            && ::lstat(encoded.constData(), &byPath) == 0
            && S_ISREG(byDescriptor.st_mode) && S_ISREG(byPath.st_mode)
            && byDescriptor.st_dev == byPath.st_dev
            && byDescriptor.st_ino == byPath.st_ino) {
            out.valid = true;
            out.device = quint64(byDescriptor.st_dev);
            out.inode = quint64(byDescriptor.st_ino);
        }
#else
        const QFileInfo info(path);
        if (info.isFile() && !info.isSymLink() && info.birthTime().isValid()) {
            out.valid = true;
            out.born = info.birthTime();
            out.canonical = info.canonicalFilePath();
        }
#endif
        return out;
    }

    bool matches(const QString &path) const
    {
        if (!valid)
            return false;
#ifdef Q_OS_UNIX
        quint64 currentDevice = 0, currentInode = 0;
        return statRegular(path, &currentDevice, &currentInode)
            && currentDevice == device && currentInode == inode;
#else
        const QFileInfo info(path);
        return info.isFile() && !info.isSymLink()
            && info.birthTime() == born && info.canonicalFilePath() == canonical;
#endif
    }
};

struct OwnedStage {
    QString path;
    StageIdentity identity;
    ~OwnedStage()
    {
        if (identity.matches(path))
            QFile::remove(path);
    }
};

bool publishNoReplace(QTemporaryFile *file, const StageIdentity &identity,
                      const QString &destination, QString *error)
{
    const QString staging = file->fileName();
    if (!identity.matches(staging)) {
        *error = QStringLiteral("A private GeoRef staging file changed before publication.");
        return false;
    }
#ifdef Q_OS_LINUX
    struct stat status {};
    if (file->handle() < 0 || ::fstat(file->handle(), &status) != 0
        || quint64(status.st_dev) != identity.device
        || quint64(status.st_ino) != identity.inode) {
        *error = QStringLiteral("A private GeoRef staging handle was retired.");
        return false;
    }
    const QByteArray descriptor = QByteArrayLiteral("/proc/self/fd/")
        + QByteArray::number(file->handle());
    const QByteArray output = QFile::encodeName(destination);
    if (::linkat(AT_FDCWD, descriptor.constData(), AT_FDCWD,
                 output.constData(), AT_SYMLINK_FOLLOW) != 0) {
        *error = QStringLiteral("Could not publish a GeoRef output without overwriting a file.");
        return false;
    }
#elif defined(Q_OS_UNIX)
    const QByteArray stagingName = QFile::encodeName(staging);
    const QByteArray output = QFile::encodeName(destination);
    if (::link(stagingName.constData(), output.constData()) != 0) {
        *error = QStringLiteral("Could not publish a GeoRef output without overwriting a file.");
        return false;
    }
#elif defined(Q_OS_WIN)
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(staging.utf16()),
                     reinterpret_cast<LPCWSTR>(destination.utf16()),
                     MOVEFILE_WRITE_THROUGH)) {
        *error = QStringLiteral("Could not publish a GeoRef output without overwriting a file.");
        return false;
    }
#else
    Q_UNUSED(destination)
    *error = QStringLiteral("Atomic no-overwrite GeoRef publication is unavailable.");
    return false;
#endif
    return true;
}

bool writeLocationText(QIODevice *device,
                       const QVector<GeoRefService::Match> &matches,
                       QString *error)
{
    QByteArray line = QByteArrayLiteral(
        "#name latitude/Y longitude/X height/Z yaw pitch roll SAlt\n");
    if (device->write(line) != line.size()) {
        *error = QStringLiteral("Could not write location.txt.");
        return false;
    }
    for (const auto &match : matches) {
        line = QFileInfo(match.sourcePath).fileName().toUtf8() + ' '
            + dotNetNumber(match.latitude) + ' ' + dotNetNumber(match.longitude) + ' '
            + dotNetNumber(match.altitude) + ' ' + dotNetNumber(match.yaw) + ' '
            + dotNetNumber(match.pitch) + ' ' + dotNetNumber(match.roll)
            + QByteArrayLiteral(" 0\n");
        if (device->write(line) != line.size()) {
            *error = QStringLiteral("Could not write location.txt.");
            return false;
        }
    }
    return true;
}

bool writeLocationKml(QIODevice *device,
                      const QVector<GeoRefService::Match> &matches,
                      QString *error)
{
    QXmlStreamWriter xml(device);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("kml"));
    xml.writeDefaultNamespace(QStringLiteral("http://www.opengis.net/kml/2.2"));
    xml.writeStartElement(QStringLiteral("Document"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("GeoRef"));
    QByteArray coordinates;
    coordinates.reserve(qMin<int>(matches.size() * 64, 4 * 1024 * 1024));
    for (const auto &match : matches) {
        xml.writeStartElement(QStringLiteral("Placemark"));
        QString base = QFileInfo(match.sourcePath).fileName();
        const int dot = base.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) base.truncate(dot);
        xml.writeTextElement(QStringLiteral("name"), base);
        xml.writeStartElement(QStringLiteral("TimeStamp"));
        xml.writeTextElement(QStringLiteral("when"), match.timeUtc.toUTC().toString(
            QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")));
        xml.writeEndElement();
        xml.writeStartElement(QStringLiteral("Point"));
        xml.writeTextElement(QStringLiteral("altitudeMode"), QStringLiteral("absolute"));
        const QByteArray coordinate = dotNetNumber(match.longitude) + ','
            + dotNetNumber(match.latitude) + ',' + dotNetNumber(match.altitude);
        xml.writeTextElement(QStringLiteral("coordinates"),
                             QString::fromLatin1(coordinate));
        xml.writeEndElement();
        xml.writeEndElement();
        if (!coordinates.isEmpty()) coordinates += ' ';
        coordinates += coordinate;
        if (coordinates.size() > 8 * 1024 * 1024) {
            *error = QStringLiteral("The GeoRef KML coordinate list exceeds its bound.");
            return false;
        }
    }
    xml.writeStartElement(QStringLiteral("Placemark"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("path"));
    xml.writeStartElement(QStringLiteral("LineString"));
    xml.writeTextElement(QStringLiteral("altitudeMode"), QStringLiteral("absolute"));
    xml.writeTextElement(QStringLiteral("coordinates"), QString::fromLatin1(coordinates));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    if (xml.hasError()) {
        *error = QStringLiteral("Could not write location.kml.");
        return false;
    }
    return true;
}

} // namespace

struct GeoRefService::Plan::Data {
    Options options;
    FileSnapshot log;
    QVector<PhotoInfo> photos;
    QVector<Match> matches;
    QStringList outputPaths;
    QStringList warnings;
    DirectorySnapshot outputDirectory;
};

GeoRefService::Plan::Plan() = default;

GeoRefService::Plan::Plan(std::shared_ptr<const Data> data)
    : d(std::move(data))
{
}

bool GeoRefService::Plan::isValid() const { return bool(d); }
GeoRefService::Options GeoRefService::Plan::options() const
{ return d ? d->options : Options{}; }
QVector<GeoRefService::Match> GeoRefService::Plan::matches() const
{ return d ? d->matches : QVector<Match>{}; }
QStringList GeoRefService::Plan::outputPaths() const
{ return d ? d->outputPaths : QStringList{}; }
QStringList GeoRefService::Plan::warnings() const
{ return d ? d->warnings : QStringList{}; }

GeoRefService::PlanResult GeoRefService::Prepare(
    const Options &submitted, const Cancel &cancel, const Progress &progress)
{
    PlanResult result;
    try {
        const Options options = submitted;
        if (!std::isfinite(options.timeOffsetSeconds)
            || !std::isfinite(options.baseAltitudeAdjustmentMeters)
            || options.shutterLagMilliseconds == std::numeric_limits<int>::min()) {
            result.error = QStringLiteral("GeoRef numeric options must be finite and bounded.");
            return result;
        }
        const QFileInfo logInfo(options.logPath);
        const QString suffix = logInfo.suffix().toLower();
        if (suffix == QLatin1String("tlog")) {
            result.error = QStringLiteral(
                "GeoRef .tlog input is unavailable because Mission Planner's advertised path is non-functional; select a DataFlash .bin or .log file.");
            return result;
        }
        if (suffix != QLatin1String("bin") && suffix != QLatin1String("log")) {
            result.error = QStringLiteral("Select a DataFlash .bin or .log file.");
            return result;
        }
        const QFileInfo photoDirectoryInfo(options.photoDirectory);
        const QString photoDirectory = photoDirectoryInfo.canonicalFilePath();
        if (photoDirectoryInfo.isSymLink() || photoDirectory.isEmpty()
            || !photoDirectoryInfo.isDir() || !photoDirectoryInfo.isReadable()) {
            result.error = QStringLiteral("Select a readable, non-symlink photo directory.");
            return result;
        }

        auto data = std::make_shared<Plan::Data>();
        data->options = options;
        if (!captureFile(options.logPath, MaximumLogBytes, cancel, &data->log,
                         &result.error)) {
            result.cancelled = isCancelled(cancel);
            return result;
        }
        data->options.logPath = data->log.canonicalPath;
        data->options.photoDirectory = photoDirectory;

        const QString requestedOutput = options.outputDirectory.isEmpty()
            ? QDir(photoDirectory).filePath(QStringLiteral("geotagged"))
            : QFileInfo(options.outputDirectory).absoluteFilePath();
        if (!captureDirectory(requestedOutput, &data->outputDirectory,
                              &result.error)) {
            return result;
        }
        data->options.outputDirectory = data->outputDirectory.path;

        QVector<QFileInfo> entries;
        if (!listPhotoFiles(photoDirectory, &entries, &result.error))
            return result;
        QSet<QString> seenCanonical;
        QSet<QString> seenOutputs;
        qint64 totalPhotoBytes = 0;
        for (const QFileInfo &entry : entries) {
            if (entry.isSymLink() || !entry.isFile()) {
                result.error = QStringLiteral("A selected photo is not a regular non-symlink file.");
                return result;
            }
            const QString canonical = entry.canonicalFilePath();
            const QString key = canonical.toCaseFolded();
            if (canonical.isEmpty() || seenCanonical.contains(key))
                continue;
            seenCanonical.insert(key);
            if (entry.size() < 0 || entry.size() > MaximumPhotoBytes
                || totalPhotoBytes > MaximumPhotoBytesTotal - entry.size()) {
                result.error = QStringLiteral("The selected photos exceed the bounded input size.");
                return result;
            }
            totalPhotoBytes += entry.size();
            PhotoInfo photo;
            if (!captureFile(canonical, MaximumPhotoBytes, cancel,
                             &photo.snapshot, &result.error)) {
                result.cancelled = isCancelled(cancel);
                return result;
            }
            const GeoRefExif::Metadata metadata = GeoRefExif::Inspect(
                photo.snapshot.canonicalPath, cancel);
            if (isCancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            if (!metadata.success) {
                data->warnings.append(QStringLiteral("%1: %2")
                    .arg(entry.fileName(), metadata.error));
            } else if (metadata.photoTime.isValid()) {
                photo.localTime = metadata.photoTime;
                photo.utcTime = metadata.photoTime.toUTC();
            }
            if (!metadata.coordinateWarning.isEmpty())
                data->warnings.append(QStringLiteral("%1: %2")
                    .arg(entry.fileName(), metadata.coordinateWarning));
            if (!fileMatches(photo.snapshot, cancel, &result.error)) {
                result.cancelled = isCancelled(cancel);
                return result;
            }
            photo.outputPath = QDir(data->outputDirectory.path).filePath(
                photoOutputName(photo.snapshot.canonicalPath));
            const QString outputKey = QFileInfo(photo.outputPath).fileName().toCaseFolded();
            if (seenOutputs.contains(outputKey)) {
                result.error = QStringLiteral(
                    "Photo names collide after adding the _geotag suffix.");
                return result;
            }
            seenOutputs.insert(outputKey);
            data->photos.append(photo);
        }
        if (data->photos.isEmpty()) {
            result.error = QStringLiteral("The photo directory contains no supported images.");
            return result;
        }
        std::sort(data->photos.begin(), data->photos.end(),
                  [](const PhotoInfo &a, const PhotoInfo &b) {
            if (a.utcTime.isValid() != b.utcTime.isValid())
                return !a.utcTime.isValid(); // DateTime.MinValue sorts first.
            if (a.utcTime != b.utcTime)
                return a.utcTime < b.utcTime;
            return a.snapshot.canonicalPath < b.snapshot.canonicalPath;
        });

        const bool needGps = options.mode == Mode::TimeOffset
            || options.shutterLagMilliseconds != 0
            || options.useAmslAltitude || options.useGpsAltitude;
        ParsedLog parsed;
        if (!parseLog(data->log, options.useGps2, needGps,
                      options.mode == Mode::Cam,
                      options.mode == Mode::Trig,
                      cancel, &parsed, &result.error)) {
            result.cancelled = isCancelled(cancel);
            return result;
        }
        data->warnings += parsed.warnings;

        QVector<Match> matches;
        matches.reserve(data->photos.size());
        if (options.mode == Mode::Cam) {
            if (data->photos.size() != parsed.cam.size()) {
                data->warnings.append(QStringLiteral(
                    "CAM/photo count mismatch: %1 photo(s), %2 CAM record(s); matched the shorter ordered prefix.")
                                          .arg(data->photos.size())
                                          .arg(parsed.cam.size()));
            }
            auto camera = parsed.cam.cbegin();
            for (int i = 0; i < data->photos.size() && camera != parsed.cam.cend();
                 ++i, ++camera) {
                matches.append(makeMatch(data->photos.at(i), camera.value()));
            }
        } else if (options.mode == Mode::Trig) {
            if (data->photos.size() != parsed.trig.size()) {
                result.error = QStringLiteral(
                    "TRIG/photo counts differ; order-only TRIG matching cannot safely guess missing captures.");
                return result;
            }
            auto trigger = parsed.trig.cbegin();
            for (int i = 0; i < data->photos.size(); ++i, ++trigger)
                matches.append(makeMatch(data->photos.at(i), trigger.value()));
        } else {
            for (const PhotoInfo &photo : data->photos) {
                if (!photo.utcTime.isValid()) {
                    data->warnings.append(QStringLiteral("%1 has no EXIF capture time and was skipped.")
                                              .arg(QFileInfo(photo.snapshot.canonicalPath).fileName()));
                    continue;
                }
                qint64 corrected = 0;
                if (!shiftedMillisecondsKey(photo.utcTime,
                                            -options.timeOffsetSeconds,
                                            &corrected)) {
                    result.error = QStringLiteral("The photo time offset exceeds the supported DateTime range.");
                    return result;
                }
                const Location *location = nearest(parsed.gps, corrected, 5000);
                if (!location) {
                    data->warnings.append(QStringLiteral("%1 has no selected GPS fix within 5 seconds.")
                                              .arg(QFileInfo(photo.snapshot.canonicalPath).fileName()));
                    continue;
                }
                matches.append(makeMatch(photo, *location));
            }
        }

        if ((options.mode == Mode::Cam || options.mode == Mode::Trig)
            && (options.shutterLagMilliseconds != 0
                || options.useAmslAltitude || options.useGpsAltitude)) {
            const qint64 window = qMax<qint64>(
                5000, qAbs(qint64(options.shutterLagMilliseconds)) + 2000);
            for (Match &match : matches) {
                const qint64 target = match.timeUtc.toMSecsSinceEpoch()
                    + options.shutterLagMilliseconds;
                const Location *location = nearest(parsed.gps, target, window);
                if (!location) {
                    data->warnings.append(QStringLiteral("%1 has no GPS correction in the shutter window.")
                                              .arg(QFileInfo(match.sourcePath).fileName()));
                    continue;
                }
                if (options.shutterLagMilliseconds != 0) {
                    match.timeUtc = location->timeUtc;
                    match.latitude = location->latitude;
                    match.longitude = location->longitude;
                    match.roll = location->roll;
                    match.pitch = location->pitch;
                    match.yaw = location->yaw;
                }
                if (options.useGpsAltitude && location->gpsAltitude != 0.0)
                    match.altitude = location->gpsAltitude;
                else if (options.useAmslAltitude)
                    match.altitude = location->altitude;
            }
        }
        for (Match &match : matches) {
            if (options.baseAltitudeAdjustmentMeters != 0.0
                && std::isfinite(match.altitude)) {
                match.altitude += options.baseAltitudeAdjustmentMeters;
            }
            if (!match.timeUtc.isValid() || !std::isfinite(match.latitude)
                || !std::isfinite(match.longitude) || !std::isfinite(match.altitude)
                || !std::isfinite(match.roll) || !std::isfinite(match.pitch)
                || !std::isfinite(match.yaw)
                || match.latitude < -90.0 || match.latitude > 90.0
                || match.longitude < -180.0 || match.longitude > 180.0) {
                result.error = QStringLiteral("A GeoRef match has invalid time, coordinates, altitude, or attitude.");
                return result;
            }
        }
        if (matches.isEmpty()) {
            result.error = QStringLiteral("No valid photo/log matches were found.");
            return result;
        }
        data->matches = matches;
        data->outputPaths.reserve(matches.size() + 2);
        data->outputPaths.append(QDir(data->outputDirectory.path).filePath("location.txt"));
        data->outputPaths.append(QDir(data->outputDirectory.path).filePath("location.kml"));
        for (const Match &match : matches)
            data->outputPaths.append(match.outputPath);
        for (const QString &output : data->outputPaths) {
            if (pathExists(output)) {
                result.error = QStringLiteral("GeoRef output already exists and will not be overwritten: %1")
                                   .arg(output);
                return result;
            }
        }
        if (!directoryMatches(data->outputDirectory)) {
            result.error = QStringLiteral("The GeoRef output directory changed during preparation.");
            return result;
        }
        if (isCancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        reportProgress(progress, 1, 1);
        if (isCancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        if (!fileMatches(data->log, {}, &result.error)
            || !directoryMatches(data->outputDirectory)) {
            if (!result.cancelled && result.error.isEmpty())
                result.error = QStringLiteral("GeoRef inputs changed after the final preparation callback.");
            return result;
        }
        for (const PhotoInfo &photo : data->photos) {
            if (!fileMatches(photo.snapshot, {}, &result.error)) {
                return result;
            }
        }
        if (!photoSetStillMatches(data->options.photoDirectory, data->photos,
                                  &result.error)) {
            return result;
        }
        for (const QString &output : data->outputPaths) {
            if (pathExists(output)) {
                result.error = QStringLiteral("A GeoRef destination appeared during preparation.");
                return result;
            }
        }
        result.warnings = data->warnings;
        result.plan = std::shared_ptr<const Plan>(new Plan(data));
        result.success = true;
    } catch (const std::exception &exception) {
        result.error = QStringLiteral("GeoRef preparation failed: %1")
                           .arg(QString::fromUtf8(exception.what()));
    } catch (...) {
        result.error = QStringLiteral("Unexpected GeoRef preparation failure.");
    }
    return result;
}

GeoRefService::EstimateResult GeoRefService::Estimate(
    const Options &options, const Cancel &cancel, const Progress &progress)
{
    EstimateResult result;
    try {
        const QFileInfo suffixInfo(options.logPath);
        const QString suffix = suffixInfo.suffix().toLower();
        if (suffix == QLatin1String("tlog")) {
            result.error = QStringLiteral("GeoRef offset estimation does not support .tlog input.");
            return result;
        }
        if (suffix != QLatin1String("bin") && suffix != QLatin1String("log")) {
            result.error = QStringLiteral("Select a DataFlash .bin or .log file.");
            return result;
        }
        FileSnapshot log;
        if (!captureFile(options.logPath, MaximumLogBytes, cancel, &log,
                         &result.error)) {
            result.cancelled = isCancelled(cancel);
            return result;
        }
        const QFileInfo directoryInfo(options.photoDirectory);
        const QString directory = directoryInfo.canonicalFilePath();
        if (directoryInfo.isSymLink() || directory.isEmpty() || !directoryInfo.isDir()) {
            result.error = QStringLiteral("Select a readable photo directory.");
            return result;
        }
        QVector<QDateTime> photoTimes;
        QVector<FileSnapshot> photoSnapshots;
        qint64 photoBytes = 0;
        QVector<QFileInfo> files;
        if (!listPhotoFiles(directory, &files, &result.error))
            return result;
        for (const QFileInfo &file : files) {
            if (file.size() < 0 || file.size() > MaximumPhotoBytes
                || photoBytes > MaximumPhotoBytesTotal - file.size()) {
                result.error = QStringLiteral("The selected photos exceed the bounded input size.");
                return result;
            }
            photoBytes += file.size();
            FileSnapshot snapshot;
            if (!captureFile(file.absoluteFilePath(), MaximumPhotoBytes, cancel,
                             &snapshot, &result.error)) {
                result.cancelled = isCancelled(cancel);
                return result;
            }
            const auto metadata = GeoRefExif::Inspect(snapshot.canonicalPath, cancel);
            if (isCancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            if (metadata.success && metadata.photoTime.isValid())
                photoTimes.append(metadata.photoTime.toUTC());
            else if (!metadata.success)
                result.warnings.append(QStringLiteral("%1: %2")
                    .arg(file.fileName(), metadata.error));
            if (!metadata.coordinateWarning.isEmpty())
                result.warnings.append(QStringLiteral("%1: %2")
                    .arg(file.fileName(), metadata.coordinateWarning));
            photoSnapshots.append(snapshot);
        }
        std::sort(photoTimes.begin(), photoTimes.end());
        ParsedLog parsed;
        const bool useCam = options.mode == Mode::Cam;
        if (!parseLog(log, options.useGps2, !useCam, useCam, false,
                      cancel, &parsed, &result.error)) {
            result.cancelled = isCancelled(cancel);
            return result;
        }
        result.warnings += parsed.warnings;
        QVector<qint64> logTicks;
        const auto &source = useCam ? parsed.cam : parsed.gps;
        logTicks.reserve(source.size());
        for (const Location &location : source)
            if (location.timeUtc.isValid()) logTicks.append(location.unixTicks);
        const int count = qMin(photoTimes.size(), logTicks.size());
        if (count == 0) {
            result.error = QStringLiteral(
                "Could not estimate an offset because valid photo or log timestamps are missing.");
            return result;
        }
        QSet<int> indices;
        for (int i = 0; i < qMin(4, count); ++i) indices.insert(i);
        for (int i = qMax(0, count - 3); i < count; ++i) indices.insert(i);
        QVector<int> ordered = indices.values().toVector();
        std::sort(ordered.begin(), ordered.end());
        QVector<double> offsets;
        offsets.reserve(ordered.size());
        for (int index : ordered) {
            const qint64 photoTicks = photoTimes.at(index).toMSecsSinceEpoch()
                * TicksPerMillisecond;
            offsets.append(double(photoTicks - logTicks.at(index)) / 10000000.0);
        }
        std::sort(offsets.begin(), offsets.end());
        const int middle = offsets.size() / 2;
        const double median = offsets.size() % 2
            ? offsets.at(middle)
            : (offsets.at(middle - 1) + offsets.at(middle)) / 2.0;
        result.offsetSeconds = std::nearbyint(median * 1000.0) / 1000.0;
        result.hasEstimate = true;
        reportProgress(progress, 1, 1);
        if (isCancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        if (!fileMatches(log, {}, &result.error)) {
            return result;
        }
        for (const FileSnapshot &photo : photoSnapshots) {
            if (!fileMatches(photo, {}, &result.error)) {
                return result;
            }
        }
        result.success = true;
    } catch (const std::exception &exception) {
        result.error = QStringLiteral("GeoRef offset estimation failed: %1")
                           .arg(QString::fromUtf8(exception.what()));
    } catch (...) {
        result.error = QStringLiteral("Unexpected GeoRef offset estimation failure.");
    }
    return result;
}

GeoRefService::Result GeoRefService::Execute(
    const Plan &submitted, const Cancel &cancel, const Progress &progress)
{
    Result result;
    try {
        const std::shared_ptr<const Plan::Data> data = submitted.d;
        if (!data) {
            result.error = QStringLiteral("The GeoRef plan is invalid.");
            return result;
        }
        result.matches = data->matches;
        result.warnings = data->warnings;
        result.publishedPaths.reserve(data->outputPaths.size());
        result.failedPaths.reserve(data->matches.size());
        if (isCancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        if (!directoryMatches(data->outputDirectory)) {
            result.error = QStringLiteral("The frozen GeoRef output directory changed.");
            return result;
        }
        for (const QString &output : data->outputPaths) {
            if (pathExists(output)) {
                result.error = QStringLiteral("A GeoRef output already exists; nothing was overwritten.");
                return result;
            }
        }
        if (!fileMatches(data->log, cancel, &result.error)) {
            result.cancelled = isCancelled(cancel);
            return result;
        }
        for (const PhotoInfo &photo : data->photos) {
            if (!fileMatches(photo.snapshot, cancel, &result.error)) {
                result.cancelled = isCancelled(cancel);
                return result;
            }
        }
        if (!photoSetStillMatches(data->options.photoDirectory, data->photos,
                                  &result.error)) {
            return result;
        }
        reportProgress(progress, 0, data->outputPaths.size());
        if (isCancelled(cancel)) {
            result.cancelled = true;
            return result;
        }

        if (!data->outputDirectory.existed) {
            if (!directoryMatches(data->outputDirectory)
                || !QDir(data->outputDirectory.parentCanonical)
                        .mkdir(data->outputDirectory.name)) {
                result.error = QStringLiteral("Could not create the new GeoRef output directory.");
                return result;
            }
        }
        const QFileInfo currentOutputDirectory(data->outputDirectory.path);
        const QString outputCanonical = currentOutputDirectory.canonicalFilePath();
        if (currentOutputDirectory.isSymLink() || !currentOutputDirectory.isDir()
            || outputCanonical.isEmpty()) {
            result.error = QStringLiteral("The GeoRef output directory could not be frozen after creation.");
            return result;
        }
#ifdef Q_OS_UNIX
        quint64 outputDevice = 0, outputInode = 0;
        if (!statDirectory(outputCanonical, &outputDevice, &outputInode)) {
            result.error = QStringLiteral("Could not freeze the active GeoRef output directory.");
            return result;
        }
#endif
        const auto outputDirectoryStillCurrent = [&]() {
            const QFileInfo current(data->outputDirectory.path);
            if (current.isSymLink() || !current.isDir()
                || current.canonicalFilePath() != outputCanonical) {
                return false;
            }
#ifdef Q_OS_UNIX
            quint64 device = 0, inode = 0;
            return statDirectory(outputCanonical, &device, &inode)
                && device == outputDevice && inode == outputInode;
#else
            return true;
#endif
        };

        auto verifyDerivedInputs = [&]() {
            if (!fileMatches(data->log, {}, &result.error))
                return false;
            for (const PhotoInfo &photo : data->photos) {
                if (!fileMatches(photo.snapshot, {}, &result.error))
                    return false;
            }
            return photoSetStillMatches(data->options.photoDirectory,
                                        data->photos, &result.error);
        };
        auto publishReport = [&](const QString &destination,
                                 const std::function<bool(QIODevice *, QString *)> &writer) {
            QTemporaryFile stage(QDir(outputCanonical).filePath(
                QStringLiteral(".georef-report-XXXXXX.part")));
            stage.setAutoRemove(false);
            if (!stage.open()) {
                result.error = QStringLiteral("Could not create a private GeoRef report stage.");
                return false;
            }
            const StageIdentity identity = StageIdentity::capture(&stage, stage.fileName());
            OwnedStage cleanup{stage.fileName(), identity};
            if (!identity.valid || !writer(&stage, &result.error) || !stage.flush()) {
                if (result.error.isEmpty())
                    result.error = QStringLiteral("Could not flush a GeoRef report.");
                return false;
            }
            reportProgress(progress, result.publishedPaths.size() + 1,
                           data->outputPaths.size());
            if (isCancelled(cancel)) {
                result.cancelled = true;
                return false;
            }
            // From this point to publication there are no external callbacks.
            if (!verifyDerivedInputs()) {
                if (result.error.isEmpty()) result.cancelled = true;
                return false;
            }
            if (!outputDirectoryStillCurrent()) {
                result.error = QStringLiteral("The GeoRef output directory changed before publication.");
                return false;
            }
            if (pathExists(destination)) {
                result.error = QStringLiteral("A GeoRef report destination appeared; it was not overwritten.");
                return false;
            }
            if (!publishNoReplace(&stage, identity, destination, &result.error))
                return false;
            result.publishedPaths.append(destination);
            return true;
        };

        if (!publishReport(data->outputPaths.at(0),
                           [&](QIODevice *device, QString *error) {
            return writeLocationText(device, data->matches, error);
        })) return result;
        if (!publishReport(data->outputPaths.at(1),
                           [&](QIODevice *device, QString *error) {
            return writeLocationKml(device, data->matches, error);
        })) return result;

        QHash<QString, FileSnapshot> photos;
        photos.reserve(data->photos.size());
        for (const PhotoInfo &photo : data->photos)
            photos.insert(photo.snapshot.canonicalPath, photo.snapshot);
        for (const Match &match : data->matches) {
            if (isCancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            QTemporaryFile stage(QDir(outputCanonical).filePath(
                QStringLiteral(".georef-photo-XXXXXX.part")));
            stage.setAutoRemove(false);
            if (!stage.open()) {
                result.failedPaths.append(match.outputPath);
                ++result.failedPhotos;
                result.warnings.append(QStringLiteral("Could not create a private stage for %1.")
                                           .arg(QFileInfo(match.sourcePath).fileName()));
                continue;
            }
            const StageIdentity identity = StageIdentity::capture(&stage, stage.fileName());
            OwnedStage cleanup{stage.fileName(), identity};
            const GeoRefExif::Coordinates coordinates{
                match.latitude, match.longitude, match.altitude};
            const GeoRefExif::Result exif = GeoRefExif::Write(
                match.sourcePath, &stage, coordinates, cancel);
            if (exif.cancelled || isCancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            if (!identity.valid || !exif.success || !stage.flush()) {
                if (!fileMatches(data->log, cancel, &result.error)
                    || !fileMatches(photos.value(match.sourcePath), cancel,
                                    &result.error)) {
                    if (result.error.isEmpty()) result.cancelled = true;
                    return result;
                }
                result.failedPaths.append(match.outputPath);
                ++result.failedPhotos;
                result.warnings.append(QStringLiteral("%1: %2")
                    .arg(QFileInfo(match.sourcePath).fileName(),
                         exif.error.isEmpty()
                             ? QStringLiteral("EXIF output could not be staged.")
                             : exif.error));
                continue;
            }
            reportProgress(progress, result.publishedPaths.size() + 1,
                           data->outputPaths.size());
            if (isCancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            const FileSnapshot photo = photos.value(match.sourcePath);
            // From this point to publication there are no external callbacks.
            if (!fileMatches(data->log, {}, &result.error)
                || !fileMatches(photo, {}, &result.error)
                || !outputDirectoryStillCurrent()) {
                if (result.error.isEmpty()) result.cancelled = true;
                return result;
            }
            if (pathExists(match.outputPath)
                || !publishNoReplace(&stage, identity, match.outputPath,
                                     &result.error)) {
                result.failedPaths.append(match.outputPath);
                ++result.failedPhotos;
                result.warnings.append(QStringLiteral("%1 was not published because its destination changed.")
                                           .arg(QFileInfo(match.outputPath).fileName()));
                result.error.clear();
                continue;
            }
            result.publishedPaths.append(match.outputPath);
            ++result.taggedPhotos;
        }
        result.success = result.failedPhotos == 0;
        if (!result.success && result.error.isEmpty()) {
            result.error = QStringLiteral("GeoRef reports were published, but one or more photos failed.");
        }
        // No external callback follows the last irreversible publication.
    } catch (const std::exception &exception) {
        result.error = QStringLiteral("GeoRef execution failed: %1")
                           .arg(QString::fromUtf8(exception.what()));
    } catch (...) {
        result.error = QStringLiteral("Unexpected GeoRef execution failure.");
    }
    return result;
}
