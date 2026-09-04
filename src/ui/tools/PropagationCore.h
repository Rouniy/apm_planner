#ifndef PROPAGATIONCORE_H
#define PROPAGATIONCORE_H

#include <QVector>

#include <functional>

/**
 * Pure terrain and geodesic calculations for Mission Planner RF propagation.
 *
 * The core owns no map, telemetry, settings or DEM objects. Callers pass an
 * immutable request plus a synchronous terrain provider that is safe to use
 * on the caller's worker thread.
 */
namespace PropagationCore
{
constexpr double EarthRadiusMeters = 6378137.0;

enum class TerrainKind
{
    Valid,
    Ocean,
    Missing
};

struct TerrainSample
{
    TerrainKind kind{TerrainKind::Missing};
    double altitudeM{0.0};

    static TerrainSample valid(double altitudeM);
    static TerrainSample ocean();
    static TerrainSample missing();
};

struct GeoPoint
{
    double latitude{0.0};
    double longitude{0.0};
    double altitudeM{0.0};

    /** Unlike vehicle-fix validation, 0,0 is a valid propagation coordinate. */
    bool hasValidCoordinates() const;
};

/**
 * Worker-safe propagation inputs. Persistence remains owned by
 * PropagationSettingsStore; the UI/controller must convert its settings
 * snapshot explicitly.
 */
struct Parameters
{
    double clearanceMeters{5.0};
    int resolutionPixels{4};
    double azimuthStepDegrees{1.0};
    double convergenceDegrees{1.0};
    double rangeKilometers{2.0};
    double baseHeightMeters{2.0};
    double minimumAltitudeM{100.0};
    double maximumAltitudeM{400.0};
    bool elevationMap{false};
    bool manualAltitudeRange{false};
};

struct WebMercatorExtent
{
    double minimumX{0.0};
    double minimumY{0.0};
    double maximumX{0.0};
    double maximumY{0.0};

    double width() const { return maximumX - minimumX; }
    double height() const { return maximumY - minimumY; }
    bool isValid() const;
};

struct RasterRequest
{
    WebMercatorExtent extent;
    int width{0};
    int height{0};
    double vehicleAltitudeAmsl{0.0};
    Parameters parameters;
};

struct CoverageRequest
{
    GeoPoint home;
    double droneAltitudeAmsl{0.0};
    Parameters parameters;
};

using TerrainProvider = std::function<TerrainSample(
    double latitude, double longitude)>;
using CancellationCheck = std::function<bool()>;

enum class BuildStatus
{
    Finished,
    InvalidInput,
    InvalidTerrainProvider,
    Cancelled
};

struct CoverageGeometry
{
    QVector<GeoPoint> points;
    /** MP10 counts unavailable rays, not every unavailable path sample. */
    int missingSampleCount{0};
    BuildStatus status{BuildStatus::Finished};

    bool complete() const;
};

struct RasterGeometry
{
    WebMercatorExtent extent;
    int width{0};
    int height{0};
    /** Row-major, north-at-top, non-premultiplied RGBA bytes. */
    QVector<quint8> rgba;
    double minimumAltitudeM{0.0};
    double maximumAltitudeM{0.0};
    int validSampleCount{0};
    int missingSampleCount{0};
    BuildStatus status{BuildStatus::Finished};

    bool complete() const;
};

struct DistanceRingGeometry
{
    /** Battery-distance ring, radius batteryKilometersLeft * 1000. */
    QVector<GeoPoint> outer;
    /** Optional tolerance ring, radius outer radius * tolerance. */
    QVector<GeoPoint> tolerance;

    bool isEmpty() const { return outer.isEmpty() && tolerance.isEmpty(); }
};

/**
 * Builds the MP10 360-degree terrain-intercept contour. Any missing terrain
 * ray suppresses the complete polygon so missing DEM is never presented as
 * verified RF coverage.
 */
CoverageGeometry buildRfCoverage(
    const CoverageRequest &request,
    const TerrainProvider &terrainProvider,
    const CancellationCheck &isCancelled = {});

/**
 * Samples an MP10/Mapsui Web Mercator extent at resolution-sized cell
 * centres and expands the cells into a north-at-top RGBA raster.
 */
RasterGeometry buildRaster(
    const RasterRequest &request,
    const TerrainProvider &terrainProvider,
    const CancellationCheck &isCancelled = {});

/** Returns a closed geodesic circle with segments clamped to [12, 720]. */
QVector<GeoPoint> buildCircle(const GeoPoint &center,
                              double radiusMeters,
                              int segments = 72);

/** Builds the red outer and optional orange tolerance rings used by MP10. */
DistanceRingGeometry buildDistanceRings(
    const GeoPoint &center,
    double batteryKilometersLeft,
    double tolerance,
    int segments = 72);

/** Returns a WGS84-sphere destination and wraps longitude to [-180, 180). */
GeoPoint newPosition(const GeoPoint &origin,
                     double bearingDegrees,
                     double distanceMeters);

double haversineMeters(const GeoPoint &first, const GeoPoint &second);

/** Maps an elevation clearance ratio to the MP10 rainbow palette index. */
int elevationPaletteIndex(double vehicleAltitudeAmsl,
                          double terrainAltitudeM,
                          double clearanceMeters);

/** Converts EPSG:3857 metres to longitude/latitude degrees. */
GeoPoint webMercatorToGeoPoint(double x, double y);

} // namespace PropagationCore

#endif // PROPAGATIONCORE_H
