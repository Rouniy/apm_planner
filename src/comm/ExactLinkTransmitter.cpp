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
    if (!mavlink1 && message.magic == MAVLINK_STX_MAVLINK1
        && entry->max_msg_len > entry->min_msg_len) {
        // A generated extension-bearing message finalized as MAVLink 1 has
        // already overwritten the first extension bytes with its checksum.
        // Reconstructing it would silently invent field values. Require a
        // headerless/typed sender instead of emitting a corrupted MAVLink 2
        // frame (sendCommandAck() is the typed path used by CompassMot).
        return SendResult::IncompatibleVersion;
    }
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

ExactLinkTransmitter::SendResult ExactLinkTransmitter::sendCommandAck(
    int linkId, quint8 localSystemId, quint8 localComponentId,
    quint16 command, quint8 result,
    quint8 targetSystem, quint8 targetComponent,
    quint8 progress, qint32 resultParam2)
{
    if ((targetSystem != 0 || targetComponent != 0)
        && !supportsTargetedCommandAck(linkId)) {
        return SendResult::IncompatibleVersion;
    }

    // Do not use generated *_pack/encode here: they finalize through global
    // MAVLINK_COMM_0, whose version belongs to whichever physical link most
    // recently mutated it. Build the schema payload headerless, then let this
    // transmitter finalize it against the destination link's own status.
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_COMMAND_ACK;
    message.len = MAVLINK_MSG_ID_COMMAND_ACK_LEN;
    char *const payload = _MAV_PAYLOAD_NON_CONST(&message);
    _mav_put_uint16_t(payload, 0, command);
    _mav_put_uint8_t(payload, 2, result);
    _mav_put_uint8_t(payload, 3, progress);
    _mav_put_int32_t(payload, 4, resultParam2);
    _mav_put_uint8_t(payload, 8, targetSystem);
    _mav_put_uint8_t(payload, 9, targetComponent);
    return sendMessage(linkId, localSystemId, localComponentId, message);
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

unsigned int ExactLinkTransmitter::outboundVersion(int linkId) const
{
    const auto status = m_transmitStates.constFind(linkId);
    if (status == m_transmitStates.constEnd()) {
        // MAVLink 2 is the application default until protocol negotiation
        // explicitly constrains a physical link to MAVLink 1.
        return 2U;
    }
    return (status->flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1) ? 1U : 2U;
}

bool ExactLinkTransmitter::supportsTargetedCommandAck(int linkId) const
{
    // COMMAND_ACK target_system/target_component are MAVLink 2 extension
    // fields. They improve observability but do not isolate CompassMot:
    // ArduCopter treats every received COMMAND_ACK as its stop signal.
    return linkId >= 0 && outboundVersion(linkId) == 2U;
}

void ExactLinkTransmitter::setMotorStopLinkEligible(
    int linkId, bool eligible)
{
    if (linkId < 0) {
        return;
    }
    m_motorStopLinkEligibility.insert(linkId, eligible);
}

bool ExactLinkTransmitter::motorStopLinkEligible(int linkId) const
{
    return linkId >= 0
        && m_motorStopLinkEligibility.value(linkId, false);
}

void ExactLinkTransmitter::forgetLink(int linkId)
{
    m_transmitStates.remove(linkId);
    m_motorStopLinkEligibility.remove(linkId);
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
