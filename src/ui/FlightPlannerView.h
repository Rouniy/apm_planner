#ifndef FLIGHTPLANNERVIEW_H
#define FLIGHTPLANNERVIEW_H

#include "submainwindow.h"

class FlightPlannerView final : public SubMainWindow
{
    Q_OBJECT

public:
    explicit FlightPlannerView(QWidget *parent = nullptr);
};

#endif
