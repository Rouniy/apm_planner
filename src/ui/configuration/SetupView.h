#ifndef SETUPVIEW_H
#define SETUPVIEW_H

#include "ApmHardwareConfig.h"

class SetupView final : public ApmHardwareConfig
{
    Q_OBJECT

public:
    explicit SetupView(QWidget *parent = nullptr);
};

#endif
