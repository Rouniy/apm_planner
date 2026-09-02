#ifndef MISSIONELEVATIONDISPLAY_H
#define MISSIONELEVATIONDISPLAY_H

#include "flightplanner/MissionElevationProfile.h"

#include <QPointer>
#include <QTimer>
#include <QWidget>

class ElevationSourceService;
class FlightPlannerViewModel;

namespace Ui {
class MissionElevationDisplay;
}

class MissionElevationDisplay final : public QWidget
{
    Q_OBJECT

public:
    explicit MissionElevationDisplay(
        FlightPlannerViewModel *plannerViewModel,
        ElevationSourceService *elevationSource,
        QWidget *parent = nullptr);
    ~MissionElevationDisplay() override;

    const MissionElevationProfileResult &Profile() const;
    bool HasUsableMission() const;

public slots:
    void RebuildProfile();

signals:
    void profileChanged();

private:
    void scheduleProfileRebuild();
    void renderProfile();

    Ui::MissionElevationDisplay *ui = nullptr;
    QPointer<FlightPlannerViewModel> m_plannerViewModel;
    QPointer<ElevationSourceService> m_elevationSource;
    QTimer m_refreshTimer;
    MissionElevationProfileResult m_profile;
};

#endif // MISSIONELEVATIONDISPLAY_H
