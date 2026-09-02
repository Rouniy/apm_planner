#include <QtTest>

#include "MapPrefetchService.h"
#include "accessmode.h"
#include "opmaps.h"
#include "point.h"
#include "pureimagecache.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <stdexcept>

namespace {
QVector<MapTileInfo> sequentialTiles(int count, int zoom = 10)
{
    QVector<MapTileInfo> result;
    result.reserve(count);
    const int width = 1 << zoom;
    for (int index = 0; index < count; ++index) {
        result.append({zoom, index % width, index / width});
    }
    return result;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
} // namespace

class MapPrefetchServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void missionRouteMatchesMissionPlannerNavigationRules();
    void areaAndPathTilesAreUniqueSortedAndBounded();
    void splitDateLineAreaDoesNotEnumerateTheWorld();
    void dateLinePathUsesTheShortWrappedRoute();
    void pathSupportsASinglePointAndUsesACorridor();
    void prefetchLimitsConcurrencyAndWritesSharedCache();
    void cachedTilesBypassTheLoader();
    void emptyAndFailedLoadsAreCountedAndProgressIsPeriodic();
    void cancellationStopsLaunchingTilesAndKeepsCompletedFiles();
    void rejectsUnsupportedProvidersAndDefaultLoaderCacheMode();
};

void MapPrefetchServiceTest::missionRouteMatchesMissionPlannerNavigationRules()
{
    QVERIFY(MissionRoute::IsNavigation(16));
    QVERIFY(MissionRoute::IsNavigation(94));
    QVERIFY(!MissionRoute::IsNavigation(15));
    QVERIFY(!MissionRoute::IsNavigation(80));
    QVERIFY(!MissionRoute::IsFlightPath(80));
    QVERIFY(MissionRoute::IsNavigation(189));
    QVERIFY(!MissionRoute::IsFlightPath(189));
    QVERIFY(!MissionRoute::IsNavigation(192));
    QVERIFY(!MissionRoute::IsFlightPath(192));
    QVERIFY(MissionRoute::IsFlightPath(16));
}

void MapPrefetchServiceTest::areaAndPathTilesAreUniqueSortedAndBounded()
{
    const QVector<MapTileInfo> area = MapPrefetchService::AreaTiles(
        QRectF(-500.0, -500.0, 1000.0, 1000.0), 12, 13);
    QVERIFY(!area.isEmpty());
    for (int index = 1; index < area.size(); ++index) {
        QVERIFY(area.at(index - 1) < area.at(index));
    }

    const QVector<MapTileInfo> path = MapPrefetchService::PathTiles(
        {QPointF(149.16, -35.36), QPointF(149.25, -35.30)}, 12, 13);
    QVERIFY(!path.isEmpty());
    for (int index = 1; index < path.size(); ++index) {
        QVERIFY(path.at(index - 1) < path.at(index));
    }

    const QVector<MapTileInfo> huge = MapPrefetchService::AreaTiles(
        QRectF(-20037508.0, -20037508.0,
               40075016.0, 40075016.0), 21, 21);
    QCOMPARE(huge.size(), MapPrefetchService::EnumerationLimit);
}

void MapPrefetchServiceTest::pathSupportsASinglePointAndUsesACorridor()
{
    const QVector<MapTileInfo> tiles = MapPrefetchService::PathTiles(
        {QPointF(0.0, 0.0)}, 5, 5);
    QVERIFY(!tiles.isEmpty());
    QVERIFY(tiles.size() >= 4);
    for (const MapTileInfo &tile : tiles) {
        QCOMPARE(tile.zoom, 5);
    }
}

void MapPrefetchServiceTest::splitDateLineAreaDoesNotEnumerateTheWorld()
{
    constexpr double originShift = 20037508.342789244;
    const QVector<MapTileInfo> tiles = MapPrefetchService::AreaTiles(
        {QRectF(originShift - 1000.0, -500.0, 1000.0, 1000.0),
         QRectF(-originShift, -500.0, 1000.0, 1000.0)},
        3, 3);
    QVERIFY(!tiles.isEmpty());
    QVERIFY(tiles.size() <= 8);
    bool westEdge = false;
    bool eastEdge = false;
    for (const MapTileInfo &tile : tiles) {
        QCOMPARE(tile.zoom, 3);
        QVERIFY(tile.x == 0 || tile.x == 7);
        westEdge = westEdge || tile.x == 0;
        eastEdge = eastEdge || tile.x == 7;
    }
    QVERIFY(westEdge);
    QVERIFY(eastEdge);
}

void MapPrefetchServiceTest::dateLinePathUsesTheShortWrappedRoute()
{
    constexpr int zoom = 6;
    constexpr int worldTiles = 1 << zoom;
    const QVector<MapTileInfo> tiles = MapPrefetchService::PathTiles(
        {QPointF(179.0, 0.0), QPointF(-179.0, 0.0)}, zoom, zoom);
    QVERIFY(!tiles.isEmpty());
    QVERIFY(tiles.size() < 20);
    bool westEdge = false;
    bool eastEdge = false;
    for (const MapTileInfo &tile : tiles) {
        QCOMPARE(tile.zoom, zoom);
        QVERIFY(tile.x <= 1 || tile.x >= worldTiles - 2);
        westEdge = westEdge || tile.x <= 1;
        eastEdge = eastEdge || tile.x >= worldTiles - 2;
    }
    QVERIFY(westEdge);
    QVERIFY(eastEdge);
}

void MapPrefetchServiceTest::prefetchLimitsConcurrencyAndWritesSharedCache()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    MapPrefetchService service(root.path());
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};

    const MapPrefetchResult result = service.Prefetch(
        core::MapType::GoogleSatellite, sequentialTiles(16),
        [&active, &maximum](core::MapType::Types,
                            const MapTileInfo &tile,
                            const MapPrefetchService::CancellationCallback &) {
            const int current = ++active;
            int observed = maximum.load();
            while (current > observed
                   && !maximum.compare_exchange_weak(observed, current)) {
            }
            QThread::msleep(5);
            --active;
            return QByteArray("tile-") + QByteArray::number(tile.x);
        });

    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.total, 16);
    QCOMPARE(result.downloaded, 16);
    QCOMPARE(result.cached, 0);
    QCOMPARE(result.failed, 0);
    QVERIFY(maximum.load() > 1);
    QVERIFY(maximum.load() <= MapPrefetchService::MaximumConcurrentLoads);

    const MapTileInfo tile = sequentialTiles(16).at(7);
    const QString path = core::PureImageCache::sharedTilePath(
        root.path(), core::MapType::GoogleSatellite,
        core::Point(tile.x, tile.y), tile.zoom);
    QCOMPARE(readFile(path), QByteArrayLiteral("tile-7"));
}

void MapPrefetchServiceTest::cachedTilesBypassTheLoader()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    core::PureImageCache cache(root.path());
    QVERIFY(cache.replaceSharedTile(QByteArrayLiteral("existing"),
                                    core::MapType::OpenStreetMap,
                                    core::Point(3, 4), 5));
    int loads = 0;
    MapPrefetchService service(root.path());
    const MapPrefetchResult result = service.Prefetch(
        core::MapType::OpenStreetMap, {{5, 3, 4}},
        [&loads](core::MapType::Types, const MapTileInfo &,
                 const MapPrefetchService::CancellationCallback &) {
            ++loads;
            return QByteArrayLiteral("unexpected");
        });

    QCOMPARE(loads, 0);
    QCOMPARE(result.cached, 1);
    QCOMPARE(result.downloaded, 1);
    QCOMPARE(result.failed, 0);
}

void MapPrefetchServiceTest::emptyAndFailedLoadsAreCountedAndProgressIsPeriodic()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVector<QPair<int, int>> progressValues;
    MapPrefetchService service(root.path());
    const MapPrefetchResult result = service.Prefetch(
        core::MapType::BingSatellite, sequentialTiles(53),
        [](core::MapType::Types, const MapTileInfo &tile,
           const MapPrefetchService::CancellationCallback &) -> QByteArray {
            if (tile.x == 1) {
                return {};
            }
            if (tile.x == 2) {
                throw std::runtime_error("synthetic loader failure");
            }
            return QByteArrayLiteral("downloaded");
        },
        [&progressValues](int done, int total) {
            progressValues.append(qMakePair(done, total));
        });

    QCOMPARE(result.total, 53);
    QCOMPARE(result.downloaded, 51);
    QCOMPARE(result.failed, 2);
    const QVector<QPair<int, int>> expectedProgress{
        {25, 53}, {50, 53}, {53, 53}};
    QCOMPARE(progressValues, expectedProgress);
}

void MapPrefetchServiceTest::cancellationStopsLaunchingTilesAndKeepsCompletedFiles()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    std::atomic<bool> stop{false};
    std::atomic<int> loads{0};
    MapPrefetchService service(root.path());
    const MapPrefetchResult result = service.Prefetch(
        core::MapType::ArcGIS_Satellite, sequentialTiles(100),
        [&stop, &loads](core::MapType::Types, const MapTileInfo &,
                        const MapPrefetchService::CancellationCallback &) {
            const int current = ++loads;
            if (current == 5) {
                stop = true;
            }
            QThread::msleep(2);
            return QByteArrayLiteral("completed-before-cancel");
        }, {}, [&stop]() { return stop.load(); });

    QVERIFY(result.cancelled);
    QVERIFY(loads.load() >= 5);
    QVERIFY(loads.load() < result.total);
    QVERIFY(result.downloaded > 0);
    QCOMPARE(result.failed, 0);
    const QString firstPath = core::PureImageCache::sharedTilePath(
        root.path(), core::MapType::ArcGIS_Satellite,
        core::Point(0, 0), 10);
    QVERIFY(QFileInfo(firstPath).size() > 0);
}

void MapPrefetchServiceTest::rejectsUnsupportedProvidersAndDefaultLoaderCacheMode()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    MapPrefetchService service(root.path(), 1);
    MapPrefetchResult result = service.Prefetch(
        core::MapType::GDALCustom, {{5, 1, 1}},
        [](core::MapType::Types, const MapTileInfo &,
           const MapPrefetchService::CancellationCallback &) {
            return QByteArrayLiteral("tile");
        });
    QVERIFY(!result.error.isEmpty());

    const core::AccessMode::Types oldMode =
        core::OPMaps::Instance()->GetAccessMode();
    core::OPMaps::Instance()->setAccessMode(core::AccessMode::CacheOnly);
    result = service.Prefetch(core::MapType::OpenStreetMap,
                              {{5, 1, 1}});
    core::OPMaps::Instance()->setAccessMode(oldMode);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.downloaded, 0);
}

QTEST_GUILESS_MAIN(MapPrefetchServiceTest)
#include "test_mapprefetchservice.moc"
