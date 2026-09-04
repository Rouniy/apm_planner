#include "ui/tools/PropagationCore.h"

#include <QtTest>

#include <cmath>
#include <limits>

namespace
{
using namespace PropagationCore;

CoverageRequest request(double rangeKilometers = 0.1,
                        double azimuthStepDegrees = 10.0,
                        double convergenceDegrees = 5.0)
{
    CoverageRequest result;
    result.home = {34.0, 33.0, 100.0};
    result.droneAltitudeAmsl = 200.0;
    result.parameters.rangeKilometers = rangeKilometers;
    result.parameters.azimuthStepDegrees = azimuthStepDegrees;
    result.parameters.convergenceDegrees = convergenceDegrees;
    return result;
}

void verifyClosed(const QVector<GeoPoint> &points)
{
    QVERIFY(points.size() >= 4);
    QCOMPARE(points.first().latitude, points.last().latitude);
    QCOMPARE(points.first().longitude, points.last().longitude);
    QCOMPARE(points.first().altitudeM, points.last().altitudeM);
}

} // namespace

class PropagationCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void preservesMissionPlannerRfDefaults();
    void rasterKeepsNorthAtTopAndUsesTerrainPalette();
    void rasterSamplesResolutionBlocksAtTheirCentres();
    void rasterLeavesMissingOceanAndZeroTransparent();
    void rasterDerivesAutomaticAltitudeRange();
    void rasterClampsDimensionsAndResolution();
    void rasterElevationPaletteUsesVehicleClearance();
    void rasterCancellationReturnsNoPartialPixels();
    void flatTerrainCoverageReachesConfiguredRange();
    void halfDegreeAzimuthProducesFullClosedContour();
    void obstacleAboveAircraftCeilingLimitsEveryRay();
    void missingTerrainSuppressesTheWholePolygon();
    void oceanIsKnownZeroMetreTerrain();
    void zeroConvergenceRemainsBounded();
    void cancellationReturnsNoPartialGeometry();
    void geodesicCircleAndDistanceRingsAreClosed();
    void rejectsInvalidInputsWithoutSamplingTerrain();
};

void PropagationCoreTest::preservesMissionPlannerRfDefaults()
{
    const Parameters parameters;
    QCOMPARE(parameters.clearanceMeters, 5.0);
    QCOMPARE(parameters.resolutionPixels, 4);
    QCOMPARE(parameters.azimuthStepDegrees, 1.0);
    QCOMPARE(parameters.convergenceDegrees, 1.0);
    QCOMPARE(parameters.rangeKilometers, 2.0);
    QCOMPARE(parameters.baseHeightMeters, 2.0);
    QCOMPARE(parameters.minimumAltitudeM, 100.0);
    QCOMPARE(parameters.maximumAltitudeM, 400.0);
}

void PropagationCoreTest::rasterKeepsNorthAtTopAndUsesTerrainPalette()
{
    RasterRequest input;
    input.extent = {-1000.0, -1000.0, 1000.0, 1000.0};
    input.width = 2;
    input.height = 2;
    input.parameters.resolutionPixels = 1;
    input.parameters.manualAltitudeRange = true;
    input.parameters.minimumAltitudeM = 300.0;
    input.parameters.maximumAltitudeM = 0.0;
    const RasterGeometry result = buildRaster(
        input, [](double latitude, double) {
            return TerrainSample::valid(latitude > 0.0 ? 100.0 : 200.0);
        });

    QVERIFY(result.complete());
    QCOMPARE(result.width, 2);
    QCOMPARE(result.height, 2);
    QCOMPARE(result.validSampleCount, 4);
    QCOMPARE(result.missingSampleCount, 0);
    QCOMPARE(result.minimumAltitudeM, 0.0);
    QCOMPARE(result.maximumAltitudeM, 300.0);
    QCOMPARE(result.rgba.at(3), quint8(100));
    QCOMPARE(result.rgba.at(7), quint8(100));
    QCOMPARE(result.rgba.at(11), quint8(100));
    QCOMPARE(result.rgba.at(15), quint8(100));
    QVERIFY(result.rgba.at(0) != result.rgba.at(8)
            || result.rgba.at(1) != result.rgba.at(9)
            || result.rgba.at(2) != result.rgba.at(10));
}

void PropagationCoreTest::rasterDerivesAutomaticAltitudeRange()
{
    RasterRequest input;
    input.extent = {-1000.0, -10.0, 1000.0, 10.0};
    input.width = 3;
    input.height = 1;
    input.parameters.resolutionPixels = 1;
    int sample = 0;
    const RasterGeometry result = buildRaster(
        input, [&sample](double, double) {
            const double altitudes[] = {100.0, 150.0, 200.0};
            return TerrainSample::valid(altitudes[sample++]);
        });

    QVERIFY(result.complete());
    QCOMPARE(result.validSampleCount, 3);
    QCOMPARE(result.minimumAltitudeM, 100.0);
    QCOMPARE(result.maximumAltitudeM, 200.0);
    QCOMPARE(result.rgba.at(7), quint8(100));
}

void PropagationCoreTest::rasterClampsDimensionsAndResolution()
{
    RasterRequest input;
    input.extent = {-1000.0, -1000.0, 1000.0, 1000.0};
    input.width = 0;
    input.height = 5000;
    input.parameters.resolutionPixels = 1000;
    input.parameters.manualAltitudeRange = true;
    input.parameters.minimumAltitudeM = 0.0;
    input.parameters.maximumAltitudeM = 200.0;
    int samples = 0;
    const RasterGeometry result = buildRaster(
        input, [&samples](double, double) {
            ++samples;
            return TerrainSample::valid(100.0);
        });

    QVERIFY(result.complete());
    QCOMPARE(result.width, 1);
    QCOMPARE(result.height, 4096);
    QCOMPARE(samples, 64); // ceil(1/64) * ceil(4096/64)
    QCOMPARE(result.validSampleCount, 64);
    QCOMPARE(result.rgba.size(), 1 * 4096 * 4);
}

void PropagationCoreTest::rasterSamplesResolutionBlocksAtTheirCentres()
{
    RasterRequest input;
    input.extent = {-1000.0, -1000.0, 1000.0, 1000.0};
    input.width = 5;
    input.height = 3;
    input.parameters.resolutionPixels = 2;
    input.parameters.manualAltitudeRange = true;
    input.parameters.minimumAltitudeM = 0.0;
    input.parameters.maximumAltitudeM = 300.0;
    int samples = 0;
    const RasterGeometry result = buildRaster(
        input, [&samples](double, double) {
            ++samples;
            return TerrainSample::valid(100.0);
        });

    QVERIFY(result.complete());
    QCOMPARE(samples, 6); // ceil(5/2) * ceil(3/2)
    QCOMPARE(result.validSampleCount, 6);
    QCOMPARE(result.rgba.size(), 5 * 3 * 4);
    for (int pixel = 0; pixel < 5 * 3; ++pixel) {
        QCOMPARE(result.rgba.at(pixel * 4 + 3), quint8(100));
    }
}

void PropagationCoreTest::rasterLeavesMissingOceanAndZeroTransparent()
{
    RasterRequest input;
    input.extent = {-1000.0, -10.0, 1000.0, 10.0};
    input.width = 4;
    input.height = 1;
    input.parameters.resolutionPixels = 1;
    input.parameters.manualAltitudeRange = true;
    input.parameters.minimumAltitudeM = 0.0;
    input.parameters.maximumAltitudeM = 200.0;
    int sample = 0;
    const RasterGeometry result = buildRaster(
        input, [&sample](double, double) {
            switch (sample++) {
            case 0:
                return TerrainSample::missing();
            case 1:
                return TerrainSample::ocean();
            case 2:
                return TerrainSample::valid(0.0);
            default:
                return TerrainSample::valid(100.0);
            }
        });

    QVERIFY(result.complete());
    QCOMPARE(result.validSampleCount, 1);
    QCOMPARE(result.missingSampleCount, 1);
    QCOMPARE(result.rgba.at(3), quint8(0));
    QCOMPARE(result.rgba.at(7), quint8(0));
    QCOMPARE(result.rgba.at(11), quint8(0));
    QCOMPARE(result.rgba.at(15), quint8(100));
}

void PropagationCoreTest::rasterElevationPaletteUsesVehicleClearance()
{
    QCOMPARE(elevationPaletteIndex(105.0, 101.0, 5.0), 50);
    QCOMPARE(elevationPaletteIndex(105.0, 104.0, 5.0), 203);
    QCOMPARE(elevationPaletteIndex(105.0, 100.0, 5.0), 0);
    QCOMPARE(elevationPaletteIndex(105.0, 110.0, 5.0), 254);

    RasterRequest input;
    input.extent = {-1.0, -1.0, 1.0, 1.0};
    input.width = 1;
    input.height = 1;
    input.vehicleAltitudeAmsl = 105.0;
    input.parameters.elevationMap = true;
    input.parameters.clearanceMeters = 5.0;
    input.parameters.resolutionPixels = 1;
    const RasterGeometry result = buildRaster(
        input, [](double, double) { return TerrainSample::valid(104.0); });
    QVERIFY(result.complete());
    QCOMPARE(result.rgba.at(3), quint8(100));
}

void PropagationCoreTest::rasterCancellationReturnsNoPartialPixels()
{
    RasterRequest input;
    input.extent = {-1000.0, -1000.0, 1000.0, 1000.0};
    input.width = 100;
    input.height = 100;
    input.parameters.resolutionPixels = 1;
    int terrainCalls = 0;
    int cancellationChecks = 0;
    const RasterGeometry result = buildRaster(
        input,
        [&terrainCalls](double, double) {
            ++terrainCalls;
            return TerrainSample::valid(100.0);
        },
        [&cancellationChecks]() { return ++cancellationChecks > 3; });

    QCOMPARE(static_cast<int>(result.status),
             static_cast<int>(BuildStatus::Cancelled));
    QVERIFY(!result.complete());
    QVERIFY(result.rgba.isEmpty());
    QCOMPARE(terrainCalls, 300);
}

void PropagationCoreTest::flatTerrainCoverageReachesConfiguredRange()
{
    const CoverageRequest input = request();
    const CoverageGeometry result = buildRfCoverage(
        input, [](double, double) { return TerrainSample::valid(100.0); });

    QVERIFY(result.complete());
    QCOMPARE(static_cast<int>(result.status),
             static_cast<int>(BuildStatus::Finished));
    QCOMPARE(result.missingSampleCount, 0);
    QCOMPARE(result.points.size(), 37);
    verifyClosed(result.points);
    for (int index = 0; index + 1 < result.points.size(); ++index) {
        const double distance = haversineMeters(
            input.home, result.points.at(index));
        QVERIFY(distance >= 99.0);
        QVERIFY(distance <= 101.0);
    }
}

void PropagationCoreTest::halfDegreeAzimuthProducesFullClosedContour()
{
    const CoverageGeometry result = buildRfCoverage(
        request(0.01, 0.5, 15.0),
        [](double, double) { return TerrainSample::valid(100.0); });

    QVERIFY(result.complete());
    QCOMPARE(result.points.size(), 721);
    verifyClosed(result.points);
}

void PropagationCoreTest::obstacleAboveAircraftCeilingLimitsEveryRay()
{
    const CoverageRequest input = request();
    const CoverageGeometry result = buildRfCoverage(
        input, [&input](double latitude, double longitude) {
            const double distance = haversineMeters(
                input.home, {latitude, longitude, 0.0});
            return TerrainSample::valid(distance >= 45.0 ? 300.0 : 100.0);
        });

    QVERIFY(result.complete());
    verifyClosed(result.points);
    for (int index = 0; index + 1 < result.points.size(); ++index) {
        const double distance = haversineMeters(
            input.home, result.points.at(index));
        QVERIFY(distance >= 49.0);
        QVERIFY(distance <= 51.0);
        QCOMPARE(result.points.at(index).altitudeM, 300.0);
    }
}

void PropagationCoreTest::missingTerrainSuppressesTheWholePolygon()
{
    const CoverageGeometry result = buildRfCoverage(
        request(), [](double, double) { return TerrainSample::missing(); });

    QCOMPARE(static_cast<int>(result.status),
             static_cast<int>(BuildStatus::Finished));
    QVERIFY(!result.complete());
    QVERIFY(result.points.isEmpty());
    QCOMPARE(result.missingSampleCount, 36);
}

void PropagationCoreTest::oceanIsKnownZeroMetreTerrain()
{
    CoverageRequest input = request(0.01, 10.0, 5.0);
    input.home.altitudeM = 0.0;
    input.droneAltitudeAmsl = 20.0;
    const CoverageGeometry result = buildRfCoverage(
        input, [](double, double) { return TerrainSample::ocean(); });

    QVERIFY(result.complete());
    QCOMPARE(result.missingSampleCount, 0);
    for (const GeoPoint &point : result.points) {
        QCOMPARE(point.altitudeM, 0.0);
    }
}

void PropagationCoreTest::zeroConvergenceRemainsBounded()
{
    CoverageRequest input = request(0.01, 10.0, 0.0);
    int terrainCalls = 0;
    const CoverageGeometry result = buildRfCoverage(
        input, [&terrainCalls](double, double) {
            ++terrainCalls;
            return TerrainSample::valid(100.0);
        });

    QVERIFY(result.complete());
    QVERIFY(terrainCalls > 0);
    // 36 rays * 32 iterations * at most two samples for this 10 m path.
    QVERIFY(terrainCalls <= 36 * 32 * 2);
}

void PropagationCoreTest::cancellationReturnsNoPartialGeometry()
{
    int cancellationChecks = 0;
    int terrainCalls = 0;
    const CoverageGeometry result = buildRfCoverage(
        request(1.0, 10.0, 0.05),
        [&terrainCalls](double, double) {
            ++terrainCalls;
            return TerrainSample::valid(100.0);
        },
        [&cancellationChecks]() { return ++cancellationChecks > 6; });

    QCOMPARE(static_cast<int>(result.status),
             static_cast<int>(BuildStatus::Cancelled));
    QVERIFY(!result.complete());
    QVERIFY(result.points.isEmpty());
    QCOMPARE(result.missingSampleCount, 0);
    QVERIFY(terrainCalls > 0);
    QVERIFY(terrainCalls < 36 * 101);
}

void PropagationCoreTest::geodesicCircleAndDistanceRingsAreClosed()
{
    const GeoPoint center{34.0, 33.0, 42.0};
    const QVector<GeoPoint> circle = buildCircle(center, 250.0, 36);
    QCOMPARE(circle.size(), 37);
    verifyClosed(circle);
    const double quarterDistance = haversineMeters(center, circle.at(9));
    QVERIFY(quarterDistance >= 249.0);
    QVERIFY(quarterDistance <= 251.0);
    QCOMPARE(circle.at(9).altitudeM, center.altitudeM);

    const DistanceRingGeometry rings = buildDistanceRings(
        center, 0.25, 0.8, 36);
    QCOMPARE(rings.outer.size(), 37);
    QCOMPARE(rings.tolerance.size(), 37);
    verifyClosed(rings.outer);
    verifyClosed(rings.tolerance);
    const double outerDistance = haversineMeters(center, rings.outer.at(9));
    const double toleranceDistance = haversineMeters(
        center, rings.tolerance.at(9));
    QVERIFY(std::abs(outerDistance - 250.0) < 1.0);
    QVERIFY(std::abs(toleranceDistance - 200.0) < 1.0);
}

void PropagationCoreTest::rejectsInvalidInputsWithoutSamplingTerrain()
{
    int terrainCalls = 0;
    const auto terrain = [&terrainCalls](double, double) {
        ++terrainCalls;
        return TerrainSample::valid(0.0);
    };

    CoverageRequest invalidCoordinate = request();
    invalidCoordinate.home.latitude = 91.0;
    const CoverageGeometry coordinateResult = buildRfCoverage(
        invalidCoordinate, terrain);
    QCOMPARE(static_cast<int>(coordinateResult.status),
             static_cast<int>(BuildStatus::InvalidInput));

    CoverageRequest invalidSetting = request();
    invalidSetting.parameters.azimuthStepDegrees =
        std::numeric_limits<double>::quiet_NaN();
    const CoverageGeometry settingResult = buildRfCoverage(
        invalidSetting, terrain);
    QCOMPARE(static_cast<int>(settingResult.status),
             static_cast<int>(BuildStatus::InvalidInput));

    CoverageRequest emptyRange = request();
    emptyRange.parameters.rangeKilometers = 0.0;
    const CoverageGeometry emptyResult = buildRfCoverage(emptyRange, terrain);
    QCOMPARE(static_cast<int>(emptyResult.status),
             static_cast<int>(BuildStatus::Finished));
    QVERIFY(emptyResult.points.isEmpty());
    QCOMPARE(terrainCalls, 0);
}

QTEST_APPLESS_MAIN(PropagationCoreTest)

#include "test_propagationcore.moc"
