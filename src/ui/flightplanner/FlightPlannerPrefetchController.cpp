#include "FlightPlannerPrefetchController.h"

#include "FlightPlannerMissionModel.h"
#include "FlightPlannerViewModel.h"
#include "ui/map/AbstractMapWidget.h"
#include "ui/map/MapTileSourceFactory.h"

#include <QAction>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QRectF>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kEarthRadiusMetres = 6378137.0;
constexpr double kMaximumLatitude = 85.0511287798066;

QPointF toWebMercator(double longitude, double latitude)
{
    longitude = std::max(-180.0, std::min(180.0, longitude));
    latitude = std::max(-kMaximumLatitude,
                        std::min(kMaximumLatitude, latitude));
    const double latitudeRadians = latitude * kPi / 180.0;
    return QPointF(
        kEarthRadiusMetres * longitude * kPi / 180.0,
        kEarthRadiusMetres
            * std::log(std::tan(kPi / 4.0 + latitudeRadians / 2.0)));
}

QVector<QRectF> visibleWebMercatorExtents(AbstractMapWidget *map)
{
    if (!map) {
        return {};
    }
    const MapGeoBounds visible = map->VisibleTileExtent();
    if (!visible.IsValid()) {
        return {};
    }
    const auto extent = [&visible](double west, double east) {
        const QPointF southWest = toWebMercator(
            west, visible.bottom);
        const QPointF northEast = toWebMercator(
            east, visible.top);
        return QRectF(southWest, northEast).normalized();
    };
    if (visible.CrossesDateLine()) {
        return {extent(visible.left, 180.0),
                extent(-180.0, visible.right)};
    }
    return {extent(visible.left, visible.right)};
}

QVector<QPointF> flightPath(FlightPlannerViewModel *viewModel)
{
    QVector<QPointF> path;
    if (!viewModel || !viewModel->Waypoints()) {
        return path;
    }
    FlightPlannerMissionModel *model = viewModel->Waypoints();
    const QVector<WpRowData> rows = model->rows(model->missionStore());
    path.reserve(rows.size());
    for (const WpRowData &row : rows) {
        if (MissionRoute::IsFlightPath(row.Command)
            && (row.Lat != 0.0 || row.Lng != 0.0)) {
            path.append(QPointF(row.Lng, row.Lat));
        }
    }
    return path;
}
} // namespace

FlightPlannerPrefetchController::FlightPlannerPrefetchController(
    AbstractMapWidget *map,
    FlightPlannerViewModel *viewModel,
    QObject *parent)
    : QObject(parent),
      m_map(map),
      m_viewModel(viewModel)
{
    if (!m_map) {
        return;
    }

    QWidget *mapWidget = m_map->Widget();
    if (!mapWidget) {
        return;
    }

    auto *separator = new QAction(m_map);
    separator->setSeparator(true);
    separator->setObjectName(QStringLiteral("MapPrefetchSeparator"));
    mapWidget->addAction(separator);

    m_prefetchVisibleAreaAction = new QAction(
        tr("Prefetch"), m_map);
    m_prefetchVisibleAreaAction->setObjectName(
        QStringLiteral("prefetchToolStripMenuItem"));
    mapWidget->addAction(m_prefetchVisibleAreaAction);
    connect(m_prefetchVisibleAreaAction, &QAction::triggered,
            this, &FlightPlannerPrefetchController::PrefetchVisibleArea);

    m_prefetchWaypointPathAction = new QAction(
        tr("Prefetch WP Path"), m_map);
    m_prefetchWaypointPathAction->setObjectName(
        QStringLiteral("prefetchWPPathToolStripMenuItem"));
    mapWidget->addAction(m_prefetchWaypointPathAction);
    connect(m_prefetchWaypointPathAction, &QAction::triggered,
            this, &FlightPlannerPrefetchController::PrefetchWaypointPath);
}

FlightPlannerPrefetchController::~FlightPlannerPrefetchController()
{
    if (m_cancel) {
        m_cancel->store(true);
    }
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        m_workerThread->wait();
    }
}

void FlightPlannerPrefetchController::PrefetchVisibleArea()
{
    startPrefetch(false);
}

void FlightPlannerPrefetchController::PrefetchWaypointPath()
{
    startPrefetch(true);
}

bool FlightPlannerPrefetchController::promptZoomRange(
    int *minimumZoom, int *maximumZoom)
{
    if (!minimumZoom || !maximumZoom || !m_map) {
        return false;
    }
    QWidget *mapWidget = m_map->Widget();
    if (!mapWidget) {
        return false;
    }
    const int current = qBound(
        MapPrefetchService::MinimumZoom, m_map->CurrentZoomLevel(),
        MapPrefetchService::MaximumZoom);
    bool accepted = false;
    const QString minimumInput = QInputDialog::getText(
        mapWidget, tr("Tile prefetch"), tr("Minimum zoom (1-21)"),
        QLineEdit::Normal, QString::number(current), &accepted);
    if (!accepted) {
        return false;
    }
    const QString maximumInput = QInputDialog::getText(
        mapWidget, tr("Tile prefetch"), tr("Maximum zoom (1-21)"),
        QLineEdit::Normal,
        QString::number(qMin(current + 2,
                             MapPrefetchService::MaximumZoom)),
        &accepted);
    if (!accepted) {
        return false;
    }

    bool minimumOk = false;
    bool maximumOk = false;
    const int minimum = minimumInput.toInt(&minimumOk);
    const int maximum = maximumInput.toInt(&maximumOk);
    if (!minimumOk || !maximumOk
        || minimum < MapPrefetchService::MinimumZoom
        || minimum > MapPrefetchService::MaximumZoom
        || maximum < MapPrefetchService::MinimumZoom
        || maximum > MapPrefetchService::MaximumZoom
        || minimum > maximum) {
        if (m_viewModel) {
            m_viewModel->setStatus(
                tr("Invalid tile-prefetch zoom range."));
        }
        return false;
    }
    *minimumZoom = minimum;
    *maximumZoom = maximum;
    return true;
}

void FlightPlannerPrefetchController::startPrefetch(bool pathOnly)
{
    if (!m_viewModel || !m_map) {
        return;
    }
    if (m_running) {
        m_viewModel->setStatus(
            tr("Tile prefetch is already running."));
        return;
    }

    int minimumZoom = 0;
    int maximumZoom = 0;
    if (!promptZoomRange(&minimumZoom, &maximumZoom)) {
        return;
    }

    const QVector<MapTileInfo> tiles = pathOnly
        ? MapPrefetchService::PathTiles(
              flightPath(m_viewModel), minimumZoom, maximumZoom)
        : MapPrefetchService::AreaTiles(
              visibleWebMercatorExtents(m_map),
              minimumZoom, maximumZoom);
    if (tiles.isEmpty()) {
        m_viewModel->setStatus(pathOnly
            ? tr("No waypoint path to prefetch.")
            : tr("The visible map area is empty."));
        return;
    }
    if (tiles.size() > MapPrefetchService::MaximumTileCount) {
        m_viewModel->setStatus(
            tr("Tile prefetch has %1 tiles; reduce the area or zoom range.")
                .arg(tiles.size()));
        return;
    }
    if (tiles.size() > 2000
        && QMessageBox::question(
               m_map->Widget(), tr("Tile prefetch"),
               tr("Download and cache %1 map tiles?").arg(tiles.size()),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) != QMessageBox::Yes) {
        m_viewModel->setStatus(tr("Tile prefetch cancelled."));
        return;
    }

    runPrefetch(m_map->CurrentMapType(), tiles);
}

void FlightPlannerPrefetchController::runPrefetch(
    core::MapType::Types mapType,
    const QVector<MapTileInfo> &tiles)
{
    if (m_running || !m_viewModel || !m_map) {
        return;
    }
    m_running = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);

    auto *thread = new QThread(this);
    auto *worker = new QObject;
    worker->moveToThread(thread);
    m_workerThread = thread;

    const QPointer<FlightPlannerPrefetchController> guard(this);
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    const auto result = std::make_shared<MapPrefetchResult>();
    const QString cacheRoot = m_map->SharedCacheRoot();
    connect(thread, &QThread::started, worker,
            [guard, cancel, result, thread, mapType, tiles, cacheRoot]() {
        MapPrefetchService service(cacheRoot);
        *result = service.Prefetch(
            mapType, tiles, {},
            [guard](int done, int total) {
                if (!guard) {
                    return;
                }
                QMetaObject::invokeMethod(
                    guard.data(), [guard, done, total]() {
                        if (guard) {
                            guard->reportProgress(done, total);
                        }
                    }, Qt::QueuedConnection);
            },
            [cancel]() { return cancel->load(); });
        thread->quit();
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this,
            [this, mapType, result]() {
        // Keep m_running true until the worker event loop has actually
        // stopped. This prevents a second job from replacing the only
        // thread handle while the first QThread is still alive.
        finishPrefetch(mapType, *result);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void FlightPlannerPrefetchController::reportProgress(int done, int total)
{
    if (m_running && m_viewModel) {
        m_viewModel->setStatus(
            tr("Prefetching map tiles: %1/%2…").arg(done).arg(total));
    }
}

void FlightPlannerPrefetchController::finishPrefetch(
    core::MapType::Types mapType,
    const MapPrefetchResult &result)
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_cancel.reset();
    if (!m_viewModel) {
        return;
    }
    if (result.cancelled) {
        m_viewModel->setStatus(tr("Tile prefetch cancelled."));
        return;
    }
    if (!result.error.isEmpty()) {
        m_viewModel->setStatus(
            tr("Tile prefetch failed: %1").arg(result.error));
        return;
    }
    if (result.downloaded > 0) {
        MapTileSourceFactory::instance()->InvalidateMapType(mapType);
    }
    m_viewModel->setStatus(
        tr("Tile prefetch complete: %1/%2 cached%3")
            .arg(result.downloaded)
            .arg(result.total)
            .arg(result.failed > 0
                ? tr(", %1 failed.").arg(result.failed)
                : QStringLiteral(".")));
}
