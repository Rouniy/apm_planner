#ifndef CONFIGBATTERYMONITORING2VIEWMODEL_H
#define CONFIGBATTERYMONITORING2VIEWMODEL_H

#include "BatteryMonitorInstanceModel.h"

class ConfigBatteryMonitoring2ViewModel final
    : public BatteryMonitorInstanceModel
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)

public:
    explicit ConfigBatteryMonitoring2ViewModel(QObject *parent = nullptr);

    QString Title() const;
};

#endif // CONFIGBATTERYMONITORING2VIEWMODEL_H
