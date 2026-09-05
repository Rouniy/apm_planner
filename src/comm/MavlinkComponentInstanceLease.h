#ifndef MAVLINKCOMPONENTINSTANCELEASE_H
#define MAVLINKCOMPONENTINSTANCELEASE_H

#include "VehicleEndpoint.h"

#include <QMetaType>
#include <QtGlobal>

/**
 * Immutable identity of one MAVLink component within one physical-link
 * session.
 *
 * The instance epoch belongs to the component registry which issued the
 * lease.  It prevents an endpoint that disappears and is later rediscovered
 * with the same link/system/component tuple from inheriting an older
 * operation.  This is deliberately separate from
 * SwarmVehicleInstanceLease: peripherals do not become command-capable swarm
 * vehicles merely because they implement the parameter protocol.
 */
struct MavlinkComponentInstanceLease
{
    VehicleEndpoint endpoint;
    quint64 linkSessionEpoch = 0;
    quint64 instanceEpoch = 0;

    bool isValid() const noexcept
    {
        return endpoint.isValid() && linkSessionEpoch != 0
            && instanceEpoch != 0;
    }

    bool sameInstance(
        const MavlinkComponentInstanceLease &other) const noexcept
    {
        return endpoint.sameIdentity(other.endpoint)
            && linkSessionEpoch == other.linkSessionEpoch
            && instanceEpoch == other.instanceEpoch;
    }
};

inline bool operator==(const MavlinkComponentInstanceLease &left,
                       const MavlinkComponentInstanceLease &right) noexcept
{
    return left.sameInstance(right);
}

inline bool operator!=(const MavlinkComponentInstanceLease &left,
                       const MavlinkComponentInstanceLease &right) noexcept
{
    return !(left == right);
}

Q_DECLARE_METATYPE(MavlinkComponentInstanceLease)
Q_DECLARE_TYPEINFO(MavlinkComponentInstanceLease, Q_MOVABLE_TYPE);

#endif // MAVLINKCOMPONENTINSTANCELEASE_H
