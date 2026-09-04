#include "ui/tools/SwarmFollowPathCore.h"

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{

constexpr double EarthRadiusM = 6378137.0;
constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr double BaseLatitude = 35.0;
constexpr double BaseLongitude = 33.0;

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

SwarmFollowPathPoint pointEast(double eastM, double altitudeM = 100.0)
{
    const double angular = eastM / EarthRadiusM;
    const double latitude = BaseLatitude * Pi / 180.0;
    const double longitude = BaseLongitude * Pi / 180.0;
    const double targetLatitude = std::asin(
        std::sin(latitude) * std::cos(angular));
    const double targetLongitude = longitude + std::atan2(
        std::sin(Pi / 2.0) * std::sin(angular) * std::cos(latitude),
        std::cos(angular) - std::sin(latitude) * std::sin(targetLatitude));
    return {targetLatitude * 180.0 / Pi,
            targetLongitude * 180.0 / Pi, altitudeM};
}

double distance(const SwarmFollowPathPoint &left,
                const SwarmFollowPathPoint &right)
{
    const double leftLatitude = left.latitudeDegrees * Pi / 180.0;
    const double rightLatitude = right.latitudeDegrees * Pi / 180.0;
    const double deltaLatitude = rightLatitude - leftLatitude;
    const double deltaLongitude =
        (right.longitudeDegrees - left.longitudeDegrees) * Pi / 180.0;
    double value = std::sin(deltaLatitude / 2.0)
            * std::sin(deltaLatitude / 2.0)
        + std::cos(leftLatitude) * std::cos(rightLatitude)
            * std::sin(deltaLongitude / 2.0)
            * std::sin(deltaLongitude / 2.0);
    value = std::max(0.0, std::min(1.0, value));
    return EarthRadiusM * 2.0 * std::atan2(
        std::sqrt(value), std::sqrt(1.0 - value));
}

SwarmTelemetrySnapshot snapshot(
    const SwarmVehicleInstanceLease &vehicle,
    const SwarmFollowPathPoint &position,
    qint64 observedMs = 100,
    int vehicleType = MAV_TYPE_QUADROTOR,
    bool guided = false)
{
    SwarmTelemetrySnapshot value;
    value.lease = vehicle;
    value.heartbeatValid = true;
    value.heartbeatObservedMs = observedMs;
    value.lastMessageMs = observedMs;
    value.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    value.vehicleType = vehicleType;
    value.baseMode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    if (guided) {
        value.baseMode |= MAV_MODE_FLAG_GUIDED_ENABLED;
        value.customMode = vehicleType == MAV_TYPE_FIXED_WING
                || vehicleType == MAV_TYPE_GROUND_ROVER
                || vehicleType == MAV_TYPE_SURFACE_BOAT
            ? 15U : 4U;
    }
    value.positionValid = true;
    value.positionObservedMs = observedMs;
    value.latitudeDegrees = position.latitudeDegrees;
    value.longitudeDegrees = position.longitudeDegrees;
    value.relativeAltitudeM = position.relativeAltitudeM;
    return value;
}

SwarmFollowPathPlan plan(
    const SwarmVehicleInstanceLease &leader,
    const QVector<SwarmFollowPathFollower> &followers,
    double separationM = 5.0)
{
    SwarmFollowPathPlan value;
    value.leader = leader;
    value.followers = followers;
    value.separationM = separationM;
    return value;
}

void seedTrail(SwarmFollowPathTrail *trail, double endM,
               double stepM = 5.0)
{
    for (double offset = 0.0; offset <= endM; offset += stepM) {
        QCOMPARE(int(trail->record(pointEast(offset, 100.0 + offset))),
                 int(SwarmFollowPathTrailUpdate::Added));
    }
}

} // namespace

class SwarmFollowPathCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void trailSamplesAtPointOneMetreAndStaysBounded();
    void trailResetsAfterGpsJump();
    void trailInterpolatesBackwardsAcrossAntimeridian();
    void tickSortsFollowersAndResolvesEveryTarget();
    void tickWithholdsWholeBatchUntilTrailIsLongEnough();
    void exactPlanValidationRejectsAmbiguityAndReplacement();
    void followerMustHaveFreshPositionAndGuidedBaseMode();
    void planeLeaderIsAllowedButPlaneFollowerIsNot();
    void leaderGpsJumpStopsWithoutOldTargets();
};

void SwarmFollowPathCoreTest::trailSamplesAtPointOneMetreAndStaysBounded()
{
    SwarmFollowPathTrail trail;
    QCOMPARE(int(trail.record(pointEast(0.0))),
             int(SwarmFollowPathTrailUpdate::Added));
    QCOMPARE(int(trail.record(pointEast(0.05, 101.0))),
             int(SwarmFollowPathTrailUpdate::Unchanged));
    QCOMPARE(trail.count(), 1);
    QCOMPARE(trail.lengthM(), 0.0);
    QCOMPARE(int(trail.record(pointEast(0.11, 102.0))),
             int(SwarmFollowPathTrailUpdate::Added));
    QCOMPARE(trail.count(), 2);
    QVERIFY(std::abs(trail.lengthM() - 0.11) < 0.01);

    for (int index = 1; index <= SwarmFollowPathTrail::MaximumPoints; ++index) {
        trail.record(pointEast(0.11 + index));
    }
    QCOMPARE(trail.count(), SwarmFollowPathTrail::MaximumPoints);
    QVERIFY(trail.lengthM() > 4990.0);
    QVERIFY(trail.lengthM() < 5001.0);
}

void SwarmFollowPathCoreTest::trailResetsAfterGpsJump()
{
    SwarmFollowPathTrail trail;
    trail.record(pointEast(0.0));
    trail.record(pointEast(20.0));

    QCOMPARE(int(trail.record(pointEast(600.1))),
             int(SwarmFollowPathTrailUpdate::ResetAfterJump));
    QCOMPARE(trail.count(), 1);
    QCOMPARE(trail.lengthM(), 0.0);
    SwarmFollowPathPoint target;
    QVERIFY(!trail.pointBehind(1.0, &target));
}

void SwarmFollowPathCoreTest::trailInterpolatesBackwardsAcrossAntimeridian()
{
    SwarmFollowPathTrail trail;
    const SwarmFollowPathPoint older{10.0, 179.9998, 100.0};
    const SwarmFollowPathPoint newer{10.0, -179.9998, 120.0};
    trail.record(older);
    trail.record(newer);
    const double half = trail.lengthM() / 2.0;

    SwarmFollowPathPoint target;
    QVERIFY(trail.pointBehind(half, &target));
    QVERIFY(target.longitudeDegrees >= -180.0
            && target.longitudeDegrees <= 180.0);
    QVERIFY(std::abs(std::abs(target.longitudeDegrees) - 180.0) < 0.0001);
    QVERIFY(std::abs(target.relativeAltitudeM - 110.0) < 0.01);
    QVERIFY(std::abs(distance(newer, target) - half) < 0.02);
}

void SwarmFollowPathCoreTest::tickSortsFollowersAndResolvesEveryTarget()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease firstLease = lease(2, 2);
    const SwarmVehicleInstanceLease secondLease = lease(3, 3);
    SwarmFollowPathTrail trail;
    seedTrail(&trail, 15.0);
    const SwarmFollowPathPoint leaderPoint = pointEast(20.0, 120.0);
    const QList<SwarmTelemetrySnapshot> snapshots = {
        snapshot(leaderLease, leaderPoint),
        snapshot(firstLease, pointEast(12.0), 100,
                 MAV_TYPE_QUADROTOR, true),
        snapshot(secondLease, pointEast(7.0), 100,
                 MAV_TYPE_GROUND_ROVER, true)};

    const SwarmFollowPathTick tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{secondLease, 2}, {firstLease, 1}}),
        snapshots, 100, &trail);

    QCOMPARE(int(tick.state), int(SwarmFollowPathTick::State::Active));
    QCOMPARE(tick.commands.size(), 2);
    QCOMPARE(tick.commands.at(0).order, 1);
    QVERIFY(tick.commands.at(0).follower.sameInstance(firstLease));
    QVERIFY(std::abs(tick.commands.at(0).distanceBehindM - 5.0) < 0.001);
    QVERIFY(std::abs(distance(pointEast(15.0), tick.commands.at(0).target))
            < 0.02);
    QCOMPARE(tick.commands.at(1).order, 2);
    QVERIFY(tick.commands.at(1).follower.sameInstance(secondLease));
    QVERIFY(std::abs(distance(pointEast(10.0), tick.commands.at(1).target))
            < 0.02);
}

void SwarmFollowPathCoreTest::tickWithholdsWholeBatchUntilTrailIsLongEnough()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease firstLease = lease(2, 2);
    const SwarmVehicleInstanceLease secondLease = lease(3, 3);
    SwarmFollowPathTrail trail;
    seedTrail(&trail, 5.0);
    const QList<SwarmTelemetrySnapshot> snapshots = {
        snapshot(leaderLease, pointEast(6.0)),
        snapshot(firstLease, pointEast(2.0), 100,
                 MAV_TYPE_QUADROTOR, true),
        snapshot(secondLease, pointEast(1.0), 100,
                 MAV_TYPE_QUADROTOR, true)};

    const SwarmFollowPathTick tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{firstLease, 1}, {secondLease, 2}}),
        snapshots, 100, &trail);

    QCOMPARE(int(tick.state),
             int(SwarmFollowPathTick::State::WaitingForTrail));
    QVERIFY(tick.commands.isEmpty());
    QVERIFY(tick.shouldContinue());
    QVERIFY(tick.availableTrailM < tick.requiredTrailM);
}

void SwarmFollowPathCoreTest::
exactPlanValidationRejectsAmbiguityAndReplacement()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease firstLease = lease(2, 2, 1, 7);
    const SwarmVehicleInstanceLease replacement = lease(2, 2, 2, 8);
    const SwarmVehicleInstanceLease secondLease = lease(3, 3);
    const QList<SwarmTelemetrySnapshot> validSnapshots = {
        snapshot(leaderLease, pointEast(20.0)),
        snapshot(firstLease, pointEast(10.0), 100,
                 MAV_TYPE_QUADROTOR, true),
        snapshot(secondLease, pointEast(5.0), 100,
                 MAV_TYPE_QUADROTOR, true)};
    SwarmFollowPathTrail trail;

    SwarmFollowPathTick tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{firstLease, 1}, {secondLease, 1}}),
        validSnapshots, 100, &trail);
    QCOMPARE(int(tick.state), int(SwarmFollowPathTick::State::Stopped));
    QVERIFY(tick.detail.contains(QStringLiteral("unique")));
    QCOMPARE(trail.count(), 0);

    tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{firstLease, 1}}, 0.5),
        {validSnapshots.at(0), validSnapshots.at(1)}, 100, &trail);
    QVERIFY(tick.detail.contains(QStringLiteral("separation")));
    QCOMPARE(trail.count(), 0);

    tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{firstLease, 1}}),
        {validSnapshots.at(0),
         snapshot(replacement, pointEast(10.0), 100,
                  MAV_TYPE_QUADROTOR, true)},
        100, &trail);
    QVERIFY(tick.detail.contains(QStringLiteral("reconnected")));
    QCOMPARE(trail.count(), 0);

    tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{leaderLease, 1}}),
        {validSnapshots.at(0), validSnapshots.at(1)}, 100, &trail);
    QVERIFY(tick.detail.contains(QStringLiteral("leader was also")));
    QCOMPARE(trail.count(), 0);
}

void SwarmFollowPathCoreTest::
followerMustHaveFreshPositionAndGuidedBaseMode()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease followerLease = lease(2, 2);
    SwarmFollowPathTrail trail;
    SwarmTelemetrySnapshot follower = snapshot(
        followerLease, pointEast(10.0), 100, MAV_TYPE_QUADROTOR, false);

    SwarmFollowPathTick tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{followerLease, 1}}),
        {snapshot(leaderLease, pointEast(20.0)), follower},
        100, &trail);
    QVERIFY(tick.detail.contains(QStringLiteral("GUIDED")));
    QCOMPARE(trail.count(), 0);

    follower.baseMode = static_cast<quint8>(
        MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | MAV_MODE_FLAG_GUIDED_ENABLED);
    follower.customMode = 5;
    tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{followerLease, 1}}),
        {snapshot(leaderLease, pointEast(20.0)), follower},
        100, &trail);
    QVERIFY(tick.detail.contains(QStringLiteral("Loiter")));
    QVERIFY(tick.detail.contains(QStringLiteral("exact")));
    QCOMPARE(trail.count(), 0);

    follower.customMode = 4;
    follower.positionObservedMs = 100;
    tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{followerLease, 1}}),
        {snapshot(leaderLease, pointEast(20.0), 100), follower},
        5201, &trail);
    QVERIFY(tick.detail.contains(QStringLiteral("stale")));
    QCOMPARE(trail.count(), 0);

    follower.positionObservedMs = 5201;
    tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{followerLease, 1}}),
        {snapshot(leaderLease, pointEast(20.0), 100), follower},
        5201, &trail);
    QVERIFY(tick.detail.contains(QStringLiteral("leader position")));
    QCOMPARE(trail.count(), 0);
}

void SwarmFollowPathCoreTest::planeLeaderIsAllowedButPlaneFollowerIsNot()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease followerLease = lease(2, 2);
    SwarmFollowPathTrail trail;
    seedTrail(&trail, 5.0);

    SwarmFollowPathTick tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{followerLease, 1}}),
        {snapshot(leaderLease, pointEast(10.0), 100, MAV_TYPE_FIXED_WING),
         snapshot(followerLease, pointEast(5.0), 100,
                  MAV_TYPE_QUADROTOR, true)},
        100, &trail);
    QCOMPARE(int(tick.state), int(SwarmFollowPathTick::State::Active));
    QCOMPARE(tick.commands.size(), 1);

    SwarmFollowPathTrail planeFollowerTrail;
    tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{followerLease, 1}}, 30.0),
        {snapshot(leaderLease, pointEast(40.0)),
         snapshot(followerLease, pointEast(10.0), 100,
                  MAV_TYPE_FIXED_WING, true)},
        100, &planeFollowerTrail);
    QCOMPARE(int(tick.state), int(SwarmFollowPathTick::State::Stopped));
    QVERIFY(tick.commands.isEmpty());
    QVERIFY(tick.detail.contains(QStringLiteral("guided-waypoint")));
    QCOMPARE(planeFollowerTrail.count(), 0);
}

void SwarmFollowPathCoreTest::leaderGpsJumpStopsWithoutOldTargets()
{
    const SwarmVehicleInstanceLease leaderLease = lease(1, 1);
    const SwarmVehicleInstanceLease followerLease = lease(2, 2);
    SwarmFollowPathTrail trail;
    seedTrail(&trail, 20.0, 10.0);

    const SwarmFollowPathTick tick = SwarmFollowPathCore::buildTick(
        plan(leaderLease, {{followerLease, 1}}),
        {snapshot(leaderLease, pointEast(600.1)),
         snapshot(followerLease, pointEast(10.0), 100,
                  MAV_TYPE_QUADROTOR, true)},
        100, &trail);

    QCOMPARE(int(tick.state), int(SwarmFollowPathTick::State::Stopped));
    QVERIFY(tick.commands.isEmpty());
    QVERIFY(tick.detail.contains(QStringLiteral("jumped")));
    QCOMPARE(trail.count(), 1);
    QCOMPARE(trail.lengthM(), 0.0);
}

QTEST_MAIN(SwarmFollowPathCoreTest)

#include "test_swarmfollowpathcore.moc"
