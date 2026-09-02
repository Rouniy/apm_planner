#ifndef GPSRTCMPACKETIZER_H
#define GPSRTCMPACKETIZER_H

#include <QByteArray>
#include <QList>
#include <QtGlobal>

struct GpsRtcmPacket
{
    quint8 flags = 0;
    QByteArray data;
};

class GpsRtcmPacketizer final
{
public:
    static constexpr int FragmentLength = 180;
    static constexpr int MaximumFragments = 4;
    static constexpr int MaximumReassembledLength =
        FragmentLength * MaximumFragments;

    struct Result
    {
        QList<GpsRtcmPacket> packets;
        quint8 nextSequenceId = 0;
    };

    // GPS_RTCM_DATA flags: bit 0 means fragmented, bits 1-2 are the
    // fragment ID, and bits 3-7 are the five-bit sequence ID.
    static Result pack(const QByteArray &data, quint8 sequenceId);

private:
    static quint8 flags(bool fragmented, quint8 fragmentId,
                        quint8 sequenceId);
};

#endif // GPSRTCMPACKETIZER_H
