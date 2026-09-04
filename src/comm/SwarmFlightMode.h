#ifndef SWARMFLIGHTMODE_H
#define SWARMFLIGHTMODE_H

#include "SwarmTelemetryRegistry.h"

#include <QString>

namespace SwarmFlightMode
{

inline bool isCopter(int vehicleType) noexcept
{
    switch (vehicleType) {
    case MAV_TYPE_TRICOPTER:
    case MAV_TYPE_QUADROTOR:
    case MAV_TYPE_COAXIAL:
    case MAV_TYPE_HELICOPTER:
    case MAV_TYPE_HEXAROTOR:
    case MAV_TYPE_OCTOROTOR:
    case MAV_TYPE_DODECAROTOR:
    case MAV_TYPE_DECAROTOR:
        return true;
    default:
        return false;
    }
}

inline bool isPlane(int vehicleType) noexcept
{
    switch (vehicleType) {
    case MAV_TYPE_FIXED_WING:
    case MAV_TYPE_VTOL_DUOROTOR:
    case MAV_TYPE_VTOL_QUADROTOR:
    case MAV_TYPE_VTOL_TILTROTOR:
    case MAV_TYPE_VTOL_RESERVED2:
    case MAV_TYPE_VTOL_RESERVED3:
    case MAV_TYPE_VTOL_RESERVED4:
    case MAV_TYPE_VTOL_RESERVED5:
        return true;
    default:
        return false;
    }
}

inline bool isRover(int vehicleType) noexcept
{
    return vehicleType == MAV_TYPE_GROUND_ROVER
        || vehicleType == MAV_TYPE_SURFACE_BOAT;
}

/** ArduPilot base-mode bits are ambiguous; only the family custom mode is authoritative. */
inline bool isExactGuided(const SwarmTelemetrySnapshot &snapshot) noexcept
{
    if (!snapshot.heartbeatValid
        || snapshot.autopilot != MAV_AUTOPILOT_ARDUPILOTMEGA
        || (snapshot.baseMode & MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) == 0) {
        return false;
    }
    if (isCopter(snapshot.vehicleType)) {
        return snapshot.customMode == 4;
    }
    if (isPlane(snapshot.vehicleType) || isRover(snapshot.vehicleType)) {
        return snapshot.customMode == 15;
    }
    return false;
}

inline QString displayName(const SwarmTelemetrySnapshot &snapshot)
{
    if (snapshot.autopilot != MAV_AUTOPILOT_ARDUPILOTMEGA) {
        return QStringLiteral("Mode %1").arg(snapshot.customMode);
    }
    if (isCopter(snapshot.vehicleType)) {
        switch (snapshot.customMode) {
        case 0: return QStringLiteral("Stabilize");
        case 1: return QStringLiteral("Acro");
        case 2: return QStringLiteral("Alt Hold");
        case 3: return QStringLiteral("Auto");
        case 4: return QStringLiteral("Guided");
        case 5: return QStringLiteral("Loiter");
        case 6: return QStringLiteral("RTL");
        case 7: return QStringLiteral("Circle");
        case 9: return QStringLiteral("Land");
        case 16: return QStringLiteral("Pos Hold");
        case 17: return QStringLiteral("Brake");
        case 20: return QStringLiteral("Guided no GPS");
        case 21: return QStringLiteral("Smart RTL");
        case 23: return QStringLiteral("Follow");
        default: break;
        }
    } else if (isPlane(snapshot.vehicleType)) {
        switch (snapshot.customMode) {
        case 0: return QStringLiteral("Manual");
        case 10: return QStringLiteral("Auto");
        case 11: return QStringLiteral("RTL");
        case 12: return QStringLiteral("Loiter");
        case 13: return QStringLiteral("Takeoff");
        case 15: return QStringLiteral("Guided");
        case 19: return QStringLiteral("Q-Loiter");
        case 21: return QStringLiteral("Q-RTL");
        default: break;
        }
    } else if (isRover(snapshot.vehicleType)) {
        switch (snapshot.customMode) {
        case 0: return QStringLiteral("Manual");
        case 4: return QStringLiteral("Hold");
        case 5: return QStringLiteral("Loiter");
        case 6: return QStringLiteral("Follow");
        case 10: return QStringLiteral("Auto");
        case 11: return QStringLiteral("RTL");
        case 12: return QStringLiteral("Smart RTL");
        case 15: return QStringLiteral("Guided");
        default: break;
        }
    }
    return QStringLiteral("Mode %1").arg(snapshot.customMode);
}

} // namespace SwarmFlightMode

#endif // SWARMFLIGHTMODE_H
