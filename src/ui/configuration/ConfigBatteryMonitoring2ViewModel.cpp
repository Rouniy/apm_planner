#include "ConfigBatteryMonitoring2ViewModel.h"

ConfigBatteryMonitoring2ViewModel::ConfigBatteryMonitoring2ViewModel(
    QObject *parent)
    : BatteryMonitorInstanceModel(QStringLiteral("BATT2"), 1, parent)
{}

QString ConfigBatteryMonitoring2ViewModel::Title() const
{
    return tr("Battery Monitor 2");
}
