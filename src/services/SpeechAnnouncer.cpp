#include "SpeechAnnouncer.h"

#include "GAudioOutput.h"
#include "SpeechSettings.h"
#include "UASInterface.h"
#include "ui/configuration/BatteryMonitorInstanceModel.h"
#include "ui/flightdata/FlightDataViewModel.h"

#include <utility>

namespace
{
constexpr qint64 kBatteryAlertIntervalMs = 30000;
}

SpeechAnnouncer::SpeechAnnouncer(
    FlightDataViewModel *flightData, QObject *parent)
    : QObject(parent)
    , m_flightData(flightData)
    , m_settings(SpeechSettings::instance())
    , m_speaker([](const QString &message) {
        return GAudioOutput::instance()->say(message);
    })
    , m_vehicleStateProvider([flightData]() {
        VehicleState state;
        UASInterface *const uas = flightData->activeUAS();
        if (!uas) {
            return state;
        }
        state.systemId = uas->getUASID();
        state.armed = flightData->armed();
        state.valid = true;
        return state;
    })
{
    Q_ASSERT(m_flightData);
    m_elapsedClock.start();

    // String-based connects keep the injectable announcer independent from
    // FlightDataViewModel's meta-object in small unit-test targets.
    connect(m_flightData, SIGNAL(activeUASChanged(UASInterface*)),
            this, SLOT(resetCountdowns(UASInterface*)));
    connect(m_flightData, SIGNAL(batteryTelemetryChanged(double,double)),
            this, SLOT(handleBatteryTelemetry(double,double)));
    connect(m_flightData, SIGNAL(flightModeChanged(QString)),
            this, SLOT(announceFlightMode(QString)));
    connect(m_flightData, SIGNAL(currentWaypointChanged(int)),
            this, SLOT(announceWaypoint(int)));
    connect(m_flightData, SIGNAL(armedStateChanged(bool)),
            this, SLOT(announceArmState(bool)));
    resetCountdowns(m_flightData->activeUAS());
}

SpeechAnnouncer::SpeechAnnouncer(
    SpeechSettings *settings,
    Speaker speaker,
    VehicleStateProvider vehicleStateProvider,
    Clock clock,
    QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_speaker(std::move(speaker))
    , m_vehicleStateProvider(std::move(vehicleStateProvider))
    , m_clock(std::move(clock))
{
    Q_ASSERT(m_settings);
    m_elapsedClock.start();
    resetCountdowns();
}

void SpeechAnnouncer::announceFlightMode(const QString &mode)
{
    const VehicleState vehicle = currentVehicle();
    if (!m_settings || !vehicle.valid
        || (m_flightData && m_flightData->mode() != mode)) {
        return;
    }
    speak(m_settings->modeAnnouncement(
        mode, vehicle.systemId, vehicle.armed));
}

void SpeechAnnouncer::announceWaypoint(int sequence)
{
    const VehicleState vehicle = currentVehicle();
    if (!m_settings || !vehicle.valid
        || (m_flightData && m_flightData->wpNo() != sequence)) {
        return;
    }
    speak(m_settings->waypointAnnouncement(
        sequence, vehicle.systemId, vehicle.armed));
}

void SpeechAnnouncer::announceArmState(bool armed)
{
    const VehicleState vehicle = currentVehicle();
    if (!m_settings || !vehicle.valid
        || (m_flightData && m_flightData->armed() != armed)) {
        return;
    }
    speak(m_settings->armStateAnnouncement(armed, vehicle.systemId));
}

void SpeechAnnouncer::handleBatteryTelemetry(
    double voltage, double remainingPercent)
{
    const VehicleState vehicle = currentVehicle();
    if (!vehicle.valid || !m_settings
        || !m_settings->isEnabled()
        || !m_settings->batteryEnabled()
        || (m_settings->armedOnly() && !vehicle.armed)) {
        return;
    }

    const qint64 now = nowMs();
    if (now <= m_nextBatteryAlertMs) {
        return;
    }
    if (!BatteryMonitorInstanceModel::ShouldTriggerBatteryAlert(
            voltage, remainingPercent,
            m_settings->batteryWarningVoltage(),
            m_settings->batteryWarningPercent())) {
        return;
    }

    QString message = BatteryMonitorInstanceModel::FormatBatteryAlert(
        m_settings->batteryTemplate(), voltage, remainingPercent);
    message.replace(QStringLiteral("{sysid}"),
                    QString::number(vehicle.systemId));
    if (speak(message)) {
        m_nextBatteryAlertMs = now + kBatteryAlertIntervalMs;
    }
}

void SpeechAnnouncer::resetCountdowns(UASInterface *uas)
{
    Q_UNUSED(uas)
    // Match Mission Planner 10: the first periodic warning is eligible only
    // after the connection has been alive for one full alert interval.
    m_nextBatteryAlertMs = nowMs() + kBatteryAlertIntervalMs;
}

SpeechAnnouncer::VehicleState SpeechAnnouncer::currentVehicle() const
{
    return m_vehicleStateProvider
        ? m_vehicleStateProvider() : VehicleState{};
}

bool SpeechAnnouncer::speak(const QString &message) const
{
    return !message.trimmed().isEmpty()
        && m_speaker && m_speaker(message);
}

qint64 SpeechAnnouncer::nowMs() const
{
    return m_clock ? m_clock() : m_elapsedClock.elapsed();
}
