#include "FlightDataView.h"

FlightDataView::FlightDataView(QWidget *parent)
    : DockableView(QStringLiteral("FlightDataView"), parent)
{
    setObjectName(QStringLiteral("FlightDataView"));
}

bool FlightDataView::setMapWidget(QWidget *mapWidget)
{
    return addPanel(mapPanelId(), tr("Map"), mapWidget,
                    PanelLocation::Left, QString(), QSize(760, 640));
}

bool FlightDataView::setPrimaryFlightDisplay(QWidget *displayWidget)
{
    return addPanel(primaryFlightDisplayPanelId(), tr("Primary Flight Display"),
                    displayWidget, PanelLocation::Left, mapPanelId(),
                    QSize(440, 360));
}

bool FlightDataView::setInfoView(QWidget *infoWidget)
{
    const QString relativePanel = hasPanel(primaryFlightDisplayPanelId())
        ? primaryFlightDisplayPanelId() : mapPanelId();
    return addPanel(infoPanelId(), tr("Info View"), infoWidget,
                    PanelLocation::Bottom, relativePanel, QSize(440, 280));
}

QString FlightDataView::mapPanelId()
{
    return QStringLiteral("FdMap");
}

QString FlightDataView::primaryFlightDisplayPanelId()
{
    return QStringLiteral("HudHost");
}

QString FlightDataView::infoPanelId()
{
    return QStringLiteral("FdTabs");
}
