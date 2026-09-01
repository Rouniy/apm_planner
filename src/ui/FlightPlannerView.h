#ifndef FLIGHTPLANNERVIEW_H
#define FLIGHTPLANNERVIEW_H

#include "docking/DockableView.h"

class FlightPlannerView final : public DockableView
{
    Q_OBJECT

public:
    explicit FlightPlannerView(QWidget *parent = nullptr);

    bool setMapWidget(QWidget *mapWidget);
    bool setWaypointPanel(QWidget *waypointPanelWidget);
    bool setActionPanel(QWidget *actionPanelWidget);

    static QString mapPanelId();
    static QString waypointPanelId();
    static QString actionPanelId();
};

#endif
