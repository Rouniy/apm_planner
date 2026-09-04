#include "comm/VehicleEndpoint.h"
#include "comm/VehicleTargetManager.h"

#include <QSet>
#include <QSignalSpy>
#include <QtTest>

namespace
{

VehicleEndpoint endpoint(int linkId, int systemId, int componentId,
                         const QString &linkName = QString(),
                         const QString &componentName = QString())
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = linkName;
    result.componentName = componentName;
    return result;
}

} // namespace

class VehicleTargetManagerTest final : public QObject
{
    Q_OBJECT

private slots:
    void endpointIdentityIncludesLinkSystemAndComponent();
    void discoveryMembershipMatchesMissionPlannerMasterList();
    void duplicateSystemIdsAcrossLinksStayIndependent();
    void visibleComponentsHaveStableNumericOrderAndLabels();
    void observationIsIdempotentAndMetadataCanChange();
    void selectionLeaseRejectsStaleGeneration();
    void heartbeatFreshnessBelongsToTheSelectedTargetEpoch();
    void nestedSelectionSignalsStayOrderedAndSettleOnce();
    void removingSelectedLinkInvalidatesWithoutFallback();
    void missionPlannerComponentRemainsObservableButIsNotAutoSelected();
    void variantContractIsDirectlyUsableFromQml();
};

void VehicleTargetManagerTest::endpointIdentityIncludesLinkSystemAndComponent()
{
    const VehicleEndpoint first = endpoint(1, 42, 1, QStringLiteral("UDP"));
    const VehicleEndpoint sameIdentity = endpoint(
        1, 42, 1, QStringLiteral("renamed"), QStringLiteral("QUADROTOR"));
    const VehicleEndpoint otherLink = endpoint(2, 42, 1);
    const VehicleEndpoint otherComponent = endpoint(1, 42, 100);

    QVERIFY(first.isValid());
    QVERIFY(endpoint(1, 42, 0).isValid());
    QVERIFY(!endpoint(1, 0, 1).isValid());
    QCOMPARE(first, sameIdentity);
    QVERIFY(first != otherLink);
    QVERIFY(first != otherComponent);

    QSet<VehicleEndpoint> keys;
    keys.insert(first);
    keys.insert(sameIdentity);
    keys.insert(otherLink);
    keys.insert(otherComponent);
    QCOMPARE(keys.size(), 3);
}

void VehicleTargetManagerTest::discoveryMembershipMatchesMissionPlannerMasterList()
{
    QVERIFY(VehicleTargetManager::isVisibleDiscoveryMessage(0));
    QVERIFY(VehicleTargetManager::isVisibleDiscoveryMessage(235));
    QVERIFY(VehicleTargetManager::isVisibleDiscoveryMessage(310));
    QVERIFY(!VehicleTargetManager::isVisibleDiscoveryMessage(22));
    QVERIFY(!VehicleTargetManager::isVisibleDiscoveryMessage(253));
}

void VehicleTargetManagerTest::duplicateSystemIdsAcrossLinksStayIndependent()
{
    VehicleTargetManager manager;
    manager.observeEndpoint(endpoint(9, 42, 1, QStringLiteral("TCP")), true);
    manager.observeEndpoint(endpoint(3, 42, 1, QStringLiteral("COM4")), true);

    QCOMPARE(manager.endpoints().size(), 2);
    QCOMPARE(manager.endpoints().at(0).linkId, 3);
    QCOMPARE(manager.endpoints().at(1).linkId, 9);
    QVERIFY(manager.contains(3, 42, 1));
    QVERIFY(manager.contains(9, 42, 1));

    QVERIFY(manager.selectTarget(9, 42, 1));
    const VehicleTargetLease selected = manager.acquireTarget();
    QVERIFY(selected.isValid());
    QCOMPARE(selected.endpoint.linkId, 9);
    QVERIFY(manager.isCurrentTarget(9, 42, 1, selected.generation));
    QVERIFY(!manager.isCurrentTarget(3, 42, 1, selected.generation));
}

void VehicleTargetManagerTest::visibleComponentsHaveStableNumericOrderAndLabels()
{
    VehicleTargetManager manager;
    manager.observeEndpoint(endpoint(4, 8, 154, QStringLiteral("UDP")));
    manager.observeEndpoint(endpoint(4, 8, 100, QStringLiteral("UDP")));
    manager.observeEndpoint(endpoint(4, 8, 1, QStringLiteral("UDP"),
                                     QStringLiteral("QUADROTOR")));
    manager.observeEndpoint(endpoint(4, 3, 1, QStringLiteral("UDP"),
                                     QStringLiteral("FIXED_WING")));

    const QList<VehicleEndpoint> endpoints = manager.endpoints();
    QCOMPARE(endpoints.size(), 4);
    QCOMPARE(endpoints.at(0).systemId, 3);
    QCOMPARE(endpoints.at(1).componentId, 1);
    QCOMPARE(endpoints.at(2).componentId, 100);
    QCOMPARE(endpoints.at(3).componentId, 154);
    QCOMPARE(endpoints.at(0).displayName(),
             QStringLiteral("UDP-3-FIXED WING"));
    QCOMPARE(endpoints.at(2).displayName(),
             QStringLiteral("UDP-8-CAMERA"));
    QCOMPARE(endpoints.at(3).displayName(),
             QStringLiteral("UDP-8-GIMBAL"));
}

void VehicleTargetManagerTest::observationIsIdempotentAndMetadataCanChange()
{
    VehicleTargetManager manager;
    QSignalSpy added(&manager, &VehicleTargetManager::endpointAdded);
    QSignalSpy updated(&manager, &VehicleTargetManager::endpointUpdated);
    QSignalSpy changed(&manager, &VehicleTargetManager::endpointsChanged);

    const VehicleEndpoint initial = endpoint(
        1, 7, 1, QStringLiteral("ttyACM0"), QStringLiteral("GENERIC"));
    QVERIFY(manager.observeEndpoint(initial, true));
    QVERIFY(!manager.observeEndpoint(initial, true));
    QCOMPARE(added.count(), 1);
    QCOMPARE(updated.count(), 0);
    QCOMPARE(changed.count(), 1);
    const quint64 generation = manager.targetGeneration();

    VehicleEndpoint renamed = initial;
    renamed.componentName = QStringLiteral("QUADROTOR");
    QVERIFY(manager.observeEndpoint(renamed, true));
    QCOMPARE(added.count(), 1);
    QCOMPARE(updated.count(), 1);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(manager.targetGeneration(), generation);
    QCOMPARE(manager.acquireTarget().endpoint.componentName,
             QStringLiteral("QUADROTOR"));
}

void VehicleTargetManagerTest::selectionLeaseRejectsStaleGeneration()
{
    VehicleTargetManager manager;
    manager.observeEndpoint(endpoint(1, 1, 1), true);
    manager.observeEndpoint(endpoint(2, 1, 1));
    const VehicleTargetLease first = manager.acquireTarget();
    QVERIFY(first.isValid());

    QVERIFY(manager.selectTargetIfGeneration(
        2, 1, 1, first.generation));
    QCOMPARE(manager.targetGeneration(), first.generation + 1);
    QVERIFY(!manager.isCurrentTarget(
        first.endpoint.linkId, first.endpoint.systemId,
        first.endpoint.componentId, first.generation));
    QVERIFY(!manager.selectTargetIfGeneration(
        1, 1, 1, first.generation));
    QCOMPARE(manager.acquireTarget().endpoint.linkId, 2);
}

void VehicleTargetManagerTest::heartbeatFreshnessBelongsToTheSelectedTargetEpoch()
{
    VehicleTargetManager manager;
    const VehicleEndpoint first = endpoint(1, 1, 1);
    const VehicleEndpoint second = endpoint(2, 1, 1);
    QVERIFY(manager.observeEndpoint(first, true));
    QVERIFY(manager.observeEndpoint(second));

    VehicleTargetLease lease = manager.acquireTarget();
    manager.observeHeartbeat(first, false, 3, 2);
    QVERIFY(manager.hasFreshHeartbeat(lease, 3000));
    QCOMPARE(manager.heartbeatAutopilot(lease), 3);

    QVERIFY(manager.selectTarget(2, 1, 1));
    QVERIFY(manager.selectTarget(1, 1, 1));
    lease = manager.acquireTarget();
    QVERIFY(!manager.hasFreshHeartbeat(lease, 3000));
    QCOMPARE(manager.heartbeatAutopilot(lease), -1);

    manager.observeHeartbeat(first, true, 3, 2);
    QVERIFY(manager.hasFreshHeartbeat(lease, 3000));
    QVERIFY(manager.heartbeatArmed(lease));
}

void VehicleTargetManagerTest::nestedSelectionSignalsStayOrderedAndSettleOnce()
{
    VehicleTargetManager manager;
    QVERIFY(manager.observeEndpoint(endpoint(1, 1, 1), true));
    QVERIFY(manager.observeEndpoint(endpoint(2, 2, 1)));
    QVERIFY(manager.observeEndpoint(endpoint(3, 3, 1)));
    const quint64 initialGeneration = manager.targetGeneration();

    connect(&manager, &VehicleTargetManager::targetGenerationChanged,
            &manager, [&manager, initialGeneration](qulonglong generation) {
        if (generation == initialGeneration + 1) {
            QVERIFY(manager.selectTarget(3, 3, 1));
        }
    });
    QSignalSpy generations(
        &manager, &VehicleTargetManager::targetGenerationChanged);
    QSignalSpy settled(
        &manager, &VehicleTargetManager::targetGenerationSettled);
    QSignalSpy currentChanges(
        &manager, &VehicleTargetManager::currentTargetChanged);

    QVERIFY(manager.selectTarget(2, 2, 1));
    QCOMPARE(manager.acquireTarget().endpoint.linkId, 3);
    QCOMPARE(generations.count(), 2);
    QCOMPARE(generations.at(0).at(0).toULongLong(),
             initialGeneration + 1);
    QCOMPARE(generations.at(1).at(0).toULongLong(),
             initialGeneration + 2);
    QCOMPARE(settled.count(), 1);
    QCOMPARE(settled.constFirst().at(0).toULongLong(),
             initialGeneration + 2);
    QCOMPARE(currentChanges.count(), 1);
    QVERIFY(manager.isTargetGenerationSettled());
}

void VehicleTargetManagerTest::removingSelectedLinkInvalidatesWithoutFallback()
{
    VehicleTargetManager manager;
    manager.observeEndpoint(endpoint(1, 1, 1), true);
    manager.observeEndpoint(endpoint(2, 2, 1));
    const quint64 selectedGeneration = manager.targetGeneration();
    QSignalSpy removed(&manager, &VehicleTargetManager::endpointRemoved);
    QSignalSpy targetChanged(&manager,
                             &VehicleTargetManager::currentTargetChanged);

    bool committedSnapshotSeen = false;
    connect(&manager, &VehicleTargetManager::endpointsChanged,
            &manager, [&manager, &committedSnapshotSeen]() {
                committedSnapshotSeen = !manager.hasCurrentTarget()
                    && !manager.contains(1, 1, 1)
                    && manager.contains(2, 2, 1);
            });

    QVERIFY(manager.removeLink(1));
    QVERIFY(committedSnapshotSeen);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(targetChanged.count(), 1);
    QVERIFY(!manager.acquireTarget().isValid());
    QCOMPARE(manager.targetGeneration(), selectedGeneration + 1);
    QVERIFY(manager.contains(2, 2, 1));

    QVERIFY(!manager.removeLink(1));
    QCOMPARE(removed.count(), 1);
    QCOMPARE(targetChanged.count(), 1);
}

void VehicleTargetManagerTest::missionPlannerComponentRemainsObservableButIsNotAutoSelected()
{
    VehicleTargetManager manager;
    QVERIFY(manager.observeEndpoint(endpoint(
        7, 255, 190, QStringLiteral("UDP"),
        QStringLiteral("MISSIONPLANNER")), false));
    QVERIFY(manager.contains(7, 255, 190));
    QVERIFY(!manager.hasCurrentTarget());

    QVERIFY(manager.observeEndpoint(endpoint(
        7, 1, 1, QStringLiteral("UDP"), QStringLiteral("QUADROTOR")),
        true));
    QVERIFY(manager.hasCurrentTarget());
    QCOMPARE(manager.acquireTarget().endpoint.systemId, 1);
}

void VehicleTargetManagerTest::variantContractIsDirectlyUsableFromQml()
{
    VehicleTargetManager manager;
    QCOMPARE(manager.currentTargetVariant().value(
                 QStringLiteral("valid")).toBool(), false);
    manager.observeEndpoint(endpoint(
        12, 3, 100, QStringLiteral("TCP"), QStringLiteral("CAMERA")),
        true);

    const QVariantList endpoints = manager.endpointVariants();
    QCOMPARE(endpoints.size(), 1);
    const QVariantMap item = endpoints.first().toMap();
    QCOMPARE(item.value(QStringLiteral("linkId")).toInt(), 12);
    QCOMPARE(item.value(QStringLiteral("systemId")).toInt(), 3);
    QCOMPARE(item.value(QStringLiteral("componentId")).toInt(), 100);
    QCOMPARE(item.value(QStringLiteral("displayName")).toString(),
             QStringLiteral("TCP-3-CAMERA"));

    const QVariantMap selected = manager.currentTargetVariant();
    QVERIFY(selected.value(QStringLiteral("valid")).toBool());
    QCOMPARE(selected.value(QStringLiteral("generation")).toULongLong(),
             manager.targetGeneration());
}

QTEST_APPLESS_MAIN(VehicleTargetManagerTest)
#include "test_vehicletargetmanager.moc"
