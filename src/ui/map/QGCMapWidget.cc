#include "QGCMapWidget.h"
#include "logging.h"
#include "QGCMapToolBar.h"
#include "MapTileSourceFactory.h"
#include "PlannerMeasurementOverlay.h"
#include "UASInterface.h"
#include "UASManager.h"
#include "MAV2DIcon.h"
#include "Waypoint2DIcon.h"
#include "UASWaypointManager.h"
#include "ArduPilotMegaMAV.h"
#include "WaypointNavigation.h"
#include <QContextMenuEvent>
#include <QGraphicsPathItem>
#include <QInputDialog>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace
{
bool isPlannerCoordinateValid(double latitude, double longitude)
{
    return qIsFinite(latitude) && qIsFinite(longitude)
            && latitude >= -90.0 && latitude <= 90.0
            && longitude >= -180.0 && longitude <= 180.0;
}
}

QGCMapWidget::QGCMapWidget(const QString &settingsGroup,
                           bool liveVehicleEnabled, QWidget *parent) :
    mapcontrol::OPMapWidget(parent),
    firingWaypointChange(NULL),
    maxUpdateInterval(2.1f), // 2 seconds
    followUAVEnabled(false),
    trailType(mapcontrol::UAVTrailType::ByTimeElapsed),
    trailInterval(2.0f),
    followUAVID(0),
    mapInitialized(false),
    homeAltitude(0),
    uas(NULL),
    m_liveVehicleEnabled(liveVehicleEnabled),
    m_settingsGroup(settingsGroup)
{
    currWPManager = m_liveVehicleEnabled
        ? UASManager::instance()->getActiveUASWaypointManager()
        : nullptr;
    waypointLines.insert(0, new QGraphicsItemGroup(map));
    m_plannerLineGroup = new QGraphicsItemGroup(map);
    m_plannerLineGroup->setZValue(2.0);
    m_plannerLineGroup->setVisible(false);
    m_plannerMeasurementGroup = new QGraphicsItemGroup(map);
    m_plannerMeasurementGroup->setZValue(6.0);
    m_plannerMeasurementGroup->setVisible(false);
    if (currWPManager) {
        connect(currWPManager, SIGNAL(waypointEditableListChanged(int)), this, SLOT(updateWaypointList(int)));
        connect(currWPManager, SIGNAL(waypointEditableChanged(int, Waypoint*)), this, SLOT(updateWaypoint(int,Waypoint*)));
        connect(this, SIGNAL(waypointCreated(Waypoint*)), currWPManager, SLOT(addWaypointEditable(Waypoint*)));
        connect(this, SIGNAL(waypointChanged(Waypoint*)), currWPManager, SLOT(notifyOfChangeEditable(Waypoint*)));
    }
    connect(map, SIGNAL(mapChanged()), this, SLOT(redrawWaypointLines()));
    connect(map, &mapcontrol::MapGraphicItem::mapChanged,
            this, &QGCMapWidget::redrawPlannerLines);
    connect(map, &mapcontrol::MapGraphicItem::mapChanged,
            this, &QGCMapWidget::redrawPlannerMeasurement);
    offlineMode = true;
    // Widget is inactive until shown
    defaultGuidedRelativeAlt = 100.0; // Default set to 100m
    defaultGuidedAltFirstTimeSet = false;
    connect(MapTileSourceFactory::instance(),
            &MapTileSourceFactory::MapTypeChanged,
            this, &QGCMapWidget::setGlobalMapType);
    connect(MapTileSourceFactory::instance(),
            &MapTileSourceFactory::MapRefreshRequested,
            this, &QGCMapWidget::refreshGlobalMapType);
    loadSettings();

    //handy for debugging:
    //this->SetShowTileGridLines(true);

    this->setContextMenuPolicy(Qt::ActionsContextMenu);

    if (m_liveVehicleEnabled) {
        QAction *guidedaction = new QAction(this);
        guidedaction->setText("Go To Here (Guided Mode)");
        connect(guidedaction,SIGNAL(triggered()),this,SLOT(guidedActionTriggered()));
        this->addAction(guidedaction);
        guidedaction = new QAction(this);
        guidedaction->setText("Go To Here Alt (Guided Mode)");
        connect(guidedaction,SIGNAL(triggered()),this,SLOT(guidedAltActionTriggered()));
        this->addAction(guidedaction);
        QAction *cameraaction = new QAction(this);
        cameraaction->setText("Point Camera Here");
        connect(cameraaction,SIGNAL(triggered()),this,SLOT(cameraActionTriggered()));
        this->addAction(cameraaction);
    }
}

int QGCMapWidget::CurrentZoomLevel() const
{
    return qBound(1, qRound(const_cast<QGCMapWidget *>(this)->ZoomReal()), 21);
}

internals::RectLatLng QGCMapWidget::VisibleTileExtent() const
{
    if (!map || !viewport() || viewport()->width() <= 0
        || viewport()->height() <= 0) {
        return internals::RectLatLng();
    }

    const int right = qMax(0, viewport()->width() - 1);
    const int bottom = qMax(0, viewport()->height() - 1);
    const QVector<QPoint> corners{
        QPoint(0, 0), QPoint(right, 0),
        QPoint(right, bottom), QPoint(0, bottom)
    };
    double north = -90.0;
    double south = 90.0;
    QVector<double> longitudes;
    longitudes.reserve(corners.size());
    for (const QPoint &corner : corners) {
        const QPointF scenePoint = mapToScene(corner);
        const QPointF localPoint = map->mapFromScene(scenePoint);
        const internals::PointLatLng coordinate = map->FromLocalToLatLng(
            qRound(localPoint.x()), qRound(localPoint.y()));
        if (!isPlannerCoordinateValid(coordinate.Lat(), coordinate.Lng())) {
            return internals::RectLatLng();
        }
        north = qMax(north, coordinate.Lat());
        south = qMin(south, coordinate.Lat());
        longitudes.append(coordinate.Lng());
    }
    if (north <= south || longitudes.size() < 2) {
        return internals::RectLatLng();
    }
    // OPMap's Mercator projection clips rather than wraps world X. Preserve
    // the real visible span at low zoom; a largest-gap/minimal-arc algorithm
    // would turn a nearly world-wide viewport into a narrow dateline strip.
    const auto longitudeRange = std::minmax_element(
        longitudes.constBegin(), longitudes.constEnd());
    const double west = *longitudeRange.first;
    const double east = *longitudeRange.second;
    return internals::RectLatLng::FromLTRB(west, north, east, south);
}

void QGCMapWidget::guidedActionTriggered()
{
    if (!uas)
    {
        QMessageBox::information(0,"Error","Please connect first");
        return;
    }
    if (!currWPManager)
        return;
    Waypoint wp;
    double tmpAlt;
    // check the frame has not been changed from the last time we executed
    bool aslAglChanged = defaultGuidedFrame != currWPManager->getFrameRecommendation();

    if ( aslAglChanged || !defaultGuidedAltFirstTimeSet)
    {
        defaultGuidedAltFirstTimeSet = true; // so we don't prompt again.
        QString altFrame;
        defaultGuidedFrame = currWPManager->getFrameRecommendation();

        if (defaultGuidedFrame == MAV_FRAME_GLOBAL_RELATIVE_ALT){
            altFrame = "Relative Alt (AGL)";
            tmpAlt = defaultGuidedRelativeAlt;
        } else {
            altFrame = "Abs Alt (ASL)";
            // Waypoint 0 is always home on APM
            tmpAlt = currWPManager->getWaypoint(0)->getAltitude() + defaultGuidedRelativeAlt;
        }

        bool ok = false;
        tmpAlt = QInputDialog::getDouble(this,altFrame,"Enter " + altFrame + " (in meters) of destination point for guided mode",
                                          tmpAlt,0,30000.0,2,&ok);
        if (!ok)
        {
            //Use has chosen cancel. Do not send the waypoint
            return;
        }

        if (defaultGuidedFrame == MAV_FRAME_GLOBAL_RELATIVE_ALT){
            defaultGuidedRelativeAlt = tmpAlt;
        } else {
            defaultGuidedRelativeAlt = tmpAlt - currWPManager->getWaypoint(0)->getAltitude();
        }
    } else if (defaultGuidedFrame == MAV_FRAME_GLOBAL_RELATIVE_ALT){
        tmpAlt = defaultGuidedRelativeAlt;
    } else {
        tmpAlt = currWPManager->getWaypoint(0)->getAltitude() + defaultGuidedRelativeAlt;
    }
    wp.setFrame(static_cast<MAV_FRAME>(defaultGuidedFrame));
    sendGuidedAction(&wp, tmpAlt);
}

void QGCMapWidget::guidedAltActionTriggered()
{
    if (!uas)
    {
        QMessageBox::information(0,"Error","Please connect first");
        return;
    }
    if (!currWPManager)
        return;

    Waypoint wp;
    double tmpAlt;
    if(  defaultGuidedFrame != currWPManager->getFrameRecommendation()){

        defaultGuidedFrame = currWPManager->getFrameRecommendation();
        QLOG_DEBUG() << "Changing from Frame type to:"
                     << (defaultGuidedFrame == MAV_FRAME_GLOBAL_RELATIVE_ALT? "AGL": "ASL");
    }

    wp.setFrame(static_cast<MAV_FRAME>(defaultGuidedFrame));
    QString altFrame;

    if (wp.getFrame() == MAV_FRAME_GLOBAL_RELATIVE_ALT){
        altFrame = "Relative Alt (AGL)";
        tmpAlt = defaultGuidedRelativeAlt;
    } else {
        altFrame = "Abs Alt (ASL)";
        // Waypoint 0 is always home on APM
        tmpAlt = currWPManager->getWaypoint(0)->getAltitude() + defaultGuidedRelativeAlt;
    }

    bool ok = false;
    tmpAlt = QInputDialog::getDouble(this,altFrame,"Enter " + altFrame + " (in meters) of destination point for guided mode",
                                      tmpAlt,0,30000.0,2,&ok);
    if (!ok)
    {
        //Use has chosen cancel. Do not send the waypoint
        return;
    }
    if (defaultGuidedFrame == MAV_FRAME_GLOBAL_RELATIVE_ALT){
        defaultGuidedRelativeAlt = tmpAlt;
    } else {
        defaultGuidedRelativeAlt = tmpAlt - currWPManager->getWaypoint(0)->getAltitude();
    }
    sendGuidedAction(&wp, tmpAlt);
}

void QGCMapWidget::sendGuidedAction(Waypoint* wp, double alt)
{
    // Create new waypoint and send it to the WPManager to send out.
    internals::PointLatLng pos = map->FromLocalToLatLng(mousePressPos.x(), mousePressPos.y());
    QLOG_DEBUG() << "Guided action requested. Lat:" << pos.Lat() << "Lon:" << pos.Lng()
                 << "Alt:" << alt << "MAV_FRAME:"
                 << (defaultGuidedFrame == MAV_FRAME_GLOBAL_RELATIVE_ALT? "AGL": "ASL");
    wp->setLongitude(pos.Lng());
    wp->setLatitude(pos.Lat());
    wp->setAltitude(alt);
    currWPManager->goToWaypoint(wp);
}

void QGCMapWidget::cameraActionTriggered()
{
    if (!uas)
    {
        QMessageBox::information(0,"Error","Please connect first");
        return;
    }
    ArduPilotMegaMAV *newmav = qobject_cast<ArduPilotMegaMAV*>(this->uas);
    if (newmav)
    {
        newmav->setMountConfigure(4,true,true,true);
        internals::PointLatLng pos = map->FromLocalToLatLng(mousePressPos.x(), mousePressPos.y());
        newmav->setMountControl(pos.Lat(),pos.Lng(),100,true);
    }
}

void QGCMapWidget::mousePressEvent(QMouseEvent *event)
{
    QLOG_DEBUG() << "mousePressEvent pos:" << event->pos() << " posF:" << event->pos();
    mousePressPos = event->pos();
    mapcontrol::OPMapWidget::mousePressEvent(event);
}

void QGCMapWidget::mouseReleaseEvent(QMouseEvent *event)
{
    QLOG_DEBUG() << "mouseReleaseEvent pos:" << event->pos() << " posF:" << event->pos();
    mousePressPos = event->pos();
    mapcontrol::OPMapWidget::mouseReleaseEvent(event);
}

QGCMapWidget::~QGCMapWidget()
{
    clearPlannerGraphics();
    delete m_plannerMeasurementGroup;
    m_plannerMeasurementGroup = nullptr;
    delete m_plannerLineGroup;
    m_plannerLineGroup = nullptr;
    SetShowHome(false);	// doing this appears to stop the map lib crashing on exit
    SetShowUAV(false);	//   "          "
    storeSettings();
}

void QGCMapWidget::showEvent(QShowEvent* event)
{
    // Disable OP's standard UAV, we have more than one
    SetShowUAV(false);
    loadSettings();
    const internals::PointLatLng pos_lat_lon(m_lastLat, m_lastLon);

    // Pass on to parent widget
    OPMapWidget::showEvent(event);

    if (!m_liveVehicleEnabled) {
        SetShowHome(false);
        SetShowUAV(false);
        SetMouseWheelZoomType(
            internals::MouseWheelZoomType::MousePositionWithoutCenter);
        SetFollowMouse(true);
        setFrameStyle(QFrame::NoFrame);
        setBackgroundBrush(QBrush(Qt::black));
        SetCurrentPosition(pos_lat_lon);
        setFocus();
        return;
    }

    if (m_missionPlanningEnabled)
        SetShowHome(false);

    if (!mapInitialized)
    {
        connect(UASManager::instance(), SIGNAL(UASCreated(UASInterface*)), this, SLOT(addUAS(UASInterface*)), Qt::UniqueConnection);
        connect(UASManager::instance(), SIGNAL(activeUASSet(UASInterface*)), this, SLOT(activeUASSet(UASInterface*)), Qt::UniqueConnection);
        connect(UASManager::instance(), SIGNAL(homePositionChanged(double,double,double)), this, SLOT(updateHomePosition(double,double,double)));

        foreach (UASInterface* uas, UASManager::instance()->getUASList())
        {
            addUAS(uas);
        }

        //this->SetUseOpenGL(true);
        SetMouseWheelZoomType(internals::MouseWheelZoomType::MousePositionWithoutCenter);	    // set how the mouse wheel zoom functions
        SetFollowMouse(true);				    // we want a contiuous mouse position reading

        setFrameStyle(QFrame::NoFrame);      // no border frame
        setBackgroundBrush(QBrush(Qt::black)); // tile background

        if (m_missionPlanningEnabled)
        {
            SetShowHome(false);
        }
        else
        {
            // Set current home position
            updateHomePosition(UASManager::instance()->getHomeLatitude(),
                               UASManager::instance()->getHomeLongitude(),
                               UASManager::instance()->getHomeAltitude());
        }

        // Set currently selected system
        activeUASSet(UASManager::instance()->getActiveUAS());

        // Connect map updates to the adapter slots
        connect(this, SIGNAL(WPValuesChanged(WayPointItem*)), this, SLOT(handleMapWaypointEdit(WayPointItem*)));


        // Start timer
        connect(&updateTimer, SIGNAL(timeout()), this, SLOT(updateGlobalPosition()));
        mapInitialized = true;
        //QTimer::singleShot(800, this, SLOT(loadSettings()));
    }
    SetCurrentPosition(pos_lat_lon);         // set the map position
    setFocus();
    updateTimer.start(maxUpdateInterval*1000);
    // Update all UAV positions
    updateGlobalPosition();
}

void QGCMapWidget::hideEvent(QHideEvent* event)
{
    updateTimer.stop();
    storeSettings();
    OPMapWidget::hideEvent(event);
}

/**
 * @param changePosition Load also the last position from settings and update the map position.
 */
void QGCMapWidget::loadSettings()
{
    QSettings settings;
    const bool logAnalysisSettings =
        m_settingsGroup == QStringLiteral("QGC_MAPWIDGET/LogAnalysis");
    settings.beginGroup(m_settingsGroup);
    m_lastLat = settings.value(QStringLiteral("LAST_LATITUDE"), 0.0f)
                    .toDouble();
    m_lastLon = settings.value(QStringLiteral("LAST_LONGITUDE"), 0.0f)
                    .toDouble();
    m_lastZoom = settings.value(
        QStringLiteral("LAST_ZOOM"), logAnalysisSettings ? 10.0 : 1.0)
                         .toDouble();

    SetMapType(MapTileSourceFactory::instance()->CurrentMapType());

    trailType = static_cast<mapcontrol::UAVTrailType::Types>(settings.value("TRAIL_TYPE", trailType).toInt());
    trailInterval = settings.value("TRAIL_INTERVAL", trailInterval).toFloat();
    settings.endGroup();

    foreach (mapcontrol::UAVItem* uav, GetUAVS())
    {
        configureTrail(uav);
    }

    // SET INITIAL POSITION AND ZOOM
    internals::PointLatLng pos_lat_lon = internals::PointLatLng(m_lastLat, m_lastLon);
    SetCurrentPosition(pos_lat_lon);        // set the map position
    SetZoom(m_lastZoom); // set map zoom level
}

void QGCMapWidget::configureTrail(mapcontrol::UAVItem *uav)
{
    if (!uav) {
        return;
    }
    uav->SetTrailType(trailType);
    if (trailType == mapcontrol::UAVTrailType::ByDistance) {
        uav->SetTrailDistance(trailInterval);
    } else if (trailType == mapcontrol::UAVTrailType::ByTimeElapsed) {
        uav->SetTrailTime(trailInterval);
    }
}

void QGCMapWidget::storeSettings()
{
    QSettings settings;
    settings.beginGroup(m_settingsGroup);
    internals::PointLatLng pos = CurrentPosition();
    if (qIsFinite(pos.Lat()) && qIsFinite(pos.Lng())
        && pos.Lat() >= -90.0 && pos.Lat() <= 90.0
        && pos.Lng() >= -180.0 && pos.Lng() <= 180.0) {
        settings.setValue("LAST_LATITUDE", pos.Lat());
        settings.setValue("LAST_LONGITUDE", pos.Lng());
    }
    settings.setValue("LAST_ZOOM", ZoomReal());
    settings.setValue("TRAIL_TYPE", static_cast<int>(trailType));
    settings.setValue("TRAIL_INTERVAL", trailInterval);
    settings.endGroup();
    settings.sync();
}

void QGCMapWidget::setGlobalMapType(core::MapType::Types type)
{
    if (GetMapType() != type) {
        SetMapType(type);
    } else {
        ReloadMap();
    }
}

void QGCMapWidget::refreshGlobalMapType()
{
    ReloadMap();
}

void QGCMapWidget::setMissionPlanningEnabled(bool enabled)
{
    // QWidget handles ActionsContextMenu internally and does not dispatch it
    // to contextMenuEvent(). PLAN needs the clicked geographic coordinate, so
    // use the virtual event path there and restore the legacy action menu for
    // DATA/SIMULATION.
    setContextMenuPolicy(enabled ? Qt::DefaultContextMenu
                                 : Qt::ActionsContextMenu);
    if (m_missionPlanningEnabled == enabled)
    {
        updateLegacyWaypointVisibility();
        if (enabled) {
            redrawPlannerLines();
            redrawPlannerMeasurement();
        }
        return;
    }

    m_missionPlanningEnabled = enabled;
    updateLegacyWaypointVisibility();

    for (mapcontrol::WayPointItem *icon : m_plannerIcons)
        icon->setVisible(enabled);
    if (m_plannerHomeIcon)
        m_plannerHomeIcon->setVisible(enabled);
    if (m_plannerLineGroup)
        m_plannerLineGroup->setVisible(enabled);
    if (m_plannerMeasurementGroup)
        m_plannerMeasurementGroup->setVisible(enabled);

    if (enabled) {
        redrawPlannerLines();
        redrawPlannerMeasurement();
    }
}

void QGCMapWidget::setPlannerRows(
        const QVector<WpRowData> &rows,
        FlightPlannerMissionModel::MissionStore store)
{
    m_plannerRows = rows;
    m_plannerStore = store;
    rebuildPlannerGraphics();
}

void QGCMapWidget::setPlannerAltitudePresentation(
        double multiplier, const QString &unit)
{
    const QString normalizedUnit = unit.trimmed();
    if (!qIsFinite(multiplier) || multiplier <= 0.0
            || normalizedUnit.isEmpty()
            || (m_plannerAltitudeMultiplier == multiplier
                && m_plannerAltitudeUnit == normalizedUnit)) {
        return;
    }
    m_plannerAltitudeMultiplier = multiplier;
    m_plannerAltitudeUnit = normalizedUnit;
    if (m_plannerHomeIcon) {
        m_plannerHomeIcon->SetAltitudePresentation(
            m_plannerAltitudeMultiplier, m_plannerAltitudeUnit);
    }
    for (mapcontrol::WayPointItem *icon : m_plannerIcons) {
        icon->SetAltitudePresentation(
            m_plannerAltitudeMultiplier, m_plannerAltitudeUnit);
    }
}

void QGCMapWidget::setPlannerNavigationParameters(
        const FlightPlannerNavigationParameters &parameters)
{
    m_plannerNavigation = parameters;
    rebuildPlannerRoute();
    redrawPlannerLines();
}

void QGCMapWidget::setPlannerDrawnPolygon(
        const QVector<MapCoordinate> &points)
{
    m_plannerDrawnPolygon.clear();
    m_plannerDrawnPolygon.reserve(points.size());
    for (const MapCoordinate &point : points)
    {
        if (isPlannerCoordinateValid(point.latitude, point.longitude))
            m_plannerDrawnPolygon.append(point);
    }
    redrawPlannerLines();
}

void QGCMapWidget::setPlannerMeasurement(
        const QVector<MapCoordinate> &points)
{
    m_plannerMeasurementPoints.clear();
    m_plannerMeasurementPoints.reserve(qMin(points.size(), 2));
    for (const MapCoordinate &point : points) {
        if (isPlannerCoordinateValid(point.latitude, point.longitude))
            m_plannerMeasurementPoints.append(point);
        if (m_plannerMeasurementPoints.size() == 2) break;
    }
    redrawPlannerMeasurement();
}

void QGCMapWidget::redrawPlannerMeasurement()
{
    if (!m_plannerMeasurementGroup) return;

    QVector<QPointF> projected;
    if (m_missionPlanningEnabled) {
        projected.reserve(m_plannerMeasurementPoints.size());
        for (const MapCoordinate &point : m_plannerMeasurementPoints) {
            const core::Point local = map->FromLatLngToLocal(
                internals::PointLatLng(point.latitude, point.longitude));
            projected.append(QPointF(local.X(), local.Y()));
        }
    }
    MissionPlanner::PlannerMeasurementOverlay::Rebuild(
        m_plannerMeasurementGroup, projected);
    m_plannerMeasurementGroup->setVisible(m_missionPlanningEnabled);
}

void QGCMapWidget::setPlannerHome(double latitude, double longitude,
                                  double altitude)
{
    m_plannerHomeValid = isPlannerCoordinateValid(latitude, longitude)
            && qIsFinite(altitude);
    m_plannerHomeLatitude = latitude;
    m_plannerHomeLongitude = longitude;
    m_plannerHomeAltitude = altitude;
    rebuildPlannerGraphics();
}

void QGCMapWidget::clearPlannerHome()
{
    if (!m_plannerHomeValid && !m_plannerHomeIcon)
        return;
    m_plannerHomeValid = false;
    rebuildPlannerGraphics();
}

void QGCMapWidget::setPlannerSelection(int seq)
{
    m_plannerSelection = seq;
    for (auto it = m_plannerIcons.constBegin();
         it != m_plannerIcons.constEnd(); ++it)
    {
        it.value()->setSelected(it.key() == seq);
    }
}

QColor QGCMapWidget::plannerColor() const
{
    switch (m_plannerStore)
    {
    case FlightPlannerMissionModel::MissionStore::Fence:
        return QColor(255, 152, 0);
    case FlightPlannerMissionModel::MissionStore::Rally:
        return QColor(186, 104, 200);
    case FlightPlannerMissionModel::MissionStore::Mission:
    default:
        return QColor(0, 174, 239);
    }
}

void QGCMapWidget::clearPlannerLines()
{
    if (!m_plannerLineGroup)
        return;

    const QList<QGraphicsItem*> lines = m_plannerLineGroup->childItems();
    for (QGraphicsItem *line : lines)
        delete line;
}

void QGCMapWidget::clearPlannerGraphics()
{
    m_plannerGraphicsUpdate = true;
    clearPlannerLines();

    const QList<mapcontrol::WayPointItem*> icons = m_plannerIcons.values();
    m_plannerIcons.clear();
    m_plannerIconSequences.clear();
    for (mapcontrol::WayPointItem *icon : icons)
        delete icon;

    delete m_plannerHomeIcon;
    m_plannerHomeIcon = nullptr;
    m_plannerRenderedRoute.clear();
    m_plannerGraphicsUpdate = false;
}

void QGCMapWidget::rebuildPlannerGraphics()
{
    m_plannerGraphicsUpdate = true;
    clearPlannerLines();

    if (m_plannerHomeValid)
    {
        if (!m_plannerHomeIcon)
        {
            m_plannerHomeIcon = new mapcontrol::WayPointItem(
                        internals::PointLatLng(m_plannerHomeLatitude,
                                               m_plannerHomeLongitude),
                        m_plannerHomeAltitude, map, this, tr("Home"));
            m_plannerHomeIcon->SetNumber(0);
            m_plannerHomeIcon->SetAltitudePresentation(
                m_plannerAltitudeMultiplier, m_plannerAltitudeUnit);
            m_plannerHomeIcon->setFlag(QGraphicsItem::ItemIsMovable, false);
            m_plannerHomeIcon->setFlag(QGraphicsItem::ItemIsSelectable, false);
            m_plannerHomeIcon->setZValue(3.0);
            m_plannerHomeIcon->setParentItem(map);
        }
        else
        {
            m_plannerHomeIcon->SetCoord(internals::PointLatLng(
                                            m_plannerHomeLatitude,
                                            m_plannerHomeLongitude));
            m_plannerHomeIcon->SetAltitude(m_plannerHomeAltitude);
        }
        m_plannerHomeIcon->setVisible(m_missionPlanningEnabled);
    }
    else
    {
        delete m_plannerHomeIcon;
        m_plannerHomeIcon = nullptr;
    }

    QMap<int, mapcontrol::WayPointItem*> remainingIcons = m_plannerIcons;
    QMap<int, mapcontrol::WayPointItem*> updatedIcons;
    QMap<mapcontrol::WayPointItem*, int> updatedSequences;

    for (const WpRowData &row : m_plannerRows)
    {
        if (!isPlannerCoordinateValid(row.Lat, row.Lng)
                || !WpRow::CommandHasLocation(row.Command)
                || !WpRow::FrameHasGlobalLocation(row.Frame)
                || updatedIcons.contains(row.Seq))
            continue;

        const QString description = QStringLiteral("%1: %2")
                .arg(row.Seq + 1)
                .arg(WpRow::CommandNameFor(row.Command));
        mapcontrol::WayPointItem *icon = remainingIcons.take(row.Seq);
        if (!icon)
        {
            icon = new mapcontrol::WayPointItem(
                        internals::PointLatLng(row.Lat, row.Lng), row.Alt,
                        map, this, description);
            icon->SetNumber(row.Seq + 1);
            icon->SetAltitudePresentation(
                m_plannerAltitudeMultiplier, m_plannerAltitudeUnit);
            icon->setZValue(4.0);
            icon->setParentItem(map);
            connect(icon, &mapcontrol::WayPointItem::WPValuesChanged,
                    this, &QGCMapWidget::handleMapWaypointEdit);
        }
        else
        {
            icon->SetCoord(internals::PointLatLng(row.Lat, row.Lng));
            icon->SetAltitude(row.Alt);
            icon->SetDescription(description);
            icon->SetNumber(row.Seq + 1);
        }
        icon->setVisible(m_missionPlanningEnabled);
        icon->setSelected(row.Seq == m_plannerSelection);
        updatedIcons.insert(row.Seq, icon);
        updatedSequences.insert(icon, row.Seq);
    }

    m_plannerIcons = updatedIcons;
    m_plannerIconSequences = updatedSequences;
    for (mapcontrol::WayPointItem *icon : remainingIcons)
        delete icon;

    rebuildPlannerRoute();
    m_plannerGraphicsUpdate = false;
    if (m_plannerLineGroup)
        m_plannerLineGroup->setVisible(m_missionPlanningEnabled);
    redrawPlannerLines();
}

void QGCMapWidget::rebuildPlannerRoute()
{
    m_plannerRenderedRoute.clear();
    if (m_plannerStore != FlightPlannerMissionModel::MissionStore::Mission)
        return;

    QVector<FlightPlannerRoutePoint> route;
    if (m_plannerHomeValid)
    {
        route.append({m_plannerHomeLatitude,
                      m_plannerHomeLongitude, false});
    }
    for (const WpRowData &row : m_plannerRows)
    {
        const mapcontrol::WayPointItem *icon =
                m_plannerIcons.value(row.Seq, nullptr);
        if (!icon || !WpRow::CommandIsFlightPath(row.Command))
            continue;
        const internals::PointLatLng coordinate = icon->Coord();
        route.append({coordinate.Lat(), coordinate.Lng(),
                      row.Command == MAV_CMD_NAV_SPLINE_WAYPOINT});
    }
    m_plannerRenderedRoute = FlightPlannerNavigation::BuildSplineRoute(
                route, m_plannerNavigation);
}

void QGCMapWidget::redrawPlannerLines()
{
    clearPlannerLines();
    if (!m_missionPlanningEnabled || !m_plannerLineGroup)
        return;

    if (!m_plannerDrawnPolygon.isEmpty())
    {
        QPainterPath polygonPath;
        for (int i = 0; i < m_plannerDrawnPolygon.size(); ++i)
        {
            const MapCoordinate &point = m_plannerDrawnPolygon.at(i);
            const core::Point local = map->FromLatLngToLocal(
                        internals::PointLatLng(point.latitude,
                                               point.longitude));
            if (i == 0)
                polygonPath.moveTo(local.X(), local.Y());
            else
                polygonPath.lineTo(local.X(), local.Y());
        }
        if (m_plannerDrawnPolygon.size() >= 3) {
            polygonPath.closeSubpath();
        } else if (m_plannerDrawnPolygon.size() == 1) {
            const MapCoordinate &point = m_plannerDrawnPolygon.first();
            const core::Point local = map->FromLatLngToLocal(
                internals::PointLatLng(point.latitude, point.longitude));
            polygonPath.addEllipse(
                QPointF(local.X(), local.Y()), 5.0, 5.0);
        }

        auto *polygon = new QGraphicsPathItem(
                    polygonPath, m_plannerLineGroup);
        QColor outline(255, 64, 129);
        QPen pen(outline);
        pen.setWidth(3);
        pen.setStyle(Qt::DashLine);
        pen.setCosmetic(true);
        polygon->setPen(pen);
        if (m_plannerDrawnPolygon.size() >= 3) {
            outline.setAlpha(38);
            polygon->setBrush(outline);
        }
        polygon->setZValue(2.5);
    }

    if (m_plannerStore == FlightPlannerMissionModel::MissionStore::Rally)
        return;

    QPainterPath path;
    int pathPointCount = 0;
    if (m_plannerStore == FlightPlannerMissionModel::MissionStore::Mission)
    {
        for (const FlightPlannerRoutePoint &point : m_plannerRenderedRoute)
        {
            const internals::PointLatLng coordinate(
                        point.latitude, point.longitude);
            const core::Point local = map->FromLatLngToLocal(coordinate);
            if (pathPointCount == 0)
                path.moveTo(local.X(), local.Y());
            else
                path.lineTo(local.X(), local.Y());
            ++pathPointCount;
        }

        if (m_plannerNavigation.wpRadiusMeters > 0.0)
        {
            for (const WpRowData &row : m_plannerRows)
            {
                const mapcontrol::WayPointItem *icon =
                        m_plannerIcons.value(row.Seq, nullptr);
                if (!icon || !WpRow::CommandIsFlightPath(row.Command))
                    continue;
                const internals::PointLatLng coordinate = icon->Coord();
                const qreal radius = map->metersToPixels(
                            m_plannerNavigation.wpRadiusMeters, coordinate);
                if (!qIsFinite(radius) || radius <= 0.0)
                    continue;
                const core::Point local = map->FromLatLngToLocal(coordinate);
                path.addEllipse(QPointF(local.X(), local.Y()), radius, radius);
            }
        }
    }
    else
    {
        const auto addFencePath = [this](const QPainterPath &geometry,
                                         bool exclusion, bool fill)
        {
            if (geometry.isEmpty()) return;
            auto *item = new QGraphicsPathItem(
                geometry, m_plannerLineGroup);
            QPen pen(plannerColor());
            pen.setWidth(3);
            pen.setCosmetic(true);
            if (exclusion) pen.setStyle(Qt::DashLine);
            item->setPen(pen);
            if (fill) {
                QColor fillColor = plannerColor();
                fillColor.setAlpha(exclusion ? 26 : 38);
                item->setBrush(fillColor);
            }
            item->setZValue(2.0);
        };

        for (int first = 0; first < m_plannerRows.size();) {
            const WpRowData &row = m_plannerRows.at(first);
            const bool circle =
                row.Command == MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION
                || row.Command == MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION;
            if (circle) {
                const mapcontrol::WayPointItem *icon =
                    m_plannerIcons.value(row.Seq, nullptr);
                if (icon && qIsFinite(row.P1) && row.P1 > 0.0) {
                    const internals::PointLatLng coordinate = icon->Coord();
                    const qreal radius = map->metersToPixels(
                        row.P1, coordinate);
                    if (qIsFinite(radius) && radius > 0.0) {
                        const core::Point local =
                            map->FromLatLngToLocal(coordinate);
                        QPainterPath circlePath;
                        circlePath.addEllipse(
                            QPointF(local.X(), local.Y()), radius, radius);
                        addFencePath(circlePath,
                            row.Command
                                == MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION,
                            true);
                    }
                }
                ++first;
                continue;
            }

            const bool polygon =
                row.Command
                    == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
                || row.Command
                    == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
            if (!polygon) {
                ++first;
                continue;
            }

            int vertexCount = 0;
            const bool declaredInRange = qIsFinite(row.P1)
                && row.P1 >= 3.0
                && row.P1 <= m_plannerRows.size() - first;
            if (declaredInRange) {
                vertexCount = static_cast<int>(std::round(row.P1));
                if (std::abs(row.P1 - vertexCount) > 0.000001) {
                    vertexCount = 0;
                }
            }
            if (vertexCount == 0) {
                vertexCount = 0;
                while (first + vertexCount < m_plannerRows.size()) {
                    const WpRowData &candidate =
                        m_plannerRows.at(first + vertexCount);
                    if (candidate.Command != row.Command
                        || !qIsFinite(candidate.P1)
                        || std::abs(candidate.P1) > 0.000001) {
                        break;
                    }
                    ++vertexCount;
                }
            }

            if (vertexCount >= 3 && declaredInRange) {
                for (int offset = 0; offset < vertexCount; ++offset) {
                    const WpRowData &candidate =
                        m_plannerRows.at(first + offset);
                    if (candidate.Command != row.Command
                        || !qIsFinite(candidate.P1)
                        || std::abs(candidate.P1 - vertexCount) > 0.000001) {
                        vertexCount = 0;
                        break;
                    }
                }
            }

            QPainterPath polygonPath;
            int renderedVertices = 0;
            for (int offset = 0; offset < vertexCount; ++offset) {
                const WpRowData &vertex = m_plannerRows.at(first + offset);
                const mapcontrol::WayPointItem *icon =
                    m_plannerIcons.value(vertex.Seq, nullptr);
                if (!icon) continue;
                const core::Point local = map->FromLatLngToLocal(icon->Coord());
                if (renderedVertices == 0)
                    polygonPath.moveTo(local.X(), local.Y());
                else
                    polygonPath.lineTo(local.X(), local.Y());
                ++renderedVertices;
            }
            if (renderedVertices >= 3) {
                polygonPath.closeSubpath();
                addFencePath(polygonPath,
                    row.Command
                        == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION,
                    true);
            }
            first += std::max(1, vertexCount);
        }
        return;
    }

    if (path.elementCount() < 2)
        return;

    QGraphicsPathItem *line = new QGraphicsPathItem(path, m_plannerLineGroup);
    QPen pen(plannerColor());
    pen.setWidth(3);
    pen.setCosmetic(true);
    line->setPen(pen);
    line->setZValue(2.0);
}

void QGCMapWidget::updateLegacyWaypointVisibility()
{
    const bool visible = !m_missionPlanningEnabled;
    for (mapcontrol::WayPointItem *icon : waypointsToIcons)
        icon->setVisible(visible);
    for (QGraphicsItemGroup *group : waypointLines)
    {
        if (group)
            group->setVisible(visible);
    }

    if (m_missionPlanningEnabled)
    {
        SetShowHome(false);
    }
    else
    {
        updateHomePosition(UASManager::instance()->getHomeLatitude(),
                           UASManager::instance()->getHomeLongitude(),
                           UASManager::instance()->getHomeAltitude());
    }
}

void QGCMapWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (m_missionPlanningEnabled)
    {
        const QPointF localPosition = map->mapFromScene(mapToScene(event->pos()));
        const internals::PointLatLng pos =
                map->FromLocalToLatLng(localPosition.x(), localPosition.y());
        emit plannerCoordinateRequested(pos.Lat(), pos.Lng());
        OPMapWidget::mouseDoubleClickEvent(event);
        return;
    }

    // If a waypoint manager is available
    if (currWPManager)
    {
        // Create new waypoint
        internals::PointLatLng pos = map->FromLocalToLatLng(event->pos().x(), event->pos().y());
        currWPManager->blockSignals(true);
        Waypoint* wp = currWPManager->createWaypoint();
        //            wp->setFrame(MAV_FRAME_GLOBAL_RELATIVE_ALT);
        wp->setLatitude(pos.Lat());
        wp->setLongitude(pos.Lng());
        currWPManager->blockSignals(false);
        currWPManager->notifyOfChangeEditable(NULL); // yes: NULL to fire waypointEditableListChanged
    }

    OPMapWidget::mouseDoubleClickEvent(event);
}

int QGCMapWidget::plannerWaypointSequenceAt(
        const QPoint &viewportPosition) const
{
    const QList<QGraphicsItem *> hitItems = items(viewportPosition);
    for (QGraphicsItem *hit : hitItems) {
        for (QGraphicsItem *item = hit; item; item = item->parentItem()) {
            for (auto it = m_plannerIconSequences.constBegin();
                 it != m_plannerIconSequences.constEnd(); ++it) {
                if (static_cast<QGraphicsItem *>(it.key()) == item)
                    return it.value();
            }
        }
    }
    return -1;
}

void QGCMapWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (m_missionPlanningEnabled) {
        const QPointF localPosition = map->mapFromScene(
            mapToScene(event->pos()));
        const internals::PointLatLng position = map->FromLocalToLatLng(
            localPosition.x(), localPosition.y());
        if (isPlannerCoordinateValid(position.Lat(), position.Lng())) {
            emit plannerContextMenuRequested(
                position.Lat(), position.Lng(), event->globalPos(),
                plannerWaypointSequenceAt(event->pos()));
        }
        event->accept();
        return;
    }
    OPMapWidget::contextMenuEvent(event);
}


/**
 *
 * @param uas the UAS/MAV to monitor/display with the map widget
 */
void QGCMapWidget::addUAS(UASInterface* uas)
{
    QLOG_DEBUG() << "addUAS" << uas->getUASName();

    connect(uas, SIGNAL(globalPositionChanged(UASInterface*,double,double,double,quint64)), this, SLOT(updateGlobalPosition(UASInterface*,double,double,double,quint64)));
    connect(uas, SIGNAL(systemSpecsChanged(int)), this, SLOT(updateSystemSpecs(int)));
}

void QGCMapWidget::activeUASSet(UASInterface* uas)
{
    // Only execute if proper UAS is set
    if (!uas)
    {
        this->uas = 0;
        return;
    }
    if (this->uas == uas) return;

    QLOG_DEBUG() << "activeUASSet" << uas->getUASName();

    // Disconnect old MAV manager
    if (currWPManager)
    {
        // Disconnect the waypoint manager / data storage from the UI
        disconnect(currWPManager, SIGNAL(waypointEditableListChanged(int)), this, SLOT(updateWaypointList(int)));
        disconnect(currWPManager, SIGNAL(waypointEditableChanged(int, Waypoint*)), this, SLOT(updateWaypoint(int,Waypoint*)));
        disconnect(this, SIGNAL(waypointCreated(Waypoint*)), currWPManager, SLOT(addWaypointEditable(Waypoint*)));
        disconnect(this, SIGNAL(waypointChanged(Waypoint*)), currWPManager, SLOT(notifyOfChangeEditable(Waypoint*)));

        QGraphicsItemGroup* group = waypointLine(this->uas ? this->uas->getUASID() : 0);
        if (group)
        {
            // Delete existing waypoint lines
            foreach (QGraphicsItem* item, group->childItems())
            {
                group->removeFromGroup(item);
                delete item;
                item = 0;
            }
        }
    }

    this->uas = uas;
    this->currWPManager = uas->getWaypointManager();

    updateSelectedSystem(uas->getUASID());
    followUAVID = uas->getUASID();
    updateWaypointList(uas->getUASID());

    // Connect the waypoint manager / data storage to the UI
    connect(currWPManager, SIGNAL(waypointEditableListChanged(int)), this, SLOT(updateWaypointList(int)));
    connect(currWPManager, SIGNAL(waypointEditableChanged(int, Waypoint*)), this, SLOT(updateWaypoint(int,Waypoint*)));
    connect(this, SIGNAL(waypointCreated(Waypoint*)), currWPManager, SLOT(addWaypointEditable(Waypoint*)));
    connect(this, SIGNAL(waypointChanged(Waypoint*)), currWPManager, SLOT(notifyOfChangeEditable(Waypoint*)));

}

/**
 * Updates the global position of one MAV and append the last movement to the trail
 *
 * @param uas The unmanned air system
 * @param lat Latitude in WGS84 ellipsoid
 * @param lon Longitutde in WGS84 ellipsoid
 * @param alt Altitude over mean sea level
 * @param usec Timestamp of the position message in milliseconds FIXME will move to microseconds
 */
void QGCMapWidget::updateGlobalPosition(UASInterface* uas, double lat, double lon, double alt, quint64 usec)
{
    Q_UNUSED(usec);

    // Immediate update
    if (maxUpdateInterval == 0)
    {
        // Get reference to graphic UAV item
        mapcontrol::UAVItem* uav = GetUAV(uas->getUASID());
        // Check if reference is valid, else create a new one
        if (uav == NULL)
        {
            MAV2DIcon* newUAV = new MAV2DIcon(map, this, uas);
            newUAV->setParentItem(map);
            UAVS.insert(uas->getUASID(), newUAV);
            uav = GetUAV(uas->getUASID());
            configureTrail(uav);
        }

        // Set new lat/lon position of UAV icon
        internals::PointLatLng pos_lat_lon = internals::PointLatLng(lat, lon);
        uav->SetUAVPos(pos_lat_lon, alt);

        if(this->uas == uas){
            // save the last know postion
            m_lastLat = uas->getLatitude();
            m_lastLon = uas->getLongitude();
        }

        // Follow status
        if (followUAVEnabled && (uas->getUASID() == followUAVID) && isValidGpsLocation(uas)){
            SetCurrentPosition(pos_lat_lon);
        }
        // Convert from radians to degrees and apply
        uav->SetUAVHeading((uas->getYaw()/M_PI)*180.0f);
    }
}

bool QGCMapWidget::isValidGpsLocation(UASInterface* system) const
{
    if ((system->getLatitude() == 0.0f)
            ||(system->getLongitude() == 0.0f)){
        return false;
    }
    return true;
}

/**
 * Pulls in the positions of all UAVs from the UAS manager
 */
void QGCMapWidget::updateGlobalPosition()
{
    QList<UASInterface*> systems = UASManager::instance()->getUASList();
    foreach (UASInterface* system, systems)
    {
        // Get reference to graphic UAV item
        mapcontrol::UAVItem* uav = GetUAV(system->getUASID());
        // Check if reference is valid, else create a new one
        if (uav == NULL)
        {
            MAV2DIcon* newUAV = new MAV2DIcon(map, this, system);
            AddUAV(system->getUASID(), newUAV);
            uav = newUAV;
            configureTrail(uav);
        }

        // Set new lat/lon position of UAV icon
        internals::PointLatLng pos_lat_lon = internals::PointLatLng(system->getLatitude(), system->getLongitude());
        uav->SetUAVPos(pos_lat_lon, system->getAltitudeAMSL());
        // Follow status
        if (followUAVEnabled && (system->getUASID() == followUAVID) && isValidGpsLocation(system)) {
            SetCurrentPosition(pos_lat_lon);
        }
        // Convert from radians to degrees and apply
        uav->SetUAVHeading((system->getYaw()/M_PI)*180.0f);
    }
}

void QGCMapWidget::updateLocalPosition()
{
    QList<UASInterface*> systems = UASManager::instance()->getUASList();
    foreach (UASInterface* system, systems)
    {
        // Get reference to graphic UAV item
        mapcontrol::UAVItem* uav = GetUAV(system->getUASID());
        // Check if reference is valid, else create a new one
        if (uav == NULL)
        {
            MAV2DIcon* newUAV = new MAV2DIcon(map, this, system);
            AddUAV(system->getUASID(), newUAV);
            uav = newUAV;
            configureTrail(uav);
        }

        // Set new lat/lon position of UAV icon
        internals::PointLatLng pos_lat_lon = internals::PointLatLng(system->getLatitude(), system->getLongitude());
        uav->SetUAVPos(pos_lat_lon, system->getAltitudeAMSL());
        // Follow status
        if (followUAVEnabled && (system->getUASID() == followUAVID) && isValidGpsLocation(system)) {
            SetCurrentPosition(pos_lat_lon);
        }
        // Convert from radians to degrees and apply
        uav->SetUAVHeading((system->getYaw()/M_PI)*180.0f);
    }
}

void QGCMapWidget::updateLocalPositionEstimates()
{
    updateLocalPosition();
}


void QGCMapWidget::updateSystemSpecs(int uas)
{
    foreach (mapcontrol::UAVItem* p, UAVS.values())
    {
        MAV2DIcon* icon = dynamic_cast<MAV2DIcon*>(p);
        if (icon && icon->getUASId() == uas)
        {
            // Set new airframe
            icon->setAirframe(UASManager::instance()->getUASForId(uas)->getAirframe());
            icon->drawIcon();
        }
    }
}

/**
 * Does not update the system type or configuration, only the selected status
 */
void QGCMapWidget::updateSelectedSystem(int uas)
{
    foreach (mapcontrol::UAVItem* p, UAVS.values())
    {
        MAV2DIcon* icon = dynamic_cast<MAV2DIcon*>(p);
        if (icon)
        {
            // Set as selected if ids match
            icon->setSelectedUAS((icon->getUASId() == uas));
        }
    }
}


// MAP NAVIGATION
void QGCMapWidget::showGoToDialog()
{
    bool ok;
    QString text = QInputDialog::getText(this, tr("Please enter coordinates"),
                                         tr("Coordinates (Lat,Lon):"), QLineEdit::Normal,
                                         QString("%1,%2").arg(CurrentPosition().Lat(), 0, 'g', 6).arg(CurrentPosition().Lng(), 0, 'g', 6), &ok);
    if (ok && !text.isEmpty())
    {
        QStringList split = text.split(",");
        if (split.length() == 2)
        {
            bool convert;
            double latitude = split.first().toDouble(&convert);
            ok &= convert;
            double longitude = split.last().toDouble(&convert);
            ok &= convert;

            if (ok)
            {
                internals::PointLatLng pos_lat_lon = internals::PointLatLng(latitude, longitude);
                SetCurrentPosition(pos_lat_lon);        // set the map position
            }
        }
    }
}


void QGCMapWidget::updateHomePosition(double latitude, double longitude, double altitude)
{
    homeAltitude = altitude;
    if (m_missionPlanningEnabled)
    {
        SetShowHome(false);
        return;
    }

    SetShowHome(true);                      // display the HOME position on the map
    Home->SetSafeArea(0);
    Home->SetShowSafeArea(false);
    Home->SetCoord(internals::PointLatLng(latitude, longitude));
    Home->SetAltitude(altitude);

    Home->RefreshPos();     // Force repainting
}

void QGCMapWidget::goHome()
{
    if (m_missionPlanningEnabled)
    {
        if (!m_plannerHomeValid)
            return;
        SetCurrentPosition(internals::PointLatLng(m_plannerHomeLatitude,
                                                   m_plannerHomeLongitude));
        SetZoom(18);
        return;
    }
    if (!Home)
        return;

    SetCurrentPosition(Home->Coord());
    SetZoom(18); //zoom to "large RC park" size
}

void QGCMapWidget::lastPosition()
{
    internals::PointLatLng pos_lat_lon = internals::PointLatLng(m_lastLat, m_lastLon);
    SetCurrentPosition(pos_lat_lon);
    SetZoom(m_lastZoom); //zoom to "large RC park" size
}

/**
 * Limits the update rate on the specified interval. Set to zero (0) to run at maximum
 * telemetry speed. Recommended rate is 2 s.
 */
void QGCMapWidget::setUpdateRateLimit(float seconds)
{
    maxUpdateInterval = seconds;
    updateTimer.start(maxUpdateInterval*1000);
}

void QGCMapWidget::cacheVisibleRegion()
{
    internals::RectLatLng rect = map->SelectedArea();

    if (rect.IsEmpty())
    {
        QMessageBox msgBox(this);
        msgBox.setIcon(QMessageBox::Information);
        msgBox.setText("Cannot cache tiles for offline use");
        msgBox.setInformativeText("Please select an area first by holding down SHIFT or ALT and selecting the area with the left mouse button.");
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setDefaultButton(QMessageBox::Ok);
        msgBox.exec();
    }
    else
    {
        RipMap();
        // Set empty area = unselect area
        map->SetSelectedArea(internals::RectLatLng());
    }
}


// WAYPOINT MAP INTERACTION FUNCTIONS

void QGCMapWidget::shiftOtherSelectedWaypoints(mapcontrol::WayPointItem* selectedWaypoint,
                                               double shiftLong, double shiftLat)
{
    QMap<mapcontrol::WayPointItem*, Waypoint*>::iterator i;
    for (i = iconsToWaypoints.begin(); i != iconsToWaypoints.end(); ++i)
    {
        mapcontrol::WayPointItem* waypoint = i.key();

        if (waypoint->isSelected())
        {
            if (waypoint == selectedWaypoint)
            {
                continue;
            }

            // Update WP values
            Waypoint* wp = i.value();
            internals::PointLatLng pos = waypoint->Coord();

            // Block waypoint signals
            wp->blockSignals(true);
            wp->setLatitude(pos.Lat() - shiftLat);
            wp->setLongitude(pos.Lng() - shiftLong);
            wp->blockSignals(false);

            emit waypointChanged(wp);
        }
    }
}

void QGCMapWidget::handleMapWaypointEdit(mapcontrol::WayPointItem* waypoint)
{
    const auto plannerIt = m_plannerIconSequences.constFind(waypoint);
    if (plannerIt != m_plannerIconSequences.constEnd())
    {
        if (m_plannerGraphicsUpdate || !m_missionPlanningEnabled)
            return;

        const int seq = plannerIt.value();
        const internals::PointLatLng pos = map->FromLocalToLatLng(
                    waypoint->pos().x(), waypoint->pos().y());
        m_plannerGraphicsUpdate = true;
        waypoint->SetCoord(pos);
        m_plannerGraphicsUpdate = false;
        for (WpRowData &row : m_plannerRows)
        {
            if (row.Seq == seq)
            {
                row.Lat = pos.Lat();
                row.Lng = pos.Lng();
                break;
            }
        }
        rebuildPlannerRoute();
        redrawPlannerLines();
        emit plannerWaypointMoved(seq, pos.Lat(), pos.Lng());
        return;
    }

    // Block circle updates
    Waypoint* wp = iconsToWaypoints.value(waypoint, NULL);

    // Delete UI element if wp doesn't exist
    if (!wp)
    {
        WPDelete(waypoint);
        return;
    }

    // Protect from vicious double update cycle
    if (firingWaypointChange == wp) return;
    // Not in cycle, block now from entering it
    firingWaypointChange = wp;

    QLOG_TRACE() << "UPDATING WP FROM MAP" << wp->getId();

    // Update WP values
    internals::PointLatLng pos = waypoint->Coord();

    double shiftLat = wp->getLatitude() - pos.Lat();
    double shiftLong = wp->getLongitude() - pos.Lng();
    // Block waypoint signals
    wp->blockSignals(true);
    wp->setLatitude(pos.Lat());
    wp->setLongitude(pos.Lng());
    wp->blockSignals(false);

    firingWaypointChange = NULL;

    emit waypointChanged(wp);

    shiftOtherSelectedWaypoints(waypoint, shiftLong, shiftLat);
}

// WAYPOINT UPDATE FUNCTIONS

/**
 * This function is called if a a single waypoint is updated and
 * also if the whole list changes.
 */
void QGCMapWidget::updateWaypoint(int uas, Waypoint* wp)
{
    QLOG_TRACE() << __FILE__ << __LINE__ << "UPDATING WP FUNCTION CALLED";
    // Source of the event was in this widget, do nothing
    if (firingWaypointChange == wp) {
        return;
    }
    // Currently only accept waypoint updates from the UAS in focus
    // this has to be changed to accept read-only updates from other systems as well.
    UASInterface* uasInstance = UASManager::instance()->getUASForId(uas);
    if (currWPManager)
    {
        // Only accept waypoints in global coordinate frame
        if (((wp->getFrame() == MAV_FRAME_GLOBAL) ||
             (wp->getFrame() == MAV_FRAME_GLOBAL_RELATIVE_ALT) ||
             (wp->getFrame() == MAV_FRAME_GLOBAL_TERRAIN_ALT)) && (wp->isNavigationType() || wp->visibleOnMapWidget()))
        {
            // We're good, this is a global waypoint

            // Get the index of this waypoint
            int wpindex = currWPManager->getIndexOf(wp);
            // If not found, return (this should never happen, but helps safety)
            if (wpindex < 0) return;
            // Mark this wp as currently edited
            firingWaypointChange = wp;

            QLOG_TRACE() << "UPDATING WAYPOINT" << wpindex << "IN 2D MAP";

            // Check if wp exists yet in map
            if (!waypointsToIcons.contains(wp))
            {
                QLOG_TRACE() << "UPDATING NEW WAYPOINT" << wpindex << "IN 2D MAP";
                // Create icon for new WP
                QColor wpColor(Qt::red);
                if (uasInstance) wpColor = uasInstance->getColor();
                Waypoint2DIcon* icon = new Waypoint2DIcon(map, this, wp, wpColor, wpindex);
                ConnectWP(icon);
                icon->setParentItem(map);
                // Update maps to allow inverse data association
                waypointsToIcons.insert(wp, icon);
                iconsToWaypoints.insert(icon, wp);
                icon->setVisible(!m_missionPlanningEnabled);
            }
            else
            {
                QLOG_TRACE() << "UPDATING EXISTING WAYPOINT" << wpindex << "IN 2D MAP";
                // Waypoint exists, block it's signals and update it
                mapcontrol::WayPointItem* icon = waypointsToIcons.value(wp);
                // Make sure we don't die on a null pointer
                // this should never happen, just a precaution
                if (!icon) return;
                // Block outgoing signals to prevent an infinite signal loop
                // should not happen, just a precaution
                this->blockSignals(true);
                // Update the WP
                Waypoint2DIcon* wpicon = dynamic_cast<Waypoint2DIcon*>(icon);
                if (wpicon)
                {
                    // Let icon read out values directly from waypoint
                    icon->SetNumber(wpindex);
                    wpicon->updateWaypoint();
                }
                else
                {
                    // Use safe standard interfaces for non Waypoint-class based wps
                    icon->SetCoord(internals::PointLatLng(wp->getLatitude(), wp->getLongitude()));
                    icon->SetAltitude(wp->getAltitude());
                    icon->SetHeading(wp->getYaw());
                    icon->SetNumber(wpindex);
                }
                // Re-enable signals again
                this->blockSignals(false);
            }

            redrawWaypointLines(uas);

            firingWaypointChange = NULL;
        }
        else
        {
            // Check if the index of this waypoint is larger than the global
            // waypoint list. This implies that the coordinate frame of this
            // waypoint was changed and the list containing only global
            // waypoints was shortened. Thus update the whole list
            if (waypointsToIcons.count() > currWPManager->getGlobalFrameAndNavTypeCount())
            {
                updateWaypointList(uas);
            }
        }
    }
}

void QGCMapWidget::redrawWaypointLines()
{
    redrawWaypointLines(uas ? uas->getUASID() : 0);
}

void QGCMapWidget::redrawWaypointLines(int uas)
{
    QLOG_TRACE() << "REDRAW WAYPOINT LINES FOR UAS" << uas;

    if (!currWPManager)
        return;

    QGraphicsItemGroup* group = waypointLine(uas);
    if (!group)
        return;
    Q_ASSERT(group->parentItem() == map);
    group->setVisible(!m_missionPlanningEnabled);

    // Delete existing waypoint lines
    foreach (QGraphicsItem* item, group->childItems())
    {
        QLOG_TRACE() << "DELETE EXISTING WAYPOINT LINES" << item;
        delete item;
    }

    QList<Waypoint*> wps = currWPManager->getGlobalFrameAndNavTypeWaypointList(true);
    if (wps.size() > 1)
    {
        QPainterPath path = WaypointNavigation::path(wps, *map);
        if (path.elementCount() > 1)
        {
            QGraphicsPathItem* gpi = new QGraphicsPathItem(map);
            gpi->setPath(path);

            QColor color(Qt::red);
            UASInterface* uasInstance = UASManager::instance()->getUASForId(uas);
            if (uasInstance) color = uasInstance->getColor();
            QPen pen(color);
            pen.setWidth(2);
            gpi->setPen(pen);

            QLOG_TRACE() << "ADDING WAYPOINT LINES" << gpi;
            group->addToGroup(gpi);
        }
    }
}

/**
 * Update the whole list of waypoints. This is e.g. necessary if the list order changed.
 * The UAS manager will emit the appropriate signal whenever updating the list
 * is necessary.
 */
void QGCMapWidget::updateWaypointList(int uas)
{
    QLOG_DEBUG() << "UPDATE WP LIST IN 2D MAP CALLED FOR UAS" << uas;
    // Currently only accept waypoint updates from the UAS in focus
    // this has to be changed to accept read-only updates from other systems as well.
    if (currWPManager)
    {
        // Delete first all old waypoints
        // this is suboptimal (quadratic, but wps should stay in the sub-100 range anyway)
        QList<Waypoint* > wps = currWPManager->getGlobalFrameAndNavTypeWaypointList(false);
        foreach (Waypoint* wp, waypointsToIcons.keys())
        {
            if (!wps.contains(wp))
            {
                QLOG_TRACE() << "DELETE EXISTING WP" << wp->getId();
                // Get icon to work on
                mapcontrol::WayPointItem* icon = waypointsToIcons.value(wp);
                waypointsToIcons.remove(wp);
                iconsToWaypoints.remove(icon);
                WPDelete(icon);
            }
        }

        // Update all existing waypoints
        foreach (Waypoint* wp, waypointsToIcons.keys())
        {
            QLOG_TRACE() << "UPDATING EXISTING WP" << wp->getId();
            updateWaypoint(uas, wp);
        }

        // Update all potentially new waypoints
        foreach (Waypoint* wp, wps)
        {
            // Update / add only if new
            if (!waypointsToIcons.contains(wp))
            {
                QLOG_TRACE() << "UPDATING NEW WP" << wp->getId();
                updateWaypoint(uas, wp);
            }
        }

        updateLegacyWaypointVisibility();

//        redrawWaypointLines(uas);
    }
}
