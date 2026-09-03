#ifndef SERIALPASSTHROUGHWINDOW_H
#define SERIALPASSTHROUGHWINDOW_H

#include "SerialPassThroughViewModel.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

/** Mission Planner 10 TOOLS > MAVLink Mirror modeless window. */
class SerialPassThroughWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        MavlinkMirrorService::RawWriter rawWriter;
        MavlinkMirrorService::OutputFactory outputFactory;
        MavlinkMirrorService::UdpPortGuard udpPortGuard;
        SerialPassThroughViewModel::Dependencies viewModel;
    };

    static constexpr int WindowWidth = 480;
    static constexpr int WindowHeight = 380;

    explicit SerialPassThroughWindow(QWidget *owner = nullptr);
    SerialPassThroughWindow(Dependencies dependencies,
                            QWidget *owner = nullptr);
    ~SerialPassThroughWindow() override;

    /** Creates a new independent modeless top-level window on every call. */
    static SerialPassThroughWindow *OpenWindow(QWidget *owner = nullptr);

    MavlinkMirrorService *service() const { return m_service; }
    SerialPassThroughViewModel *viewModel() const { return m_viewModel; }

private:
    static Dependencies DefaultDependencies();
    void buildUi(QWidget *owner);
    void connectApplicationSignals();
    void syncUi();

    MavlinkMirrorService *m_service = nullptr;
    SerialPassThroughViewModel *m_viewModel = nullptr;
    QComboBox *m_port = nullptr;
    QComboBox *m_baud = nullptr;
    QCheckBox *m_writeBack = nullptr;
    QPushButton *m_toggle = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_txBytes = nullptr;
    QLabel *m_rxBytes = nullptr;
};

#endif // SERIALPASSTHROUGHWINDOW_H
