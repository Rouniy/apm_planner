#ifndef PROPAGATIONSETTINGSWINDOW_H
#define PROPAGATIONSETTINGSWINDOW_H

#include "PropagationSettingsStore.h"

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;

/** Mission Planner 10 TOOLS > RF Propagation Settings modeless window. */
class PropagationSettingsWindow final : public QWidget
{
    Q_OBJECT

public:
    static constexpr int WindowWidth = 480;
    static constexpr int WindowHeight = 700;
    static constexpr int MinimumWindowWidth = 420;
    static constexpr int MinimumWindowHeight = 500;

    explicit PropagationSettingsWindow(QWidget *owner = nullptr);
    PropagationSettingsWindow(PropagationSettingsStore &store,
                              QWidget *owner = nullptr);
    ~PropagationSettingsWindow() override;

    /** Creates a fresh independent modeless window on every call. */
    static PropagationSettingsWindow *OpenWindow(QWidget *owner = nullptr);

    PropagationSettings currentSettings() const;

signals:
    void settingsChanged(const PropagationSettings &settings);

private:
    void buildUi(QWidget *owner);
    void wireChanges();
    void syncFromStore();
    void save();
    static void selectChoice(QComboBox *combo, double value);
    static void selectChoice(QComboBox *combo, int value);

    QPointer<PropagationSettingsStore> m_store;
    bool m_loading = true;

    QCheckBox *m_elevation = nullptr;
    QCheckBox *m_terrain = nullptr;
    QCheckBox *m_rf = nullptr;
    QCheckBox *m_droneDistance = nullptr;
    QCheckBox *m_homeDistance = nullptr;
    QCheckBox *m_showScale = nullptr;
    QCheckBox *m_manualAltitude = nullptr;
    QDoubleSpinBox *m_clearance = nullptr;
    QComboBox *m_resolution = nullptr;
    QComboBox *m_azimuthStep = nullptr;
    QComboBox *m_convergence = nullptr;
    QDoubleSpinBox *m_range = nullptr;
    QDoubleSpinBox *m_baseHeight = nullptr;
    QDoubleSpinBox *m_tolerance = nullptr;
    QDoubleSpinBox *m_minimumAltitude = nullptr;
    QDoubleSpinBox *m_maximumAltitude = nullptr;
    QPushButton *m_close = nullptr;
};

#endif // PROPAGATIONSETTINGSWINDOW_H
