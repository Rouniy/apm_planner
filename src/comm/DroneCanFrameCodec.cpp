#include "DroneCanFrameCodec.h"

bool DroneCanFrameCodec::decodeNodeStatus(
    quint32 mavlinkCanId, const QByteArray &frameData,
    DroneCanNodeStatusFrame *status)
{
    if (!status || !(mavlinkCanId & ExtendedFrameFlag)
        || frameData.size() != 8) {
        return false;
    }

    const quint32 canId = mavlinkCanId & ExtendedIdMask;
    const bool serviceNotMessage = (canId & 0x80U) != 0;
    const int sourceNodeId = int(canId & 0x7FU);
    const quint16 dataTypeId = quint16((canId >> 8U) & 0xFFFFU);
    if (serviceNotMessage || sourceNodeId == 0
        || dataTypeId != NodeStatusDataTypeId) {
        return false;
    }

    const quint8 tail = quint8(frameData.at(frameData.size() - 1));
    const bool startOfTransfer = (tail & 0x80U) != 0;
    const bool endOfTransfer = (tail & 0x40U) != 0;
    const bool toggle = (tail & 0x20U) != 0;
    if (!startOfTransfer || !endOfTransfer || toggle) {
        return false;
    }

    const auto byte = [&frameData](int index) {
        return quint8(frameData.at(index));
    };
    DroneCanNodeStatusFrame decoded;
    decoded.nodeId = sourceNodeId;
    decoded.uptimeSeconds = quint32(byte(0))
        | (quint32(byte(1)) << 8U)
        | (quint32(byte(2)) << 16U)
        | (quint32(byte(3)) << 24U);
    // DSDL scalar fields are serialized most-significant bit first inside
    // this packed byte: uint2 health, uint3 mode, uint3 sub_mode.
    decoded.health = (byte(4) >> 6U) & 0x03U;
    decoded.mode = (byte(4) >> 3U) & 0x07U;
    decoded.subMode = byte(4) & 0x07U;
    decoded.vendorSpecificStatusCode = quint16(byte(5))
        | (quint16(byte(6)) << 8U);
    decoded.transferId = tail & 0x1FU;
    *status = decoded;
    return true;
}
