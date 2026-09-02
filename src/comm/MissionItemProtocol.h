#ifndef MISSIONITEMPROTOCOL_H
#define MISSIONITEMPROTOCOL_H

#include "QGCMAVLink.h"

#include <algorithm>
#include <array>

namespace MissionItemProtocol
{
inline bool commandHasLocation(quint16 command)
{
    // MAVLink's generated C enum does not retain command metadata. Keep this
    // explicit list shared by the wire codec and planner UI so PARAM5/6 are
    // never scaled differently in the two layers.
    // 188 and 43003 are defined by the newer dialect used by Mission Planner
    // 10 and intentionally retained even though the bundled C dialect is
    // older. MAV_CMD_FIXED_MAG_CAL_YAW (42006) is not listed: its latitude
    // and longitude are param3/param4, not MISSION_ITEM x/y (param5/param6).
    static constexpr std::array<quint16, 41> commands{{
        16, 17, 18, 19, 21, 22, 23, 24, 25, 31,
        80, 81, 82, 84, 85, 94, 179, 188, 189, 192,
        195, 201, 4001, 5000, 5001, 5002, 5003, 5004, 5100,
        30001, 31000, 31001, 31002, 31003, 31004, 31005, 31006,
        31007, 31008, 31009, 43003,
    }};
    return std::find(commands.cbegin(), commands.cend(), command)
            != commands.cend();
}

inline bool frameHasGlobalLocation(quint8 frame)
{
    return frame == MAV_FRAME_GLOBAL
            || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT
            || frame == MAV_FRAME_GLOBAL_TERRAIN_ALT
            || frame == MAV_FRAME_GLOBAL_INT
            || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT_INT
            || frame == MAV_FRAME_GLOBAL_TERRAIN_ALT_INT;
}

inline double coordinateScale(quint8 frame, quint16 command)
{
    if (frame == MAV_FRAME_MISSION || !commandHasLocation(command)) {
        return 1.0;
    }
    return frameHasGlobalLocation(frame) ? 1.0e7 : 1.0e4;
}
}

#endif // MISSIONITEMPROTOCOL_H
