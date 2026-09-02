#include "FlightDataViewModel.h"

#include "FlightDataMissionProgress.h"
#include "HudControl.h"
#include "UASInterface.h"
#include "UASManager.h"
#include "UASWaypointManager.h"
#include "Waypoint.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;

double headingDegrees(double radians)
{
    double degrees = std::fmod(radians * kRadiansToDegrees, 360.0);
    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

bool nameContains(const QString &name, const QString &part)
{
    return name.contains(part, Qt::CaseInsensitive);
}
}

FlightDataViewModel::FlightDataViewModel(QObject *parent, bool bindToUasManager)
    : QObject(parent)
{
    if (!bindToUasManager) {
        return;
    }
    UASManager *manager = UASManager::instance();
    connect(manager,
            QOverload<UASInterface *>::of(&UASManager::activeUASSet),
            this, &FlightDataViewModel::setActiveUAS);
    connect(manager, &UASManager::UASDeleted, this,
            [this](UASInterface *uas) {
                if (m_uas == uas) {
                    setActiveUAS(nullptr);
                }
            });
    setActiveUAS(manager->silentGetActiveUAS());
}

FlightDataViewModel::~FlightDataViewModel()
{
    if (m_uas) {
        disconnect(m_uas, nullptr, this, nullptr);
    }
    if (m_waypointManager) {
        disconnect(m_waypointManager, nullptr, this, nullptr);
    }
}

UASInterface *FlightDataViewModel::activeUAS() const
{
    return m_uas.data();
}

void FlightDataViewModel::attachHud(HudControl *hud)
{
    if (m_hudConnection) {
        disconnect(m_hudConnection);
    }
    m_hud = hud;
    if (m_hud) {
        m_hudConnection = connect(this, &FlightDataViewModel::telemetryChanged,
                                  m_hud, [this]() { applyToHud(); });
        applyToHud();
        m_hud->snapToValues();
    }
}

bool FlightDataViewModel::isCurrentUas(UASInterface *uas) const
{
    return !uas || uas == m_uas;
}

void FlightDataViewModel::resetTelemetry()
{
    m_roll = m_pitch = m_yaw = 0.0;
    m_alt = m_airSpeed = m_groundSpeed = m_verticalSpeed = 0.0;
    m_satCount = 0.0;
    m_gpsHdop = 0.0;
    m_gpsFixType = 0;
    m_armed = false;
    m_prearmOk = false;
    m_mode = QStringLiteral("UNKNOWN");
    m_batteryVoltage = m_currentAmps = 0.0;
    m_batteryRemaining = 0;
    m_navBearing = m_targetAlt = m_targetSpeed = 0.0;
    m_windDir = m_windVel = m_aoa = m_ssa = 0.0;
    m_xTrackError = m_turnRate = m_wpDist = 0.0;
    m_wpNo = 0;
    m_missionItemCount = 0;
    m_missionProgress = 0.0;
    m_missionProgressText = tr("No mission loaded");
    m_batteryVoltage2 = m_currentAmps2 = 0.0;
    m_batteryRemaining2 = 0;
    m_throttlePercent = 0.0;
    m_failsafe = false;
    m_safetyActive = false;
    m_linkQuality = 0.0;
}

void FlightDataViewModel::setActiveUAS(UASInterface *uas)
{
    if (m_uas == uas) {
        return;
    }
    if (m_uas) {
        disconnect(m_uas, nullptr, this, nullptr);
    }
    if (m_waypointManager) {
        disconnect(m_waypointManager, nullptr, this, nullptr);
        m_waypointManager = nullptr;
    }
    m_uas = uas;
    resetTelemetry();

    if (m_uas) {
        connect(m_uas,
                QOverload<UASInterface *, double, double, double, quint64>::of(
                    &UASInterface::attitudeChanged),
                this, &FlightDataViewModel::updateAttitude);
        connect(m_uas, &UASInterface::attitudeRotationRatesChanged,
                this, &FlightDataViewModel::updateAttitudeRates);
        connect(m_uas, &UASInterface::altitudeChanged,
                this, &FlightDataViewModel::updateAltitude);
        connect(m_uas, &UASInterface::speedChanged,
                this, &FlightDataViewModel::updateSpeed);
        connect(m_uas, &UASInterface::batteryChanged,
                this, &FlightDataViewModel::updateBattery);
        connect(m_uas, &UASInterface::thrustChanged,
                this, &FlightDataViewModel::updateThrust);
        connect(m_uas, QOverload<bool>::of(&UASInterface::armingChanged),
                this, &FlightDataViewModel::updateArmed);
        connect(m_uas, &UASInterface::modeChanged,
                this, &FlightDataViewModel::updateMode);
        connect(m_uas, &UASInterface::navModeChanged,
                this, &FlightDataViewModel::updateNavMode);
        connect(m_uas, &UASInterface::gpsLocalizationChanged,
                this, &FlightDataViewModel::updateGpsFix);
        connect(m_uas, &UASInterface::dropRateChanged,
                this, &FlightDataViewModel::updateDropRate);
        connect(m_uas, &UASInterface::navigationControllerErrorsChanged,
                this, &FlightDataViewModel::updateNavigation);
        connect(m_uas, &UASInterface::valueChanged,
                this, &FlightDataViewModel::updateValue);
        connect(m_uas, &UASInterface::textMessageReceived,
                this, &FlightDataViewModel::updateTextMessage);
        connect(m_uas,
                QOverload<UASInterface *, QString, QString>::of(
                    &UASInterface::statusChanged),
                this, &FlightDataViewModel::updateStatus);
        connect(m_uas, &UASInterface::heartbeat,
                this, &FlightDataViewModel::updateHeartbeat);
        connect(m_uas, &UASInterface::heartbeatTimeout,
                this, &FlightDataViewModel::updateHeartbeatTimeout);
        connect(m_uas, SIGNAL(satelliteCountChanged(int,QString)),
                this, SLOT(updateSatelliteCount(int,QString)));
        connect(m_uas, SIGNAL(gpsHdopChanged(double,QString)),
                this, SLOT(updateGpsHdop(double,QString)));

        m_roll = m_uas->getRoll() * kRadiansToDegrees;
        m_pitch = m_uas->getPitch() * kRadiansToDegrees;
        m_yaw = headingDegrees(m_uas->getYaw());
        m_alt = m_uas->getAltitudeRelative();
        m_armed = m_uas->isArmed();
        m_prearmOk = m_armed;
        m_mode = m_uas->getShortMode().isEmpty()
            ? QStringLiteral("UNKNOWN") : m_uas->getShortMode();
        m_groundSpeed = m_uas->property("groundSpeed").toDouble();
        m_airSpeed = m_uas->property("airSpeed").toDouble();
        m_satCount = m_uas->property("satelliteCount").toDouble();
        m_gpsFixType = m_uas->property("gpsFix").toInt();
        m_wpDist = m_uas->property("distToWaypoint").toDouble();
        m_navBearing = m_uas->property("bearingToWaypoint").toDouble();
        connectWaypointManager(m_uas->getWaypointManager());
    }
    publish();
    emit activeUASChanged(m_uas);
}

void FlightDataViewModel::connectWaypointManager(UASWaypointManager *manager)
{
    m_waypointManager = manager;
    if (!m_waypointManager) {
        return;
    }
    connect(m_waypointManager, &UASWaypointManager::currentWaypointChanged,
            this, &FlightDataViewModel::updateWaypoint);
    connect(m_waypointManager, &UASWaypointManager::waypointDistanceChanged,
            this, &FlightDataViewModel::updateWaypointDistance);
    connect(m_waypointManager,
            QOverload<>::of(&UASWaypointManager::waypointEditableListChanged),
            this, [this]() {
        updateMissionProgress();
        publish();
    });
    connect(m_waypointManager,
            QOverload<>::of(&UASWaypointManager::waypointViewOnlyListChanged),
            this, [this]() {
        updateMissionProgress();
        publish();
    });
    updateMissionProgress();
}

void FlightDataViewModel::updateMissionProgress()
{
    if (!m_waypointManager) {
        m_missionItemCount = 0;
        m_missionProgress = 0.0;
        m_missionProgressText = tr("No mission loaded");
        return;
    }

    double homeLatitude = 0.0;
    double homeLongitude = 0.0;
    QVector<FlightDataMissionPoint> missionPoints;
    QList<Waypoint *> waypoints = m_waypointManager->getWaypointViewOnlyList();
    if (waypoints.isEmpty()) {
        waypoints = m_waypointManager->getWaypointEditableList();
    }
    missionPoints.reserve(waypoints.size());
    for (Waypoint *waypoint : waypoints) {
        if (!waypoint || !waypoint->isGlobalFrame()
            || !waypoint->isNavigationType()) {
            continue;
        }
        if (waypoint->getId() == 0) {
            homeLatitude = waypoint->getLatitude();
            homeLongitude = waypoint->getLongitude();
            continue;
        }
        FlightDataMissionPoint point;
        point.sequence = waypoint->getId();
        point.latitude = waypoint->getLatitude();
        point.longitude = waypoint->getLongitude();
        missionPoints.append(point);
    }

    const FlightDataMissionProgress progress =
        FlightDataMissionProgressCalculator::Calculate(
            homeLatitude, homeLongitude, missionPoints,
            m_wpNo, m_wpDist);
    m_missionItemCount = progress.itemCount;
    m_missionProgress = progress.percent;
    if (progress.itemCount == 0) {
        m_missionProgressText = tr("No mission loaded");
        return;
    }
    m_missionProgressText =
        tr("Mission WP %1/%2  \u2022  %3 / %4 m  \u2022  next %5 m")
            .arg(m_wpNo)
            .arg(progress.itemCount)
            .arg(progress.travelledDistanceMeters, 0, 'f', 0)
            .arg(progress.totalDistanceMeters, 0, 'f', 0)
            .arg(m_wpDist, 0, 'f', 0);
}

void FlightDataViewModel::publish()
{
    emit telemetryChanged();
}

void FlightDataViewModel::applyToHud()
{
    if (!m_hud) {
        return;
    }
    m_hud->setRoll(m_roll);
    m_hud->setPitch(m_pitch);
    m_hud->setYaw(m_yaw);
    m_hud->setAlt(m_alt);
    m_hud->setAirSpeed(m_airSpeed);
    m_hud->setGroundSpeed(m_groundSpeed);
    m_hud->setVerticalSpeed(m_verticalSpeed);
    m_hud->setSatCount(m_satCount);
    m_hud->setGpsFixType(m_gpsFixType);
    m_hud->setArmed(m_armed);
    m_hud->setPrearmOk(m_prearmOk);
    m_hud->setMode(m_mode);
    m_hud->setBatteryVoltage(m_batteryVoltage);
    m_hud->setBatteryRemaining(m_batteryRemaining);
    m_hud->setCurrentAmps(m_currentAmps);
    m_hud->setNavBearing(m_navBearing);
    m_hud->setTargetAlt(m_targetAlt);
    m_hud->setTargetSpeed(m_targetSpeed);
    m_hud->setWindDir(m_windDir);
    m_hud->setWindVel(m_windVel);
    m_hud->setAoa(m_aoa);
    m_hud->setSsa(m_ssa);
    m_hud->setXTrackError(m_xTrackError);
    m_hud->setTurnRate(m_turnRate);
    m_hud->setWpDist(m_wpDist);
    m_hud->setWpNo(m_wpNo);
    m_hud->setBatteryVoltage2(m_batteryVoltage2);
    m_hud->setBatteryRemaining2(m_batteryRemaining2);
    m_hud->setCurrentAmps2(m_currentAmps2);
    m_hud->setThrottlePercent(m_throttlePercent);
    m_hud->setFailsafe(m_failsafe);
    m_hud->setSafetyActive(m_safetyActive);
    m_hud->setLinkQuality(m_linkQuality);
}

void FlightDataViewModel::updateAttitude(UASInterface *uas, double roll,
                                         double pitch, double yaw, quint64 timestamp)
{
    Q_UNUSED(timestamp)
    if (!isCurrentUas(uas)) return;
    m_roll = roll * kRadiansToDegrees;
    m_pitch = pitch * kRadiansToDegrees;
    m_yaw = headingDegrees(yaw);
    publish();
}

void FlightDataViewModel::updateAttitudeRates(int uasId, double rollRate,
                                              double pitchRate, double yawRate,
                                              quint64 timestamp)
{
    Q_UNUSED(rollRate)
    Q_UNUSED(pitchRate)
    Q_UNUSED(timestamp)
    if (!m_uas || uasId != m_uas->getUASID()) return;
    m_turnRate = yawRate * kRadiansToDegrees;
    publish();
}

void FlightDataViewModel::updateAltitude(UASInterface *uas, double altitudeAmsl,
                                         double altitudeRelative, double climbRate,
                                         quint64 timestamp)
{
    Q_UNUSED(altitudeAmsl)
    Q_UNUSED(timestamp)
    if (!isCurrentUas(uas)) return;
    m_alt = altitudeRelative;
    m_verticalSpeed = climbRate;
    publish();
}

void FlightDataViewModel::updateSpeed(UASInterface *uas, double groundSpeed,
                                      double airSpeed, quint64 timestamp)
{
    Q_UNUSED(timestamp)
    if (!isCurrentUas(uas)) return;
    m_groundSpeed = groundSpeed;
    m_airSpeed = airSpeed;
    publish();
}

void FlightDataViewModel::updateBattery(UASInterface *uas, double voltage,
                                        double current, double percent, int seconds)
{
    Q_UNUSED(seconds)
    if (!isCurrentUas(uas)) return;
    m_batteryVoltage = voltage;
    m_currentAmps = current;
    m_batteryRemaining = qBound(0, qRound(percent), 100);
    emit batteryTelemetryChanged(voltage, percent);
    publish();
}

void FlightDataViewModel::updateThrust(UASInterface *uas, double thrust)
{
    if (!isCurrentUas(uas)) return;
    m_throttlePercent = qBound(0.0, thrust * 100.0, 100.0);
    publish();
}

void FlightDataViewModel::updateArmed(bool armed)
{
    m_armed = armed;
    if (armed) {
        m_prearmOk = true;
    }
    publish();
}

void FlightDataViewModel::updateMode(int uasId, const QString &mode,
                                     const QString &description)
{
    Q_UNUSED(description)
    if (!m_uas || uasId != m_uas->getUASID()) return;
    if (!mode.isEmpty()) m_mode = mode;
    publish();
}

void FlightDataViewModel::updateNavMode(int uasId, int mode, const QString &text)
{
    Q_UNUSED(mode)
    if (!m_uas || uasId != m_uas->getUASID() || text.isEmpty()) return;
    m_mode = text;
    publish();
}

void FlightDataViewModel::updateGpsFix(UASInterface *uas, int fix)
{
    if (!isCurrentUas(uas)) return;
    m_gpsFixType = fix;
    publish();
}

void FlightDataViewModel::updateSatelliteCount(int count, const QString &name)
{
    Q_UNUSED(name)
    m_satCount = count;
    publish();
}

void FlightDataViewModel::updateGpsHdop(double value, const QString &name)
{
    Q_UNUSED(name)
    m_gpsHdop = value;
    publish();
}

void FlightDataViewModel::updateDropRate(int uasId, float receiveDrop)
{
    if (!m_uas || uasId != m_uas->getUASID()) return;
    m_linkQuality = qBound(0.0, 100.0 - static_cast<double>(receiveDrop), 100.0);
    publish();
}

void FlightDataViewModel::updateNavigation(UASInterface *uas, double altitudeError,
                                           double speedError, double xtrackError)
{
    if (!isCurrentUas(uas)) return;
    m_targetAlt = m_alt + altitudeError;
    m_targetSpeed = (m_airSpeed > 0.0 ? m_airSpeed : m_groundSpeed) + speedError;
    m_xTrackError = xtrackError;
    if (m_uas) {
        m_navBearing = m_uas->property("bearingToWaypoint").toDouble();
        m_wpDist = m_uas->property("distToWaypoint").toDouble();
    }
    updateMissionProgress();
    publish();
}

void FlightDataViewModel::updateValue(int uasId, const QString &name,
                                      const QString &unit, const QVariant &value,
                                      quint64 timestamp)
{
    Q_UNUSED(unit)
    Q_UNUSED(timestamp)
    if (!m_uas || uasId != m_uas->getUASID()) return;
    const double number = value.toDouble();
    if (nameContains(name, QStringLiteral("GPS Sats"))) {
        m_satCount = number;
    } else if (nameContains(name, QStringLiteral("GPS Fix"))) {
        m_gpsFixType = value.toInt();
    } else if (nameContains(name, QStringLiteral("bearingToWaypoint"))) {
        m_navBearing = number;
    } else if (nameContains(name, QStringLiteral("distToWaypoint"))) {
        m_wpDist = number;
        updateMissionProgress();
    } else if (nameContains(name, QStringLiteral("Wind Direction"))
               || nameContains(name, QStringLiteral("wind_dir"))) {
        m_windDir = number;
    } else if (nameContains(name, QStringLiteral("Wind Speed"))
               || nameContains(name, QStringLiteral("wind_vel"))) {
        m_windVel = number;
    } else if (nameContains(name, QStringLiteral("AOA"))) {
        m_aoa = number;
    } else if (nameContains(name, QStringLiteral("SSA"))) {
        m_ssa = number;
    } else if (nameContains(name, QStringLiteral("Battery2"))
               && nameContains(name, QStringLiteral("Voltage"))) {
        m_batteryVoltage2 = number;
    } else if (nameContains(name, QStringLiteral("Battery2"))
               && nameContains(name, QStringLiteral("Current"))) {
        m_currentAmps2 = number;
    } else if (nameContains(name, QStringLiteral("Battery2"))) {
        m_batteryRemaining2 = qBound(0, value.toInt(), 100);
    } else {
        return;
    }
    publish();
}

void FlightDataViewModel::updateTextMessage(int uasId, int componentId,
                                            int severity, const QString &text)
{
    Q_UNUSED(componentId)
    Q_UNUSED(severity)
    if (!m_uas || uasId != m_uas->getUASID()) return;
    if (nameContains(text, QStringLiteral("PreArm"))
        || nameContains(text, QStringLiteral("not arm"))) {
        m_prearmOk = false;
    } else if (nameContains(text, QStringLiteral("arming checks passed"))) {
        m_prearmOk = true;
    }
    if (nameContains(text, QStringLiteral("failsafe"))) {
        m_failsafe = !nameContains(text, QStringLiteral("cleared"))
            && !nameContains(text, QStringLiteral("resolved"));
    }
    if (nameContains(text, QStringLiteral("safety on"))) m_safetyActive = true;
    if (nameContains(text, QStringLiteral("safety off"))) m_safetyActive = false;
    publish();
}

void FlightDataViewModel::updateStatus(UASInterface *uas, const QString &status,
                                       const QString &description)
{
    if (!isCurrentUas(uas)) return;
    const QString combined = status + QLatin1Char(' ') + description;
    if (nameContains(combined, QStringLiteral("failsafe"))) m_failsafe = true;
    if (nameContains(combined, QStringLiteral("active")) && !status.isEmpty()) {
        m_linkQuality = qMax(m_linkQuality, 100.0);
    }
    publish();
}

void FlightDataViewModel::updateHeartbeat(UASInterface *uas)
{
    if (!isCurrentUas(uas)) return;
    if (m_linkQuality <= 0.0) m_linkQuality = 100.0;
    publish();
}

void FlightDataViewModel::updateHeartbeatTimeout(bool timeout, unsigned int milliseconds)
{
    Q_UNUSED(milliseconds)
    if (timeout) m_linkQuality = 0.0;
    publish();
}

void FlightDataViewModel::updateWaypoint(quint16 sequence)
{
    m_wpNo = sequence;
    updateMissionProgress();
    publish();
}

void FlightDataViewModel::updateWaypointDistance(double distance)
{
    m_wpDist = distance;
    updateMissionProgress();
    publish();
}
