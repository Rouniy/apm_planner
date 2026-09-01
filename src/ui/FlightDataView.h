#ifndef FLIGHTDATAVIEW_H
#define FLIGHTDATAVIEW_H

#include "submainwindow.h"

class FlightDataView final : public SubMainWindow
{
    Q_OBJECT

public:
    explicit FlightDataView(QWidget *parent = nullptr);
};

#endif
