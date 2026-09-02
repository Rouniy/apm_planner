#ifndef FLIGHTPLANNERACTIONPANEL_H
#define FLIGHTPLANNERACTIONPANEL_H

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;

class FlightPlannerViewModel;
namespace MissionPlanner { class FenceRallyController; }

class FlightPlannerActionPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit FlightPlannerActionPanel(FlightPlannerViewModel *viewModel,
                                      QWidget *parent = nullptr);

private:
    void loadFile(bool append);
    void saveFile();
    void loadPolygon();
    void savePolygon();
    void offsetPolygon();
    void openSurveyGrid();
    void syncMissionType(const QString &type);
    void syncSaveAvailability();
    void syncMapType(int type);
    void syncStatus(const QString &status);
    void syncTransferState();
    void syncPolygonState();
    void syncAltitudePresentation();

    QPointer<FlightPlannerViewModel> m_viewModel;
    MissionPlanner::FenceRallyController *m_fenceRallyController = nullptr;
    QComboBox *m_mapTypeCombo = nullptr;
    QComboBox *m_missionTypeCombo = nullptr;
    QCheckBox *m_useMavFtp = nullptr;
    QDoubleSpinBox *m_homeLat = nullptr;
    QDoubleSpinBox *m_homeLng = nullptr;
    QDoubleSpinBox *m_homeAlt = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_loadButton = nullptr;
    QPushButton *m_appendButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_polygonDrawButton = nullptr;
    QPushButton *m_polygonClearButton = nullptr;
    QPushButton *m_polygonFromWaypointsButton = nullptr;
    QPushButton *m_polygonLoadButton = nullptr;
    QPushButton *m_polygonSaveButton = nullptr;
    QPushButton *m_polygonOffsetButton = nullptr;
    QPushButton *m_fenceInclusionButton = nullptr;
    QPushButton *m_fenceExclusionButton = nullptr;
    QDoubleSpinBox *m_polygonOffset = nullptr;
    QLabel *m_polygonAreaLabel = nullptr;
    QPushButton *m_surveyGridButton = nullptr;
    QPushButton *m_setHomeButton = nullptr;
    QPushButton *m_readButton = nullptr;
    QPushButton *m_writeButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QProgressBar *m_transferProgress = nullptr;
};

#endif
