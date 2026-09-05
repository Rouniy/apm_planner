#include "MAVLinkFrameParser.h"

#include <mavlink_helpers.h>

MAVLinkFrameParser::MAVLinkFrameParser()
{
    m_frameBytes.reserve(MAVLINK_MAX_PACKET_LEN);
    m_lastFrame.reserve(MAVLINK_MAX_PACKET_LEN);
}

unsigned int MAVLinkFrameParser::parseByte(
    quint8 byte, mavlink_message_t *message)
{
    if (!message) {
        return MAVLINK_FRAMING_INCOMPLETE;
    }

    const bool parserIdle = m_status.parse_state == MAVLINK_PARSE_STATE_UNINIT
        || m_status.parse_state == MAVLINK_PARSE_STATE_IDLE;
    if (parserIdle) {
        if (byte == MAVLINK_STX || byte == MAVLINK_STX_MAVLINK1) {
            beginFrameCapture(byte);
        }
    } else {
        appendFrameByte(byte);
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
        finishFrameCapture(MAVLINK_FRAMING_BAD_CRC);
        return MAVLINK_FRAMING_BAD_CRC;
    }
    mavlink_status_t decodedStatus{};
    const unsigned int framing = mavlink_frame_char_buffer(
        &m_receiveBuffer, &m_status, byte, message, &decodedStatus);
    if (m_status.parse_state == MAVLINK_PARSE_STATE_IDLE) {
        finishFrameCapture(framing);
    }
    return framing;
}

QByteArray MAVLinkFrameParser::lastFrame() const
{
    return m_lastFrame;
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

void MAVLinkFrameParser::beginFrameCapture(quint8 byte)
{
    m_lastFrame.clear();
    m_frameBytes.clear();
    m_capturingFrame = true;
    m_captureOverflow = false;
    appendFrameByte(byte);
}

void MAVLinkFrameParser::appendFrameByte(quint8 byte)
{
    if (!m_capturingFrame || m_captureOverflow) {
        return;
    }
    if (m_frameBytes.size() >= MAVLINK_MAX_PACKET_LEN) {
        m_frameBytes.clear();
        m_captureOverflow = true;
        return;
    }
    m_frameBytes.append(static_cast<char>(byte));
}

void MAVLinkFrameParser::finishFrameCapture(unsigned int framing)
{
    const bool complete = framing == MAVLINK_FRAMING_OK
        || framing == MAVLINK_FRAMING_BAD_CRC
        || framing == MAVLINK_FRAMING_BAD_SIGNATURE;
    if (complete && m_capturingFrame && !m_captureOverflow) {
        m_lastFrame = m_frameBytes;
    }
    m_frameBytes.clear();
    m_capturingFrame = false;
    m_captureOverflow = false;
}
