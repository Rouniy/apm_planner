/**
******************************************************************************
*
* @file       cache.cpp
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
#include "cache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace {

QString readUtf8File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

bool writeUtf8File(const QString &path, const QString &content)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }

    QSaveFile file(path);
    const QByteArray bytes = content.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace

namespace core {
    Cache* Cache::m_pInstance=0;

    Cache* Cache::Instance()
    {
        if(!m_pInstance)
            m_pInstance=new Cache;
        return m_pInstance;
    }

    Cache::Cache()
        : Cache(QString())
    {
    }

    Cache::Cache(const QString &sharedCacheRoot)
        : ImageCache(sharedCacheRoot)
    {
        // Keep all persistent map data below the cross-backend shared root.
        // The retired setCacheLocation() API used to initialise these paths;
        // leaving them empty would make geocoder responses relative to the
        // process working directory.
        const QDir metadataRoot(QDir(ImageCache.sharedCacheRootPath())
                                    .filePath(QStringLiteral("metadata")));
        geoCache = metadataRoot.filePath(QStringLiteral("geocoder"))
            + QDir::separator();
        placemarkCache = metadataRoot.filePath(QStringLiteral("placemark"))
            + QDir::separator();
    }

    QString Cache::GetGeocoderFromCache(const QString &urlEnd)
    {
#ifdef DEBUG_GetGeocoderFromCache
        qDebug()<<"Entered GetGeocoderFromCache";
#endif
        QString filename=geoCache+QString(urlEnd)+".geo";
#ifdef DEBUG_GetGeocoderFromCache
        qDebug()<<"GetGeocoderFromCache: Does file exist?:"<<filename;
#endif
        const QString ret = readUtf8File(filename);
#ifdef DEBUG_GetGeocoderFromCache
        qDebug()<<"GetGeocoderFromCache:Returning:"<<ret;
#endif
        return ret;
    }

    void Cache::CacheGeocoder(const QString &urlEnd, const QString &content)
    {
        QString filename=geoCache+QString(urlEnd)+".geo";
#ifdef DEBUG_CACHE
        qDebug()<<"CacheGeocoder: Filename:"<<filename;
#endif //DEBUG_CACHE
#ifdef DEBUG_CACHE
        qDebug()<<"CacheGeocoder: Path:"<<QFileInfo(filename).absolutePath();
        qDebug()<<"CacheGeocoder: OpenFile:"<<filename;
#endif //DEBUG_CACHE
        writeUtf8File(filename, content);
    }

    QString Cache::GetPlacemarkFromCache(const QString &urlEnd)
    {
#ifdef DEBUG_CACHE
        qDebug()<<"Entered GetPlacemarkFromCache";
#endif //DEBUG_CACHE
        QString filename=placemarkCache+QString(urlEnd)+".plc";
#ifdef DEBUG_CACHE
        qDebug()<<"GetPlacemarkFromCache: Does file exist?:"<<filename;
#endif //DEBUG_CACHE
        const QString ret = readUtf8File(filename);
#ifdef DEBUG_CACHE
        qDebug()<<"GetPlacemarkFromCache:Returning:"<<ret;
#endif //DEBUG_CACHE
        return ret;
    }
    void Cache::CachePlacemark(const QString &urlEnd, const QString &content)
    {
        QString filename=placemarkCache+QString(urlEnd)+".plc";
#ifdef DEBUG_CACHE
        qDebug()<<"CachePlacemark: Filename:"<<filename;
#endif //DEBUG_CACHE
#ifdef DEBUG_CACHE
        qDebug()<<"CachePlacemark: Path:"<<QFileInfo(filename).absolutePath();
        qDebug()<<"CachePlacemark: OpenFile:"<<filename;
#endif //DEBUG_CACHE
        writeUtf8File(filename, content);
    }
}
