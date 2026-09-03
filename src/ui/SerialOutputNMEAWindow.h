#ifndef SERIALOUTPUTNMEAWINDOW_H
#define SERIALOUTPUTNMEAWINDOW_H

#include "SerialOutputNMEAViewModel.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;

/** Mission Planner 10 TOOLS > NMEA Output modeless window. */
class SerialOutputNMEAWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        NmeaOutputService::OutputFactory outputFactory;
        NmeaOutputService::UdpPortGuard udpPortGuard;
        NmeaOutputService::Clock clock;
        SerialOutputNMEAViewModel::Dependencies viewModel;
    };

    static constexpr int WindowWidth = 480;
    static constexpr int WindowHeight = 400;

    explicit SerialOutputNMEAWindow(QWidget *owner = nullptr);
    SerialOutputNMEAWindow(Dependencies dependencies,
                           QWidget *owner = nullptr);
    ~SerialOutputNMEAWindow() override;

    static SerialOutputNMEAWindow *OpenWindow(QWidget *owner = nullptr);

    NmeaOutputService *service() const { return m_service; }
    SerialOutputNMEAViewModel *viewModel() const { return m_viewModel; }

private:
    static Dependencies DefaultDependencies();
    void buildUi(QWidget *owner);
    void connectApplicationSignals();
    void syncUi();

    NmeaOutputService *m_service = nullptr;
    SerialOutputNMEAViewModel *m_viewModel = nullptr;
    QComboBox *m_port = nullptr;
    QComboBox *m_baud = nullptr;
    QComboBox *m_rate = nullptr;
    QPushButton *m_toggle = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_lastSentence = nullptr;
};

#endif // SERIALOUTPUTNMEAWINDOW_H
