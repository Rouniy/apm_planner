#include "QGCMapToolBar.h"
#include "UASManager.h"
#include "ArduPilotMegaMAV.h"
#include "AbstractMapWidget.h"
#include "MapTileSourceFactory.h"
#include "MapWidgetFactory.h"
#include "uavtrailtype.h"
#include "ui_QGCMapToolBar.h"

#include <QSettings>

QGCMapToolBar::QGCMapToolBar(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::QGCMapToolBar),
    map(NULL),
    optionsMenu(this),
    trailPlotMenu(this),
    updateTimesMenu(this),
    mapTypesMenu(this),
    mapWidgetsMenu(this),
    trailSettingsGroup(new QActionGroup(this)),
    updateTimesGroup(new QActionGroup(this)),
    mapTypesGroup(new QActionGroup(this)),
    mapWidgetsGroup(new QActionGroup(this))
{
    ui->setupUi(this);
}

static const struct {
    const char*    name;
    core::MapType::Types type;
} sMapTypes[] = {
    { "Bing Hybrid", core::MapType::BingHybrid },
    { "Bing Map", core::MapType::BingMap },
    { "Bing Satellite", core::MapType::BingSatellite },
    { "Google Hybrid", core::MapType::GoogleHybrid },
    { "Google Map", core::MapType::GoogleMap },
    { "Google Satellite", core::MapType::GoogleSatellite },
    { "Google Terrain", core::MapType::GoogleTerrain },
    { "OpenStreetMap", core::MapType::OpenStreetMap },
    { "ArcGIS Map", core::MapType::ArcGIS_Map },
    { "Esri World Imagery", core::MapType::ArcGIS_Satellite },
    { "ArcGIS Terrain", core::MapType::ArcGIS_Terrain },
    { "ArcGIS World Topo", core::MapType::ArcGIS_WorldTopo },
    { "Statkart Topo", core::MapType::Statkart_Topo },
    { "Statkart Basemap", core::MapType::Statkart_Basemap },
    { "Eniro N,S,F,D,P", core::MapType::Eniro_Topo },
    { "Japan Map", core::MapType::JapanMap },
    { "GDAL Custom", core::MapType::GDALCustom },
};

static const size_t sNumMapTypes = sizeof(sMapTypes) / sizeof(sMapTypes[0]);

void QGCMapToolBar::setMap(AbstractMapWidget *map)
{
    if (this->map) {
        Q_ASSERT(this->map == map);
        return;
    }
    this->map = map;

    loadSettings();

    if (map)
    {
        connect(ui->goToButton, &QPushButton::clicked,
                map, &AbstractMapWidget::ShowGoToDialog);
        connect(ui->goHomeButton, SIGNAL(clicked()), this, SLOT(goHome()));
        connect(ui->lastPosButton, &QPushButton::clicked,
                map, &AbstractMapWidget::LastPosition);
        connect(ui->clearTrailsButton, &QPushButton::clicked,
                map, &AbstractMapWidget::DeleteTrails);
        connect(map, &AbstractMapWidget::TileLoadStarted,
                this, &QGCMapToolBar::tileLoadStart);
        connect(map, &AbstractMapWidget::TileLoadCompleted,
                this, &QGCMapToolBar::tileLoadEnd);
        connect(map, &AbstractMapWidget::TilesStillToLoad,
                this, &QGCMapToolBar::tileLoadProgress);
        connect(ui->ripMapButton, &QPushButton::clicked,
                map, &AbstractMapWidget::CacheVisibleRegion);

        map->SetFollowUAVEnabled(ui->followPushButton->isChecked());
        connect(ui->followPushButton, &QPushButton::clicked,
                map, &AbstractMapWidget::SetFollowUAVEnabled);

        // Edit mode handling
        ui->editButton->hide();

        const int uavTrailTimeList[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};                      // seconds
        const int uavTrailTimeCount = 10;

        const int uavTrailDistanceList[] = {1, 2, 5, 10, 20, 50, 100, 200, 500};             // meters
        const int uavTrailDistanceCount = 9;

        // Set exclusive items
        trailSettingsGroup->setExclusive(true);
        updateTimesGroup->setExclusive(true);
        mapTypesGroup->setExclusive(true);

        // Build up menu
        trailPlotMenu.setTitle(tr("&Add trail dot every.."));
        updateTimesMenu.setTitle(tr("&Limit map view update rate to.."));
        mapTypesMenu.setTitle(tr("&Map type"));


        //setup the mapTypesMenu
        QAction* action;
        core::MapType::Types mapType = map->CurrentMapType();
        for (size_t i = 0; i < sNumMapTypes; ++i) {
            action = mapTypesMenu.addAction(tr(sMapTypes[i].name), this, SLOT(setMapType()));
            action->setData(sMapTypes[i].type);
            action->setCheckable(true);
            mapTypesGroup->addAction(action);
            if (mapType == sMapTypes[i].type) action->setChecked(true);
        }
        optionsMenu.addMenu(&mapTypesMenu);

        mapWidgetsMenu.setTitle(tr("&Map widget"));
        mapWidgetsGroup->setExclusive(true);
        MapWidgetFactory *widgetFactory = MapWidgetFactory::instance();
        rebuildMapWidgetsMenu();
        optionsMenu.addMenu(&mapWidgetsMenu);
        connect(widgetFactory, &MapWidgetFactory::StatusMessage,
                this, &QGCMapToolBar::showMapStatus,
                Qt::UniqueConnection);
        connect(widgetFactory, &MapWidgetFactory::BackendChanged,
                this, &QGCMapToolBar::syncMapWidgetBackend,
                Qt::UniqueConnection);
        connect(widgetFactory,
                &MapWidgetFactory::AvailableBackendsChanged,
                this, &QGCMapToolBar::rebuildMapWidgetsMenu,
                Qt::UniqueConnection);
        if (!widgetFactory->LastStatus().isEmpty()) {
            showMapStatus(widgetFactory->LastStatus());
        }
        connect(MapTileSourceFactory::instance(),
                &MapTileSourceFactory::MapTypeChanged,
                this, &QGCMapToolBar::updateMapType,
                Qt::UniqueConnection);
        connect(MapTileSourceFactory::instance(),
                &MapTileSourceFactory::StatusMessage,
                this, &QGCMapToolBar::showMapStatus,
                Qt::UniqueConnection);

        // FIXME MARK CURRENT VALUES IN MENU
        QAction *defaultTrailAction = trailPlotMenu.addAction(tr("No trail"), this, SLOT(setUAVTrailTime()));
        defaultTrailAction->setData(-1);
        defaultTrailAction->setCheckable(true);
        trailSettingsGroup->addAction(defaultTrailAction);

        for (int i = 0; i < uavTrailTimeCount; ++i)
        {
            action = trailPlotMenu.addAction(tr("%1 second%2").arg(uavTrailTimeList[i]).arg((uavTrailTimeList[i] > 1) ? "s" : ""), this, SLOT(setUAVTrailTime()));
            action->setData(uavTrailTimeList[i]);
            action->setCheckable(true);
            trailSettingsGroup->addAction(action);
            if (static_cast<mapcontrol::UAVTrailType::Types>(map->TrailType()) == mapcontrol::UAVTrailType::ByTimeElapsed && map->TrailInterval() == uavTrailTimeList[i])
            {
                // This is the current active time, set the action checked
                action->setChecked(true);
            }
        }
        for (int i = 0; i < uavTrailDistanceCount; ++i)
        {
            action = trailPlotMenu.addAction(tr("%1 meter%2").arg(uavTrailDistanceList[i]).arg((uavTrailDistanceList[i] > 1) ? "s" : ""), this, SLOT(setUAVTrailDistance()));
            action->setData(uavTrailDistanceList[i]);
            action->setCheckable(true);
            trailSettingsGroup->addAction(action);
            if (static_cast<mapcontrol::UAVTrailType::Types>(map->TrailType()) == mapcontrol::UAVTrailType::ByDistance && map->TrailInterval() == uavTrailDistanceList[i])
            {
                // This is the current active time, set the action checked
                action->setChecked(true);
            }
        }

        // Set no trail checked if no action is checked yet
        if (!trailSettingsGroup->checkedAction())
        {
            defaultTrailAction->setChecked(true);
        }

        optionsMenu.addMenu(&trailPlotMenu);

        // Add update times menu
        for (int i = 100; i < 5000; i+=400)
        {
            float time = i/1000.0f; // Convert from ms to seconds
            QAction* action = updateTimesMenu.addAction(tr("%1 seconds").arg(time), this, SLOT(setUpdateInterval()));
            action->setData(time);
            action->setCheckable(true);
            if (time == map->UpdateRateLimit())
            {
                action->blockSignals(true);
                action->setChecked(true);
                action->blockSignals(false);
            }
            updateTimesGroup->addAction(action);
        }

        // If the current time is not part of the menu defaults
        // still add it as new option
        if (!updateTimesGroup->checkedAction())
        {
            float time = map->UpdateRateLimit();
            QAction* action = updateTimesMenu.addAction(tr("uptate every %1 seconds").arg(time), this, SLOT(setUpdateInterval()));
            action->setData(time);
            action->setCheckable(true);
            action->setChecked(true);
            updateTimesGroup->addAction(action);
        }
        optionsMenu.addMenu(&updateTimesMenu);

        ui->optionsButton->setMenu(&optionsMenu);
    }
}

void QGCMapToolBar::setUAVTrailTime()
{
    if (!map) {
        return;
    }
    QObject* sender = QObject::sender();
    QAction* action = qobject_cast<QAction*>(sender);

    if (action)
    {
        bool ok;
        int trailTime = action->data().toInt(&ok);
        if (ok)
        {
            map->SetTrailModeTimed(trailTime);
            ui->posLabel->setText(tr("Trail mode: Every %1 second%2").arg(trailTime).arg((trailTime > 1) ? "s" : ""));
        }
    }
}

void QGCMapToolBar::setUAVTrailDistance()
{
    if (!map) {
        return;
    }
    QObject* sender = QObject::sender();
    QAction* action = qobject_cast<QAction*>(sender);

    if (action)
    {
        bool ok;
        int trailDistance = action->data().toInt(&ok);
        if (ok)
        {
            map->SetTrailModeDistance(trailDistance);
            ui->posLabel->setText(tr("Trail mode: Every %1 meter%2").arg(trailDistance).arg((trailDistance > 1) ? "s" : ""));
        }
    }
}

void QGCMapToolBar::setUpdateInterval()
{
    if (!map) {
        return;
    }
    QObject* sender = QObject::sender();
    QAction* action = qobject_cast<QAction*>(sender);

    if (action)
    {
        bool ok;
        float time = action->data().toFloat(&ok);
        if (ok)
        {
            map->SetUpdateRateLimit(time);
            ui->posLabel->setText(tr("Map update rate limit: %1 second%2").arg(time).arg((time != 1.0f) ? "s" : ""));
        }
    }
}

void QGCMapToolBar::setMapWidgetBackend()
{
    QAction *action = qobject_cast<QAction *>(sender());
    if (!action) {
        return;
    }
    MapWidgetFactory *factory = MapWidgetFactory::instance();
    const QString backendId = action->data().toString();
    if (!factory->SetBackend(backendId)) {
        return;
    }
    if (map && backendId != map->BackendId()) {
        showMapStatus(tr("Map widget will change after restart: %1")
                          .arg(action->text()));
    } else {
        showMapStatus(tr("Map widget: %1").arg(action->text()));
    }
}

void QGCMapToolBar::rebuildMapWidgetsMenu()
{
    for (QAction *action : mapWidgetsGroup->actions()) {
        mapWidgetsGroup->removeAction(action);
    }
    mapWidgetsMenu.clear();
    MapWidgetFactory *factory = MapWidgetFactory::instance();
    for (const MapWidgetBackendInfo &backend
         : factory->AvailableBackends()) {
        QAction *action = mapWidgetsMenu.addAction(
            backend.displayName, this, SLOT(setMapWidgetBackend()));
        action->setObjectName(
            QStringLiteral("MapWidgetBackend_%1").arg(backend.id));
        action->setData(backend.id);
        action->setCheckable(true);
        mapWidgetsGroup->addAction(action);
    }
    syncMapWidgetBackend(factory->RequestedBackend());
}

void QGCMapToolBar::syncMapWidgetBackend(const QString &backendId)
{
    QString selected = backendId;
    if (mapWidgetsGroup->actions().isEmpty()) {
        return;
    }
    bool available = false;
    for (QAction *action : mapWidgetsGroup->actions()) {
        if (action->data().toString() == selected) {
            available = true;
            break;
        }
    }
    if (!available) {
        selected = MapWidgetFactory::instance()->CurrentBackend();
    }
    for (QAction *action : mapWidgetsGroup->actions()) {
        action->setChecked(action->data().toString() == selected);
    }
}

void QGCMapToolBar::setMapType()
{
    QObject* sender = QObject::sender();
    QAction* action = qobject_cast<QAction*>(sender);

    if (action)
    {
        bool ok;
        int mapType = action->data().toInt(&ok);
        if (ok)
        {
            MapTileSourceFactory *factory =
                MapTileSourceFactory::instance();
            factory->SetMapType(
                static_cast<core::MapType::Types>(mapType));
            updateMapType(factory->CurrentMapType());
            if (!factory->LastStatus().isEmpty()) {
                showMapStatus(factory->LastStatus());
            }
        }
    }
}

void QGCMapToolBar::updateMapType(core::MapType::Types type)
{
    for (QAction *action : mapTypesGroup->actions()) {
        action->setChecked(
            action->data().toInt() == static_cast<int>(type));
    }
    ui->posLabel->setText(tr("Map type: %1").arg(
        core::MapType::StrByType(type)));
}

void QGCMapToolBar::showMapStatus(const QString &status)
{
    ui->posLabel->setText(status);
}

void QGCMapToolBar::tileLoadStart()
{
    ui->posLabel->setText(tr("Starting to load tiles.."));
}

void QGCMapToolBar::tileLoadEnd()
{
    ui->posLabel->setText(tr("Finished"));
}

void QGCMapToolBar::tileLoadProgress(int progress)
{
    if (progress == 1)
    {
        ui->posLabel->setText(tr("1 tile to load.."));
    }
    else if (progress > 0)
    {
        ui->posLabel->setText(tr("%1 tiles to load..").arg(progress));
    }
    else
    {
        tileLoadEnd();
    }
}

void QGCMapToolBar::goHome()
{
    if (!map) {
        return;
    }
    UASManager *umanager = UASManager::instance();
    if (umanager){
        ArduPilotMegaMAV* apmUas= dynamic_cast<ArduPilotMegaMAV*>(umanager->getActiveUAS());
        if (apmUas){
            UASWaypointManager* wpManager = apmUas->getWaypointManager();
            const Waypoint* homeWp = wpManager->getWaypoint(0); // Waypoint 0 is home in APM
            if (homeWp){
                map->UpdateHomePosition(homeWp->getLatitude(), homeWp->getLongitude(), homeWp->getAltitude());
                map->GoHome();
            }
        } else {
            map->GoHome();
        }
    }
}

void QGCMapToolBar::loadSettings()
{
    QSettings settings;
    settings.beginGroup("QGC_MAPTOOL");
    bool follow = settings.value("FOLLOW_UAV", false).toBool();
    ui->followPushButton->setChecked(follow);
}

void QGCMapToolBar::storeSettings()
{
    QSettings settings;
    settings.beginGroup("QGC_MAPTOOL");
    settings.setValue("FOLLOW_UAV", ui->followPushButton->isChecked());
    settings.endGroup();
    settings.sync();
}

QGCMapToolBar::~QGCMapToolBar()
{
    storeSettings();
    delete ui;
    delete trailSettingsGroup;
    delete updateTimesGroup;
    delete mapTypesGroup;
    delete mapWidgetsGroup;
    // FIXME Delete all actions
}
