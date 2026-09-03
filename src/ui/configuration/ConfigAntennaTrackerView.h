#ifndef CONFIGANTENNATRACKERVIEW_H
#define CONFIGANTENNATRACKERVIEW_H

#include "AntennaTrackerUIView.h"

/*
 * Mission Planner 10 SETUP > "Antenna Tracker (Serial)" page
 * (GCSViews/ConfigurationView/ConfigAntennaTrackerView.axaml): the same
 * controls as the Live page without the telemetry and manual-slew blocks,
 * with "Rev" checkboxes, a centred "0" under each trim slider and the
 * "Find Trim Pan (Sik Radio)" spelling. Shares the view model with the Live page.
 */
class ConfigAntennaTrackerView final : public AntennaTrackerUIView
{
    Q_OBJECT

public:
    explicit ConfigAntennaTrackerView(AntennaTrackerUIViewModel *viewModel,
                                      QWidget *parent = nullptr);
};

#endif // CONFIGANTENNATRACKERVIEW_H
