#include "FlightDataView.h"

#include <QVariant>

FlightDataView::FlightDataView(QWidget *parent)
    : SubMainWindow(parent)
{
    setObjectName(QStringLiteral("FlightDataView"));
    setProperty("legacyViewObjectName", QStringLiteral("VIEW_FLIGHT"));
}
