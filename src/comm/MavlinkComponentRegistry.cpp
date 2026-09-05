#include "MavlinkComponentRegistry.h"
#include "Px4FlowFrameAssembler.h"
#include <QPointer>
#include <algorithm>
#include <limits>

MavlinkComponentRegistry::MavlinkComponentRegistry(QObject *parent, Clock clock)
    : QObject(parent), m_clock(std::move(clock))
{
    m_elapsed.start();
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &MavlinkComponentRegistry::expireStale);
    m_timer.start();
}
qint64 MavlinkComponentRegistry::now() const
{ return m_clock ? m_clock() : m_elapsed.elapsed(); }

void MavlinkComponentRegistry::beginLinkSession(int linkId, quint64 epoch)
{
    if (linkId < 0 || epoch == 0 || m_sessions.value(linkId) == epoch) return;
    QPointer<MavlinkComponentRegistry> guard(this);
    const quint64 previous = m_sessions.value(linkId);
    const quint64 expectedRevision = m_sessionRevisions.value(linkId) + (previous != 0 ? 1 : 0);
    endLinkSession(linkId, previous);
    // A retirement listener can synchronously install (and even end) a new
    // session. That newer lifecycle decision wins over this suspended begin.
    if (!guard || m_sessionRevisions.value(linkId) != expectedRevision) return;
    ++m_sessionRevisions[linkId];
    m_sessions.insert(linkId, epoch);
}
void MavlinkComponentRegistry::endLinkSession(int linkId, quint64 epoch)
{
    if (epoch == 0 || m_sessions.value(linkId) != epoch) return;
    m_sessions.remove(linkId);
    ++m_sessionRevisions[linkId];
    QList<MavlinkComponentInstanceLease> retired;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it.key().linkId == linkId) {
            retired.append(it->lease);
            it = m_entries.erase(it);
        } else ++it;
    }
    QPointer<MavlinkComponentRegistry> guard(this);
    for (const auto &lease : retired) {
        emit componentRetired(lease);
        if (!guard) return;
    }
    if (!retired.isEmpty()) emit componentsChanged();
}
void MavlinkComponentRegistry::expireStale()
{
    const qint64 stamp = now();
    QList<MavlinkComponentInstanceLease> retired;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (stamp < it->lastSeenMs || stamp - it->lastSeenMs >= StaleAfterMs) {
            retired.append(it->lease);
            it = m_entries.erase(it);
        } else ++it;
    }
    QPointer<MavlinkComponentRegistry> guard(this);
    for (const auto &lease : retired) {
        emit componentRetired(lease);
        if (!guard) return;
    }
    if (!retired.isEmpty()) emit componentsChanged();
}
void MavlinkComponentRegistry::observeMessage(
    int linkId, quint64 epoch, const mavlink_message_t &message)
{
    if (epoch == 0 || m_sessions.value(linkId) != epoch
        || message.sysid == 0 || message.compid == 0
        || message.compid == MAV_COMP_ID_MISSIONPLANNER) return;
    VehicleEndpoint endpoint;
    endpoint.linkId = linkId;
    endpoint.systemId = message.sysid;
    endpoint.componentId = message.compid;
    QPointer<MavlinkComponentRegistry> guard(this);
    expireStale();
    if (!guard || m_sessions.value(linkId) != epoch) return;
    mavlink_heartbeat_t heartbeat{};
    const bool isHeartbeat = message.msgid == MAVLINK_MSG_ID_HEARTBEAT;
    if (isHeartbeat) mavlink_msg_heartbeat_decode(&message, &heartbeat);
    auto it = m_entries.find(endpoint);
    if (it != m_entries.end() && isHeartbeat
        && (heartbeat.type == MAV_TYPE_GCS
            || (it->heartbeatKnown && (it->type != heartbeat.type
                || it->autopilot != heartbeat.autopilot)))) {
        const auto retired = it->lease;
        m_entries.erase(it);
        emit componentRetired(retired);
        if (!guard || m_sessions.value(linkId) != epoch) return;
        emit componentsChanged();
        if (!guard || m_sessions.value(linkId) != epoch) return;
        // A callback-created replacement must not be overwritten by this
        // suspended packet, including a second heartbeat identity change.
        if (m_entries.contains(endpoint)) return;
        it = m_entries.end();
    }
    if (it != m_entries.end()) {
        // PX4Flow VIDEO_ONLY firmware can suspend its normal heartbeat loop.
        // Incoming sensor traffic still proves presence in this physical epoch.
        it->lastSeenMs = now();
        if (isHeartbeat) {
            it->heartbeatKnown = true;
            it->type = heartbeat.type;
            it->autopilot = heartbeat.autopilot;
        }
        return;
    }
    bool admitted = false;
    if (isHeartbeat) {
        admitted = heartbeat.type != MAV_TYPE_GCS;
    } else if (message.msgid == MAVLINK_MSG_ID_DATA_TRANSMISSION_HANDSHAKE) {
        mavlink_data_transmission_handshake_t data{};
        mavlink_msg_data_transmission_handshake_decode(&message, &data);
        Px4FlowFrameAssembler assembler;
        admitted = assembler.begin({data.type, data.size, data.width, data.height,
            data.packets, data.payload, data.jpg_quality})
            == Px4FlowFrameAssembler::BeginResult::Started;
    }
    if (!admitted || m_entries.size() >= MaximumComponents
        || m_nextInstance == std::numeric_limits<quint64>::max()) return;
    Entry entry;
    entry.lease = {endpoint, epoch, ++m_nextInstance};
    entry.lastSeenMs = now();
    entry.heartbeatKnown = isHeartbeat;
    entry.type = heartbeat.type;
    entry.autopilot = heartbeat.autopilot;
    m_entries.insert(endpoint, entry);
    emit componentsChanged();
}
QList<MavlinkComponentInstanceLease> MavlinkComponentRegistry::components() const
{
    QList<MavlinkComponentInstanceLease> result;
    for (const auto &entry : m_entries)
        if (validateLease(entry.lease)) result.append(entry.lease);
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return a.endpoint < b.endpoint;
    });
    return result;
}
bool MavlinkComponentRegistry::validateLease(const MavlinkComponentInstanceLease &lease) const
{
    const auto it = m_entries.constFind(lease.endpoint);
    const qint64 stamp = now();
    return lease.isValid() && m_sessions.value(lease.endpoint.linkId) == lease.linkSessionEpoch
        && it != m_entries.cend() && it->lease.sameInstance(lease)
        && stamp >= it->lastSeenMs && stamp - it->lastSeenMs < StaleAfterMs;
}
