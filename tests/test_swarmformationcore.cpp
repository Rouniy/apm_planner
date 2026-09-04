#include "ui/tools/SwarmFormationCore.h"

#include <QtTest>

#include <cmath>
#include <limits>

namespace
{

SwarmVehicleInstanceLease lease(int linkId, int systemId,
                                quint64 linkEpoch = 1,
                                quint64 instanceEpoch = 1)
{
    SwarmVehicleInstanceLease value;
    value.endpoint.linkId = linkId;
    value.endpoint.systemId = systemId;
    value.endpoint.componentId = MAV_COMP_ID_AUTOPILOT1;
    value.endpoint.linkName = QStringLiteral("Link %1").arg(linkId);
    value.linkSessionEpoch = linkEpoch;
    value.instanceEpoch = instanceEpoch;
    return value;
}

SwarmTelemetrySnapshot snapshot(
    const SwarmVehicleInstanceLease &vehicle, qint64 observedMs,
    int vehicleType = MAV_TYPE_QUADROTOR)
{
    SwarmTelemetrySnapshot value;
    value.lease = vehicle;
    value.heartbeatValid = true;
    value.heartbeatObservedMs = observedMs;
    value.lastMessageMs = observedMs;
    value.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    value.vehicleType = vehicleType;
    value.positionValid = true;
    value.positionObservedMs = observedMs;
    value.latitudeDegrees = 35.123456;
    value.longitudeDegrees = 33.654321;
    value.relativeAltitudeM = 100.0;
    value.velocityValid = true;
    value.velocityNorthMps = 1.25;
    value.velocityEastMps = -2.5;
    value.velocityDownMps = 0.3;
    value.headingValid = true;
    value.headingDegrees = 37.0;
    value.attitudeValid = true;
    value.attitudeObservedMs = observedMs;
    value.yawRadians = 37.0 * 3.14159265358979323846 / 180.0;
    return value;
}

bool near(double actual, double expected, double tolerance = 1.0e-5)
{
    return std::abs(actual - expected) <= tolerance;
}

} // namespace

class SwarmFormationCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void targetAndInversePreserveLeaderRelativeOffset();
    void validatesEveryLeaseAndFieldBeforeProducingCommands();
    void rejectsStaleConsumedTelemetry();
    void rejectsDuplicateAndReplacementInstances();
    void supportsOnlyExactArduPilotFamilies();
    void rejectsPlaneUntilAttitudeControllerIsPorted();
    void absoluteYawRotationAndAntimeridianAreCorrect();
    void heartbeatAndFollowerYawAreRequiredBeforeAnyCommand();
    void groupMembershipMustMatchExactly();
};

void SwarmFormationCoreTest::targetAndInversePreserveLeaderRelativeOffset()
{
    SwarmTelemetrySnapshot leader = snapshot(lease(1, 1), 100);
    leader.yawRadians = 237.0 * 3.14159265358979323846 / 180.0;
    const SwarmFormationOffset requested{-18.5, 42.25, 7.5};
    const SwarmFormationTarget target =
        SwarmFormationCore::targetFromLeader(leader, requested);
    SwarmTelemetrySnapshot follower = snapshot(lease(2, 2), 100);
    follower.latitudeDegrees = target.latitudeDegrees;
    follower.longitudeDegrees = target.longitudeDegrees;
    follower.relativeAltitudeM = target.relativeAltitudeM;
    const SwarmFormationOffset recovered =
        SwarmFormationCore::offsetFromLeader(leader, follower);

    QVERIFY(near(recovered.x, requested.x, 1.0e-4));
    QVERIFY(near(recovered.y, requested.y, 1.0e-4));
    QVERIFY(near(recovered.z, requested.z));
    QVERIFY(near(target.velocityNorthMps, 1.25));
    QVERIFY(near(target.velocityEastMps, -2.5));
    QVERIFY(near(target.velocityDownMps, 0.3));
    QVERIFY(near(target.yawDegrees, 237.0));
}

void SwarmFormationCoreTest::
validatesEveryLeaseAndFieldBeforeProducingCommands()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease firstLease = lease(2, 2);
    const SwarmVehicleInstanceLease secondLease = lease(3, 3);
    SwarmTelemetrySnapshot leader = snapshot(leaderLease, 100);
    SwarmTelemetrySnapshot first = snapshot(firstLease, 100);
    SwarmTelemetrySnapshot second = snapshot(secondLease, 100);
    SwarmFormationPlan plan;
    plan.leader = leaderLease;
    plan.followers = {
        {firstLease, {10.0, 0.0, 5.0}},
        {secondLease, {-10.0, 0.0, 5.0}}
    };

    SwarmFormationTick tick = SwarmFormationCore::buildTick(
        plan, {leader, first, second}, 100);
    QVERIFY2(tick.isValid(), qPrintable(tick.error));
    QCOMPARE(tick.commands.size(), 2);
    QVERIFY(tick.commands.at(0).follower.sameInstance(firstLease));

    plan.followers[1].offset.x = std::numeric_limits<double>::quiet_NaN();
    tick = SwarmFormationCore::buildTick(
        plan, {leader, first, second}, 100);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.commands.isEmpty());
    QVERIFY(tick.error.contains(QStringLiteral("invalid or excessive")));
}

void SwarmFormationCoreTest::rejectsStaleConsumedTelemetry()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease followerLease = lease(2, 2);
    SwarmTelemetrySnapshot leader = snapshot(leaderLease, 100);
    leader.heartbeatObservedMs = 5200;
    const SwarmTelemetrySnapshot follower = snapshot(followerLease, 6000);
    SwarmFormationPlan plan;
    plan.leader = leaderLease;
    plan.followers = {{followerLease, {5.0, 0.0, 0.0}}};

    SwarmFormationTick tick = SwarmFormationCore::buildTick(
        plan, {leader, follower}, 5200);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.error.contains(QStringLiteral("leader position")));
    QVERIFY(tick.commands.isEmpty());

    leader = snapshot(leaderLease, 5200);
    leader.velocityValid = false;
    tick = SwarmFormationCore::buildTick(
        plan, {leader, follower}, 5200);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.error.contains(QStringLiteral("leader velocity")));
}

void SwarmFormationCoreTest::rejectsDuplicateAndReplacementInstances()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease followerLease = lease(2, 2, 1, 8);
    const SwarmVehicleInstanceLease replacement = lease(2, 2, 2, 9);
    const SwarmTelemetrySnapshot leader = snapshot(leaderLease, 100);
    const SwarmTelemetrySnapshot follower = snapshot(replacement, 100);
    SwarmFormationPlan plan;
    plan.leader = leaderLease;
    plan.followers = {{followerLease, {5.0, 0.0, 0.0}}};

    SwarmFormationTick tick = SwarmFormationCore::buildTick(
        plan, {leader, follower}, 100);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.error.contains(QStringLiteral("reconnected")));
    QVERIFY(tick.commands.isEmpty());

    plan.followers = {{leaderLease, {5.0, 0.0, 0.0}}};
    tick = SwarmFormationCore::buildTick(plan, {leader}, 100);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.error.contains(QStringLiteral("leader was also")));
}

void SwarmFormationCoreTest::supportsOnlyExactArduPilotFamilies()
{
    SwarmTelemetrySnapshot vehicle = snapshot(lease(1, 1), 100);
    QVERIFY(SwarmFormationCore::supportsFormation(vehicle));
    QVERIFY(SwarmFormationCore::supportsPositionFollower(vehicle));

    vehicle.vehicleType = MAV_TYPE_GROUND_ROVER;
    QVERIFY(SwarmFormationCore::supportsFormation(vehicle));
    QVERIFY(SwarmFormationCore::supportsPositionFollower(vehicle));

    vehicle.vehicleType = MAV_TYPE_FIXED_WING;
    QVERIFY(SwarmFormationCore::supportsFormation(vehicle));
    QVERIFY(!SwarmFormationCore::supportsPositionFollower(vehicle));

    vehicle.autopilot = MAV_AUTOPILOT_PX4;
    QVERIFY(!SwarmFormationCore::supportsFormation(vehicle));
    vehicle.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    vehicle.vehicleType = MAV_TYPE_GCS;
    QVERIFY(!SwarmFormationCore::supportsFormation(vehicle));
}

void SwarmFormationCoreTest::rejectsPlaneUntilAttitudeControllerIsPorted()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease planeLease = lease(2, 2);
    const SwarmTelemetrySnapshot leader = snapshot(leaderLease, 100);
    const SwarmTelemetrySnapshot plane = snapshot(
        planeLease, 100, MAV_TYPE_FIXED_WING);
    SwarmFormationPlan plan;
    plan.leader = leaderLease;
    plan.followers = {{planeLease, {10.0, 0.0, 3.0}}};
    const SwarmFormationTick tick = SwarmFormationCore::buildTick(
        plan, {leader, plane}, 100);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.commands.isEmpty());
    QVERIFY(tick.error.contains(QStringLiteral("not yet enabled")));
}

void SwarmFormationCoreTest::absoluteYawRotationAndAntimeridianAreCorrect()
{
    SwarmTelemetrySnapshot leader = snapshot(lease(1, 1), 100);
    leader.latitudeDegrees = 0.25;
    leader.longitudeDegrees = 179.99999;
    leader.yawRadians = 0.0;
    SwarmFormationTarget target = SwarmFormationCore::targetFromLeader(
        leader, {10.0, 0.0, 0.0});
    QVERIFY(target.longitudeDegrees > 179.99999
            || target.longitudeDegrees < -179.9999);
    QVERIFY(near(target.latitudeDegrees, leader.latitudeDegrees, 1.0e-7));

    leader.yawRadians = 90.0 * 3.14159265358979323846 / 180.0;
    target = SwarmFormationCore::targetFromLeader(
        leader, {10.0, 0.0, 0.0});
    QVERIFY(target.latitudeDegrees < leader.latitudeDegrees);
    QVERIFY(near(target.longitudeDegrees, leader.longitudeDegrees, 1.0e-7));
    QCOMPARE(SwarmFormationCommand::PositionVelocityTypeMask,
             quint16(0x0DC0));
}

void SwarmFormationCoreTest::
heartbeatAndFollowerYawAreRequiredBeforeAnyCommand()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease followerLease = lease(2, 2);
    SwarmTelemetrySnapshot leader = snapshot(leaderLease, 100);
    SwarmTelemetrySnapshot follower = snapshot(followerLease, 100);
    SwarmFormationPlan plan;
    plan.leader = leaderLease;
    plan.followers = {{followerLease, {5.0, 0.0, 0.0}}};

    follower.heartbeatObservedMs = 0;
    SwarmFormationTick tick = SwarmFormationCore::buildTick(
        plan, {leader, follower}, 5001);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.commands.isEmpty());

    leader = snapshot(leaderLease, 100);
    follower = snapshot(followerLease, 100);
    follower.attitudeObservedMs = 0;
    tick = SwarmFormationCore::buildTick(
        plan, {leader, follower}, 5001);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.error.contains(QStringLiteral("attitude yaw")));

    plan.alignYaw = false;
    tick = SwarmFormationCore::buildTick(
        plan, {leader, follower}, 5001);
    QVERIFY2(tick.isValid(), qPrintable(tick.error));
    QVERIFY(!tick.commands.first().issueYawAction);
}

void SwarmFormationCoreTest::groupMembershipMustMatchExactly()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease followerLease = lease(2, 2);
    const SwarmTelemetrySnapshot leader = snapshot(leaderLease, 100);
    const SwarmTelemetrySnapshot follower = snapshot(followerLease, 100);
    SwarmFormationPlan plan;
    plan.leader = leaderLease;
    plan.followers = {{followerLease, {5.0, 0.0, 0.0}}};

    SwarmFormationTick tick = SwarmFormationCore::buildTick(
        plan, {leader}, 100);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.error.contains(QStringLiteral("does not match")));

    tick = SwarmFormationCore::buildTick(
        plan, {leader, follower, follower}, 100);
    QVERIFY(!tick.isValid());
    QVERIFY(tick.commands.isEmpty());
}

QTEST_MAIN(SwarmFormationCoreTest)
#include "test_swarmformationcore.moc"
