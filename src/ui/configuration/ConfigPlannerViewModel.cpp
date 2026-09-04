#include "ConfigPlannerViewModel.h"

#include "services/HudDisplaySettings.h"
#include "services/SpeechSettings.h"

#include <QCoreApplication>
#include <QSettings>

#include <cmath>

namespace {
constexpr double kFeetPerMeter = 3.280839895013123;
}

ConfigPlannerViewModel::ConfigPlannerViewModel(
    QSettings *settings, DisplayViewProfileService *profiles, QObject *parent)
    : QObject(parent),
      m_profiles(profiles ? profiles : DisplayViewProfileService::instance())
{
    if (settings) {
        m_settings = settings;
        m_hudSettings = new HudDisplaySettings(settings, this);
        m_speechSettings = new SpeechSettings(settings, this);
    } else {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
        m_hudSettings = HudDisplaySettings::instance();
        m_speechSettings = SpeechSettings::instance();
    }
    m_settings->setFallbacksEnabled(false);
    connect(m_profiles, &DisplayViewProfileService::changed,
            this, &ConfigPlannerViewModel::stateChanged);
    connect(m_hudSettings, &HudDisplaySettings::overlayEnabledChanged,
            this, [this](bool enabled) {
        emit hudOverlayEnabledChanged(enabled);
        emit stateChanged();
    });
    connect(m_speechSettings, &SpeechSettings::enabledChanged,
            this, [this](bool enabled) {
        emit speechEnabledChanged(enabled);
    });
    connect(m_speechSettings, &SpeechSettings::policyChanged,
            this, &ConfigPlannerViewModel::stateChanged);
    reload();
}

ConfigPlannerViewModel::~ConfigPlannerViewModel() = default;

ConfigPlannerViewModel *ConfigPlannerViewModel::instance()
{
    static ConfigPlannerViewModel *const singleton =
        new ConfigPlannerViewModel(nullptr, nullptr,
                                   QCoreApplication::instance());
    return singleton;
}

QStringList ConfigPlannerViewModel::sectionTitles()
{
    return {
        QStringLiteral("Display"),
        QStringLiteral("Speech"),
        QStringLiteral("Flight Command Shortcuts"),
        QStringLiteral("Waypoints / Connect"),
        QStringLiteral("Startup UDP Listeners"),
        QStringLiteral("Telemetry Stream Rates (Hz)"),
        QStringLiteral("Aircraft Icon / Map"),
        QStringLiteral("Logs"),
        QStringLiteral("Advanced")
    };
}

QString ConfigPlannerViewModel::canonicalLinearUnits(const QString &units)
{
    const QString value = units.trimmed();
    if (value.compare(QStringLiteral("Meters"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Meters");
    }
    if (value.compare(QStringLiteral("Feet"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Feet");
    }
    return QString();
}

DisplayViewProfile ConfigPlannerViewModel::displayProfile() const
{
    return m_profiles->current();
}

bool ConfigPlannerViewModel::hudOverlayEnabled() const
{
    return m_hudSettings && m_hudSettings->overlayEnabled();
}

bool ConfigPlannerViewModel::speechEnabled() const
{
    return m_speechSettings && m_speechSettings->isEnabled();
}

bool ConfigPlannerViewModel::speechArmedOnly() const
{
    return m_speechSettings && m_speechSettings->armedOnly();
}

bool ConfigPlannerViewModel::speechWaypointEnabled() const
{
    return m_speechSettings && m_speechSettings->waypointEnabled();
}

bool ConfigPlannerViewModel::speechModeEnabled() const
{
    return m_speechSettings && m_speechSettings->modeEnabled();
}

bool ConfigPlannerViewModel::speechCustomEnabled() const
{
    return m_speechSettings && m_speechSettings->customEnabled();
}

bool ConfigPlannerViewModel::speechBatteryEnabled() const
{
    return m_speechSettings && m_speechSettings->batteryEnabled();
}

bool ConfigPlannerViewModel::speechAltWarningEnabled() const
{
    return m_speechSettings && m_speechSettings->altWarningEnabled();
}

bool ConfigPlannerViewModel::speechArmDisarmEnabled() const
{
    return m_speechSettings && m_speechSettings->armDisarmEnabled();
}

bool ConfigPlannerViewModel::speechLowSpeedEnabled() const
{
    return m_speechSettings && m_speechSettings->lowSpeedEnabled();
}

QString ConfigPlannerViewModel::speechWaypointTemplate() const
{
    return m_speechSettings ? m_speechSettings->waypointTemplate() : QString();
}

QString ConfigPlannerViewModel::speechModeTemplate() const
{
    return m_speechSettings ? m_speechSettings->modeTemplate() : QString();
}

QString ConfigPlannerViewModel::speechCustomTemplate() const
{
    return m_speechSettings ? m_speechSettings->customTemplate() : QString();
}

QString ConfigPlannerViewModel::speechBatteryTemplate() const
{
    return m_speechSettings ? m_speechSettings->batteryTemplate() : QString();
}

QString ConfigPlannerViewModel::speechAltWarningTemplate() const
{
    return m_speechSettings
        ? m_speechSettings->altWarningTemplate() : QString();
}

QString ConfigPlannerViewModel::speechArmTemplate() const
{
    return m_speechSettings ? m_speechSettings->armTemplate() : QString();
}

QString ConfigPlannerViewModel::speechDisarmTemplate() const
{
    return m_speechSettings ? m_speechSettings->disarmTemplate() : QString();
}

QString ConfigPlannerViewModel::speechLowGroundSpeedTemplate() const
{
    return m_speechSettings
        ? m_speechSettings->lowGroundSpeedTemplate() : QString();
}

QString ConfigPlannerViewModel::speechLowAirSpeedTemplate() const
{
    return m_speechSettings
        ? m_speechSettings->lowAirSpeedTemplate() : QString();
}

double ConfigPlannerViewModel::speechBatteryWarningVoltage() const
{
    return m_speechSettings
        ? m_speechSettings->batteryWarningVoltage() : 9.6;
}

double ConfigPlannerViewModel::speechBatteryWarningPercent() const
{
    return m_speechSettings
        ? m_speechSettings->batteryWarningPercent() : 20.0;
}

double ConfigPlannerViewModel::speechAltWarningHeightMeters() const
{
    return m_speechSettings
        ? m_speechSettings->altWarningHeightMeters() : 2.0;
}

bool ConfigPlannerViewModel::speechAltWarningHeightConfigured() const
{
    return m_speechSettings
        && m_speechSettings->altWarningHeightConfigured();
}

double ConfigPlannerViewModel::speechLowGroundSpeedTriggerMps() const
{
    return m_speechSettings
        ? m_speechSettings->lowGroundSpeedTriggerMps() : 0.0;
}

double ConfigPlannerViewModel::speechLowAirSpeedTriggerMps() const
{
    return m_speechSettings
        ? m_speechSettings->lowAirSpeedTriggerMps() : 0.0;
}

QString ConfigPlannerViewModel::altitudeUnitLabel() const
{
    return m_altitudeUnits == QStringLiteral("Feet")
        ? QStringLiteral("ft") : QStringLiteral("m");
}

double ConfigPlannerViewModel::altitudeFromMeters(double meters) const
{
    return m_altitudeUnits == QStringLiteral("Feet")
        ? meters * kFeetPerMeter : meters;
}

double ConfigPlannerViewModel::altitudeToMeters(double displayValue) const
{
    return m_altitudeUnits == QStringLiteral("Feet")
        ? displayValue / kFeetPerMeter : displayValue;
}

void ConfigPlannerViewModel::reload()
{
    m_settings->sync();
    m_hudSettings->reload();
    m_speechSettings->reload();
    m_altitudeUnits = canonicalLinearUnits(m_settings->value(
        QStringLiteral("altunits"), QStringLiteral("Meters")).toString());
    if (m_altitudeUnits.isEmpty()) {
        m_altitudeUnits = QStringLiteral("Meters");
    }
    m_distanceUnits = canonicalLinearUnits(m_settings->value(
        QStringLiteral("distunits"), QStringLiteral("Meters")).toString());
    if (m_distanceUnits.isEmpty()) {
        m_distanceUnits = QStringLiteral("Meters");
    }
    m_startupUdp = PlannerStartupUdpOptions::load(*m_settings);
    m_settings->beginGroup(QStringLiteral("AUTO_UPDATE"));
    m_betaUpdates = m_settings->value(
        QStringLiteral("RELEASE_TYPE"), QStringLiteral("stable"))
                            .toString().trimmed()
                            .compare(QStringLiteral("beta"),
                                     Qt::CaseInsensitive) == 0;
    m_settings->endGroup();
    m_lastError.clear();
    emit stateChanged();
}

bool ConfigPlannerViewModel::syncSettings(const QString &error)
{
    m_settings->sync();
    if (m_settings->status() == QSettings::NoError) {
        m_lastError.clear();
        return true;
    }
    m_lastError = error;
    return false;
}

bool ConfigPlannerViewModel::setAltitudeUnits(const QString &units)
{
    const QString canonical = canonicalLinearUnits(units);
    if (canonical.isEmpty()) {
        m_lastError = tr("Unsupported altitude unit.");
        return false;
    }
    if (canonical == m_altitudeUnits) {
        m_lastError.clear();
        return true;
    }
    m_altitudeUnits = canonical;
    m_lastError.clear();
    emit altitudeUnitsChanged(canonical);
    emit stateChanged();
    return true;
}

bool ConfigPlannerViewModel::setDistanceUnits(const QString &units)
{
    const QString canonical = canonicalLinearUnits(units);
    if (canonical.isEmpty()) {
        m_lastError = tr("Unsupported distance unit.");
        return false;
    }
    if (canonical == m_distanceUnits) {
        m_lastError.clear();
        return true;
    }
    m_distanceUnits = canonical;
    m_lastError.clear();
    emit distanceUnitsChanged(canonical);
    emit stateChanged();
    return true;
}

bool ConfigPlannerViewModel::setDisplayPreset(DisplayViewPreset preset)
{
    if (!m_profiles->applyPreset(preset, &m_lastError)) {
        return false;
    }
    m_lastError.clear();
    return true;
}

bool ConfigPlannerViewModel::setStartupUdpOptions(
    const PlannerStartupUdpOptions &options)
{
    const PlannerStartupUdpOptions normalized = options.normalized();
    normalized.save(*m_settings);
    if (!syncSettings(tr("Could not save Startup UDP listener settings."))) {
        return false;
    }
    m_startupUdp = normalized;
    emit stateChanged();
    return true;
}

bool ConfigPlannerViewModel::setBetaUpdatesEnabled(bool enabled)
{
    if (m_betaUpdates == enabled) {
        return true;
    }
    m_settings->beginGroup(QStringLiteral("AUTO_UPDATE"));
    m_settings->setValue(QStringLiteral("RELEASE_TYPE"),
                         enabled ? QStringLiteral("beta")
                                 : QStringLiteral("stable"));
    m_settings->endGroup();
    if (!syncSettings(tr("Could not save the update channel."))) {
        return false;
    }
    m_betaUpdates = enabled;
    emit stateChanged();
    return true;
}

bool ConfigPlannerViewModel::setHudOverlayEnabled(bool enabled)
{
    if (!m_hudSettings) {
        m_lastError = tr("HUD settings are unavailable.");
        return false;
    }
    m_hudSettings->setOverlayEnabled(enabled);
    m_lastError.clear();
    return m_hudSettings->overlayEnabled() == enabled;
}

bool ConfigPlannerViewModel::setSpeechEnabled(bool enabled)
{
    if (!m_speechSettings) {
        m_lastError = tr("Speech settings are unavailable.");
        return false;
    }
    m_speechSettings->setEnabled(enabled);
    m_lastError.clear();
    return m_speechSettings->isEnabled() == enabled;
}

bool ConfigPlannerViewModel::setSpeechArmedOnly(bool enabled)
{
    if (!m_speechSettings) return false;
    m_speechSettings->setArmedOnly(enabled);
    return m_speechSettings->armedOnly() == enabled;
}

bool ConfigPlannerViewModel::setSpeechWaypointEnabled(bool enabled)
{
    if (!m_speechSettings) return false;
    m_speechSettings->setWaypointEnabled(enabled);
    return m_speechSettings->waypointEnabled() == enabled;
}

bool ConfigPlannerViewModel::setSpeechModeEnabled(bool enabled)
{
    if (!m_speechSettings) return false;
    m_speechSettings->setModeEnabled(enabled);
    return m_speechSettings->modeEnabled() == enabled;
}

bool ConfigPlannerViewModel::setSpeechCustomEnabled(bool enabled)
{
    if (!m_speechSettings) return false;
    m_speechSettings->setCustomEnabled(enabled);
    return m_speechSettings->customEnabled() == enabled;
}

bool ConfigPlannerViewModel::setSpeechBatteryEnabled(bool enabled)
{
    if (!m_speechSettings) return false;
    m_speechSettings->setBatteryEnabled(enabled);
    return m_speechSettings->batteryEnabled() == enabled;
}

bool ConfigPlannerViewModel::setSpeechAltWarningEnabled(bool enabled)
{
    if (!m_speechSettings) return false;
    m_speechSettings->setAltWarningEnabled(enabled);
    return m_speechSettings->altWarningEnabled() == enabled;
}

bool ConfigPlannerViewModel::setSpeechArmDisarmEnabled(bool enabled)
{
    if (!m_speechSettings) return false;
    m_speechSettings->setArmDisarmEnabled(enabled);
    return m_speechSettings->armDisarmEnabled() == enabled;
}

bool ConfigPlannerViewModel::setSpeechLowSpeedEnabled(bool enabled)
{
    if (!m_speechSettings) return false;
    m_speechSettings->setLowSpeedEnabled(enabled);
    return m_speechSettings->lowSpeedEnabled() == enabled;
}

bool ConfigPlannerViewModel::setSpeechWaypointTemplate(const QString &text)
{
    if (!m_speechSettings || text.isEmpty()) return false;
    m_speechSettings->setWaypointTemplate(text);
    return m_speechSettings->waypointTemplate() == text;
}

bool ConfigPlannerViewModel::setSpeechModeTemplate(const QString &text)
{
    if (!m_speechSettings || text.isEmpty()) return false;
    m_speechSettings->setModeTemplate(text);
    return m_speechSettings->modeTemplate() == text;
}

bool ConfigPlannerViewModel::setSpeechCustomTemplate(const QString &text)
{
    if (!m_speechSettings || text.isEmpty()) return false;
    m_speechSettings->setCustomTemplate(text);
    return m_speechSettings->customTemplate() == text;
}

bool ConfigPlannerViewModel::setSpeechBatteryTemplate(const QString &text)
{
    if (!m_speechSettings || text.isEmpty()) return false;
    m_speechSettings->setBatteryTemplate(text);
    return m_speechSettings->batteryTemplate() == text;
}

bool ConfigPlannerViewModel::setSpeechAltWarningTemplate(const QString &text)
{
    if (!m_speechSettings || text.isEmpty()) return false;
    m_speechSettings->setAltWarningTemplate(text);
    return m_speechSettings->altWarningTemplate() == text;
}

bool ConfigPlannerViewModel::setSpeechArmTemplate(const QString &text)
{
    if (!m_speechSettings || text.isEmpty()) return false;
    m_speechSettings->setArmTemplate(text);
    return m_speechSettings->armTemplate() == text;
}

bool ConfigPlannerViewModel::setSpeechDisarmTemplate(const QString &text)
{
    if (!m_speechSettings || text.isEmpty()) return false;
    m_speechSettings->setDisarmTemplate(text);
    return m_speechSettings->disarmTemplate() == text;
}

bool ConfigPlannerViewModel::setSpeechLowGroundSpeedTemplate(
    const QString &text)
{
    if (!m_speechSettings || text.isEmpty()) return false;
    m_speechSettings->setLowGroundSpeedTemplate(text);
    return m_speechSettings->lowGroundSpeedTemplate() == text;
}

bool ConfigPlannerViewModel::setSpeechLowAirSpeedTemplate(
    const QString &text)
{
    if (!m_speechSettings || text.isEmpty()) return false;
    m_speechSettings->setLowAirSpeedTemplate(text);
    return m_speechSettings->lowAirSpeedTemplate() == text;
}

bool ConfigPlannerViewModel::setSpeechBatteryWarningVoltage(double voltage)
{
    if (!m_speechSettings || !std::isfinite(voltage) || voltage < 0.0) {
        return false;
    }
    m_speechSettings->setBatteryWarningVoltage(voltage);
    return qFuzzyCompare(
        m_speechSettings->batteryWarningVoltage() + 1.0, voltage + 1.0);
}

bool ConfigPlannerViewModel::setSpeechBatteryWarningPercent(double percent)
{
    if (!m_speechSettings || !std::isfinite(percent) || percent < 0.0) {
        return false;
    }
    m_speechSettings->setBatteryWarningPercent(percent);
    return qFuzzyCompare(
        m_speechSettings->batteryWarningPercent() + 1.0, percent + 1.0);
}

bool ConfigPlannerViewModel::setSpeechAltWarningHeightMeters(
    double heightMeters)
{
    if (!m_speechSettings || !std::isfinite(heightMeters)
        || heightMeters < 0.0) {
        return false;
    }
    m_speechSettings->setAltWarningHeightMeters(heightMeters);
    return qFuzzyCompare(
        m_speechSettings->altWarningHeightMeters() + 1.0,
        heightMeters + 1.0);
}

bool ConfigPlannerViewModel::setSpeechLowGroundSpeedTriggerMps(
    double triggerMps)
{
    if (!m_speechSettings || !std::isfinite(triggerMps)
        || triggerMps < 0.0) {
        return false;
    }
    m_speechSettings->setLowGroundSpeedTriggerMps(triggerMps);
    return qFuzzyCompare(
        m_speechSettings->lowGroundSpeedTriggerMps() + 1.0,
        triggerMps + 1.0);
}

bool ConfigPlannerViewModel::setSpeechLowAirSpeedTriggerMps(
    double triggerMps)
{
    if (!m_speechSettings || !std::isfinite(triggerMps)
        || triggerMps < 0.0) {
        return false;
    }
    m_speechSettings->setLowAirSpeedTriggerMps(triggerMps);
    return qFuzzyCompare(
        m_speechSettings->lowAirSpeedTriggerMps() + 1.0,
        triggerMps + 1.0);
}
