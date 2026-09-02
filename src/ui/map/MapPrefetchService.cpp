#include "MapPrefetchService.h"

#include "accessmode.h"
#include "opmaps.h"
#include "point.h"
#include "pureimagecache.h"
#include "urlfactory.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kEarthRadiusMetres = 6378137.0;
constexpr double kMaximumLatitude = 85.0511287798066;
constexpr double kOriginShift = 20037508.342789244;
constexpr double kWorldWidth = kOriginShift * 2.0;
constexpr double kTilePixels = 256.0;
constexpr qint64 kMaximumNetworkTileBytes = 32LL * 1024LL * 1024LL;

bool cancelled(const MapPrefetchService::CancellationCallback &callback)
{
    return callback && callback();
}

QPointF toWebMercator(const QPointF &longitudeLatitude)
{
    const double longitude = std::max(-180.0,
                                      std::min(180.0, longitudeLatitude.x()));
    const double latitude = std::max(-kMaximumLatitude,
                                     std::min(kMaximumLatitude,
                                              longitudeLatitude.y()));
    const double latitudeRadians = latitude * kPi / 180.0;
    const double x = kEarthRadiusMetres * longitude * kPi / 180.0;
    const double y = kEarthRadiusMetres
        * std::log(std::tan(kPi / 4.0 + latitudeRadians / 2.0));
    return QPointF(x, y);
}

int tileCoordinate(double value, int zoom, bool isY)
{
    const int count = 1 << zoom;
    const double normalized = isY
        ? (kOriginShift - value) / kWorldWidth
        : (value + kOriginShift) / kWorldWidth;
    const int coordinate = static_cast<int>(std::floor(normalized * count));
    return std::max(0, std::min(count - 1, coordinate));
}

bool insertExtentTiles(const QRectF &extent,
                       int zoom,
                       std::set<MapTileInfo> *tiles)
{
    if (!tiles) {
        return true;
    }

    const QRectF normalized = extent.normalized();
    if (!normalized.isValid()) {
        return true;
    }
    const double west = std::max(-kOriginShift,
                                 std::min(kOriginShift, normalized.left()));
    const double east = std::max(-kOriginShift,
                                 std::min(kOriginShift, normalized.right()));
    const double south = std::max(-kOriginShift,
                                  std::min(kOriginShift, normalized.top()));
    const double north = std::max(-kOriginShift,
                                  std::min(kOriginShift, normalized.bottom()));

    const int minimumX = tileCoordinate(west, zoom, false);
    const int maximumX = tileCoordinate(east, zoom, false);
    const int minimumY = tileCoordinate(north, zoom, true);
    const int maximumY = tileCoordinate(south, zoom, true);
    for (int x = minimumX; x <= maximumX; ++x) {
        for (int y = minimumY; y <= maximumY; ++y) {
            tiles->insert({zoom, x, y});
            if (static_cast<int>(tiles->size())
                >= MapPrefetchService::EnumerationLimit) {
                return false;
            }
        }
    }
    return true;
}

double wrappedWebMercatorX(double x)
{
    double wrapped = std::fmod(x + kOriginShift, kWorldWidth);
    if (wrapped < 0.0) {
        wrapped += kWorldWidth;
    }
    return wrapped - kOriginShift;
}

bool insertCorridorTiles(double x, double y, double corridor, int zoom,
                         std::set<MapTileInfo> *tiles)
{
    const double center = wrappedWebMercatorX(x);
    const double west = center - corridor;
    const double east = center + corridor;
    const auto insert = [y, corridor, zoom, tiles](double left,
                                                   double right) {
        return insertExtentTiles(
            QRectF(QPointF(left, y - corridor),
                   QPointF(right, y + corridor)),
            zoom, tiles);
    };
    if (west < -kOriginShift) {
        return insert(west + kWorldWidth, kOriginShift)
            && insert(-kOriginShift, east);
    }
    if (east > kOriginShift) {
        return insert(west, kOriginShift)
            && insert(-kOriginShift, east - kWorldWidth);
    }
    return insert(west, east);
}

QVector<MapTileInfo> vectorFromSet(const std::set<MapTileInfo> &tiles)
{
    QVector<MapTileInfo> result;
    result.reserve(static_cast<int>(tiles.size()));
    for (const MapTileInfo &tile : tiles) {
        result.append(tile);
    }
    return result;
}

QVector<MapTileInfo> normalizedTiles(const QVector<MapTileInfo> &input)
{
    std::set<MapTileInfo> unique;
    for (const MapTileInfo &tile : input) {
        if (tile.zoom < MapPrefetchService::MinimumZoom
            || tile.zoom > MapPrefetchService::MaximumZoom) {
            continue;
        }
        const int dimension = 1 << tile.zoom;
        if (tile.x < 0 || tile.y < 0
            || tile.x >= dimension || tile.y >= dimension) {
            continue;
        }
        unique.insert(tile);
        if (static_cast<int>(unique.size())
            >= MapPrefetchService::EnumerationLimit) {
            break;
        }
    }
    return vectorFromSet(unique);
}

void setRequestHeaders(QNetworkRequest *request,
                       core::MapType::Types mapType)
{
    request->setRawHeader("User-Agent", "APMPlanner3/3.0");
    request->setRawHeader("Accept", "image/*,*/*;q=0.8");
    if (mapType == core::MapType::GoogleSatellite
        || mapType == core::MapType::GoogleHybrid) {
        request->setRawHeader("Referer", "https://maps.google.com/");
    } else if (mapType == core::MapType::BingSatellite) {
        request->setRawHeader("Referer", "https://www.bing.com/maps/");
    }
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    request->setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                          QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
}
} // namespace

bool MissionRoute::IsNavigation(quint16 command)
{
    return (command >= 16 && command <= 94 && command != 80)
        || command == 189;
}

bool MissionRoute::IsFlightPath(quint16 command)
{
    return IsNavigation(command) && command != 189;
}

MapPrefetchService::MapPrefetchService(const QString &cacheRoot,
                                       int networkTimeoutMs)
    : m_cacheRoot(cacheRoot.isEmpty()
          ? core::PureImageCache::sharedCacheRoot() : cacheRoot),
      m_networkTimeoutMs(std::max(1, networkTimeoutMs))
{
}

QVector<MapTileInfo> MapPrefetchService::AreaTiles(
    const QRectF &webMercatorExtent,
    int minimumZoom,
    int maximumZoom)
{
    return AreaTiles(QVector<QRectF>{webMercatorExtent},
                     minimumZoom, maximumZoom);
}

QVector<MapTileInfo> MapPrefetchService::AreaTiles(
    const QVector<QRectF> &webMercatorExtents,
    int minimumZoom,
    int maximumZoom)
{
    std::set<MapTileInfo> tiles;
    const int firstZoom = std::max(MinimumZoom,
                                   std::min(MaximumZoom, minimumZoom));
    const int lastZoom = std::max(MinimumZoom,
                                  std::min(MaximumZoom, maximumZoom));
    if (firstZoom > lastZoom || webMercatorExtents.isEmpty()) {
        return {};
    }

    for (int zoom = firstZoom; zoom <= lastZoom; ++zoom) {
        for (const QRectF &extent : webMercatorExtents) {
            if (!insertExtentTiles(extent, zoom, &tiles)) {
                return vectorFromSet(tiles);
            }
        }
    }
    return vectorFromSet(tiles);
}

QVector<MapTileInfo> MapPrefetchService::PathTiles(
    const QVector<QPointF> &longitudeLatitudePath,
    int minimumZoom,
    int maximumZoom)
{
    std::set<MapTileInfo> tiles;
    const int firstZoom = std::max(MinimumZoom,
                                   std::min(MaximumZoom, minimumZoom));
    const int lastZoom = std::max(MinimumZoom,
                                  std::min(MaximumZoom, maximumZoom));
    if (firstZoom > lastZoom || longitudeLatitudePath.isEmpty()) {
        return {};
    }

    QVector<QPointF> path;
    path.reserve(longitudeLatitudePath.size());
    for (const QPointF &point : longitudeLatitudePath) {
        if (std::isfinite(point.x()) && std::isfinite(point.y())) {
            path.append(toWebMercator(point));
        }
    }
    if (path.isEmpty()) {
        return {};
    }

    for (int zoom = firstZoom; zoom <= lastZoom; ++zoom) {
        const double resolution = kWorldWidth
            / (kTilePixels * static_cast<double>(1 << zoom));
        const double sampleDistance = resolution * 128.0;
        const double corridor = resolution * 64.0;
        const int pathSize = static_cast<int>(path.size());
        const int segmentCount = std::max(1, pathSize - 1);
        for (int segment = 0; segment < segmentCount; ++segment) {
            const QPointF start = path.at(segment);
            const QPointF end = path.at(std::min(segment + 1,
                                                  pathSize - 1));
            double deltaX = end.x() - start.x();
            if (deltaX > kWorldWidth / 2.0) {
                deltaX -= kWorldWidth;
            } else if (deltaX < -kWorldWidth / 2.0) {
                deltaX += kWorldWidth;
            }
            const double deltaY = end.y() - start.y();
            const double distance = std::hypot(deltaX, deltaY);
            const int samples = std::max(
                1, static_cast<int>(std::ceil(distance / sampleDistance)));
            for (int sample = 0; sample <= samples; ++sample) {
                const double fraction = static_cast<double>(sample) / samples;
                const double x = start.x() + deltaX * fraction;
                const double y = start.y() + deltaY * fraction;
                if (!insertCorridorTiles(
                        x, y, corridor, zoom, &tiles)) {
                    return vectorFromSet(tiles);
                }
            }
        }
    }
    return vectorFromSet(tiles);
}

bool MapPrefetchService::IsSupportedMapType(core::MapType::Types mapType)
{
    return mapType == core::MapType::GoogleSatellite
        || mapType == core::MapType::GoogleHybrid
        || mapType == core::MapType::BingSatellite
        || mapType == core::MapType::OpenStreetMap
        || mapType == core::MapType::ArcGIS_Satellite;
}

MapPrefetchResult MapPrefetchService::Prefetch(
    core::MapType::Types mapType,
    const QVector<MapTileInfo> &requestedTiles,
    const TileLoader &loader,
    const ProgressCallback &progress,
    const CancellationCallback &isCancelled) const
{
    MapPrefetchResult result;
    if (!IsSupportedMapType(mapType)) {
        result.error = QStringLiteral("Unsupported map provider.");
        return result;
    }
    if (!loader
        && core::OPMaps::Instance()->GetAccessMode()
            != core::AccessMode::ServerAndCache) {
        result.error = QStringLiteral(
            "Tile prefetch requires the ServerAndCache map-cache mode.");
        return result;
    }

    const QVector<MapTileInfo> tiles = normalizedTiles(requestedTiles);
    result.total = tiles.size();
    if (requestedTiles.size() > MaximumTileCount
        || tiles.size() > MaximumTileCount) {
        result.error = QStringLiteral(
            "The requested range contains more than 20000 tiles; "
            "reduce the area or zoom range.");
        return result;
    }
    if (tiles.isEmpty()) {
        if (progress) {
            progress(0, 0);
        }
        return result;
    }
    if (cancelled(isCancelled)) {
        result.cancelled = true;
        if (progress) {
            progress(0, result.total);
        }
        return result;
    }

    core::PureImageCache cache(m_cacheRoot);
    const TileLoader effectiveLoader = loader
        ? loader
        : [this](core::MapType::Types type,
                 const MapTileInfo &tile,
                 const CancellationCallback &cancel) {
              return loadFromNetwork(type, tile, cancel);
          };

    std::atomic<int> nextIndex{0};
    std::atomic<int> downloaded{0};
    std::atomic<int> cached{0};
    std::atomic<int> failed{0};
    std::mutex completionMutex;
    std::condition_variable completionChanged;
    int completed = 0;
    const int workerCount = std::min(
        MaximumConcurrentLoads, static_cast<int>(tiles.size()));
    int workersAlive = workerCount;

    auto worker = [&]() {
        while (!cancelled(isCancelled)) {
            const int index = nextIndex.fetch_add(1);
            if (index >= tiles.size()) {
                break;
            }
            const MapTileInfo tile = tiles.at(index);
            const core::Point position(tile.x, tile.y);
            bool counted = false;
            try {
                if (!cache.GetImageFromCache(mapType, position,
                                             tile.zoom).isEmpty()) {
                    // Mission Planner reports any non-empty provider result
                    // as downloaded, including a persistent-cache hit.
                    ++downloaded;
                    ++cached;
                    counted = true;
                } else if (!cancelled(isCancelled)) {
                    const QByteArray bytes = effectiveLoader(
                        mapType, tile, isCancelled);
                    if (!bytes.isEmpty()) {
                        if (cache.replaceSharedTile(bytes, mapType,
                                                    position, tile.zoom)) {
                            ++downloaded;
                        } else {
                            ++failed;
                        }
                        counted = true;
                    } else if (!cancelled(isCancelled)) {
                        ++failed;
                        counted = true;
                    }
                }
            } catch (const std::exception &) {
                if (!cancelled(isCancelled)) {
                    ++failed;
                    counted = true;
                }
            } catch (...) {
                if (!cancelled(isCancelled)) {
                    ++failed;
                    counted = true;
                }
            }

            if (counted) {
                {
                    std::lock_guard<std::mutex> lock(completionMutex);
                    ++completed;
                }
                completionChanged.notify_one();
            }
        }
        {
            std::lock_guard<std::mutex> lock(completionMutex);
            --workersAlive;
        }
        completionChanged.notify_one();
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(workerCount));
    for (int index = 0; index < workerCount; ++index) {
        workers.emplace_back(worker);
    }

    int observedCompleted = 0;
    int nextProgress = 25;
    int lastReported = -1;
    for (;;) {
        int currentCompleted = 0;
        int currentWorkers = 0;
        {
            std::unique_lock<std::mutex> lock(completionMutex);
            completionChanged.wait(lock, [&]() {
                return completed != observedCompleted || workersAlive == 0;
            });
            currentCompleted = completed;
            currentWorkers = workersAlive;
        }
        observedCompleted = currentCompleted;
        while (progress && nextProgress <= currentCompleted) {
            progress(nextProgress, result.total);
            lastReported = nextProgress;
            nextProgress += 25;
        }
        if (currentWorkers == 0) {
            break;
        }
    }

    for (std::thread &thread : workers) {
        thread.join();
    }

    result.downloaded = downloaded.load();
    result.cached = cached.load();
    result.failed = failed.load();
    result.cancelled = cancelled(isCancelled)
        && completed < result.total;
    if (progress && lastReported != completed) {
        progress(completed, result.total);
    }
    return result;
}

QByteArray MapPrefetchService::loadFromNetwork(
    core::MapType::Types mapType,
    const MapTileInfo &tile,
    const CancellationCallback &isCancelled) const
{
    if (cancelled(isCancelled)) {
        return {};
    }

    core::UrlFactory urls;
    const QString url = urls.MakeImageUrl(
        mapType, core::Point(tile.x, tile.y), tile.zoom,
        QStringLiteral("en"));
    if (url.isEmpty()) {
        return {};
    }

    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(url)};
    setRequestHeaders(&request, mapType);
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QTimer cancellationPoll;
    cancellationPoll.setInterval(50);
    bool timedOut = false;
    bool wasCancelled = false;
    bool tooLarge = false;

    QObject::connect(reply, &QNetworkReply::finished,
                     &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        reply->abort();
        loop.quit();
    });
    QObject::connect(&cancellationPoll, &QTimer::timeout, &loop, [&]() {
        if (cancelled(isCancelled)) {
            wasCancelled = true;
            reply->abort();
            loop.quit();
        }
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
        if (reply->bytesAvailable() > kMaximumNetworkTileBytes) {
            tooLarge = true;
            reply->abort();
            loop.quit();
        }
    });

    timeout.start(m_networkTimeoutMs);
    cancellationPoll.start();
    loop.exec();
    timeout.stop();
    cancellationPoll.stop();

    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool httpSuccess = status >= 200 && status < 300;
    const bool networkSuccess = reply->error() == QNetworkReply::NoError;
    QByteArray bytes;
    if (!timedOut && !wasCancelled && !tooLarge
        && httpSuccess && networkSuccess) {
        bytes = reply->readAll();
        if (bytes.size() > kMaximumNetworkTileBytes) {
            bytes.clear();
        }
    }
    reply->deleteLater();
    return bytes;
}
