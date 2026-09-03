#ifndef CONFIGPLANNERVIEW_H
#define CONFIGPLANNERVIEW_H

#include "ConfigPlannerViewModel.h"

#include <QList>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QVBoxLayout;

struct ConfigPlannerMapBackend
{
    QString id;
    QString displayName;
};

class ConfigPlannerView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigPlannerView(ConfigPlannerViewModel *viewModel = nullptr,
                               QWidget *parent = nullptr);

    ConfigPlannerViewModel *viewModel() const { return m_viewModel; }

    void setMapBackends(const QList<ConfigPlannerMapBackend> &backends,
                        const QString &selectedId,
                        const QString &status = QString());
    void setMapBackendStatus(const QString &status);
    void setAudioMuted(bool muted);
    void setHeartbeatEnabled(bool enabled);
    void setMavlinkLoggingEnabled(bool enabled);
    void setAutoProxyEnabled(bool enabled);
    void setLogDirectories(const QString &dataFlashDirectory,
                           const QString &tlogDirectory);

public slots:
    void reloadSettings();

signals:
    void altitudeUnitsChanged(const QString &units);
    void distanceUnitsChanged(const QString &units);
    void audioMuteChanged(bool muted);
    void heartbeatChanged(bool enabled);
    void mavlinkLoggingChanged(bool enabled);
    void autoProxyChanged(bool enabled);
    void mapBackendRequested(const QString &backendId);
    void dataFlashLogDirectorySelected(const QString &directory);
    void tlogDirectorySelected(const QString &directory);
    void legacyOptionsRequested();
    void legacyTelemetryOptionsRequested();

private slots:
    void syncFromModel();
    void saveStartupUdpOptions();
    void chooseDataFlashLogDirectory();
    void chooseTlogDirectory();

private:
    QGroupBox *addSection(QVBoxLayout *layout, int index,
                          const QString &objectName);
    QLabel *addUnavailableNote(QVBoxLayout *layout, const QString &text,
                               const QString &objectName);

    QPointer<ConfigPlannerViewModel> m_viewModel;
    QComboBox *m_altitudeUnits = nullptr;
    QComboBox *m_distanceUnits = nullptr;
    QLabel *m_layoutLabel = nullptr;
    QComboBox *m_layout = nullptr;
    QLabel *m_layoutStatus = nullptr;
    QCheckBox *m_audioMute = nullptr;
    QCheckBox *m_heartbeat = nullptr;
    QCheckBox *m_startupUdpEnabled = nullptr;
    QSpinBox *m_startupUdpPrimary = nullptr;
    QSpinBox *m_startupUdpAlternate = nullptr;
    QLabel *m_startupUdpStatus = nullptr;
    QComboBox *m_mapBackend = nullptr;
    QLabel *m_mapBackendStatus = nullptr;
    QCheckBox *m_mavlinkLogging = nullptr;
    QLineEdit *m_dataFlashLogDirectory = nullptr;
    QLineEdit *m_tlogDirectory = nullptr;
    QCheckBox *m_betaUpdates = nullptr;
    QLabel *m_betaStatus = nullptr;
    QCheckBox *m_autoProxy = nullptr;
};

#endif // CONFIGPLANNERVIEW_H
