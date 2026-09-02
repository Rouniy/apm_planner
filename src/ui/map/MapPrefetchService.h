#ifndef MAPPREFETCHSERVICE_H
#define MAPPREFETCHSERVICE_H

#include "maptype.h"

#include <QByteArray>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <functional>

struct MapTileInfo
{
    int zoom = 0;
    int x = 0;
    int y = 0;

    bool operator==(const MapTileInfo &other) const
    {
        return zoom == other.zoom && x == other.x && y == other.y;
    }

    bool operator<(const MapTileInfo &other) const
    {
        if (zoom != other.zoom) {
            return zoom < other.zoom;
        }
        if (x != other.x) {
            return x < other.x;
        }
        return y < other.y;
    }
};

struct MapPrefetchResult
{
    int total = 0;
    // Matches Mission Planner: a non-empty persistent-cache hit and a newly
    // fetched tile both count as downloaded/available.
    int downloaded = 0;
    // Diagnostic subset of downloaded: tiles already present on disk.
    int cached = 0;
    int failed = 0;
    bool cancelled = false;
    QString error;
};

// Mission Planner treats DO_LAND_START as a positioned navigation marker,
// but it is not part of the route flown by the vehicle.
class MissionRoute final
{
public:
    static bool IsNavigation(quint16 command);
    static bool IsFlightPath(quint16 command);

private:
    MissionRoute() = delete;
};

class MapPrefetchService final
{
public:
    using CancellationCallback = std::function<bool()>;
    using ProgressCallback = std::function<void(int done, int total)>;
    using TileLoader = std::function<QByteArray(
        core::MapType::Types mapType,
        const MapTileInfo &tile,
        const CancellationCallback &isCancelled)>;

    static constexpr int MinimumZoom = 1;
    static constexpr int MaximumZoom = 21;
    static constexpr int MaximumTileCount = 20000;
    static constexpr int EnumerationLimit = MaximumTileCount + 1;
    static constexpr int MaximumConcurrentLoads = 4;

    explicit MapPrefetchService(const QString &cacheRoot = QString(),
                                int networkTimeoutMs = 30000);

    // The extent uses EPSG:3857 metres. QRectF::left/right are west/east;
    // top/bottom ordering is irrelevant because the rectangle is normalized.
    static QVector<MapTileInfo> AreaTiles(
        const QRectF &webMercatorExtent,
        int minimumZoom,
        int maximumZoom);
    static QVector<MapTileInfo> AreaTiles(
        const QVector<QRectF> &webMercatorExtents,
        int minimumZoom,
        int maximumZoom);

    // A path point is QPointF(longitude, latitude), in WGS84 degrees.
    // Samples are spaced by 128 screen pixels and use a +/-64-pixel
    // WebMercator corridor at every requested zoom level.
    static QVector<MapTileInfo> PathTiles(
        const QVector<QPointF> &longitudeLatitudePath,
        int minimumZoom,
        int maximumZoom);

    // This is deliberately synchronous: call it from one outer worker
    // thread. The supplied loader and cancellation callback must be
    // thread-safe because up to four invocations run concurrently. Progress
    // is delivered on the calling thread. A default loader uses Qt Network
    // and the OPMap URL factory.
    MapPrefetchResult Prefetch(
        core::MapType::Types mapType,
        const QVector<MapTileInfo> &tiles,
        const TileLoader &loader = TileLoader(),
        const ProgressCallback &progress = ProgressCallback(),
        const CancellationCallback &isCancelled = CancellationCallback()) const;

    static bool IsSupportedMapType(core::MapType::Types mapType);

private:
    QByteArray loadFromNetwork(
        core::MapType::Types mapType,
        const MapTileInfo &tile,
        const CancellationCallback &isCancelled) const;

    QString m_cacheRoot;
    int m_networkTimeoutMs = 30000;
};

#endif // MAPPREFETCHSERVICE_H
