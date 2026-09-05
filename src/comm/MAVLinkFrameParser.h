#ifndef MAVLINKFRAMEPARSER_H
#define MAVLINKFRAMEPARSER_H

#include <QByteArray>
#include <mavlink.h>
#include <QtGlobal>

class MAVLinkFrameParser final
{
public:
    MAVLinkFrameParser();

    unsigned int parseByte(quint8 byte, mavlink_message_t *message);
    // Exact received bytes of the most recently completed framing result.
    // Cleared when the next MAVLink STX begins, never populated by a partial
    // candidate, and bounded by MAVLINK_MAX_PACKET_LEN.
    QByteArray lastFrame() const;
    mavlink_status_t &status();
    const mavlink_status_t &status() const;
    void setOutboundVersion(unsigned int version);

private:
    void beginFrameCapture(quint8 byte);
    void appendFrameByte(quint8 byte);
    void finishFrameCapture(unsigned int framing);

    mavlink_message_t m_receiveBuffer{};
    mavlink_status_t m_status{};
    QByteArray m_frameBytes;
    QByteArray m_lastFrame;
    bool m_capturingFrame = false;
    bool m_captureOverflow = false;
};

#endif
