#include "SpeechAnnouncer.h"

#include "GAudioOutput.h"
#include "SpeechSettings.h"
#include "UASInterface.h"
#include "ui/configuration/BatteryMonitorInstanceModel.h"
#include "ui/flightdata/FlightDataViewModel.h"

#include <QSettings>

#include <cmath>

namespace
{
constexpr qint64 kBatteryAlertIntervalMs = 30000;

double finiteSetting(
    const QSettings &settings, const QString &key, double fallback)
{
    bool ok = false;
    const double value = settings.value(key, fallback).toDouble(&ok);
    return ok && std::isfinite(value) ? value : fallback;
}
}

SpeechAnnouncer::SpeechAnnouncer(
    FlightDataViewModel *flightData, QObject *parent)
    : QObject(parent)
    , m_flightData(flightData)
{
    Q_ASSERT(m_flightData);
    connect(m_flightData, &FlightDataViewModel::activeUASChanged,
            this, &SpeechAnnouncer::resetCountdowns);
    connect(m_flightData, &FlightDataViewModel::batteryTelemetryChanged,
            this, &SpeechAnnouncer::handleBatteryTelemetry);
    resetCountdowns(m_flightData->activeUAS());
}

void SpeechAnnouncer::handleBatteryTelemetry(
    double voltage, double remainingPercent)
{
    if (!m_flightData->activeUAS()) {
        return;
    }
    const QSettings settings;
    if (!SpeechSettings::instance()->isEnabled()
        || !settings.value(
                QStringLiteral("speechbatteryenabled"), false).toBool()
        || (settings.value(
                QStringLiteral("speech_armed_only"), false).toBool()
            && !m_flightData->armed())) {
        return;
    }

    if (!m_batteryAlertInterval.isValid()
        || m_batteryAlertInterval.elapsed() <= kBatteryAlertIntervalMs) {
        return;
    }
    const double warningVoltage = finiteSetting(
        settings, QStringLiteral("speechbatteryvolt"), 9.6);
    const double warningPercent = finiteSetting(
        settings, QStringLiteral("speechbatterypercent"), 20.0);
    if (!BatteryMonitorInstanceModel::ShouldTriggerBatteryAlert(
            voltage, remainingPercent,
            warningVoltage, warningPercent)) {
        return;
    }

    const QString message = BatteryMonitorInstanceModel::FormatBatteryAlert(
        settings.value(
            QStringLiteral("speechbattery"),
            QStringLiteral(
                "WARNING, Battery at {batv} Volt, {batp} percent"))
            .toString(),
        voltage, remainingPercent);
    if (GAudioOutput::instance()->say(message)) {
        m_batteryAlertInterval.restart();
    }
}

void SpeechAnnouncer::resetCountdowns(UASInterface *uas)
{
    Q_UNUSED(uas)
    // Match Mission Planner 10: the first periodic warning is eligible only
    // after the connection has been alive for one full alert interval.
    m_batteryAlertInterval.start();
}
