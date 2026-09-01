#ifndef FLIGHTDATAVIEW_H
#define FLIGHTDATAVIEW_H

#include "docking/DockableView.h"

class FlightDataView final : public DockableView
{
    Q_OBJECT

public:
    explicit FlightDataView(QWidget *parent = nullptr);

    bool setMapWidget(QWidget *mapWidget);
    bool setPrimaryFlightDisplay(QWidget *displayWidget);
    bool setInfoView(QWidget *infoWidget);

    static QString mapPanelId();
    static QString primaryFlightDisplayPanelId();
    static QString infoPanelId();
};

#endif
