#ifndef VEHICLEENDPOINT_H
#define VEHICLEENDPOINT_H

#include <QHashFunctions>
#include <QMetaType>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

/**
 * Exact address of one MAVLink component on one physical transport.
 *
 * A system/component pair is not globally unique: two links can carry the
 * same ids.  Keeping linkId in the identity is therefore mandatory for every
 * future command, parameter and mission transaction.
 */
struct VehicleEndpoint
{
    Q_GADGET
    Q_PROPERTY(int linkId MEMBER linkId)
    Q_PROPERTY(int systemId MEMBER systemId)
    Q_PROPERTY(int componentId MEMBER componentId)
    Q_PROPERTY(QString linkName MEMBER linkName)
    Q_PROPERTY(QString componentName MEMBER componentName)

public:
    int linkId = -1;
    int systemId = 0;
    int componentId = 0;
    QString linkName;
    QString componentName;

    bool isValid() const noexcept;
    bool sameIdentity(const VehicleEndpoint &other) const noexcept;
    bool metadataEquals(const VehicleEndpoint &other) const noexcept;
    QString displayName() const;
    QVariantMap toVariantMap() const;

    static QString defaultComponentName(int componentId);
};

inline bool operator==(const VehicleEndpoint &left,
                       const VehicleEndpoint &right) noexcept
{
    return left.sameIdentity(right);
}

inline uint qHash(const VehicleEndpoint &endpoint, uint seed = 0) noexcept
{
    seed = ::qHash(endpoint.linkId, seed);
    seed = ::qHash(endpoint.systemId, seed);
    return ::qHash(endpoint.componentId, seed);
}

inline bool operator!=(const VehicleEndpoint &left,
                       const VehicleEndpoint &right) noexcept
{
    return !(left == right);
}

inline bool operator<(const VehicleEndpoint &left,
                      const VehicleEndpoint &right) noexcept
{
    if (left.linkId != right.linkId) {
        return left.linkId < right.linkId;
    }
    if (left.systemId != right.systemId) {
        return left.systemId < right.systemId;
    }
    return left.componentId < right.componentId;
}

/** Immutable snapshot used by asynchronous target-aware operations. */
struct VehicleTargetLease
{
    VehicleEndpoint endpoint;
    quint64 generation = 0;

    bool isValid() const noexcept { return endpoint.isValid(); }
};

Q_DECLARE_METATYPE(VehicleEndpoint)
Q_DECLARE_METATYPE(VehicleTargetLease)
Q_DECLARE_TYPEINFO(VehicleEndpoint, Q_MOVABLE_TYPE);
Q_DECLARE_TYPEINFO(VehicleTargetLease, Q_MOVABLE_TYPE);

#endif // VEHICLEENDPOINT_H
