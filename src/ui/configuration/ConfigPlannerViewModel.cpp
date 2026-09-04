#include "ConfigPlannerViewModel.h"

#include "services/HudDisplaySettings.h"
#include "services/SpeechSettings.h"

#include <QCoreApplication>
#include <QSettings>

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
        emit stateChanged();
    });
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
