#ifndef DEVICEOPERATIONSWINDOW_H
#define DEVICEOPERATIONSWINDOW_H

#include "DeviceOperationsViewModel.h"

#include <QWidget>

class ExactLinkTransmitter;
class VehicleTargetManager;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

/** Mission Planner 10 TOOLS > MAVLink Device Operations modeless window. */
class DeviceOperationsWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        VehicleTargetManager *targetManager = nullptr;
        ExactLinkTransmitter *transmitter = nullptr;
        DeviceOperationsViewModel::Dependencies viewModel;
        quint8 localSystemId = 255;
        quint8 localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    };

    static constexpr int WindowWidth = 760;
    static constexpr int WindowHeight = 520;
    static constexpr int MinimumWindowWidth = 700;
    static constexpr int MinimumWindowHeight = 420;

    explicit DeviceOperationsWindow(QWidget *owner = nullptr);
    DeviceOperationsWindow(Dependencies dependencies,
                           QWidget *owner = nullptr);
    ~DeviceOperationsWindow() override;

    /** Creates a fresh independent modeless window on every call. */
    static DeviceOperationsWindow *OpenWindow(QWidget *owner = nullptr);

    DeviceOperationService *service() const { return m_service; }
    DeviceOperationsViewModel *viewModel() const { return m_viewModel; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    static Dependencies DefaultDependencies();
    void buildUi(QWidget *owner);
    void connectApplicationSignals();
    void syncUi();

    DeviceOperationService *m_service = nullptr;
    DeviceOperationsViewModel *m_viewModel = nullptr;
    QSpinBox *m_systemId = nullptr;
    QSpinBox *m_componentId = nullptr;
    QComboBox *m_busType = nullptr;
    QLineEdit *m_busName = nullptr;
    QSpinBox *m_busNumber = nullptr;
    QSpinBox *m_address = nullptr;
    QSpinBox *m_registerStart = nullptr;
    QSpinBox *m_count = nullptr;
    QLabel *m_busNameLabel = nullptr;
    QLabel *m_busNumberLabel = nullptr;
    QLabel *m_addressLabel = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QPushButton *m_useActiveTarget = nullptr;
    QPushButton *m_read = nullptr;
    QPushButton *m_test = nullptr;
    QProgressBar *m_progress = nullptr;
};

#endif // DEVICEOPERATIONSWINDOW_H
