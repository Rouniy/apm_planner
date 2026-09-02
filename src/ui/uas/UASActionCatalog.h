#ifndef UASACTIONCATALOG_H
#define UASACTIONCATALOG_H

#include "comm/QGCMAVLink.h"

#include <array>

namespace UASActionCatalog
{

struct Entry
{
    const char *name;
    MAV_CMD command;
};

inline constexpr std::array<Entry, 7> StandardActions{{
    {"Loiter Unlimited", MAV_CMD_NAV_LOITER_UNLIM},
    {"Return To Launch", MAV_CMD_NAV_RETURN_TO_LAUNCH},
    {"Preflight Calibration", MAV_CMD_PREFLIGHT_CALIBRATION},
    {"Mission Start", MAV_CMD_MISSION_START},
    {"Preflight Reboot", MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN},
    {"Trigger Camera", MAV_CMD_DO_DIGICAM_CONTROL},
    {"Format_SD_Card", MAV_CMD_STORAGE_FORMAT},
}};

struct CommandRequest
{
    MAV_CMD command;
    int confirmation;
    std::array<float, 7> parameters;
    int component;
};

inline constexpr CommandRequest formatSdCardRequest()
{
    // Mission Planner targets the first storage device and requests format.
    return {MAV_CMD_STORAGE_FORMAT, 1,
            {{1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}},
            MAV_COMP_ID_PRIMARY};
}

} // namespace UASActionCatalog

#endif // UASACTIONCATALOG_H
