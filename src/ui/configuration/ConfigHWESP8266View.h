#ifndef CONFIGHWESP8266VIEW_H
#define CONFIGHWESP8266VIEW_H

#include "ConfigHWESP8266ViewModel.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;

/*
 * Mission Planner 10 SETUP > ESP8266 Setup page
 * (GCSViews/ConfigurationView/ConfigHWESP8266View.axaml): title, status,
 * SSID / Password / WiFi Channel / UART Baud, the Station (STA) mode box
 * with IP / Gateway / Subnet, Save and Reset to defaults, and the details
 * dump. Every input and both buttons are locked while an operation runs.
 */
class ConfigHWESP8266View final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigHWESP8266View(Esp8266ParameterClient *client, QWidget *parent = nullptr);
    ~ConfigHWESP8266View() override;

    ConfigHWESP8266ViewModel *viewModel() const { return m_viewModel; }
    Esp8266ParameterClient *client() const { return m_viewModel->client(); }

    void setConnected(bool connected);
    bool isConnected() const { return m_viewModel->isConnected(); }

public slots:
    void activate();
    void deactivate();
    void parameterTargetChanged();
    void save();
    void resetDefaults();

private:
    void buildUi();
    void syncControls();
    void selectComboValue(QComboBox *combo, const QString &value);

    ConfigHWESP8266ViewModel *m_viewModel = nullptr;
    QScrollArea *m_scroll = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QLineEdit *m_ssid = nullptr;
    QLineEdit *m_password = nullptr;
    QComboBox *m_channel = nullptr;
    QComboBox *m_baud = nullptr;
    QWidget *m_stationBox = nullptr;
    QCheckBox *m_staMode = nullptr;
    QWidget *m_stationGrid = nullptr;
    QLineEdit *m_ipSta = nullptr;
    QLineEdit *m_gatewaySta = nullptr;
    QLineEdit *m_subnetSta = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_resetButton = nullptr;
    QLabel *m_details = nullptr;
};

#endif
