#include "PropagationCore.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace PropagationCore
{
namespace
{
constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr double MinimumAzimuthStepDegrees = 0.5;
constexpr double MaximumAzimuthStepDegrees = 10.0;
constexpr double MinimumConvergenceDegrees = 0.05;
constexpr int MaximumRayIterations = 32;
constexpr int MaximumPathSegments = 20000;
constexpr double PathSampleSpacingMeters = 10.0;
constexpr int MaximumRasterDimension = 4096;
constexpr int MaximumResolutionPixels = 64;

double radians(double degrees)
{
    return degrees * Pi / 180.0;
}

double degrees(double radiansValue)
{
    return radiansValue * 180.0 / Pi;
}

bool cancellationRequested(const CancellationCheck &isCancelled)
{
    return isCancelled && isCancelled();
}

bool finiteRfInput(const CoverageRequest &request)
{
    const Parameters &parameters = request.parameters;
    return request.home.hasValidCoordinates()
        && std::isfinite(request.home.altitudeM)
        && std::isfinite(parameters.rangeKilometers)
        && std::isfinite(parameters.azimuthStepDegrees)
        && std::isfinite(parameters.convergenceDegrees)
        && std::isfinite(parameters.baseHeightMeters)
        && std::isfinite(parameters.clearanceMeters);
}

bool finiteRasterInput(const RasterRequest &request)
{
    return request.extent.isValid()
        && std::isfinite(request.vehicleAltitudeAmsl)
        && std::isfinite(request.parameters.clearanceMeters)
        && std::isfinite(request.parameters.minimumAltitudeM)
        && std::isfinite(request.parameters.maximumAltitudeM);
}

struct RgbaColor
{
    quint8 red{0};
    quint8 green{0};
    quint8 blue{0};
    quint8 alpha{0};
};

RgbaColor palette(int index)
{
    if (index <= 0 || index >= 255) {
        return {};
    }

    // Keep MP10's float arithmetic and truncating byte conversion so the
    // generated overlay colors remain stable across ports.
    const float progress = 1.0F - index / 256.0F;
    const float division = std::abs(std::fmod(progress, 1.0F)) * 5.0F;
    const quint8 ascending = static_cast<quint8>(
        std::fmod(division, 1.0F) * 255.0F);
    const quint8 descending = static_cast<quint8>(255 - ascending);
    switch (static_cast<int>(division)) {
    case 0:
        return {255, ascending, 0, 100};
    case 1:
        return {descending, 255, 0, 100};
    case 2:
        return {0, 255, ascending, 100};
    case 3:
        return {0, descending, 255, 100};
    case 4:
        return {ascending, 0, 255, 100};
    default:
        return {255, 0, descending, 100};
    }
}

struct TraceResult
{
    GeoPoint point;
    bool missing{false};
    bool cancelled{false};
};

TraceResult traceRay(const GeoPoint &home,
                     double bearing,
                     double rangeMeters,
                     double startAltitude,
                     double ceiling,
                     double convergenceDegrees,
                     const TerrainProvider &terrainProvider,
                     const CancellationCheck &isCancelled)
{
    const GeoPoint endpoint = newPosition(home, bearing, rangeMeters);
    double minimumAngle = 0.0;
    double maximumAngle = Pi / 2.0;
    double angle = Pi / 4.0;
    GeoPoint answer = endpoint;

    // MP10 samples every 10 m at normal ranges and caps extreme ranges so a
    // worker remains cancellable. Avoid converting an oversized double to int
    // before applying the same 20,000-segment cap.
    const double requestedSegments = std::ceil(
        rangeMeters / PathSampleSpacingMeters);
    const int pathSegments = requestedSegments >= MaximumPathSegments
        ? MaximumPathSegments
        : std::max(1, static_cast<int>(requestedSegments));

    for (int iteration = 0;
         iteration < MaximumRayIterations
             && minimumAngle + radians(convergenceDegrees) < maximumAngle;
         ++iteration) {
        if (cancellationRequested(isCancelled)) {
            return {GeoPoint{}, false, true};
        }

        bool hit = false;
        bool moveUp = false;
        TerrainSample lastSample = TerrainSample::missing();
        double lastLineAltitude = startAltitude;
        double lastDistance = 0.0;

        for (int index = 0; index <= pathSegments; ++index) {
            if ((index & 127) == 0
                && cancellationRequested(isCancelled)) {
                return {GeoPoint{}, false, true};
            }

            const double fraction = index / double(pathSegments);
            // Preserve MP10/SightGen's path interpolation exactly. A future
            // reference change may replace this chord with geodesic
            // interpolation for contours that cross the antimeridian.
            const double latitude = home.latitude
                + (endpoint.latitude - home.latitude) * fraction;
            const double longitude = home.longitude
                + (endpoint.longitude - home.longitude) * fraction;
            const TerrainSample sample = terrainProvider(latitude, longitude);
            if (sample.kind == TerrainKind::Missing) {
                return {answer, true, false};
            }

            const double terrainAltitude = sample.kind == TerrainKind::Ocean
                ? 0.0 : sample.altitudeM;
            const double distance = haversineMeters(
                home, {latitude, longitude, 0.0});
            const double lineAltitude = startAltitude
                + distance * std::tan(angle);
            answer = {latitude, longitude, terrainAltitude};
            lastSample = sample;
            lastLineAltitude = lineAltitude;
            lastDistance = distance;
            if (terrainAltitude >= lineAltitude
                || terrainAltitude >= ceiling) {
                break;
            }
        }

        const double lastTerrain = lastSample.kind == TerrainKind::Ocean
            ? 0.0 : lastSample.altitudeM;
        if (std::abs(lastTerrain - lastLineAltitude) <= 0.1
            && std::abs(lastTerrain - ceiling) <= 0.1) {
            hit = true;
        } else {
            // This is SightGen's branch decision: raise the ray only when the
            // terrain is above both the current ray and aircraft ceiling.
            moveUp = lastTerrain > lastLineAltitude
                && lastTerrain > ceiling;
        }

        if (hit) {
            break;
        }
        if (moveUp) {
            minimumAngle = angle;
        } else {
            maximumAngle = angle;
        }
        angle = (maximumAngle + minimumAngle) / 2.0;

        if (lastDistance >= rangeMeters
            && maximumAngle - minimumAngle <= 1.0e-12) {
            break;
        }
    }
    return {answer, false, false};
}

} // namespace

TerrainSample TerrainSample::valid(double altitudeM)
{
    return {TerrainKind::Valid, altitudeM};
}

TerrainSample TerrainSample::ocean()
{
    return {TerrainKind::Ocean, 0.0};
}

TerrainSample TerrainSample::missing()
{
    return {TerrainKind::Missing, 0.0};
}

bool GeoPoint::hasValidCoordinates() const
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0;
}

bool WebMercatorExtent::isValid() const
{
    return std::isfinite(minimumX) && std::isfinite(minimumY)
        && std::isfinite(maximumX) && std::isfinite(maximumY)
        && maximumX > minimumX && maximumY > minimumY;
}

bool CoverageGeometry::complete() const
{
    return status == BuildStatus::Finished
        && missingSampleCount == 0 && points.size() >= 4;
}

bool RasterGeometry::complete() const
{
    const qint64 expectedBytes = qint64(width) * height * 4;
    return status == BuildStatus::Finished && width > 0 && height > 0
        && expectedBytes == rgba.size();
}

CoverageGeometry buildRfCoverage(
    const CoverageRequest &request,
    const TerrainProvider &terrainProvider,
    const CancellationCheck &isCancelled)
{
    CoverageGeometry result;
    if (!finiteRfInput(request)) {
        result.status = BuildStatus::InvalidInput;
        return result;
    }

    const double rangeMeters = std::max(
        0.0, request.parameters.rangeKilometers) * 1000.0;
    if (rangeMeters <= 0.0) {
        // MP10 treats a non-positive range as a valid empty result.
        return result;
    }
    if (!terrainProvider) {
        result.status = BuildStatus::InvalidTerrainProvider;
        return result;
    }
    if (cancellationRequested(isCancelled)) {
        result.status = BuildStatus::Cancelled;
        return result;
    }

    const double azimuthStep = std::clamp(
        request.parameters.azimuthStepDegrees,
        MinimumAzimuthStepDegrees, MaximumAzimuthStepDegrees);
    const int rayCount = std::max(
        1, static_cast<int>(std::ceil(360.0 / azimuthStep)));
    const double convergence = std::max(
        MinimumConvergenceDegrees,
        request.parameters.convergenceDegrees);
    const double baseHeight = std::max(
        0.0, request.parameters.baseHeightMeters);
    const double clearance = std::max(
        0.0, request.parameters.clearanceMeters);

    double ceiling = request.droneAltitudeAmsl;
    if (!std::isfinite(ceiling) || ceiling == 0.0) {
        ceiling = request.home.altitudeM
            + std::max(baseHeight, clearance) + 0.1;
    }
    ceiling -= clearance;

    QVector<GeoPoint> rayPoints(rayCount);
    int missingCount = 0;
    for (int ray = 0; ray < rayCount; ++ray) {
        if (cancellationRequested(isCancelled)) {
            result.status = BuildStatus::Cancelled;
            return result;
        }
        double bearing = ray * azimuthStep;
        if (bearing >= 360.0) {
            bearing = std::nextafter(360.0, 0.0);
        }
        const TraceResult traced = traceRay(
            request.home, bearing, rangeMeters,
            request.home.altitudeM + baseHeight, ceiling, convergence,
            terrainProvider, isCancelled);
        if (traced.cancelled) {
            result.status = BuildStatus::Cancelled;
            return result;
        }
        rayPoints[ray] = traced.point;
        if (traced.missing) {
            ++missingCount;
        }
    }

    result.missingSampleCount = missingCount;
    if (missingCount != 0) {
        // Match MP10 fail-closed behaviour: do not bridge missing sectors with
        // a polygon that appears to be verified coverage.
        return result;
    }

    result.points.reserve(rayPoints.size() + 1);
    bool hasPrevious = false;
    GeoPoint previous;
    for (const GeoPoint &point : rayPoints) {
        if (hasPrevious && haversineMeters(previous, point) < 0.01) {
            continue;
        }
        result.points.append(point);
        previous = point;
        hasPrevious = true;
    }
    if (result.points.size() >= 3) {
        result.points.append(result.points.first());
    }
    return result;
}

RasterGeometry buildRaster(
    const RasterRequest &request,
    const TerrainProvider &terrainProvider,
    const CancellationCheck &isCancelled)
{
    RasterGeometry result;
    result.extent = request.extent;
    if (!finiteRasterInput(request)) {
        result.status = BuildStatus::InvalidInput;
        return result;
    }

    result.width = std::clamp(request.width, 1, MaximumRasterDimension);
    result.height = std::clamp(request.height, 1, MaximumRasterDimension);
    const int resolution = std::clamp(
        request.parameters.resolutionPixels, 1, MaximumResolutionPixels);
    if (!terrainProvider) {
        result.status = BuildStatus::InvalidTerrainProvider;
        return result;
    }

    const int columns = (result.width + resolution - 1) / resolution;
    const int rows = (result.height + resolution - 1) / resolution;
    QVector<TerrainSample> samples(columns * rows);
    for (int row = 0; row < rows; ++row) {
        if (cancellationRequested(isCancelled)) {
            result.status = BuildStatus::Cancelled;
            return result;
        }
        const double pixelY = std::min(
            result.height - 0.5,
            row * resolution + resolution / 2.0);
        const double worldY = request.extent.maximumY
            - pixelY / result.height * request.extent.height();
        for (int column = 0; column < columns; ++column) {
            const double pixelX = std::min(
                result.width - 0.5,
                column * resolution + resolution / 2.0);
            const double worldX = request.extent.minimumX
                + pixelX / result.width * request.extent.width();
            const GeoPoint point = webMercatorToGeoPoint(worldX, worldY);
            samples[row * columns + column] = terrainProvider(
                point.latitude, point.longitude);
        }
    }

    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    for (const TerrainSample &sample : samples) {
        // Match GMapMarkerElevation: ocean, unavailable terrain and an exact
        // zero-metre valid sample are transparent. Ocean remains known terrain
        // for RF tracing and therefore is not counted as missing here.
        if (sample.kind != TerrainKind::Valid || sample.altitudeM == 0.0) {
            if (sample.kind == TerrainKind::Missing) {
                ++result.missingSampleCount;
            }
            continue;
        }
        minimum = std::min(minimum, sample.altitudeM);
        maximum = std::max(maximum, sample.altitudeM);
        ++result.validSampleCount;
    }

    if (request.parameters.manualAltitudeRange) {
        minimum = std::min(request.parameters.minimumAltitudeM,
                           request.parameters.maximumAltitudeM);
        maximum = std::max(request.parameters.minimumAltitudeM,
                           request.parameters.maximumAltitudeM);
    } else if (result.validSampleCount == 0) {
        minimum = 0.0;
        maximum = 0.0;
    }
    result.minimumAltitudeM = minimum;
    result.maximumAltitudeM = maximum;

    result.rgba.fill(0, result.width * result.height * 4);
    for (int row = 0; row < rows; ++row) {
        if (cancellationRequested(isCancelled)) {
            result.rgba.clear();
            result.status = BuildStatus::Cancelled;
            return result;
        }
        for (int column = 0; column < columns; ++column) {
            const TerrainSample &sample = samples.at(
                row * columns + column);
            if (sample.kind != TerrainKind::Valid
                || sample.altitudeM == 0.0) {
                continue;
            }

            int paletteIndex = 0;
            if (request.parameters.elevationMap) {
                paletteIndex = elevationPaletteIndex(
                    request.vehicleAltitudeAmsl, sample.altitudeM,
                    request.parameters.clearanceMeters);
            } else {
                const double span = maximum - minimum;
                const double normalized = span
                        <= std::numeric_limits<double>::denorm_min()
                    ? 0.0
                    : std::clamp(
                        (sample.altitudeM - minimum) / span, 0.0, 1.0);
                paletteIndex = static_cast<int>(normalized * 255.0);
            }
            const RgbaColor color = palette(paletteIndex);
            const int x0 = column * resolution;
            const int y0 = row * resolution;
            const int x1 = std::min(result.width, x0 + resolution);
            const int y1 = std::min(result.height, y0 + resolution);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const int offset = (y * result.width + x) * 4;
                    result.rgba[offset] = color.red;
                    result.rgba[offset + 1] = color.green;
                    result.rgba[offset + 2] = color.blue;
                    result.rgba[offset + 3] = color.alpha;
                }
            }
        }
    }
    return result;
}

QVector<GeoPoint> buildCircle(const GeoPoint &center,
                              double radiusMeters,
                              int segments)
{
    if (!center.hasValidCoordinates() || !std::isfinite(radiusMeters)
        || radiusMeters <= 0.0) {
        return {};
    }

    segments = std::clamp(segments, 12, 720);
    QVector<GeoPoint> result(segments + 1);
    for (int index = 0; index < segments; ++index) {
        result[index] = newPosition(
            center, index * 360.0 / segments, radiusMeters);
    }
    result[segments] = result[0];
    return result;
}

DistanceRingGeometry buildDistanceRings(
    const GeoPoint &center,
    double batteryKilometersLeft,
    double tolerance,
    int segments)
{
    DistanceRingGeometry result;
    if (!std::isfinite(batteryKilometersLeft)
        || batteryKilometersLeft <= 0.0) {
        return result;
    }

    const double radiusMeters = batteryKilometersLeft * 1000.0;
    result.outer = buildCircle(center, radiusMeters, segments);
    if (std::isfinite(tolerance) && tolerance > 0.0) {
        result.tolerance = buildCircle(
            center, radiusMeters * tolerance, segments);
    }
    return result;
}

GeoPoint newPosition(const GeoPoint &origin,
                     double bearingDegrees,
                     double distanceMeters)
{
    if (!origin.hasValidCoordinates()
        || !std::isfinite(origin.altitudeM)
        || !std::isfinite(bearingDegrees)
        || !std::isfinite(distanceMeters)) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan};
    }

    const double angularDistance = distanceMeters / EarthRadiusMeters;
    const double bearing = radians(bearingDegrees);
    const double latitude1 = radians(origin.latitude);
    const double longitude1 = radians(origin.longitude);
    const double latitude2 = std::asin(
        std::sin(latitude1) * std::cos(angularDistance)
        + std::cos(latitude1) * std::sin(angularDistance)
            * std::cos(bearing));
    const double longitude2 = longitude1 + std::atan2(
        std::sin(bearing) * std::sin(angularDistance)
            * std::cos(latitude1),
        std::cos(angularDistance)
            - std::sin(latitude1) * std::sin(latitude2));
    const double wrappedLongitude = std::fmod(
        degrees(longitude2) + 540.0, 360.0) - 180.0;
    return {degrees(latitude2), wrappedLongitude, origin.altitudeM};
}

double haversineMeters(const GeoPoint &first, const GeoPoint &second)
{
    if (!first.hasValidCoordinates() || !second.hasValidCoordinates()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double latitude1 = radians(first.latitude);
    const double latitude2 = radians(second.latitude);
    const double latitudeDelta = latitude2 - latitude1;
    const double longitudeDelta = radians(
        second.longitude - first.longitude);
    const double haversine = std::pow(std::sin(latitudeDelta / 2.0), 2.0)
        + std::cos(latitude1) * std::cos(latitude2)
            * std::pow(std::sin(longitudeDelta / 2.0), 2.0);
    return 2.0 * EarthRadiusMeters * std::asin(
        std::min(1.0, std::sqrt(haversine)));
}

int elevationPaletteIndex(double vehicleAltitudeAmsl,
                          double terrainAltitudeM,
                          double clearanceMeters)
{
    const double clearance = std::max(0.001, clearanceMeters);
    const double normalized = std::clamp(
        (vehicleAltitudeAmsl - terrainAltitudeM) / clearance, 0.0, 1.0);
    return static_cast<int>((1.0 - normalized) * 254.0);
}

GeoPoint webMercatorToGeoPoint(double x, double y)
{
    if (!std::isfinite(x) || !std::isfinite(y)) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan};
    }
    const double longitude = degrees(x / EarthRadiusMeters);
    const double latitude = degrees(
        2.0 * std::atan(std::exp(y / EarthRadiusMeters)) - Pi / 2.0);
    return {latitude, longitude, 0.0};
}

} // namespace PropagationCore
