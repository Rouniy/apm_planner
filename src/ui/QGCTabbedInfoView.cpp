#include "QGCTabbedInfoView.h"

#include "flightdata/FlightDataViewModel.h"
#include "flightdata/PreflightChecklistModel.h"
#include "flightdata/PreflightChecklistWidget.h"
#include "flightdata/SimpleActionsWidget.h"

QGCTabbedInfoView::QGCTabbedInfoView(QWidget *parent) : QWidget(parent)
{
    ui.setupUi(this);
    messageView = new QGCMessageView(this);
    actionsWidget = new UASActionsWidget(this);
    quickView = new UASQuickView(this);
    rawView = new UASRawStatusView(this);
    m_preflightModel = new PreflightChecklistModel(this);
    m_preflightWidget = new PreflightChecklistWidget(m_preflightModel, this);
    m_simpleActionsWidget = new SimpleActionsWidget(this);

    ui.tabWidget->setObjectName(QStringLiteral("FdTabs"));
    ui.tabWidget->addTab(quickView, tr("Quick"));
    ui.tabWidget->addTab(actionsWidget, tr("Actions"));
    ui.tabWidget->addTab(messageView, tr("Messages"));
    ui.tabWidget->addTab(m_simpleActionsWidget, tr("Simple Actions"));
    ui.tabWidget->addTab(m_preflightWidget, tr("PreFlight"));
    ui.tabWidget->addTab(rawView, tr("Status"));

    connect(m_simpleActionsWidget, &SimpleActionsWidget::quickModeRequested,
            actionsWidget, &UASActionsWidget::requestQuickMode);
    connect(actionsWidget,
            &UASActionsWidget::commandSurfaceAvailableChanged,
            m_simpleActionsWidget,
            &SimpleActionsWidget::setActionsAvailable);
    connect(actionsWidget,
            &UASActionsWidget::commandSurfaceAvailableChanged,
            this, [this](bool) { syncPreflightTelemetry(); });
    connect(actionsWidget, &UASActionsWidget::actionStatusChanged,
            m_simpleActionsWidget, &SimpleActionsWidget::setActionStatus);
    connect(actionsWidget, &UASActionsWidget::clearTrackRequested,
            this, &QGCTabbedInfoView::clearTrackRequested);
    connect(actionsWidget, &UASActionsWidget::joystickSetupRequested,
            this, &QGCTabbedInfoView::joystickSetupRequested);
    connect(actionsWidget, &UASActionsWidget::rawSensorViewRequested,
            this, [this]() { ui.tabWidget->setCurrentWidget(rawView); });
    m_simpleActionsWidget->setActionsAvailable(
        actionsWidget->commandSurfaceAvailable());
    m_simpleActionsWidget->setActionStatus(
        actionsWidget->actionStatus(),
        !actionsWidget->commandSurfaceAvailable());
}
void QGCTabbedInfoView::addSource(MAVLinkDecoder *decoder)
{
    m_decoder = decoder;
    rawView->addSource(decoder);
    quickView->addSource(decoder);
}

QGCTabbedInfoView::~QGCTabbedInfoView()
{
}

void QGCTabbedInfoView::installQuickView(QWidget *view)
{
    if (!view || ui.tabWidget->indexOf(view) >= 0) return;
    // Keep the useful raw-value legacy selector available without using its
    // system-id-only graph for exact-target Warning Manager colors.
    const int index = ui.tabWidget->indexOf(quickView);
    if (index >= 0) {
        ui.tabWidget->removeTab(index);
        ui.tabWidget->insertTab(index, view, tr("Quick"));
        ui.tabWidget->addTab(quickView, tr("Quick (Legacy)"));
        ui.tabWidget->setCurrentWidget(view);
    }
}

void QGCTabbedInfoView::setFlightDataViewModel(
    FlightDataViewModel *viewModel)
{
    if (m_flightDataViewModel == viewModel) {
        syncPreflightTelemetry();
        return;
    }
    if (m_flightDataViewModel) {
        disconnect(m_flightDataViewModel, nullptr, this, nullptr);
    }
    m_flightDataViewModel = viewModel;
    if (m_flightDataViewModel) {
        connect(m_flightDataViewModel,
                &FlightDataViewModel::telemetryChanged,
                this, &QGCTabbedInfoView::syncPreflightTelemetry);
        connect(m_flightDataViewModel,
                &FlightDataViewModel::activeUASChanged,
                this, [this](UASInterface *) { syncPreflightTelemetry(); });
        connect(m_flightDataViewModel, &QObject::destroyed,
                this, [this]() {
                    m_flightDataViewModel = nullptr;
                    syncPreflightTelemetry();
                });
    }
    syncPreflightTelemetry();
}

PreflightChecklistModel *QGCTabbedInfoView::preflightChecklistModel() const
{
    return m_preflightModel;
}

SimpleActionsWidget *QGCTabbedInfoView::simpleActionsWidget() const
{
    return m_simpleActionsWidget;
}

void QGCTabbedInfoView::syncPreflightTelemetry()
{
    if (!m_preflightModel) {
        return;
    }
    PreflightTelemetry telemetry;
    if (m_flightDataViewModel) {
        UASInterface *uas = m_flightDataViewModel->activeUAS();
        telemetry.connected = uas && actionsWidget
            && actionsWidget->vehicleConnected();
        telemetry.gpsFixType = m_flightDataViewModel->gpsFixType();
        telemetry.satelliteCount = qRound(m_flightDataViewModel->satCount());
        telemetry.linkQuality = m_flightDataViewModel->linkQuality();
        telemetry.batteryVoltage = m_flightDataViewModel->batteryVoltage();
        telemetry.mode = m_flightDataViewModel->mode();
        telemetry.altitude = m_flightDataViewModel->alt();
    }
    m_preflightModel->setTelemetry(telemetry);
}
