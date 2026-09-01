#ifndef FLIGHTPLANNERACTIONPANEL_H
#define FLIGHTPLANNERACTIONPANEL_H

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

class FlightPlannerViewModel;

class FlightPlannerActionPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit FlightPlannerActionPanel(FlightPlannerViewModel *viewModel,
                                      QWidget *parent = nullptr);

private:
    void loadFile(bool append);
    void saveFile();
    void setHomeFromVehicle();
    void syncMissionType(const QString &type);
    void syncSaveAvailability();
    void syncMapType(int type);
    void syncStatus(const QString &status);

    QPointer<FlightPlannerViewModel> m_viewModel;
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
    QPushButton *m_setHomeButton = nullptr;
};

#endif
