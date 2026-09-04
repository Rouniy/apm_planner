#ifndef CONFIGPLANNERVIEWMODEL_H
#define CONFIGPLANNERVIEWMODEL_H

#include "DisplayViewProfile.h"
#include "PlannerStartupUdpOptions.h"

#include <QObject>
#include <QStringList>
#include <memory>

class QSettings;
class HudDisplaySettings;
class SpeechSettings;

/**
 * Settings-owned, transport-free state for the native Planner page.
 *
 * Application services (audio, links, map renderer and MainWindow) are wired
 * by ConfigPlannerViewIntegration so this model remains deterministic and
 * testable without constructing the application singleton graph.
 */
class ConfigPlannerViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigPlannerViewModel(
        QSettings *settings = nullptr,
        DisplayViewProfileService *profiles = nullptr,
        QObject *parent = nullptr);
    ~ConfigPlannerViewModel() override;

    static ConfigPlannerViewModel *instance();
    static QStringList sectionTitles();

    QString altitudeUnits() const { return m_altitudeUnits; }
    QString distanceUnits() const { return m_distanceUnits; }
    DisplayViewProfile displayProfile() const;
    PlannerStartupUdpOptions startupUdpOptions() const
    {
        return m_startupUdp;
    }
    bool betaUpdatesEnabled() const { return m_betaUpdates; }
    bool hudOverlayEnabled() const;
    bool speechEnabled() const;
    bool speechArmedOnly() const;
    bool speechWaypointEnabled() const;
    bool speechModeEnabled() const;
    bool speechCustomEnabled() const;
    bool speechBatteryEnabled() const;
    bool speechAltWarningEnabled() const;
    bool speechArmDisarmEnabled() const;
    bool speechLowSpeedEnabled() const;
    QString speechWaypointTemplate() const;
    QString speechModeTemplate() const;
    QString speechCustomTemplate() const;
    QString speechBatteryTemplate() const;
    QString speechAltWarningTemplate() const;
    QString speechArmTemplate() const;
    QString speechDisarmTemplate() const;
    QString speechLowGroundSpeedTemplate() const;
    QString speechLowAirSpeedTemplate() const;
    double speechBatteryWarningVoltage() const;
    double speechBatteryWarningPercent() const;
    double speechAltWarningHeightMeters() const;
    bool speechAltWarningHeightConfigured() const;
    double speechLowGroundSpeedTriggerMps() const;
    double speechLowAirSpeedTriggerMps() const;
    QString altitudeUnitLabel() const;
    double altitudeFromMeters(double meters) const;
    double altitudeToMeters(double displayValue) const;
    QString lastError() const { return m_lastError; }

    bool setAltitudeUnits(const QString &units);
    bool setDistanceUnits(const QString &units);
    bool setDisplayPreset(DisplayViewPreset preset);
    bool setStartupUdpOptions(const PlannerStartupUdpOptions &options);
    bool setBetaUpdatesEnabled(bool enabled);
    bool setHudOverlayEnabled(bool enabled);
    bool setSpeechEnabled(bool enabled);
    bool setSpeechArmedOnly(bool enabled);
    bool setSpeechWaypointEnabled(bool enabled);
    bool setSpeechModeEnabled(bool enabled);
    bool setSpeechCustomEnabled(bool enabled);
    bool setSpeechBatteryEnabled(bool enabled);
    bool setSpeechAltWarningEnabled(bool enabled);
    bool setSpeechArmDisarmEnabled(bool enabled);
    bool setSpeechLowSpeedEnabled(bool enabled);
    bool setSpeechWaypointTemplate(const QString &text);
    bool setSpeechModeTemplate(const QString &text);
    bool setSpeechCustomTemplate(const QString &text);
    bool setSpeechBatteryTemplate(const QString &text);
    bool setSpeechAltWarningTemplate(const QString &text);
    bool setSpeechArmTemplate(const QString &text);
    bool setSpeechDisarmTemplate(const QString &text);
    bool setSpeechLowGroundSpeedTemplate(const QString &text);
    bool setSpeechLowAirSpeedTemplate(const QString &text);
    bool setSpeechBatteryWarningVoltage(double voltage);
    bool setSpeechBatteryWarningPercent(double percent);
    bool setSpeechAltWarningHeightMeters(double heightMeters);
    bool setSpeechLowGroundSpeedTriggerMps(double triggerMps);
    bool setSpeechLowAirSpeedTriggerMps(double triggerMps);

public slots:
    void reload();

signals:
    void stateChanged();
    void altitudeUnitsChanged(const QString &units);
    void distanceUnitsChanged(const QString &units);
    void hudOverlayEnabledChanged(bool enabled);
    void speechEnabledChanged(bool enabled);

private:
    static QString canonicalLinearUnits(const QString &units);
    bool syncSettings(const QString &error);

    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    DisplayViewProfileService *m_profiles = nullptr;
    HudDisplaySettings *m_hudSettings = nullptr;
    SpeechSettings *m_speechSettings = nullptr;
    QString m_altitudeUnits = QStringLiteral("Meters");
    QString m_distanceUnits = QStringLiteral("Meters");
    PlannerStartupUdpOptions m_startupUdp;
    bool m_betaUpdates = false;
    QString m_lastError;
};

#endif // CONFIGPLANNERVIEWMODEL_H
