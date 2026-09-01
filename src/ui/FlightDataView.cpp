#include "FlightDataView.h"

FlightDataView::FlightDataView(QWidget *parent)
    : DockableView(QStringLiteral("FlightDataView"), parent)
{
    setObjectName(QStringLiteral("FlightDataView"));
}

bool FlightDataView::setHudWidget(QWidget *hudHost)
{
    return addPanel(hudPanelId(), tr("HUD"), hudHost,
                    PanelLocation::Left, QString(), QSize(440, 360));
}

bool FlightDataView::setMapWidget(QWidget *mapWidget)
{
    if (!hasPanel(hudPanelId())) {
        return false;
    }
    const bool added = addPanel(mapPanelId(), tr("Map"), mapWidget,
                                PanelLocation::Right, hudPanelId(), QSize(660, 640));
    if (added) {
        setPanelSizeWeights(QStringList{hudPanelId(), mapPanelId()},
                            QList<int>{2, 3}, Qt::Horizontal);
    }
    return added;
}

bool FlightDataView::setInfoView(QWidget *infoWidget)
{
    const QString relativePanel = hasPanel(hudPanelId())
        ? hudPanelId() : mapPanelId();
    return addPanel(infoPanelId(), tr("Info View"), infoWidget,
                    PanelLocation::Bottom, relativePanel, QSize(440, 280));
}

QString FlightDataView::hudPanelId()
{
    return QStringLiteral("HudHost");
}

QString FlightDataView::mapPanelId()
{
    return QStringLiteral("FdMap");
}

QString FlightDataView::infoPanelId()
{
    return QStringLiteral("FdTabs");
}
