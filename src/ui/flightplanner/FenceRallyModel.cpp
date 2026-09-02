#include "FenceRallyModel.h"

#include "QGCMAVLink.h"

#include <QObject>

#include <algorithm>
#include <cmath>

namespace MissionPlanner
{
namespace
{
constexpr double kIntegerTolerance = 0.000001;

bool isGlobalFrame(quint8 frame)
{
    return frame == MAV_FRAME_GLOBAL
            || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT
            || frame == MAV_FRAME_GLOBAL_TERRAIN_ALT
            || frame == MAV_FRAME_GLOBAL_INT
            || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT_INT
            || frame == MAV_FRAME_GLOBAL_TERRAIN_ALT_INT;
}

QString coordinateError(const QString &subject,
                        const GeoCoordinate &coordinate)
{
    if (!std::isfinite(coordinate.Latitude)
        || coordinate.Latitude < -90.0
        || coordinate.Latitude > 90.0) {
        return QObject::tr("%1 latitude must be between -90 and 90 degrees.")
                .arg(subject);
    }
    if (!std::isfinite(coordinate.Longitude)
        || coordinate.Longitude < -180.0
        || coordinate.Longitude > 180.0) {
        return QObject::tr("%1 longitude must be between -180 and 180 degrees.")
                .arg(subject);
    }
    if (!std::isfinite(coordinate.Altitude)) {
        return QObject::tr("%1 altitude must be finite.").arg(subject);
    }
    return {};
}

bool samePoint(const GeoCoordinate &left, const GeoCoordinate &right)
{
    return std::abs(left.Latitude - right.Latitude) <= 1e-12
            && std::abs(left.Longitude - right.Longitude) <= 1e-12;
}

int distinctPointCount(const QVector<GeoCoordinate> &points)
{
    QVector<GeoCoordinate> distinct;
    for (const GeoCoordinate &point : points) {
        const bool present = std::any_of(
                distinct.cbegin(), distinct.cend(),
                [&point](const GeoCoordinate &candidate) {
                    return samePoint(point, candidate);
                });
        if (!present) {
            distinct.append(point);
        }
    }
    return distinct.size();
}

bool isPolygonCommand(quint16 command)
{
    return command == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
            || command == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
}

FencePolygon::PolyType polygonMode(quint16 command)
{
    return command == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
            ? FencePolygon::PolyType::Inclusive
            : FencePolygon::PolyType::Exclusive;
}

quint16 polygonCommand(FencePolygon::PolyType mode)
{
    return mode == FencePolygon::PolyType::Inclusive
            ? MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
            : MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
}

quint16 circleCommand(FenceCircle::PolyType mode)
{
    return mode == FenceCircle::PolyType::Inclusive
            ? MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION
            : MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION;
}

WpRowData rowFor(quint16 command, quint8 frame,
                 const GeoCoordinate &coordinate)
{
    WpRowData row;
    row.Command = command;
    row.Frame = frame;
    row.Lat = coordinate.Latitude;
    row.Lng = coordinate.Longitude;
    row.Alt = coordinate.Altitude;
    return row;
}
}

ModelValidationResult Fence::validate() const
{
    ModelValidationResult result;
    if (HasReturn) {
        if (!isGlobalFrame(ReturnPoint.Frame)) {
            result.error = QObject::tr(
                    "Fence return point requires a global MAVLink frame.");
            return result;
        }
        result.error = coordinateError(QObject::tr("Fence return point"),
                                       ReturnPoint.Return);
        if (!result.error.isEmpty()) {
            return result;
        }
    }

    for (int polygonIndex = 0; polygonIndex < Polygons.size();
         ++polygonIndex) {
        const FencePolygon &polygon = Polygons.at(polygonIndex);
        if (polygon.Points.size() < 3) {
            result.error = QObject::tr(
                    "Fence polygon %1 requires at least 3 vertices.")
                    .arg(polygonIndex + 1);
            return result;
        }
        for (int pointIndex = 0; pointIndex < polygon.Points.size();
             ++pointIndex) {
            result.error = coordinateError(
                    QObject::tr("Fence polygon %1 vertex %2")
                        .arg(polygonIndex + 1).arg(pointIndex + 1),
                    polygon.Points.at(pointIndex));
            if (!result.error.isEmpty()) {
                return result;
            }
        }
        if (distinctPointCount(polygon.Points) < 3) {
            result.error = QObject::tr(
                    "Fence polygon %1 requires at least 3 distinct vertices.")
                    .arg(polygonIndex + 1);
            return result;
        }
    }

    for (int circleIndex = 0; circleIndex < Circles.size(); ++circleIndex) {
        const FenceCircle &circle = Circles.at(circleIndex);
        result.error = coordinateError(
                QObject::tr("Fence circle %1 center").arg(circleIndex + 1),
                circle.Center);
        if (!result.error.isEmpty()) {
            return result;
        }
        if (!std::isfinite(circle.Radius) || circle.Radius <= 0.0) {
            result.error = QObject::tr(
                    "Fence circle %1 requires a positive finite radius.")
                    .arg(circleIndex + 1);
            return result;
        }
    }

    result.ok = true;
    return result;
}

QVector<WpRowData> Fence::FenceToLocation(QString *error) const
{
    if (error) {
        error->clear();
    }
    const ModelValidationResult validation = validate();
    if (!validation.ok) {
        if (error) {
            *error = validation.error;
        }
        return {};
    }

    QVector<WpRowData> rows;
    int sequence = 0;
    if (HasReturn) {
        WpRowData row = rowFor(MAV_CMD_NAV_FENCE_RETURN_POINT,
                               ReturnPoint.Frame, ReturnPoint.Return);
        row.Seq = sequence++;
        rows.append(row);
    }
    for (const FencePolygon &polygon : Polygons) {
        const quint16 command = polygonCommand(polygon.Mode);
        for (const GeoCoordinate &point : polygon.Points) {
            WpRowData row = rowFor(command, MAV_FRAME_GLOBAL, point);
            row.Seq = sequence++;
            row.P1 = polygon.Points.size();
            rows.append(row);
        }
    }
    for (const FenceCircle &circle : Circles) {
        WpRowData row = rowFor(circleCommand(circle.Mode),
                               MAV_FRAME_GLOBAL, circle.Center);
        row.Seq = sequence++;
        row.P1 = circle.Radius;
        rows.append(row);
    }
    return rows;
}

Fence::DecodeResult Fence::LocationToFence(
        const QVector<WpRowData> &rows)
{
    DecodeResult result;
    for (int index = 0; index < rows.size();) {
        const WpRowData &row = rows.at(index);
        const GeoCoordinate coordinate{row.Lat, row.Lng, row.Alt};
        const QString subject = QObject::tr("Fence item %1").arg(index + 1);
        result.error = coordinateError(subject, coordinate);
        if (!result.error.isEmpty()) {
            return result;
        }

        if (row.Command == MAV_CMD_NAV_FENCE_RETURN_POINT) {
            if (result.fence.HasReturn) {
                result.error = QObject::tr(
                        "Fence contains more than one return point.");
                return result;
            }
            result.fence.HasReturn = true;
            result.fence.ReturnPoint.Return = coordinate;
            result.fence.ReturnPoint.Frame = row.Frame;
            ++index;
            continue;
        }

        if (row.Command == MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION
            || row.Command == MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION) {
            if (!std::isfinite(row.P1) || row.P1 <= 0.0) {
                result.error = QObject::tr(
                        "Fence circle item %1 requires a positive radius in P1.")
                        .arg(index + 1);
                return result;
            }
            FenceCircle circle;
            circle.Center = coordinate;
            circle.Radius = row.P1;
            circle.Mode = row.Command == MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION
                    ? FenceCircle::PolyType::Inclusive
                    : FenceCircle::PolyType::Exclusive;
            result.fence.Circles.append(circle);
            ++index;
            continue;
        }

        if (isPolygonCommand(row.Command)) {
            const bool inferCount = std::isfinite(row.P1)
                    && std::abs(row.P1) <= kIntegerTolerance;
            int count = 0;
            if (inferCount) {
                int end = index;
                while (end < rows.size()
                       && rows.at(end).Command == row.Command
                       && std::isfinite(rows.at(end).P1)
                       && std::abs(rows.at(end).P1) <= kIntegerTolerance) {
                    ++end;
                }
                count = end - index;
                if (count < 3) {
                    result.error = QObject::tr(
                            "Fence polygon item %1 needs at least 3 contiguous vertices when P1 is zero.")
                            .arg(index + 1);
                    return result;
                }
            } else {
                if (!std::isfinite(row.P1) || row.P1 < 3.0) {
                    result.error = QObject::tr(
                            "Fence polygon item %1 must declare at least 3 vertices in P1.")
                            .arg(index + 1);
                    return result;
                }
                if (row.P1 > rows.size() - index) {
                    result.error = QObject::tr(
                            "Fence polygon item %1 declares %2 vertices, but only %3 remain.")
                            .arg(index + 1)
                            .arg(row.P1, 0, 'g', 12)
                            .arg(rows.size() - index);
                    return result;
                }
                count = static_cast<int>(std::round(row.P1));
                if (std::abs(row.P1 - count) > kIntegerTolerance) {
                    result.error = QObject::tr(
                            "Fence polygon item %1 must declare an integer vertex count in P1.")
                            .arg(index + 1);
                    return result;
                }
            }
            FencePolygon polygon;
            polygon.Mode = polygonMode(row.Command);
            polygon.Points.reserve(count);
            for (int offset = 0; offset < count; ++offset) {
                const WpRowData &vertex = rows.at(index + offset);
                if (vertex.Command != row.Command
                    || !std::isfinite(vertex.P1)
                    || (inferCount
                        ? std::abs(vertex.P1) > kIntegerTolerance
                        : std::abs(vertex.P1 - count) > kIntegerTolerance)) {
                    result.error = QObject::tr(
                            "Fence polygon item %1 must use command %2 and P1=%3.")
                            .arg(index + offset + 1).arg(row.Command)
                            .arg(inferCount ? 0 : count);
                    return result;
                }
                const GeoCoordinate point{
                    vertex.Lat, vertex.Lng, vertex.Alt
                };
                result.error = coordinateError(
                        QObject::tr("Fence polygon vertex %1")
                            .arg(index + offset + 1),
                        point);
                if (!result.error.isEmpty()) {
                    return result;
                }
                polygon.Points.append(point);
            }
            result.fence.Polygons.append(polygon);
            index += count;
            continue;
        }

        result.error = QObject::tr("Unsupported fence command %1 at item %2.")
                .arg(row.Command).arg(index + 1);
        return result;
    }

    const ModelValidationResult validation = result.fence.validate();
    if (!validation.ok) {
        result.error = validation.error;
        return result;
    }
    result.ok = true;
    return result;
}

ModelValidationResult RallyPoints::validate() const
{
    ModelValidationResult result;
    for (int index = 0; index < Points.size(); ++index) {
        const RallyPoint &point = Points.at(index);
        if (!isGlobalFrame(point.Frame)) {
            result.error = QObject::tr(
                    "Rally point %1 requires a global MAVLink frame.")
                    .arg(index + 1);
            return result;
        }
        result.error = coordinateError(
                QObject::tr("Rally point %1").arg(index + 1),
                point.Position);
        if (!result.error.isEmpty()) {
            return result;
        }
        if (!std::isfinite(point.BreakAltitude)) {
            result.error = QObject::tr(
                    "Rally point %1 break altitude must be finite.")
                    .arg(index + 1);
            return result;
        }
        if (!std::isfinite(point.LandHeading)
            || point.LandHeading < 0.0 || point.LandHeading > 65535.0
            || std::abs(point.LandHeading - std::round(point.LandHeading))
                    > kIntegerTolerance) {
            result.error = QObject::tr(
                    "Rally point %1 landing heading must be an integer from 0 to 65535 centidegrees.")
                    .arg(index + 1);
            return result;
        }
    }
    result.ok = true;
    return result;
}

QVector<WpRowData> RallyPoints::RallyToLocation(QString *error) const
{
    if (error) {
        error->clear();
    }
    const ModelValidationResult validation = validate();
    if (!validation.ok) {
        if (error) {
            *error = validation.error;
        }
        return {};
    }

    QVector<WpRowData> rows;
    rows.reserve(Points.size());
    for (int index = 0; index < Points.size(); ++index) {
        const RallyPoint &point = Points.at(index);
        WpRowData row = rowFor(MAV_CMD_NAV_RALLY_POINT,
                               point.Frame, point.Position);
        row.Seq = index;
        row.P1 = point.BreakAltitude;
        row.P2 = point.LandHeading;
        row.P3 = point.Flags;
        rows.append(row);
    }
    return rows;
}

RallyPoints::DecodeResult RallyPoints::LocationToRally(
        const QVector<WpRowData> &rows)
{
    DecodeResult result;
    result.rally.Points.reserve(rows.size());
    for (int index = 0; index < rows.size(); ++index) {
        const WpRowData &row = rows.at(index);
        if (row.Command != MAV_CMD_NAV_RALLY_POINT) {
            result.error = QObject::tr(
                    "Unsupported rally command %1 at item %2.")
                    .arg(row.Command).arg(index + 1);
            return result;
        }
        if (!std::isfinite(row.P3) || row.P3 < 0.0 || row.P3 > 255.0) {
            result.error = QObject::tr(
                    "Rally item %1 flags in P3 must be an integer from 0 to 255.")
                    .arg(index + 1);
            return result;
        }
        const int flags = static_cast<int>(std::round(row.P3));
        if (std::abs(row.P3 - flags) > kIntegerTolerance) {
            result.error = QObject::tr(
                    "Rally item %1 flags in P3 must be an integer from 0 to 255.")
                    .arg(index + 1);
            return result;
        }
        RallyPoint point;
        point.Position = {row.Lat, row.Lng, row.Alt};
        point.BreakAltitude = row.P1;
        point.LandHeading = row.P2;
        point.Flags = static_cast<quint8>(flags);
        point.Frame = row.Frame;
        result.rally.Points.append(point);
    }

    const ModelValidationResult validation = result.rally.validate();
    if (!validation.ok) {
        result.error = validation.error;
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace MissionPlanner
