#ifndef DRONECANFRAMECODEC_H
#define DRONECANFRAMECODEC_H

#include <QByteArray>
#include <QtGlobal>

struct DroneCanNodeStatusFrame
{
    int nodeId = -1;
    quint32 uptimeSeconds = 0;
    quint8 health = 0;
    quint8 mode = 0;
    quint8 subMode = 0;
    quint16 vendorSpecificStatusCode = 0;
    quint8 transferId = 0;
};

class DroneCanFrameCodec final
{
public:
    static constexpr quint16 NodeStatusDataTypeId = 341;
    static constexpr int NodeOfflineTimeoutMs = 3000;
    static constexpr quint32 ExtendedFrameFlag = 0x80000000U;
    static constexpr quint32 ExtendedIdMask = 0x1FFFFFFFU;

    static bool decodeNodeStatus(quint32 mavlinkCanId,
                                 const QByteArray &frameData,
                                 DroneCanNodeStatusFrame *status);
};

#endif // DRONECANFRAMECODEC_H
