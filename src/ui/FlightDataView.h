#ifndef FLIGHTDATAVIEW_H
#define FLIGHTDATAVIEW_H

#include "docking/DockableView.h"

class FlightDataView final : public DockableView
{
    Q_OBJECT

public:
    explicit FlightDataView(QWidget *parent = nullptr);

    bool setHudWidget(QWidget *hudHost);
    bool setMapWidget(QWidget *mapWidget);
    bool setInfoView(QWidget *infoWidget);

    static QString hudPanelId();
    static QString mapPanelId();
    static QString infoPanelId();
};

#endif
