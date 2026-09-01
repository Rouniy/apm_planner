#include "FlightPlannerView.h"

#include <QPointer>
#include <QTimer>

FlightPlannerView::FlightPlannerView(QWidget *parent)
    : DockableView(QStringLiteral("FlightPlannerView"), parent)
{
    setObjectName(QStringLiteral("FlightPlannerView"));
}

bool FlightPlannerView::setMapWidget(QWidget *mapWidget)
{
    return addPanel(mapPanelId(), tr("Map"), mapWidget,
                    PanelLocation::Left, QString(), QSize(932, 430));
}

bool FlightPlannerView::setWaypointPanel(QWidget *waypointPanelWidget)
{
    if (!waypointPanelWidget) return false;
    waypointPanelWidget->setMaximumHeight(210);
    const bool added = addPanel(waypointPanelId(), tr("Waypoints"),
                                waypointPanelWidget, PanelLocation::Bottom,
                                mapPanelId(), QSize(932, 210));
    if (added) {
        setPanelSizeWeights(QStringList{mapPanelId(), waypointPanelId()},
                            QList<int>{430, 210}, Qt::Vertical);
        const QPointer<QWidget> panel(waypointPanelWidget);
        QTimer::singleShot(0, this, [panel]() {
            if (panel) panel->setMaximumHeight(QWIDGETSIZE_MAX);
        });
    } else {
        waypointPanelWidget->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    return added;
}

bool FlightPlannerView::setActionPanel(QWidget *actionPanelWidget)
{
    if (!actionPanelWidget) return false;
    actionPanelWidget->setMaximumWidth(168);
    const bool added = addPanel(actionPanelId(), tr("Actions"),
                                actionPanelWidget, PanelLocation::Right,
                                mapPanelId(), QSize(168, 640));
    if (added) {
        setPanelSizeWeights(QStringList{mapPanelId(), actionPanelId()},
                            QList<int>{932, 168}, Qt::Horizontal);
        const QPointer<QWidget> panel(actionPanelWidget);
        QTimer::singleShot(0, this, [panel]() {
            if (panel) panel->setMaximumWidth(QWIDGETSIZE_MAX);
        });
    } else {
        actionPanelWidget->setMaximumWidth(QWIDGETSIZE_MAX);
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
