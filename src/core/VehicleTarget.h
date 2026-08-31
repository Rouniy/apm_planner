#ifndef VEHICLETARGET_H
#define VEHICLETARGET_H

#include <QtGlobal>

struct VehicleTarget
{
    int linkId = -1;
    quint8 systemId = 0;
    quint8 componentId = 0;
    quint64 revision = 0;

    bool isValid() const
    {
        return linkId >= 0 && systemId != 0;
    }

    bool sameEndpoint(const VehicleTarget &other) const
    {
        return linkId == other.linkId
            && systemId == other.systemId
            && componentId == other.componentId;
    }
};

inline bool operator==(const VehicleTarget &left, const VehicleTarget &right)
{
    return left.sameEndpoint(right) && left.revision == right.revision;
}

inline bool operator!=(const VehicleTarget &left, const VehicleTarget &right)
{
    return !(left == right);
}

#endif
