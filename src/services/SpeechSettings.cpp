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
const QString kBatteryEnabledKey = QStringLiteral("speechbatteryenabled");
const QString kBatteryTemplateKey = QStringLiteral("speechbattery");
const QString kBatteryVoltageKey = QStringLiteral("speechbatteryvolt");
const QString kBatteryPercentKey = QStringLiteral("speechbatterypercent");
const QString kArmEnabledKey = QStringLiteral("speecharmenabled");
const QString kArmTemplateKey = QStringLiteral("speecharm");
const QString kDisarmTemplateKey = QStringLiteral("speechdisarm");

const QString kDefaultWaypointTemplate =
    QStringLiteral("Heading to Waypoint {wpn}");
const QString kDefaultModeTemplate =
    QStringLiteral("Mode changed to {mode}");
const QString kDefaultBatteryTemplate = QStringLiteral(
    "WARNING, Battery at {batv} Volt, {batp} percent");
const QString kDefaultArmTemplate = QStringLiteral("Armed");
const QString kDefaultDisarmTemplate = QStringLiteral("Disarmed");

double doubleSetting(const QSettings *settings, const QString &key,
                     double fallback)
{
    bool ok = false;
    const double value = settings->value(key, fallback).toDouble(&ok);
    return ok && std::isfinite(value) ? value : fallback;
}

QString substitute(QString speechTemplate, const QString &token,
                   const QString &value, int sysid)
{
    speechTemplate.replace(token, value);
    speechTemplate.replace(QStringLiteral("{sysid}"),
                           QString::number(sysid));
    return speechTemplate;
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
    m_settings->setValue(kWaypointEnabledKey, enabled);
    if (enabled && !m_settings->contains(kWaypointTemplateKey)) {
        m_settings->setValue(kWaypointTemplateKey, kDefaultWaypointTemplate);
    }
    m_settings->sync();
    m_waypointEnabled = enabled;
    m_waypointTemplate = m_settings->value(
        kWaypointTemplateKey, kDefaultWaypointTemplate).toString();
    emit policyChanged();
}

void SpeechSettings::setModeEnabled(bool enabled)
{
    m_settings->setValue(kModeEnabledKey, enabled);
    if (enabled && !m_settings->contains(kModeTemplateKey)) {
        m_settings->setValue(kModeTemplateKey, kDefaultModeTemplate);
    }
    m_settings->sync();
    m_modeEnabled = enabled;
    m_modeTemplate = m_settings->value(
        kModeTemplateKey, kDefaultModeTemplate).toString();
    emit policyChanged();
}

void SpeechSettings::setBatteryEnabled(bool enabled)
{
    m_settings->setValue(kBatteryEnabledKey, enabled);
    if (enabled && !m_settings->contains(kBatteryTemplateKey)) {
        m_settings->setValue(kBatteryTemplateKey, kDefaultBatteryTemplate);
    }
    if (enabled && !m_settings->contains(kBatteryVoltageKey)) {
        m_settings->setValue(kBatteryVoltageKey, 9.6);
    }
    if (enabled && !m_settings->contains(kBatteryPercentKey)) {
        m_settings->setValue(kBatteryPercentKey, 20);
    }
    m_settings->sync();
    m_batteryEnabled = enabled;
    m_batteryTemplate = m_settings->value(
        kBatteryTemplateKey, kDefaultBatteryTemplate).toString();
    m_batteryWarningVoltage = doubleSetting(
        m_settings, kBatteryVoltageKey, 9.6);
    m_batteryWarningPercent = doubleSetting(
        m_settings, kBatteryPercentKey, 20.0);
    emit policyChanged();
}

void SpeechSettings::setArmDisarmEnabled(bool enabled)
{
    m_settings->setValue(kArmEnabledKey, enabled);
    if (enabled && !m_settings->contains(kArmTemplateKey)) {
        m_settings->setValue(kArmTemplateKey, kDefaultArmTemplate);
    }
    if (enabled && !m_settings->contains(kDisarmTemplateKey)) {
        m_settings->setValue(kDisarmTemplateKey, kDefaultDisarmTemplate);
    }
    m_settings->sync();
    m_armDisarmEnabled = enabled;
    m_armTemplate = m_settings->value(
        kArmTemplateKey, kDefaultArmTemplate).toString();
    m_disarmTemplate = m_settings->value(
        kDisarmTemplateKey, kDefaultDisarmTemplate).toString();
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

void SpeechSettings::setBatteryTemplate(const QString &speechTemplate)
{
    m_settings->setValue(kBatteryTemplateKey, speechTemplate);
    m_settings->sync();
    m_batteryTemplate = speechTemplate;
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

void SpeechSettings::setBatteryWarningVoltage(double voltage)
{
    if (!std::isfinite(voltage)) {
        return;
    }
    m_settings->setValue(kBatteryVoltageKey, voltage);
    m_settings->sync();
    m_batteryWarningVoltage = voltage;
    emit policyChanged();
}

void SpeechSettings::setBatteryWarningPercent(double percent)
{
    if (!std::isfinite(percent)) {
        return;
    }
    m_settings->setValue(kBatteryPercentKey, percent);
    m_settings->sync();
    m_batteryWarningPercent = percent;
    emit policyChanged();
}

QString SpeechSettings::modeAnnouncement(const QString &mode, int sysid,
                                         bool armed) const
{
    if (!m_enabled || !m_modeEnabled || (m_armedOnly && !armed)) {
        return QString();
    }
    return substitute(m_modeTemplate, QStringLiteral("{mode}"), mode, sysid);
}

QString SpeechSettings::waypointAnnouncement(int wpn, int sysid,
                                             bool armed) const
{
    if (!m_enabled || !m_waypointEnabled || (m_armedOnly && !armed)) {
        return QString();
    }
    return substitute(m_waypointTemplate, QStringLiteral("{wpn}"),
                      QString::number(wpn), sysid);
}

QString SpeechSettings::armStateAnnouncement(bool armed, int sysid) const
{
    // Mission Planner exempts arm/disarm announcements from armed-only mode:
    // otherwise the transition to disarmed could never be announced.
    if (!m_enabled || !m_armDisarmEnabled) {
        return QString();
    }
    QString speechTemplate = armed ? m_armTemplate : m_disarmTemplate;
    speechTemplate.replace(QStringLiteral("{sysid}"),
                           QString::number(sysid));
    return speechTemplate;
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
    m_batteryEnabled = m_settings->value(
        kBatteryEnabledKey, false).toBool();
    m_batteryTemplate = m_settings->value(
        kBatteryTemplateKey, kDefaultBatteryTemplate).toString();
    m_batteryWarningVoltage = doubleSetting(
        m_settings, kBatteryVoltageKey, 9.6);
    m_batteryWarningPercent = doubleSetting(
        m_settings, kBatteryPercentKey, 20.0);
    m_armDisarmEnabled = m_settings->value(
        kArmEnabledKey, false).toBool();
    m_armTemplate = m_settings->value(
        kArmTemplateKey, kDefaultArmTemplate).toString();
    m_disarmTemplate = m_settings->value(
        kDisarmTemplateKey, kDefaultDisarmTemplate).toString();
}

void SpeechSettings::reload()
{
    m_settings->sync();

    const bool oldEnabled = m_enabled;
    const bool oldArmedOnly = m_armedOnly;
    const bool oldWaypointEnabled = m_waypointEnabled;
    const bool oldModeEnabled = m_modeEnabled;
    const bool oldBatteryEnabled = m_batteryEnabled;
    const bool oldArmDisarmEnabled = m_armDisarmEnabled;
    const QString oldWaypointTemplate = m_waypointTemplate;
    const QString oldModeTemplate = m_modeTemplate;
    const QString oldBatteryTemplate = m_batteryTemplate;
    const QString oldArmTemplate = m_armTemplate;
    const QString oldDisarmTemplate = m_disarmTemplate;
    const double oldBatteryWarningVoltage = m_batteryWarningVoltage;
    const double oldBatteryWarningPercent = m_batteryWarningPercent;

    readSettings();

    if (oldEnabled != m_enabled) {
        emit enabledChanged(m_enabled);
    }
    if (oldEnabled != m_enabled
        || oldArmedOnly != m_armedOnly
        || oldWaypointEnabled != m_waypointEnabled
        || oldModeEnabled != m_modeEnabled
        || oldBatteryEnabled != m_batteryEnabled
        || oldArmDisarmEnabled != m_armDisarmEnabled
        || oldWaypointTemplate != m_waypointTemplate
        || oldModeTemplate != m_modeTemplate
        || oldBatteryTemplate != m_batteryTemplate
        || oldArmTemplate != m_armTemplate
        || oldDisarmTemplate != m_disarmTemplate
        || oldBatteryWarningVoltage != m_batteryWarningVoltage
        || oldBatteryWarningPercent != m_batteryWarningPercent) {
        emit policyChanged();
    }
}
