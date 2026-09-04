#include "comm/SwarmTelemetryRegistry.h"

#include <QPointer>
#include <QSignalSpy>
#include <QtTest>

#include <cmath>
#include <limits>

namespace
{

VehicleEndpoint endpoint(int linkId, int systemId)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = MAV_COMP_ID_AUTOPILOT1;
    value.linkName = QStringLiteral("Link %1").arg(linkId);
    value.componentName = QStringLiteral("AUTOPILOT1");
    return value;
}

mavlink_message_t heartbeat(
    int systemId, int componentId = MAV_COMP_ID_AUTOPILOT1,
    int autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA,
    int vehicleType = MAV_TYPE_QUADROTOR, bool armed = false)
{
    mavlink_heartbeat_t payload{};
    payload.autopilot = static_cast<quint8>(autopilot);
    payload.type = static_cast<quint8>(vehicleType);
    payload.base_mode = armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0;
    payload.custom_mode = 17;
    payload.system_status = MAV_STATE_ACTIVE;
    payload.mavlink_version = 3;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(
        static_cast<quint8>(systemId), static_cast<quint8>(componentId),
        &message, &payload);
    return message;
}

mavlink_message_t globalPosition(int systemId, quint32 bootTimeMs = 1000)
{
    mavlink_global_position_int_t payload{};
    payload.time_boot_ms = bootTimeMs;
    payload.lat = 351234567;
    payload.lon = 331234567;
    payload.alt = 123450;
    payload.relative_alt = 23450;
    payload.vx = 125;
    payload.vy = -250;
    payload.vz = 75;
    payload.hdg = 1234;
    mavlink_message_t message{};
    mavlink_msg_global_position_int_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_message_t vfrHud(int systemId, int heading = 91,
                         int throttle = 42)
{
    mavlink_vfr_hud_t payload{};
    payload.airspeed = 12.5F;
    payload.groundspeed = 10.25F;
    payload.alt = 124.0F;
    payload.climb = -0.75F;
    payload.heading = static_cast<qint16>(heading);
    payload.throttle = static_cast<quint16>(throttle);
    mavlink_message_t message{};
    mavlink_msg_vfr_hud_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_message_t attitude(int systemId, quint32 bootTimeMs = 1000)
{
    mavlink_attitude_t payload{};
    payload.time_boot_ms = bootTimeMs;
    payload.roll = 0.1F;
    payload.pitch = -0.2F;
    payload.yaw = 1.5F;
    payload.rollspeed = 0.01F;
    payload.pitchspeed = -0.02F;
    payload.yawspeed = 0.03F;
    mavlink_message_t message{};
    mavlink_msg_attitude_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_message_t extendedState(int systemId)
{
    mavlink_extended_sys_state_t payload{};
    payload.vtol_state = MAV_VTOL_STATE_MC;
    payload.landed_state = MAV_LANDED_STATE_IN_AIR;
    mavlink_message_t message{};
    mavlink_msg_extended_sys_state_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

bool near(double actual, double expected)
{
    return std::abs(actual - expected) < 1.0e-6;
}

} // namespace

class SwarmTelemetryRegistryTest final : public QObject
{
    Q_OBJECT

private slots:
    void activationRequiresCommandCapableAutopilotHeartbeat();
    void parsesRequiredTelemetryIntoInstanceSnapshot();
    void duplicateSystemIdsAcrossLinksRemainDistinct();
    void reconnectingSameLinkInvalidatesOldInstance();
    void staleHeartbeatRetiresAndRequiresNewHeartbeat();
    void bootTimeResetRetiresAndRequiresNewHeartbeat();
    void capacityIsBoundedToTwentyFourAutopilots();
    void groupValidationIsAllOrNothing();
    void invalidVfrValuesClearValidity();
    void synchronousSignalsMayReenterOrDeleteRegistry();
};

void SwarmTelemetryRegistryTest::
activationRequiresCommandCapableAutopilotHeartbeat()
{
    qint64 now = 0;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 session = registry.beginLinkSession(4, QStringLiteral("UDP"));

    QVERIFY(!registry.observeMessage(4, session, globalPosition(7)));
    QVERIFY(!registry.observeMessage(
        4, session, heartbeat(7, MAV_COMP_ID_MISSIONPLANNER)));
    QVERIFY(!registry.observeMessage(4, session, heartbeat(7, 42)));
    QVERIFY(!registry.observeMessage(
        4, session, heartbeat(7, MAV_COMP_ID_AUTOPILOT1,
                              MAV_AUTOPILOT_INVALID)));
    QVERIFY(!registry.observeMessage(
        4, session, heartbeat(7, MAV_COMP_ID_AUTOPILOT1,
                              MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_GCS)));
    QCOMPARE(registry.endpointCount(), 0);

    QVERIFY(registry.observeMessage(4, session, heartbeat(7)));
    QCOMPARE(registry.endpoints().size(), 1);
    QVERIFY(registry.endpoints().constFirst().sameIdentity(endpoint(4, 7)));
    const SwarmVehicleInstanceLease lease =
        registry.acquireVehicle(endpoint(4, 7));
    QVERIFY(lease.isValid());

    QSignalSpy retired(&registry, &SwarmTelemetryRegistry::endpointRetired);
    QVERIFY(!registry.observeMessage(
        4, session, heartbeat(7, MAV_COMP_ID_AUTOPILOT1,
                              MAV_AUTOPILOT_INVALID)));
    QCOMPARE(registry.endpointCount(), 0);
    QVERIFY(!registry.validateLease(lease));
    QCOMPARE(retired.count(), 1);
    QCOMPARE(qvariant_cast<SwarmTelemetryRegistry::RetirementReason>(
                 retired.at(0).at(1)),
             SwarmTelemetryRegistry::RetirementReason::NoLongerCommandCapable);
}

void SwarmTelemetryRegistryTest::parsesRequiredTelemetryIntoInstanceSnapshot()
{
    qint64 now = 100;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 session = registry.beginLinkSession(2, QStringLiteral("TCP"));
    QVERIFY(registry.observeMessage(2, session, heartbeat(42,
        MAV_COMP_ID_AUTOPILOT1, MAV_AUTOPILOT_ARDUPILOTMEGA,
        MAV_TYPE_QUADROTOR, true)));
    now = 110;
    QVERIFY(registry.observeMessage(2, session, globalPosition(42)));
    now = 120;
    QVERIFY(registry.observeMessage(2, session, vfrHud(42)));
    now = 130;
    QVERIFY(registry.observeMessage(2, session, attitude(42)));
    now = 140;
    QVERIFY(registry.observeMessage(2, session, extendedState(42)));

    SwarmTelemetrySnapshot snapshot;
    QVERIFY(registry.acquireSnapshot(endpoint(2, 42), &snapshot));
    QVERIFY(snapshot.heartbeatValid);
    QVERIFY(snapshot.armed);
    QCOMPARE(snapshot.customMode, quint32(17));
    QCOMPARE(snapshot.heartbeatObservedMs, qint64(100));
    QVERIFY(snapshot.positionValid);
    QVERIFY(snapshot.velocityValid);
    QVERIFY(near(snapshot.latitudeDegrees, 35.1234567));
    QVERIFY(near(snapshot.longitudeDegrees, 33.1234567));
    QVERIFY(near(snapshot.altitudeAmslM, 123.45));
    QVERIFY(near(snapshot.relativeAltitudeM, 23.45));
    QVERIFY(near(snapshot.velocityNorthMps, 1.25));
    QVERIFY(near(snapshot.velocityEastMps, -2.5));
    QVERIFY(near(snapshot.velocityDownMps, 0.75));
    QVERIFY(snapshot.vfrHudValid);
    QVERIFY(near(snapshot.airspeedMps, 12.5));
    QVERIFY(near(snapshot.groundSpeedMps, 10.25));
    QCOMPARE(snapshot.throttlePercent, 42);
    QVERIFY(snapshot.headingValid);
    QVERIFY(near(snapshot.headingDegrees, 91.0));
    QVERIFY(snapshot.attitudeValid);
    QVERIFY(near(snapshot.rollRadians, 0.1));
    QVERIFY(near(snapshot.pitchRadians, -0.2));
    QVERIFY(near(snapshot.yawRadians, 1.5));
    QVERIFY(snapshot.extendedSystemStateValid);
    QCOMPARE(snapshot.vtolState, int(MAV_VTOL_STATE_MC));
    QCOMPARE(snapshot.landedState, int(MAV_LANDED_STATE_IN_AIR));
    QCOMPARE(snapshot.lastMessageMs, qint64(140));
}

void SwarmTelemetryRegistryTest::duplicateSystemIdsAcrossLinksRemainDistinct()
{
    qint64 now = 0;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 firstSession = registry.beginLinkSession(3, QStringLiteral("COM3"));
    const quint64 secondSession = registry.beginLinkSession(9, QStringLiteral("TCP"));
    QVERIFY(registry.observeMessage(3, firstSession, heartbeat(42)));
    QVERIFY(registry.observeMessage(9, secondSession, heartbeat(42)));

    QCOMPARE(registry.endpointCount(), 2);
    const SwarmVehicleInstanceLease first = registry.acquireVehicle(endpoint(3, 42));
    const SwarmVehicleInstanceLease second = registry.acquireVehicle(endpoint(9, 42));
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(first != second);
    QVERIFY(first.endpoint.systemId == second.endpoint.systemId);

    const SwarmVehicleGroupLease group = registry.acquireGroup(
        {endpoint(3, 42), endpoint(9, 42)});
    QVERIFY(group.isValid());
    QCOMPARE(group.members.size(), 2);
    QVERIFY(registry.validateGroup(group));
}

void SwarmTelemetryRegistryTest::reconnectingSameLinkInvalidatesOldInstance()
{
    qint64 now = 0;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 firstSession = registry.beginLinkSession(5, QStringLiteral("UDP"));
    QVERIFY(registry.observeMessage(5, firstSession, heartbeat(8)));
    const SwarmVehicleInstanceLease oldLease = registry.acquireVehicle(endpoint(5, 8));

    const quint64 secondSession = registry.beginLinkSession(5, QStringLiteral("UDP"));
    QVERIFY(secondSession != firstSession);
    QVERIFY(!registry.validateLease(oldLease));
    QCOMPARE(registry.endpointCount(), 0);
    QVERIFY(!registry.endLinkSession(5, firstSession));
    QVERIFY(!registry.observeMessage(5, firstSession, heartbeat(8)));
    QVERIFY(!registry.observeMessage(5, secondSession, globalPosition(8)));
    QVERIFY(registry.observeMessage(5, secondSession, heartbeat(8)));

    const SwarmVehicleInstanceLease newLease = registry.acquireVehicle(endpoint(5, 8));
    QVERIFY(newLease.isValid());
    QVERIFY(newLease.linkSessionEpoch != oldLease.linkSessionEpoch);
    QVERIFY(newLease.instanceEpoch != oldLease.instanceEpoch);
}

void SwarmTelemetryRegistryTest::staleHeartbeatRetiresAndRequiresNewHeartbeat()
{
    qint64 now = 0;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    registry.setHeartbeatMaximumAgeForTesting(100);
    const quint64 session = registry.beginLinkSession(1);
    QVERIFY(registry.observeMessage(1, session, heartbeat(11)));
    const SwarmVehicleInstanceLease oldLease = registry.acquireVehicle(endpoint(1, 11));

    now = 100;
    QVERIFY(registry.validateLease(oldLease));
    now = 101;
    QVERIFY(!registry.validateLease(oldLease));
    QCOMPARE(registry.retireStaleEndpoints(), 1);
    QCOMPARE(registry.endpointCount(), 0);
    QVERIFY(!registry.observeMessage(1, session, globalPosition(11)));
    QVERIFY(registry.observeMessage(1, session, heartbeat(11)));

    const SwarmVehicleInstanceLease newLease = registry.acquireVehicle(endpoint(1, 11));
    QVERIFY(newLease.isValid());
    QVERIFY(newLease.instanceEpoch != oldLease.instanceEpoch);
}

void SwarmTelemetryRegistryTest::bootTimeResetRetiresAndRequiresNewHeartbeat()
{
    qint64 now = 0;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 session = registry.beginLinkSession(12);
    QVERIFY(registry.observeMessage(12, session, heartbeat(44)));
    QVERIFY(registry.observeMessage(12, session,
                                    globalPosition(44, 90000)));
    const SwarmVehicleInstanceLease oldLease =
        registry.acquireVehicle(endpoint(12, 44));
    QVERIFY(oldLease.isValid());

    // Cross-stream reordering below the conservative threshold is an ordinary
    // sample, not a reboot.
    QVERIFY(registry.observeMessage(12, session, attitude(44, 89500)));
    QVERIFY(registry.validateLease(oldLease));

    // Establish a normal post-wrap high-water mark, then regress far enough
    // to identify a quick autopilot restart on the unchanged physical link.
    QVERIFY(registry.observeMessage(12, session,
                                    globalPosition(44, 120000)));
    QSignalSpy retired(&registry, &SwarmTelemetryRegistry::endpointRetired);
    QVERIFY(!registry.observeMessage(12, session, attitude(44, 1000)));
    QCOMPARE(registry.endpointCount(), 0);
    QVERIFY(!registry.validateLease(oldLease));
    QCOMPARE(retired.count(), 1);
    QCOMPARE(qvariant_cast<SwarmTelemetryRegistry::RetirementReason>(
                 retired.at(0).at(1)),
             SwarmTelemetryRegistry::RetirementReason::BootTimeReset);

    QVERIFY(!registry.observeMessage(12, session,
                                     globalPosition(44, 1100)));
    QVERIFY(registry.observeMessage(12, session, heartbeat(44)));
    QVERIFY(registry.observeMessage(12, session,
                                    globalPosition(44, 1200)));
    const SwarmVehicleInstanceLease replacement =
        registry.acquireVehicle(endpoint(12, 44));
    QVERIFY(replacement.isValid());
    QVERIFY(replacement.instanceEpoch != oldLease.instanceEpoch);

    SwarmTelemetryRegistry wrapRegistry([&now]() { return now; });
    const quint64 wrapSession = wrapRegistry.beginLinkSession(14);
    QVERIFY(wrapRegistry.observeMessage(14, wrapSession, heartbeat(46)));
    QVERIFY(wrapRegistry.observeMessage(
        14, wrapSession,
        globalPosition(46, std::numeric_limits<quint32>::max() - 10)));
    const SwarmVehicleInstanceLease wrapLease =
        wrapRegistry.acquireVehicle(endpoint(14, 46));
    QVERIFY(wrapRegistry.observeMessage(
        14, wrapSession, attitude(46, 25)));
    QVERIFY(wrapRegistry.validateLease(wrapLease));
}

void SwarmTelemetryRegistryTest::capacityIsBoundedToTwentyFourAutopilots()
{
    qint64 now = 0;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 session = registry.beginLinkSession(7);
    for (int systemId = 1;
         systemId <= SwarmTelemetryRegistry::MaximumVehicleEndpoints;
         ++systemId) {
        QVERIFY(registry.observeMessage(7, session, heartbeat(systemId)));
    }
    QCOMPARE(registry.endpointCount(),
             SwarmTelemetryRegistry::MaximumVehicleEndpoints);
    QVERIFY(!registry.observeMessage(
        7, session,
        heartbeat(SwarmTelemetryRegistry::MaximumVehicleEndpoints + 1)));
    QCOMPARE(registry.endpointCount(),
             SwarmTelemetryRegistry::MaximumVehicleEndpoints);
    QVERIFY(registry.observeMessage(7, session, heartbeat(1)));
}

void SwarmTelemetryRegistryTest::groupValidationIsAllOrNothing()
{
    qint64 now = 0;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 firstSession = registry.beginLinkSession(1);
    const quint64 secondSession = registry.beginLinkSession(2);
    QVERIFY(registry.observeMessage(1, firstSession, heartbeat(21)));
    QVERIFY(registry.observeMessage(2, secondSession, heartbeat(22)));
    const SwarmVehicleGroupLease group = registry.acquireGroup(
        {endpoint(1, 21), endpoint(2, 22)}, 100);
    QVERIFY(group.isValid());

    QList<SwarmTelemetrySnapshot> snapshots;
    QVERIFY(registry.validateGroup(group, &snapshots, 100));
    QCOMPARE(snapshots.size(), 2);
    now = 80;
    QVERIFY(registry.observeMessage(1, firstSession, heartbeat(21)));
    now = 101;
    snapshots = {SwarmTelemetrySnapshot()};
    QVERIFY(!registry.validateGroup(group, &snapshots, 100));
    QVERIFY(snapshots.isEmpty()); // Failure never publishes stale or prefix data.

    const quint64 replacementSession = registry.beginLinkSession(1);
    QVERIFY(replacementSession != firstSession);
    QVERIFY(!registry.validateGroup(group, nullptr, 100));
    QVERIFY(!registry.acquireGroup(
        {endpoint(2, 22), endpoint(2, 22)}, 100).isValid());

    QList<VehicleEndpoint> oversized;
    for (int systemId = 1;
         systemId <= SwarmTelemetryRegistry::MaximumVehicleEndpoints + 1;
         ++systemId) {
        oversized.append(endpoint(2, systemId));
    }
    QVERIFY(!registry.acquireGroup(oversized, 100).isValid());
}

void SwarmTelemetryRegistryTest::invalidVfrValuesClearValidity()
{
    qint64 now = 0;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 session = registry.beginLinkSession(13);
    QVERIFY(registry.observeMessage(13, session, heartbeat(45)));
    QVERIFY(registry.observeMessage(13, session, vfrHud(45)));

    SwarmTelemetrySnapshot snapshot;
    QVERIFY(registry.acquireSnapshot(endpoint(13, 45), &snapshot));
    QVERIFY(snapshot.vfrHudValid);
    QVERIFY(snapshot.headingValid);

    QVERIFY(registry.observeMessage(13, session, vfrHud(45, -1, 101)));
    QVERIFY(registry.acquireSnapshot(endpoint(13, 45), &snapshot));
    QVERIFY(!snapshot.vfrHudValid);
    QVERIFY(!snapshot.headingValid);
    QCOMPARE(snapshot.throttlePercent, 42);
}

void SwarmTelemetryRegistryTest::synchronousSignalsMayReenterOrDeleteRegistry()
{
    qint64 now = 0;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 session = registry.beginLinkSession(6);
    QSignalSpy retired(&registry, &SwarmTelemetryRegistry::endpointRetired);
    bool nestedEndSucceeded = false;
    QList<quint64> publishedRevisions;
    connect(&registry, &SwarmTelemetryRegistry::registryChanged,
            &registry, [&](qulonglong revision) {
        publishedRevisions.append(revision);
        QCOMPARE(revision, registry.revision());
    });
    connect(&registry, &SwarmTelemetryRegistry::endpointActivated,
            &registry, [&](const SwarmTelemetrySnapshot &) {
        nestedEndSucceeded = registry.endLinkSession(6, session);
    });
    QVERIFY(registry.observeMessage(6, session, heartbeat(31)));
    QVERIFY(nestedEndSucceeded);
    QCOMPARE(registry.endpointCount(), 0);
    QCOMPARE(retired.count(), 1);
    QVERIFY(!publishedRevisions.isEmpty());
    QCOMPARE(publishedRevisions.constLast(), registry.revision());

    auto *doomed = new SwarmTelemetryRegistry([&now]() { return now; });
    const quint64 doomedSession = doomed->beginLinkSession(8);
    QPointer<SwarmTelemetryRegistry> guard(doomed);
    connect(doomed, &SwarmTelemetryRegistry::endpointActivated,
            doomed, [doomed](const SwarmTelemetrySnapshot &) {
        delete doomed;
    });
    QVERIFY(doomed->observeMessage(8, doomedSession, heartbeat(32)));
    QVERIFY(guard.isNull());
}

QTEST_MAIN(SwarmTelemetryRegistryTest)

#include "test_swarmtelemetryregistry.moc"
