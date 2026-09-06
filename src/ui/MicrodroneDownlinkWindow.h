#ifndef MICRODRONEDOWNLINKWINDOW_H
#define MICRODRONEDOWNLINKWINDOW_H

#include "MicrodroneDownlinkViewModel.h"

#include <QWidget>

class QCloseEvent;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;

/** Mission Planner 10 TOOLS > MicroDrone Downlink modeless window. */
class MicrodroneDownlinkWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        MicrodroneDownlinkService::Dependencies service;
        MicrodroneDownlinkViewModel::PortEnumerator enumeratePorts;
    };

    static constexpr int WindowWidth = 580;
    static constexpr int WindowHeight = 440;
    static constexpr int MinimumWidth = 520;
    static constexpr int MinimumHeight = 390;

    explicit MicrodroneDownlinkWindow(QWidget *owner = nullptr);
    MicrodroneDownlinkWindow(Dependencies dependencies,
                             QWidget *owner = nullptr);
    ~MicrodroneDownlinkWindow() override;

    static MicrodroneDownlinkWindow *OpenWindow(QWidget *owner = nullptr);

    MicrodroneDownlinkService *service() const { return m_service; }
    MicrodroneDownlinkViewModel *viewModel() const { return m_viewModel; }
    bool isClosing() const { return m_closing; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    static Dependencies DefaultDependencies();
    void buildUi(QWidget *owner);
    void connectApplicationSignals();
    void syncUi();
    bool syncCombo(QComboBox *combo, const QStringList &items,
                   const QString &selected);

    MicrodroneDownlinkService *m_service = nullptr;
    MicrodroneDownlinkViewModel *m_viewModel = nullptr;
    QComboBox *m_port = nullptr;
    QComboBox *m_baud = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_toggle = nullptr;
    QProgressBar *m_busy = nullptr;
    QLabel *m_source = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_lastLine = nullptr;
    bool m_closing = false;
    bool m_destroying = false;
};

#endif // MICRODRONEDOWNLINKWINDOW_H
