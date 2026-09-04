#include "SpeechAnnouncer.h"

#include "GAudioOutput.h"
#include "SpeechSettings.h"
#include "SpeechTelemetrySource.h"
#include "UASInterface.h"
#include "ui/configuration/BatteryMonitorInstanceModel.h"
#include "ui/flightdata/FlightDataViewModel.h"

#include <QSettings>
#include <QTimer>

#include <cmath>
#include <utility>

namespace
{
constexpr qint64 kBatteryAlertIntervalMs = 30000;
constexpr qint64 kCustomIntervalMs = 30000;
constexpr qint64 kLowSpeedIntervalMs = 10000;
constexpr qint64 kAltitudeWarningIntervalMs = 10000;
constexpr qint64 kNoDataConnectionGraceMs = 30000;
constexpr qint64 kNoDataPacketAgeMs = 3000;
constexpr qint64 kNoDataRepeatMs = 5000;

struct DisplayUnits
{
    double altitudeMultiplier = 1.0;
    double speedMultiplier = 1.0;
    QString altitudeLabel = QStringLiteral("m");
    QString speedLabel = QStringLiteral("m/s");
};

DisplayUnits displayUnits(const QString &altitudeUnits,
                          const QString &speedUnits)
{
    DisplayUnits units;
    if (altitudeUnits.compare(QStringLiteral("Feet"),
                              Qt::CaseInsensitive) == 0) {
        units.altitudeMultiplier = 3.280839895013123;
        units.altitudeLabel = QStringLiteral("ft");
    }

    if (speedUnits.compare(QStringLiteral("fps"),
                           Qt::CaseInsensitive) == 0) {
        units.speedMultiplier = 3.280839895013123;
        units.speedLabel = QStringLiteral("fps");
    } else if (speedUnits.compare(QStringLiteral("kph"),
                                  Qt::CaseInsensitive) == 0) {
        units.speedMultiplier = 3.6;
        units.speedLabel = QStringLiteral("kph");
    } else if (speedUnits.compare(QStringLiteral("mph"),
                                  Qt::CaseInsensitive) == 0) {
        units.speedMultiplier = 2.2369362920544;
        units.speedLabel = QStringLiteral("mph");
    } else if (speedUnits.compare(QStringLiteral("knots"),
                                  Qt::CaseInsensitive) == 0) {
        units.speedMultiplier = 1.9438444924406;
        units.speedLabel = QStringLiteral("kts");
    }
    return units;
}
}

SpeechAnnouncer::SpeechAnnouncer(
    FlightDataViewModel *flightData,
    SpeechTelemetrySource *telemetrySource,
    QObject *parent)
    : QObject(parent)
    , m_flightData(flightData)
    , m_telemetrySource(telemetrySource)
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
    , m_readyProvider([]() {
        return GAudioOutput::instance()->isSpeechIdle();
    })
    , m_unitProvider([]() {
        QSettings settings;
        settings.setFallbacksEnabled(false);
        DisplayUnitState units;
        units.altitudeUnits = settings.value(
            QStringLiteral("altunits"),
            QStringLiteral("Meters")).toString();
        units.speedUnits = settings.value(
            QStringLiteral("speedunits"),
            QStringLiteral("meters_per_second")).toString();
        return units;
    })
{
    Q_ASSERT(m_flightData);
    m_elapsedClock.start();
    if (m_telemetrySource) {
        m_clock = [this]() {
            return m_telemetrySource
                ? m_telemetrySource->monotonicTimeMs() : 0;
        };
    }

    // String-based connects keep the injectable announcer independent from
    // FlightDataViewModel's meta-object in small unit-test targets.
    if (m_telemetrySource) {
        m_targetGeneration =
            m_telemetrySource->snapshot().lease.generation;
        connect(m_telemetrySource,
                &SpeechTelemetrySource::batteryTelemetryChanged,
                this, &SpeechAnnouncer::handleBatteryTelemetry);
        connect(m_telemetrySource,
                &SpeechTelemetrySource::flightModeChanged,
                this, &SpeechAnnouncer::announceFlightMode);
        connect(m_telemetrySource,
                &SpeechTelemetrySource::waypointChanged,
                this, &SpeechAnnouncer::announceWaypoint);
        connect(m_telemetrySource, &SpeechTelemetrySource::armedChanged,
                this, &SpeechAnnouncer::announceArmState);
        connect(m_telemetrySource,
                &SpeechTelemetrySource::targetEpochChanged,
                this, [this]() {
            // A queued phrase belongs to the vehicle epoch in which it was
            // created. Never finish or replay it for a newly selected target.
            GAudioOutput::instance()->stopSpeech();
            m_targetGeneration = m_telemetrySource
                ? m_telemetrySource->snapshot().lease.generation : 0;
            resetCountdowns();
        });
    } else {
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
    }
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &SpeechAnnouncer::tick);
    m_timer->start();
    resetCountdowns(m_flightData->activeUAS());
}

SpeechAnnouncer::SpeechAnnouncer(
    SpeechSettings *settings,
    Speaker speaker,
    VehicleStateProvider vehicleStateProvider,
    Clock clock,
    QObject *parent,
    ReadyProvider readyProvider,
    UnitProvider unitProvider)
    : QObject(parent)
    , m_settings(settings)
    , m_speaker(std::move(speaker))
    , m_vehicleStateProvider(std::move(vehicleStateProvider))
    , m_clock(std::move(clock))
    , m_readyProvider(std::move(readyProvider))
    , m_unitProvider(std::move(unitProvider))
{
    Q_ASSERT(m_settings);
    m_elapsedClock.start();
    resetCountdowns();
}

QString SpeechAnnouncer::formatTelemetryTemplate(
    const QString &speechTemplate, const VehicleState &state,
    const QString &altitudeUnits, const QString &speedUnits)
{
    const DisplayUnits units = displayUnits(altitudeUnits, speedUnits);
    QMap<QString, QString> replacements{
        {QStringLiteral("{sysid}"), QString::number(state.systemId)},
        {QStringLiteral("{compid}"), QString::number(state.componentId)},
        {QStringLiteral("{altunit}"), units.altitudeLabel},
        {QStringLiteral("{speedunit}"), units.speedLabel}
    };
    replacements.insert(
        QStringLiteral("{wpn}"),
        !state.waypointValid || state.waypointNumber == 0
            ? QStringLiteral("Home")
            : QString::number(state.waypointNumber));
    if (state.altitudeValid) {
        replacements.insert(
            QStringLiteral("{alt}"),
            QString::number(qRound(
                state.altitudeMeters * units.altitudeMultiplier)));
    }
    if (state.airspeedValid) {
        replacements.insert(
            QStringLiteral("{asp}"),
            QString::number(qRound(
                state.airspeedMps * units.speedMultiplier)));
    }
    if (state.groundSpeedValid) {
        replacements.insert(
            QStringLiteral("{gsp}"),
            QString::number(qRound(
                state.groundSpeedMps * units.speedMultiplier)));
    }
    if (state.modeValid) {
        replacements.insert(QStringLiteral("{mode}"), state.mode);
    }
    if (state.batteryVoltageValid) {
        replacements.insert(
            QStringLiteral("{batv}"),
            QString::number(state.batteryVoltage, 'f', 2));
    }
    if (state.batteryRemainingValid) {
        replacements.insert(
            QStringLiteral("{batp}"),
            QString::number(qRound(state.batteryRemainingPercent)));
    }

    return SpeechSettings::formatTemplate(speechTemplate, replacements);
}

void SpeechAnnouncer::announceFlightMode(const QString &mode)
{
    const VehicleState vehicle = currentVehicle();
    if (!m_settings || !vehicle.valid
        || (!m_telemetrySource && m_flightData
            && m_flightData->mode() != mode)) {
        return;
    }
    speak(m_settings->modeAnnouncement(
        mode, vehicle.systemId, vehicle.armed));
}

void SpeechAnnouncer::announceWaypoint(int sequence)
{
    const VehicleState vehicle = currentVehicle();
    if (!m_settings || !vehicle.valid
        || (!m_telemetrySource && m_flightData
            && m_flightData->wpNo() != sequence)) {
        return;
    }
    speak(m_settings->waypointAnnouncement(
        sequence, vehicle.systemId, vehicle.armed));
}

void SpeechAnnouncer::announceArmState(bool armed)
{
    const VehicleState vehicle = currentVehicle();
    if (vehicle.valid) {
        observeArmState(armed);
    }
    if (!m_settings || !vehicle.valid
        || (!m_telemetrySource && m_flightData
            && m_flightData->armed() != armed)) {
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

void SpeechAnnouncer::tick()
{
    if (!m_settings) {
        return;
    }

    const VehicleState vehicle = currentVehicle();
    if (!vehicle.valid) {
        return;
    }
    const qint64 now = nowMs();
    if (vehicle.generation != 0
        && vehicle.generation != m_targetGeneration) {
        m_targetGeneration = vehicle.generation;
        resetPeriodicCountdowns(now);
        return;
    }
    observeArmState(vehicle.armed);
    if (vehicle.armed && vehicle.altitudeValid) {
        m_altitudeMaximumMeters = qMax(
            m_altitudeMaximumMeters, vehicle.altitudeMeters);
    }
    if (!m_settings->isEnabled()) {
        return;
    }
    if (m_readyProvider && !m_readyProvider()) {
        return;
    }

    if (m_settings->armedOnly() && !vehicle.armed) {
        return;
    }

    if (vehicle.armed
        && vehicle.connectedSinceMs >= 0
        && vehicle.lastPacketMs >= 0
        && now > vehicle.connectedSinceMs + kNoDataConnectionGraceMs
        && now > vehicle.lastPacketMs + kNoDataPacketAgeMs
        && now > m_lastNoDataMs + kNoDataRepeatMs) {
        const qint64 silentSeconds =
            qMax<qint64>(0, (now - vehicle.lastPacketMs) / 1000);
        if (speak(QStringLiteral("WARNING No Data for %1 Seconds")
                      .arg(silentSeconds))) {
            m_lastNoDataMs = now;
        }
        return;
    }

    const DisplayUnitState units = m_unitProvider
        ? m_unitProvider() : DisplayUnitState{};

    if (m_settings->customEnabled()
        && now > m_lastCustomMs + kCustomIntervalMs) {
        if (speak(formatTelemetryTemplate(
                m_settings->customTemplate(), vehicle,
                units.altitudeUnits, units.speedUnits))) {
            m_lastCustomMs = now;
        }
        return;
    }

    if (vehicle.armed && vehicle.altitudeValid) {
        const double threshold = m_settings->altWarningHeightMeters();
        if (m_settings->altWarningEnabled()
            && m_settings->altWarningHeightConfigured()
            && vehicle.altitudeMeters != 0.0
            && vehicle.altitudeMeters <= threshold
            && m_altitudeMaximumMeters > threshold
            && now > m_lastAltWarningMs + kAltitudeWarningIntervalMs) {
            if (speak(formatTelemetryTemplate(
                    m_settings->altWarningTemplate(), vehicle,
                    units.altitudeUnits, units.speedUnits))) {
                m_lastAltWarningMs = now;
            }
            return;
        }
    }

    if (!m_settings->lowSpeedEnabled() || !vehicle.armed
        || now <= m_lastLowSpeedMs + kLowSpeedIntervalMs) {
        return;
    }
    if (vehicle.airspeedValid
        && vehicle.airspeedMps < m_settings->lowAirSpeedTriggerMps()) {
        if (speak(formatTelemetryTemplate(
                m_settings->lowAirSpeedTemplate(), vehicle,
                units.altitudeUnits, units.speedUnits))) {
            m_lastLowSpeedMs = now;
        }
    } else if (vehicle.groundSpeedValid
               && vehicle.groundSpeedMps
                    < m_settings->lowGroundSpeedTriggerMps()) {
        if (speak(formatTelemetryTemplate(
                m_settings->lowGroundSpeedTemplate(), vehicle,
                units.altitudeUnits, units.speedUnits))) {
            m_lastLowSpeedMs = now;
        }
    }
}

void SpeechAnnouncer::resetCountdowns(UASInterface *uas)
{
    Q_UNUSED(uas)
    // Match Mission Planner 10: the first periodic warning is eligible only
    // after the connection has been alive for one full alert interval.
    const qint64 now = nowMs();
    m_nextBatteryAlertMs = now + kBatteryAlertIntervalMs;
    resetPeriodicCountdowns(now);
}

SpeechAnnouncer::VehicleState SpeechAnnouncer::currentVehicle() const
{
    if (m_telemetrySource) {
        const SpeechTelemetrySource::Snapshot snapshot =
            m_telemetrySource->snapshot();
        VehicleState state;
        state.systemId = snapshot.lease.endpoint.systemId;
        state.armed = snapshot.heartbeatValid && snapshot.armed;
        state.valid = snapshot.isValid();
        state.generation = snapshot.lease.generation;
        state.connectedSinceMs = snapshot.connectedSinceMs;
        state.lastPacketMs = snapshot.lastPacketMs;
        state.altitudeValid = snapshot.altitudeValid;
        state.altitudeMeters = snapshot.altitudeMeters;
        state.airspeedValid = snapshot.airspeedValid;
        state.airspeedMps = snapshot.airspeedMps;
        state.groundSpeedValid = snapshot.groundSpeedValid;
        state.groundSpeedMps = snapshot.groundSpeedMps;
        state.waypointValid = snapshot.waypointValid;
        state.waypointNumber = snapshot.waypointNumber;
        state.componentId = snapshot.lease.endpoint.componentId;
        state.modeValid = snapshot.modeValid;
        state.mode = snapshot.mode;
        state.batteryVoltageValid = snapshot.batteryVoltageValid;
        state.batteryVoltage = snapshot.batteryVoltage;
        state.batteryRemainingValid = snapshot.batteryRemainingValid;
        state.batteryRemainingPercent =
            snapshot.batteryRemainingPercent;
        return state;
    }
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

void SpeechAnnouncer::observeArmState(bool armed)
{
    if (!m_armStateObserved || armed != m_lastArmed) {
        m_armStateObserved = true;
        m_lastArmed = armed;
        m_altitudeMaximumMeters = 0.0;
    }
}

void SpeechAnnouncer::resetPeriodicCountdowns(qint64 now)
{
    m_lastCustomMs = now;
    m_lastLowSpeedMs = now;
    m_lastAltWarningMs = now;
    m_lastNoDataMs = now;
    m_altitudeMaximumMeters = 0.0;
    m_armStateObserved = false;
    m_lastArmed = false;
}
