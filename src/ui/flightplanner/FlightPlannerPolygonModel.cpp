#include "FlightPlannerPolygonModel.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QPointF>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kDegreesToRadians = 0.017453292519943295769;
constexpr double kRadiansToDegrees = 57.295779513082320877;
constexpr double kGeometryEpsilonMeters = 1.0e-6;
constexpr double kMinimumAreaSquareMeters = 0.01;
constexpr double kMaximumLocalSpanMeters = 1000000.0;
constexpr double kMaximumOffsetMeters = 100000.0;
constexpr int kMaximumPolygonVertices = 4096;

struct LocalProjection
{
    double latitudeOrigin = 0.0;
    double longitudeOrigin = 0.0;
    double longitudeScale = 1.0;

    QPointF toLocal(const SurveyGridCoordinate &coordinate) const
    {
        double longitudeDelta = coordinate.longitude - longitudeOrigin;
        while (longitudeDelta > 180.0) {
            longitudeDelta -= 360.0;
        }
        while (longitudeDelta < -180.0) {
            longitudeDelta += 360.0;
        }
        return QPointF(longitudeDelta * kDegreesToRadians * longitudeScale,
                       (coordinate.latitude - latitudeOrigin)
                               * kDegreesToRadians * kEarthRadiusMeters);
    }

    SurveyGridCoordinate toGeo(const QPointF &point, double altitude) const
    {
        SurveyGridCoordinate coordinate;
        coordinate.latitude = latitudeOrigin
                + point.y() * kRadiansToDegrees / kEarthRadiusMeters;
        coordinate.longitude = longitudeOrigin
                + point.x() * kRadiansToDegrees / longitudeScale;
        while (coordinate.longitude > 180.0) {
            coordinate.longitude -= 360.0;
        }
        while (coordinate.longitude < -180.0) {
            coordinate.longitude += 360.0;
        }
        coordinate.altitude = altitude;
        return coordinate;
    }
};

struct PreparedPolygon
{
    QVector<SurveyGridCoordinate> geographic;
    QVector<QPointF> local;
    LocalProjection projection;
    double signedTwiceArea = 0.0;
};

bool sameHorizontalPosition(const SurveyGridCoordinate &left,
                            const SurveyGridCoordinate &right)
{
    return std::abs(left.latitude - right.latitude) < 1.0e-9
            && std::abs(left.longitude - right.longitude) < 1.0e-9;
}

bool finiteCoordinate(const SurveyGridCoordinate &coordinate)
{
    return std::isfinite(coordinate.latitude)
            && std::isfinite(coordinate.longitude)
            && std::isfinite(coordinate.altitude);
}

double squaredDistance(const QPointF &first, const QPointF &second)
{
    const double dx = first.x() - second.x();
    const double dy = first.y() - second.y();
    return dx * dx + dy * dy;
}

double crossProduct(const QPointF &first, const QPointF &second,
                    const QPointF &third)
{
    return (second.x() - first.x()) * (third.y() - first.y())
            - (second.y() - first.y()) * (third.x() - first.x());
}

bool pointOnSegment(const QPointF &point, const QPointF &first,
                    const QPointF &second)
{
    if (std::abs(crossProduct(first, second, point))
            > kGeometryEpsilonMeters) {
        return false;
    }
    return point.x() >= std::min(first.x(), second.x())
                               - kGeometryEpsilonMeters
            && point.x() <= std::max(first.x(), second.x())
                               + kGeometryEpsilonMeters
            && point.y() >= std::min(first.y(), second.y())
                               - kGeometryEpsilonMeters
            && point.y() <= std::max(first.y(), second.y())
                               + kGeometryEpsilonMeters;
}

int orientation(const QPointF &first, const QPointF &second,
                const QPointF &third)
{
    const double cross = crossProduct(first, second, third);
    if (std::abs(cross) <= kGeometryEpsilonMeters) {
        return 0;
    }
    return cross > 0.0 ? 1 : -1;
}

bool segmentsIntersect(const QPointF &a1, const QPointF &a2,
                       const QPointF &b1, const QPointF &b2)
{
    const int o1 = orientation(a1, a2, b1);
    const int o2 = orientation(a1, a2, b2);
    const int o3 = orientation(b1, b2, a1);
    const int o4 = orientation(b1, b2, a2);
    if (o1 * o2 < 0 && o3 * o4 < 0) {
        return true;
    }
    return (o1 == 0 && pointOnSegment(b1, a1, a2))
            || (o2 == 0 && pointOnSegment(b2, a1, a2))
            || (o3 == 0 && pointOnSegment(a1, b1, b2))
            || (o4 == 0 && pointOnSegment(a2, b1, b2));
}

QString preparePolygon(const QVector<SurveyGridCoordinate> &source,
                       PreparedPolygon *prepared)
{
    if (!prepared) {
        return QStringLiteral("Polygon validation has no output target");
    }
    if (source.size() < 3) {
        return QStringLiteral("Polygon requires at least three vertices");
    }
    if (source.size() > kMaximumPolygonVertices) {
        return QStringLiteral("Polygon exceeds the 4096 vertex limit");
    }

    prepared->geographic = source;
    if (prepared->geographic.size() > 1
            && sameHorizontalPosition(prepared->geographic.first(),
                                      prepared->geographic.last())) {
        prepared->geographic.removeLast();
    }
    if (prepared->geographic.size() < 3) {
        return QStringLiteral("Polygon requires at least three distinct vertices");
    }

    double latitudeSum = 0.0;
    for (int index = 0; index < prepared->geographic.size(); ++index) {
        const SurveyGridCoordinate &coordinate =
                prepared->geographic.at(index);
        if (!finiteCoordinate(coordinate)) {
            return QStringLiteral("Polygon vertex %1 is not finite")
                    .arg(index + 1);
        }
        if (coordinate.latitude < -90.0 || coordinate.latitude > 90.0) {
            return QStringLiteral(
                    "Polygon vertex %1 has latitude outside [-90, 90]")
                    .arg(index + 1);
        }
        if (coordinate.longitude < -180.0
                || coordinate.longitude > 180.0) {
            return QStringLiteral(
                    "Polygon vertex %1 has longitude outside [-180, 180]")
                    .arg(index + 1);
        }
        latitudeSum += coordinate.latitude;
    }

    prepared->projection.latitudeOrigin =
            latitudeSum / prepared->geographic.size();
    prepared->projection.longitudeOrigin =
            prepared->geographic.first().longitude;
    const double cosineLatitude = std::cos(
            prepared->projection.latitudeOrigin * kDegreesToRadians);
    if (std::abs(cosineLatitude) < 1.0e-6) {
        return QStringLiteral("Polygon is too close to a geographic pole");
    }
    prepared->projection.longitudeScale =
            kEarthRadiusMeters * cosineLatitude;

    prepared->local.clear();
    prepared->local.reserve(prepared->geographic.size());
    for (const SurveyGridCoordinate &coordinate : prepared->geographic) {
        prepared->local.append(prepared->projection.toLocal(coordinate));
    }

    double minimumX = prepared->local.first().x();
    double maximumX = minimumX;
    double minimumY = prepared->local.first().y();
    double maximumY = minimumY;
    prepared->signedTwiceArea = 0.0;
    for (int firstIndex = 0; firstIndex < prepared->local.size();
         ++firstIndex) {
        const QPointF &first = prepared->local.at(firstIndex);
        const QPointF &second = prepared->local.at(
                (firstIndex + 1) % prepared->local.size());
        if (squaredDistance(first, second)
                <= kGeometryEpsilonMeters * kGeometryEpsilonMeters) {
            return QStringLiteral("Polygon contains duplicate adjacent vertices");
        }
        minimumX = std::min(minimumX, first.x());
        maximumX = std::max(maximumX, first.x());
        minimumY = std::min(minimumY, first.y());
        maximumY = std::max(maximumY, first.y());
        prepared->signedTwiceArea +=
                first.x() * second.y() - second.x() * first.y();
    }
    if (std::hypot(maximumX - minimumX, maximumY - minimumY)
            > kMaximumLocalSpanMeters) {
        return QStringLiteral(
                "Polygon span exceeds the 1000 km local-projection limit");
    }
    if (std::abs(prepared->signedTwiceArea) * 0.5
            < kMinimumAreaSquareMeters) {
        return QStringLiteral("Polygon is degenerate");
    }

    const int count = prepared->local.size();
    for (int firstVertex = 0; firstVertex < count; ++firstVertex) {
        for (int secondVertex = firstVertex + 1;
             secondVertex < count; ++secondVertex) {
            if (squaredDistance(prepared->local.at(firstVertex),
                                prepared->local.at(secondVertex))
                    <= kGeometryEpsilonMeters * kGeometryEpsilonMeters) {
                return QStringLiteral("Polygon contains duplicate vertices");
            }
        }
    }
    for (int firstEdge = 0; firstEdge < count; ++firstEdge) {
        const int firstNext = (firstEdge + 1) % count;
        for (int secondEdge = firstEdge + 1; secondEdge < count;
             ++secondEdge) {
            const int secondNext = (secondEdge + 1) % count;
            if (firstNext == secondEdge || secondNext == firstEdge) {
                continue;
            }
            if (segmentsIntersect(prepared->local.at(firstEdge),
                                  prepared->local.at(firstNext),
                                  prepared->local.at(secondEdge),
                                  prepared->local.at(secondNext))) {
                return QStringLiteral("Polygon is self-intersecting");
            }
        }
    }
    return QString();
}

bool finitePoint(const QPointF &point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool intersectLines(const QPointF &firstPoint, const QPointF &firstDirection,
                    const QPointF &secondPoint,
                    const QPointF &secondDirection, QPointF *intersection)
{
    const double denominator = firstDirection.x() * secondDirection.y()
            - firstDirection.y() * secondDirection.x();
    if (std::abs(denominator) < 1.0e-12) {
        return false;
    }
    const QPointF delta = secondPoint - firstPoint;
    const double distance = (delta.x() * secondDirection.y()
                             - delta.y() * secondDirection.x())
            / denominator;
    *intersection = firstPoint + firstDirection * distance;
    return finitePoint(*intersection);
}

bool parseFiniteCNumber(const QString &text, double *value)
{
    bool ok = false;
    const double parsed = QLocale::c().toDouble(text, &ok);
    if (!ok || !std::isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

QString coordinateNumber(double value)
{
    return QLocale::c().toString(value, 'g', 17);
}
}

FlightPlannerPolygonModel::FlightPlannerPolygonModel(QObject *parent)
    : QObject(parent)
{
}

const QVector<SurveyGridCoordinate> &
FlightPlannerPolygonModel::DrawnPolygon() const
{
    return m_drawnPolygon;
}

int FlightPlannerPolygonModel::Count() const
{
    return m_drawnPolygon.size();
}

QString FlightPlannerPolygonModel::Status() const
{
    return m_status;
}

QString FlightPlannerPolygonModel::LastError() const
{
    return m_lastError;
}

bool FlightPlannerPolygonModel::IsValid(QString *error) const
{
    PreparedPolygon prepared;
    const QString validationError = preparePolygon(m_drawnPolygon, &prepared);
    if (error) {
        *error = validationError;
    }
    return validationError.isEmpty();
}

bool FlightPlannerPolygonModel::AddDrawnPolygonPoint(
        double latitude, double longitude, double altitude)
{
    return AddDrawnPolygonPoint({latitude, longitude, altitude});
}

bool FlightPlannerPolygonModel::AddDrawnPolygonPoint(
        const SurveyGridCoordinate &point)
{
    if (!finiteCoordinate(point) || point.latitude < -90.0
            || point.latitude > 90.0 || point.longitude < -180.0
            || point.longitude > 180.0) {
        return fail(tr("Cannot add an invalid polygon coordinate."));
    }
    for (const SurveyGridCoordinate &existing : m_drawnPolygon) {
        if (sameHorizontalPosition(existing, point)) {
            return fail(tr("Cannot add a duplicate polygon vertex."));
        }
    }

    QVector<SurveyGridCoordinate> candidate = m_drawnPolygon;
    candidate.append(point);
    if (candidate.size() >= 3) {
        PreparedPolygon prepared;
        const QString validationError = preparePolygon(candidate, &prepared);
        const bool incompleteCollinearThirdPoint = candidate.size() == 3
                && validationError == QStringLiteral("Polygon is degenerate");
        if (!validationError.isEmpty() && !incompleteCollinearThirdPoint) {
            return fail(validationError);
        }
    }
    m_drawnPolygon = candidate;
    publishChange();
    succeed(tr("Polygon point %1 added.").arg(m_drawnPolygon.size()));
    return true;
}

bool FlightPlannerPolygonModel::ReplaceDrawnPolygon(
        const QVector<SurveyGridCoordinate> &points)
{
    if (points.isEmpty()) {
        ClearDrawnPolygon();
        return true;
    }
    return replaceValidated(points,
            tr("Polygon replaced with %1 vertices.").arg(points.size()));
}

void FlightPlannerPolygonModel::ClearDrawnPolygon()
{
    const bool hadPoints = !m_drawnPolygon.isEmpty();
    m_drawnPolygon.clear();
    if (hadPoints) {
        publishChange();
    }
    succeed(tr("Polygon cleared."));
}

bool FlightPlannerPolygonModel::BuildPolygonFromWaypoints(
        const QVector<WpRowData> &waypoints)
{
    QVector<SurveyGridCoordinate> points;
    points.reserve(waypoints.size());
    for (const WpRowData &waypoint : waypoints) {
        if (!WpRow::CommandIsFlightPath(waypoint.Command)
                || !WpRow::FrameHasGlobalLocation(waypoint.Frame)) {
            continue;
        }
        points.append({waypoint.Lat, waypoint.Lng, waypoint.Alt});
    }
    return replaceValidated(points,
            tr("Polygon built from %1 waypoint(s).").arg(points.size()));
}

double FlightPlannerPolygonModel::PolygonArea() const
{
    PreparedPolygon prepared;
    if (!preparePolygon(m_drawnPolygon, &prepared).isEmpty()) {
        return 0.0;
    }
    return std::abs(prepared.signedTwiceArea) * 0.5;
}

bool FlightPlannerPolygonModel::OffsetDrawnPolygon(double meters)
{
    if (!std::isfinite(meters) || meters == 0.0) {
        return fail(tr("Polygon offset must be a finite non-zero value."));
    }
    if (std::abs(meters) > kMaximumOffsetMeters) {
        return fail(tr("Polygon offset exceeds the 100 km limit."));
    }

    PreparedPolygon prepared;
    const QString validationError = preparePolygon(m_drawnPolygon, &prepared);
    if (!validationError.isEmpty()) {
        return fail(validationError);
    }

    const bool counterClockwise = prepared.signedTwiceArea > 0.0;
    const int count = prepared.local.size();
    QVector<QPointF> directions;
    QVector<QPointF> outwardNormals;
    directions.reserve(count);
    outwardNormals.reserve(count);
    for (int edge = 0; edge < count; ++edge) {
        const QPointF delta = prepared.local.at((edge + 1) % count)
                - prepared.local.at(edge);
        const double length = std::hypot(delta.x(), delta.y());
        if (length <= kGeometryEpsilonMeters) {
            return fail(tr("Polygon contains an edge too short to offset."));
        }
        const QPointF direction(delta.x() / length, delta.y() / length);
        directions.append(direction);
        outwardNormals.append(counterClockwise
                ? QPointF(direction.y(), -direction.x())
                : QPointF(-direction.y(), direction.x()));
    }

    QVector<QPointF> offsetLocal;
    offsetLocal.reserve(count);
    for (int vertex = 0; vertex < count; ++vertex) {
        const int previousEdge = (vertex + count - 1) % count;
        const int nextEdge = vertex;
        const QPointF previousLine = prepared.local.at(vertex)
                + outwardNormals.at(previousEdge) * meters;
        const QPointF nextLine = prepared.local.at(vertex)
                + outwardNormals.at(nextEdge) * meters;
        QPointF offsetVertex;
        if (!intersectLines(previousLine, directions.at(previousEdge),
                            nextLine, directions.at(nextEdge),
                            &offsetVertex)) {
            const QPointF normalSum = outwardNormals.at(previousEdge)
                    + outwardNormals.at(nextEdge);
            const double normalLength =
                    std::hypot(normalSum.x(), normalSum.y());
            if (normalLength <= kGeometryEpsilonMeters) {
                return fail(tr("Polygon has a 180 degree corner that cannot be offset."));
            }
            offsetVertex = prepared.local.at(vertex)
                    + normalSum * (meters / normalLength);
        }
        const double miterDistance = std::hypot(
                offsetVertex.x() - prepared.local.at(vertex).x(),
                offsetVertex.y() - prepared.local.at(vertex).y());
        if (!finitePoint(offsetVertex)
                || miterDistance > std::max(1000.0,
                                            std::abs(meters) * 1000.0)) {
            return fail(tr("Polygon offset produces an excessive corner miter."));
        }
        offsetLocal.append(offsetVertex);
    }

    // An inward offset larger than the local inradius can make the
    // intersections pass through the centre and form a smaller polygon with
    // the original winding again. Winding alone cannot detect that collapse;
    // every surviving edge must still advance in its original direction.
    for (int edge = 0; edge < count; ++edge) {
        const QPointF offsetDelta =
                offsetLocal.at((edge + 1) % count) - offsetLocal.at(edge);
        const double forward = offsetDelta.x() * directions.at(edge).x()
                + offsetDelta.y() * directions.at(edge).y();
        if (!std::isfinite(forward) || forward <= kGeometryEpsilonMeters) {
            return fail(tr("The requested offset collapses the polygon."));
        }
    }

    QVector<SurveyGridCoordinate> offsetGeographic;
    offsetGeographic.reserve(count);
    for (int index = 0; index < count; ++index) {
        offsetGeographic.append(prepared.projection.toGeo(
                offsetLocal.at(index),
                prepared.geographic.at(index).altitude));
    }

    PreparedPolygon offsetPrepared;
    const QString offsetError = preparePolygon(offsetGeographic,
                                                &offsetPrepared);
    if (!offsetError.isEmpty()) {
        return fail(tr("Requested polygon offset is invalid: %1")
                    .arg(offsetError));
    }
    if ((offsetPrepared.signedTwiceArea > 0.0) != counterClockwise) {
        return fail(tr("The requested offset collapses the polygon."));
    }

    m_drawnPolygon = offsetPrepared.geographic;
    publishChange();
    succeed(tr("Polygon offset by %1 m.")
            .arg(QLocale::c().toString(meters, 'g', 12)));
    return true;
}

bool FlightPlannerPolygonModel::LoadPolygon(const QString &path, bool append)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(tr("Cannot open %1: %2").arg(path, file.errorString()));
    }
    QString text = QString::fromUtf8(file.readAll());
    if (!text.isEmpty() && text.front() == QChar::ByteOrderMark) {
        text.remove(0, 1);
    }

    QVector<SurveyGridCoordinate> loaded;
    const QStringList sourceLines = text.split(
            QRegularExpression(QStringLiteral("\\r?\\n")),
            Qt::KeepEmptyParts);
    for (int lineIndex = 0; lineIndex < sourceLines.size(); ++lineIndex) {
        const QString trimmed = sourceLines.at(lineIndex).trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList fields = trimmed.split(
                QRegularExpression(QStringLiteral("[\\s,]+")),
                Qt::SkipEmptyParts);
        if (fields.size() < 2) {
            return fail(tr("Invalid polygon coordinate at line %1: "
                           "expected latitude and longitude.")
                        .arg(lineIndex + 1));
        }
        SurveyGridCoordinate coordinate;
        if (!parseFiniteCNumber(fields.at(0), &coordinate.latitude)
                || !parseFiniteCNumber(fields.at(1),
                                       &coordinate.longitude)) {
            return fail(tr("Invalid polygon coordinate at line %1: "
                           "latitude and longitude must be finite C-locale numbers.")
                        .arg(lineIndex + 1));
        }
        if (fields.size() >= 3 && !fields.at(2).startsWith(QLatin1Char('#'))
                && !parseFiniteCNumber(fields.at(2),
                                       &coordinate.altitude)) {
            return fail(tr("Invalid polygon altitude at line %1.")
                        .arg(lineIndex + 1));
        }
        loaded.append(coordinate);
    }
    if (loaded.size() > 1
            && sameHorizontalPosition(loaded.first(), loaded.last())) {
        loaded.removeLast();
    }

    QVector<SurveyGridCoordinate> candidate = append
            ? m_drawnPolygon : QVector<SurveyGridCoordinate>();
    candidate += loaded;
    const QString operation = append ? tr("Appended") : tr("Loaded");
    return replaceValidated(candidate,
            tr("%1 polygon with %2 vertices from %3.")
                    .arg(operation)
                    .arg(loaded.size())
                    .arg(QFileInfo(path).fileName()));
}

bool FlightPlannerPolygonModel::SavePolygon(const QString &path)
{
    PreparedPolygon prepared;
    const QString validationError = preparePolygon(m_drawnPolygon, &prepared);
    if (!validationError.isEmpty()) {
        return fail(validationError);
    }

    QByteArray data("# saved by APM Planner 3.0.0\n");
    for (const SurveyGridCoordinate &coordinate : prepared.geographic) {
        data += coordinateNumber(coordinate.latitude).toUtf8();
        data += ' ';
        data += coordinateNumber(coordinate.longitude).toUtf8();
        data += '\n';
    }
    const SurveyGridCoordinate &closing = prepared.geographic.first();
    data += coordinateNumber(closing.latitude).toUtf8();
    data += ' ';
    data += coordinateNumber(closing.longitude).toUtf8();
    data += '\n';

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(tr("Cannot open %1 for writing: %2")
                    .arg(path, file.errorString()));
    }
    if (file.write(data) != data.size()) {
        const QString error = tr("Cannot write %1: %2")
                .arg(path, file.errorString());
        file.cancelWriting();
        return fail(error);
    }
    if (!file.commit()) {
        return fail(tr("Cannot commit %1: %2")
                    .arg(path, file.errorString()));
    }
    succeed(tr("Saved polygon with %1 vertices to %2.")
            .arg(prepared.geographic.size())
            .arg(QFileInfo(path).fileName()));
    return true;
}

bool FlightPlannerPolygonModel::replaceValidated(
        QVector<SurveyGridCoordinate> points, const QString &successStatus)
{
    PreparedPolygon prepared;
    const QString validationError = preparePolygon(points, &prepared);
    if (!validationError.isEmpty()) {
        return fail(validationError);
    }
    m_drawnPolygon = prepared.geographic;
    publishChange();
    succeed(successStatus);
    return true;
}

bool FlightPlannerPolygonModel::fail(const QString &error)
{
    if (m_lastError != error) {
        m_lastError = error;
        emit lastErrorChanged(m_lastError);
    }
    if (m_status != error) {
        m_status = error;
        emit statusChanged(m_status);
    }
    emit errorOccurred(error);
    return false;
}

void FlightPlannerPolygonModel::succeed(const QString &status)
{
    if (!m_lastError.isEmpty()) {
        m_lastError.clear();
        emit lastErrorChanged(m_lastError);
    }
    if (m_status != status) {
        m_status = status;
        emit statusChanged(m_status);
    }
}

void FlightPlannerPolygonModel::publishChange()
{
    emit DrawnPolygonChanged();
    emit changed();
}
