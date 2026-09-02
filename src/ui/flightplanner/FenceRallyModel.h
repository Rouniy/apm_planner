#ifndef FENCERALLYMODEL_H
#define FENCERALLYMODEL_H

#include "WpRow.h"

#include <QString>
#include <QVector>

namespace MissionPlanner
{

struct GeoCoordinate
{
    double Latitude = 0.0;
    double Longitude = 0.0;
    double Altitude = 0.0;
};

struct ModelValidationResult
{
    bool ok = false;
    QString error;
};

class FencePolygon
{
public:
    enum class PolyType
    {
        Inclusive,
        Exclusive
    };

    QVector<GeoCoordinate> Points;
    PolyType Mode = PolyType::Inclusive;
};

class FenceCircle
{
public:
    using PolyType = FencePolygon::PolyType;

    GeoCoordinate Center;
    double Radius = 0.0;
    PolyType Mode = PolyType::Inclusive;
};

class FenceReturn
{
public:
    GeoCoordinate Return;
    quint8 Frame = 3; // MAV_FRAME_GLOBAL_RELATIVE_ALT
};

class Fence
{
public:
    bool HasReturn = false;
    FenceReturn ReturnPoint;
    QVector<FencePolygon> Polygons;
    QVector<FenceCircle> Circles;

    ModelValidationResult validate() const;
    QVector<WpRowData> FenceToLocation(QString *error = nullptr) const;

    struct DecodeResult;
    static DecodeResult LocationToFence(const QVector<WpRowData> &rows);
};

struct Fence::DecodeResult
{
    bool ok = false;
    QString error;
    Fence fence;
};

class RallyPoint
{
public:
    GeoCoordinate Position;
    double BreakAltitude = 0.0;
    // Legacy MAVLink RALLY_POINT stores this value as uint16 centidegrees.
    double LandHeading = 0.0;
    quint8 Flags = 0;
    quint8 Frame = 3; // MAV_FRAME_GLOBAL_RELATIVE_ALT
};

class RallyPoints
{
public:
    QVector<RallyPoint> Points;

    ModelValidationResult validate() const;
    QVector<WpRowData> RallyToLocation(QString *error = nullptr) const;

    struct DecodeResult;
    static DecodeResult LocationToRally(const QVector<WpRowData> &rows);
};

struct RallyPoints::DecodeResult
{
    bool ok = false;
    QString error;
    RallyPoints rally;
};

} // namespace MissionPlanner

#endif // FENCERALLYMODEL_H
