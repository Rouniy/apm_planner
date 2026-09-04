#include "ui/terrain/Terrain3DCore.h"

#include <QtTest>

#include <QSet>

#include <cmath>
#include <limits>

namespace
{
using namespace Terrain3DCore;

Snapshot snapshot(double altitude = 100.0,
                  double relativeAltitude = 30.0,
                  qint64 capturedMonotonicMs = 1000)
{
    Snapshot result;
    result.vehicle = {35.1856, 33.3823, altitude};
    result.relativeAltitudeM = relativeAltitude;
    result.linkId = 7;
    result.targetGeneration = 3;
    result.capturedMonotonicMs = capturedMonotonicMs;
    result.mode = QStringLiteral("LOITER");
    result.systemId = 1;
    result.componentId = 1;
    return result;
}

Settings settings(double range = 500.0, int grid = 17)
{
    Settings result;
    result.rangeM = range;
    result.gridSize = grid;
    result.imageryEnabled = false;
    return result;
}

Mesh flatWorld(double altitude = 0.0)
{
    const MeshBuildResult built = BuildWorld(
        snapshot(100.0, 100.0), settings(),
        [altitude](double, double) { return altitude; });
    return built.mesh;
}

} // namespace

class Terrain3DCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void geographicValidityOffsetsAndDatelineRoundTrip();
    void settingsAreBoundedAndGridSizeIsOdd();
    void meshUsesFiniteDemMedianAndVehicleFallback();
    void meshCancellationIsTypedAndStopsSampling();
    void bilinearAltitudeSamplingIsStable();
    void lockedCameraExtrapolatesAndFreeCameraMoves();
    void projectionAndScreenRayUseCameraBasis();
    void rayIntersectsFlatTerrain();
    void softwareRendererProducesBoundedDiagnosticFrame();
};

void Terrain3DCoreTest::geographicValidityOffsetsAndDatelineRoundTrip()
{
    QVERIFY(!GeoPoint{}.isValid());
    QVERIFY(!(GeoPoint{91.0, 20.0, 0.0}.isValid()));
    QVERIFY(!(GeoPoint{10.0, 181.0, 0.0}.isValid()));
    QVERIFY(!(GeoPoint{10.0, 20.0,
                       std::numeric_limits<double>::infinity()}.isValid()));

    const GeoPoint origin{35.1856, 33.3823, 125.0};
    const GeoPoint moved = origin.offset(420.0, -275.0, 18.0);
    QVERIFY(moved.isValid());
    const LocalPoint local = origin.toLocal(moved);
    QVERIFY(std::abs(local.eastM - 420.0) < 0.15);
    QVERIFY(std::abs(local.northM + 275.0) < 0.01);
    QVERIFY(std::abs(local.altitudeM - 18.0) < 1.0e-9);

    const GeoPoint datelineOrigin{10.0, 179.999, 50.0};
    const GeoPoint acrossDateline = datelineOrigin.offset(500.0, 0.0);
    QVERIFY(acrossDateline.longitude < -179.99);
    const LocalPoint datelineLocal = datelineOrigin.toLocal(acrossDateline);
    QVERIFY(std::abs(datelineLocal.eastM - 500.0) < 0.1);
    QVERIFY(std::abs(datelineLocal.northM) < 0.01);
}

void Terrain3DCoreTest::settingsAreBoundedAndGridSizeIsOdd()
{
    Settings raw;
    raw.rangeM = std::numeric_limits<double>::infinity();
    raw.gridSize = 64;
    raw.textureMinZoom = -5;
    raw.textureMaxZoom = 99;
    raw.verticalExaggeration = -4.0;
    const Settings normalized = raw.normalized();
    QCOMPARE(normalized.rangeM, 1500.0);
    QCOMPARE(normalized.gridSize, 65);
    QCOMPARE(normalized.textureMinZoom, 1);
    QCOMPARE(normalized.textureMaxZoom, 20);
    QCOMPARE(normalized.verticalExaggeration, 0.25);

    raw.gridSize = 34;
    QCOMPARE(raw.normalized().gridSize, 33);

    raw.rangeM = 50.0;
    raw.gridSize = 2;
    raw.textureMinZoom = 18;
    raw.textureMaxZoom = 7;
    raw.verticalExaggeration = 100.0;
    const Settings lowerUpper = raw.normalized();
    QCOMPARE(lowerUpper.rangeM, 250.0);
    QCOMPARE(lowerUpper.gridSize, 17);
    QCOMPARE(lowerUpper.textureMinZoom, 18);
    QCOMPARE(lowerUpper.textureMaxZoom, 18);
    QCOMPARE(lowerUpper.verticalExaggeration, 8.0);
}

void Terrain3DCoreTest::meshUsesFiniteDemMedianAndVehicleFallback()
{
    int sample = 0;
    const double missing = std::numeric_limits<double>::quiet_NaN();
    const MeshBuildResult median = BuildWorld(
        snapshot(130.0, 30.0), settings(400.0),
        [&sample, missing](double, double) mutable {
            const double values[] = {10.0, 30.0, 20.0};
            return sample < 3 ? values[sample++] : missing;
        });
    QVERIFY2(median.ok(), qPrintable(median.message));
    QCOMPARE(median.mesh.vertices.size(), 17 * 17);
    QCOMPARE(median.mesh.missingSamples, 17 * 17 - 3);
    QCOMPARE(median.mesh.referenceAltitudeM, 20.0);
    QCOMPARE(median.mesh.minimumAltitudeM, 10.0);
    QCOMPARE(median.mesh.maximumAltitudeM, 30.0);
    QCOMPARE(median.mesh.sampleAltitude(0.0, 0.0), 20.0);

    const MeshBuildResult fallback = BuildWorld(
        snapshot(130.0, 30.0), settings(400.0),
        [missing](double, double) { return missing; });
    QVERIFY2(fallback.ok(), qPrintable(fallback.message));
    QCOMPARE(fallback.mesh.missingSamples, 17 * 17);
    QCOMPARE(fallback.mesh.referenceAltitudeM, 100.0);
    QCOMPARE(fallback.mesh.minimumAltitudeM, 100.0);
    QCOMPARE(fallback.mesh.maximumAltitudeM, 100.0);
}

void Terrain3DCoreTest::meshCancellationIsTypedAndStopsSampling()
{
    int checks = 0;
    int elevationCalls = 0;
    const MeshBuildResult result = BuildWorld(
        snapshot(), settings(),
        [&elevationCalls](double, double) {
            ++elevationCalls;
            return 10.0;
        },
        [&checks]() { return ++checks > 6; });
    QCOMPARE(static_cast<int>(result.error),
             static_cast<int>(ErrorCode::Cancelled));
    QVERIFY(!result.ok());
    QVERIFY(elevationCalls > 0);
    QVERIFY(elevationCalls < 17 * 17);
    QVERIFY(result.mesh.vertices.isEmpty());
}

void Terrain3DCoreTest::bilinearAltitudeSamplingIsStable()
{
    MeshBuildResult built = BuildWorld(
        snapshot(), settings(500.0),
        [](double, double) { return 0.0; });
    QVERIFY2(built.ok(), qPrintable(built.message));
    for (Vertex &vertex : built.mesh.vertices) {
        vertex.altitudeM = 100.0 + vertex.eastM * 0.01
            + vertex.northM * 0.02;
    }
    built.mesh.minimumAltitudeM = 85.0;
    built.mesh.maximumAltitudeM = 115.0;
    QVERIFY(std::abs(built.mesh.sampleAltitude(125.0, -75.0) - 99.75)
            < 1.0e-9);
    QVERIFY(std::isnan(built.mesh.sampleAltitude(501.0, 0.0)));
}

void Terrain3DCoreTest::lockedCameraExtrapolatesAndFreeCameraMoves()
{
    Mesh world = flatWorld();
    Snapshot state = snapshot(100.0, 100.0, 1000);
    state.velocityEastMps = 4.0;
    state.velocityNorthMps = 6.0;
    state.velocityVerticalMps = 2.0;
    const Camera locked = Camera::Locked(world, state, 1500);
    QVERIFY(std::abs(locked.eastM - 2.0) < 1.0e-9);
    QVERIFY(std::abs(locked.northM - 3.0) < 1.0e-9);
    QVERIFY(std::abs(locked.altitudeM - 101.0) < 1.0e-9);

    const Camera moved = locked.Move(CameraMotion::Forward, 10.0)
        .Move(CameraMotion::Right, 5.0)
        .Move(CameraMotion::Up, 2.0);
    QVERIFY(std::abs(moved.eastM - locked.eastM - 5.0) < 1.0e-9);
    QVERIFY(std::abs(moved.northM - locked.northM - 10.0) < 1.0e-9);
    QCOMPARE(moved.altitudeM, locked.altitudeM + 2.0);

    Camera angles = locked;
    angles.yawDeg = 1.0;
    angles.pitchDeg = 84.0;
    angles = angles.Move(CameraMotion::YawLeft, 3.0)
        .Move(CameraMotion::PitchUp, 10.0);
    QCOMPARE(angles.yawDeg, 358.0);
    QCOMPARE(angles.pitchDeg, 85.0);
}

void Terrain3DCoreTest::projectionAndScreenRayUseCameraBasis()
{
    const Camera camera{0.0, 0.0, 100.0, 0.0, 0.0, 0.0};
    const ProjectedPoint forward = project(
        camera, {0.0, 100.0, 100.0}, 800, 600);
    const ProjectedPoint behind = project(
        camera, {0.0, -100.0, 100.0}, 800, 600);
    QVERIFY(forward.visible);
    QVERIFY(std::abs(forward.x - 400.0) < 1.0e-9);
    QVERIFY(std::abs(forward.y - 300.0) < 1.0e-9);
    QVERIFY(!behind.visible);

    Vector3 ray;
    QVERIFY(ScreenRay(camera, 400.0, 300.0, 800, 600, &ray));
    QVERIFY(std::abs(ray.x) < 1.0e-12);
    QVERIFY(std::abs(ray.y - 1.0) < 1.0e-12);
    QVERIFY(std::abs(ray.z) < 1.0e-12);
    QVERIFY(!ScreenRay(camera, 0.0, 0.0, 0, 600, &ray));
}

void Terrain3DCoreTest::rayIntersectsFlatTerrain()
{
    const Mesh world = flatWorld();
    const Camera camera{0.0, 0.0, 100.0, 0.0, -45.0, 0.0};
    Vector3 ray;
    QVERIFY(ScreenRay(camera, 400.0, 300.0, 800, 600, &ray));
    GeoPoint hit;
    QVERIFY(TryIntersect(world, camera, ray, &hit));
    QVERIFY(std::abs(hit.altitudeM) < 0.01);
    QVERIFY(hit.latitude > world.center.latitude);
    QVERIFY(std::abs(hit.longitude - world.center.longitude) < 0.00001);

    QVERIFY(!TryIntersect(world, camera, {0.0, 0.0, 0.0}, &hit));
}

void Terrain3DCoreTest::softwareRendererProducesBoundedDiagnosticFrame()
{
    Snapshot state = snapshot(80.0, 80.0);
    state.pitchDeg = -25.0;
    state.rollDeg = 5.0;
    state.armed = true;
    MeshBuildResult built = BuildWorld(
        state, settings(500.0),
        [&state](double latitude, double longitude) {
            return 5.0 + (latitude - state.vehicle.latitude) * 1000.0
                + (longitude - state.vehicle.longitude) * 200.0;
        });
    QVERIFY2(built.ok(), qPrintable(built.message));
    Camera camera = Camera::Locked(built.mesh, state,
                                   state.capturedMonotonicMs);
    camera.northM = -100.0;
    camera.altitudeM = 100.0;

    const RenderResult rendered = Render(
        built.mesh, state, settings(500.0), camera, 100, 1200);
    QVERIFY2(rendered.ok(), qPrintable(rendered.message));
    QCOMPARE(rendered.image.size(), QSize(MinimumRenderWidth,
                                          MaximumRenderHeight));
    QCOMPARE(rendered.image.format(), QImage::Format_ARGB32_Premultiplied);
    QVERIFY(rendered.triangleCount > 0);
    QVERIFY(rendered.details.contains(QStringLiteral("17×17 mesh")));
    QVERIFY(rendered.details.contains(QStringLiteral("DEM missing 0/289")));

    Settings clearSettings = settings(500.0);
    clearSettings.fogEnabled = false;
    const RenderResult withoutFog = Render(
        built.mesh, state, clearSettings, camera, 100, 1200);
    QVERIFY2(withoutFog.ok(), qPrintable(withoutFog.message));
    QVERIFY(withoutFog.image != rendered.image);

    Settings exaggeratedSettings = settings(500.0);
    exaggeratedSettings.verticalExaggeration = 4.0;
    const RenderResult exaggerated = Render(
        built.mesh, state, exaggeratedSettings, camera, 100, 1200);
    QVERIFY2(exaggerated.ok(), qPrintable(exaggerated.message));
    QVERIFY(exaggerated.image != rendered.image);

    QSet<QRgb> sampledColors;
    bool foundVehicleMarker = false;
    const QRgb vehicleGreen = qRgb(50, 205, 50);
    for (int y = 0; y < rendered.image.height(); y += 20) {
        for (int x = 0; x < rendered.image.width(); x += 20) {
            sampledColors.insert(rendered.image.pixel(x, y));
        }
    }
    for (int y = 0; y < rendered.image.height() && !foundVehicleMarker; ++y) {
        for (int x = 0; x < rendered.image.width(); ++x) {
            if (rendered.image.pixel(x, y) == vehicleGreen) {
                foundVehicleMarker = true;
                break;
            }
        }
    }
    QVERIFY(sampledColors.size() > 8);
    QVERIFY(foundVehicleMarker);
}

QTEST_MAIN(Terrain3DCoreTest)

#include "test_terrain3dcore.moc"
