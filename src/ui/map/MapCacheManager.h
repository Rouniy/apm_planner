#ifndef MAPCACHEMANAGER_H
#define MAPCACHEMANAGER_H

#include "maptype.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <functional>

struct MapCacheSnapshot
{
    QString name;
    QString path;
    qint64 sizeBytes = 0;
    qint64 fileCount = 0;
    QDateTime lastWriteUtc;
    bool isTotal = false;
};

struct MapCacheDeleteResult
{
    qint64 removedFiles = 0;
    qint64 freedBytes = 0;
    qint64 failedFiles = 0;
    bool maintenanceBusy = false;
    QString error;
};

class MapCacheManager final
{
public:
    static QList<MapCacheSnapshot> scan(const QString &cacheRoot = QString());
    static MapCacheDeleteResult deleteOlderThan(
        const MapCacheSnapshot &entry, const QDateTime &cutoffUtc,
        const QString &cacheRoot = QString());
    static MapCacheDeleteResult deleteAll(
        const MapCacheSnapshot &entry,
        const QString &cacheRoot = QString());
    static QString formatBytes(qint64 bytes);

private:
    MapCacheManager() = delete;
};

struct MapTileIndex
{
    int column = 0;
    int row = 0;
    int zoom = 0;

    bool operator==(const MapTileIndex &other) const
    {
        return column == other.column && row == other.row
            && zoom == other.zoom;
    }
};

struct MapTileImportProgress
{
    qint64 discovered = 0;
    qint64 imported = 0;
    qint64 skipped = 0;
    qint64 failed = 0;
    QString currentFile;
};

struct MapTileImportResult
{
    qint64 discovered = 0;
    qint64 imported = 0;
    qint64 skipped = 0;
    qint64 failed = 0;
    qint64 importedBytes = 0;
    bool canceled = false;
    bool maintenanceBusy = false;
    QString error;
};

class MapTileImporter final
{
public:
    using ProgressCallback =
        std::function<void(const MapTileImportProgress &progress)>;
    using CancellationCallback = std::function<bool()>;

    static constexpr int MaximumZoom = 21;
    static constexpr qint64 MaximumTileBytes = 32LL * 1024LL * 1024LL;

    static MapTileImportResult importTiles(
        const QString &sourceRoot, core::MapType::Types mapType,
        const QString &cacheRoot = QString(),
        const ProgressCallback &progress = ProgressCallback(),
        const CancellationCallback &isCanceled = CancellationCallback());

    static bool tryParseOfficialPath(const QString &sourceRoot,
                                     const QString &filePath,
                                     MapTileIndex *index);

private:
    MapTileImporter() = delete;
};

#endif // MAPCACHEMANAGER_H
