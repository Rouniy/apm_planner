#ifndef CONFIGPLANNERVIEWINTEGRATION_H
#define CONFIGPLANNERVIEWINTEGRATION_H

class ConfigPlannerView;

// Wires the transport-free native Planner page to the application-owned
// services. Kept outside the view so unit tests never construct MainWindow,
// LinkManager, audio or map singletons.
void BindConfigPlannerViewToApplication(ConfigPlannerView *view);

#endif // CONFIGPLANNERVIEWINTEGRATION_H
