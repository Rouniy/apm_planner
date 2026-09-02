#include "FlightDataView.h"

FlightDataView::FlightDataView(QWidget *parent)
    : DockableView(QStringLiteral("FlightDataView"), parent)
{
    setObjectName(QStringLiteral("FlightDataView"));
}

bool FlightDataView::setHudWidget(QWidget *hudHost)
{
    if (!hudHost) {
        return false;
    }
    hudHost->setMinimumWidth(240);
    return addPanel(hudPanelId(), tr("HUD"), hudHost,
                    PanelLocation::Left, QString(), QSize(440, 320));
}

bool FlightDataView::setMapWidget(QWidget *mapWidget)
{
    if (!mapWidget || !hasPanel(hudPanelId())) {
        return false;
    }
    mapWidget->setMinimumWidth(240);
    const bool added = addPanel(mapPanelId(), tr("Map"), mapWidget,
                                PanelLocation::Right, hudPanelId(),
                                QSize(660, 640), true);
    if (added) {
        setPanelSizeWeights(QStringList{hudPanelId(), mapPanelId()},
                            QList<int>{2, 3}, Qt::Horizontal);
    }
    return added;
}

bool FlightDataView::setInfoView(QWidget *infoWidget)
{
    if (!infoWidget) {
        return false;
    }
    const QString relativePanel = hasPanel(hudPanelId())
        ? hudPanelId() : mapPanelId();
    const bool added = addPanel(infoPanelId(), tr("Info View"), infoWidget,
                                PanelLocation::Bottom, relativePanel,
                                QSize(440, 320));
    if (added && hasPanel(hudPanelId())) {
        // MP10 uses equal-height HUD and FdTabs rows, separated by a 4 px
        // splitter. The outer DATA columns remain 2:3.
        setPanelSizeWeights(QStringList{hudPanelId(), infoPanelId()},
                            QList<int>{1, 1}, Qt::Vertical);
        if (hasPanel(mapPanelId())) {
            setPanelSizeWeights(QStringList{hudPanelId(), mapPanelId()},
                                QList<int>{2, 3}, Qt::Horizontal);
        }
    }
    return added;
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
