#ifndef ANTENNATRACKERUIVIEW_H
#define ANTENNATRACKERUIVIEW_H

#include "AntennaTrackerUIViewModel.h"

#include <QPointer>
#include <QWidget>

class AntennaTrackerAxisPanel;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;

/*
 * Mission Planner 10 SETUP > "Antenna Tracker (Live)" page
 * (Views/AntennaTrackerUIView.axaml): interface / port / baud, Connect,
 * "Find Trim Pan (SiK Radio)", the servo warning, the Vehicle / Commanded
 * Az / El read-outs, Manual Slew with Home / Center and two sliders, the Pan and
 * Tilt boxes and the status line. ConfigAntennaTrackerView derives the Serial
 * layout from it. The page does not own the view model: both pages share one
 * AntennaTrackerUIViewModel owned by SetupView.
 */
class AntennaTrackerUIView : public QWidget
{
    Q_OBJECT

public:
    enum class PageLayout { Live, Serial };

    explicit AntennaTrackerUIView(AntennaTrackerUIViewModel *viewModel, QWidget *parent = nullptr);
    ~AntennaTrackerUIView() override;

    AntennaTrackerUIViewModel *viewModel() const { return m_viewModel; }
    PageLayout pageLayout() const { return m_layout; }

    QComboBox *interfaceCombo() const { return m_interface; }
    QComboBox *portCombo() const { return m_port; }
    QComboBox *baudCombo() const { return m_baud; }
    QPushButton *connectButton() const { return m_connect; }
    QPushButton *findTrimButton() const { return m_findTrim; }
    QLabel *statusLabel() const { return m_status; }
    AntennaTrackerAxisPanel *panPanel() const { return m_pan; }
    AntennaTrackerAxisPanel *tiltPanel() const { return m_tilt; }
    // Live-only widgets; null on the Serial page.
    QCheckBox *manualModeCheck() const { return m_manualMode; }
    QPushButton *homeCenterButton() const { return m_homeCenter; }
    QSlider *manualAzimuthSlider() const { return m_manualAzimuth; }
    QSlider *manualElevationSlider() const { return m_manualElevation; }

public slots:
    // MP10 IActivationAware / IDeactivationAware forwarded by SetupView.
    void activate();
    void deactivate();

protected:
    AntennaTrackerUIView(AntennaTrackerUIViewModel *viewModel, PageLayout layout, QWidget *parent);

private:
    void buildUi();
    void bindViewModel();
    void syncAll();
    void syncPorts();
    void syncEnabledStates();
    void syncTelemetry();
    void syncManual();
    void selectComboText(QComboBox *combo, const QString &text);
    QString prefix() const;

    QPointer<AntennaTrackerUIViewModel> m_viewModel;
    PageLayout m_layout;
    QScrollArea *m_scroll = nullptr;
    QLabel *m_title = nullptr;
    QComboBox *m_interface = nullptr;
    QComboBox *m_port = nullptr;
    QComboBox *m_baud = nullptr;
    QPushButton *m_connect = nullptr;
    QPushButton *m_findTrim = nullptr;
    QLabel *m_warning = nullptr;
    QFrame *m_telemetryBox = nullptr;
    QLabel *m_vehicleAzimuth = nullptr;
    QLabel *m_vehicleElevation = nullptr;
    QLabel *m_commandedAzimuth = nullptr;
    QLabel *m_commandedElevation = nullptr;
    QFrame *m_manualBox = nullptr;
    QCheckBox *m_manualMode = nullptr;
    QPushButton *m_homeCenter = nullptr;
    QSlider *m_manualAzimuth = nullptr;
    QLabel *m_manualAzimuthLabel = nullptr;
    QSlider *m_manualElevation = nullptr;
    QLabel *m_manualElevationLabel = nullptr;
    AntennaTrackerAxisPanel *m_pan = nullptr;
    AntennaTrackerAxisPanel *m_tilt = nullptr;
    QLabel *m_status = nullptr;
};

#endif // ANTENNATRACKERUIVIEW_H
