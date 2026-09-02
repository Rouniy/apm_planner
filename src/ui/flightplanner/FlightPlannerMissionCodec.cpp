#include "FlightPlannerMissionCodec.h"

#include "comm/MissionItemProtocol.h"

#include <cmath>
#include <limits>

namespace
{
bool scaledCoordinate(double value, double scale, qint32 *result)
{
    if (!result || !std::isfinite(value)) {
        return false;
    }
    const double scaled = std::round(value * scale);
    if (scaled < static_cast<double>(std::numeric_limits<qint32>::min())
        || scaled > static_cast<double>(std::numeric_limits<qint32>::max())) {
        return false;
    }
    *result = static_cast<qint32>(scaled);
    return true;
}

bool isFencePolygon(quint16 command)
{
    return command == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
            || command == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
}

bool isFenceCircle(quint16 command)
{
    return command == MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION
            || command == MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION;
}

bool isFenceCommand(quint16 command)
{
    return command == MAV_CMD_NAV_FENCE_RETURN_POINT
        || isFencePolygon(command) || isFenceCircle(command);
}

bool isGlobalFrame(quint8 frame)
{
    return MissionItemProtocol::frameHasGlobalLocation(frame);
}

QString validateLocation(const WpRowData &row, int index,
                         const QString &kind)
{
    if (!isGlobalFrame(row.Frame)) {
        return QObject::tr("%1 item %2 requires a global MAVLink frame.")
            .arg(kind).arg(index + 1);
    }
    if (!std::isfinite(row.Lat) || row.Lat < -90.0 || row.Lat > 90.0
        || !std::isfinite(row.Lng) || row.Lng < -180.0
        || row.Lng > 180.0 || !std::isfinite(row.Alt)) {
        return QObject::tr("%1 item %2 has an invalid coordinate.")
            .arg(kind).arg(index + 1);
    }
    return {};
}

void populateDefaultFencePolygonCounts(QVector<WpRowData> *rows)
{
    if (!rows)
        return;

    for (int first = 0; first < rows->size();) {
        WpRowData &row = (*rows)[first];
        if (!isFencePolygon(row.Command)) {
            ++first;
            continue;
        }

        int declared = 0;
        const bool declaredInRange = std::isfinite(row.P1)
            && row.P1 >= 3.0 && row.P1 <= rows->size() - first;
        if (declaredInRange)
            declared = static_cast<int>(std::round(row.P1));
        if (declaredInRange
            && std::abs(row.P1 - declared) <= 0.000001) {
            first += declared;
            continue;
        }
        if (!std::isfinite(row.P1) || std::abs(row.P1) > 0.000001) {
            ++first;
            continue;
        }

        int end = first;
        while (end < rows->size()
               && isFencePolygon(rows->at(end).Command)
               && rows->at(end).Command == row.Command
               && std::isfinite(rows->at(end).P1)
               && std::abs(rows->at(end).P1) <= 0.000001) {
            ++end;
        }
        const int vertexCount = end - first;
        if (vertexCount >= 3) {
            for (int index = first; index < end; ++index)
                (*rows)[index].P1 = vertexCount;
        }
        first = end;
    }
}

QString validateFence(const QVector<WpRowData> &rows)
{
    bool returnPointSeen = false;
    for (int first = 0; first < rows.size();) {
        const WpRowData &row = rows.at(first);
        if (!isFenceCommand(row.Command)) {
            return QObject::tr("Unsupported fence command %1 at item %2.")
                .arg(row.Command).arg(first + 1);
        }
        const QString locationError = validateLocation(
            row, first, QObject::tr("Fence"));
        if (!locationError.isEmpty()) return locationError;
        if (row.Command == MAV_CMD_NAV_FENCE_RETURN_POINT) {
            if (returnPointSeen) {
                return QObject::tr("Fence contains more than one return point.");
            }
            returnPointSeen = true;
            ++first;
            continue;
        }
        if (isFenceCircle(row.Command)) {
            if (!std::isfinite(row.P1) || row.P1 <= 0.0) {
                return QObject::tr("Fence circle %1 requires a positive radius.")
                        .arg(first + 1);
            }
            ++first;
            continue;
        }
        if (!isFencePolygon(row.Command)) {
            ++first;
            continue;
        }

        const double declared = row.P1;
        if (!std::isfinite(declared) || declared < 3.0
            || declared > rows.size() - first) {
            return QObject::tr(
                    "Fence polygon at item %1 needs P1 to declare at least 3 vertices.")
                    .arg(first + 1);
        }
        const int vertexCount = static_cast<int>(std::round(declared));
        if (std::abs(declared - vertexCount) > 0.000001) {
            return QObject::tr(
                    "Fence polygon at item %1 needs P1 to declare an integer vertex count.")
                    .arg(first + 1);
        }
        const int end = first + vertexCount;
        if (end > rows.size()) {
            return QObject::tr(
                    "Fence polygon at item %1 declares %2 vertices, but only %3 remain.")
                    .arg(first + 1).arg(vertexCount).arg(rows.size() - first);
        }
        for (int index = first; index < end; ++index) {
            const WpRowData &vertex = rows.at(index);
            if (vertex.Command != row.Command
                || !std::isfinite(vertex.P1)
                || std::abs(vertex.P1 - vertexCount) > 0.000001) {
                return QObject::tr(
                    "Fence polygon item %1 must declare P1=%2 vertices.")
                        .arg(index + 1).arg(vertexCount);
            }
            const QString vertexError = validateLocation(
                vertex, index, QObject::tr("Fence"));
            if (!vertexError.isEmpty()) return vertexError;
        }
        first = end;
    }
    return {};
}

QString validateRally(const QVector<WpRowData> &rows)
{
    for (int index = 0; index < rows.size(); ++index) {
        const WpRowData &row = rows.at(index);
        if (row.Command != MAV_CMD_NAV_RALLY_POINT) {
            return QObject::tr("Unsupported rally command %1 at item %2.")
                .arg(row.Command).arg(index + 1);
        }
        const QString locationError = validateLocation(
            row, index, QObject::tr("Rally"));
        if (!locationError.isEmpty()) return locationError;
        if (!std::isfinite(row.P1) || !std::isfinite(row.P2)
            || row.P2 < 0.0 || row.P2 > 65535.0) {
            return QObject::tr(
                "Rally item %1 has an invalid break altitude or landing heading.")
                .arg(index + 1);
        }
        const int heading = static_cast<int>(std::round(row.P2));
        if (std::abs(row.P2 - heading) > 0.000001) {
            return QObject::tr(
                "Rally item %1 has an invalid break altitude or landing heading.")
                .arg(index + 1);
        }
        if (!std::isfinite(row.P3) || row.P3 < 0.0 || row.P3 > 255.0) {
            return QObject::tr(
                "Rally item %1 flags in P3 must be an integer from 0 to 255.")
                .arg(index + 1);
        }
        const int flags = static_cast<int>(std::round(row.P3));
        if (std::abs(row.P3 - flags) > 0.000001) {
            return QObject::tr(
                "Rally item %1 flags in P3 must be an integer from 0 to 255.")
                .arg(index + 1);
        }
    }
    return {};
}

bool rowToItem(const WpRowData &row, quint16 sequence,
               MAV_MISSION_TYPE type, mavlink_mission_item_int_t *item)
{
    if (!item || !std::isfinite(row.P1) || !std::isfinite(row.P2)
        || !std::isfinite(row.P3) || !std::isfinite(row.P4)
        || !std::isfinite(row.Lat) || !std::isfinite(row.Lng)
        || !std::isfinite(row.Alt)) {
        return false;
    }

    mavlink_mission_item_int_t encoded{};
    encoded.seq = sequence;
    encoded.command = row.Command;
    encoded.frame = row.Frame;
    encoded.param1 = static_cast<float>(row.P1);
    encoded.param2 = static_cast<float>(row.P2);
    encoded.param3 = static_cast<float>(row.P3);
    encoded.param4 = static_cast<float>(row.P4);
    const double scale = MissionItemProtocol::coordinateScale(
            row.Frame, row.Command);
    if (!scaledCoordinate(row.Lat, scale, &encoded.x)
        || !scaledCoordinate(row.Lng, scale, &encoded.y)) {
        return false;
    }
    encoded.z = static_cast<float>(row.Alt);
    encoded.current = 0;
    encoded.autocontinue = 1;
    encoded.mission_type = static_cast<quint8>(type);
    *item = encoded;
    return true;
}

WpRowData itemToRow(const mavlink_mission_item_int_t &item, int sequence)
{
    WpRowData row;
    row.Seq = sequence;
    row.Command = item.command;
    row.P1 = item.param1;
    row.P2 = item.param2;
    row.P3 = item.param3;
    row.P4 = item.param4;
    const double scale = MissionItemProtocol::coordinateScale(
            item.frame, item.command);
    row.Lat = item.x / scale;
    row.Lng = item.y / scale;
    row.Alt = item.z;
    row.Frame = item.frame;
    return row;
}
}

MAV_MISSION_TYPE FlightPlannerMissionCodec::missionType(
        FlightPlannerMissionModel::MissionStore store)
{
    switch (store) {
    case FlightPlannerMissionModel::MissionStore::Fence:
        return MAV_MISSION_TYPE_FENCE;
    case FlightPlannerMissionModel::MissionStore::Rally:
        return MAV_MISSION_TYPE_RALLY;
    case FlightPlannerMissionModel::MissionStore::Mission:
    default:
        return MAV_MISSION_TYPE_MISSION;
    }
}

FlightPlannerMissionCodec::DecodeResult FlightPlannerMissionCodec::decode(
        FlightPlannerMissionModel::MissionStore store,
        const QVector<mavlink_mission_item_int_t> &items)
{
    DecodeResult result;
    const MAV_MISSION_TYPE expectedType = missionType(store);
    for (int index = 0; index < items.size(); ++index) {
        const mavlink_mission_item_int_t &item = items.at(index);
        if (item.seq != index) {
            result.error = QObject::tr(
                    "Expected mission item %1, received %2.")
                    .arg(index).arg(item.seq);
            return result;
        }
        if (item.mission_type != static_cast<quint8>(expectedType)) {
            result.error = QObject::tr("Mission type changed during transfer.");
            return result;
        }
    }

    int firstGridItem = 0;
    if (store == FlightPlannerMissionModel::MissionStore::Mission
        && !items.isEmpty()) {
        const mavlink_mission_item_int_t &home = items.first();
        const WpRowData decodedHome = itemToRow(home, 0);
        if (!MissionItemProtocol::commandHasLocation(home.command)
            || !MissionItemProtocol::frameHasGlobalLocation(home.frame)
            || decodedHome.Lat < -90.0 || decodedHome.Lat > 90.0
            || decodedHome.Lng < -180.0 || decodedHome.Lng > 180.0
            || !std::isfinite(decodedHome.Alt)) {
            result.error = QObject::tr("Vehicle returned an invalid home item.");
            return result;
        }
        result.homeValid = true;
        result.homeLatitude = decodedHome.Lat;
        result.homeLongitude = decodedHome.Lng;
        result.homeAltitude = decodedHome.Alt;
        firstGridItem = 1;
    }

    result.rows.reserve(items.size() - firstGridItem);
    for (int index = firstGridItem; index < items.size(); ++index) {
        result.rows.append(itemToRow(items.at(index), index - firstGridItem));
    }
    if (store == FlightPlannerMissionModel::MissionStore::Fence) {
        result.error = validateFence(result.rows);
    } else if (store == FlightPlannerMissionModel::MissionStore::Rally) {
        result.error = validateRally(result.rows);
    }
    if (!result.error.isEmpty()) {
        result.rows.clear();
        return result;
    }
    result.ok = true;
    return result;
}

FlightPlannerMissionCodec::EncodeResult FlightPlannerMissionCodec::encode(
        FlightPlannerMissionModel::MissionStore store,
        const QVector<WpRowData> &rows, bool homeValid,
        double homeLatitude, double homeLongitude, double homeAltitude)
{
    EncodeResult result;
    const MAV_MISSION_TYPE type = missionType(store);
    QVector<WpRowData> encodedRows = rows;
    if (store == FlightPlannerMissionModel::MissionStore::Mission) {
        if (rows.isEmpty()) {
            result.error = QObject::tr("Mission requires at least one waypoint.");
            return result;
        }
        if (!homeValid || homeLatitude < -90.0 || homeLatitude > 90.0
            || homeLongitude < -180.0 || homeLongitude > 180.0
            || !std::isfinite(homeAltitude)) {
            result.error = QObject::tr("A valid Home location is required.");
            return result;
        }
    }
    if (store == FlightPlannerMissionModel::MissionStore::Fence) {
        populateDefaultFencePolygonCounts(&encodedRows);
        result.error = validateFence(encodedRows);
        if (!result.error.isEmpty()) {
            return result;
        }
    } else if (store == FlightPlannerMissionModel::MissionStore::Rally) {
        result.error = validateRally(encodedRows);
        if (!result.error.isEmpty()) {
            return result;
        }
    }

    result.items.reserve(encodedRows.size()
            + (store == FlightPlannerMissionModel::MissionStore::Mission));
    if (store == FlightPlannerMissionModel::MissionStore::Mission) {
        WpRowData home;
        home.Command = MAV_CMD_NAV_WAYPOINT;
        home.Frame = MAV_FRAME_GLOBAL;
        home.Lat = homeLatitude;
        home.Lng = homeLongitude;
        home.Alt = homeAltitude;
        mavlink_mission_item_int_t encodedHome{};
        if (!rowToItem(home, 0, type, &encodedHome)) {
            result.error = QObject::tr("Home location cannot be encoded.");
            return result;
        }
        encodedHome.current = 1;
        result.items.append(encodedHome);
    }

    const int sequenceOffset = result.items.size();
    for (int index = 0; index < encodedRows.size(); ++index) {
        mavlink_mission_item_int_t encoded{};
        if (!rowToItem(encodedRows.at(index),
                       static_cast<quint16>(index + sequenceOffset),
                       type, &encoded)) {
            result.items.clear();
            result.error = QObject::tr("Mission item %1 cannot be encoded.")
                    .arg(index + 1);
            return result;
        }
        result.items.append(encoded);
    }
    result.ok = true;
    return result;
}
