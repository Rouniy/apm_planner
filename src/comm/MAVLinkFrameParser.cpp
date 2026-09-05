#include "MAVLinkFrameParser.h"

#include <mavlink_helpers.h>

MAVLinkFrameParser::MAVLinkFrameParser() = default;

unsigned int MAVLinkFrameParser::parseByte(
    quint8 byte, mavlink_message_t *message)
{
    if (!message) {
        return MAVLINK_FRAMING_INCOMPLETE;
    }
    // The vendored mavgen helper overwrites BAD_CRC with the result of its
    // signature check at the end of a signed frame (and a null signing key
    // makes that check succeed). Never let a signature rehabilitate corrupt
    // wire bytes, or let such a packet advance the native replay/timestamp
    // state. Finish consuming the signature, preserving framing recovery.
    if (m_status.parse_state == MAVLINK_PARSE_STATE_SIGNATURE_WAIT
        && m_status.signature_wait == 1
        && m_receiveBuffer.checksum
            != (quint16(m_receiveBuffer.ck[0]) | (quint16(m_receiveBuffer.ck[1]) << 8))) {
        m_receiveBuffer.signature[MAVLINK_SIGNATURE_BLOCK_LEN - 1] = byte;
        *message = m_receiveBuffer;
        message->checksum = quint16(message->ck[0]) | (quint16(message->ck[1]) << 8);
        m_status.signature_wait = 0;
        m_status.parse_state = MAVLINK_PARSE_STATE_IDLE;
        m_status.msg_received = MAVLINK_FRAMING_BAD_CRC;
        m_status.parse_error = 0;
        return MAVLINK_FRAMING_BAD_CRC;
    }
    mavlink_status_t decodedStatus{};
    return mavlink_frame_char_buffer(
        &m_receiveBuffer, &m_status, byte, message, &decodedStatus);
}

mavlink_status_t &MAVLinkFrameParser::status()
{
    return m_status;
}

const mavlink_status_t &MAVLinkFrameParser::status() const
{
    return m_status;
}

void MAVLinkFrameParser::setOutboundVersion(unsigned int version)
{
    if (version > 1) {
        m_status.flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    } else {
        m_status.flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    }
}
