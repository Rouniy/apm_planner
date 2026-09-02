#include "SurveyGridGenerator.h"

#include <QPointF>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kDegreesToRadians = 0.017453292519943295769;
constexpr double kRadiansToDegrees = 57.295779513082320877;
constexpr double kGeometryEpsilonMeters = 1.0e-7;
constexpr int kMaximumTransectsPerPass = 10000;
constexpr int kMaximumGeneratedPoints = 1000000;
constexpr double kMaximumLocalSpanMeters = 1000000.0;

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

struct LocalSegment
{
    QPointF first;
    QPointF second;
    int laneIndex = 0;
};

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
    return point.x() >= std::min(first.x(), second.x()) - kGeometryEpsilonMeters
        && point.x() <= std::max(first.x(), second.x()) + kGeometryEpsilonMeters
        && point.y() >= std::min(first.y(), second.y()) - kGeometryEpsilonMeters
        && point.y() <= std::max(first.y(), second.y()) + kGeometryEpsilonMeters;
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

QString validateOptions(const SurveyGridOptions &options)
{
    if (!std::isfinite(options.altitudeMeters)) {
        return QStringLiteral("Survey altitude must be finite");
    }
    if (!std::isfinite(options.distanceMeters)
        || options.distanceMeters <= 0.0) {
        return QStringLiteral("Distance between survey lines must be greater than zero");
    }
    if (!std::isfinite(options.spacingMeters)
        || options.spacingMeters < 0.0) {
        return QStringLiteral("Photo spacing must be zero or greater");
    }
    if (!std::isfinite(options.angleDegrees)) {
        return QStringLiteral("Survey angle must be finite");
    }
    if (!std::isfinite(options.overshoot1Meters)
        || !std::isfinite(options.overshoot2Meters)) {
        return QStringLiteral("Survey overshoot distances must be finite");
    }
    if (!std::isfinite(options.leadin1Meters)
        || !std::isfinite(options.leadin2Meters)) {
        return QStringLiteral("Survey lead-in distances must be finite");
    }
    return QString();
}

QString preparePolygon(const QVector<SurveyGridCoordinate> &source,
                       QVector<SurveyGridCoordinate> *prepared,
                       QVector<QPointF> *localPolygon,
                       LocalProjection *projection)
{
    if (source.size() < 3) {
        return QStringLiteral("Survey polygon requires at least three vertices");
    }

    prepared->clear();
    prepared->reserve(source.size());
    for (int index = 0; index < source.size(); ++index) {
        const SurveyGridCoordinate &coordinate = source.at(index);
        if (!finiteCoordinate(coordinate)) {
            return QStringLiteral("Survey polygon vertex %1 is not finite")
                .arg(index + 1);
        }
        if (coordinate.latitude < -90.0 || coordinate.latitude > 90.0) {
            return QStringLiteral("Survey polygon vertex %1 has latitude outside [-90, 90]")
                .arg(index + 1);
        }
        if (coordinate.longitude < -180.0 || coordinate.longitude > 180.0) {
            return QStringLiteral("Survey polygon vertex %1 has longitude outside [-180, 180]")
                .arg(index + 1);
        }
        if (!prepared->isEmpty()) {
            const SurveyGridCoordinate &previous = prepared->last();
            if (previous.latitude == coordinate.latitude
                && previous.longitude == coordinate.longitude) {
                continue;
            }
        }
        prepared->append(coordinate);
    }
    if (prepared->size() > 1
        && prepared->first().latitude == prepared->last().latitude
        && prepared->first().longitude == prepared->last().longitude) {
        prepared->removeLast();
    }
    if (prepared->size() < 3) {
        return QStringLiteral("Survey polygon requires at least three distinct vertices");
    }

    double latitudeSum = 0.0;
    for (const SurveyGridCoordinate &coordinate : *prepared) {
        latitudeSum += coordinate.latitude;
    }
    projection->latitudeOrigin = latitudeSum / prepared->size();
    projection->longitudeOrigin = prepared->first().longitude;
    const double cosineLatitude = std::cos(projection->latitudeOrigin
                                            * kDegreesToRadians);
    if (std::abs(cosineLatitude) < 1.0e-6) {
        return QStringLiteral("Survey polygon is too close to a geographic pole");
    }
    projection->longitudeScale = kEarthRadiusMeters * cosineLatitude;

    localPolygon->clear();
    localPolygon->reserve(prepared->size());
    for (const SurveyGridCoordinate &coordinate : *prepared) {
        localPolygon->append(projection->toLocal(coordinate));
    }

    double minimumX = localPolygon->first().x();
    double maximumX = minimumX;
    double minimumY = localPolygon->first().y();
    double maximumY = minimumY;
    double twiceArea = 0.0;
    for (int index = 0; index < localPolygon->size(); ++index) {
        const QPointF &first = localPolygon->at(index);
        const QPointF &second = localPolygon->at(
            (index + 1) % localPolygon->size());
        minimumX = std::min(minimumX, first.x());
        maximumX = std::max(maximumX, first.x());
        minimumY = std::min(minimumY, first.y());
        maximumY = std::max(maximumY, first.y());
        twiceArea += first.x() * second.y() - second.x() * first.y();
    }
    if (std::hypot(maximumX - minimumX, maximumY - minimumY)
        > kMaximumLocalSpanMeters) {
        return QStringLiteral("Survey polygon span exceeds the 1000 km local-grid limit");
    }
    const int count = localPolygon->size();
    for (int firstEdge = 0; firstEdge < count; ++firstEdge) {
        const int firstNext = (firstEdge + 1) % count;
        for (int secondEdge = firstEdge + 1; secondEdge < count;
             ++secondEdge) {
            const int secondNext = (secondEdge + 1) % count;
            if (firstEdge == secondEdge || firstNext == secondEdge
                || secondNext == firstEdge) {
                continue;
            }
            if (segmentsIntersect(localPolygon->at(firstEdge),
                                  localPolygon->at(firstNext),
                                  localPolygon->at(secondEdge),
                                  localPolygon->at(secondNext))) {
                return QStringLiteral("Survey polygon is self-intersecting");
            }
        }
    }
    if (std::abs(twiceArea) < 2.0) {
        return QStringLiteral("Survey polygon is degenerate or has area below one square meter");
    }
    return QString();
}

double dotProduct(const QPointF &point, const QPointF &axis)
{
    return point.x() * axis.x() + point.y() * axis.y();
}

QPointF fromAxes(double normalOffset, double distanceAlong,
                 const QPointF &normal, const QPointF &direction)
{
    return QPointF(normal.x() * normalOffset
                       + direction.x() * distanceAlong,
                   normal.y() * normalOffset
                       + direction.y() * distanceAlong);
}

QVector<LocalSegment> buildPass(const QVector<QPointF> &polygon,
                                double angleDegrees,
                                double distanceMeters,
                                QString *error)
{
    double normalizedAngle = std::fmod(angleDegrees, 180.0);
    if (normalizedAngle < 0.0) {
        normalizedAngle += 180.0;
    }
    const double angleRadians = normalizedAngle * kDegreesToRadians;
    // Mission Planner angle is a clockwise bearing from north.
    const QPointF direction(std::sin(angleRadians), std::cos(angleRadians));
    const QPointF normal(std::cos(angleRadians), -std::sin(angleRadians));

    double minimumOffset = dotProduct(polygon.first(), normal);
    double maximumOffset = minimumOffset;
    for (const QPointF &point : polygon) {
        const double offset = dotProduct(point, normal);
        minimumOffset = std::min(minimumOffset, offset);
        maximumOffset = std::max(maximumOffset, offset);
    }
    const double width = maximumOffset - minimumOffset;
    const double laneCountEstimate = std::max(
        1.0, std::ceil(width / distanceMeters - 1.0e-9));
    if (!std::isfinite(laneCountEstimate)
        || laneCountEstimate > kMaximumTransectsPerPass) {
        *error = QStringLiteral("Survey grid would exceed %1 lines; increase Distance")
            .arg(kMaximumTransectsPerPass);
        return {};
    }
    const int laneCount = static_cast<int>(laneCountEstimate);
    const double firstOffset = (minimumOffset + maximumOffset) * 0.5
        - (laneCount - 1) * distanceMeters * 0.5;

    QVector<LocalSegment> result;
    for (int lane = 0; lane < laneCount; ++lane) {
        const double offset = firstOffset + lane * distanceMeters;
        QVector<double> intersections;
        intersections.reserve(polygon.size());
        for (int edge = 0; edge < polygon.size(); ++edge) {
            const QPointF &first = polygon.at(edge);
            const QPointF &second = polygon.at((edge + 1) % polygon.size());
            const double firstOffsetAtEdge = dotProduct(first, normal);
            const double secondOffsetAtEdge = dotProduct(second, normal);
            // Half-open edge rule counts a shared polygon vertex once.
            const bool crosses = (firstOffsetAtEdge <= offset
                                  && secondOffsetAtEdge > offset)
                || (secondOffsetAtEdge <= offset
                    && firstOffsetAtEdge > offset);
            if (!crosses) {
                continue;
            }
            const double ratio = (offset - firstOffsetAtEdge)
                / (secondOffsetAtEdge - firstOffsetAtEdge);
            const QPointF point = first + (second - first) * ratio;
            intersections.append(dotProduct(point, direction));
        }
        std::sort(intersections.begin(), intersections.end());
        if ((intersections.size() % 2) != 0) {
            *error = QStringLiteral("Survey polygon produced an odd number of line intersections");
            return {};
        }

        QVector<LocalSegment> laneSegments;
        for (int index = 0; index + 1 < intersections.size(); index += 2) {
            const double firstDistance = intersections.at(index);
            const double secondDistance = intersections.at(index + 1);
            if (secondDistance - firstDistance <= kGeometryEpsilonMeters) {
                continue;
            }
            LocalSegment segment;
            segment.first = fromAxes(offset, firstDistance, normal, direction);
            segment.second = fromAxes(offset, secondDistance, normal, direction);
            segment.laneIndex = lane;
            laneSegments.append(segment);
        }
        if ((lane % 2) != 0) {
            std::reverse(laneSegments.begin(), laneSegments.end());
            for (LocalSegment &segment : laneSegments) {
                std::swap(segment.first, segment.second);
            }
        }
        result += laneSegments;
    }
    if (result.isEmpty()) {
        *error = QStringLiteral("Survey grid does not intersect the polygon");
    }
    return result;
}

QPointF anchorForStartPosition(const SurveyGridOptions &options,
                               const LocalProjection &projection,
                               const QVector<QPointF> &polygon,
                               QString *error)
{
    double minimumX = polygon.first().x();
    double maximumX = minimumX;
    double minimumY = polygon.first().y();
    double maximumY = minimumY;
    for (const QPointF &point : polygon) {
        minimumX = std::min(minimumX, point.x());
        maximumX = std::max(maximumX, point.x());
        minimumY = std::min(minimumY, point.y());
        maximumY = std::max(maximumY, point.y());
    }

    switch (options.startPosition) {
    case SurveyGridOptions::StartPosition::BottomLeft:
        return QPointF(minimumX, minimumY);
    case SurveyGridOptions::StartPosition::TopLeft:
        return QPointF(minimumX, maximumY);
    case SurveyGridOptions::StartPosition::BottomRight:
        return QPointF(maximumX, minimumY);
    case SurveyGridOptions::StartPosition::TopRight:
        return QPointF(maximumX, maximumY);
    case SurveyGridOptions::StartPosition::Home:
        if (!finiteCoordinate(options.homeLocation)
            || options.homeLocation.latitude < -90.0
            || options.homeLocation.latitude > 90.0
            || options.homeLocation.longitude < -180.0
            || options.homeLocation.longitude > 180.0) {
            *error = QStringLiteral("Survey Home start coordinate is invalid");
            return {};
        }
        return projection.toLocal(options.homeLocation);
    case SurveyGridOptions::StartPosition::Point:
        if (!finiteCoordinate(options.startPoint)
            || options.startPoint.latitude < -90.0
            || options.startPoint.latitude > 90.0
            || options.startPoint.longitude < -180.0
            || options.startPoint.longitude > 180.0) {
            *error = QStringLiteral("Survey Point start coordinate is invalid");
            return {};
        }
        return projection.toLocal(options.startPoint);
    }
    *error = QStringLiteral("Survey start position is invalid");
    return {};
}

void orientPassToAnchor(QVector<LocalSegment> *segments, const QPointF &anchor)
{
    if (segments->isEmpty()) {
        return;
    }
    const double candidates[] = {
        squaredDistance(anchor, segments->first().first),
        squaredDistance(anchor, segments->first().second),
        squaredDistance(anchor, segments->last().first),
        squaredDistance(anchor, segments->last().second),
    };
    int nearest = 0;
    for (int index = 1; index < 4; ++index) {
        if (candidates[index] < candidates[nearest]) {
            nearest = index;
        }
    }
    if (nearest >= 2) {
        std::reverse(segments->begin(), segments->end());
    }
    if ((nearest % 2) != 0) {
        for (LocalSegment &segment : *segments) {
            std::swap(segment.first, segment.second);
        }
    }
}

QPointF extendedPoint(const QPointF &from, const QPointF &towards,
                      double extensionMeters)
{
    const double dx = towards.x() - from.x();
    const double dy = towards.y() - from.y();
    const double length = std::hypot(dx, dy);
    if (length <= kGeometryEpsilonMeters || extensionMeters == 0.0) {
        return towards;
    }
    return QPointF(towards.x() + dx * extensionMeters / length,
                   towards.y() + dy * extensionMeters / length);
}

SurveyGridPoint makePoint(const QPointF &localPoint,
                          SurveyGridPointType type,
                          const LocalProjection &projection,
                          double altitude)
{
    return {projection.toGeo(localPoint, altitude), type};
}

QString appendPass(const QVector<LocalSegment> &segments, int passIndex,
                   double angleDegrees, const SurveyGridOptions &options,
                   const LocalProjection &projection,
                   SurveyGridResult *result)
{
    double normalizedAngle = std::fmod(angleDegrees, 180.0);
    if (normalizedAngle < 0.0) {
        normalizedAngle += 180.0;
    }
    const double angleRadians = normalizedAngle * kDegreesToRadians;
    const QPointF nominalDirection(std::sin(angleRadians),
                                   std::cos(angleRadians));
    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        const LocalSegment &segment = segments.at(segmentIndex);
        const bool followsNominalDirection =
            dotProduct(segment.second - segment.first, nominalDirection) >= 0.0;
        const double overshoot = followsNominalDirection
            ? options.overshoot1Meters : options.overshoot2Meters;
        const double leadin = followsNominalDirection
            ? options.leadin1Meters : options.leadin2Meters;
        const QPointF stripStart = extendedPoint(segment.second,
                                                 segment.first, leadin);
        const QPointF surveyStart = leadin < 0.0
            ? stripStart : segment.first;
        const QPointF stripEnd = extendedPoint(segment.first, segment.second,
                                               overshoot);
        const QPointF surveyEnd = overshoot < 0.0 ? stripEnd : segment.second;

        const double length = std::sqrt(squaredDistance(segment.first,
                                                         segment.second));
        int photoPointCount = 0;
        if (options.spacingMeters > 0.0) {
            const double estimate = std::max(
                0.0, std::ceil(length / options.spacingMeters) - 1.0);
            if (!std::isfinite(estimate)
                || estimate > kMaximumGeneratedPoints) {
                return QStringLiteral("Survey photo spacing would exceed %1 generated points")
                    .arg(kMaximumGeneratedPoints);
            }
            photoPointCount = static_cast<int>(estimate);
        }
        if (result->path.size() > kMaximumGeneratedPoints
            - photoPointCount - 4) {
            return QStringLiteral("Survey grid would exceed %1 generated points")
                .arg(kMaximumGeneratedPoints);
        }

        SurveyGridTransect transect;
        transect.passIndex = passIndex;
        transect.laneIndex = segment.laneIndex;
        transect.points.append(makePoint(stripStart,
                                         SurveyGridPointType::StripStart,
                                         projection, options.altitudeMeters));
        transect.points.append(makePoint(surveyStart,
                                         SurveyGridPointType::SurveyStart,
                                         projection, options.altitudeMeters));
        if (options.spacingMeters > 0.0) {
            for (double distance = options.spacingMeters;
                 distance < length - kGeometryEpsilonMeters;
                 distance += options.spacingMeters) {
                const double ratio = distance / length;
                const QPointF photo = segment.first
                    + (segment.second - segment.first) * ratio;
                transect.points.append(makePoint(photo,
                                                 SurveyGridPointType::Photo,
                                                 projection,
                                                 options.altitudeMeters));
            }
        }
        transect.points.append(makePoint(surveyEnd,
                                         SurveyGridPointType::SurveyEnd,
                                         projection, options.altitudeMeters));
        transect.points.append(makePoint(stripEnd,
                                         SurveyGridPointType::StripEnd,
                                         projection, options.altitudeMeters));
        result->path += transect.points;
        result->transects.append(transect);
    }
    return QString();
}

} // namespace

SurveyGridResult SurveyGridGenerator::CreateGrid(
    const QVector<SurveyGridCoordinate> &polygon,
    const SurveyGridOptions &options)
{
    SurveyGridResult result;
    result.error = validateOptions(options);
    if (!result.error.isEmpty()) {
        return result;
    }

    QVector<SurveyGridCoordinate> preparedPolygon;
    QVector<QPointF> localPolygon;
    LocalProjection projection;
    result.error = preparePolygon(polygon, &preparedPolygon, &localPolygon,
                                  &projection);
    if (!result.error.isEmpty()) {
        return result;
    }

    QString generationError;
    QVector<LocalSegment> primary = buildPass(
        localPolygon, options.angleDegrees, options.distanceMeters,
        &generationError);
    if (!generationError.isEmpty()) {
        result.error = generationError;
        return result;
    }

    QPointF anchor = anchorForStartPosition(options, projection, localPolygon,
                                            &result.error);
    if (!result.error.isEmpty()) {
        return result;
    }
    orientPassToAnchor(&primary, anchor);
    result.error = appendPass(primary, 0, options.angleDegrees, options,
                              projection, &result);
    if (!result.error.isEmpty()) {
        result.transects.clear();
        result.path.clear();
        return result;
    }

    if (options.crossGrid) {
        QVector<LocalSegment> secondary = buildPass(
            localPolygon, options.angleDegrees + 90.0,
            options.distanceMeters, &generationError);
        if (!generationError.isEmpty()) {
            result.transects.clear();
            result.path.clear();
            result.error = generationError;
            return result;
        }
        orientPassToAnchor(&secondary,
                           projection.toLocal(result.path.last().coordinate));
        result.error = appendPass(secondary, 1, options.angleDegrees + 90.0,
                                  options, projection, &result);
        if (!result.error.isEmpty()) {
            result.transects.clear();
            result.path.clear();
            return result;
        }
    }

    result.success = true;
    result.error.clear();
    return result;
}

QString SurveyGridGenerator::PointTag(SurveyGridPointType type)
{
    switch (type) {
    case SurveyGridPointType::StripStart:
        return QStringLiteral("S");
    case SurveyGridPointType::SurveyStart:
        return QStringLiteral("SM");
    case SurveyGridPointType::Photo:
        return QStringLiteral("M");
    case SurveyGridPointType::SurveyEnd:
        return QStringLiteral("ME");
    case SurveyGridPointType::StripEnd:
        return QStringLiteral("E");
    }
    return QString();
}
