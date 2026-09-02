#ifndef MAVLINKFRAMEPARSER_H
#define MAVLINKFRAMEPARSER_H

#include <mavlink.h>
#include <QtGlobal>

class MAVLinkFrameParser final
{
public:
    MAVLinkFrameParser();

    unsigned int parseByte(quint8 byte, mavlink_message_t *message);
    mavlink_status_t &status();
    const mavlink_status_t &status() const;
    void setOutboundVersion(unsigned int version);

private:
    mavlink_message_t m_receiveBuffer{};
    mavlink_status_t m_status{};
};

#endif
