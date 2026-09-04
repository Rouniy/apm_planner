#include "comm/MovingBasePositionStore.h"
#include "comm/VehicleTargetManager.h"

#include <QSignalSpy>
#include <QtTest>

#include <limits>

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

MovingBasePositionSnapshot snapshotOf(const QSignalSpy &spy, int index)
{
    return qvariant_cast<MovingBasePositionSnapshot>(
        spy.at(index).at(0));
}

} // namespace

class MovingBasePositionStoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void validatesTypedFixAndFormatsTag();
    void isolatesDuplicateIdsByExactLease();
    void rejectsStaleGenerationAndTimestamp();
    void clearAndEndpointRemovalPublishLifecycle();
    void reentrantTargetChangeNeverPublishesOldFixAsCurrent();
};

void MovingBasePositionStoreTest::validatesTypedFixAndFormatsTag()
{
    MovingBasePositionFix value = fix(0.0, 0.0, 14.25, 7);
    QVERIFY(value.isValid());
    QCOMPARE(value.displayTag(), QStringLiteral("Sats 12 hdop 0.8"));

    value.hdop = 1.25;
    QCOMPARE(value.displayTag(), QStringLiteral("Sats 12 hdop 1.25"));
    value.latitudeDegrees = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!value.isValid());
    value = fix(91.0, 0.0, 0.0, 0);
    QVERIFY(!value.isValid());
    value = fix(1.0, 2.0, 3.0, -1);
    QVERIFY(!value.isValid());
}

void MovingBasePositionStoreTest::isolatesDuplicateIdsByExactLease()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    QVERIFY(targets.observeEndpoint(endpoint(9)));
    MovingBasePositionStore store(&targets);

    const VehicleTargetLease first = targets.acquireTarget();
    QVERIFY(store.update(first, fix(35.1, 33.2, 42.0, 100)));
    QVERIFY(store.snapshot(first).isValid());

    QVERIFY(targets.selectTarget(9, 42, 1));
    const VehicleTargetLease second = targets.acquireTarget();
    QVERIFY(second.endpoint.systemId == first.endpoint.systemId);
    QVERIFY(second.endpoint.linkId != first.endpoint.linkId);
    QVERIFY(!store.currentSnapshot().isValid());
    QVERIFY(store.update(second, fix(36.1, 34.2, 52.0, 110)));

    QCOMPARE(store.snapshot(first).fix.latitudeDegrees, 35.1);
    QCOMPARE(store.snapshot(second).fix.latitudeDegrees, 36.1);
    QCOMPARE(store.currentSnapshot().target.endpoint.linkId, 9);
}

void MovingBasePositionStoreTest::rejectsStaleGenerationAndTimestamp()
{
    VehicleTargetManager targets;
    const VehicleEndpoint firstEndpoint = endpoint(3);
    QVERIFY(targets.observeEndpoint(firstEndpoint, true));
    QVERIFY(targets.observeEndpoint(endpoint(9)));
    MovingBasePositionStore store(&targets);

    const VehicleTargetLease first = targets.acquireTarget();
    QVERIFY(store.update(first, fix(35.1, 33.2, 42.0, 100)));
    QVERIFY(!store.update(first, fix(35.2, 33.3, 43.0, 99)));
    QCOMPARE(store.snapshot(first).fix.observedMonotonicMs, 100);

    QVERIFY(targets.selectTarget(9, 42, 1));
    QVERIFY(!store.update(first, fix(35.3, 33.4, 44.0, 101)));
    QVERIFY(!store.clear(first));

    QVERIFY(targets.selectTarget(firstEndpoint.linkId,
                                 firstEndpoint.systemId,
                                 firstEndpoint.componentId));
    const VehicleTargetLease nextEpoch = targets.acquireTarget();
    QVERIFY(nextEpoch.generation > first.generation);
    QVERIFY(!store.snapshot(nextEpoch).isValid());
    QVERIFY(!store.update(first, fix(35.4, 33.5, 45.0, 102)));
    QVERIFY(store.update(nextEpoch, fix(35.5, 33.6, 46.0, 103)));
    QVERIFY(!store.snapshot(first).isValid());
    QCOMPARE(store.snapshot(nextEpoch).fix.latitudeDegrees, 35.5);
}

void MovingBasePositionStoreTest::clearAndEndpointRemovalPublishLifecycle()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    MovingBasePositionStore store(&targets);
    QSignalSpy updated(&store, &MovingBasePositionStore::positionUpdated);
    QSignalSpy cleared(&store, &MovingBasePositionStore::positionCleared);
    QSignalSpy current(
        &store, &MovingBasePositionStore::currentSnapshotChanged);

    VehicleTargetLease lease = targets.acquireTarget();
    QVERIFY(store.update(lease, fix(35.1, 33.2, 42.0, 100)));
    QCOMPARE(updated.count(), 1);
    QCOMPARE(current.count(), 1);
    QVERIFY(snapshotOf(current, 0).isValid());
    QVERIFY(store.clearCurrent());
    QCOMPARE(cleared.count(), 1);
    QCOMPARE(current.count(), 2);
    QVERIFY(!snapshotOf(current, 1).isValid());
    QVERIFY(!store.clearCurrent());

    QVERIFY(store.update(lease, fix(35.3, 33.4, 44.0, 101)));
    QCOMPARE(updated.count(), 2);
    QVERIFY(targets.removeLink(3));
    QCOMPARE(cleared.count(), 2);
    QVERIFY(!store.snapshot(lease).isValid());
    QVERIFY(!store.currentSnapshot().isValid());
}

void MovingBasePositionStoreTest::
reentrantTargetChangeNeverPublishesOldFixAsCurrent()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    QVERIFY(targets.observeEndpoint(endpoint(9)));
    MovingBasePositionStore store(&targets);
    const VehicleTargetLease first = targets.acquireTarget();
    QSignalSpy current(
        &store, &MovingBasePositionStore::currentSnapshotChanged);

    connect(&store, &MovingBasePositionStore::positionUpdated,
            &store, [&targets](const MovingBasePositionSnapshot &) {
                targets.selectTarget(9, 42, 1);
            });
    QVERIFY(store.update(first, fix(35.1, 33.2, 42.0, 100)));
    QCOMPARE(current.count(), 1);
    QVERIFY(!snapshotOf(current, 0).isValid());
    QCOMPARE(targets.acquireTarget().endpoint.linkId, 9);
}

QTEST_GUILESS_MAIN(MovingBasePositionStoreTest)
#include "test_movingbasepositionstore.moc"
