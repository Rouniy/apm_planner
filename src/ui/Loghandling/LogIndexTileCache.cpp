#include "LogIndexTileCache.h"
#include "pureimagecache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

LogIndex::TileReader LogIndexTileCache::reader(
    const QString &cacheRoot, core::MapType::Types mapType)
{
    const QString root = cacheRoot.isEmpty()
        ? core::PureImageCache::sharedCacheRoot() : QDir::cleanPath(cacheRoot);
    if (core::PureImageCache::providerCacheDirectory(mapType).isEmpty()) return {};
    return [root, mapType](int x, int y, int zoom) -> QByteArray {
        if (zoom < 0 || zoom > 21 || x < 0 || y < 0
            || x >= (1 << zoom) || y >= (1 << zoom)) return {};
        constexpr qint64 MaximumTileBytes = 32LL * 1024 * 1024;
        const QString path = core::PureImageCache::sharedTilePath(
            root, mapType, core::Point(x, y), zoom);
        const QFileInfo before(path);
        if (!before.isFile() || before.isSymLink()
            || before.size() <= 0 || before.size() > MaximumTileBytes) return {};
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        const QByteArray bytes = file.read(MaximumTileBytes + 1);
        const QFileInfo after(path);
        if (file.error() != QFileDevice::NoError || bytes.size() != before.size()
            || !after.isFile() || after.isSymLink() || after.size() != before.size()
            || after.lastModified() != before.lastModified()) return {};
        return bytes;
    };
}
