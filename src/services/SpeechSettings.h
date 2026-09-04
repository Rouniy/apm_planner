#ifndef SPEECHSETTINGS_H
#define SPEECHSETTINGS_H

#include <QObject>
#include <QMap>
#include <QString>

#include <memory>

class QSettings;

/**
 * Application-owned Mission Planner speech policy.
 *
 * Production callers share instance(). Tests and isolated consumers may
 * inject their own QSettings object, which remains owned by its caller.
 */
class SpeechSettings final : public QObject
{
    Q_OBJECT

public:
    explicit SpeechSettings(QSettings *settings = nullptr,
                            QObject *parent = nullptr);
    ~SpeechSettings() override;

    static SpeechSettings *instance();
    static QString settingsKey();

    bool isEnabled() const { return m_enabled; }
    bool armedOnly() const { return m_armedOnly; }
    bool waypointEnabled() const { return m_waypointEnabled; }
    bool modeEnabled() const { return m_modeEnabled; }
    // Periodic MP10 policy. Altitude is canonical metres and both speed
    // thresholds are canonical metres per second; display conversion belongs
    // at the UI/announcement boundary.
    bool customEnabled() const { return m_customEnabled; }
    bool batteryEnabled() const { return m_batteryEnabled; }
    bool altWarningEnabled() const { return m_altWarningEnabled; }
    bool armDisarmEnabled() const { return m_armDisarmEnabled; }
    bool lowSpeedEnabled() const { return m_lowSpeedEnabled; }

    QString waypointTemplate() const { return m_waypointTemplate; }
    QString modeTemplate() const { return m_modeTemplate; }
    QString customTemplate() const { return m_customTemplate; }
    QString batteryTemplate() const { return m_batteryTemplate; }
    QString altWarningTemplate() const { return m_altWarningTemplate; }
    QString armTemplate() const { return m_armTemplate; }
    QString disarmTemplate() const { return m_disarmTemplate; }
    QString lowGroundSpeedTemplate() const
    {
        return m_lowGroundSpeedTemplate;
    }
    QString lowAirSpeedTemplate() const { return m_lowAirSpeedTemplate; }
    double batteryWarningVoltage() const { return m_batteryWarningVoltage; }
    double batteryWarningPercent() const { return m_batteryWarningPercent; }
    double altWarningHeightMeters() const { return m_altWarningHeightMeters; }
    bool altWarningHeightConfigured() const
    {
        return m_altWarningHeightConfigured;
    }
    double lowGroundSpeedTriggerMps() const
    {
        return m_lowGroundSpeedTriggerMps;
    }
    double lowAirSpeedTriggerMps() const
    {
        return m_lowAirSpeedTriggerMps;
    }

    // Replaces only the explicitly supplied braced tokens (for example
    // {wpn}, {alt}, {gsp}, {asp}, {sysid}) and preserves every unknown token.
    static QString formatTemplate(
        QString speechTemplate,
        const QMap<QString, QString> &replacements);

    QString modeAnnouncement(const QString &mode, int sysid,
                             bool armed) const;
    QString waypointAnnouncement(int wpn, int sysid, bool armed) const;
    QString armStateAnnouncement(bool armed, int sysid) const;

public slots:
    void setEnabled(bool enabled);
    void setArmedOnly(bool armedOnly);
    void setWaypointEnabled(bool enabled);
    void setModeEnabled(bool enabled);
    void setCustomEnabled(bool enabled);
    void setBatteryEnabled(bool enabled);
    void setAltWarningEnabled(bool enabled);
    void setArmDisarmEnabled(bool enabled);
    void setLowSpeedEnabled(bool enabled);
    void setWaypointTemplate(const QString &speechTemplate);
    void setModeTemplate(const QString &speechTemplate);
    void setCustomTemplate(const QString &speechTemplate);
    void setBatteryTemplate(const QString &speechTemplate);
    void setAltWarningTemplate(const QString &speechTemplate);
    void setArmTemplate(const QString &speechTemplate);
    void setDisarmTemplate(const QString &speechTemplate);
    void setLowGroundSpeedTemplate(const QString &speechTemplate);
    void setLowAirSpeedTemplate(const QString &speechTemplate);
    void setBatteryWarningVoltage(double voltage);
    void setBatteryWarningPercent(double percent);
    void setAltWarningHeightMeters(double heightMeters);
    void setLowGroundSpeedTriggerMps(double triggerMps);
    void setLowAirSpeedTriggerMps(double triggerMps);
    void reload();

signals:
    void enabledChanged(bool enabled);
    void policyChanged();

private:
    void readSettings();

    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    bool m_enabled = false;
    bool m_armedOnly = false;
    bool m_waypointEnabled = false;
    bool m_modeEnabled = false;
    bool m_customEnabled = false;
    bool m_batteryEnabled = false;
    bool m_altWarningEnabled = false;
    bool m_armDisarmEnabled = false;
    bool m_lowSpeedEnabled = false;
    QString m_waypointTemplate;
    QString m_modeTemplate;
    QString m_customTemplate;
    QString m_batteryTemplate;
    QString m_altWarningTemplate;
    QString m_armTemplate;
    QString m_disarmTemplate;
    QString m_lowGroundSpeedTemplate;
    QString m_lowAirSpeedTemplate;
    double m_batteryWarningVoltage = 9.6;
    double m_batteryWarningPercent = 20.0;
    double m_altWarningHeightMeters = 2.0;
    bool m_altWarningHeightConfigured = false;
    double m_lowGroundSpeedTriggerMps = 0.0;
    double m_lowAirSpeedTriggerMps = 0.0;
};

#endif // SPEECHSETTINGS_H
