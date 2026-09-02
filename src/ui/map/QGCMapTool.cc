#include "logging.h"
#include "UASInterface.h"
#include "UASManager.h"
#include "UAS.h"
#include "QGCMapTool.h"
#include "AbstractMapWidget.h"
#include "CompiledMapBackends.h"
#include "ui/flightdata/FlightDataMapOverlay.h"
#include "ui/flightdata/FlightDataViewModel.h"
#include "MapWidgetFactory.h"
#include "ui_QGCMapTool.h"

#include <QAction>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>

const static int MapToolZoomFactor = 10; // This may need to be different for win/linux/mac

QGCMapTool::QGCMapTool(QWidget *parent)
    : QGCMapTool(MapWidgetRole::FlightData, parent)
{
}

QGCMapTool::QGCMapTool(MapWidgetRole role, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::QGCMapTool),
    m_role(role),
    m_uasInterface(NULL)
{
    ui->setupUi(this);

    RegisterCompiledMapBackends();
    m_mapBackend = MapWidgetFactory::instance()->CreateMapWidget(
        role, ui->mapHost, this);
    if (!m_mapBackend || !m_mapBackend->Widget()) {
        QLOG_ERROR() << "No map widget backend is available";
        ui->mapHost->setStyleSheet(QStringLiteral("background: #202020;"));
        ui->toolBar->setEnabled(false);
        ui->zoomSlider->setEnabled(false);
        return;
    }

    auto *mapLayout = new QGridLayout(ui->mapHost);
    mapLayout->setObjectName(
        m_role == MapWidgetRole::FlightData
            ? QStringLiteral("MapVideoLayout")
            : QStringLiteral("MapLayout"));
    mapLayout->setContentsMargins(0, 0, 0, 0);
    mapLayout->setSpacing(0);
    m_mapBackend->Widget()->setObjectName(QStringLiteral("map"));
    mapLayout->addWidget(m_mapBackend->Widget(), 0, 0);
    for (QLabel *label : {ui->longitudeLabel, ui->latitudeLabel,
                          ui->satsLabel, ui->hdopLabel,
                          ui->fixLabel, ui->zoomLabel}) {
        label->raise();
    }

    // Connect map and toolbar
    ui->toolBar->setMap(m_mapBackend);
    if (m_role == MapWidgetRole::FlightData) {
        for (QLabel *label : {ui->longitudeLabel, ui->latitudeLabel,
                              ui->satsLabel, ui->hdopLabel,
                              ui->fixLabel, ui->zoomLabel}) {
            label->hide();
        }
        ui->toolBar->hide();
        ui->zoomSlider->hide();
        m_flightDataOverlay = new FlightDataMapOverlay(
            m_mapBackend, mapLayout, ui->mapHost);
    }
    // Connect zoom slider and map
    ui->zoomSlider->setMinimum(m_mapBackend->MinZoom() * MapToolZoomFactor);
    ui->zoomSlider->setMaximum(m_mapBackend->MaxZoom() * MapToolZoomFactor);
    setZoom(qRound(m_mapBackend->ZoomReal()));

    connect(ui->zoomSlider, &QSlider::valueChanged,
            this, &QGCMapTool::setMapZoom);
    connect(m_mapBackend, &AbstractMapWidget::ZoomChanged,
            this, &QGCMapTool::setZoom);

    connect(UASManager::instance(),SIGNAL(activeUASSet(UASInterface*)),this,SLOT(activeUASSet(UASInterface*)), Qt::UniqueConnection);

    if (UASManager::instance()->getActiveUAS())
    {
        activeUASSet(UASManager::instance()->getActiveUAS());
    }
}

void QGCMapTool::setMapZoom(int zoom)
{
    if (m_mapBackend) {
        m_mapBackend->SetZoom(
            static_cast<double>(zoom) / MapToolZoomFactor);
    }
}

void QGCMapTool::setZoom(int zoom)
{
    ui->zoomSlider->setValue(zoom * MapToolZoomFactor);
    ui->zoomLabel->setText("ZOOM: " + QString::number(zoom));
}

void QGCMapTool::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    emit visibilityChanged(true);
}

void QGCMapTool::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    emit visibilityChanged(false);
}

QGCMapTool::~QGCMapTool()
{
    delete ui;
}

AbstractMapWidget *QGCMapTool::mapWidget() const
{
    return m_mapBackend;
}

void QGCMapTool::setFlightDataViewModel(FlightDataViewModel *viewModel)
{
    if (m_role != MapWidgetRole::FlightData
        || m_flightDataViewModel == viewModel) {
        return;
    }
    if (m_flightDataViewModel) {
        disconnect(m_flightDataViewModel, nullptr, this, nullptr);
    }
    m_flightDataViewModel = viewModel;
    if (m_flightDataViewModel) {
        connect(m_flightDataViewModel,
                &FlightDataViewModel::telemetryChanged,
                this, &QGCMapTool::updateFlightDataOverlay);
        connect(m_flightDataViewModel, &QObject::destroyed, this, [this]() {
            m_flightDataViewModel = nullptr;
            updateFlightDataOverlay();
        });
    }
    updateFlightDataOverlay();
}

void QGCMapTool::updateFlightDataOverlay()
{
    if (!m_flightDataOverlay) {
        return;
    }
    if (!m_flightDataViewModel) {
        m_flightDataOverlay->setTelemetry(
            0.0, 0.0, 0.0, 0.0, 0.0,
            tr("No mission loaded"), 0.0);
        return;
    }
    m_flightDataOverlay->setTelemetry(
        m_flightDataViewModel->satCount(),
        m_flightDataViewModel->gpsHdop(),
        m_flightDataViewModel->groundSpeed(),
        m_flightDataViewModel->windDir(),
        m_flightDataViewModel->windVel(),
        m_flightDataViewModel->missionProgressText(),
        m_flightDataViewModel->missionProgress());
}

void QGCMapTool::activeUASSet(UASInterface *uasInterface)
{
    QLOG_INFO() << "QGCMapTool::activeUASSet";
    if (m_uasInterface) {
        // The legacy disconnect signatures did not match their connects, so
        // every vehicle switch accumulated live callbacks from old objects.
        disconnect(m_uasInterface.data(), nullptr, this, nullptr);
    }
    m_uasInterface = uasInterface;

    if (!m_uasInterface) {
        ui->latitudeLabel->setText(tr("LAT:"));
        ui->longitudeLabel->setText(tr("LON:"));
        ui->fixLabel->setText(tr("FIX:"));
        ui->satsLabel->setText(tr("SATS:"));
        ui->hdopLabel->setText(tr("HDOP:"));
        return;
    }

    connect(m_uasInterface, SIGNAL(globalPositionChanged(UASInterface*,double,double,double,quint64)),
            this, SLOT(globalPositionUpdate()), Qt::UniqueConnection);
    if (UAS *uas = qobject_cast<UAS *>(m_uasInterface.data())) {
        connect(uas, SIGNAL(gpsHdopChanged(double,QString)),
                this, SLOT(gpsHdopChanged(double,QString)), Qt::UniqueConnection);
        connect(uas, SIGNAL(gpsFixChanged(int,QString)),
                this, SLOT(gpsFixChanged(int,QString)), Qt::UniqueConnection);
        connect(uas, SIGNAL(satelliteCountChanged(int,QString)),
                this, SLOT(satelliteCountChanged(int,QString)), Qt::UniqueConnection);
    }

}

void QGCMapTool::globalPositionUpdate()
{
    if (!m_uasInterface || sender() != m_uasInterface.data()) {
        return;
    }
    ui->latitudeLabel->setText(tr("LAT: %1").arg(m_uasInterface->getLatitude()));
    ui->longitudeLabel->setText(tr("LON: %1").arg(m_uasInterface->getLongitude()));
}

void QGCMapTool::gpsHdopChanged(double value, const QString &)
{
    if (!m_uasInterface || sender() != m_uasInterface.data()) {
        return;
    }
    QString stringHdop = QString::number(value,'g',2);
    ui->hdopLabel->setText(tr("HDOP: %1").arg(stringHdop));
}

void QGCMapTool::gpsFixChanged(int, const QString &)
{
    if (!m_uasInterface || sender() != m_uasInterface.data()) {
        return;
    }
    if (UAS *uas = qobject_cast<UAS *>(m_uasInterface.data())) {
        ui->fixLabel->setText(tr("FIX: %1").arg(uas->getGpsFixString()));
    }
}

void QGCMapTool::satelliteCountChanged(int value, const QString &)
{
    if (!m_uasInterface || sender() != m_uasInterface.data()) {
        return;
    }
    ui->satsLabel->setText(tr("SATS: %1").arg(value));
}
