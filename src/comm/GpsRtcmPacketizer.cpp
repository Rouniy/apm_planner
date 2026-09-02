#include "GpsRtcmPacketizer.h"

#include <algorithm>

quint8 GpsRtcmPacketizer::flags(
    bool fragmented, quint8 fragmentId, quint8 sequenceId)
{
    quint8 result = static_cast<quint8>((sequenceId & 0x1fU) << 3);
    if (fragmented) {
        result |= 0x01U;
        result |= static_cast<quint8>((fragmentId & 0x03U) << 1);
    }
    return result;
}

GpsRtcmPacketizer::Result GpsRtcmPacketizer::pack(
    const QByteArray &data, quint8 sequenceId)
{
    Result result;
    result.nextSequenceId = static_cast<quint8>(sequenceId & 0x1fU);
    if (data.isEmpty()) {
        return result;
    }

    if (data.size() > MaximumReassembledLength) {
        for (int offset = 0; offset < data.size();
             offset += FragmentLength) {
            const int length = std::min(FragmentLength,
                                        data.size() - offset);
            result.packets.append({
                flags(false, 0, result.nextSequenceId),
                data.mid(offset, length)
            });
            result.nextSequenceId = static_cast<quint8>(
                (result.nextSequenceId + 1U) & 0x1fU);
        }
        return result;
    }

    if (data.size() <= FragmentLength) {
        result.packets.append({flags(false, 0, result.nextSequenceId),
                               data});
        result.nextSequenceId = static_cast<quint8>(
            (result.nextSequenceId + 1U) & 0x1fU);
        return result;
    }

    quint8 fragmentId = 0;
    for (int offset = 0; offset < data.size();
         offset += FragmentLength, ++fragmentId) {
        const int length = std::min(FragmentLength, data.size() - offset);
        result.packets.append({
            flags(true, fragmentId, result.nextSequenceId),
            data.mid(offset, length)
        });
    }

    // With fewer than four full fragments, a zero-length fragment terminates
    // an exact multiple of 180 bytes for ArduPilot/PX4 reassembly.
    if ((data.size() % FragmentLength) == 0
        && fragmentId < MaximumFragments) {
        result.packets.append({
            flags(true, fragmentId, result.nextSequenceId), QByteArray()
        });
    }
    result.nextSequenceId = static_cast<quint8>(
        (result.nextSequenceId + 1U) & 0x1fU);
    return result;
}
