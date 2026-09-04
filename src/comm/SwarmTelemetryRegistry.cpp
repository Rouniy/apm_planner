#include "SwarmTelemetryRegistry.h"

#include <QPointer>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace
{

constexpr int MaximumSupportedHeartbeatAgeMs = 10 * 60 * 1000;
constexpr qint64 BootTimeResetThresholdMs = 5 * 1000;

bool allFinite(std::initializer_list<double> values)
{
    for (double value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

} // namespace

SwarmTelemetryRegistry::SwarmTelemetryRegistry(QObject *parent)
    : SwarmTelemetryRegistry(Clock(), parent)
{
}

SwarmTelemetryRegistry::SwarmTelemetryRegistry(Clock clock, QObject *parent)
    : QObject(parent)
    , m_clock(std::move(clock))
{
    m_monotonicClock.start();
    qRegisterMetaType<SwarmVehicleInstanceLease>();
    qRegisterMetaType<SwarmTelemetrySnapshot>();
    qRegisterMetaType<SwarmVehicleGroupLease>();
    qRegisterMetaType<SwarmTelemetryRegistry::RetirementReason>();

    m_retirementTimer.setSingleShot(false);
    connect(&m_retirementTimer, &QTimer::timeout,
            this, &SwarmTelemetryRegistry::retireStaleEndpoints);
    updateRetirementTimer();
}

quint64 SwarmTelemetryRegistry::beginLinkSession(
    int linkId, const QString &linkName)
{
    if (linkId < 0) {
        return 0;
    }

    QList<PendingEvent> events;
    const auto previous = m_linkSessions.constFind(linkId);
    if (previous != m_linkSessions.constEnd()) {
        QList<VehicleEndpoint> previousEndpoints;
        for (auto vehicle = m_vehicles.constBegin();
             vehicle != m_vehicles.constEnd(); ++vehicle) {
            if (vehicle.key().linkId == linkId) {
                previousEndpoints.append(vehicle.key());
            }
        }
        std::sort(previousEndpoints.begin(), previousEndpoints.end());
        for (const VehicleEndpoint &endpoint : previousEndpoints) {
            retireEndpoint(endpoint, RetirementReason::LinkSessionEnded,
                           &events);
        }
        PendingEvent ended;
        ended.type = PendingEventType::LinkEnded;
        ended.linkId = linkId;
        ended.epoch = previous->epoch;
        events.append(ended);
        m_linkSessions.remove(linkId);
    }

    const quint64 epoch = nextLinkSessionEpoch();
    m_linkSessions.insert(linkId, LinkSession{epoch, linkName.trimmed()});
    PendingEvent began;
    began.type = PendingEventType::LinkBegan;
    began.linkId = linkId;
    began.epoch = epoch;
    events.append(began);
    publishMutation(events);
    return epoch;
}

bool SwarmTelemetryRegistry::endLinkSession(
    int linkId, quint64 expectedSessionEpoch)
{
    const auto session = m_linkSessions.constFind(linkId);
    if (session == m_linkSessions.constEnd() || expectedSessionEpoch == 0
        || session->epoch != expectedSessionEpoch) {
        return false;
    }

    QList<VehicleEndpoint> removedEndpoints;
    for (auto vehicle = m_vehicles.constBegin();
         vehicle != m_vehicles.constEnd(); ++vehicle) {
        if (vehicle.key().linkId == linkId) {
            removedEndpoints.append(vehicle.key());
        }
    }
    std::sort(removedEndpoints.begin(), removedEndpoints.end());

    QList<PendingEvent> events;
    for (const VehicleEndpoint &endpoint : removedEndpoints) {
        retireEndpoint(endpoint, RetirementReason::LinkSessionEnded, &events);
    }
    m_linkSessions.remove(linkId);
    PendingEvent ended;
    ended.type = PendingEventType::LinkEnded;
    ended.linkId = linkId;
    ended.epoch = expectedSessionEpoch;
    events.append(ended);
    publishMutation(events);
    return true;
}

quint64 SwarmTelemetryRegistry::currentLinkSessionEpoch(int linkId) const
{
    return m_linkSessions.value(linkId).epoch;
}

bool SwarmTelemetryRegistry::observeMessage(
    int linkId, quint64 linkSessionEpoch,
    const mavlink_message_t &message)
{
    const auto session = m_linkSessions.constFind(linkId);
    if (session == m_linkSessions.constEnd() || linkSessionEpoch == 0
        || session->epoch != linkSessionEpoch
        || !isParsedMessage(message.msgid)) {
        return false;
    }

    const qint64 observedMs = nowMs();
    QList<PendingEvent> events;
    retireStaleAt(observedMs, &events);

    const VehicleEndpoint endpoint = endpointFor(linkId, *session, message);
    if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        mavlink_heartbeat_t heartbeat{};
        mavlink_msg_heartbeat_decode(&message, &heartbeat);
        if (!isCommandCapableHeartbeat(message, heartbeat)) {
            retireEndpoint(endpoint, RetirementReason::NoLongerCommandCapable,
                           &events);
            publishMutation(events);
            return false;
        }

        auto vehicle = m_vehicles.find(endpoint);
        const bool activating = vehicle == m_vehicles.end();
        if (activating) {
            if (m_vehicles.size() >= MaximumVehicleEndpoints) {
                publishMutation(events);
                return false;
            }
            VehicleRecord record;
            record.snapshot.lease.endpoint = endpoint;
            record.snapshot.lease.linkSessionEpoch = session->epoch;
            record.snapshot.lease.instanceEpoch = nextInstanceEpoch();
            vehicle = m_vehicles.insert(endpoint, record);
        }

        updateHeartbeat(&vehicle->snapshot, heartbeat, observedMs);
        PendingEvent event;
        event.type = activating ? PendingEventType::Activated
                                : PendingEventType::Updated;
        event.snapshot = vehicle->snapshot;
        events.append(event);
        publishMutation(events);
        return true;
    }

    auto vehicle = m_vehicles.find(endpoint);
    if (vehicle == m_vehicles.end()
        || vehicle->snapshot.lease.linkSessionEpoch != session->epoch) {
        publishMutation(events);
        return false;
    }

    if (observeBootTime(&(*vehicle), message) == BootTimeObservation::Reset) {
        retireEndpoint(endpoint, RetirementReason::BootTimeReset, &events);
        publishMutation(events);
        return false;
    }

    updateMessage(&vehicle->snapshot, message, observedMs);
    PendingEvent updated;
    updated.type = PendingEventType::Updated;
    updated.snapshot = vehicle->snapshot;
    events.append(updated);
    publishMutation(events);
    return true;
}

QList<VehicleEndpoint> SwarmTelemetryRegistry::endpoints() const
{
    QList<VehicleEndpoint> result = m_vehicles.keys();
    std::sort(result.begin(), result.end());
    return result;
}

SwarmVehicleInstanceLease SwarmTelemetryRegistry::acquireVehicle(
    const VehicleEndpoint &endpoint, int heartbeatMaximumAgeMs) const
{
    SwarmTelemetrySnapshot snapshot;
    return acquireSnapshot(endpoint, &snapshot, heartbeatMaximumAgeMs)
        ? snapshot.lease : SwarmVehicleInstanceLease();
}

bool SwarmTelemetryRegistry::acquireSnapshot(
    const VehicleEndpoint &endpoint, SwarmTelemetrySnapshot *snapshot,
    int heartbeatMaximumAgeMs) const
{
    if (!snapshot) {
        return false;
    }
    const auto record = m_vehicles.constFind(endpoint);
    const int maximumAge = heartbeatMaximumAgeMs > 0
        ? heartbeatMaximumAgeMs : m_heartbeatMaximumAgeMs;
    const qint64 now = nowMs();
    if (record == m_vehicles.constEnd()
        || !leaseMatchesRecord(record->snapshot.lease, *record)
        || !heartbeatIsFresh(record->snapshot, now, maximumAge)) {
        return false;
    }
    *snapshot = record->snapshot;
    return true;
}

bool SwarmTelemetryRegistry::snapshotForLease(
    const SwarmVehicleInstanceLease &lease,
    SwarmTelemetrySnapshot *snapshot) const
{
    if (!snapshot || !lease.isValid()) {
        return false;
    }
    const auto record = m_vehicles.constFind(lease.endpoint);
    if (record == m_vehicles.constEnd()
        || !leaseMatchesRecord(lease, *record)) {
        return false;
    }
    *snapshot = record->snapshot;
    return true;
}

bool SwarmTelemetryRegistry::validateLease(
    const SwarmVehicleInstanceLease &lease,
    int heartbeatMaximumAgeMs) const
{
    SwarmTelemetrySnapshot snapshot;
    if (!snapshotForLease(lease, &snapshot)) {
        return false;
    }
    const int maximumAge = heartbeatMaximumAgeMs > 0
        ? heartbeatMaximumAgeMs : m_heartbeatMaximumAgeMs;
    return heartbeatIsFresh(snapshot, nowMs(), maximumAge);
}

SwarmVehicleGroupLease SwarmTelemetryRegistry::acquireGroup(
    const QList<VehicleEndpoint> &requestedEndpoints,
    int heartbeatMaximumAgeMs) const
{
    SwarmVehicleGroupLease group;
    if (requestedEndpoints.isEmpty()
        || requestedEndpoints.size() > MaximumVehicleEndpoints) {
        return group;
    }

    QSet<VehicleEndpoint> unique;
    QList<SwarmVehicleInstanceLease> members;
    members.reserve(requestedEndpoints.size());
    const int maximumAge = heartbeatMaximumAgeMs > 0
        ? heartbeatMaximumAgeMs : m_heartbeatMaximumAgeMs;
    const qint64 now = nowMs();
    for (const VehicleEndpoint &endpoint : requestedEndpoints) {
        if (unique.contains(endpoint)) {
            return SwarmVehicleGroupLease();
        }
        unique.insert(endpoint);
        const auto record = m_vehicles.constFind(endpoint);
        if (record == m_vehicles.constEnd()
            || !leaseMatchesRecord(record->snapshot.lease, *record)
            || !heartbeatIsFresh(record->snapshot, now, maximumAge)) {
            return SwarmVehicleGroupLease();
        }
        members.append(record->snapshot.lease);
    }
    group.members = members;
    group.acquiredAtMs = now;
    return group;
}

bool SwarmTelemetryRegistry::validateGroup(
    const SwarmVehicleGroupLease &group,
    QList<SwarmTelemetrySnapshot> *snapshots,
    int heartbeatMaximumAgeMs) const
{
    if (snapshots) {
        snapshots->clear();
    }
    if (!group.isValid()
        || group.members.size() > MaximumVehicleEndpoints) {
        return false;
    }
    const int maximumAge = heartbeatMaximumAgeMs > 0
        ? heartbeatMaximumAgeMs : m_heartbeatMaximumAgeMs;
    const qint64 now = nowMs();
    QSet<VehicleEndpoint> unique;
    QList<SwarmTelemetrySnapshot> validated;
    validated.reserve(group.members.size());
    for (const SwarmVehicleInstanceLease &lease : group.members) {
        if (!lease.isValid() || unique.contains(lease.endpoint)) {
            return false;
        }
        unique.insert(lease.endpoint);
        const auto record = m_vehicles.constFind(lease.endpoint);
        if (record == m_vehicles.constEnd()
            || !leaseMatchesRecord(lease, *record)
            || !heartbeatIsFresh(record->snapshot, now, maximumAge)) {
            return false;
        }
        validated.append(record->snapshot);
    }
    if (snapshots) {
        *snapshots = validated;
    }
    return true;
}

bool SwarmTelemetryRegistry::observationIsFresh(
    qint64 observedMs, int maximumAgeMs) const
{
    if (observedMs < 0 || maximumAgeMs <= 0) {
        return false;
    }
    const qint64 now = nowMs();
    return observedMs <= now && now - observedMs <= maximumAgeMs;
}

int SwarmTelemetryRegistry::retireStaleEndpoints()
{
    QList<PendingEvent> events;
    const int retired = retireStaleAt(nowMs(), &events);
    publishMutation(events);
    return retired;
}

void SwarmTelemetryRegistry::setHeartbeatMaximumAgeForTesting(
    int maximumAgeMs)
{
    m_heartbeatMaximumAgeMs = qBound(
        1, maximumAgeMs, MaximumSupportedHeartbeatAgeMs);
    updateRetirementTimer();
}

bool SwarmTelemetryRegistry::isParsedMessage(quint32 messageId) noexcept
{
    return messageId == MAVLINK_MSG_ID_HEARTBEAT
        || messageId == MAVLINK_MSG_ID_GLOBAL_POSITION_INT
        || messageId == MAVLINK_MSG_ID_VFR_HUD
        || messageId == MAVLINK_MSG_ID_ATTITUDE
        || messageId == MAVLINK_MSG_ID_EXTENDED_SYS_STATE;
}

bool SwarmTelemetryRegistry::isCommandCapableHeartbeat(
    const mavlink_message_t &message,
    const mavlink_heartbeat_t &heartbeat) noexcept
{
    return message.sysid != 0
        && message.compid == MAV_COMP_ID_AUTOPILOT1
        && message.compid != MAV_COMP_ID_MISSIONPLANNER
        && heartbeat.type != MAV_TYPE_GCS
        && heartbeat.autopilot != MAV_AUTOPILOT_INVALID;
}

bool SwarmTelemetryRegistry::validPosition(
    qint32 latitudeE7, qint32 longitudeE7) noexcept
{
    constexpr qint32 MaximumLatitudeE7 = 900000000;
    constexpr qint32 MaximumLongitudeE7 = 1800000000;
    return latitudeE7 >= -MaximumLatitudeE7
        && latitudeE7 <= MaximumLatitudeE7
        && longitudeE7 >= -MaximumLongitudeE7
        && longitudeE7 <= MaximumLongitudeE7
        && (latitudeE7 != 0 || longitudeE7 != 0);
}

bool SwarmTelemetryRegistry::messageBootTimeMs(
    const mavlink_message_t &message, quint32 *bootTimeMs) noexcept
{
    if (!bootTimeMs) {
        return false;
    }
    switch (message.msgid) {
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t position{};
        mavlink_msg_global_position_int_decode(&message, &position);
        *bootTimeMs = position.time_boot_ms;
        return true;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
        mavlink_attitude_t attitude{};
        mavlink_msg_attitude_decode(&message, &attitude);
        *bootTimeMs = attitude.time_boot_ms;
        return true;
    }
    default:
        return false;
    }
}

VehicleEndpoint SwarmTelemetryRegistry::endpointFor(
    int linkId, const LinkSession &session,
    const mavlink_message_t &message) const
{
    VehicleEndpoint endpoint;
    endpoint.linkId = linkId;
    endpoint.systemId = message.sysid;
    endpoint.componentId = message.compid;
    endpoint.linkName = session.name;
    endpoint.componentName = VehicleEndpoint::defaultComponentName(
        message.compid);
    return endpoint;
}

bool SwarmTelemetryRegistry::leaseMatchesRecord(
    const SwarmVehicleInstanceLease &lease,
    const VehicleRecord &record) const noexcept
{
    if (!lease.isValid() || !lease.sameInstance(record.snapshot.lease)) {
        return false;
    }
    const auto session = m_linkSessions.constFind(lease.endpoint.linkId);
    return session != m_linkSessions.constEnd()
        && session->epoch == lease.linkSessionEpoch;
}

bool SwarmTelemetryRegistry::heartbeatIsFresh(
    const SwarmTelemetrySnapshot &snapshot, qint64 now,
    int maximumAgeMs) const noexcept
{
    return snapshot.heartbeatValid && snapshot.heartbeatObservedMs >= 0
        && maximumAgeMs > 0 && now >= snapshot.heartbeatObservedMs
        && now - snapshot.heartbeatObservedMs <= maximumAgeMs;
}

void SwarmTelemetryRegistry::updateHeartbeat(
    SwarmTelemetrySnapshot *snapshot,
    const mavlink_heartbeat_t &heartbeat, qint64 observedMs) const
{
    snapshot->lastMessageMs = observedMs;
    snapshot->heartbeatObservedMs = observedMs;
    snapshot->heartbeatValid = true;
    snapshot->armed =
        (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
    snapshot->autopilot = heartbeat.autopilot;
    snapshot->vehicleType = heartbeat.type;
    snapshot->baseMode = heartbeat.base_mode;
    snapshot->customMode = heartbeat.custom_mode;
    snapshot->systemStatus = heartbeat.system_status;
}

void SwarmTelemetryRegistry::updateMessage(
    SwarmTelemetrySnapshot *snapshot,
    const mavlink_message_t &message, qint64 observedMs) const
{
    snapshot->lastMessageMs = observedMs;
    switch (message.msgid) {
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t position{};
        mavlink_msg_global_position_int_decode(&message, &position);
        snapshot->positionObservedMs = observedMs;
        snapshot->positionValid = validPosition(position.lat, position.lon);
        snapshot->latitudeDegrees =
            static_cast<double>(position.lat) / 1.0e7;
        snapshot->longitudeDegrees =
            static_cast<double>(position.lon) / 1.0e7;
        snapshot->altitudeAmslM =
            static_cast<double>(position.alt) / 1000.0;
        snapshot->relativeAltitudeM =
            static_cast<double>(position.relative_alt) / 1000.0;
        snapshot->velocityValid = true;
        snapshot->velocityNorthMps =
            static_cast<double>(position.vx) / 100.0;
        snapshot->velocityEastMps =
            static_cast<double>(position.vy) / 100.0;
        snapshot->velocityDownMps =
            static_cast<double>(position.vz) / 100.0;
        snapshot->headingValid =
            position.hdg != std::numeric_limits<quint16>::max();
        if (snapshot->headingValid) {
            snapshot->headingDegrees =
                static_cast<double>(position.hdg) / 100.0;
        }
        break;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
        mavlink_vfr_hud_t hud{};
        mavlink_msg_vfr_hud_decode(&message, &hud);
        snapshot->vfrHudObservedMs = observedMs;
        snapshot->vfrHudValid = hud.throttle <= 100 && allFinite(
            {hud.airspeed, hud.groundspeed, hud.alt, hud.climb});
        if (snapshot->vfrHudValid) {
            snapshot->airspeedMps = hud.airspeed;
            snapshot->groundSpeedMps = hud.groundspeed;
            snapshot->vfrAltitudeAmslM = hud.alt;
            snapshot->climbMps = hud.climb;
            snapshot->throttlePercent = hud.throttle;
        }
        if (hud.heading >= 0 && hud.heading <= 360) {
            snapshot->headingValid = true;
            snapshot->headingDegrees = hud.heading;
        } else {
            snapshot->headingValid = false;
        }
        break;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
        mavlink_attitude_t attitude{};
        mavlink_msg_attitude_decode(&message, &attitude);
        snapshot->attitudeObservedMs = observedMs;
        snapshot->attitudeValid = allFinite(
            {attitude.roll, attitude.pitch, attitude.yaw,
             attitude.rollspeed, attitude.pitchspeed, attitude.yawspeed});
        if (snapshot->attitudeValid) {
            snapshot->rollRadians = attitude.roll;
            snapshot->pitchRadians = attitude.pitch;
            snapshot->yawRadians = attitude.yaw;
            snapshot->rollRateRadiansPerSecond = attitude.rollspeed;
            snapshot->pitchRateRadiansPerSecond = attitude.pitchspeed;
            snapshot->yawRateRadiansPerSecond = attitude.yawspeed;
        }
        break;
    }
    case MAVLINK_MSG_ID_EXTENDED_SYS_STATE: {
        mavlink_extended_sys_state_t state{};
        mavlink_msg_extended_sys_state_decode(&message, &state);
        snapshot->extendedSystemStateObservedMs = observedMs;
        snapshot->extendedSystemStateValid = true;
        snapshot->vtolState = state.vtol_state;
        snapshot->landedState = state.landed_state;
        break;
    }
    default:
        break;
    }
}

SwarmTelemetryRegistry::BootTimeObservation
SwarmTelemetryRegistry::observeBootTime(
    VehicleRecord *record, const mavlink_message_t &message)
{
    quint32 bootTimeMs = 0;
    if (!record || !messageBootTimeMs(message, &bootTimeMs)) {
        return BootTimeObservation::NotPresent;
    }
    if (!record->bootTimeValid) {
        record->bootTimeValid = true;
        record->latestBootTimeMs = bootTimeMs;
        return BootTimeObservation::Accepted;
    }

    // Signed modular subtraction treats the natural uint32 wrap as forward
    // progress while still recognizing a meaningful reboot regression. Small
    // negative deltas are tolerated because telemetry streams may interleave
    // or arrive slightly out of order; they never move the high-water mark.
    const quint32 forwardDelta = bootTimeMs - record->latestBootTimeMs;
    if (forwardDelta < 0x80000000U) {
        record->latestBootTimeMs = bootTimeMs;
        return BootTimeObservation::Accepted;
    }
    const qint64 regressionMs = static_cast<qint64>(
        record->latestBootTimeMs - bootTimeMs);
    return regressionMs > BootTimeResetThresholdMs
        ? BootTimeObservation::Reset
        : BootTimeObservation::Accepted;
}

int SwarmTelemetryRegistry::retireStaleAt(
    qint64 now, QList<PendingEvent> *events)
{
    QList<VehicleEndpoint> stale;
    for (auto vehicle = m_vehicles.constBegin();
         vehicle != m_vehicles.constEnd(); ++vehicle) {
        if (!heartbeatIsFresh(vehicle->snapshot, now,
                              m_heartbeatMaximumAgeMs)) {
            stale.append(vehicle.key());
        }
    }
    std::sort(stale.begin(), stale.end());
    for (const VehicleEndpoint &endpoint : stale) {
        retireEndpoint(endpoint, RetirementReason::HeartbeatStale, events);
    }
    return stale.size();
}

bool SwarmTelemetryRegistry::retireEndpoint(
    const VehicleEndpoint &endpoint, RetirementReason reason,
    QList<PendingEvent> *events)
{
    const auto vehicle = m_vehicles.find(endpoint);
    if (vehicle == m_vehicles.end()) {
        return false;
    }
    const SwarmVehicleInstanceLease lease = vehicle->snapshot.lease;
    m_vehicles.erase(vehicle);
    PendingEvent retired;
    retired.type = PendingEventType::Retired;
    retired.lease = lease;
    retired.retirementReason = reason;
    events->append(retired);
    return true;
}

void SwarmTelemetryRegistry::publishMutation(QList<PendingEvent> events)
{
    if (events.isEmpty()) {
        return;
    }
    ++m_revision;
    for (const PendingEvent &event : events) {
        m_pendingEvents.enqueue(event);
    }
    PendingEvent changed;
    changed.type = PendingEventType::RegistryChanged;
    changed.revision = m_revision;
    m_pendingEvents.enqueue(changed);
    drainPendingEvents();
}

void SwarmTelemetryRegistry::drainPendingEvents()
{
    if (m_drainingEvents) {
        return;
    }
    m_drainingEvents = true;
    QPointer<SwarmTelemetryRegistry> guard(this);
    while (!m_pendingEvents.isEmpty()) {
        const PendingEvent event = m_pendingEvents.dequeue();
        switch (event.type) {
        case PendingEventType::LinkBegan:
            emit linkSessionBegan(event.linkId, event.epoch);
            break;
        case PendingEventType::LinkEnded:
            emit linkSessionEnded(event.linkId, event.epoch);
            break;
        case PendingEventType::Activated:
            emit endpointActivated(event.snapshot);
            break;
        case PendingEventType::Updated:
            emit endpointUpdated(event.snapshot);
            break;
        case PendingEventType::Retired:
            emit endpointRetired(event.lease, event.retirementReason);
            break;
        case PendingEventType::RegistryChanged:
            if (event.revision == m_revision) {
                emit registryChanged(event.revision);
            }
            break;
        }
        if (guard.isNull()) {
            return;
        }
    }
    m_drainingEvents = false;
}

qint64 SwarmTelemetryRegistry::nowMs() const
{
    const qint64 sampled = m_clock ? m_clock() : m_monotonicClock.elapsed();
    const qint64 nonNegative = qMax<qint64>(0, sampled);
    m_lastNowMs = qMax(m_lastNowMs, nonNegative);
    return m_lastNowMs;
}

quint64 SwarmTelemetryRegistry::nextLinkSessionEpoch()
{
    ++m_nextLinkEpoch;
    if (m_nextLinkEpoch == 0) {
        ++m_nextLinkEpoch;
    }
    return m_nextLinkEpoch;
}

quint64 SwarmTelemetryRegistry::nextInstanceEpoch()
{
    ++m_nextVehicleInstanceEpoch;
    if (m_nextVehicleInstanceEpoch == 0) {
        ++m_nextVehicleInstanceEpoch;
    }
    return m_nextVehicleInstanceEpoch;
}

void SwarmTelemetryRegistry::updateRetirementTimer()
{
    const int interval = qBound(50, m_heartbeatMaximumAgeMs / 2, 1000);
    m_retirementTimer.start(interval);
}
