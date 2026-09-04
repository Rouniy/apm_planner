#include "comm/MovingBasePositionStore.h"
#include "comm/VehicleTargetManager.h"
#include "ui/flightdata/MovingBaseMapController.h"
#include "ui/map/AbstractMapWidget.h"

#include <QPointer>
#include <QtTest>

#include <functional>

namespace
{

VehicleEndpoint endpoint(int linkId, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("Link %1").arg(linkId);
    return result;
}

MovingBasePositionFix fix(double latitude, double longitude,
                          double altitude, qint64 observedMs)
{
    MovingBasePositionFix result;
    result.latitudeDegrees = latitude;
    result.longitudeDegrees = longitude;
    result.altitudeAmslMetres = altitude;
    result.satellites = 12;
    result.hdop = 0.8;
    result.observedMonotonicMs = observedMs;
    return result;
}

class FakeMapBackend final : public AbstractMapWidget
{
public:
    explicit FakeMapBackend(QObject *parent = nullptr)
        : AbstractMapWidget(parent)
    {
    }

    QString BackendId() const override { return QStringLiteral("fake"); }
    QString SharedCacheRoot() const override
    {
        return QStringLiteral("/fake");
    }
    QWidget *Widget() const override { return nullptr; }
    int MinZoom() const override { return 1; }
    int MaxZoom() const override { return 20; }
    double ZoomReal() const override { return 10.0; }
    int CurrentZoomLevel() const override { return 10; }
    QSize ViewportPixelSize() const override { return QSize(); }
    MapGeoBounds VisibleTileExtent() const override { return {}; }
    MapCoordinate CurrentPosition() const override { return {}; }
    core::MapType::Types CurrentMapType() const override
    {
        return core::MapType::OpenStreetMap;
    }
    bool FollowUAVEnabled() const override { return false; }
    float UpdateRateLimit() const override { return 0.0f; }
    int TrailType() const override { return 0; }
    float TrailInterval() const override { return 0.0f; }
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
    void SetPropagationRaster(const QImage &, const MapGeoBounds &) override {}
    void ClearPropagationRaster() override {}
    void SetPropagationContour(const MapOverlayPolyline &) override {}
    void ClearPropagationContour() override {}
    void SetPropagationRings(
        const QVector<MapOverlayPolyline> &) override {}
    void ClearPropagationRings() override {}
    void SetPropagationStatus(const QString &, const QString &) override {}
    void SetMissionPlanningEnabled(bool) override {}
    void SetPlannerRows(const QVector<WpRowData> &,
                        FlightPlannerMissionModel::MissionStore) override {}
    void SetPlannerAltitudePresentation(double, const QString &) override {}
    void SetPlannerHome(double, double, double) override {}
    void ClearPlannerHome() override {}
    void SetPlannerSelection(int) override {}
    void SetLogTrail(const QVector<MapCoordinate> &) override {}
    void SetLogCursor(const MapCoordinate &, double) override {}

    void SetMovingBase(const MapCoordinate &position,
                       const QString &tag) override
    {
        ++setCount;
        lastPosition = position;
        lastTag = tag;
        visible = true;
        const std::function<void()> callback = onSet;
        onSet = {};
        if (callback) {
            callback();
        }
    }

    void ClearMovingBase() override
    {
        ++clearCount;
        visible = false;
        const std::function<void()> callback = onClear;
        onClear = {};
        if (callback) {
            callback();
        }
    }

    int setCount = 0;
    int clearCount = 0;
    bool visible = false;
    MapCoordinate lastPosition;
    QString lastTag;
    std::function<void()> onSet;
    std::function<void()> onClear;
};

} // namespace

class MovingBaseMapControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void attachReplaysCurrentSnapshotAndDetachClears();
    void storeUpdatesAndEmptySnapshotDriveMap();
    void targetGenerationChangeClearsWithoutCrossEndpointLeak();
    void reentrantStoreUpdateConvergesToLatestFix();
    void reentrantTargetAndAttachmentChangesStaySafe();
    void sourceDestructionClearsTheAttachedMap();
    void destructionDoesNotRetainOrDereferenceBackends();
};

void MovingBaseMapControllerTest::
attachReplaysCurrentSnapshotAndDetachClears()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    MovingBasePositionStore store(&targets);
    const VehicleTargetLease lease = targets.acquireTarget();
    QVERIFY(store.update(lease, fix(35.1, 33.2, 42.0, 100)));

    MovingBaseMapController controller(&store, &targets);
    FakeMapBackend map;
    controller.attachMap(&map);
    QCOMPARE(controller.attachedMap(),
             static_cast<AbstractMapWidget *>(&map));
    QCOMPARE(map.setCount, 1);
    QVERIFY(map.visible);
    QCOMPARE(map.lastPosition.latitude, 35.1);
    QCOMPARE(map.lastPosition.longitude, 33.2);
    QCOMPARE(map.lastPosition.altitude, 42.0);
    QCOMPARE(map.lastTag, QStringLiteral("Sats 12 hdop 0.8"));

    controller.detachMap();
    QVERIFY(!controller.attachedMap());
    QCOMPARE(map.clearCount, 1);
    QVERIFY(!map.visible);

    controller.attachMap(&map);
    QCOMPARE(map.setCount, 2);
    QVERIFY(map.visible);
}

void MovingBaseMapControllerTest::storeUpdatesAndEmptySnapshotDriveMap()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    MovingBasePositionStore store(&targets);
    MovingBaseMapController controller(&store, &targets);
    FakeMapBackend map;
    controller.attachMap(&map);
    QCOMPARE(map.clearCount, 1);
    QVERIFY(!map.visible);

    const VehicleTargetLease lease = targets.acquireTarget();
    QVERIFY(store.update(lease, fix(35.1, 33.2, 42.0, 100)));
    QCOMPARE(map.setCount, 1);
    QVERIFY(map.visible);

    QVERIFY(store.clearCurrent());
    QCOMPARE(map.clearCount, 2);
    QVERIFY(!map.visible);
}

void MovingBaseMapControllerTest::
targetGenerationChangeClearsWithoutCrossEndpointLeak()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    QVERIFY(targets.observeEndpoint(endpoint(9)));
    MovingBasePositionStore store(&targets);
    MovingBaseMapController controller(&store, &targets);
    FakeMapBackend map;
    controller.attachMap(&map);

    const VehicleTargetLease first = targets.acquireTarget();
    QVERIFY(store.update(first, fix(35.1, 33.2, 42.0, 100)));
    QVERIFY(map.visible);
    QCOMPARE(map.setCount, 1);

    bool clearObservedDuringInvalidation = false;
    connect(&targets, &VehicleTargetManager::targetGenerationChanged,
            &targets, [&map, &clearObservedDuringInvalidation](qulonglong) {
                clearObservedDuringInvalidation = !map.visible;
            });
    QVERIFY(targets.selectTarget(9, 42, 1));
    QVERIFY(clearObservedDuringInvalidation);
    QVERIFY(!map.visible);
    QVERIFY(!store.update(first, fix(36.0, 34.0, 50.0, 101)));
    QCOMPARE(map.setCount, 1);

    const VehicleTargetLease second = targets.acquireTarget();
    QVERIFY(store.update(second, fix(36.1, 34.2, 52.0, 102)));
    QCOMPARE(map.setCount, 2);
    QCOMPARE(map.lastPosition.latitude, 36.1);
    QVERIFY(map.visible);

    QVERIFY(targets.selectTarget(3, 42, 1));
    QVERIFY(!map.visible);
    QCOMPARE(map.setCount, 2);
}

void MovingBaseMapControllerTest::
reentrantStoreUpdateConvergesToLatestFix()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    MovingBasePositionStore store(&targets);
    MovingBaseMapController controller(&store, &targets);
    FakeMapBackend map;
    controller.attachMap(&map);

    const VehicleTargetLease lease = targets.acquireTarget();
    map.onSet = [&store, lease]() {
        QVERIFY(store.update(lease, fix(35.9, 33.8, 49.0, 101)));
    };
    QVERIFY(store.update(lease, fix(35.1, 33.2, 42.0, 100)));
    QCOMPARE(map.setCount, 2);
    QVERIFY(map.visible);
    QCOMPARE(map.lastPosition.latitude, 35.9);
    QCOMPARE(map.lastPosition.longitude, 33.8);
    QCOMPARE(map.lastPosition.altitude, 49.0);
}

void MovingBaseMapControllerTest::
reentrantTargetAndAttachmentChangesStaySafe()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    QVERIFY(targets.observeEndpoint(endpoint(9)));
    MovingBasePositionStore store(&targets);
    MovingBaseMapController controller(&store, &targets);
    FakeMapBackend firstMap;
    FakeMapBackend secondMap;
    controller.attachMap(&firstMap);

    const VehicleTargetLease first = targets.acquireTarget();
    firstMap.onSet = [&targets]() {
        QVERIFY(targets.selectTarget(9, 42, 1));
    };
    QVERIFY(store.update(first, fix(35.1, 33.2, 42.0, 100)));
    QVERIFY(!firstMap.visible);

    QVERIFY(targets.selectTarget(3, 42, 1));
    const VehicleTargetLease nextEpoch = targets.acquireTarget();
    QVERIFY(store.update(nextEpoch, fix(35.5, 33.6, 46.0, 101)));
    QVERIFY(firstMap.visible);

    firstMap.onClear = [&controller, &secondMap]() {
        controller.attachMap(&secondMap);
    };
    controller.detachMap();
    QCOMPARE(controller.attachedMap(),
             static_cast<AbstractMapWidget *>(&secondMap));
    QVERIFY(!firstMap.visible);
    QVERIFY(secondMap.visible);
    QCOMPARE(secondMap.lastPosition.latitude, 35.5);

    secondMap.onClear = [&controller]() { controller.detachMap(); };
    controller.attachMap(&firstMap);
    QVERIFY(!controller.attachedMap());
    QVERIFY(!firstMap.visible);
    QVERIFY(!secondMap.visible);
}

void MovingBaseMapControllerTest::
sourceDestructionClearsTheAttachedMap()
{
    auto *targets = new VehicleTargetManager;
    QVERIFY(targets->observeEndpoint(endpoint(3), true));
    auto *store = new MovingBasePositionStore(targets);
    const VehicleTargetLease lease = targets->acquireTarget();
    QVERIFY(store->update(lease, fix(35.1, 33.2, 42.0, 100)));

    MovingBaseMapController controller(store, targets);
    FakeMapBackend map;
    controller.attachMap(&map);
    QVERIFY(map.visible);

    delete targets;
    QVERIFY(!map.visible);
    QCOMPARE(controller.attachedMap(),
             static_cast<AbstractMapWidget *>(&map));
    delete store;
    QVERIFY(!map.visible);
}

void MovingBaseMapControllerTest::
destructionDoesNotRetainOrDereferenceBackends()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    MovingBasePositionStore store(&targets);
    const VehicleTargetLease lease = targets.acquireTarget();
    QVERIFY(store.update(lease, fix(35.1, 33.2, 42.0, 100)));

    auto *controller = new MovingBaseMapController(&store, &targets);
    auto *deletedMap = new FakeMapBackend;
    controller->attachMap(deletedMap);
    delete deletedMap;
    QVERIFY(!controller->attachedMap());
    QVERIFY(store.update(lease, fix(35.2, 33.3, 43.0, 101)));

    FakeMapBackend survivingMap;
    controller->attachMap(&survivingMap);
    QVERIFY(survivingMap.visible);
    const int clearCountBeforeDelete = survivingMap.clearCount;
    delete controller;
    QVERIFY(!survivingMap.visible);
    QCOMPARE(survivingMap.clearCount, clearCountBeforeDelete + 1);
    QVERIFY(store.update(lease, fix(35.3, 33.4, 44.0, 102)));

    QPointer<MovingBaseMapController> guardedController =
        new MovingBaseMapController(&store, &targets);
    FakeMapBackend reentrantMap;
    reentrantMap.onSet = [&guardedController]() {
        delete guardedController.data();
    };
    guardedController->attachMap(&reentrantMap);
    QVERIFY(guardedController.isNull());
    QVERIFY(!reentrantMap.visible);
}

QTEST_GUILESS_MAIN(MovingBaseMapControllerTest)
#include "test_movingbasemapcontroller.moc"
