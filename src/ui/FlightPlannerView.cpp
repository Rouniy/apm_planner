#include "FlightPlannerView.h"

#include <QVariant>

FlightPlannerView::FlightPlannerView(QWidget *parent)
    : SubMainWindow(parent)
{
    setObjectName(QStringLiteral("FlightPlannerView"));
    setProperty("legacyViewObjectName", QStringLiteral("VIEW_MISSION"));
}
