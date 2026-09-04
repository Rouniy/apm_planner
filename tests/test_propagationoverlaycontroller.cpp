#include "ui/tools/PropagationOverlayController.h"

#include <QtTest>

#include <QImage>
#include <QSemaphore>
#include <QSettings>
#include <QTemporaryDir>
#include <QWidget>
#include <QtMath>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

namespace
{

class FakeMapBackend final : public AbstractMapWidget
{
public:
    explicit FakeMapBackend(QWidget *surface, QObject *parent = nullptr)
        : AbstractMapWidget(parent)
        , m_surface(surface)
    {
    }

    QString BackendId() const override { return QStringLiteral("fake"); }
    QString SharedCacheRoot() const override
    {
        return QStringLiteral("/fake");
    }
    QWidget *Widget() const override { return m_surface; }
    int MinZoom() const override { return 1; }
    int MaxZoom() const override { return 20; }
    double ZoomReal() const override { return 10.0; }
    int CurrentZoomLevel() const override { return 10; }
    QSize ViewportPixelSize() const override { return viewportSize; }
    MapGeoBounds VisibleTileExtent() const override { return visibleBounds; }
    MapCoordinate CurrentPosition() const override { return {}; }
    core::MapType::Types CurrentMapType() const override
    {
        return core::MapType::OpenStreetMap;
    }
    bool FollowUAVEnabled() const override { return false; }
    float UpdateRateLimit() const override { return 0.0F; }
    int TrailType() const override { return 0; }
    float TrailInterval() const override { return 0.0F; }
    void SetZoom(double) override {}
    void SetCurrentPosition(double, double) override {}
    void SetAcceleratedRenderingEnabled(bool) override {}
    void SetFollowUAVEnabled(bool) override {}
    void SetTrailModeTimed(int) override {}
    void SetTrailModeDistance(int) override {}
    void DeleteTrails() override {}
    void SetUpdateRateLimit(float) override {}
    void ShowGoToDialog() override {}
    void GoHome() override {}
    void LastPosition() override {}
    void CacheVisibleRegion() override {}
    void UpdateHomePosition(double, double, double) override {}
    void SetMissionPlanningEnabled(bool) override {}
    void SetPlannerRows(
        const QVector<WpRowData> &,
        FlightPlannerMissionModel::MissionStore) override {}
    void SetPlannerAltitudePresentation(double, const QString &) override {}
    void SetPlannerHome(double, double, double) override {}
    void ClearPlannerHome() override {}
    void SetPlannerSelection(int) override {}
    void SetLogTrail(const QVector<MapCoordinate> &) override {}
    void SetLogCursor(const MapCoordinate &, double) override {}

    void SetPropagationRaster(const QImage &image,
                              const MapGeoBounds &bounds) override
    {
        ++rasterSetCount;
        raster = image;
        rasterBounds = bounds;
    }

    void ClearPropagationRaster() override
    {
        ++rasterClearCount;
        raster = {};
        rasterBounds = {};
    }

    void SetPropagationContour(
        const MapOverlayPolyline &value) override
    {
        ++contourSetCount;
        contour = value;
        contourPresent = true;
        contourHistory.append(value);
    }

    void ClearPropagationContour() override
    {
        ++contourClearCount;
        contour = {};
        contourPresent = false;
    }

    void SetPropagationRings(
        const QVector<MapOverlayPolyline> &value) override
    {
        ++ringsSetCount;
        rings = value;
    }

    void ClearPropagationRings() override
    {
        ++ringsClearCount;
        rings.clear();
    }

    void SetPropagationStatus(const QString &newLegend,
                              const QString &newStatus) override
    {
        ++statusSetCount;
        legend = newLegend;
        status = newStatus;
    }

    QVector<MapOverlayPolyline> allPolylines() const
    {
        QVector<MapOverlayPolyline> result = rings;
        if (contourPresent) {
            result.prepend(contour);
        }
        return result;
    }

    int lineCount(const QColor &color) const
    {
        int count = 0;
        for (const MapOverlayPolyline &line : allPolylines()) {
            if (line.color == color) {
                ++count;
            }
        }
        return count;
    }

    const MapOverlayPolyline *line(const QColor &color,
                                   double altitude) const
    {
        if (contourPresent && contour.color == color
            && !contour.points.isEmpty()
            && contour.points.first().altitude == altitude) {
            return &contour;
        }
        for (const MapOverlayPolyline &candidate : rings) {
            if (candidate.color == color && !candidate.points.isEmpty()
                && candidate.points.first().altitude == altitude) {
                return &candidate;
            }
        }
        return nullptr;
    }

    MapGeoBounds visibleBounds{32.0, 35.0, 34.0, 33.0};
    QSize viewportSize{24, 18};
    QImage raster;
    MapGeoBounds rasterBounds;
    MapOverlayPolyline contour;
    bool contourPresent = false;
    QVector<MapOverlayPolyline> rings;
    QVector<MapOverlayPolyline> contourHistory;
    QString legend;
    QString status;
    int rasterSetCount = 0;
    int rasterClearCount = 0;
    int contourSetCount = 0;
    int contourClearCount = 0;
    int ringsSetCount = 0;
    int ringsClearCount = 0;
    int statusSetCount = 0;

private:
    QWidget *m_surface = nullptr;
};

PropagationMapState validState()
{
    PropagationMapState state;
    state.homeValid = true;
    state.home = {34.0, 33.0, 50.0};
    state.telemetry.vehicleIdentity = 7;
    state.telemetry.positionValid = true;
    state.telemetry.latitude = 34.01;
    state.telemetry.longitude = 33.02;
    state.telemetry.altitudeAmsl = 120.0;
    state.telemetry.batteryKilometresLeft = 0.25;
    return state;
}

PropagationSettings fastSettings()
{
    PropagationSettings settings = PropagationSettings::Default();
    settings.resolutionPixels = 64;
    settings.azimuthStepDegrees = 10.0;
    settings.convergenceDegrees = 15.0;
    settings.rangeKilometers = 0.01;
    settings.minimumAltitude = 0.0;
    settings.maximumAltitude = 200.0;
    settings.manualAltitudeRange = true;
    return settings;
}

class Harness final
{
public:
    explicit Harness(
        PropagationCore::TerrainProvider terrainProvider =
            [](double, double) {
                return PropagationCore::TerrainSample::valid(10.0);
            })
        : settings(directory.filePath(QStringLiteral("propagation.ini")),
                   QSettings::IniFormat)
        , store(&settings)
        , map(&surface)
        , state(validState())
    {
        surface.resize(24, 18);
        controller.reset(new PropagationOverlayController(
            &map,
            [this]() { return state; },
            terrainProvider,
            &store));
    }

    bool apply(const PropagationSettings &value)
    {
        return store.setSettings(value);
    }

    QTemporaryDir directory;
    QSettings settings;
    PropagationSettingsStore store;
    QWidget surface;
    FakeMapBackend map;
    PropagationMapState state;
    std::unique_ptr<PropagationOverlayController> controller;
};

double distanceFrom(const PropagationCore::GeoPoint &center,
                    const MapOverlayPolyline &line)
{
    if (line.points.isEmpty()) {
        return qQNaN();
    }
    const MapCoordinate point = line.points.first();
    return PropagationCore::haversineMeters(
        center, {point.latitude, point.longitude, point.altitude});
}

} // namespace

class PropagationOverlayControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void rasterEnablesAndClearsIndependently();
    void rfAndRingsEnableAndClearIndependently();
    void publishesHomeAndDroneDistanceRingPairs();
    void quantizesAltitudeLikeMissionPlanner();
    void missingTerrainFailsClosedAndPublishesStatus();
    void workerFailuresClearPreviouslyPublishedLayers();
    void staleCancelledWorkerCannotPublishOldHome();
    void activeLifecycleSuppressesRefreshAndDestructorClears();
};

void PropagationOverlayControllerTest::rasterEnablesAndClearsIndependently()
{
    Harness harness;
    QVERIFY(harness.directory.isValid());
    PropagationSettings settings = fastSettings();
    settings.terrainMap = true;
    settings.homeDistance = true;
    settings.showScale = true;
    QVERIFY(harness.apply(settings));

    harness.controller->setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.map.raster.isNull(), 5000);
    QCOMPARE(harness.map.raster.format(), QImage::Format_RGBA8888);
    QCOMPARE(harness.map.raster.width(), 24 + 384);
    QCOMPARE(harness.map.raster.height(), 18 + 384);
    QVERIFY(harness.map.rasterBounds.IsValid());
    QVERIFY(harness.map.legend.contains(QStringLiteral("Elevation (AMSL)")));
    QCOMPARE(harness.map.rings.size(), 2);
    QVERIFY(!harness.map.contourPresent);

    const int clears = harness.map.rasterClearCount;
    settings.terrainMap = false;
    QVERIFY(harness.apply(settings));
    QTRY_VERIFY_WITH_TIMEOUT(harness.map.raster.isNull(), 1000);
    QVERIFY(harness.map.rasterClearCount > clears);
    QCOMPARE(harness.map.rings.size(), 2);
    QVERIFY(!harness.map.contourPresent);
    QVERIFY(harness.map.legend.isEmpty());
    QVERIFY(harness.map.status.isEmpty());
}

void PropagationOverlayControllerTest::
rfAndRingsEnableAndClearIndependently()
{
    Harness harness;
    PropagationSettings settings = fastSettings();
    settings.rfMap = true;
    settings.homeDistance = true;
    settings.tolerance = 0.8;
    QVERIFY(harness.apply(settings));

    harness.controller->setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(harness.map.allPolylines().size(), 3, 5000);
    QCOMPARE(harness.map.lineCount(Qt::white), 1);
    QCOMPARE(harness.map.lineCount(QColor(255, 0, 0)), 1);
    QCOMPARE(harness.map.lineCount(QColor(255, 165, 0)), 1);
    QVERIFY(harness.map.raster.isNull());

    const int contourClears = harness.map.contourClearCount;
    settings.rfMap = false;
    QVERIFY(harness.apply(settings));
    QTRY_COMPARE_WITH_TIMEOUT(harness.map.allPolylines().size(), 2, 1000);
    QVERIFY(harness.map.contourClearCount > contourClears);
    QCOMPARE(harness.map.lineCount(Qt::white), 0);
    QCOMPARE(harness.map.lineCount(QColor(255, 0, 0)), 1);
    QCOMPARE(harness.map.lineCount(QColor(255, 165, 0)), 1);

    const int ringClears = harness.map.ringsClearCount;
    settings.rfMap = true;
    settings.homeDistance = false;
    QVERIFY(harness.apply(settings));
    QTRY_COMPARE_WITH_TIMEOUT(harness.map.allPolylines().size(), 1, 5000);
    QVERIFY(harness.map.ringsClearCount > ringClears);
    QCOMPARE(harness.map.lineCount(Qt::white), 1);
    QCOMPARE(harness.map.lineCount(QColor(255, 0, 0)), 0);
    QCOMPARE(harness.map.lineCount(QColor(255, 165, 0)), 0);

    settings.rfMap = false;
    QVERIFY(harness.apply(settings));
    QTRY_VERIFY_WITH_TIMEOUT(harness.map.allPolylines().isEmpty(), 1000);
}

void PropagationOverlayControllerTest::
publishesHomeAndDroneDistanceRingPairs()
{
    Harness harness;
    PropagationSettings settings = fastSettings();
    settings.homeDistance = true;
    settings.droneDistance = true;
    settings.tolerance = 0.8;
    QVERIFY(harness.apply(settings));

    harness.controller->setActive(true);
    QCOMPARE(harness.map.allPolylines().size(), 4);
    QCOMPARE(harness.map.lineCount(QColor(255, 0, 0)), 2);
    QCOMPARE(harness.map.lineCount(QColor(255, 165, 0)), 2);

    const MapOverlayPolyline *homeOuter = harness.map.line(
        QColor(255, 0, 0), harness.state.home.altitudeM);
    const MapOverlayPolyline *homeTolerance = harness.map.line(
        QColor(255, 165, 0), harness.state.home.altitudeM);
    const MapOverlayPolyline *droneOuter = harness.map.line(
        QColor(255, 0, 0), harness.state.telemetry.altitudeAmsl);
    const MapOverlayPolyline *droneTolerance = harness.map.line(
        QColor(255, 165, 0), harness.state.telemetry.altitudeAmsl);
    QVERIFY(homeOuter);
    QVERIFY(homeTolerance);
    QVERIFY(droneOuter);
    QVERIFY(droneTolerance);
    for (const MapOverlayPolyline *line : {
             homeOuter, homeTolerance, droneOuter, droneTolerance}) {
        QCOMPARE(line->points.size(), 73);
        QVERIFY(line->dashed);
        QCOMPARE(line->width, 1.0);
        QCOMPARE(line->points.first().latitude,
                 line->points.last().latitude);
        QCOMPARE(line->points.first().longitude,
                 line->points.last().longitude);
    }
    QVERIFY(std::abs(distanceFrom(harness.state.home, *homeOuter) - 250.0)
            < 1.0);
    QVERIFY(std::abs(
        distanceFrom(harness.state.home, *homeTolerance) - 200.0) < 1.0);
    const PropagationCore::GeoPoint drone{
        harness.state.telemetry.latitude,
        harness.state.telemetry.longitude,
        harness.state.telemetry.altitudeAmsl};
    QVERIFY(std::abs(distanceFrom(drone, *droneOuter) - 250.0) < 1.0);
    QVERIFY(std::abs(distanceFrom(drone, *droneTolerance) - 200.0) < 1.0);

    settings.homeDistance = false;
    QVERIFY(harness.apply(settings));
    QCOMPARE(harness.map.allPolylines().size(), 2);
    QVERIFY(!harness.map.line(
        QColor(255, 0, 0), harness.state.home.altitudeM));
    QVERIFY(harness.map.line(
        QColor(255, 0, 0), harness.state.telemetry.altitudeAmsl));

    settings.droneDistance = false;
    QVERIFY(harness.apply(settings));
    QVERIFY(harness.map.allPolylines().isEmpty());
}

void PropagationOverlayControllerTest::
quantizesAltitudeLikeMissionPlanner()
{
    QCOMPARE(PropagationOverlayController::quantizeAltitude(100.24), 100.0);
    QCOMPARE(PropagationOverlayController::quantizeAltitude(100.25), 100.5);
    QCOMPARE(PropagationOverlayController::quantizeAltitude(100.26), 100.5);
    QCOMPARE(PropagationOverlayController::quantizeAltitude(-100.25), -100.5);
    QCOMPARE(PropagationOverlayController::quantizeAltitude(qQNaN()), 0.0);
}

void PropagationOverlayControllerTest::
missingTerrainFailsClosedAndPublishesStatus()
{
    std::atomic_int samples{0};
    Harness harness([&samples](double, double) {
        ++samples;
        return PropagationCore::TerrainSample::missing();
    });
    PropagationSettings settings = fastSettings();
    settings.terrainMap = true;
    settings.rfMap = true;
    QVERIFY(harness.apply(settings));

    harness.controller->setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(
        harness.map.status.contains(QStringLiteral("still unavailable")),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        harness.map.status.contains(QStringLiteral("no elevation data")),
        5000);
    QVERIFY(samples.load() > 0);
    QVERIFY(harness.map.raster.isNull());
    QVERIFY(harness.map.allPolylines().isEmpty());
    QCOMPARE(harness.map.lineCount(Qt::white), 0);
    QVERIFY(harness.map.legend.isEmpty());
}

void PropagationOverlayControllerTest::
workerFailuresClearPreviouslyPublishedLayers()
{
    std::atomic_bool fail{false};
    Harness harness([&fail](double, double) {
        if (fail.load()) {
            throw std::runtime_error("synthetic terrain failure");
        }
        return PropagationCore::TerrainSample::valid(10.0);
    });
    PropagationSettings settings = fastSettings();
    settings.terrainMap = true;
    settings.rfMap = true;
    QVERIFY(harness.apply(settings));
    harness.controller->setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.map.raster.isNull(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(harness.map.contourPresent, 5000);

    fail.store(true);
    harness.controller->invalidateTerrain();
    QTRY_VERIFY_WITH_TIMEOUT(
        harness.map.status.contains(QStringLiteral("failed")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(harness.map.raster.isNull(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.map.contourPresent, 5000);
}

void PropagationOverlayControllerTest::
staleCancelledWorkerCannotPublishOldHome()
{
    QSemaphore firstSampleEntered;
    QSemaphore releaseFirstSample;
    std::atomic_bool first{true};
    Harness harness([&](double, double) {
        if (first.exchange(false)) {
            firstSampleEntered.release();
            releaseFirstSample.acquire();
        }
        return PropagationCore::TerrainSample::valid(10.0);
    });
    PropagationSettings settings = fastSettings();
    settings.rfMap = true;
    QVERIFY(harness.apply(settings));

    harness.controller->setActive(true);
    if (!firstSampleEntered.tryAcquire(1, 3000)) {
        // Ensure teardown cannot wait forever if the worker reached the
        // blocking provider just after this timeout.
        releaseFirstSample.release();
        QFAIL("the first RF worker did not enter the terrain provider");
    }
    harness.state.home.latitude = 35.0;
    harness.state.home.longitude = 34.0;
    harness.controller->refresh();
    releaseFirstSample.release();

    QTRY_COMPARE_WITH_TIMEOUT(harness.map.lineCount(Qt::white), 1, 5000);
    for (const MapOverlayPolyline &line : harness.map.contourHistory) {
        if (line.color == Qt::white && !line.points.isEmpty()) {
            QVERIFY2(line.points.first().latitude > 34.9,
                     "a stale RF contour for the previous Home was published");
        }
    }
}

void PropagationOverlayControllerTest::
activeLifecycleSuppressesRefreshAndDestructorClears()
{
    Harness harness;
    PropagationSettings settings = fastSettings();
    settings.homeDistance = true;
    QVERIFY(harness.apply(settings));

    harness.controller->refresh();
    QCOMPARE(harness.map.ringsSetCount, 0);
    harness.controller->setActive(true);
    QCOMPARE(harness.map.rings.size(), 2);
    const int activePublications = harness.map.ringsSetCount;

    harness.controller->setActive(false);
    harness.state.telemetry.batteryKilometresLeft = 0.5;
    harness.controller->refresh();
    QCOMPARE(harness.map.ringsSetCount, activePublications);

    harness.controller->setActive(true);
    QVERIFY(harness.map.ringsSetCount > activePublications);
    const MapOverlayPolyline *outer = harness.map.line(
        QColor(255, 0, 0), harness.state.home.altitudeM);
    QVERIFY(outer);
    QVERIFY(std::abs(distanceFrom(harness.state.home, *outer) - 500.0) < 1.0);

    const int rasterClears = harness.map.rasterClearCount;
    const int contourClears = harness.map.contourClearCount;
    const int ringsClears = harness.map.ringsClearCount;
    const int statusPublications = harness.map.statusSetCount;
    harness.controller.reset();
    QCOMPARE(harness.map.rasterClearCount, rasterClears + 1);
    QCOMPARE(harness.map.contourClearCount, contourClears + 1);
    QCOMPARE(harness.map.ringsClearCount, ringsClears + 1);
    QCOMPARE(harness.map.statusSetCount, statusPublications + 1);
    QVERIFY(harness.map.raster.isNull());
    QVERIFY(harness.map.allPolylines().isEmpty());
    QVERIFY(harness.map.legend.isEmpty());
    QVERIFY(harness.map.status.isEmpty());
}

QTEST_MAIN(PropagationOverlayControllerTest)

#include "test_propagationoverlaycontroller.moc"
