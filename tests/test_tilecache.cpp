#include <QtTest>

#include "alllayersoftype.h"
#include "opmaps.h"
#include "pureimagecache.h"
#include "urlfactory.h"

#include <QFile>
#include <QTemporaryDir>

namespace {
class FakeLocalTileProvider final : public core::LocalTileProvider
{
public:
    QByteArray tileImage(core::MapType::Types type,
                         const core::Point &position,
                         int zoom) override
    {
        lastType = type;
        lastPosition = position;
        lastZoom = zoom;
        ++calls;
        return bytes;
    }

    QByteArray bytes = QByteArrayLiteral("first-local-raster-tile");
    int calls = 0;
    core::MapType::Types lastType = static_cast<core::MapType::Types>(0);
    core::Point lastPosition;
    int lastZoom = -1;
};

class LocalTileProviderRegistration final
{
public:
    explicit LocalTileProviderRegistration(core::LocalTileProvider *provider)
    {
        core::OPMaps::Instance()->invalidateLocalTiles(core::MapType::GDALCustom);
        core::OPMaps::Instance()->setLocalTileProvider(provider);
    }

    ~LocalTileProviderRegistration()
    {
        core::OPMaps::Instance()->setLocalTileProvider(nullptr);
        core::OPMaps::Instance()->invalidateLocalTiles(core::MapType::GDALCustom);
    }
};
}

class TileCacheTest final : public QObject
{
    Q_OBJECT

private slots:
    void platformRoots();
    void providerDirectories();
    void sharedPathAndRoundTrip();
    void sharedQuotaRemovesLeastRecentlyUsedTiles();
    void sharedAgeCleanupPreservesRecentTiles();
    void missionPlannerCompatibleUrls();
    void googleHybridUsesOneNativeLayer();
    void gdalCustomUsesSatelliteAndLocalLayers();
    void gdalCustomUsesOnlyMemoryCache();
};

void TileCacheTest::platformRoots()
{
    using core::PureImageCache;
    using core::TileCachePlatform;

    QCOMPARE(PureImageCache::sharedCacheRootForPlatform(
                 TileCachePlatform::Linux, QStringLiteral("/home/tester"), QString(), QString()),
             QStringLiteral("/home/tester/.cache/MissionPlanner/map-tiles"));
    QCOMPARE(PureImageCache::sharedCacheRootForPlatform(
                 TileCachePlatform::Linux, QStringLiteral("/home/tester"), QString(),
                 QStringLiteral("/var/cache/tester")),
             QStringLiteral("/var/cache/tester/MissionPlanner/map-tiles"));
    QCOMPARE(PureImageCache::sharedCacheRootForPlatform(
                 TileCachePlatform::Linux, QStringLiteral("/home/tester"), QString(),
                 QStringLiteral("relative-cache")),
             QStringLiteral("/home/tester/.cache/MissionPlanner/map-tiles"));
    QCOMPARE(PureImageCache::sharedCacheRootForPlatform(
                 TileCachePlatform::Windows, QStringLiteral("C:/Users/tester"),
                 QStringLiteral("D:/Local"), QString()),
             QStringLiteral("D:/Local/MissionPlanner/cache/map-tiles"));
    QCOMPARE(PureImageCache::sharedCacheRootForPlatform(
                 TileCachePlatform::Windows, QStringLiteral("C:/Users/tester"), QString(), QString()),
             QStringLiteral("C:/Users/tester/AppData/Local/MissionPlanner/cache/map-tiles"));
    QCOMPARE(PureImageCache::sharedCacheRootForPlatform(
                 TileCachePlatform::MacOS, QStringLiteral("/Users/tester"), QString(), QString()),
             QStringLiteral("/Users/tester/Library/Caches/MissionPlanner/map-tiles"));
}

void TileCacheTest::providerDirectories()
{
    using core::MapType;
    using core::PureImageCache;

    QCOMPARE(PureImageCache::providerCacheDirectory(MapType::GoogleSatellite),
             QStringLiteral("googlesatellitemap-a52b97e5747b7cd4"));
    QCOMPARE(PureImageCache::providerCacheDirectory(MapType::GoogleHybrid),
             QStringLiteral("googlehybridmap-cd9494fe865f0e67"));
    QCOMPARE(PureImageCache::providerCacheDirectory(MapType::BingSatellite),
             QStringLiteral("bingsatellitemap-300e1755bb3d3f03"));
    QCOMPARE(PureImageCache::providerCacheDirectory(MapType::OpenStreetMap),
             QStringLiteral("openstreetmap-f65928ca3a8e2a2e"));
    QCOMPARE(PureImageCache::providerCacheDirectory(MapType::ArcGIS_Satellite),
             QStringLiteral("esriworldimagery-313cb2e33e57e602"));
}

void TileCacheTest::sharedPathAndRoundTrip()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const core::Point tilePosition(12, 34);
    const QByteArray tileBytes("mission-planner-compatible-tile");
    core::PureImageCache cache(root.path());

    const QString expectedPath = root.filePath(
        QStringLiteral("googlesatellitemap-a52b97e5747b7cd4/7/12/34.tile"));
    QCOMPARE(core::PureImageCache::sharedTilePath(
                 root.path(), core::MapType::GoogleSatellite, tilePosition, 7),
             expectedPath);

    QVERIFY(cache.PutImageToCache(tileBytes, core::MapType::GoogleSatellite, tilePosition, 7));
    QVERIFY(QFile::exists(expectedPath));
    QCOMPARE(cache.GetImageFromCache(core::MapType::GoogleSatellite, tilePosition, 7), tileBytes);
}

void TileCacheTest::sharedQuotaRemovesLeastRecentlyUsedTiles()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    core::PureImageCache cache(root.path());

    const core::Point oldestPosition(1, 1);
    const core::Point middlePosition(2, 2);
    const core::Point newestPosition(3, 3);
    QVERIFY(cache.PutImageToCache(QByteArray(5, 'a'), core::MapType::OpenStreetMap,
                                  oldestPosition, 4));
    QVERIFY(cache.PutImageToCache(QByteArray(5, 'b'), core::MapType::OpenStreetMap,
                                  middlePosition, 4));
    QVERIFY(cache.PutImageToCache(QByteArray(5, 'c'), core::MapType::OpenStreetMap,
                                  newestPosition, 4));

    const QList<core::Point> positions{oldestPosition, middlePosition, newestPosition};
    for (int index = 0; index < positions.size(); ++index) {
        QFile tile(core::PureImageCache::sharedTilePath(
            root.path(), core::MapType::OpenStreetMap, positions.at(index), 4));
        QVERIFY(tile.open(QIODevice::ReadOnly));
        const QDateTime useTime = QDateTime::currentDateTimeUtc().addDays(index - 3);
        QVERIFY(tile.setFileTime(useTime, QFileDevice::FileAccessTime));
        QVERIFY(tile.setFileTime(useTime, QFileDevice::FileModificationTime));
    }

    QCOMPARE(cache.sharedCacheSizeBytes(), qint64(15));
    QCOMPARE(cache.pruneSharedCache(8), 2);
    QCOMPARE(cache.sharedCacheSizeBytes(), qint64(5));
    QVERIFY(!QFile::exists(core::PureImageCache::sharedTilePath(
        root.path(), core::MapType::OpenStreetMap, oldestPosition, 4)));
    QVERIFY(!QFile::exists(core::PureImageCache::sharedTilePath(
        root.path(), core::MapType::OpenStreetMap, middlePosition, 4)));
    QVERIFY(QFile::exists(core::PureImageCache::sharedTilePath(
        root.path(), core::MapType::OpenStreetMap, newestPosition, 4)));
}

void TileCacheTest::sharedAgeCleanupPreservesRecentTiles()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    core::PureImageCache cache(root.path());
    const core::Point oldPosition(4, 4);
    const core::Point recentPosition(5, 5);
    QVERIFY(cache.PutImageToCache(QByteArray("old"), core::MapType::BingSatellite,
                                  oldPosition, 6));
    QVERIFY(cache.PutImageToCache(QByteArray("recent"), core::MapType::BingSatellite,
                                  recentPosition, 6));

    QFile oldTile(core::PureImageCache::sharedTilePath(
        root.path(), core::MapType::BingSatellite, oldPosition, 6));
    QVERIFY(oldTile.open(QIODevice::ReadOnly));
    const QDateTime oldTime = QDateTime::currentDateTimeUtc().addDays(-45);
    QVERIFY(oldTile.setFileTime(oldTime, QFileDevice::FileAccessTime));
    QVERIFY(oldTile.setFileTime(oldTime, QFileDevice::FileModificationTime));
    oldTile.close();

    QCOMPARE(cache.deleteSharedTilesOlderThan(30), 1);
    QVERIFY(!QFile::exists(core::PureImageCache::sharedTilePath(
        root.path(), core::MapType::BingSatellite, oldPosition, 6)));
    QVERIFY(QFile::exists(core::PureImageCache::sharedTilePath(
        root.path(), core::MapType::BingSatellite, recentPosition, 6)));
}

void TileCacheTest::missionPlannerCompatibleUrls()
{
    core::UrlFactory factory;
    const core::Point tilePosition(12, 34);

    QCOMPARE(factory.MakeImageUrl(core::MapType::GoogleSatellite, tilePosition, 7, QStringLiteral("en")),
             QStringLiteral("https://mt1.google.com/vt/lyrs=s&x=12&y=34&z=7"));
    QCOMPARE(factory.MakeImageUrl(core::MapType::GoogleHybrid, tilePosition, 7, QStringLiteral("en")),
             QStringLiteral("https://mt1.google.com/vt/lyrs=y&x=12&y=34&z=7"));
    QCOMPARE(factory.MakeImageUrl(core::MapType::OpenStreetMap, tilePosition, 7, QStringLiteral("en")),
             QStringLiteral("https://tile.openstreetmap.org/7/12/34.png"));
    QCOMPARE(factory.MakeImageUrl(core::MapType::BingSatellite, tilePosition, 7, QStringLiteral("en")),
             QStringLiteral("https://ecn.t0.tiles.virtualearth.net/tiles/a0201120.jpeg?g=1&n=z"));
    QCOMPARE(factory.MakeImageUrl(core::MapType::ArcGIS_Satellite, tilePosition, 7, QStringLiteral("en")),
             QStringLiteral("https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/7/34/12"));
}

void TileCacheTest::googleHybridUsesOneNativeLayer()
{
    core::AllLayersOfType layers;
    const QVector<core::MapType::Types> hybrid =
        layers.GetAllLayersOfType(core::MapType::GoogleHybrid);

    QCOMPARE(hybrid.size(), 1);
    QCOMPARE(hybrid.constFirst(), core::MapType::GoogleHybrid);
}

void TileCacheTest::gdalCustomUsesSatelliteAndLocalLayers()
{
    QCOMPARE(core::MapType::StrByType(core::MapType::GDALCustom),
             QStringLiteral("GDAL Custom"));
    QCOMPARE(core::MapType::TypeByStr(QStringLiteral("GDAL Custom")),
             core::MapType::GDALCustom);

    core::AllLayersOfType layers;
    const QVector<core::MapType::Types> custom =
        layers.GetAllLayersOfType(core::MapType::GDALCustom);

    QCOMPARE(custom,
             QVector<core::MapType::Types>({core::MapType::GoogleSatellite,
                                            core::MapType::GDALCustom}));
}

void TileCacheTest::gdalCustomUsesOnlyMemoryCache()
{
    FakeLocalTileProvider provider;
    LocalTileProviderRegistration registration(&provider);
    const core::RawTile satelliteTile(
        core::MapType::GoogleSatellite, core::Point(72, 43), 9);
    const QByteArray satelliteBytes =
        QByteArrayLiteral("shared-satellite-memory-tile");
    core::OPMaps::Instance()->AddTileToMemoryCache(
        satelliteTile, satelliteBytes);
    const core::Point position(71, 42);
    const QString forbiddenDiskPath = core::PureImageCache::sharedTilePath(
        core::PureImageCache::sharedCacheRoot(),
        core::MapType::GDALCustom, position, 9);
    QVERIFY2(!QFile::exists(forbiddenDiskPath),
             qPrintable(forbiddenDiskPath));

    QCOMPARE(core::OPMaps::Instance()->GetImageFrom(
                 core::MapType::GDALCustom, position, 9),
             provider.bytes);
    QCOMPARE(provider.calls, 1);
    QCOMPARE(provider.lastType, core::MapType::GDALCustom);
    QCOMPARE(provider.lastPosition, position);
    QCOMPARE(provider.lastZoom, 9);

    provider.bytes = QByteArrayLiteral("second-local-raster-tile");
    QCOMPARE(core::OPMaps::Instance()->GetImageFrom(
                 core::MapType::GDALCustom, position, 9),
             QByteArrayLiteral("first-local-raster-tile"));
    QCOMPARE(provider.calls, 1);

    core::OPMaps::Instance()->invalidateLocalTiles(core::MapType::GDALCustom);
    QCOMPARE(core::OPMaps::Instance()->GetTileFromMemoryCache(satelliteTile),
             satelliteBytes);
    QCOMPARE(core::OPMaps::Instance()->GetImageFrom(
                 core::MapType::GDALCustom, position, 9),
             provider.bytes);
    QCOMPARE(provider.calls, 2);
    QVERIFY2(!QFile::exists(forbiddenDiskPath),
             qPrintable(forbiddenDiskPath));
}

QTEST_APPLESS_MAIN(TileCacheTest)

#include "test_tilecache.moc"
