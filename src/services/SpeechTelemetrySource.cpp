#include "SpeechTelemetrySource.h"

#include "comm/VehicleTargetManager.h"
#include "services/StatusTextPolicy.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace {
QString tableMode(quint32 mode,
                  std::initializer_list<std::pair<quint32, const char *>> table)
{
    for (const auto &entry : table) {
        if (entry.first == mode) {
            return QString::fromLatin1(entry.second);
        }
    }
    return QStringLiteral("Mode %1").arg(mode);
}

bool isCopter(int type)
{
    switch (type) {
    case MAV_TYPE_TRICOPTER:
    case MAV_TYPE_QUADROTOR:
    case MAV_TYPE_COAXIAL:
    case MAV_TYPE_HELICOPTER:
    case MAV_TYPE_HEXAROTOR:
    case MAV_TYPE_OCTOROTOR:
    case MAV_TYPE_DODECAROTOR:
    case MAV_TYPE_DECAROTOR:
        return true;
    default:
        return false;
    }
}

bool isPlane(int type)
{
    switch (type) {
    case MAV_TYPE_FIXED_WING:
    case MAV_TYPE_VTOL_DUOROTOR:
    case MAV_TYPE_VTOL_QUADROTOR:
    case MAV_TYPE_VTOL_TILTROTOR:
    case MAV_TYPE_VTOL_RESERVED2:
    case MAV_TYPE_VTOL_RESERVED3:
    case MAV_TYPE_VTOL_RESERVED4:
    case MAV_TYPE_VTOL_RESERVED5:
        return true;
    default:
        return false;
    }
}
} // namespace

SpeechTelemetrySource::SpeechTelemetrySource(
    VehicleTargetManager *targets, Clock clock, QObject *parent)
    : QObject(parent)
    , m_targets(targets)
    , m_clock(std::move(clock))
{
    qRegisterMetaType<ExactStatusText>();
    qRegisterMetaType<ExactSpeechTelemetryEvent>();
    m_elapsedClock.start();
    if (!m_targets) {
        return;
    }
    connect(m_targets, &VehicleTargetManager::targetGenerationChanged,
            this, &SpeechTelemetrySource::invalidateTargetEpoch);
    connect(m_targets, &VehicleTargetManager::targetGenerationSettled,
            this, &SpeechTelemetrySource::resetTargetEpoch);
    resetTargetEpoch();
}

void SpeechTelemetrySource::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (!accepts(linkId, message)) {
        return;
    }

    const VehicleTargetLease capturedLease = m_snapshot.lease;
    const qint64 observedMs = nowMs();
    bool changed = m_snapshot.lastPacketMs != observedMs;
    m_snapshot.lastPacketMs = observedMs;
    bool armedEvent = false;
    bool modeEvent = false;
    bool waypointEvent = false;
    bool batteryEvent = false;
    bool statusTextEvent = false;
    ExactStatusText completedStatusText;

    switch (message.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
        mavlink_heartbeat_t heartbeat{};
        mavlink_msg_heartbeat_decode(&message, &heartbeat);
        const bool armed =
            (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
        const QString mode = modeText(heartbeat.autopilot, heartbeat.type,
                                      heartbeat.custom_mode,
                                      heartbeat.base_mode);
        armedEvent = (m_snapshot.heartbeatValid
                      && m_snapshot.armed != armed)
            || (!m_snapshot.heartbeatValid && armed);
        modeEvent = !m_snapshot.modeValid || m_snapshot.mode != mode;
        changed = changed || !m_snapshot.heartbeatValid
            || m_snapshot.armed != armed || modeEvent;
        m_snapshot.heartbeatValid = true;
        m_snapshot.armed = armed;
        m_snapshot.modeValid = true;
        m_snapshot.customMode = heartbeat.custom_mode;
        m_snapshot.mode = mode;
        m_autopilot = heartbeat.autopilot;
        m_vehicleType = heartbeat.type;
        if (m_flightVersionSeen) {
            updateFirmwareCompatibility();
        }
        break;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t position{};
        mavlink_msg_global_position_int_decode(&message, &position);
        const double altitudeMeters =
            static_cast<double>(position.relative_alt) / 1000.0;
        const bool velocityValid =
            position.vx != std::numeric_limits<qint16>::max()
            && position.vy != std::numeric_limits<qint16>::max();
        changed = changed || !m_snapshot.altitudeValid
            || m_snapshot.altitudeMeters != altitudeMeters;
        m_snapshot.altitudeValid = true;
        m_snapshot.altitudeMeters = altitudeMeters;
        if (velocityValid) {
            const double northMps = static_cast<double>(position.vx) / 100.0;
            const double eastMps = static_cast<double>(position.vy) / 100.0;
            const double groundSpeedMps = std::hypot(northMps, eastMps);
            changed = changed || !m_snapshot.groundSpeedValid
                || m_snapshot.groundSpeedMps != groundSpeedMps;
            m_snapshot.groundSpeedValid = true;
            m_snapshot.groundSpeedMps = groundSpeedMps;
        } else if (m_snapshot.groundSpeedValid) {
            changed = true;
            m_snapshot.groundSpeedValid = false;
            m_snapshot.groundSpeedMps = 0.0;
        }
        break;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
        mavlink_vfr_hud_t hud{};
        mavlink_msg_vfr_hud_decode(&message, &hud);
        if (std::isfinite(static_cast<double>(hud.airspeed))) {
            const double airspeedMps = static_cast<double>(hud.airspeed);
            changed = changed || !m_snapshot.airspeedValid
                || m_snapshot.airspeedMps != airspeedMps;
            m_snapshot.airspeedValid = true;
            m_snapshot.airspeedMps = airspeedMps;
        } else if (m_snapshot.airspeedValid) {
            changed = true;
            m_snapshot.airspeedValid = false;
            m_snapshot.airspeedMps = 0.0;
        }
        if (std::isfinite(static_cast<double>(hud.groundspeed))) {
            const double groundSpeedMps = static_cast<double>(hud.groundspeed);
            changed = changed || !m_snapshot.groundSpeedValid
                || m_snapshot.groundSpeedMps != groundSpeedMps;
            m_snapshot.groundSpeedValid = true;
            m_snapshot.groundSpeedMps = groundSpeedMps;
        } else if (m_snapshot.groundSpeedValid) {
            changed = true;
            m_snapshot.groundSpeedValid = false;
            m_snapshot.groundSpeedMps = 0.0;
        }
        break;
    }
    case MAVLINK_MSG_ID_MISSION_CURRENT: {
        mavlink_mission_current_t current{};
        mavlink_msg_mission_current_decode(&message, &current);
        waypointEvent = !m_snapshot.waypointValid
            || m_snapshot.waypointNumber != static_cast<int>(current.seq);
        changed = changed || waypointEvent;
        m_snapshot.waypointValid = true;
        m_snapshot.waypointNumber = static_cast<int>(current.seq);
        break;
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
        mavlink_sys_status_t status{};
        mavlink_msg_sys_status_decode(&message, &status);
        const bool voltageValid =
            status.voltage_battery != std::numeric_limits<quint16>::max();
        const bool remainingValid = status.battery_remaining >= 0;
        const double voltage = voltageValid
            ? static_cast<double>(status.voltage_battery) / 1000.0 : 0.0;
        const double remaining = remainingValid
            ? static_cast<double>(status.battery_remaining) : 0.0;
        const bool batteryChanged =
            m_snapshot.batteryVoltageValid != voltageValid
            || m_snapshot.batteryRemainingValid != remainingValid
            || (voltageValid && m_snapshot.batteryVoltage != voltage)
            || (remainingValid
                && m_snapshot.batteryRemainingPercent != remaining);
        changed = changed || batteryChanged;
        // Battery cadence is time based, so unchanged valid telemetry must
        // still offer the policy another sample after its 30-second holdoff.
        batteryEvent = voltageValid || remainingValid;
        m_snapshot.batteryVoltageValid = voltageValid;
        m_snapshot.batteryVoltage = voltage;
        m_snapshot.batteryRemainingValid = remainingValid;
        m_snapshot.batteryRemainingPercent = remaining;
        break;
    }
    case MAVLINK_MSG_ID_AUTOPILOT_VERSION: {
        mavlink_autopilot_version_t version{};
        mavlink_msg_autopilot_version_decode(&message, &version);
        m_flightSwVersion = version.flight_sw_version;
        m_flightCustomVersion = QByteArray(
            reinterpret_cast<const char *>(version.flight_custom_version),
            int(sizeof(version.flight_custom_version)));
        m_flightVersionSeen = m_flightSwVersion != 0;
        updateFirmwareCompatibility();
        break;
    }
    case MAVLINK_MSG_ID_STATUSTEXT:
        statusTextEvent = observeStatusText(
            capturedLease, message, observedMs, &completedStatusText);
        break;
    default:
        break;
    }

    if (changed) {
        emit snapshotChanged();
    }
    if (!isCurrentLease(capturedLease)) {
        return;
    }
    if (armedEvent) {
        emit armedChanged(m_snapshot.armed);
        if (!isCurrentLease(capturedLease)) {
            return;
        }
        ExactSpeechTelemetryEvent event;
        event.lease = capturedLease;
        event.kind = ExactSpeechTelemetryEvent::Armed;
        event.armed = m_snapshot.armed;
        emit exactSpeechTelemetry(event);
        if (!isCurrentLease(capturedLease)) {
            return;
        }
    }
    if (modeEvent) {
        emit flightModeChanged(m_snapshot.mode);
        if (!isCurrentLease(capturedLease)) {
            return;
        }
        ExactSpeechTelemetryEvent event;
        event.lease = capturedLease;
        event.kind = ExactSpeechTelemetryEvent::FlightMode;
        event.mode = m_snapshot.mode;
        emit exactSpeechTelemetry(event);
        if (!isCurrentLease(capturedLease)) {
            return;
        }
    }
    if (waypointEvent) {
        emit waypointChanged(m_snapshot.waypointNumber);
        if (!isCurrentLease(capturedLease)) {
            return;
        }
        ExactSpeechTelemetryEvent event;
        event.lease = capturedLease;
        event.kind = ExactSpeechTelemetryEvent::Waypoint;
        event.waypoint = m_snapshot.waypointNumber;
        emit exactSpeechTelemetry(event);
        if (!isCurrentLease(capturedLease)) {
            return;
        }
    }
    if (batteryEvent) {
        emit batteryTelemetryChanged(
            m_snapshot.batteryVoltageValid
                ? m_snapshot.batteryVoltage : 0.0,
            m_snapshot.batteryRemainingValid
                ? m_snapshot.batteryRemainingPercent : 0.0);
        if (!isCurrentLease(capturedLease)) {
            return;
        }
        ExactSpeechTelemetryEvent event;
        event.lease = capturedLease;
        event.kind = ExactSpeechTelemetryEvent::Battery;
        event.batteryVoltage = m_snapshot.batteryVoltageValid
            ? m_snapshot.batteryVoltage : 0.0;
        event.batteryRemainingPercent = m_snapshot.batteryRemainingValid
            ? m_snapshot.batteryRemainingPercent : 0.0;
        emit exactSpeechTelemetry(event);
        if (!isCurrentLease(capturedLease)) {
            return;
        }
    }
    // A snapshot listener may synchronously select another target. The event
    // still carries the captured lease, and both producer and consumer check
    // it again so connection order cannot leak a stale vehicle message.
    if (statusTextEvent && isCurrentLease(completedStatusText.lease)) {
        emit statusTextCompleted(completedStatusText);
    }
}

QString SpeechTelemetrySource::modeText(
    int autopilot, int vehicleType, quint32 customMode, quint8 baseMode)
{
    if (autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA) {
        if (isCopter(vehicleType)) {
            return tableMode(customMode, {
                {0, "Stabilize"}, {1, "Acro"}, {2, "Alt Hold"},
                {3, "Auto"}, {4, "Guided"}, {5, "Loiter"}, {6, "RTL"},
                {7, "Circle"}, {9, "Land"}, {11, "Drift"}, {13, "Sport"},
                {14, "Flip"}, {15, "Auto Tune"}, {16, "Pos Hold"},
                {17, "Brake"}, {18, "Throw"}, {19, "Avoid-ADSB"},
                {20, "Guided no GPS"}, {21, "Smart RTL"}, {22, "Flowhold"},
                {23, "Follow"}, {24, "ZigZag"}, {25, "System ID"},
                {26, "Auto Rotate"}, {27, "Auto RTL"}, {28, "Turtle"},
                {29, "Rate Acro"}
            });
        }
        if (isPlane(vehicleType)) {
            return tableMode(customMode, {
                {0, "Manual"}, {1, "Circle"}, {2, "Stabilize"},
                {3, "Training"}, {4, "Acro"}, {5, "Fly by wire A"},
                {6, "Fly by wire B"}, {7, "Cruise"}, {8, "Autotune"},
                {9, "Land"}, {10, "Auto"}, {11, "RTL"}, {12, "Loiter"},
                {13, "Takeoff"}, {14, "Avoid-ADSB"}, {15, "Guided"},
                {16, "Initializing"}, {17, "Q-Stabilize"}, {18, "Q-Hover"},
                {19, "Q-Loiter"}, {20, "Q-Land"}, {21, "Q-RTL"},
                {22, "Q-Autotune"}, {23, "Q-Acro"}, {24, "Thermal"},
                {25, "Loiter Alt Q-Land"}
            });
        }
        if (vehicleType == MAV_TYPE_GROUND_ROVER
            || vehicleType == MAV_TYPE_SURFACE_BOAT) {
            return tableMode(customMode, {
                {0, "Manual"}, {1, "Acro"}, {2, "Learning"},
                {3, "Steering"}, {4, "Hold"}, {5, "Loiter"},
                {6, "Follow"}, {7, "Simple"}, {8, "Dock"},
                {9, "Circle"}, {10, "Auto"}, {11, "RTL"},
                {12, "Smart RTL"}, {15, "Guided"}, {16, "Initialising"}
            });
        }
        if (vehicleType == MAV_TYPE_ANTENNA_TRACKER) {
            return QStringLiteral("Ant Tracker");
        }
    }

    if (autopilot == MAV_AUTOPILOT_PX4
        && (baseMode & MAV_MODE_FLAG_CUSTOM_MODE_ENABLED)) {
        const quint8 mainMode = static_cast<quint8>((customMode >> 16) & 0xffU);
        const quint8 subMode = static_cast<quint8>((customMode >> 24) & 0xffU);
        if (mainMode == 4) {
            return tableMode(subMode, {
                {1, "Auto: Ready"}, {2, "Auto: Takeoff"},
                {3, "Auto: Loiter"}, {4, "Auto"}, {5, "RTL"},
                {6, "Auto: Landing"}
            });
        }
        return tableMode(mainMode, {
            {1, "Manual"}, {2, "Altitude Control"},
            {3, "Position Control"}, {5, "Acro"},
            {6, "Offboard Control"}, {7, "Stabilized"},
            {8, "Rattitude"}
        });
    }

    if (baseMode & MAV_MODE_FLAG_AUTO_ENABLED) {
        return QStringLiteral("Auto");
    }
    if (baseMode & MAV_MODE_FLAG_GUIDED_ENABLED) {
        return QStringLiteral("Guided");
    }
    if (baseMode & MAV_MODE_FLAG_STABILIZE_ENABLED) {
        return QStringLiteral("Stabilize");
    }
    if (baseMode & MAV_MODE_FLAG_MANUAL_INPUT_ENABLED) {
        return QStringLiteral("Manual");
    }
    return QStringLiteral("Mode %1").arg(customMode);
}

void SpeechTelemetrySource::invalidateTargetEpoch()
{
    clearStatusTextState();
    m_snapshot = Snapshot();
    emit snapshotChanged();
}

void SpeechTelemetrySource::resetTargetEpoch()
{
    clearStatusTextState();
    Snapshot next;
    if (m_targets) {
        next.lease = m_targets->acquireTarget();
    }
    if (next.lease.isValid()) {
        next.connectedSinceMs = nowMs();
    }
    m_snapshot = next;
    emit snapshotChanged();
    emit targetEpochChanged();
}

bool SpeechTelemetrySource::accepts(
    int linkId, const mavlink_message_t &message) const
{
    const VehicleTargetLease &lease = m_snapshot.lease;
    return m_targets && lease.isValid()
        && linkId == lease.endpoint.linkId
        && message.sysid == lease.endpoint.systemId
        && message.compid == lease.endpoint.componentId
        && m_targets->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation);
}

bool SpeechTelemetrySource::isCurrentLease(
    const VehicleTargetLease &lease) const
{
    return m_targets && lease.isValid() && m_snapshot.lease.isValid()
        && lease.generation == m_snapshot.lease.generation
        && lease.endpoint.sameIdentity(m_snapshot.lease.endpoint)
        && m_targets->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation);
}

bool SpeechTelemetrySource::observeStatusText(
    const VehicleTargetLease &lease, const mavlink_message_t &message,
    qint64 observedMs, ExactStatusText *completed)
{
    if (!completed) {
        return false;
    }

    constexpr qint64 kAssemblyTimeoutMs = 5000;
    constexpr int kMaximumAssemblies = 8;
    constexpr int kMaximumMessageBytes = 512;

    for (auto it = m_statusAssemblies.begin();
         it != m_statusAssemblies.end();) {
        if (it->updatedAtMs >= 0
            && observedMs > it->updatedAtMs + kAssemblyTimeoutMs) {
            it = m_statusAssemblies.erase(it);
        } else {
            ++it;
        }
    }

    mavlink_statustext_t status{};
    mavlink_msg_statustext_decode(&message, &status);
    QByteArray chunk(reinterpret_cast<const char *>(status.text),
                     MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN);
    const int terminator = chunk.indexOf('\0');
    const bool terminal = terminator >= 0;
    if (terminal) {
        chunk.resize(terminator);
    }

    QByteArray utf8;
    const quint16 id = status.id;
    if (id == 0) {
        if (status.chunk_seq != 0) {
            return false;
        }
        utf8 = chunk;
    } else if (status.chunk_seq == 0) {
        if (!m_statusAssemblies.contains(id)
            && m_statusAssemblies.size() >= kMaximumAssemblies) {
            auto oldest = m_statusAssemblies.begin();
            for (auto it = m_statusAssemblies.begin();
                 it != m_statusAssemblies.end(); ++it) {
                if (it->updatedAtMs < oldest->updatedAtMs) {
                    oldest = it;
                }
            }
            m_statusAssemblies.erase(oldest);
        }
        if (terminal) {
            m_statusAssemblies.remove(id);
            utf8 = chunk;
        } else {
            StatusAssembly assembly;
            assembly.rawSeverity = status.severity;
            assembly.nextChunk = 1;
            assembly.utf8 = chunk;
            assembly.updatedAtMs = observedMs;
            m_statusAssemblies.insert(id, assembly);
            return false;
        }
    } else {
        auto it = m_statusAssemblies.find(id);
        if (it == m_statusAssemblies.end()) {
            return false;
        }
        if (it->rawSeverity != status.severity
            || it->nextChunk != status.chunk_seq
            || it->utf8.size() + chunk.size() > kMaximumMessageBytes) {
            m_statusAssemblies.erase(it);
            return false;
        }
        it->utf8.append(chunk);
        it->updatedAtMs = observedMs;
        if (!terminal) {
            ++it->nextChunk;
            return false;
        }
        utf8 = it->utf8;
        m_statusAssemblies.erase(it);
    }

    const QString text = QString::fromUtf8(utf8.constData(), utf8.size());
    if (m_autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA) {
        m_firmwareVersion.parseVersion(text);
        m_legacySeverityCompatibility =
            StatusTextPolicy::requiresLegacySeverityCompatibility(
                m_firmwareVersion);
    }
    const int normalizedSeverity = m_legacySeverityCompatibility
        ? StatusTextPolicy::normalizeLegacySeverity(status.severity)
        : int(status.severity);

    completed->lease = lease;
    completed->rawSeverity = status.severity;
    completed->severity = static_cast<quint8>(normalizedSeverity);
    completed->messageId = id;
    completed->text = text;
    completed->completedAtMs = observedMs;
    return true;
}

void SpeechTelemetrySource::updateFirmwareCompatibility()
{
    if (!m_flightVersionSeen) {
        return;
    }
    APMFirmwareVersion version;
    version.parseFlightSwVersion(
        m_flightSwVersion, m_flightCustomVersion,
        m_autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA
            ? StatusTextPolicy::firmwareVehicleType(m_vehicleType)
            : QString());
    m_firmwareVersion = version;
    m_legacySeverityCompatibility =
        StatusTextPolicy::requiresLegacySeverityCompatibility(
            m_firmwareVersion);
}

void SpeechTelemetrySource::clearStatusTextState()
{
    m_statusAssemblies.clear();
    m_firmwareVersion = APMFirmwareVersion();
    m_flightCustomVersion.clear();
    m_flightSwVersion = 0;
    m_autopilot = MAV_AUTOPILOT_GENERIC;
    m_vehicleType = MAV_TYPE_GENERIC;
    m_flightVersionSeen = false;
    m_legacySeverityCompatibility = false;
}

qint64 SpeechTelemetrySource::nowMs() const
{
    return m_clock ? m_clock() : m_elapsedClock.elapsed();
}
