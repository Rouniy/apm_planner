#include "QgcPlanFileCodec.h"

#include "QGCMAVLink.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

#include <cmath>
#include <limits>

namespace MissionPlanner
{
namespace
{
constexpr int kPlanVersion = 1;
constexpr int kMissionVersion = 2;
constexpr int kGeoFenceVersion = 2;
constexpr int kRallyVersion = 2;
constexpr int kExtensionVersion = 1;
constexpr qint64 kMaximumFileSize = 64LL * 1024LL * 1024LL;

QString itemName(const QString &section, int index)
{
    return QStringLiteral("%1 item %2").arg(section).arg(index + 1);
}

bool readFiniteNumber(const QJsonValue &value, const QString &name,
                      double *number, QString *error)
{
    if (!value.isDouble()) {
        *error = QStringLiteral("%1 must be a number.").arg(name);
        return false;
    }
    const double candidate = value.toDouble();
    if (!std::isfinite(candidate)) {
        *error = QStringLiteral("%1 must be finite.").arg(name);
        return false;
    }
    *number = candidate;
    return true;
}

bool readInteger(const QJsonValue &value, const QString &name,
                 int minimum, int maximum, int *number, QString *error)
{
    double candidate = 0.0;
    if (!readFiniteNumber(value, name, &candidate, error)) {
        return false;
    }
    if (candidate < minimum || candidate > maximum
        || candidate != std::floor(candidate)) {
        *error = QStringLiteral("%1 must be an integer from %2 to %3.")
                .arg(name).arg(minimum).arg(maximum);
        return false;
    }
    *number = static_cast<int>(candidate);
    return true;
}

bool requireObject(const QJsonObject &object, const QString &key,
                   QJsonObject *value, QString *error)
{
    if (!object.contains(key) || !object.value(key).isObject()) {
        *error = QStringLiteral("%1 must be an object.").arg(key);
        return false;
    }
    *value = object.value(key).toObject();
    return true;
}

bool requireArray(const QJsonObject &object, const QString &key,
                  QJsonArray *value, QString *error)
{
    if (!object.contains(key) || !object.value(key).isArray()) {
        *error = QStringLiteral("%1 must be an array.").arg(key);
        return false;
    }
    *value = object.value(key).toArray();
    return true;
}

bool readVersion(const QJsonObject &object, int expected,
                 const QString &name, QString *error)
{
    if (!object.contains(QStringLiteral("version"))) {
        *error = QStringLiteral("%1.version is required.").arg(name);
        return false;
    }
    int version = 0;
    if (!readInteger(object.value(QStringLiteral("version")),
                     name + QStringLiteral(".version"), 0,
                     std::numeric_limits<int>::max(), &version, error)) {
        return false;
    }
    if (version != expected) {
        *error = QStringLiteral("%1 version %2 is unsupported; expected %3.")
                .arg(name).arg(version).arg(expected);
        return false;
    }
    return true;
}

bool validateCoordinate(const GeoCoordinate &coordinate,
                        const QString &name, QString *error)
{
    if (!std::isfinite(coordinate.Latitude)
        || coordinate.Latitude < -90.0
        || coordinate.Latitude > 90.0) {
        *error = QStringLiteral("%1 latitude must be finite and between -90 and 90 degrees.")
                .arg(name);
        return false;
    }
    if (!std::isfinite(coordinate.Longitude)
        || coordinate.Longitude < -180.0
        || coordinate.Longitude > 180.0) {
        *error = QStringLiteral("%1 longitude must be finite and between -180 and 180 degrees.")
                .arg(name);
        return false;
    }
    if (!std::isfinite(coordinate.Altitude)) {
        *error = QStringLiteral("%1 altitude must be finite.").arg(name);
        return false;
    }
    return true;
}

bool readCoordinate(const QJsonValue &value, int size,
                    const QString &name, GeoCoordinate *coordinate,
                    QString *error)
{
    if (!value.isArray()) {
        *error = QStringLiteral("%1 must be an array.").arg(name);
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() != size) {
        *error = QStringLiteral("%1 must contain exactly %2 numbers.")
                .arg(name).arg(size);
        return false;
    }
    if (!readFiniteNumber(array.at(0), name + QStringLiteral(" latitude"),
                          &coordinate->Latitude, error)
        || !readFiniteNumber(array.at(1), name + QStringLiteral(" longitude"),
                             &coordinate->Longitude, error)) {
        return false;
    }
    coordinate->Altitude = 0.0;
    if (size == 3
        && !readFiniteNumber(array.at(2), name + QStringLiteral(" altitude"),
                             &coordinate->Altitude, error)) {
        return false;
    }
    return validateCoordinate(*coordinate, name, error);
}

QJsonArray coordinateJson(const GeoCoordinate &coordinate, bool altitude)
{
    QJsonArray result;
    result.append(coordinate.Latitude);
    result.append(coordinate.Longitude);
    if (altitude) {
        result.append(coordinate.Altitude);
    }
    return result;
}

bool validateRow(const WpRowData &row, const QString &name, QString *error)
{
    if (row.Seq < 0 || row.Seq > std::numeric_limits<quint16>::max()) {
        *error = QStringLiteral("%1 sequence must be from 0 to 65535.")
                .arg(name);
        return false;
    }
    const double parameters[] = {
        row.P1, row.P2, row.P3, row.P4, row.Lat, row.Lng, row.Alt
    };
    for (int index = 0; index < 7; ++index) {
        if (!std::isfinite(parameters[index])) {
            *error = QStringLiteral("%1 parameter %2 must be finite.")
                    .arg(name).arg(index + 1);
            return false;
        }
    }
    const GeoCoordinate coordinate{row.Lat, row.Lng, row.Alt};
    return validateCoordinate(coordinate, name, error);
}

bool validateRows(const QVector<WpRowData> &rows, const QString &section,
                  QString *error)
{
    if (rows.size() > std::numeric_limits<quint16>::max()) {
        *error = QStringLiteral("%1 contains more than 65535 items.").arg(section);
        return false;
    }
    for (int index = 0; index < rows.size(); ++index) {
        if (!validateRow(rows.at(index), itemName(section, index), error)) {
            return false;
        }
    }
    return true;
}

QJsonArray rowParameters(const WpRowData &row)
{
    QJsonArray params;
    params.append(row.P1);
    params.append(row.P2);
    params.append(row.P3);
    params.append(row.P4);
    params.append(row.Lat);
    params.append(row.Lng);
    params.append(row.Alt);
    return params;
}

QJsonObject standardMissionItem(const WpRowData &row)
{
    QJsonObject item;
    item.insert(QStringLiteral("autoContinue"), true);
    item.insert(QStringLiteral("command"), static_cast<int>(row.Command));
    item.insert(QStringLiteral("doJumpId"), row.Seq + 1);
    item.insert(QStringLiteral("frame"), static_cast<int>(row.Frame));
    item.insert(QStringLiteral("params"), rowParameters(row));
    item.insert(QStringLiteral("type"), QStringLiteral("SimpleItem"));
    return item;
}

QJsonArray standardMissionItems(const QVector<WpRowData> &rows)
{
    QJsonArray result;
    for (const WpRowData &row : rows) {
        result.append(standardMissionItem(row));
    }
    return result;
}

QJsonObject exactRow(const WpRowData &row)
{
    QJsonObject object;
    object.insert(QStringLiteral("seq"), row.Seq);
    object.insert(QStringLiteral("command"), static_cast<int>(row.Command));
    object.insert(QStringLiteral("frame"), static_cast<int>(row.Frame));
    object.insert(QStringLiteral("params"), rowParameters(row));
    return object;
}

QJsonArray exactRows(const QVector<WpRowData> &rows)
{
    QJsonArray result;
    for (const WpRowData &row : rows) {
        result.append(exactRow(row));
    }
    return result;
}

bool parseParameters(const QJsonValue &value, const QString &name,
                     WpRowData *row, QString *error)
{
    if (!value.isArray()) {
        *error = QStringLiteral("%1.params must be an array.").arg(name);
        return false;
    }
    const QJsonArray params = value.toArray();
    if (params.size() != 7) {
        *error = QStringLiteral("%1.params must contain exactly 7 numbers.")
                .arg(name);
        return false;
    }
    double values[7] = {};
    for (int index = 0; index < 7; ++index) {
        if (!readFiniteNumber(params.at(index),
                              QStringLiteral("%1.params[%2]")
                                  .arg(name).arg(index),
                              &values[index], error)) {
            return false;
        }
    }
    row->P1 = values[0];
    row->P2 = values[1];
    row->P3 = values[2];
    row->P4 = values[3];
    row->Lat = values[4];
    row->Lng = values[5];
    row->Alt = values[6];
    return true;
}

bool parseStandardMissionParameters(const QJsonObject &object,
                                    const QString &name, WpRowData *row,
                                    QString *error)
{
    const QJsonValue paramsValue = object.value(QStringLiteral("params"));
    if (!paramsValue.isArray()) {
        *error = QStringLiteral("%1.params must be an array.").arg(name);
        return false;
    }
    const QJsonArray params = paramsValue.toArray();
    if (params.size() != 4 && params.size() != 7) {
        *error = QStringLiteral(
            "%1.params must contain exactly 4 or 7 numbers.").arg(name);
        return false;
    }

    double values[7] = {};
    for (int index = 0; index < params.size(); ++index) {
        const QJsonValue value = params.at(index);
        // QGroundControl serializes unset MAVLink float parameters as JSON
        // null. WpRowData intentionally has no NaN state, so use MAVLink's
        // neutral zero when importing a standard file without our exact block.
        if (value.isNull()) {
            values[index] = 0.0;
            continue;
        }
        if (!readFiniteNumber(value,
                              QStringLiteral("%1.params[%2]")
                                  .arg(name).arg(index),
                              &values[index], error)) {
            return false;
        }
    }
    row->P1 = values[0];
    row->P2 = values[1];
    row->P3 = values[2];
    row->P4 = values[3];

    if (params.size() == 7) {
        row->Lat = values[4];
        row->Lng = values[5];
        row->Alt = values[6];
        return true;
    }

    GeoCoordinate coordinate;
    if (!readCoordinate(object.value(QStringLiteral("coordinate")), 3,
                        name + QStringLiteral(".coordinate"),
                        &coordinate, error)) {
        return false;
    }
    row->Lat = coordinate.Latitude;
    row->Lng = coordinate.Longitude;
    row->Alt = coordinate.Altitude;
    return true;
}

bool parseExactRows(const QJsonValue &value, const QString &section,
                    QVector<WpRowData> *rows, QString *error)
{
    if (!value.isArray()) {
        *error = QStringLiteral("apmPlanner.%1 must be an array.").arg(section);
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() > std::numeric_limits<quint16>::max()) {
        *error = QStringLiteral("apmPlanner.%1 contains more than 65535 items.")
                .arg(section);
        return false;
    }
    rows->clear();
    rows->reserve(array.size());
    for (int index = 0; index < array.size(); ++index) {
        const QString name = QStringLiteral("apmPlanner.%1[%2]")
                .arg(section).arg(index);
        if (!array.at(index).isObject()) {
            *error = QStringLiteral("%1 must be an object.").arg(name);
            return false;
        }
        const QJsonObject object = array.at(index).toObject();
        if (!object.contains(QStringLiteral("seq"))
            || !object.contains(QStringLiteral("command"))
            || !object.contains(QStringLiteral("frame"))
            || !object.contains(QStringLiteral("params"))) {
            *error = QStringLiteral("%1 requires seq, command, frame and params.")
                    .arg(name);
            return false;
        }
        WpRowData row;
        int integer = 0;
        if (!readInteger(object.value(QStringLiteral("seq")),
                         name + QStringLiteral(".seq"), 0,
                         std::numeric_limits<quint16>::max(), &row.Seq, error)
            || !readInteger(object.value(QStringLiteral("command")),
                            name + QStringLiteral(".command"), 0,
                            std::numeric_limits<quint16>::max(), &integer,
                            error)) {
            return false;
        }
        row.Command = static_cast<quint16>(integer);
        if (!readInteger(object.value(QStringLiteral("frame")),
                         name + QStringLiteral(".frame"), 0,
                         std::numeric_limits<quint8>::max(), &integer,
                         error)) {
            return false;
        }
        row.Frame = static_cast<quint8>(integer);
        if (!parseParameters(object.value(QStringLiteral("params")), name,
                             &row, error)
            || !validateRow(row, name, error)) {
            return false;
        }
        rows->append(row);
    }
    return true;
}

bool parseSimpleMissionItem(const QJsonObject &object, int fallbackSequence,
                            const QString &name, WpRowData *row,
                            QString *error)
{
    if (!object.value(QStringLiteral("autoContinue")).isBool()) {
        *error = QStringLiteral("%1.autoContinue must be a boolean.").arg(name);
        return false;
    }
    int integer = 0;
    if (!readInteger(object.value(QStringLiteral("command")),
                     name + QStringLiteral(".command"), 0,
                     std::numeric_limits<quint16>::max(), &integer, error)) {
        return false;
    }
    row->Command = static_cast<quint16>(integer);
    if (!readInteger(object.value(QStringLiteral("frame")),
                     name + QStringLiteral(".frame"), 0,
                     std::numeric_limits<quint8>::max(), &integer, error)) {
        return false;
    }
    row->Frame = static_cast<quint8>(integer);
    row->Seq = fallbackSequence;
    if (object.contains(QStringLiteral("doJumpId"))) {
        if (!readInteger(object.value(QStringLiteral("doJumpId")),
                         name + QStringLiteral(".doJumpId"), 1,
                         std::numeric_limits<int>::max(), &integer, error)) {
            return false;
        }
        row->Seq = integer - 1;
    }
    return parseStandardMissionParameters(object, name, row, error)
            && validateRow(*row, name, error);
}

bool parseGeneratedMissionItems(const QJsonArray &array,
                                QVector<WpRowData> *rows, QString *error,
                                const QString &prefix)
{
    // Current QGC Survey and CorridorScan items store the exact MAVLink
    // expansion under TransectStyleComplexItem.Items.  Treat that expansion
    // as authoritative instead of trying to reproduce QGC's geometry and
    // camera calculations.  The generated list is deliberately not parsed
    // recursively: QGC writes MissionItem objects here, and accepting nested
    // ComplexItems would permit unbounded recursion without adding a valid
    // interoperability case.
    if (array.isEmpty()) {
        *error = QStringLiteral(
                "%1 must not be empty; this ComplexItem has no lossless mission expansion.")
                .arg(prefix);
        return false;
    }
    if (rows->size() + array.size()
        > std::numeric_limits<quint16>::max()) {
        *error = QStringLiteral("%1 would exceed the 65535 mission item limit.")
                .arg(prefix);
        return false;
    }

    for (int index = 0; index < array.size(); ++index) {
        const QString name = QStringLiteral("%1[%2]").arg(prefix).arg(index);
        if (!array.at(index).isObject()) {
            *error = QStringLiteral("%1 must be an object.").arg(name);
            return false;
        }
        const QJsonObject item = array.at(index).toObject();
        if (item.value(QStringLiteral("type"))
            != QJsonValue(QStringLiteral("SimpleItem"))) {
            *error = QStringLiteral(
                    "%1 must be a generated SimpleItem; nested or unknown items cannot be imported losslessly.")
                    .arg(name);
            return false;
        }
        WpRowData row;
        if (!parseSimpleMissionItem(item, rows->size(), name, &row, error)) {
            return false;
        }
        rows->append(row);
    }
    return true;
}

bool parseComplexMissionItem(const QJsonObject &item,
                             QVector<WpRowData> *rows, QString *error,
                             const QString &name)
{
    const QJsonValue complexTypeValue =
            item.value(QStringLiteral("complexItemType"));
    if (!complexTypeValue.isString()
        || complexTypeValue.toString().isEmpty()) {
        *error = QStringLiteral("%1.complexItemType must be a non-empty string.")
                .arg(name);
        return false;
    }

    QJsonObject transect;
    QJsonArray children;
    if (!requireObject(item, QStringLiteral("TransectStyleComplexItem"),
                       &transect, error)
        || !requireArray(transect, QStringLiteral("Items"), &children,
                         error)) {
        if (!error->isEmpty()) {
            *error = QStringLiteral("%1 (%2): %3")
                    .arg(name, complexTypeValue.toString(), *error);
        }
        return false;
    }
    return parseGeneratedMissionItems(
            children, rows, error,
            name + QStringLiteral(".TransectStyleComplexItem.Items"));
}

bool parseMissionItemArray(const QJsonArray &array,
                           QVector<WpRowData> *rows, QString *error,
                           const QString &prefix)
{
    for (int index = 0; index < array.size(); ++index) {
        const QString name = QStringLiteral("%1[%2]").arg(prefix).arg(index);
        if (!array.at(index).isObject()) {
            *error = QStringLiteral("%1 must be an object.").arg(name);
            return false;
        }
        const QJsonObject item = array.at(index).toObject();
        if (!item.value(QStringLiteral("type")).isString()) {
            *error = QStringLiteral("%1.type must be a string.").arg(name);
            return false;
        }
        const QString type = item.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("SimpleItem")) {
            WpRowData row;
            if (!parseSimpleMissionItem(item, rows->size(), name, &row,
                                        error)) {
                return false;
            }
            rows->append(row);
            continue;
        }
        if (type == QStringLiteral("ComplexItem")) {
            if (!parseComplexMissionItem(item, rows, error, name)) {
                return false;
            }
            continue;
        }
        *error = QStringLiteral("%1.type '%2' is unsupported.").arg(name, type);
        return false;
    }
    return true;
}

bool buildFenceObject(const QVector<WpRowData> &rows, QJsonObject *object,
                      QString *error)
{
    const Fence::DecodeResult decoded = Fence::LocationToFence(rows);
    if (!decoded.ok) {
        *error = QStringLiteral("Fence: %1").arg(decoded.error);
        return false;
    }

    QJsonArray polygons;
    for (const FencePolygon &polygon : decoded.fence.Polygons) {
        QJsonArray points;
        for (const GeoCoordinate &point : polygon.Points) {
            points.append(coordinateJson(point, false));
        }
        QJsonObject value;
        value.insert(QStringLiteral("inclusion"),
                     polygon.Mode == FencePolygon::PolyType::Inclusive);
        value.insert(QStringLiteral("polygon"), points);
        value.insert(QStringLiteral("version"), 1);
        polygons.append(value);
    }

    QJsonArray circles;
    for (const FenceCircle &circle : decoded.fence.Circles) {
        QJsonObject geometry;
        geometry.insert(QStringLiteral("center"),
                        coordinateJson(circle.Center, false));
        geometry.insert(QStringLiteral("radius"), circle.Radius);
        QJsonObject value;
        value.insert(QStringLiteral("circle"), geometry);
        value.insert(QStringLiteral("inclusion"),
                     circle.Mode == FenceCircle::PolyType::Inclusive);
        value.insert(QStringLiteral("version"), 1);
        circles.append(value);
    }

    object->insert(QStringLiteral("circles"), circles);
    object->insert(QStringLiteral("polygons"), polygons);
    object->insert(QStringLiteral("version"), kGeoFenceVersion);
    if (decoded.fence.HasReturn) {
        object->insert(QStringLiteral("breachReturn"),
                       coordinateJson(decoded.fence.ReturnPoint.Return, true));
    }
    return true;
}

bool buildRallyObject(const QVector<WpRowData> &rows, QJsonObject *object,
                      QString *error)
{
    const RallyPoints::DecodeResult decoded = RallyPoints::LocationToRally(rows);
    if (!decoded.ok) {
        *error = QStringLiteral("Rally: %1").arg(decoded.error);
        return false;
    }
    QJsonArray points;
    for (const RallyPoint &point : decoded.rally.Points) {
        points.append(coordinateJson(point.Position, true));
    }
    object->insert(QStringLiteral("points"), points);
    object->insert(QStringLiteral("version"), kRallyVersion);
    return true;
}

bool parseFenceObject(const QJsonObject &object, QVector<WpRowData> *rows,
                      QString *error)
{
    if (!readVersion(object, kGeoFenceVersion, QStringLiteral("geoFence"),
                     error)) {
        return false;
    }
    QJsonArray polygons;
    QJsonArray circles;
    if (!requireArray(object, QStringLiteral("polygons"), &polygons, error)
        || !requireArray(object, QStringLiteral("circles"), &circles, error)) {
        return false;
    }

    rows->clear();
    int sequence = 0;
    if (object.contains(QStringLiteral("breachReturn"))) {
        GeoCoordinate coordinate;
        if (!readCoordinate(object.value(QStringLiteral("breachReturn")), 3,
                            QStringLiteral("geoFence.breachReturn"),
                            &coordinate, error)) {
            return false;
        }
        WpRowData row;
        row.Seq = sequence++;
        row.Command = MAV_CMD_NAV_FENCE_RETURN_POINT;
        row.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        row.Lat = coordinate.Latitude;
        row.Lng = coordinate.Longitude;
        row.Alt = coordinate.Altitude;
        rows->append(row);
    }

    for (int index = 0; index < polygons.size(); ++index) {
        const QString name = QStringLiteral("geoFence.polygons[%1]").arg(index);
        if (!polygons.at(index).isObject()) {
            *error = QStringLiteral("%1 must be an object.").arg(name);
            return false;
        }
        const QJsonObject polygon = polygons.at(index).toObject();
        if (!readVersion(polygon, 1, name, error)
            || !polygon.value(QStringLiteral("inclusion")).isBool()) {
            if (error->isEmpty()) {
                *error = QStringLiteral("%1.inclusion must be a boolean.")
                        .arg(name);
            }
            return false;
        }
        QJsonArray points;
        if (!requireArray(polygon, QStringLiteral("polygon"), &points, error)
            || points.size() < 3) {
            if (points.size() < 3 && error->isEmpty()) {
                *error = QStringLiteral("%1.polygon requires at least 3 points.")
                        .arg(name);
            }
            return false;
        }
        const quint16 command = polygon.value(QStringLiteral("inclusion")).toBool()
                ? MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
                : MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
        for (int pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
            GeoCoordinate coordinate;
            if (!readCoordinate(points.at(pointIndex), 2,
                                QStringLiteral("%1.polygon[%2]")
                                    .arg(name).arg(pointIndex),
                                &coordinate, error)) {
                return false;
            }
            WpRowData row;
            row.Seq = sequence++;
            row.Command = command;
            row.Frame = MAV_FRAME_GLOBAL;
            row.P1 = points.size();
            row.Lat = coordinate.Latitude;
            row.Lng = coordinate.Longitude;
            rows->append(row);
        }
    }

    for (int index = 0; index < circles.size(); ++index) {
        const QString name = QStringLiteral("geoFence.circles[%1]").arg(index);
        if (!circles.at(index).isObject()) {
            *error = QStringLiteral("%1 must be an object.").arg(name);
            return false;
        }
        const QJsonObject circle = circles.at(index).toObject();
        if (!readVersion(circle, 1, name, error)
            || !circle.value(QStringLiteral("inclusion")).isBool()) {
            if (error->isEmpty()) {
                *error = QStringLiteral("%1.inclusion must be a boolean.")
                        .arg(name);
            }
            return false;
        }
        QJsonObject geometry;
        if (!requireObject(circle, QStringLiteral("circle"), &geometry, error)) {
            return false;
        }
        GeoCoordinate center;
        if (!readCoordinate(geometry.value(QStringLiteral("center")), 2,
                            name + QStringLiteral(".circle.center"),
                            &center, error)) {
            return false;
        }
        double radius = 0.0;
        if (!readFiniteNumber(geometry.value(QStringLiteral("radius")),
                              name + QStringLiteral(".circle.radius"),
                              &radius, error)
            || radius <= 0.0) {
            if (radius <= 0.0 && error->isEmpty()) {
                *error = QStringLiteral("%1.circle.radius must be positive.")
                        .arg(name);
            }
            return false;
        }
        WpRowData row;
        row.Seq = sequence++;
        row.Command = circle.value(QStringLiteral("inclusion")).toBool()
                ? MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION
                : MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION;
        row.Frame = MAV_FRAME_GLOBAL;
        row.P1 = radius;
        row.Lat = center.Latitude;
        row.Lng = center.Longitude;
        rows->append(row);
    }

    const Fence::DecodeResult decoded = Fence::LocationToFence(*rows);
    if (!decoded.ok) {
        *error = QStringLiteral("geoFence: %1").arg(decoded.error);
        return false;
    }
    return true;
}

bool parseRallyObject(const QJsonObject &object, QVector<WpRowData> *rows,
                      QString *error)
{
    if (!readVersion(object, kRallyVersion, QStringLiteral("rallyPoints"),
                     error)) {
        return false;
    }
    QJsonArray points;
    if (!requireArray(object, QStringLiteral("points"), &points, error)) {
        return false;
    }
    rows->clear();
    rows->reserve(points.size());
    for (int index = 0; index < points.size(); ++index) {
        GeoCoordinate coordinate;
        if (!readCoordinate(points.at(index), 3,
                            QStringLiteral("rallyPoints.points[%1]").arg(index),
                            &coordinate, error)) {
            return false;
        }
        WpRowData row;
        row.Seq = index;
        row.Command = MAV_CMD_NAV_RALLY_POINT;
        row.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        row.Lat = coordinate.Latitude;
        row.Lng = coordinate.Longitude;
        row.Alt = coordinate.Altitude;
        rows->append(row);
    }
    const RallyPoints::DecodeResult decoded = RallyPoints::LocationToRally(*rows);
    if (!decoded.ok) {
        *error = QStringLiteral("rallyPoints: %1").arg(decoded.error);
        return false;
    }
    return true;
}

bool validatePlan(const QgcPlanFileCodec::PlanData &plan, QString *error)
{
    if (!validateCoordinate(plan.Home, QStringLiteral("Home"), error)) {
        return false;
    }
    if (!std::isfinite(plan.CruiseSpeed) || plan.CruiseSpeed <= 0.0) {
        *error = QStringLiteral("CruiseSpeed must be positive and finite.");
        return false;
    }
    if (!std::isfinite(plan.HoverSpeed) || plan.HoverSpeed <= 0.0) {
        *error = QStringLiteral("HoverSpeed must be positive and finite.");
        return false;
    }
    if (plan.FirmwareType < 0 || plan.FirmwareType > 255) {
        *error = QStringLiteral("FirmwareType must be from 0 to 255.");
        return false;
    }
    if (plan.VehicleType < 0 || plan.VehicleType > 255) {
        *error = QStringLiteral("VehicleType must be from 0 to 255.");
        return false;
    }
    if (!validateRows(plan.Mission, QStringLiteral("Mission"), error)
        || !validateRows(plan.Fence, QStringLiteral("Fence"), error)
        || !validateRows(plan.Rally, QStringLiteral("Rally"), error)) {
        return false;
    }
    QJsonObject ignored;
    return buildFenceObject(plan.Fence, &ignored, error)
            && buildRallyObject(plan.Rally, &ignored, error);
}

} // namespace

QgcPlanFileCodec::EncodeResult QgcPlanFileCodec::EncodePlan(
        const PlanData &plan)
{
    EncodeResult result;
    if (!validatePlan(plan, &result.error)) {
        return result;
    }

    QJsonObject geoFence;
    QJsonObject rallyPoints;
    if (!buildFenceObject(plan.Fence, &geoFence, &result.error)
        || !buildRallyObject(plan.Rally, &rallyPoints, &result.error)) {
        return result;
    }

    QJsonObject mission;
    mission.insert(QStringLiteral("cruiseSpeed"), plan.CruiseSpeed);
    mission.insert(QStringLiteral("firmwareType"), plan.FirmwareType);
    mission.insert(QStringLiteral("hoverSpeed"), plan.HoverSpeed);
    mission.insert(QStringLiteral("items"), standardMissionItems(plan.Mission));
    mission.insert(QStringLiteral("plannedHomePosition"),
                   coordinateJson(plan.Home, true));
    mission.insert(QStringLiteral("vehicleType"), plan.VehicleType);
    mission.insert(QStringLiteral("version"), kMissionVersion);

    QJsonObject extension;
    extension.insert(QStringLiteral("version"), kExtensionVersion);
    extension.insert(QStringLiteral("missionItems"), exactRows(plan.Mission));
    extension.insert(QStringLiteral("fenceItems"), exactRows(plan.Fence));
    extension.insert(QStringLiteral("rallyItems"), exactRows(plan.Rally));

    QJsonObject root;
    root.insert(QStringLiteral("fileType"), QStringLiteral("Plan"));
    root.insert(QStringLiteral("geoFence"), geoFence);
    root.insert(QStringLiteral("groundStation"),
                QStringLiteral("APM Planner 3.0.0"));
    root.insert(QStringLiteral("mission"), mission);
    root.insert(QStringLiteral("rallyPoints"), rallyPoints);
    root.insert(QStringLiteral("version"), kPlanVersion);
    root.insert(QStringLiteral("apmPlanner"), extension);

    result.data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    result.ok = true;
    return result;
}

QgcPlanFileCodec::DecodeResult QgcPlanFileCodec::DecodePlan(
        const QByteArray &data)
{
    DecodeResult result;
    if (data.size() > kMaximumFileSize) {
        result.error = QStringLiteral("QGC Plan exceeds the 64 MiB size limit.");
        return result;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.error = QStringLiteral("Invalid QGC Plan JSON at offset %1: %2")
                .arg(parseError.offset).arg(parseError.errorString());
        return result;
    }
    if (!document.isObject()) {
        result.error = QStringLiteral("QGC Plan root must be an object.");
        return result;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("fileType"))
            != QJsonValue(QStringLiteral("Plan"))) {
        result.error = QStringLiteral("fileType must be 'Plan'.");
        return result;
    }
    if (!root.value(QStringLiteral("groundStation")).isString()
        || root.value(QStringLiteral("groundStation")).toString().isEmpty()) {
        result.error = QStringLiteral("groundStation must be a non-empty string.");
        return result;
    }
    if (!readVersion(root, kPlanVersion, QStringLiteral("Plan"),
                     &result.error)) {
        return result;
    }

    QJsonObject mission;
    QJsonObject geoFence;
    QJsonObject rallyPoints;
    if (!requireObject(root, QStringLiteral("mission"), &mission,
                       &result.error)
        || !requireObject(root, QStringLiteral("geoFence"), &geoFence,
                          &result.error)
        || !requireObject(root, QStringLiteral("rallyPoints"), &rallyPoints,
                          &result.error)
        || !readVersion(mission, kMissionVersion, QStringLiteral("mission"),
                        &result.error)) {
        return result;
    }

    if (!readCoordinate(mission.value(QStringLiteral("plannedHomePosition")),
                        3, QStringLiteral("mission.plannedHomePosition"),
                        &result.plan.Home, &result.error)) {
        return result;
    }
    if (!readInteger(mission.value(QStringLiteral("firmwareType")),
                     QStringLiteral("mission.firmwareType"), 0, 255,
                     &result.plan.FirmwareType, &result.error)
        || !readInteger(mission.value(QStringLiteral("vehicleType")),
                        QStringLiteral("mission.vehicleType"), 0, 255,
                        &result.plan.VehicleType, &result.error)
        || !readFiniteNumber(mission.value(QStringLiteral("cruiseSpeed")),
                             QStringLiteral("mission.cruiseSpeed"),
                             &result.plan.CruiseSpeed, &result.error)
        || !readFiniteNumber(mission.value(QStringLiteral("hoverSpeed")),
                             QStringLiteral("mission.hoverSpeed"),
                             &result.plan.HoverSpeed, &result.error)
        || result.plan.CruiseSpeed <= 0.0 || result.plan.HoverSpeed <= 0.0) {
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                    "mission cruiseSpeed and hoverSpeed must be positive.");
        }
        return result;
    }

    QJsonArray missionItems;
    if (!requireArray(mission, QStringLiteral("items"), &missionItems,
                      &result.error)
        || !parseMissionItemArray(missionItems, &result.plan.Mission,
                                  &result.error,
                                  QStringLiteral("mission.items"))
        || !parseFenceObject(geoFence, &result.plan.Fence, &result.error)
        || !parseRallyObject(rallyPoints, &result.plan.Rally, &result.error)) {
        return result;
    }

    if (root.contains(QStringLiteral("apmPlanner"))) {
        if (!root.value(QStringLiteral("apmPlanner")).isObject()) {
            result.error = QStringLiteral("apmPlanner must be an object.");
            return result;
        }
        const QJsonObject extension =
                root.value(QStringLiteral("apmPlanner")).toObject();
        if (!readVersion(extension, kExtensionVersion,
                         QStringLiteral("apmPlanner"), &result.error)) {
            return result;
        }
        QVector<WpRowData> exactMission;
        QVector<WpRowData> exactFence;
        QVector<WpRowData> exactRally;
        if (!parseExactRows(extension.value(QStringLiteral("missionItems")),
                            QStringLiteral("missionItems"), &exactMission,
                            &result.error)
            || !parseExactRows(extension.value(QStringLiteral("fenceItems")),
                               QStringLiteral("fenceItems"), &exactFence,
                               &result.error)
            || !parseExactRows(extension.value(QStringLiteral("rallyItems")),
                               QStringLiteral("rallyItems"), &exactRally,
                               &result.error)) {
            return result;
        }
        QJsonObject exactFenceObject;
        QJsonObject exactRallyObject;
        if (standardMissionItems(exactMission) != missionItems
            || !buildFenceObject(exactFence, &exactFenceObject, &result.error)
            || !buildRallyObject(exactRally, &exactRallyObject, &result.error)
            || exactFenceObject != geoFence || exactRallyObject != rallyPoints) {
            if (result.error.isEmpty()) {
                result.error = QStringLiteral(
                        "apmPlanner exact items do not match the standard QGC Plan sections.");
            }
            return result;
        }
        result.plan.Mission = exactMission;
        result.plan.Fence = exactFence;
        result.plan.Rally = exactRally;
    }

    if (!validatePlan(result.plan, &result.error)) {
        return result;
    }
    result.ok = true;
    return result;
}

QgcPlanFileCodec::Result QgcPlanFileCodec::SavePlan(
        const QString &path, const PlanData &plan)
{
    Result result;
    if (path.isEmpty()) {
        result.error = QStringLiteral("QGC Plan path must not be empty.");
        return result;
    }
    const EncodeResult encoded = EncodePlan(plan);
    if (!encoded.ok) {
        result.error = encoded.error;
        return result;
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Cannot open '%1' for atomic save: %2")
                .arg(path, file.errorString());
        return result;
    }
    if (file.write(encoded.data) != encoded.data.size()) {
        result.error = QStringLiteral("Cannot write '%1': %2")
                .arg(path, file.errorString());
        file.cancelWriting();
        return result;
    }
    if (!file.commit()) {
        result.error = QStringLiteral("Cannot commit '%1': %2")
                .arg(path, file.errorString());
        return result;
    }
    result.ok = true;
    return result;
}

QgcPlanFileCodec::DecodeResult QgcPlanFileCodec::LoadPlan(
        const QString &path)
{
    DecodeResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot open '%1': %2")
                .arg(path, file.errorString());
        return result;
    }
    if (file.size() > kMaximumFileSize) {
        result.error = QStringLiteral("QGC Plan exceeds the 64 MiB size limit.");
        return result;
    }
    return DecodePlan(file.readAll());
}

} // namespace MissionPlanner
