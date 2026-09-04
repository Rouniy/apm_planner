#ifndef TERRAIN3DCORE_H
#define TERRAIN3DCORE_H

#include <QImage>
#include <QString>
#include <QVector>

#include <functional>

/**
 * Dependency-free geometry, terrain and software-rendering primitives for the
 * Mission Planner 10 3D Terrain workflow. The core deliberately owns neither
 * telemetry nor map/DEM services; callers provide an immutable snapshot and a
 * synchronous elevation sampler suitable for a worker thread.
 */
namespace Terrain3DCore
{
constexpr double EarthRadiusM = 6378137.0;
constexpr double FieldOfViewDeg = 90.0;
constexpr double NearPlaneM = 2.0;

constexpr int MinimumRenderWidth = 320;
constexpr int MaximumRenderWidth = 1280;
constexpr int MinimumRenderHeight = 240;
constexpr int MaximumRenderHeight = 800;

struct LocalPoint
{
    double eastM{0.0};
    double northM{0.0};
    double altitudeM{0.0};
};

struct GeoPoint
{
    double latitude{0.0};
    double longitude{0.0};
    double altitudeM{0.0};

    /** Rejects non-finite/out-of-range values and the uninitialised 0,0 fix. */
    bool isValid() const;

    /** Applies a local tangent-plane offset and wraps longitude at the dateline. */
    GeoPoint offset(double eastM, double northM,
                    double altitudeDeltaM = 0.0) const;

    /** Returns point relative to this origin in east/north/up metres. */
    LocalPoint toLocal(const GeoPoint &point) const;
};

struct Settings
{
    double rangeM{1500.0};
    int gridSize{33};
    int textureMinZoom{12};
    int textureMaxZoom{20};
    double verticalExaggeration{1.0};
    bool fogEnabled{true};
    bool imageryEnabled{true};

    Settings normalized() const;
};

struct Snapshot
{
    GeoPoint vehicle;
    double relativeAltitudeM{0.0};
    double rollDeg{0.0};
    double pitchDeg{0.0};
    double yawDeg{0.0};
    double velocityNorthMps{0.0};
    double velocityEastMps{0.0};
    /** Positive values move the vehicle upward, matching the MP10 core. */
    double velocityVerticalMps{0.0};
    /** Exact physical-target epoch used to reject stale worker results. */
    int linkId{-1};
    quint64 targetGeneration{0};
    /** Monotonic observation time, in the same epoch as the window clock. */
    qint64 capturedMonotonicMs{0};
    QString mode;
    bool armed{false};
    quint8 systemId{0};
    quint8 componentId{0};

    bool hasPosition() const { return vehicle.isValid(); }
};

struct Vertex
{
    double eastM{0.0};
    double northM{0.0};
    double altitudeM{0.0};
    double latitude{0.0};
    double longitude{0.0};
};

struct Mesh
{
    GeoPoint center;
    double rangeM{0.0};
    int gridSize{0};
    QVector<Vertex> vertices;
    double minimumAltitudeM{0.0};
    double maximumAltitudeM{0.0};
    double referenceAltitudeM{0.0};
    int missingSamples{0};

    bool isValid() const;

    /** Bilinear AMSL sample; returns NaN outside the square mesh. */
    double sampleAltitude(double eastM, double northM) const;
};

/** UI-facing name used by the Terrain3D window integration. */
using World = Mesh;

enum class ErrorCode
{
    None,
    InvalidSnapshot,
    InvalidElevationProvider,
    InvalidMesh,
    Cancelled
};

struct MeshBuildResult
{
    ErrorCode error{ErrorCode::None};
    QString message;
    Mesh mesh;

    bool ok() const { return error == ErrorCode::None; }
};

/** A non-finite return value means that the DEM sample is unavailable. */
using ElevationProvider = std::function<double(double latitude,
                                                double longitude)>;
using CancellationCheck = std::function<bool()>;

MeshBuildResult buildMesh(const Snapshot &snapshot,
                          const Settings &settings,
                          const ElevationProvider &elevation,
                          const CancellationCheck &isCancelled = {});

inline MeshBuildResult BuildWorld(
    const Snapshot &snapshot, const Settings &settings,
    const ElevationProvider &elevation,
    const CancellationCheck &isCancelled = {})
{
    return buildMesh(snapshot, settings, elevation, isCancelled);
}

enum class CameraMotion
{
    Forward,
    Backward,
    Left,
    Right,
    Up,
    Down,
    YawLeft,
    YawRight,
    PitchUp,
    PitchDown
};

struct Camera
{
    double eastM{0.0};
    double northM{0.0};
    double altitudeM{0.0};
    double yawDeg{0.0};
    double pitchDeg{0.0};
    double rollDeg{0.0};

    /** Extrapolates a locked snapshot by at most one second. */
    static Camera locked(const Mesh &mesh, const Snapshot &snapshot,
                         qint64 nowMonotonicMs);

    static Camera Locked(const World &world, const Snapshot &snapshot,
                         qint64 nowMonotonicMs)
    {
        return locked(world, snapshot, nowMonotonicMs);
    }

    Camera moved(CameraMotion motion, double amount = 10.0) const;

    Camera Move(CameraMotion motion, double amount = 10.0) const
    {
        return moved(motion, amount);
    }
};

struct Vector3
{
    double x{0.0};
    double y{0.0};
    double z{0.0};

    bool isFinite() const;
    double lengthSquared() const;
};

struct ProjectedPoint
{
    double x{0.0};
    double y{0.0};
    double depth{0.0};
    bool visible{false};
};

ProjectedPoint project(const Camera &camera, const Vector3 &world,
                       int width, int height);

/** Returns a normalized world-space ray, or false for invalid input. */
bool screenRay(const Camera &camera, double x, double y,
               int width, int height, Vector3 *ray);

inline bool ScreenRay(const Camera &camera, double x, double y,
                      int width, int height, Vector3 *ray)
{
    return screenRay(camera, x, y, width, height, ray);
}

/** Intersects the ray with the bilinear terrain surface. */
bool intersectTerrain(const Mesh &mesh, const Camera &camera,
                      const Vector3 &ray, GeoPoint *hit);

inline bool TryIntersect(const World &world, const Camera &camera,
                         const Vector3 &ray, GeoPoint *hit)
{
    return intersectTerrain(world, camera, ray, hit);
}

struct RenderResult
{
    ErrorCode error{ErrorCode::None};
    QString message;
    QImage image;
    QString details;
    int triangleCount{0};

    bool ok() const { return error == ErrorCode::None; }
};

/**
 * Produces a bounded ARGB32 frame with sky/horizon, far-to-near shaded terrain,
 * optional fog, a sparse grid, a vehicle marker when visible, and diagnostics.
 * Map imagery and vehicle commands intentionally remain outside this core.
 */
RenderResult render(const Mesh &mesh, const Snapshot &snapshot,
                    const Settings &settings, const Camera &camera,
                    int requestedWidth, int requestedHeight);

inline RenderResult Render(const World &world, const Snapshot &snapshot,
                           const Settings &settings, const Camera &camera,
                           int requestedWidth, int requestedHeight)
{
    return render(world, snapshot, settings, camera,
                  requestedWidth, requestedHeight);
}

} // namespace Terrain3DCore

#endif // TERRAIN3DCORE_H
