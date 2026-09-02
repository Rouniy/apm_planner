#include "MapCacheManager.h"

#include "point.h"
#include "pureimagecache.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QImage>
#include <QImageReader>
#include <QLockFile>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace {
const QString kMaintenanceLockName = QStringLiteral(".maintenance.lock");

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString absoluteCleanPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString canonicalOrAbsolutePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? absoluteCleanPath(path)
                               : QDir::cleanPath(canonical);
}

QString requestedCacheRoot(const QString &cacheRoot)
{
    return absoluteCleanPath(cacheRoot.isEmpty()
        ? core::PureImageCache::sharedCacheRoot() : cacheRoot);
}

#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
const QString::SplitBehavior kSkipEmptyParts = QString::SkipEmptyParts;
#else
const Qt::SplitBehavior kSkipEmptyParts = Qt::SkipEmptyParts;
#endif

bool pathsEqual(const QString &left, const QString &right)
{
    return QString::compare(QDir::cleanPath(left), QDir::cleanPath(right),
                            pathCaseSensitivity()) == 0;
}

bool isStrictChildPath(const QString &root, const QString &path)
{
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanPath = QDir::cleanPath(path);
    if (pathsEqual(cleanRoot, cleanPath)) {
        return false;
    }
    const QString prefix = cleanRoot.endsWith(QLatin1Char('/'))
        ? cleanRoot : cleanRoot + QLatin1Char('/');
    return cleanPath.startsWith(prefix, pathCaseSensitivity());
}

bool isInsideRoot(const QString &root, const QString &path, bool allowRoot)
{
    return (allowRoot && pathsEqual(root, path))
        || isStrictChildPath(root, path);
}

bool nameLess(const QFileInfo &left, const QFileInfo &right)
{
    const int folded = QString::compare(left.fileName(), right.fileName(),
                                        Qt::CaseInsensitive);
    if (folded != 0) {
        return folded < 0;
    }
    return left.fileName() < right.fileName();
}

bool isOwnedTile(const QFileInfo &fileInfo)
{
    return fileInfo.isFile() && !fileInfo.isSymLink()
        && fileInfo.suffix().compare(QStringLiteral("tile"),
                                     Qt::CaseInsensitive) == 0;
}

bool isCacheableMapType(core::MapType::Types type)
{
    return type == core::MapType::GoogleSatellite
        || type == core::MapType::GoogleHybrid
        || type == core::MapType::BingSatellite
        || type == core::MapType::OpenStreetMap
        || type == core::MapType::ArcGIS_Satellite;
}

QFileInfoList sortedEntries(const QString &directory)
{
    QFileInfoList entries = QDir(directory).entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System,
        QDir::NoSort);
    std::sort(entries.begin(), entries.end(), nameLess);
    return entries;
}

MapCacheSnapshot snapshotDirectory(const QString &name,
                                   const QString &directory,
                                   const QString &canonicalRoot)
{
    MapCacheSnapshot snapshot;
    snapshot.name = name;
    snapshot.path = directory;

    QStringList pending{directory};
    while (!pending.isEmpty()) {
        const QString current = pending.takeLast();
        const QFileInfo currentInfo(current);
        if (!currentInfo.isDir() || currentInfo.isSymLink()
            || !currentInfo.isReadable()) {
            continue;
        }
        const QString canonicalDirectory = currentInfo.canonicalFilePath();
        if (canonicalDirectory.isEmpty()
            || !isStrictChildPath(canonicalRoot, canonicalDirectory)) {
            continue;
        }

        const QFileInfoList entries = sortedEntries(canonicalDirectory);
        for (int index = entries.size() - 1; index >= 0; --index) {
            const QFileInfo &entry = entries.at(index);
            if (entry.isSymLink()) {
                continue;
            }
            const QString canonical = entry.canonicalFilePath();
            if (canonical.isEmpty()
                || !isStrictChildPath(canonicalRoot, canonical)) {
                continue;
            }
            if (entry.isDir()) {
                pending.append(canonical);
                continue;
            }
            if (!isOwnedTile(entry)) {
                continue;
            }
            snapshot.sizeBytes += entry.size();
            ++snapshot.fileCount;
            const QDateTime modified = entry.lastModified().toUTC();
            if (modified.isValid()
                && (!snapshot.lastWriteUtc.isValid()
                    || modified > snapshot.lastWriteUtc)) {
                snapshot.lastWriteUtc = modified;
            }
        }
    }
    return snapshot;
}

void removeEmptyDirectories(const QVector<QString> &directories,
                            const QString &keepDirectory,
                            bool keepDirectChildren)
{
    QVector<QString> ordered = directories;
    std::sort(ordered.begin(), ordered.end(), [](const QString &left,
                                                  const QString &right) {
        if (left.size() != right.size()) {
            return left.size() > right.size();
        }
        return left > right;
    });
    for (const QString &directory : ordered) {
        if (pathsEqual(directory, keepDirectory)) {
            continue;
        }
        if (keepDirectChildren
            && pathsEqual(QFileInfo(directory).absolutePath(),
                          keepDirectory)) {
            continue;
        }
        const QFileInfo info(directory);
        if (!info.isSymLink() && info.isDir()
            && QDir(directory).entryList(
                QDir::AllEntries | QDir::NoDotAndDotDot
                    | QDir::Hidden | QDir::System).isEmpty()) {
            QDir().rmdir(directory);
        }
    }
}

template<typename Result>
void reportLockFailure(QLockFile &lock, Result *result);

MapCacheDeleteResult deleteMatching(const MapCacheSnapshot &entry,
                                    const QDateTime &cutoffUtc,
                                    const QString &cacheRoot,
                                    bool deleteEverything)
{
    MapCacheDeleteResult result;
    const QString requestedRoot = requestedCacheRoot(cacheRoot);
    const QFileInfo requestedRootInfo(requestedRoot);
    if (requestedRootInfo.exists()
        && (!requestedRootInfo.isDir() || requestedRootInfo.isSymLink())) {
        result.error = QStringLiteral("The configured map cache root is not a safe directory.");
        return result;
    }
    const QString root = canonicalOrAbsolutePath(requestedRoot);

    const QString requestedTarget = absoluteCleanPath(entry.path);
    const QFileInfo requestedTargetInfo(requestedTarget);
    if (requestedTargetInfo.isSymLink()) {
        result.error = QStringLiteral("The map cache target is a symbolic link.");
        return result;
    }
    const QString target = canonicalOrAbsolutePath(requestedTarget);
    if (!isInsideRoot(root, target, entry.isTotal)
        || (entry.isTotal && !pathsEqual(root, target))) {
        result.error = QStringLiteral(
            "The map cache target is outside the configured cache root.");
        return result;
    }
    if (!entry.isTotal && pathsEqual(root, target)) {
        result.error = QStringLiteral(
            "Only a total-cache snapshot may target the cache root.");
        return result;
    }
    if (!entry.isTotal
        && !pathsEqual(QFileInfo(target).absolutePath(), root)) {
        result.error = QStringLiteral(
            "A provider cache target must be an immediate child of the cache root.");
        return result;
    }
    if (!requestedTargetInfo.exists()) {
        return result;
    }
    if (!requestedTargetInfo.isDir()) {
        result.error = QStringLiteral("The map cache target is not a directory.");
        return result;
    }
    if (!deleteEverything && !cutoffUtc.isValid()) {
        result.error = QStringLiteral("The map cache cutoff time is invalid.");
        return result;
    }

    const QString maintenanceLockPath =
        QDir(root).filePath(kMaintenanceLockName);
    if (QFileInfo(maintenanceLockPath).isSymLink()) {
        result.error = QStringLiteral(
            "The map cache maintenance lock is a symbolic link.");
        return result;
    }
    QLockFile maintenanceLock(maintenanceLockPath);
    if (!maintenanceLock.tryLock(0)) {
        reportLockFailure(maintenanceLock, &result);
        return result;
    }

    const QFileInfo lockedTargetInfo(requestedTarget);
    const QString lockedTarget = canonicalOrAbsolutePath(requestedTarget);
    if (!lockedTargetInfo.exists() || !lockedTargetInfo.isDir()
        || lockedTargetInfo.isSymLink()
        || !pathsEqual(lockedTarget, target)) {
        result.error = QStringLiteral(
            "The map cache target changed before maintenance started.");
        return result;
    }
    if (!entry.isTotal) {
        bool currentProvider = false;
        const QList<MapCacheSnapshot> currentEntries =
            MapCacheManager::scan(root);
        for (const MapCacheSnapshot &current : currentEntries) {
            if (!current.isTotal && pathsEqual(current.path, lockedTarget)) {
                currentProvider = true;
                break;
            }
        }
        if (!currentProvider) {
            result.error = QStringLiteral(
                "The selected provider cache is no longer available.");
            return result;
        }
    }

    QStringList pending{target};
    QVector<QString> visitedDirectories{target};
    while (!pending.isEmpty()) {
        const QString current = pending.takeLast();
        const QFileInfo currentInfo(current);
        if (!currentInfo.isDir() || currentInfo.isSymLink()
            || !currentInfo.isReadable()) {
            continue;
        }
        const QString canonicalDirectory = currentInfo.canonicalFilePath();
        if (canonicalDirectory.isEmpty()
            || !isInsideRoot(root, canonicalDirectory, entry.isTotal)) {
            continue;
        }

        const QFileInfoList entries = sortedEntries(canonicalDirectory);
        for (int index = entries.size() - 1; index >= 0; --index) {
            const QFileInfo &fileInfo = entries.at(index);
            if (fileInfo.isSymLink()) {
                continue;
            }
            const QString canonical = fileInfo.canonicalFilePath();
            if (canonical.isEmpty()
                || !isInsideRoot(root, canonical, false)) {
                continue;
            }
            if (fileInfo.isDir()) {
                pending.append(canonical);
                visitedDirectories.append(canonical);
                continue;
            }
            if (!isOwnedTile(fileInfo)) {
                continue;
            }
            if (!deleteEverything
                && fileInfo.lastModified().toUTC() >= cutoffUtc.toUTC()) {
                continue;
            }
            const qint64 size = fileInfo.size();
            if (QFile::remove(canonical)) {
                ++result.removedFiles;
                result.freedBytes += size;
            } else {
                ++result.failedFiles;
            }
        }
    }

    removeEmptyDirectories(visitedDirectories, target, entry.isTotal);
    return result;
}

bool decimalInteger(const QString &text, int *value)
{
    if (text.isEmpty()) {
        return false;
    }
    for (const QChar character : text) {
        if (character < QLatin1Char('0')
            || character > QLatin1Char('9')) {
            return false;
        }
    }
    bool ok = false;
    const int parsed = text.toInt(&ok, 10);
    if (ok && value) {
        *value = parsed;
    }
    return ok;
}

bool supportedImageExtension(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("jpg")
        || suffix == QStringLiteral("jpeg")
        || suffix == QStringLiteral("png");
}

bool decodableJpegOrPng(const QByteArray &bytes)
{
    const bool jpegSignature = bytes.size() >= 3
        && static_cast<unsigned char>(bytes.at(0)) == 0xff
        && static_cast<unsigned char>(bytes.at(1)) == 0xd8
        && static_cast<unsigned char>(bytes.at(2)) == 0xff;
    const QByteArray pngSignature = QByteArray::fromHex("89504e470d0a1a0a");
    if (!jpegSignature && !bytes.startsWith(pngSignature)) {
        return false;
    }

    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return false;
    }
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const QByteArray format = reader.format().toLower();
    if (format != QByteArrayLiteral("jpg")
        && format != QByteArrayLiteral("jpeg")
        && format != QByteArrayLiteral("png")) {
        return false;
    }
    const QSize decodedSize = reader.size();
    constexpr int maximumDimension = 4096;
    constexpr qint64 maximumPixels = 16LL * 1024LL * 1024LL;
    if (!decodedSize.isValid()
        || decodedSize.width() > maximumDimension
        || decodedSize.height() > maximumDimension
        || static_cast<qint64>(decodedSize.width())
                * decodedSize.height() > maximumPixels) {
        return false;
    }
    const QImage image = reader.read();
    return !image.isNull() && image.width() > 0 && image.height() > 0;
}

bool ensureSafeDestination(const QString &root, const QString &tilePath)
{
    const QString cleanTilePath = absoluteCleanPath(tilePath);
    if (!isStrictChildPath(root, cleanTilePath)) {
        return false;
    }
    const QString parentPath = QFileInfo(cleanTilePath).absolutePath();
    const QString relativeParent = QDir(root).relativeFilePath(parentPath);
    const QStringList components = QDir::fromNativeSeparators(relativeParent)
        .split(QLatin1Char('/'), kSkipEmptyParts);

    QString current = root;
    for (const QString &component : components) {
        if (component == QStringLiteral(".")
            || component == QStringLiteral("..")) {
            return false;
        }
        current = QDir(current).filePath(component);
        QFileInfo info(current);
        if (info.isSymLink()) {
            return false;
        }
        if (!info.exists()) {
            const QString parent = QFileInfo(current).absolutePath();
            if (!QDir(parent).mkdir(QFileInfo(current).fileName())) {
                return false;
            }
            info.setFile(current);
        }
        if (!info.isDir()) {
            return false;
        }
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || !isStrictChildPath(root, canonical)) {
            return false;
        }
        current = canonical;
    }

    const QFileInfo tileInfo(cleanTilePath);
    if (tileInfo.isSymLink()) {
        return false;
    }
    const QString canonicalTile = tileInfo.canonicalFilePath();
    return canonicalTile.isEmpty()
        || isStrictChildPath(root, canonicalTile);
}

quint64 tileKey(const MapTileIndex &index)
{
    return (static_cast<quint64>(index.zoom) << 42)
        | (static_cast<quint64>(index.column) << 21)
        | static_cast<quint64>(index.row);
}

void reportImport(const MapTileImporter::ProgressCallback &callback,
                  const MapTileImportResult &result,
                  const QString &currentFile)
{
    if (!callback) {
        return;
    }
    callback({result.discovered, result.imported, result.skipped,
              result.failed, currentFile});
}

void reportImportPeriodically(
    const MapTileImporter::ProgressCallback &callback,
    const MapTileImportResult &result, const QString &currentFile)
{
    if (result.discovered == 1 || result.discovered % 25 == 0) {
        reportImport(callback, result, currentFile);
    }
}

template<typename Result>
void reportLockFailure(QLockFile &lock, Result *result)
{
    if (lock.error() == QLockFile::LockFailedError) {
        result->maintenanceBusy = true;
        return;
    }
    result->error = QStringLiteral(
        "The map cache maintenance lock cannot be created or opened.");
}
} // namespace

QList<MapCacheSnapshot> MapCacheManager::scan(const QString &cacheRoot)
{
    const QString requestedRoot = requestedCacheRoot(cacheRoot);
    const QFileInfo requestedRootInfo(requestedRoot);
    const bool safeRoot = requestedRootInfo.isDir()
        && !requestedRootInfo.isSymLink();
    const QString root = safeRoot
        ? canonicalOrAbsolutePath(requestedRoot) : requestedRoot;
    QList<MapCacheSnapshot> snapshots;

    if (safeRoot) {
        QFileInfoList providers = QDir(root).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden
                | QDir::System,
            QDir::NoSort);
        std::sort(providers.begin(), providers.end(), nameLess);
        for (const QFileInfo &provider : providers) {
            if (provider.isSymLink()) {
                continue;
            }
            const QString canonical = provider.canonicalFilePath();
            if (canonical.isEmpty() || !isStrictChildPath(root, canonical)) {
                continue;
            }
            snapshots.append(snapshotDirectory(
                provider.fileName(), canonical, root));
        }
    }

    MapCacheSnapshot total;
    total.name = QStringLiteral("Total");
    total.path = root;
    total.isTotal = true;
    for (const MapCacheSnapshot &snapshot : snapshots) {
        total.sizeBytes += snapshot.sizeBytes;
        total.fileCount += snapshot.fileCount;
        if (snapshot.lastWriteUtc.isValid()
            && (!total.lastWriteUtc.isValid()
                || snapshot.lastWriteUtc > total.lastWriteUtc)) {
            total.lastWriteUtc = snapshot.lastWriteUtc;
        }
    }
    snapshots.append(total);
    return snapshots;
}

MapCacheDeleteResult MapCacheManager::deleteOlderThan(
    const MapCacheSnapshot &entry, const QDateTime &cutoffUtc,
    const QString &cacheRoot)
{
    return deleteMatching(entry, cutoffUtc, cacheRoot, false);
}

MapCacheDeleteResult MapCacheManager::deleteAll(
    const MapCacheSnapshot &entry, const QString &cacheRoot)
{
    return deleteMatching(entry, QDateTime(), cacheRoot, true);
}

QString MapCacheManager::formatBytes(qint64 bytes)
{
    static const char *const units[] = {
        "B", "KiB", "MiB", "GiB", "TiB"
    };
    double value = static_cast<double>(qMax<qint64>(0, bytes));
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QStringLiteral("%1 B").arg(static_cast<qulonglong>(value));
    }
    QString number = QString::number(value, 'f', 2);
    while (number.endsWith(QLatin1Char('0'))) {
        number.chop(1);
    }
    if (number.endsWith(QLatin1Char('.'))) {
        number.chop(1);
    }
    return QStringLiteral("%1 %2").arg(number, QString::fromLatin1(units[unit]));
}

bool MapTileImporter::tryParseOfficialPath(const QString &sourceRoot,
                                           const QString &filePath,
                                           MapTileIndex *index)
{
    if (!index || sourceRoot.trimmed().isEmpty()
        || filePath.trimmed().isEmpty()) {
        return false;
    }
    *index = MapTileIndex();

    const QFileInfo rootInfo(sourceRoot);
    if (rootInfo.isSymLink()) {
        return false;
    }
    const QString root = canonicalOrAbsolutePath(sourceRoot);
    const QFileInfo fileInfo(filePath);
    if (fileInfo.isSymLink()) {
        return false;
    }
    const QString file = canonicalOrAbsolutePath(filePath);
    if (!isStrictChildPath(root, file)) {
        return false;
    }

    const QString relative = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(file));
    const QStringList parts = relative.split(
        QLatin1Char('/'), kSkipEmptyParts);
    if (parts.size() < 3) {
        return false;
    }

    const QString zoomPart = parts.at(parts.size() - 3);
    int zoom = -1;
    int row = -1;
    int column = -1;
    if (zoomPart.size() < 2
        || (zoomPart.at(0) != QLatin1Char('Z')
            && zoomPart.at(0) != QLatin1Char('z'))
        || !decimalInteger(zoomPart.mid(1), &zoom)
        || !decimalInteger(parts.at(parts.size() - 2), &row)
        || !decimalInteger(
            QFileInfo(parts.constLast()).completeBaseName(), &column)
        || zoom < 0 || zoom > MaximumZoom) {
        return false;
    }

    const int width = 1 << zoom;
    if (row < 0 || column < 0 || row >= width || column >= width) {
        return false;
    }
    index->column = column;
    index->row = row;
    index->zoom = zoom;
    return true;
}

MapTileImportResult MapTileImporter::importTiles(
    const QString &sourceRoot, core::MapType::Types mapType,
    const QString &cacheRoot, const ProgressCallback &progress,
    const CancellationCallback &isCanceled)
{
    MapTileImportResult result;
    if (!isCacheableMapType(mapType)) {
        result.error = QStringLiteral(
            "The selected map provider does not use the shared tile cache.");
        return result;
    }
    const QFileInfo sourceInfo(sourceRoot);
    if (sourceRoot.trimmed().isEmpty() || !sourceInfo.isDir()
        || sourceInfo.isSymLink()) {
        result.error = QStringLiteral(
            "The tile source directory does not exist or is not safe.");
        return result;
    }
    const QString source = sourceInfo.canonicalFilePath();
    if (source.isEmpty()) {
        result.error = QStringLiteral(
            "The tile source directory cannot be resolved.");
        return result;
    }

    const QString requestedRoot = cacheRoot.isEmpty()
        ? core::PureImageCache::sharedCacheRoot() : cacheRoot;
    const QFileInfo requestedRootInfo(requestedRoot);
    if (requestedRootInfo.isSymLink()) {
        result.error = QStringLiteral(
            "The configured map cache root is a symbolic link.");
        return result;
    }
    if (!QDir().mkpath(absoluteCleanPath(requestedRoot))) {
        result.error = QStringLiteral("The map cache root cannot be created.");
        return result;
    }
    const QString root = canonicalOrAbsolutePath(requestedRoot);
    const QFileInfo rootInfo(root);
    if (!rootInfo.isDir() || rootInfo.isSymLink()) {
        result.error = QStringLiteral(
            "The configured map cache root is not a safe directory.");
        return result;
    }
    if (isInsideRoot(source, root, true)
        || isInsideRoot(root, source, true)) {
        result.error = QStringLiteral(
            "The tile source and map cache directories must not overlap.");
        return result;
    }

    const QString maintenanceLockPath =
        QDir(root).filePath(kMaintenanceLockName);
    if (QFileInfo(maintenanceLockPath).isSymLink()) {
        result.error = QStringLiteral(
            "The map cache maintenance lock is a symbolic link.");
        return result;
    }
    QLockFile maintenanceLock(maintenanceLockPath);
    if (!maintenanceLock.tryLock(0)) {
        reportLockFailure(maintenanceLock, &result);
        return result;
    }

    core::PureImageCache cache(root);
    QSet<quint64> importedIndexes;
    QStringList pending{source};
    while (!pending.isEmpty()) {
        if (isCanceled && isCanceled()) {
            result.canceled = true;
            break;
        }

        const QString currentPath = pending.takeLast();
        const QFileInfo currentInfo(currentPath);
        if (currentInfo.isSymLink()) {
            if (supportedImageExtension(currentInfo.fileName())) {
                ++result.discovered;
                ++result.skipped;
                reportImportPeriodically(progress, result, currentPath);
            }
            continue;
        }

        if (currentInfo.isDir()) {
            if (!currentInfo.isReadable()) {
                ++result.failed;
                reportImport(progress, result, currentPath);
                continue;
            }
            const QString canonicalDirectory =
                currentInfo.canonicalFilePath();
            if (canonicalDirectory.isEmpty()
                || !isInsideRoot(source, canonicalDirectory, true)) {
                ++result.failed;
                reportImport(progress, result, currentPath);
                continue;
            }

            const QFileInfoList entries = sortedEntries(canonicalDirectory);
            for (int index = entries.size() - 1; index >= 0; --index) {
                const QFileInfo &child = entries.at(index);
                if (child.isDir() || child.isFile() || child.isSymLink()) {
                    pending.append(child.absoluteFilePath());
                }
            }
            continue;
        }
        if (!currentInfo.isFile()
            || !supportedImageExtension(currentInfo.fileName())) {
            continue;
        }
        ++result.discovered;

        const QString canonicalFile = currentInfo.canonicalFilePath();
        MapTileIndex tileIndex;
        if (canonicalFile.isEmpty()
            || !isStrictChildPath(source, canonicalFile)
            || !tryParseOfficialPath(source, canonicalFile, &tileIndex)) {
            ++result.skipped;
            reportImportPeriodically(progress, result, currentPath);
            continue;
        }

        const quint64 key = tileKey(tileIndex);
        if (importedIndexes.contains(key)) {
            ++result.skipped;
            reportImportPeriodically(progress, result, canonicalFile);
            continue;
        }
        importedIndexes.insert(key);

        const qint64 fileSize = currentInfo.size();
        if (fileSize <= 0 || fileSize > MaximumTileBytes) {
            ++result.skipped;
            reportImportPeriodically(progress, result, canonicalFile);
            continue;
        }

        QFile file(canonicalFile);
        if (!file.open(QIODevice::ReadOnly)) {
            ++result.failed;
            reportImportPeriodically(progress, result, canonicalFile);
            continue;
        }
        const QByteArray bytes = file.read(MaximumTileBytes + 1);
        if (bytes.size() != fileSize) {
            ++result.failed;
            reportImportPeriodically(progress, result, canonicalFile);
            continue;
        }
        if (!decodableJpegOrPng(bytes)) {
            ++result.skipped;
            reportImportPeriodically(progress, result, canonicalFile);
            continue;
        }

        const core::Point position(tileIndex.column, tileIndex.row);
        const QString destination = core::PureImageCache::sharedTilePath(
            root, mapType, position, tileIndex.zoom);
        if (!ensureSafeDestination(root, destination)
            || !cache.replaceSharedTile(
                bytes, mapType, position, tileIndex.zoom)) {
            ++result.failed;
            reportImportPeriodically(progress, result, canonicalFile);
            continue;
        }
        ++result.imported;
        result.importedBytes += bytes.size();
        reportImportPeriodically(progress, result, canonicalFile);
    }

    reportImport(progress, result, source);
    return result;
}
