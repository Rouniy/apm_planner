#include "FlightPlannerView.h"

FlightPlannerView::FlightPlannerView(QWidget *parent)
    : DockableView(QStringLiteral("FlightPlannerView"), parent)
{
    setObjectName(QStringLiteral("FlightPlannerView"));
}

bool FlightPlannerView::setMapWidget(QWidget *mapWidget)
{
    return addPanel(mapPanelId(), tr("Map"), mapWidget,
                    PanelLocation::Left, QString(), QSize(900, 600));
}

bool FlightPlannerView::setWaypointPanel(QWidget *waypointPanelWidget)
{
    return addPanel(waypointPanelId(), tr("Waypoints"), waypointPanelWidget,
                    PanelLocation::Bottom, mapPanelId(), QSize(900, 250));
}

bool FlightPlannerView::setActionPanel(QWidget *actionPanelWidget)
{
    return addPanel(actionPanelId(), tr("Actions"), actionPanelWidget,
                    PanelLocation::Right, mapPanelId(), QSize(300, 600));
}

QString FlightPlannerView::mapPanelId()
{
    return QStringLiteral("Map");
}

QString FlightPlannerView::waypointPanelId()
{
    return QStringLiteral("WaypointPanel");
}

QString FlightPlannerView::actionPanelId()
{
    return QStringLiteral("ActionPanel");
}
