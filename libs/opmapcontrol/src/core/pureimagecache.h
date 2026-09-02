/**
******************************************************************************
*
* @file       pureimagecache.h
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
#ifndef PUREIMAGECACHE_H
#define PUREIMAGECACHE_H

#include <QByteArray>
#include <QReadWriteLock>
#include <QString>

#include "maptype.h"
#include "point.h"
namespace core {
    enum class TileCachePlatform
    {
        Windows,
        MacOS,
        Linux
    };

    class PureImageCache
    {

    public:
        explicit PureImageCache(const QString &sharedCacheRoot = QString());
        bool PutImageToCache(const QByteArray &tile,const MapType::Types &type,const core::Point &pos, const int &zoom);
        QByteArray GetImageFromCache(MapType::Types type, core::Point pos, int zoom);
        void deleteOlderTiles(int const& days);

        // This filesystem layout is shared with Mission Planner 10 and Hermes/GTU.
        static QString sharedCacheRoot();
        static QString sharedCacheRootForPlatform(TileCachePlatform platform,
                                                  const QString &homePath,
                                                  const QString &localApplicationData,
                                                  const QString &xdgCacheHome);
        static QString sharedTilePath(const QString &root,
                                      MapType::Types type,
                                      const core::Point &pos,
                                      int zoom);
        static QString providerCacheDirectory(MapType::Types type);
        QString sharedCacheRootPath() const { return m_sharedCacheRoot; }
        qint64 sharedCacheSizeBytes() const;
        int pruneSharedCache(qint64 maximumBytes);
        int deleteSharedTilesOlderThan(int days);
        bool replaceSharedTile(const QByteArray &tile,
                               MapType::Types type,
                               const core::Point &pos,
                               int zoom);

    private:
        bool writeSharedTile(const QByteArray &tile,
                             MapType::Types type,
                             const core::Point &pos,
                             int zoom,
                             bool replaceExisting = false) const;
        QByteArray readSharedTile(MapType::Types type,
                                  const core::Point &pos,
                                  int zoom) const;

        QString m_sharedCacheRoot;
        mutable QReadWriteLock m_sharedCacheLock;

    };

}
#endif // PUREIMAGECACHE_H
