#include "FlightModeTelemetryMonitor.h"

#include "comm/VehicleTargetManager.h"

#include <limits>

FlightModeTelemetryMonitor::FlightModeTelemetryMonitor(
    VehicleTargetManager *targets, const VehicleTargetLease &lease,
    QObject *parent)
    : QObject(parent),
      m_targets(targets),
      m_lease(lease)
{
    if (!m_targets || !m_lease.isValid()) {
        m_invalidated = true;
        return;
    }
    connect(m_targets, &VehicleTargetManager::targetGenerationChanged,
            this, [this](qulonglong) {
        if (!m_targets
            || !m_targets->isCurrentTarget(
                m_lease.endpoint.linkId,
                m_lease.endpoint.systemId,
                m_lease.endpoint.componentId,
                m_lease.generation)) {
            invalidate();
        }
    });
}

void FlightModeTelemetryMonitor::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (!accepts(linkId, message)) {
        return;
    }

    if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        mavlink_heartbeat_t heartbeat{};
        mavlink_msg_heartbeat_decode(&message, &heartbeat);
        m_heartbeatSeen = true;
        emit heartbeatObserved(
            heartbeat.custom_mode,
            (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0,
            heartbeat.autopilot,
            heartbeat.type);
        return;
    }
    if (!m_heartbeatSeen) {
        return;
    }

    if (message.msgid == MAVLINK_MSG_ID_RC_CHANNELS) {
        mavlink_rc_channels_t rc{};
        mavlink_msg_rc_channels_decode(&message, &rc);
        const quint16 channels[] = {
            rc.chan1_raw, rc.chan2_raw, rc.chan3_raw, rc.chan4_raw,
            rc.chan5_raw, rc.chan6_raw, rc.chan7_raw, rc.chan8_raw,
            rc.chan9_raw, rc.chan10_raw, rc.chan11_raw, rc.chan12_raw,
            rc.chan13_raw, rc.chan14_raw, rc.chan15_raw, rc.chan16_raw,
            rc.chan17_raw, rc.chan18_raw
        };
        emitChannels(channels, 18, 1);
    } else if (message.msgid == MAVLINK_MSG_ID_RC_CHANNELS_RAW) {
        mavlink_rc_channels_raw_t rc{};
        mavlink_msg_rc_channels_raw_decode(&message, &rc);
        const quint16 channels[] = {
            rc.chan1_raw, rc.chan2_raw, rc.chan3_raw, rc.chan4_raw,
            rc.chan5_raw, rc.chan6_raw, rc.chan7_raw, rc.chan8_raw
        };
        emitChannels(channels, 8, static_cast<int>(rc.port) * 8 + 1);
    }
}

bool FlightModeTelemetryMonitor::accepts(
    int linkId, const mavlink_message_t &message)
{
    if (m_invalidated || !m_targets || !m_lease.isValid()
        || linkId != m_lease.endpoint.linkId
        || message.sysid != m_lease.endpoint.systemId
        || message.compid != m_lease.endpoint.componentId) {
        return false;
    }
    if (!m_targets->isCurrentTarget(
            m_lease.endpoint.linkId,
            m_lease.endpoint.systemId,
            m_lease.endpoint.componentId,
            m_lease.generation)) {
        invalidate();
        return false;
    }
    return true;
}

void FlightModeTelemetryMonitor::invalidate()
{
    if (m_invalidated) {
        return;
    }
    m_invalidated = true;
    m_heartbeatSeen = false;
    emit targetInvalidated();
}

void FlightModeTelemetryMonitor::emitChannels(
    const quint16 *channels, int count, int firstChannelOneBased)
{
    for (int index = 0; index < count; ++index) {
        if (channels[index] != std::numeric_limits<quint16>::max()) {
            emit rcInputObserved(firstChannelOneBased + index,
                                 channels[index]);
        }
    }
}
