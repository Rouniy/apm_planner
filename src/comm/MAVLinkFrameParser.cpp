#include "MAVLinkFrameParser.h"

#include <mavlink_helpers.h>

MAVLinkFrameParser::MAVLinkFrameParser() = default;

unsigned int MAVLinkFrameParser::parseByte(
    quint8 byte, mavlink_message_t *message)
{
    if (!message) {
        return MAVLINK_FRAMING_INCOMPLETE;
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
