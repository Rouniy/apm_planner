#include "Terrain3DCore.h"

#include <QColor>
#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Terrain3DCore
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double radians(double degrees)
{
    return degrees * kPi / 180.0;
}

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

double clampDouble(double value, double minimum, double maximum)
{
    return std::max(minimum, std::min(maximum, value));
}

int clampInt(int value, int minimum, int maximum)
{
    return std::max(minimum, std::min(maximum, value));
}

double normalizedLongitude(double longitude)
{
    double wrapped = std::fmod(longitude + 180.0, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped - 180.0;
}

Vector3 add(const Vector3 &left, const Vector3 &right)
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vector3 subtract(const Vector3 &left, const Vector3 &right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vector3 multiply(const Vector3 &vector, double scalar)
{
    return {vector.x * scalar, vector.y * scalar, vector.z * scalar};
}

double dot(const Vector3 &left, const Vector3 &right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vector3 cross(const Vector3 &left, const Vector3 &right)
{
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

Vector3 normalized(const Vector3 &vector)
{
    const double magnitudeSquared = vector.lengthSquared();
    if (!vector.isFinite() || !std::isfinite(magnitudeSquared)
        || magnitudeSquared <= std::numeric_limits<double>::epsilon()) {
        return {};
    }
    return multiply(vector, 1.0 / std::sqrt(magnitudeSquared));
}

struct Basis
{
    Vector3 forward;
    Vector3 right;
    Vector3 up;
};

Basis cameraBasis(const Camera &camera)
{
    const double yaw = radians(finiteOr(camera.yawDeg, 0.0));
    const double pitch = radians(clampDouble(
        finiteOr(camera.pitchDeg, 0.0), -89.9, 89.9));
    const double roll = radians(finiteOr(camera.rollDeg, 0.0));

    const Vector3 forward = normalized(
        {std::sin(yaw) * std::cos(pitch),
         std::cos(yaw) * std::cos(pitch), std::sin(pitch)});
    Vector3 right = normalized(cross(forward, {0.0, 0.0, 1.0}));
    if (!right.isFinite() || right.lengthSquared() < 0.1) {
        right = {1.0, 0.0, 0.0};
    }
    const Vector3 up = normalized(cross(right, forward));
    const Vector3 rolledRight = normalized(add(
        multiply(right, std::cos(roll)), multiply(up, std::sin(roll))));
    const Vector3 rolledUp = normalized(subtract(
        multiply(up, std::cos(roll)), multiply(right, std::sin(roll))));
    return {forward, rolledRight, rolledUp};
}

double lerp(double from, double to, double amount)
{
    return from + (to - from) * amount;
}

QColor blend(const QColor &from, const QColor &to, double amount)
{
    amount = clampDouble(amount, 0.0, 1.0);
    return QColor(
        qRound(lerp(from.red(), to.red(), amount)),
        qRound(lerp(from.green(), to.green(), amount)),
        qRound(lerp(from.blue(), to.blue(), amount)));
}

QColor elevationColor(double altitude, double minimum, double maximum,
                      double brightness, double fog)
{
    const double ratio = maximum <= minimum
        ? 0.5
        : clampDouble((altitude - minimum) / (maximum - minimum), 0.0, 1.0);
    const QColor low(48, 105, 54);
    const QColor middle(122, 112, 69);
    const QColor high(196, 196, 185);
    const QColor base = ratio < 0.55
        ? blend(low, middle, ratio / 0.55)
        : blend(middle, high, (ratio - 0.55) / 0.45);
    const auto shade = [brightness, fog](int value) {
        return clampInt(qRound(value * brightness + fog * 60.0), 0, 255);
    };
    return QColor(shade(base.red()), shade(base.green()), shade(base.blue()),
                  clampInt(qRound(255.0 - fog * 70.0), 0, 255));
}

QString errorMessage(ErrorCode error)
{
    switch (error) {
    case ErrorCode::None:
        return QString();
    case ErrorCode::InvalidSnapshot:
        return QStringLiteral("A valid vehicle position is required.");
    case ErrorCode::InvalidElevationProvider:
        return QStringLiteral("A terrain elevation provider is required.");
    case ErrorCode::InvalidMesh:
        return QStringLiteral("The terrain mesh is invalid.");
    case ErrorCode::Cancelled:
        return QStringLiteral("Terrain construction was cancelled.");
    }
    return QStringLiteral("Terrain operation failed.");
}

struct Triangle
{
    int a{0};
    int b{0};
    int c{0};
    double depth{0.0};
    double brightness{1.0};
};

bool cancellationRequested(const CancellationCheck &check)
{
    return check && check();
}

} // namespace

bool GeoPoint::isValid() const
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && std::isfinite(altitudeM) && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0
        && (std::abs(latitude) > 1.0e-9 || std::abs(longitude) > 1.0e-9);
}

GeoPoint GeoPoint::offset(double eastM, double northM,
                          double altitudeDeltaM) const
{
    if (!isValid() || !std::isfinite(eastM) || !std::isfinite(northM)
        || !std::isfinite(altitudeDeltaM)) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan};
    }
    const double latitudeResult = latitude
        + northM / EarthRadiusM * 180.0 / kPi;
    const double cosine = std::max(1.0e-6, std::cos(radians(latitude)));
    const double longitudeResult = normalizedLongitude(
        longitude + eastM / (EarthRadiusM * cosine) * 180.0 / kPi);
    return {latitudeResult, longitudeResult, altitudeM + altitudeDeltaM};
}

LocalPoint GeoPoint::toLocal(const GeoPoint &point) const
{
    if (!isValid() || !point.isValid()) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan};
    }
    const double north = (point.latitude - latitude) * kPi / 180.0
        * EarthRadiusM;
    const double meanLatitude = radians((point.latitude + latitude) * 0.5);
    const double deltaLongitude = normalizedLongitude(
        point.longitude - longitude);
    const double east = deltaLongitude * kPi / 180.0 * EarthRadiusM
        * std::cos(meanLatitude);
    return {east, north, point.altitudeM - altitudeM};
}

Settings Settings::normalized() const
{
    const int minimumZoom = clampInt(textureMinZoom, 1, 20);
    const int maximumZoom = clampInt(textureMaxZoom, minimumZoom, 20);
    const int boundedGrid = clampInt(gridSize, 17, 65);
    const int gridStep = clampInt(
        qRound((boundedGrid - 17) / 8.0), 0, 6);
    const int normalizedGrid = 17 + gridStep * 8;
    return {clampDouble(finiteOr(rangeM, 1500.0), 250.0, 5000.0),
            normalizedGrid,
            minimumZoom,
            maximumZoom,
            clampDouble(finiteOr(verticalExaggeration, 1.0), 0.25, 8.0),
            fogEnabled,
            imageryEnabled};
}

bool Mesh::isValid() const
{
    const qint64 expected = static_cast<qint64>(gridSize) * gridSize;
    return center.isValid() && std::isfinite(rangeM)
        && rangeM >= 250.0 && rangeM <= 5000.0
        && gridSize >= 17 && gridSize <= 65 && (gridSize & 1) != 0
        && expected == vertices.size()
        && std::isfinite(minimumAltitudeM)
        && std::isfinite(maximumAltitudeM)
        && minimumAltitudeM <= maximumAltitudeM
        && std::isfinite(referenceAltitudeM)
        && missingSamples >= 0 && missingSamples <= vertices.size();
}

double Mesh::sampleAltitude(double eastM, double northM) const
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (!isValid() || !std::isfinite(eastM) || !std::isfinite(northM)) {
        return nan;
    }
    const double x = (eastM + rangeM) / (rangeM * 2.0) * (gridSize - 1);
    const double y = (northM + rangeM) / (rangeM * 2.0) * (gridSize - 1);
    if (x < 0.0 || y < 0.0 || x > gridSize - 1 || y > gridSize - 1) {
        return nan;
    }
    const int x0 = clampInt(static_cast<int>(std::floor(x)), 0, gridSize - 1);
    const int y0 = clampInt(static_cast<int>(std::floor(y)), 0, gridSize - 1);
    const int x1 = std::min(x0 + 1, gridSize - 1);
    const int y1 = std::min(y0 + 1, gridSize - 1);
    const double tx = x - x0;
    const double ty = y - y0;
    const double top = lerp(vertices.at(y0 * gridSize + x0).altitudeM,
                            vertices.at(y0 * gridSize + x1).altitudeM, tx);
    const double bottom = lerp(vertices.at(y1 * gridSize + x0).altitudeM,
                               vertices.at(y1 * gridSize + x1).altitudeM, tx);
    return lerp(top, bottom, ty);
}

MeshBuildResult buildMesh(const Snapshot &snapshot,
                          const Settings &settings,
                          const ElevationProvider &elevation,
                          const CancellationCheck &isCancelled)
{
    MeshBuildResult result;
    if (!snapshot.hasPosition()) {
        result.error = ErrorCode::InvalidSnapshot;
        result.message = errorMessage(result.error);
        return result;
    }
    if (!elevation) {
        result.error = ErrorCode::InvalidElevationProvider;
        result.message = errorMessage(result.error);
        return result;
    }
    if (cancellationRequested(isCancelled)) {
        result.error = ErrorCode::Cancelled;
        result.message = errorMessage(result.error);
        return result;
    }

    const Settings normalizedSettings = settings.normalized();
    const int size = normalizedSettings.gridSize;
    const double spacing = normalizedSettings.rangeM * 2.0 / (size - 1);
    double fallback = std::isfinite(snapshot.relativeAltitudeM)
        ? snapshot.vehicle.altitudeM - snapshot.relativeAltitudeM
        : snapshot.vehicle.altitudeM;
    fallback = finiteOr(fallback, 0.0);

    Mesh mesh;
    mesh.center = snapshot.vehicle;
    mesh.rangeM = normalizedSettings.rangeM;
    mesh.gridSize = size;
    mesh.vertices.resize(size * size);
    QVector<double> rawAltitudes(size * size,
                                 std::numeric_limits<double>::quiet_NaN());
    QVector<double> validAltitudes;
    validAltitudes.reserve(size * size);

    for (int row = 0; row < size; ++row) {
        if (cancellationRequested(isCancelled)) {
            result.error = ErrorCode::Cancelled;
            result.message = errorMessage(result.error);
            return result;
        }
        const double north = -mesh.rangeM + row * spacing;
        for (int column = 0; column < size; ++column) {
            if (cancellationRequested(isCancelled)) {
                result.error = ErrorCode::Cancelled;
                result.message = errorMessage(result.error);
                return result;
            }
            const double east = -mesh.rangeM + column * spacing;
            const GeoPoint geo = mesh.center.offset(east, north);
            const int index = row * size + column;
            const double altitude = elevation(geo.latitude, geo.longitude);
            if (std::isfinite(altitude)) {
                rawAltitudes[index] = altitude;
                validAltitudes.append(altitude);
            } else {
                ++mesh.missingSamples;
            }
            mesh.vertices[index] = {east, north, 0.0,
                                    geo.latitude, geo.longitude};
        }
    }

    double replacement = fallback;
    if (!validAltitudes.isEmpty()) {
        std::sort(validAltitudes.begin(), validAltitudes.end());
        replacement = validAltitudes.at(validAltitudes.size() / 2);
    }
    mesh.referenceAltitudeM = replacement;
    mesh.minimumAltitudeM = std::numeric_limits<double>::infinity();
    mesh.maximumAltitudeM = -std::numeric_limits<double>::infinity();
    for (int index = 0; index < mesh.vertices.size(); ++index) {
        const double altitude = std::isfinite(rawAltitudes.at(index))
            ? rawAltitudes.at(index) : replacement;
        mesh.vertices[index].altitudeM = altitude;
        mesh.minimumAltitudeM = std::min(mesh.minimumAltitudeM, altitude);
        mesh.maximumAltitudeM = std::max(mesh.maximumAltitudeM, altitude);
    }

    if (!mesh.isValid()) {
        result.error = ErrorCode::InvalidMesh;
        result.message = errorMessage(result.error);
        return result;
    }
    result.mesh = std::move(mesh);
    return result;
}

Camera Camera::locked(const Mesh &mesh, const Snapshot &snapshot,
                      qint64 nowMonotonicMs)
{
    const LocalPoint position = mesh.center.toLocal(snapshot.vehicle);
    const double elapsed = clampDouble(
        (nowMonotonicMs - snapshot.capturedMonotonicMs) / 1000.0,
        0.0, 1.0);
    const double velocityEast = finiteOr(snapshot.velocityEastMps, 0.0);
    const double velocityNorth = finiteOr(snapshot.velocityNorthMps, 0.0);
    const double velocityVertical = finiteOr(snapshot.velocityVerticalMps, 0.0);
    const double east = finiteOr(position.eastM, 0.0) + velocityEast * elapsed;
    const double north = finiteOr(position.northM, 0.0) + velocityNorth * elapsed;
    double altitude = snapshot.vehicle.altitudeM + velocityVertical * elapsed;
    const double terrain = mesh.sampleAltitude(east, north);
    if (std::isfinite(terrain)) {
        altitude = std::max(altitude, terrain + 1.0);
    }
    return {east, north, altitude,
            finiteOr(snapshot.yawDeg, 0.0),
            clampDouble(finiteOr(snapshot.pitchDeg, 0.0), -85.0, 85.0),
            finiteOr(snapshot.rollDeg, 0.0)};
}

Camera Camera::moved(CameraMotion motion, double amount) const
{
    if (!std::isfinite(amount)) {
        return *this;
    }
    Camera result = *this;
    const double yaw = radians(finiteOr(yawDeg, 0.0));
    const double eastForward = std::sin(yaw);
    const double northForward = std::cos(yaw);
    switch (motion) {
    case CameraMotion::Forward:
        result.eastM += eastForward * amount;
        result.northM += northForward * amount;
        break;
    case CameraMotion::Backward:
        result.eastM -= eastForward * amount;
        result.northM -= northForward * amount;
        break;
    case CameraMotion::Left:
        result.eastM -= northForward * amount;
        result.northM += eastForward * amount;
        break;
    case CameraMotion::Right:
        result.eastM += northForward * amount;
        result.northM -= eastForward * amount;
        break;
    case CameraMotion::Up:
        result.altitudeM += amount;
        break;
    case CameraMotion::Down:
        result.altitudeM -= amount;
        break;
    case CameraMotion::YawLeft:
        result.yawDeg = normalizedLongitude(result.yawDeg - amount);
        if (result.yawDeg < 0.0) {
            result.yawDeg += 360.0;
        }
        break;
    case CameraMotion::YawRight:
        result.yawDeg = normalizedLongitude(result.yawDeg + amount);
        if (result.yawDeg < 0.0) {
            result.yawDeg += 360.0;
        }
        break;
    case CameraMotion::PitchUp:
        result.pitchDeg = clampDouble(result.pitchDeg + amount, -85.0, 85.0);
        break;
    case CameraMotion::PitchDown:
        result.pitchDeg = clampDouble(result.pitchDeg - amount, -85.0, 85.0);
        break;
    }
    return result;
}

bool Vector3::isFinite() const
{
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

double Vector3::lengthSquared() const
{
    return x * x + y * y + z * z;
}

ProjectedPoint project(const Camera &camera, const Vector3 &world,
                       int width, int height)
{
    const Basis basis = cameraBasis(camera);
    const Vector3 origin{camera.eastM, camera.northM, camera.altitudeM};
    const Vector3 delta = subtract(world, origin);
    const double viewX = dot(delta, basis.right);
    const double viewY = dot(delta, basis.up);
    const double depth = dot(delta, basis.forward);
    if (!std::isfinite(depth) || depth <= NearPlaneM
        || width <= 0 || height <= 0) {
        return {0.0, 0.0, depth, false};
    }
    const double focal = height
        / (2.0 * std::tan(radians(FieldOfViewDeg) * 0.5));
    const double x = width * 0.5 + viewX / depth * focal;
    const double y = height * 0.5 - viewY / depth * focal;
    const bool visible = std::isfinite(x) && std::isfinite(y)
        && x >= -width * 2.0 && x <= width * 3.0
        && y >= -height * 2.0 && y <= height * 3.0;
    return {x, y, depth, visible};
}

bool screenRay(const Camera &camera, double x, double y,
               int width, int height, Vector3 *ray)
{
    if (!ray || width <= 0 || height <= 0 || !std::isfinite(x)
        || !std::isfinite(y)) {
        return false;
    }
    const Basis basis = cameraBasis(camera);
    const double tangent = std::tan(radians(FieldOfViewDeg) * 0.5);
    const double normalizedX = (x / width * 2.0 - 1.0)
        * (width / static_cast<double>(height)) * tangent;
    const double normalizedY = (1.0 - y / height * 2.0) * tangent;
    const Vector3 result = normalized(add(
        basis.forward,
        add(multiply(basis.right, normalizedX),
            multiply(basis.up, normalizedY))));
    if (!result.isFinite() || result.lengthSquared() < 0.5) {
        return false;
    }
    *ray = result;
    return true;
}

bool intersectTerrain(const Mesh &mesh, const Camera &camera,
                      const Vector3 &inputRay, GeoPoint *hit)
{
    if (hit) {
        *hit = {};
    }
    if (!hit || !mesh.isValid() || !inputRay.isFinite()
        || inputRay.lengthSquared() < 0.5) {
        return false;
    }
    const Vector3 ray = normalized(inputRay);
    const double step = std::max(2.0, mesh.rangeM / (mesh.gridSize - 1));
    const double maximumDistance = mesh.rangeM * 3.0;

    const auto clearanceAt = [&mesh, &camera, &ray](
        double distance, double *east, double *north, double *terrain) {
        *east = camera.eastM + ray.x * distance;
        *north = camera.northM + ray.y * distance;
        *terrain = mesh.sampleAltitude(*east, *north);
        const double altitude = camera.altitudeM + ray.z * distance;
        return std::isfinite(*terrain)
            ? altitude - *terrain
            : std::numeric_limits<double>::quiet_NaN();
    };

    double previousDistance = NearPlaneM;
    double previousEast = 0.0;
    double previousNorth = 0.0;
    double previousTerrain = 0.0;
    double previousClearance = clearanceAt(
        previousDistance, &previousEast, &previousNorth, &previousTerrain);
    bool enteredWorld = false;
    for (double distance = previousDistance + step;
         distance <= maximumDistance; distance += step) {
        double east = 0.0;
        double north = 0.0;
        double terrain = 0.0;
        const double clearance = clearanceAt(
            distance, &east, &north, &terrain);
        const bool inside = std::abs(east) <= mesh.rangeM
            && std::abs(north) <= mesh.rangeM;
        if (!inside) {
            if (enteredWorld) {
                break;
            }
            previousDistance = distance;
            previousClearance = clearance;
            continue;
        }
        enteredWorld = true;
        if (!std::isfinite(clearance)) {
            previousDistance = distance;
            previousClearance = clearance;
            continue;
        }
        if (clearance <= 0.0 && std::isfinite(previousClearance)
            && previousClearance > 0.0) {
            const double fraction = previousClearance
                / (previousClearance - clearance);
            const double resolvedDistance = previousDistance
                + (distance - previousDistance) * fraction;
            clearanceAt(resolvedDistance, &east, &north, &terrain);
            GeoPoint location = mesh.center.offset(
                east, north, terrain - mesh.center.altitudeM);
            location.altitudeM = terrain;
            if (!location.isValid()) {
                return false;
            }
            *hit = location;
            return true;
        }
        previousDistance = distance;
        previousClearance = clearance;
    }
    return false;
}

RenderResult render(const Mesh &mesh, const Snapshot &snapshot,
                    const Settings &settings, const Camera &camera,
                    int requestedWidth, int requestedHeight)
{
    RenderResult result;
    if (!mesh.isValid() || !snapshot.hasPosition()) {
        result.error = ErrorCode::InvalidMesh;
        result.message = errorMessage(result.error);
        return result;
    }
    const Settings normalizedSettings = settings.normalized();
    const int width = clampInt(requestedWidth,
                               MinimumRenderWidth, MaximumRenderWidth);
    const int height = clampInt(requestedHeight,
                                MinimumRenderHeight, MaximumRenderHeight);
    result.image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    if (result.image.isNull()) {
        result.error = ErrorCode::InvalidMesh;
        result.message = QStringLiteral("Unable to allocate the terrain frame.");
        return result;
    }

    QPainter painter(&result.image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient sky(0.0, 0.0, 0.0, height);
    sky.setColorAt(0.0, QColor(47, 104, 164));
    sky.setColorAt(0.65, QColor(157, 196, 221));
    sky.setColorAt(1.0, QColor(203, 214, 198));
    painter.fillRect(result.image.rect(), sky);

    painter.save();
    QPen horizon(QColor(255, 255, 255, 70));
    horizon.setWidthF(1.0);
    painter.setPen(horizon);
    const double roll = radians(finiteOr(camera.rollDeg, 0.0));
    const double half = width;
    painter.drawLine(
        QPointF(width * 0.5 - std::cos(roll) * half,
                height * 0.5 - std::sin(roll) * half),
        QPointF(width * 0.5 + std::cos(roll) * half,
                height * 0.5 + std::sin(roll) * half));
    painter.restore();

    QVector<ProjectedPoint> projected;
    projected.resize(mesh.vertices.size());
    for (int index = 0; index < mesh.vertices.size(); ++index) {
        const Vertex &vertex = mesh.vertices.at(index);
        const double altitude = mesh.referenceAltitudeM
            + (vertex.altitudeM - mesh.referenceAltitudeM)
                * normalizedSettings.verticalExaggeration;
        projected[index] = project(camera,
                                   {vertex.eastM, vertex.northM, altitude},
                                   width, height);
    }

    QVector<Triangle> triangles;
    triangles.reserve((mesh.gridSize - 1) * (mesh.gridSize - 1) * 2);
    const Vector3 light = normalized({-0.35, -0.25, 1.0});
    const auto addTriangle = [&triangles, &projected, &mesh, &light](
        int a, int b, int c) {
        if (!projected.at(a).visible || !projected.at(b).visible
            || !projected.at(c).visible) {
            return;
        }
        const Vertex &va = mesh.vertices.at(a);
        const Vertex &vb = mesh.vertices.at(b);
        const Vertex &vc = mesh.vertices.at(c);
        const Vector3 edge1{vb.eastM - va.eastM,
                            vb.northM - va.northM,
                            vb.altitudeM - va.altitudeM};
        const Vector3 edge2{vc.eastM - va.eastM,
                            vc.northM - va.northM,
                            vc.altitudeM - va.altitudeM};
        Vector3 normal = normalized(cross(edge1, edge2));
        if (normal.z < 0.0) {
            normal = multiply(normal, -1.0);
        }
        const double illumination = clampDouble(dot(normal, light), 0.0, 1.0);
        triangles.append({a, b, c,
                          (projected.at(a).depth + projected.at(b).depth
                           + projected.at(c).depth) / 3.0,
                          0.58 + illumination * 0.42});
    };
    for (int row = 0; row < mesh.gridSize - 1; ++row) {
        for (int column = 0; column < mesh.gridSize - 1; ++column) {
            const int a = row * mesh.gridSize + column;
            const int b = a + 1;
            const int c = a + mesh.gridSize;
            const int d = c + 1;
            addTriangle(a, c, d);
            addTriangle(a, d, b);
        }
    }
    std::sort(triangles.begin(), triangles.end(),
              [](const Triangle &left, const Triangle &right) {
        return left.depth > right.depth;
    });

    painter.setPen(Qt::NoPen);
    for (const Triangle &triangle : triangles) {
        const Vertex &a = mesh.vertices.at(triangle.a);
        const Vertex &b = mesh.vertices.at(triangle.b);
        const Vertex &c = mesh.vertices.at(triangle.c);
        const double altitude = (a.altitudeM + b.altitudeM + c.altitudeM) / 3.0;
        const double fog = normalizedSettings.fogEnabled
            ? clampDouble((triangle.depth - mesh.rangeM * 0.25)
                              / (mesh.rangeM * 1.8),
                          0.0, 0.82)
            : 0.0;
        painter.setBrush(elevationColor(
            altitude, mesh.minimumAltitudeM, mesh.maximumAltitudeM,
            triangle.brightness, fog));
        QPolygonF polygon;
        polygon << QPointF(projected.at(triangle.a).x,
                           projected.at(triangle.a).y)
                << QPointF(projected.at(triangle.b).x,
                           projected.at(triangle.b).y)
                << QPointF(projected.at(triangle.c).x,
                           projected.at(triangle.c).y);
        painter.drawPolygon(polygon);
    }
    result.triangleCount = triangles.size();

    painter.save();
    QPen gridPen(QColor(255, 255, 255, 35));
    gridPen.setWidthF(1.0);
    painter.setPen(gridPen);
    painter.setBrush(Qt::NoBrush);
    const auto drawGridLine = [&painter, &projected](const QVector<int> &indices) {
        QPainterPath path;
        bool open = false;
        for (int index : indices) {
            const ProjectedPoint &point = projected.at(index);
            if (!point.visible) {
                open = false;
                continue;
            }
            if (!open) {
                path.moveTo(point.x, point.y);
                open = true;
            } else {
                path.lineTo(point.x, point.y);
            }
        }
        painter.drawPath(path);
    };
    const int gridStep = std::max(2, mesh.gridSize / 8);
    for (int row = 0; row < mesh.gridSize; row += gridStep) {
        QVector<int> indices;
        indices.reserve(mesh.gridSize);
        for (int column = 0; column < mesh.gridSize; ++column) {
            indices.append(row * mesh.gridSize + column);
        }
        drawGridLine(indices);
    }
    for (int column = 0; column < mesh.gridSize; column += gridStep) {
        QVector<int> indices;
        indices.reserve(mesh.gridSize);
        for (int row = 0; row < mesh.gridSize; ++row) {
            indices.append(row * mesh.gridSize + column);
        }
        drawGridLine(indices);
    }
    painter.restore();

    const LocalPoint vehicleLocal = mesh.center.toLocal(snapshot.vehicle);
    const double vehicleAltitude = mesh.referenceAltitudeM
        + (snapshot.vehicle.altitudeM - mesh.referenceAltitudeM)
            * normalizedSettings.verticalExaggeration;
    const ProjectedPoint vehicle = project(
        camera, {vehicleLocal.eastM, vehicleLocal.northM, vehicleAltitude},
        width, height);
    if (vehicle.visible && vehicle.x >= -30.0 && vehicle.x <= width + 30.0
        && vehicle.y >= -30.0 && vehicle.y <= height + 30.0) {
        QPen markerOutline(Qt::black);
        markerOutline.setWidthF(2.0);
        painter.setPen(markerOutline);
        painter.setBrush(QColor(50, 205, 50));
        painter.drawEllipse(QPointF(vehicle.x, vehicle.y), 8.0, 8.0);
        painter.setPen(Qt::white);
        painter.drawText(QPointF(vehicle.x + 12.0, vehicle.y - 8.0),
                         QStringLiteral("MAV"));
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 145));
    painter.drawRoundedRect(QRectF(12.0, 12.0,
                                   std::min(390.0, width - 24.0), 82.0),
                            6.0, 6.0);
    painter.setPen(Qt::white);
    QFont overlayFont = painter.font();
    overlayFont.setPixelSize(13);
    painter.setFont(overlayFont);
    const QString state = snapshot.armed
        ? QStringLiteral("ARMED") : QStringLiteral("DISARMED");
    painter.drawText(QPointF(22.0, 34.0),
                     QStringLiteral("SYS %1:%2  %3  %4")
                         .arg(snapshot.systemId).arg(snapshot.componentId)
                         .arg(snapshot.mode, state));
    painter.drawText(QPointF(22.0, 56.0),
                     QStringLiteral("%1, %2  AMSL %3 m")
                         .arg(snapshot.vehicle.latitude, 0, 'f', 6)
                         .arg(snapshot.vehicle.longitude, 0, 'f', 6)
                         .arg(snapshot.vehicle.altitudeM, 0, 'f', 1));
    painter.drawText(QPointF(22.0, 78.0),
                     QStringLiteral("HDG %1  P %2  R %3  terrain %4..%5 m")
                         .arg(camera.yawDeg, 3, 'f', 0, QLatin1Char('0'))
                         .arg(camera.pitchDeg, 0, 'f', 1)
                         .arg(camera.rollDeg, 0, 'f', 1)
                         .arg(mesh.minimumAltitudeM, 0, 'f', 0)
                         .arg(mesh.maximumAltitudeM, 0, 'f', 0));

    QPen reticle(QColor(255, 255, 255, 190));
    reticle.setWidthF(1.5);
    painter.setPen(reticle);
    painter.drawLine(QPointF(width * 0.5 - 10.0, height * 0.5),
                     QPointF(width * 0.5 + 10.0, height * 0.5));
    painter.drawLine(QPointF(width * 0.5, height * 0.5 - 10.0),
                     QPointF(width * 0.5, height * 0.5 + 10.0));
    painter.end();

    result.details = QStringLiteral("%1×%2 mesh · elevation shading · "
                                    "DEM missing %3/%4 · triangles %5")
                         .arg(mesh.gridSize).arg(mesh.gridSize)
                         .arg(mesh.missingSamples).arg(mesh.vertices.size())
                         .arg(result.triangleCount);
    return result;
}

} // namespace Terrain3DCore
