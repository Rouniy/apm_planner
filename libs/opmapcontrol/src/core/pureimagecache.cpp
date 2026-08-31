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
#include <QSettings>
#include <QVector>
#include <QWriteLocker>

#include <algorithm>
//#define DEBUG_PUREIMAGECACHE
namespace core {
    qlonglong PureImageCache::ConnCounter=0;

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
                                          int zoom) const
    {
        QWriteLocker cacheLocker(&m_sharedCacheLock);
        if (tile.isEmpty()) {
            return false;
        }

        const QString path = sharedTilePath(m_sharedCacheRoot, type, pos, zoom);
        const QFileInfo existing(path);
        if (existing.isFile() && existing.size() > 0) {
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

    void PureImageCache::setGtileCache(const QString &value)
    {
        lock.lockForWrite();
        gtilecache=QDir::cleanPath(value);
        if (value.trimmed().isEmpty()) {
            gtilecache.clear();
            lock.unlock();
            return;
        }
        if(!QDir().mkpath(gtilecache))
        {
#ifdef DEBUG_PUREIMAGECACHE
            qDebug()<<"Unable to create legacy cache directory" << gtilecache;
#endif //DEBUG_PUREIMAGECACHE
        }
        {
            QString db=QDir(gtilecache).filePath(QStringLiteral("Data.qmdb"));
            if(!QFileInfo(db).exists())
            {
#ifdef DEBUG_PUREIMAGECACHE
                qDebug()<<"Try to create EmptyDB";
#endif //DEBUG_PUREIMAGECACHE
                CreateEmptyDB(db);
            }
        }
        lock.unlock();
    }
    QString PureImageCache::GtileCache()
    {
        return gtilecache;
    }


    bool PureImageCache::CreateEmptyDB(const QString &file)
    {
#ifdef DEBUG_PUREIMAGECACHE
        qDebug()<<"Create database at!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!:"<<file;
#endif //DEBUG_PUREIMAGECACHE
        QFileInfo File(file);
        QDir dir=File.absoluteDir();
        QString path=dir.absolutePath();
        QString filename=File.fileName();
        if(File.exists())
            QFile(filename).remove();
        if(!dir.exists())
        {
#ifdef DEBUG_PUREIMAGECACHE
            qDebug()<<"CreateEmptyDB: Cache path doesn't exist, try to create";
#endif //DEBUG_PUREIMAGECACHE
            if(!dir.mkpath(path))
            {
#ifdef DEBUG_PUREIMAGECACHE
                qDebug()<<"CreateEmptyDB: Could not create path";
#endif //DEBUG_PUREIMAGECACHE
                return false;
            }
        }
        QSqlDatabase db;

        db = QSqlDatabase::addDatabase("QSQLITE",QLatin1String("CreateConn"));
        db.setDatabaseName(file);
        if (!db.open())
        {
#ifdef DEBUG_PUREIMAGECACHE
            qDebug()<<"CreateEmptyDB: Unable to create database";
#endif //DEBUG_PUREIMAGECACHE

            return false;
        }
        QSqlQuery query(db);
        query.exec("CREATE TABLE IF NOT EXISTS Tiles (id INTEGER NOT NULL PRIMARY KEY, X INTEGER NOT NULL, Y INTEGER NOT NULL, Zoom INTEGER NOT NULL, Type INTEGER NOT NULL,Date TEXT)");
        if(query.numRowsAffected()==-1)
        {
#ifdef DEBUG_PUREIMAGECACHE
            qDebug()<<"CreateEmptyDB: "<<query.lastError().driverText();
#endif //DEBUG_PUREIMAGECACHE
            db.close();
            return false;
        }
        query.exec("CREATE TABLE IF NOT EXISTS TilesData (id INTEGER NOT NULL PRIMARY KEY CONSTRAINT fk_Tiles_id REFERENCES Tiles(id) ON DELETE CASCADE, Tile BLOB NULL)");
        if(query.numRowsAffected()==-1)
        {
#ifdef DEBUG_PUREIMAGECACHE
            qDebug()<<"CreateEmptyDB: "<<query.lastError().driverText();
#endif //DEBUG_PUREIMAGECACHE
            db.close();
            return false;
        }
        query.exec(
                "CREATE TRIGGER fki_TilesData_id_Tiles_id "
                "BEFORE INSERT ON [TilesData] "
                "FOR EACH ROW BEGIN "
                "SELECT RAISE(ROLLBACK, 'insert on table TilesData violates foreign key constraint fki_TilesData_id_Tiles_id') "
                "WHERE (SELECT id FROM Tiles WHERE id = NEW.id) IS NULL; "
                "END");
        if(query.numRowsAffected()==-1)
        {
#ifdef DEBUG_PUREIMAGECACHE
            qDebug()<<"CreateEmptyDB: "<<query.lastError().driverText();
#endif //DEBUG_PUREIMAGECACHE
            db.close();
            return false;
        }
        query.exec(
                "CREATE TRIGGER fku_TilesData_id_Tiles_id "
                "BEFORE UPDATE ON [TilesData] "
                "FOR EACH ROW BEGIN "
                "SELECT RAISE(ROLLBACK, 'update on table TilesData violates foreign key constraint fku_TilesData_id_Tiles_id') "
                "WHERE (SELECT id FROM Tiles WHERE id = NEW.id) IS NULL; "
                "END");
        if(query.numRowsAffected()==-1)
        {
#ifdef DEBUG_PUREIMAGECACHE
            qDebug()<<"CreateEmptyDB: "<<query.lastError().driverText();
#endif //DEBUG_PUREIMAGECACHE
            db.close();
            return false;
        }
        query.exec(
                "CREATE TRIGGER fkdc_TilesData_id_Tiles_id "
                "BEFORE DELETE ON Tiles "
                "FOR EACH ROW BEGIN "
                "DELETE FROM TilesData WHERE TilesData.id = OLD.id; "
                "END");
        if(query.numRowsAffected()==-1)
        {
#ifdef DEBUG_PUREIMAGECACHE
            qDebug()<<"CreateEmptyDB: "<<query.lastError().driverText();
#endif //DEBUG_PUREIMAGECACHE
            db.close();
            return false;
        }
        db.close();
        QSqlDatabase::removeDatabase(QLatin1String("CreateConn"));
        return true;
    }
    bool PureImageCache::PutImageToCache(const QByteArray &tile, const MapType::Types &type,const Point &pos,const int &zoom)
    {
        const bool sharedCacheWritten = writeSharedTile(tile, type, pos, zoom);
        bool legacyCacheWritten = false;
        if(gtilecache.isEmpty() || gtilecache.isNull())
            return sharedCacheWritten;
        lock.lockForRead();
#ifdef DEBUG_PUREIMAGECACHE
        qDebug()<<"PutImageToCache Start:";//<<pos;
#endif //DEBUG_PUREIMAGECACHE
        Mcounter.lock();
        qlonglong id=++ConnCounter;
        Mcounter.unlock();
        {
            QSqlDatabase cn;
            cn = QSqlDatabase::addDatabase("QSQLITE",QString::number(id));
            QString db=QDir(gtilecache).filePath(QStringLiteral("Data.qmdb"));
            cn.setDatabaseName(db);
            cn.setConnectOptions("QSQLITE_ENABLE_SHARED_CACHE");
            if(cn.open())
            {
                bool tileRowWritten = false;
                {
                    QSqlQuery query(cn);
                    query.prepare("INSERT INTO Tiles(X, Y, Zoom, Type,Date) VALUES(?, ?, ?, ?,?)");
                    query.addBindValue(pos.X());
                    query.addBindValue(pos.Y());
                    query.addBindValue(zoom);

                    query.addBindValue((int)type);
                    query.addBindValue(QDateTime::currentDateTime().toString());
                    tileRowWritten = query.exec();
                }
                if (tileRowWritten) {
                    QSqlQuery query(cn);
                    query.prepare("INSERT INTO TilesData(id, Tile) VALUES((SELECT last_insert_rowid()), ?)");
                    query.addBindValue(tile);
                    legacyCacheWritten = query.exec();
                }
                cn.close();
            }
        }
        QSqlDatabase::removeDatabase(QString::number(id));
        lock.unlock();
        return sharedCacheWritten || legacyCacheWritten;
    }
    QByteArray PureImageCache::GetImageFromCache(MapType::Types type, Point pos, int zoom)
    {
        QByteArray ar = readSharedTile(type, pos, zoom);
        if (!ar.isEmpty())
            return ar;

        lock.lockForRead();
        if(gtilecache.isEmpty()|gtilecache.isNull())
        {
            lock.unlock();
            return ar;
        }
        QString dir=gtilecache;
        Mcounter.lock();
        qlonglong id=++ConnCounter;
        Mcounter.unlock();
#ifdef DEBUG_PUREIMAGECACHE
        qDebug()<<"Cache dir="<<dir<<" Try to GET:"<<pos.X()+","+pos.Y();
#endif //DEBUG_PUREIMAGECACHE

            QString db=QDir(dir).filePath(QStringLiteral("Data.qmdb"));
			{
				QSqlDatabase cn;
			
				cn = QSqlDatabase::addDatabase("QSQLITE",QString::number(id));

	            cn.setDatabaseName(db);
		        cn.setConnectOptions("QSQLITE_ENABLE_SHARED_CACHE");
			    if(cn.open())
				{
					QSqlQuery query(cn);
					query.prepare(QStringLiteral(
                        "SELECT Tile FROM TilesData WHERE id = "
                        "(SELECT id FROM Tiles WHERE X=? AND Y=? AND Zoom=? AND Type=? "
                        "ORDER BY id DESC LIMIT 1)"));
                    query.addBindValue(pos.X());
                    query.addBindValue(pos.Y());
                    query.addBindValue(zoom);
                    query.addBindValue(static_cast<int>(type));
                    query.exec();
					query.next();
					if(query.isValid())
					{
						ar=query.value(0).toByteArray();
					}
					cn.close();
				}
			}
			QSqlDatabase::removeDatabase(QString::number(id));
        lock.unlock();
        if (!ar.isEmpty())
            writeSharedTile(ar, type, pos, zoom);
        return ar;
    }
    void PureImageCache::deleteOlderTiles(int const& days)
    {
        deleteSharedTilesOlderThan(days);
        if(gtilecache.isEmpty()|gtilecache.isNull())
            return;
        QList<long> add;
        bool ret=true;
        QString dir=gtilecache;
        {
            QString db=QDir(dir).filePath(QStringLiteral("Data.qmdb"));
            ret=QFileInfo(db).exists();
            if(ret)
            {
                QSqlDatabase cn;
                Mcounter.lock();
                qlonglong id=++ConnCounter;
                Mcounter.unlock();
                cn = QSqlDatabase::addDatabase("QSQLITE",QString::number(id));
                cn.setDatabaseName(db);
                cn.setConnectOptions("QSQLITE_ENABLE_SHARED_CACHE");
                if(cn.open())
                {
                    {
                        QSqlQuery query(cn);
                        query.exec(QString("SELECT id, X, Y, Zoom, Type, Date FROM Tiles"));
                        while(query.next())
                        {
                            if(QDateTime::fromString(query.value(5).toString()).daysTo(QDateTime::currentDateTime())>days)
                                add.append(query.value(0).toLongLong());
                        }
                        foreach(long i,add)
                        {
                            query.exec(QString("DELETE FROM Tiles WHERE id = %1;").arg(i));
                        }
                    }

                    cn.close();
                }
                QSqlDatabase::removeDatabase(QString::number(id));
            }
        }
    }
    // PureImageCache::ExportMapDataToDB("C:/Users/Xapo/Documents/mapcontrol/debug/mapscache/data.qmdb","C:/Users/Xapo/Documents/mapcontrol/debug/mapscache/data2.qmdb");
    bool PureImageCache::ExportMapDataToDB(QString sourceFile, QString destFile)
    {
        bool ret=true;
        QList<long> add;
        if(!QFileInfo(destFile).exists())
        {
#ifdef DEBUG_PUREIMAGECACHE
            qDebug()<<"Try to create EmptyDB";
#endif //DEBUG_PUREIMAGECACHE
            ret=CreateEmptyDB(destFile);
        }
        if(!ret) return false;
        QSqlDatabase ca = QSqlDatabase::addDatabase("QSQLITE","ca");
        ca.setDatabaseName(sourceFile);

        if(ca.open())
        {
            QSqlDatabase cb = QSqlDatabase::addDatabase("QSQLITE","cb");
            cb.setDatabaseName(destFile);
            if(cb.open())
            {
                QSqlQuery queryb(cb);
                queryb.exec(QString("ATTACH DATABASE \"%1\" AS Source").arg(sourceFile));
                QSqlQuery querya(ca);
                querya.exec("SELECT id, X, Y, Zoom, Type, Date FROM Tiles");
                while(querya.next())
                {
                    long id=querya.value(0).toLongLong();
                    queryb.exec(QString("SELECT id FROM Tiles WHERE X=%1 AND Y=%2 AND Zoom=%3 AND Type=%4;").arg(querya.value(1).toLongLong()).arg(querya.value(2).toLongLong()).arg(querya.value(3).toLongLong()).arg(querya.value(4).toLongLong()));
                    if(!queryb.next())
                    {
                        add.append(id);
                    }

                }
                long f;
                foreach(f,add)
                {
                    queryb.exec(QString("INSERT INTO Tiles(X, Y, Zoom, Type, Date) SELECT X, Y, Zoom, Type, Date FROM Source.Tiles WHERE id=%1").arg(f));
                    queryb.exec(QString("INSERT INTO TilesData(id, Tile) Values((SELECT last_insert_rowid()), (SELECT Tile FROM Source.TilesData WHERE id=%1))").arg(f));
                }
                add.clear();
                ca.close();
                cb.close();

            }
            else return false;
        }
        else return false;
        QSqlDatabase::removeDatabase("ca");
        QSqlDatabase::removeDatabase("cb");
        return true;

    }

}
