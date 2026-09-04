#include "SpeechSettings.h"

#include <QSettings>

#include <cmath>

namespace {
const QString kEnabledKey = QStringLiteral("speechenable");
const QString kArmedOnlyKey = QStringLiteral("speech_armed_only");
const QString kWaypointEnabledKey = QStringLiteral("speechwaypointenabled");
const QString kWaypointTemplateKey = QStringLiteral("speechwaypoint");
const QString kModeEnabledKey = QStringLiteral("speechmodeenabled");
const QString kModeTemplateKey = QStringLiteral("speechmode");
const QString kCustomEnabledKey = QStringLiteral("speechcustomenabled");
const QString kCustomTemplateKey = QStringLiteral("speechcustom");
const QString kBatteryEnabledKey = QStringLiteral("speechbatteryenabled");
const QString kBatteryTemplateKey = QStringLiteral("speechbattery");
const QString kBatteryVoltageKey = QStringLiteral("speechbatteryvolt");
const QString kBatteryPercentKey = QStringLiteral("speechbatterypercent");
const QString kAltWarningEnabledKey = QStringLiteral("speechaltenabled");
const QString kAltWarningTemplateKey = QStringLiteral("speechalt");
const QString kAltWarningHeightKey = QStringLiteral("speechaltheight");
const QString kArmEnabledKey = QStringLiteral("speecharmenabled");
const QString kArmTemplateKey = QStringLiteral("speecharm");
const QString kDisarmTemplateKey = QStringLiteral("speechdisarm");
const QString kLowSpeedEnabledKey = QStringLiteral("speechlowspeedenabled");
const QString kLowGroundSpeedTemplateKey =
    QStringLiteral("speechlowgroundspeed");
const QString kLowGroundSpeedTriggerKey =
    QStringLiteral("speechlowgroundspeedtrigger");
const QString kLowAirSpeedTemplateKey = QStringLiteral("speechlowairspeed");
const QString kLowAirSpeedTriggerKey =
    QStringLiteral("speechlowairspeedtrigger");

const QString kDefaultWaypointTemplate =
    QStringLiteral("Heading to Waypoint {wpn}");
const QString kDefaultModeTemplate =
    QStringLiteral("Mode changed to {mode}");
const QString kDefaultCustomTemplate = QStringLiteral(
    "Heading to Waypoint {wpn}, altitude is {alt}, Ground speed is {gsp} ");
const QString kDefaultBatteryTemplate = QStringLiteral(
    "WARNING, Battery at {batv} Volt, {batp} percent");
const QString kDefaultAltWarningTemplate =
    QStringLiteral("WARNING, low altitude {alt}");
const QString kDefaultArmTemplate = QStringLiteral("Armed");
const QString kDefaultDisarmTemplate = QStringLiteral("Disarmed");
const QString kDefaultLowGroundSpeedTemplate =
    QStringLiteral("Low Ground Speed {gsp}");
const QString kDefaultLowAirSpeedTemplate =
    QStringLiteral("Low Air Speed {asp}");

double nonNegativeDoubleSetting(const QSettings *settings, const QString &key,
                                double fallback, bool *configured = nullptr)
{
    bool ok = false;
    const bool contains = settings->contains(key);
    const double value = settings->value(key, fallback).toDouble(&ok);
    const bool valid = contains && ok && std::isfinite(value) && value >= 0.0;
    if (configured) {
        *configured = valid;
    }
    return valid ? value : fallback;
}
}

SpeechSettings::SpeechSettings(QSettings *settings, QObject *parent)
    : QObject(parent)
{
    if (settings) {
        m_settings = settings;
    } else {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
    }

    m_settings->setFallbacksEnabled(false);
    // Construction observes existing policy without creating implicit keys.
    readSettings();
}

SpeechSettings::~SpeechSettings() = default;

SpeechSettings *SpeechSettings::instance()
{
    static SpeechSettings service;
    return &service;
}

QString SpeechSettings::settingsKey()
{
    return kEnabledKey;
}

void SpeechSettings::setEnabled(bool enabled)
{
    // Persist even when the cached default already has this value. This keeps
    // an explicit user choice distinct from the constructor's missing-key
    // default while avoiding a spurious live-state signal.
    const bool changed = m_enabled != enabled;
    m_settings->setValue(kEnabledKey, enabled);
    m_settings->sync();
    m_enabled = enabled;
    if (changed) {
        emit enabledChanged(enabled);
    }
    emit policyChanged();
}

void SpeechSettings::setArmedOnly(bool armedOnly)
{
    m_settings->setValue(kArmedOnlyKey, armedOnly);
    m_settings->sync();
    m_armedOnly = armedOnly;
    emit policyChanged();
}

void SpeechSettings::setWaypointEnabled(bool enabled)
{
    if (enabled && !m_settings->contains(kWaypointTemplateKey)) {
        m_settings->setValue(kWaypointTemplateKey, kDefaultWaypointTemplate);
    }
    m_settings->setValue(kWaypointEnabledKey, enabled);
    m_settings->sync();
    m_waypointEnabled = enabled;
    m_waypointTemplate = m_settings->value(
        kWaypointTemplateKey, kDefaultWaypointTemplate).toString();
    emit policyChanged();
}

void SpeechSettings::setModeEnabled(bool enabled)
{
    if (enabled && !m_settings->contains(kModeTemplateKey)) {
        m_settings->setValue(kModeTemplateKey, kDefaultModeTemplate);
    }
    m_settings->setValue(kModeEnabledKey, enabled);
    m_settings->sync();
    m_modeEnabled = enabled;
    m_modeTemplate = m_settings->value(
        kModeTemplateKey, kDefaultModeTemplate).toString();
    emit policyChanged();
}

void SpeechSettings::setCustomEnabled(bool enabled)
{
    if (enabled && !m_settings->contains(kCustomTemplateKey)) {
        m_settings->setValue(kCustomTemplateKey, kDefaultCustomTemplate);
    }
    m_settings->setValue(kCustomEnabledKey, enabled);
    m_settings->sync();
    m_customEnabled = enabled;
    m_customTemplate = m_settings->value(
        kCustomTemplateKey, kDefaultCustomTemplate).toString();
    emit policyChanged();
}

void SpeechSettings::setBatteryEnabled(bool enabled)
{
    if (enabled && !m_settings->contains(kBatteryTemplateKey)) {
        m_settings->setValue(kBatteryTemplateKey, kDefaultBatteryTemplate);
    }
    if (enabled && !m_settings->contains(kBatteryVoltageKey)) {
        m_settings->setValue(kBatteryVoltageKey, 9.6);
    }
    if (enabled && !m_settings->contains(kBatteryPercentKey)) {
        m_settings->setValue(kBatteryPercentKey, 20);
    }
    m_settings->setValue(kBatteryEnabledKey, enabled);
    m_settings->sync();
    m_batteryEnabled = enabled;
    m_batteryTemplate = m_settings->value(
        kBatteryTemplateKey, kDefaultBatteryTemplate).toString();
    m_batteryWarningVoltage = nonNegativeDoubleSetting(
        m_settings, kBatteryVoltageKey, 9.6);
    m_batteryWarningPercent = nonNegativeDoubleSetting(
        m_settings, kBatteryPercentKey, 20.0);
    emit policyChanged();
}

void SpeechSettings::setAltWarningEnabled(bool enabled)
{
    if (enabled && !m_settings->contains(kAltWarningTemplateKey)) {
        m_settings->setValue(kAltWarningTemplateKey,
                             kDefaultAltWarningTemplate);
    }
    if (enabled && !m_settings->contains(kAltWarningHeightKey)) {
        m_settings->setValue(kAltWarningHeightKey, 2.0);
    }
    m_settings->setValue(kAltWarningEnabledKey, enabled);
    m_settings->sync();
    m_altWarningEnabled = enabled;
    m_altWarningTemplate = m_settings->value(
        kAltWarningTemplateKey, kDefaultAltWarningTemplate).toString();
    m_altWarningHeightMeters = nonNegativeDoubleSetting(
        m_settings, kAltWarningHeightKey, 2.0,
        &m_altWarningHeightConfigured);
    emit policyChanged();
}

void SpeechSettings::setArmDisarmEnabled(bool enabled)
{
    if (enabled && !m_settings->contains(kArmTemplateKey)) {
        m_settings->setValue(kArmTemplateKey, kDefaultArmTemplate);
    }
    if (enabled && !m_settings->contains(kDisarmTemplateKey)) {
        m_settings->setValue(kDisarmTemplateKey, kDefaultDisarmTemplate);
    }
    m_settings->setValue(kArmEnabledKey, enabled);
    m_settings->sync();
    m_armDisarmEnabled = enabled;
    m_armTemplate = m_settings->value(
        kArmTemplateKey, kDefaultArmTemplate).toString();
    m_disarmTemplate = m_settings->value(
        kDisarmTemplateKey, kDefaultDisarmTemplate).toString();
    emit policyChanged();
}

void SpeechSettings::setLowSpeedEnabled(bool enabled)
{
    if (enabled && !m_settings->contains(kLowGroundSpeedTemplateKey)) {
        m_settings->setValue(kLowGroundSpeedTemplateKey,
                             kDefaultLowGroundSpeedTemplate);
    }
    if (enabled && !m_settings->contains(kLowGroundSpeedTriggerKey)) {
        m_settings->setValue(kLowGroundSpeedTriggerKey, 0.0);
    }
    if (enabled && !m_settings->contains(kLowAirSpeedTemplateKey)) {
        m_settings->setValue(kLowAirSpeedTemplateKey,
                             kDefaultLowAirSpeedTemplate);
    }
    if (enabled && !m_settings->contains(kLowAirSpeedTriggerKey)) {
        m_settings->setValue(kLowAirSpeedTriggerKey, 0.0);
    }
    m_settings->setValue(kLowSpeedEnabledKey, enabled);
    m_settings->sync();
    m_lowSpeedEnabled = enabled;
    m_lowGroundSpeedTemplate = m_settings->value(
        kLowGroundSpeedTemplateKey,
        kDefaultLowGroundSpeedTemplate).toString();
    m_lowGroundSpeedTriggerMps = nonNegativeDoubleSetting(
        m_settings, kLowGroundSpeedTriggerKey, 0.0);
    m_lowAirSpeedTemplate = m_settings->value(
        kLowAirSpeedTemplateKey, kDefaultLowAirSpeedTemplate).toString();
    m_lowAirSpeedTriggerMps = nonNegativeDoubleSetting(
        m_settings, kLowAirSpeedTriggerKey, 0.0);
    emit policyChanged();
}

void SpeechSettings::setWaypointTemplate(const QString &speechTemplate)
{
    m_settings->setValue(kWaypointTemplateKey, speechTemplate);
    m_settings->sync();
    m_waypointTemplate = speechTemplate;
    emit policyChanged();
}

void SpeechSettings::setModeTemplate(const QString &speechTemplate)
{
    m_settings->setValue(kModeTemplateKey, speechTemplate);
    m_settings->sync();
    m_modeTemplate = speechTemplate;
    emit policyChanged();
}

void SpeechSettings::setCustomTemplate(const QString &speechTemplate)
{
    m_settings->setValue(kCustomTemplateKey, speechTemplate);
    m_settings->sync();
    m_customTemplate = speechTemplate;
    emit policyChanged();
}

void SpeechSettings::setBatteryTemplate(const QString &speechTemplate)
{
    m_settings->setValue(kBatteryTemplateKey, speechTemplate);
    m_settings->sync();
    m_batteryTemplate = speechTemplate;
    emit policyChanged();
}

void SpeechSettings::setAltWarningTemplate(const QString &speechTemplate)
{
    m_settings->setValue(kAltWarningTemplateKey, speechTemplate);
    m_settings->sync();
    m_altWarningTemplate = speechTemplate;
    emit policyChanged();
}

void SpeechSettings::setArmTemplate(const QString &speechTemplate)
{
    m_settings->setValue(kArmTemplateKey, speechTemplate);
    m_settings->sync();
    m_armTemplate = speechTemplate;
    emit policyChanged();
}

void SpeechSettings::setDisarmTemplate(const QString &speechTemplate)
{
    m_settings->setValue(kDisarmTemplateKey, speechTemplate);
    m_settings->sync();
    m_disarmTemplate = speechTemplate;
    emit policyChanged();
}

void SpeechSettings::setLowGroundSpeedTemplate(
    const QString &speechTemplate)
{
    m_settings->setValue(kLowGroundSpeedTemplateKey, speechTemplate);
    m_settings->sync();
    m_lowGroundSpeedTemplate = speechTemplate;
    emit policyChanged();
}

void SpeechSettings::setLowAirSpeedTemplate(const QString &speechTemplate)
{
    m_settings->setValue(kLowAirSpeedTemplateKey, speechTemplate);
    m_settings->sync();
    m_lowAirSpeedTemplate = speechTemplate;
    emit policyChanged();
}

void SpeechSettings::setBatteryWarningVoltage(double voltage)
{
    if (!std::isfinite(voltage) || voltage < 0.0) {
        return;
    }
    m_settings->setValue(kBatteryVoltageKey, voltage);
    m_settings->sync();
    m_batteryWarningVoltage = voltage;
    emit policyChanged();
}

void SpeechSettings::setBatteryWarningPercent(double percent)
{
    if (!std::isfinite(percent) || percent < 0.0) {
        return;
    }
    m_settings->setValue(kBatteryPercentKey, percent);
    m_settings->sync();
    m_batteryWarningPercent = percent;
    emit policyChanged();
}

void SpeechSettings::setAltWarningHeightMeters(double heightMeters)
{
    if (!std::isfinite(heightMeters) || heightMeters < 0.0) {
        return;
    }
    m_settings->setValue(kAltWarningHeightKey, heightMeters);
    m_settings->sync();
    m_altWarningHeightMeters = heightMeters;
    m_altWarningHeightConfigured = true;
    emit policyChanged();
}

void SpeechSettings::setLowGroundSpeedTriggerMps(double triggerMps)
{
    if (!std::isfinite(triggerMps) || triggerMps < 0.0) {
        return;
    }
    m_settings->setValue(kLowGroundSpeedTriggerKey, triggerMps);
    m_settings->sync();
    m_lowGroundSpeedTriggerMps = triggerMps;
    emit policyChanged();
}

void SpeechSettings::setLowAirSpeedTriggerMps(double triggerMps)
{
    if (!std::isfinite(triggerMps) || triggerMps < 0.0) {
        return;
    }
    m_settings->setValue(kLowAirSpeedTriggerKey, triggerMps);
    m_settings->sync();
    m_lowAirSpeedTriggerMps = triggerMps;
    emit policyChanged();
}

QString SpeechSettings::formatTemplate(
    QString speechTemplate,
    const QMap<QString, QString> &replacements)
{
    for (auto it = replacements.cbegin(); it != replacements.cend(); ++it) {
        speechTemplate.replace(it.key(), it.value());
    }
    return speechTemplate;
}

QString SpeechSettings::modeAnnouncement(const QString &mode, int sysid,
                                         bool armed) const
{
    if (!m_enabled || !m_modeEnabled || (m_armedOnly && !armed)) {
        return QString();
    }
    return formatTemplate(m_modeTemplate,
                          {{QStringLiteral("{mode}"), mode},
                           {QStringLiteral("{sysid}"),
                            QString::number(sysid)}});
}

QString SpeechSettings::waypointAnnouncement(int wpn, int sysid,
                                             bool armed) const
{
    if (!m_enabled || !m_waypointEnabled || (m_armedOnly && !armed)) {
        return QString();
    }
    return formatTemplate(m_waypointTemplate,
                          {{QStringLiteral("{wpn}"),
                            wpn == 0 ? QStringLiteral("Home")
                                     : QString::number(wpn)},
                           {QStringLiteral("{sysid}"),
                            QString::number(sysid)}});
}

QString SpeechSettings::armStateAnnouncement(bool armed, int sysid) const
{
    // Mission Planner exempts arm/disarm announcements from armed-only mode:
    // otherwise the transition to disarmed could never be announced.
    if (!m_enabled || !m_armDisarmEnabled) {
        return QString();
    }
    return formatTemplate(armed ? m_armTemplate : m_disarmTemplate,
                          {{QStringLiteral("{sysid}"),
                            QString::number(sysid)}});
}

void SpeechSettings::readSettings()
{
    m_enabled = m_settings->value(kEnabledKey, false).toBool();
    m_armedOnly = m_settings->value(kArmedOnlyKey, false).toBool();
    m_waypointEnabled = m_settings->value(
        kWaypointEnabledKey, false).toBool();
    m_waypointTemplate = m_settings->value(
        kWaypointTemplateKey, kDefaultWaypointTemplate).toString();
    m_modeEnabled = m_settings->value(kModeEnabledKey, false).toBool();
    m_modeTemplate = m_settings->value(
        kModeTemplateKey, kDefaultModeTemplate).toString();
    m_customEnabled = m_settings->value(
        kCustomEnabledKey, false).toBool();
    m_customTemplate = m_settings->value(
        kCustomTemplateKey, kDefaultCustomTemplate).toString();
    m_batteryEnabled = m_settings->value(
        kBatteryEnabledKey, false).toBool();
    m_batteryTemplate = m_settings->value(
        kBatteryTemplateKey, kDefaultBatteryTemplate).toString();
    m_batteryWarningVoltage = nonNegativeDoubleSetting(
        m_settings, kBatteryVoltageKey, 9.6);
    m_batteryWarningPercent = nonNegativeDoubleSetting(
        m_settings, kBatteryPercentKey, 20.0);
    m_altWarningEnabled = m_settings->value(
        kAltWarningEnabledKey, false).toBool();
    m_altWarningTemplate = m_settings->value(
        kAltWarningTemplateKey, kDefaultAltWarningTemplate).toString();
    m_altWarningHeightMeters = nonNegativeDoubleSetting(
        m_settings, kAltWarningHeightKey, 2.0,
        &m_altWarningHeightConfigured);
    m_armDisarmEnabled = m_settings->value(
        kArmEnabledKey, false).toBool();
    m_armTemplate = m_settings->value(
        kArmTemplateKey, kDefaultArmTemplate).toString();
    m_disarmTemplate = m_settings->value(
        kDisarmTemplateKey, kDefaultDisarmTemplate).toString();
    m_lowSpeedEnabled = m_settings->value(
        kLowSpeedEnabledKey, false).toBool();
    m_lowGroundSpeedTemplate = m_settings->value(
        kLowGroundSpeedTemplateKey,
        kDefaultLowGroundSpeedTemplate).toString();
    m_lowGroundSpeedTriggerMps = nonNegativeDoubleSetting(
        m_settings, kLowGroundSpeedTriggerKey, 0.0);
    m_lowAirSpeedTemplate = m_settings->value(
        kLowAirSpeedTemplateKey, kDefaultLowAirSpeedTemplate).toString();
    m_lowAirSpeedTriggerMps = nonNegativeDoubleSetting(
        m_settings, kLowAirSpeedTriggerKey, 0.0);
}

void SpeechSettings::reload()
{
    m_settings->sync();

    const bool oldEnabled = m_enabled;
    const bool oldArmedOnly = m_armedOnly;
    const bool oldWaypointEnabled = m_waypointEnabled;
    const bool oldModeEnabled = m_modeEnabled;
    const bool oldCustomEnabled = m_customEnabled;
    const bool oldBatteryEnabled = m_batteryEnabled;
    const bool oldAltWarningEnabled = m_altWarningEnabled;
    const bool oldArmDisarmEnabled = m_armDisarmEnabled;
    const bool oldLowSpeedEnabled = m_lowSpeedEnabled;
    const QString oldWaypointTemplate = m_waypointTemplate;
    const QString oldModeTemplate = m_modeTemplate;
    const QString oldCustomTemplate = m_customTemplate;
    const QString oldBatteryTemplate = m_batteryTemplate;
    const QString oldAltWarningTemplate = m_altWarningTemplate;
    const QString oldArmTemplate = m_armTemplate;
    const QString oldDisarmTemplate = m_disarmTemplate;
    const QString oldLowGroundSpeedTemplate = m_lowGroundSpeedTemplate;
    const QString oldLowAirSpeedTemplate = m_lowAirSpeedTemplate;
    const double oldBatteryWarningVoltage = m_batteryWarningVoltage;
    const double oldBatteryWarningPercent = m_batteryWarningPercent;
    const double oldAltWarningHeightMeters = m_altWarningHeightMeters;
    const bool oldAltWarningHeightConfigured =
        m_altWarningHeightConfigured;
    const double oldLowGroundSpeedTriggerMps =
        m_lowGroundSpeedTriggerMps;
    const double oldLowAirSpeedTriggerMps = m_lowAirSpeedTriggerMps;

    readSettings();

    if (oldEnabled != m_enabled) {
        emit enabledChanged(m_enabled);
    }
    if (oldEnabled != m_enabled
        || oldArmedOnly != m_armedOnly
        || oldWaypointEnabled != m_waypointEnabled
        || oldModeEnabled != m_modeEnabled
        || oldCustomEnabled != m_customEnabled
        || oldBatteryEnabled != m_batteryEnabled
        || oldAltWarningEnabled != m_altWarningEnabled
        || oldArmDisarmEnabled != m_armDisarmEnabled
        || oldLowSpeedEnabled != m_lowSpeedEnabled
        || oldWaypointTemplate != m_waypointTemplate
        || oldModeTemplate != m_modeTemplate
        || oldCustomTemplate != m_customTemplate
        || oldBatteryTemplate != m_batteryTemplate
        || oldAltWarningTemplate != m_altWarningTemplate
        || oldArmTemplate != m_armTemplate
        || oldDisarmTemplate != m_disarmTemplate
        || oldLowGroundSpeedTemplate != m_lowGroundSpeedTemplate
        || oldLowAirSpeedTemplate != m_lowAirSpeedTemplate
        || oldBatteryWarningVoltage != m_batteryWarningVoltage
        || oldBatteryWarningPercent != m_batteryWarningPercent
        || oldAltWarningHeightMeters != m_altWarningHeightMeters
        || oldAltWarningHeightConfigured != m_altWarningHeightConfigured
        || oldLowGroundSpeedTriggerMps != m_lowGroundSpeedTriggerMps
        || oldLowAirSpeedTriggerMps != m_lowAirSpeedTriggerMps) {
        emit policyChanged();
    }
}
