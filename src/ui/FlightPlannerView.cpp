#include "FlightPlannerView.h"

FlightPlannerView::FlightPlannerView(QWidget *parent)
    : DockableView(QStringLiteral("FlightPlannerView"), parent)
{
    setObjectName(QStringLiteral("FlightPlannerView"));
}

bool FlightPlannerView::setMapWidget(QWidget *mapWidget)
{
    return addPanel(mapPanelId(), tr("Map"), mapWidget,
                    PanelLocation::Left, QString(), QSize(932, 430), true);
}

bool FlightPlannerView::setWaypointPanel(QWidget *waypointPanelWidget)
{
    if (!waypointPanelWidget) return false;
    const bool added = addPanel(waypointPanelId(), tr("Waypoints"),
                                waypointPanelWidget, PanelLocation::Bottom,
                                mapPanelId(), QSize(932, 210));
    if (added) {
        // The MP10 planner reserves an exact 210 logical-pixel waypoint row.
        waypointPanelWidget->setFixedHeight(210);
        setPanelSizeWeights(QStringList{mapPanelId(), waypointPanelId()},
                            QList<int>{430, 210}, Qt::Vertical);
    }
    return added;
}

bool FlightPlannerView::setActionPanel(QWidget *actionPanelWidget)
{
    if (!actionPanelWidget) return false;
    const bool added = addPanel(actionPanelId(), tr("Actions"),
                                actionPanelWidget, PanelLocation::Right,
                                mapPanelId(), QSize(168, 640));
    if (added) {
        // Mission Planner 10 uses a fixed 168 logical-pixel action column.
        actionPanelWidget->setFixedWidth(168);
        setPanelSizeWeights(QStringList{mapPanelId(), actionPanelId()},
                            QList<int>{932, 168}, Qt::Horizontal);
    }
    return added;
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
