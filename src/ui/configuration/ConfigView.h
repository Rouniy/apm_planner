#ifndef CONFIGVIEW_H
#define CONFIGVIEW_H

#include "ApmSoftwareConfig.h"

class ConfigView final : public ApmSoftwareConfig
{
    Q_OBJECT

public:
    explicit ConfigView(QWidget *parent = nullptr);
};

#endif
