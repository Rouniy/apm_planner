#include "OPMapBackendWidget.h"

#include "MapWidgetFactory.h"
#include "QGCMapWidget.h"
#include "cache.h"
#include "gpsitem.h"
#include "pointlatlng.h"

#include <QDir>

OPMapBackendWidget::OPMapBackendWidget(
    MapWidgetRole role, const QString &sharedCacheRoot,
    QWidget *widgetParent, QObject *parent)
    : AbstractMapWidget(parent),
      m_map(new QGCMapWidget(
          SettingsGroup(role),
          role != MapWidgetRole::LogAnalysis
              && role != MapWidgetRole::Preview,
          widgetParent))
{
    Q_ASSERT(QDir::cleanPath(sharedCacheRoot)
             == QDir::cleanPath(SharedCacheRoot()));
    connect(m_map, QOverload<int>::of(&QGCMapWidget::zoomChanged),
            this, &AbstractMapWidget::ZoomChanged);
    connect(m_map, &QGCMapWidget::OnTileLoadStart,
            this, &AbstractMapWidget::TileLoadStarted);
    connect(m_map, &QGCMapWidget::OnTileLoadComplete,
            this, &AbstractMapWidget::TileLoadCompleted);
    connect(m_map, &QGCMapWidget::OnTilesStillToLoad,
            this, &AbstractMapWidget::TilesStillToLoad);
    connect(m_map, &QGCMapWidget::plannerCoordinateRequested,
            this, &AbstractMapWidget::PlannerCoordinateRequested);
    connect(m_map, &QGCMapWidget::plannerContextMenuRequested,
            this, &AbstractMapWidget::PlannerContextMenuRequested);
    connect(m_map, &QGCMapWidget::plannerWaypointMoved,
            this, &AbstractMapWidget::PlannerWaypointMoved);
}

OPMapBackendWidget::~OPMapBackendWidget()
{
    delete m_map.data();
}

void OPMapBackendWidget::RegisterBackend()
{
    MapWidgetFactory::instance()->RegisterBackend(
        QString::fromLatin1(MapWidgetFactory::OPMapBackendId),
        tr("OPMap"),
        QString::fromLatin1(MapWidgetFactory::SharedCacheContract),
        [](MapWidgetRole role, const QString &sharedCacheRoot,
           QWidget *widgetParent, QObject *owner) {
            return new OPMapBackendWidget(
                role, sharedCacheRoot, widgetParent, owner);
        });
}

QString OPMapBackendWidget::SettingsGroup(MapWidgetRole role)
{
    switch (role) {
    case MapWidgetRole::FlightPlanner:
        return QStringLiteral("QGC_MAPWIDGET/FlightPlanner");
    case MapWidgetRole::Simulation:
        return QStringLiteral("QGC_MAPWIDGET/Simulation");
    case MapWidgetRole::LogAnalysis:
        return QStringLiteral("QGC_MAPWIDGET/LogAnalysis");
    case MapWidgetRole::Preview:
        return QStringLiteral("QGC_MAPWIDGET/Preview");
    case MapWidgetRole::FlightData:
    default:
        return QStringLiteral("QGC_MAPWIDGET/FlightData");
    }
}

QString OPMapBackendWidget::BackendId() const
{
    return QString::fromLatin1(MapWidgetFactory::OPMapBackendId);
}

QString OPMapBackendWidget::SharedCacheRoot() const
{
    return core::Cache::Instance()->ImageCache.sharedCacheRootPath();
}

QWidget *OPMapBackendWidget::Widget() const { return m_map; }
QGCMapWidget *OPMapBackendWidget::OPMapWidget() const { return m_map; }
int OPMapBackendWidget::MinZoom() const { return m_map ? m_map->MinZoom() : 1; }
int OPMapBackendWidget::MaxZoom() const { return m_map ? m_map->MaxZoom() : 21; }
double OPMapBackendWidget::ZoomReal() const { return m_map ? m_map->ZoomReal() : 1.0; }
int OPMapBackendWidget::CurrentZoomLevel() const { return m_map ? m_map->CurrentZoomLevel() : 1; }
QSize OPMapBackendWidget::ViewportPixelSize() const
{
    return m_map && m_map->viewport() ? m_map->viewport()->size() : QSize();
}
MapGeoBounds OPMapBackendWidget::VisibleTileExtent() const
{
    if (!m_map) {
        return {};
    }
    const internals::RectLatLng extent = m_map->VisibleTileExtent();
    return {extent.Left(), extent.Top(), extent.Right(), extent.Bottom()};
}
MapCoordinate OPMapBackendWidget::CurrentPosition() const
{
    if (!m_map) {
        return {};
    }
    const internals::PointLatLng position = m_map->CurrentPosition();
    return {position.Lat(), position.Lng(), 0.0};
}
core::MapType::Types OPMapBackendWidget::CurrentMapType() const { return m_map ? m_map->GetMapType() : core::MapType::GoogleSatellite; }
bool OPMapBackendWidget::FollowUAVEnabled() const { return m_map && m_map->getFollowUAVEnabled(); }
float OPMapBackendWidget::UpdateRateLimit() const { return m_map ? m_map->getUpdateRateLimit() : 0.0f; }
int OPMapBackendWidget::TrailType() const { return m_map ? m_map->getTrailType() : 0; }
float OPMapBackendWidget::TrailInterval() const { return m_map ? m_map->getTrailInterval() : 0.0f; }

void OPMapBackendWidget::SetZoom(double zoom) { if (m_map) m_map->SetZoom(zoom); }
void OPMapBackendWidget::SetCurrentPosition(double latitude, double longitude) { if (m_map) m_map->SetCurrentPosition(internals::PointLatLng(latitude, longitude)); }
void OPMapBackendWidget::SetAcceleratedRenderingEnabled(bool enabled) { if (m_map) m_map->SetUseOpenGL(enabled); }
void OPMapBackendWidget::SetFollowUAVEnabled(bool enabled) { if (m_map) m_map->setFollowUAVEnabled(enabled); }
void OPMapBackendWidget::SetTrailModeTimed(int seconds) { if (m_map) m_map->setTrailModeTimed(seconds); }
void OPMapBackendWidget::SetTrailModeDistance(int metres) { if (m_map) m_map->setTrailModeDistance(metres); }
void OPMapBackendWidget::DeleteTrails() { if (m_map) m_map->deleteTrails(); }
void OPMapBackendWidget::SetUpdateRateLimit(float seconds) { if (m_map) m_map->setUpdateRateLimit(seconds); }
void OPMapBackendWidget::ShowGoToDialog() { if (m_map) m_map->showGoToDialog(); }
void OPMapBackendWidget::GoHome() { if (m_map) m_map->goHome(); }
void OPMapBackendWidget::LastPosition() { if (m_map) m_map->lastPosition(); }
void OPMapBackendWidget::CacheVisibleRegion() { if (m_map) m_map->cacheVisibleRegion(); }
void OPMapBackendWidget::UpdateHomePosition(double latitude, double longitude, double altitude) { if (m_map) m_map->updateHomePosition(latitude, longitude, altitude); }
void OPMapBackendWidget::SetMovingBase(const MapCoordinate &position,
                                       const QString &tag)
{
    if (m_map) {
        m_map->setMovingBase(position, tag);
    }
}
void OPMapBackendWidget::ClearMovingBase()
{
    if (m_map) {
        m_map->clearMovingBase();
    }
}
void OPMapBackendWidget::SetPropagationRaster(
    const QImage &image, const MapGeoBounds &bounds)
{
    if (m_map) {
        m_map->setPropagationRaster(image, bounds);
    }
}
void OPMapBackendWidget::ClearPropagationRaster()
{
    if (m_map) {
        m_map->clearPropagationRaster();
    }
}
void OPMapBackendWidget::SetPropagationContour(
    const MapOverlayPolyline &contour)
{
    if (m_map) {
        m_map->setPropagationContour(contour);
    }
}
void OPMapBackendWidget::ClearPropagationContour()
{
    if (m_map) {
        m_map->clearPropagationContour();
    }
}
void OPMapBackendWidget::SetPropagationRings(
    const QVector<MapOverlayPolyline> &rings)
{
    if (m_map) {
        m_map->setPropagationRings(rings);
    }
}
void OPMapBackendWidget::ClearPropagationRings()
{
    if (m_map) {
        m_map->clearPropagationRings();
    }
}
void OPMapBackendWidget::SetPropagationStatus(
    const QString &legend, const QString &status)
{
    if (m_map) {
        m_map->setPropagationStatus(legend, status);
    }
}
void OPMapBackendWidget::SetMissionPlanningEnabled(bool enabled) { if (m_map) m_map->setMissionPlanningEnabled(enabled); }
void OPMapBackendWidget::SetPlannerRows(const QVector<WpRowData> &rows, FlightPlannerMissionModel::MissionStore store) { if (m_map) m_map->setPlannerRows(rows, store); }
void OPMapBackendWidget::SetPlannerAltitudePresentation(double multiplier, const QString &unit) { if (m_map) m_map->setPlannerAltitudePresentation(multiplier, unit); }
void OPMapBackendWidget::SetPlannerNavigationParameters(const FlightPlannerNavigationParameters &parameters) { if (m_map) m_map->setPlannerNavigationParameters(parameters); }
void OPMapBackendWidget::SetPlannerDrawnPolygon(const QVector<MapCoordinate> &points) { if (m_map) m_map->setPlannerDrawnPolygon(points); }
void OPMapBackendWidget::SetPlannerMeasurement(const QVector<MapCoordinate> &points) { if (m_map) m_map->setPlannerMeasurement(points); }
void OPMapBackendWidget::SetPlannerHome(double latitude, double longitude, double altitude) { if (m_map) m_map->setPlannerHome(latitude, longitude, altitude); }
void OPMapBackendWidget::ClearPlannerHome() { if (m_map) m_map->clearPlannerHome(); }
void OPMapBackendWidget::SetPlannerSelection(int sequence) { if (m_map) m_map->setPlannerSelection(sequence); }
void OPMapBackendWidget::SetLogTrail(const QVector<MapCoordinate> &points)
{
    if (!m_map) {
        return;
    }
    int firstValid = -1;
    for (int i = 0; i < points.size(); ++i) {
        if (points.at(i).IsValid()) {
            firstValid = i;
            break;
        }
    }
    if (firstValid < 0) {
        if (m_logTrail) {
            m_logTrail->DeleteTrail();
        }
        if (m_logCursor) {
            m_logCursor->ShowUavPic(false);
        }
        return;
    }
    if (!m_logTrail) {
        m_logTrail = m_map->AddTrail();
    }
    m_logTrail->DeleteTrail();
    m_logTrail->SetTrailDistance(0);
    m_logTrail->SetTrailType(mapcontrol::UAVTrailType::ByDistance);
    m_logTrail->ShowUavPic(false);
    SetCurrentPosition(points.at(firstValid).latitude,
                       points.at(firstValid).longitude);
    for (const MapCoordinate &point : points) {
        if (!point.IsValid()) {
            continue;
        }
        m_logTrail->SetUAVPos(
            internals::PointLatLng(point.latitude, point.longitude),
            qRound(point.altitude));
    }
    m_logTrail->SetShowTrail(false);
    m_logTrail->SetShowTrailLine(true);
    m_logTrail->RefreshPos();
    if (!m_logCursor) {
        m_logCursor = m_map->AddTrailCursor();
        m_logCursor->SetTrailType(mapcontrol::UAVTrailType::NoTrail);
    }
    m_logCursor->ShowUavPic(true);
    SetLogCursor(points.at(firstValid), 0.0);
}

void OPMapBackendWidget::SetLogCursor(
    const MapCoordinate &position, double heading)
{
    if (!m_map || !position.IsValid() || !std::isfinite(heading)) {
        return;
    }
    if (!m_logCursor) {
        m_logCursor = m_map->AddTrailCursor();
        m_logCursor->SetTrailType(mapcontrol::UAVTrailType::NoTrail);
    }
    m_logCursor->ShowUavPic(true);
    m_logCursor->SetUAVPos(
        internals::PointLatLng(position.latitude, position.longitude),
        qRound(position.altitude));
    m_logCursor->SetUAVHeading(heading);
    m_logCursor->RefreshPos();
}
