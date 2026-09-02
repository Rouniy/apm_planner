/**
******************************************************************************
*
* @file       pureimagecache.cpp
* @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
* @brief      
* @see        The GNU Public License (GPL) Version 3
* @defgroup   OPMapWidget
* @{
* 
*****************************************************************************/
/* 
* This program is free software; you can redistribute it and/or modify 
* it under the terms of the GNU General Public License as published by 
* the Free Software Foundation; either version 3 of the License, or 
* (at your option) any later version.
* 
* This program is distributed in the hope that it will be useful, but 
* WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY 
* or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
* for more details.
* 
* You should have received a copy of the GNU General Public License along 
* with this program; if not, write to the Free Software Foundation, Inc., 
* 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/
#include "pureimagecache.h"
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QLockFile>
#include <QReadLocker>
#include <QSaveFile>
#include <QVector>
#include <QWriteLocker>

#include <algorithm>
namespace core {
    namespace {
    const QString kGoogleSatelliteCache = QStringLiteral("googlesatellitemap-a52b97e5747b7cd4");
    const QString kGoogleHybridCache = QStringLiteral("googlehybridmap-cd9494fe865f0e67");
    const QString kBingSatelliteCache = QStringLiteral("bingsatellitemap-300e1755bb3d3f03");
    const QString kOpenStreetMapCache = QStringLiteral("openstreetmap-f65928ca3a8e2a2e");
    const QString kEsriWorldImageryCache = QStringLiteral("esriworldimagery-313cb2e33e57e602");

    QString sanitizedProviderName(const QString &value)
    {
        QString result;
        result.reserve(value.size());
        bool previousWasDash = false;
        for (const QChar character : value) {
            if (character.isLetterOrNumber()) {
                result.append(character.toLower());
                previousWasDash = false;
            } else if (!previousWasDash && !result.isEmpty()) {
                result.append(QLatin1Char('-'));
                previousWasDash = true;
            }
        }
        while (result.endsWith(QLatin1Char('-'))) {
            result.chop(1);
        }
        return result.isEmpty() ? QStringLiteral("tiles") : result;
    }

    struct SharedTileInfo
    {
        QString path;
        qint64 size = 0;
        QDateTime lastUse;
    };

    QVector<SharedTileInfo> sharedTiles(const QString &root)
    {
        QVector<SharedTileInfo> result;
        QDirIterator iterator(root, QStringList(QStringLiteral("*.tile")),
                              QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            iterator.next();
            const QFileInfo info = iterator.fileInfo();
            const QDateTime lastUse = info.lastRead().isValid()
                ? info.lastRead() : info.lastModified();
            result.append({info.absoluteFilePath(), info.size(), lastUse});
        }
        return result;
    }
    }

    PureImageCache::PureImageCache(const QString &sharedRoot)
        : m_sharedCacheRoot(sharedRoot.isEmpty() ? sharedCacheRoot() : sharedRoot)
    {

    }

    QString PureImageCache::sharedCacheRoot()
    {
#ifdef Q_OS_WIN
        return sharedCacheRootForPlatform(TileCachePlatform::Windows,
                                          QDir::homePath(),
                                          qEnvironmentVariable("LOCALAPPDATA"),
                                          QString());
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
        return sharedCacheRootForPlatform(TileCachePlatform::MacOS,
                                          QDir::homePath(),
                                          QString(),
                                          QString());
#else
        return sharedCacheRootForPlatform(TileCachePlatform::Linux,
                                          QDir::homePath(),
                                          QString(),
                                          qEnvironmentVariable("XDG_CACHE_HOME"));
#endif
    }

    QString PureImageCache::sharedCacheRootForPlatform(TileCachePlatform platform,
                                                        const QString &homePath,
                                                        const QString &localApplicationData,
                                                        const QString &xdgCacheHome)
    {
        QString base;
        switch (platform) {
        case TileCachePlatform::Windows:
            base = localApplicationData.isEmpty()
                ? QDir(homePath).filePath(QStringLiteral("AppData/Local"))
                : localApplicationData;
            return QDir::cleanPath(
                QDir(base).filePath(QStringLiteral("MissionPlanner/cache/map-tiles")));
        case TileCachePlatform::MacOS:
            return QDir::cleanPath(QDir(homePath).filePath(
                QStringLiteral("Library/Caches/MissionPlanner/map-tiles")));
        case TileCachePlatform::Linux:
            base = !xdgCacheHome.isEmpty() && QFileInfo(xdgCacheHome).isAbsolute()
                ? xdgCacheHome
                : QDir(homePath).filePath(QStringLiteral(".cache"));
            return QDir::cleanPath(
                QDir(base).filePath(QStringLiteral("MissionPlanner/map-tiles")));
        }
        return QString();
    }

    QString PureImageCache::providerCacheDirectory(MapType::Types type)
    {
        switch (type) {
        case MapType::GoogleSatellite:
            return kGoogleSatelliteCache;
        case MapType::GoogleHybrid:
            return kGoogleHybridCache;
        case MapType::BingSatellite:
            return kBingSatelliteCache;
        case MapType::OpenStreetMap:
            return kOpenStreetMapCache;
        case MapType::ArcGIS_Satellite:
            return kEsriWorldImageryCache;
        default:
            return QStringLiteral("apm-%1-%2")
                .arg(sanitizedProviderName(MapType::StrByType(type)))
                .arg(static_cast<int>(type));
        }
    }

    QString PureImageCache::sharedTilePath(const QString &root,
                                            MapType::Types type,
                                            const Point &pos,
                                            int zoom)
    {
        return QDir(root).filePath(QStringLiteral("%1/%2/%3/%4.tile")
                                       .arg(providerCacheDirectory(type))
                                       .arg(zoom)
                                       .arg(pos.X())
                                       .arg(pos.Y()));
    }

    bool PureImageCache::writeSharedTile(const QByteArray &tile,
                                          MapType::Types type,
                                          const Point &pos,
                                          int zoom,
                                          bool replaceExisting) const
    {
        QWriteLocker cacheLocker(&m_sharedCacheLock);
        if (tile.isEmpty()) {
            return false;
        }

        const QString path = sharedTilePath(m_sharedCacheRoot, type, pos, zoom);
        const QFileInfo existing(path);
        if (!replaceExisting && existing.isFile() && existing.size() > 0) {
            return true;
        }
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            return false;
        }

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(tile) != tile.size()) {
            file.cancelWriting();
            return false;
        }
        return file.commit();
    }

    bool PureImageCache::replaceSharedTile(const QByteArray &tile,
                                            MapType::Types type,
                                            const Point &pos,
                                            int zoom)
    {
        return writeSharedTile(tile, type, pos, zoom, true);
    }

    QByteArray PureImageCache::readSharedTile(MapType::Types type,
                                               const Point &pos,
                                               int zoom) const
    {
        QReadLocker cacheLocker(&m_sharedCacheLock);
        QFile file(sharedTilePath(m_sharedCacheRoot, type, pos, zoom));
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) {
            return QByteArray();
        }
        const QByteArray tile = file.readAll();
        file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileAccessTime);
        return tile;
    }

    qint64 PureImageCache::sharedCacheSizeBytes() const
    {
        QReadLocker cacheLocker(&m_sharedCacheLock);
        qint64 total = 0;
        const QVector<SharedTileInfo> tiles = sharedTiles(m_sharedCacheRoot);
        for (const SharedTileInfo &tile : tiles) {
            total += tile.size;
        }
        return total;
    }

    int PureImageCache::pruneSharedCache(qint64 maximumBytes)
    {
        if (maximumBytes < 0 || !QDir().mkpath(m_sharedCacheRoot)) {
            return 0;
        }

        QLockFile maintenanceLock(QDir(m_sharedCacheRoot).filePath(
            QStringLiteral(".maintenance.lock")));
        if (!maintenanceLock.tryLock(0)) {
            return 0;
        }

        QWriteLocker cacheLocker(&m_sharedCacheLock);
        QVector<SharedTileInfo> tiles = sharedTiles(m_sharedCacheRoot);
        qint64 total = 0;
        for (const SharedTileInfo &tile : tiles) {
            total += tile.size;
        }
        if (total <= maximumBytes) {
            return 0;
        }

        std::sort(tiles.begin(), tiles.end(), [](const SharedTileInfo &left,
                                                  const SharedTileInfo &right) {
            if (left.lastUse == right.lastUse) {
                return left.path < right.path;
            }
            return left.lastUse < right.lastUse;
        });

        int removed = 0;
        for (const SharedTileInfo &tile : tiles) {
            if (total <= maximumBytes) {
                break;
            }
            if (QFile::remove(tile.path)) {
                total -= tile.size;
                ++removed;
            }
        }
        return removed;
    }

    int PureImageCache::deleteSharedTilesOlderThan(int days)
    {
        if (days < 0 || !QDir(m_sharedCacheRoot).exists()) {
            return 0;
        }

        QLockFile maintenanceLock(QDir(m_sharedCacheRoot).filePath(
            QStringLiteral(".maintenance.lock")));
        if (!maintenanceLock.tryLock(0)) {
            return 0;
        }

        const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-days);
        QWriteLocker cacheLocker(&m_sharedCacheLock);
        int removed = 0;
        const QVector<SharedTileInfo> tiles = sharedTiles(m_sharedCacheRoot);
        for (const SharedTileInfo &tile : tiles) {
            if (tile.lastUse.isValid() && tile.lastUse < cutoff
                && QFile::remove(tile.path)) {
                ++removed;
            }
        }
        return removed;
    }

    bool PureImageCache::PutImageToCache(const QByteArray &tile, const MapType::Types &type,const Point &pos,const int &zoom)
    {
        return writeSharedTile(tile, type, pos, zoom);
    }

    QByteArray PureImageCache::GetImageFromCache(MapType::Types type, Point pos, int zoom)
    {
        return readSharedTile(type, pos, zoom);
    }

    void PureImageCache::deleteOlderTiles(int const& days)
    {
        deleteSharedTilesOlderThan(days);
    }

}
