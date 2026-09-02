#include "ExactLinkTransmitter.h"

#include <mavlink_helpers.h>

#include <cstring>
#include <utility>

ExactLinkTransmitter::ExactLinkTransmitter(
    FrameWriter frameWriter, QObject *parent)
    : QObject(parent)
    , m_frameWriter(std::move(frameWriter))
{
}

ExactLinkTransmitter::SendResult ExactLinkTransmitter::sendMessage(
    int linkId, quint8 localSystemId, quint8 localComponentId,
    mavlink_message_t message)
{
    if (linkId < 0) {
        return SendResult::InvalidLink;
    }
    if (!m_frameWriter) {
        return SendResult::TransportUnavailable;
    }

    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(message.msgid);
    if (!entry) {
        return SendResult::InvalidMessage;
    }
    const auto existingStatus = m_transmitStates.constFind(linkId);
    const bool mavlink1 = existingStatus != m_transmitStates.constEnd()
        && (existingStatus->flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1);
    if (mavlink1 && message.msgid > 255U) {
        // mavlink_finalize_message_buffer() truncates a v2-only message ID in
        // a v1 header.  Reject it without consuming the link sequence.
        return SendResult::IncompatibleVersion;
    }

    // Generated *_encode() helpers finalize once through MAVLINK_COMM_0.
    // For a trimmed MAVLink 2 payload they place the checksum immediately at
    // payload[len].  A second finalization could then mistake that checksum
    // byte for a non-zero trailing field and put it on the wire as payload
    // (notably the NUL terminator of a 15-character parameter name).  Restore
    // the protocol-defined zero tail before assigning this link's sequence.
    if (message.len < entry->max_msg_len) {
        std::memset(
            &_MAV_PAYLOAD_NON_CONST(&message)[message.len], 0,
            static_cast<size_t>(entry->max_msg_len - message.len));
    }

    mavlink_status_t &status = transmitStatus(linkId);
    mavlink_finalize_message_buffer(
        &message, localSystemId, localComponentId, &status,
        entry->min_msg_len, entry->max_msg_len, entry->crc_extra);

    quint8 buffer[MAVLINK_MAX_PACKET_LEN]{};
    const quint16 frameLength =
        mavlink_msg_to_send_buffer(buffer, &message);
    if (!m_frameWriter(
            linkId,
            QByteArray(reinterpret_cast<const char *>(buffer), frameLength))) {
        return SendResult::TransportUnavailable;
    }
    return SendResult::Sent;
}

void ExactLinkTransmitter::setOutboundVersion(
    int linkId, unsigned int version)
{
    if (linkId < 0 || (version != 1U && version != 2U)) {
        return;
    }
    mavlink_status_t &status = transmitStatus(linkId);
    if (version == 1U) {
        status.flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    } else {
        status.flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    }
}

void ExactLinkTransmitter::forgetLink(int linkId)
{
    m_transmitStates.remove(linkId);
}

mavlink_status_t &ExactLinkTransmitter::transmitStatus(int linkId)
{
    auto status = m_transmitStates.find(linkId);
    if (status == m_transmitStates.end()) {
        mavlink_status_t initial{};
        status = m_transmitStates.insert(linkId, initial);
    }
    return status.value();
}
