#include "ui/tools/SwarmFollowLeaderCore.h"

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

SwarmFollowPathPoint offsetPoint(double eastM, double northM,
                                 double altitudeM = 2.0)
{
    const double distanceM = std::hypot(eastM, northM);
    if (distanceM <= std::numeric_limits<double>::epsilon()) {
        return {BaseLatitude, BaseLongitude, altitudeM};
    }
    const double bearing = std::atan2(eastM, northM);
    const double angular = distanceM / EarthRadiusM;
    const double latitude = BaseLatitude * Pi / 180.0;
    const double longitude = BaseLongitude * Pi / 180.0;
    const double targetLatitude = std::asin(
        std::sin(latitude) * std::cos(angular)
        + std::cos(latitude) * std::sin(angular) * std::cos(bearing));
    const double targetLongitude = longitude + std::atan2(
        std::sin(bearing) * std::sin(angular) * std::cos(latitude),
        std::cos(angular) - std::sin(latitude) * std::sin(targetLatitude));
    return {targetLatitude * 180.0 / Pi,
            targetLongitude * 180.0 / Pi, altitudeM};
}

std::pair<double, double> eastNorthFromBase(
    const SwarmFollowPathPoint &point)
{
    const double fromLatitude = BaseLatitude * Pi / 180.0;
    const double toLatitude = point.latitudeDegrees * Pi / 180.0;
    const double deltaLatitude = toLatitude - fromLatitude;
    const double deltaLongitude =
        (point.longitudeDegrees - BaseLongitude) * Pi / 180.0;
    double haversine = std::sin(deltaLatitude / 2.0)
            * std::sin(deltaLatitude / 2.0)
        + std::cos(fromLatitude) * std::cos(toLatitude)
            * std::sin(deltaLongitude / 2.0)
            * std::sin(deltaLongitude / 2.0);
    haversine = std::max(0.0, std::min(1.0, haversine));
    const double distance = EarthRadiusM * 2.0 * std::atan2(
        std::sqrt(haversine), std::sqrt(1.0 - haversine));
    const double bearing = std::atan2(
        std::sin(deltaLongitude) * std::cos(toLatitude),
        std::cos(fromLatitude) * std::sin(toLatitude)
            - std::sin(fromLatitude) * std::cos(toLatitude)
                * std::cos(deltaLongitude));
    return {distance * std::sin(bearing),
            distance * std::cos(bearing)};
}

SwarmTelemetrySnapshot snapshot(
    const SwarmVehicleInstanceLease &vehicle,
    const SwarmFollowPathPoint &position,
    qint64 observedMs = 100,
    int vehicleType = MAV_TYPE_QUADROTOR,
    int autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA,
    bool guided = true,
    double velocityNorthMps = 0.0,
    double velocityEastMps = 0.0,
    double velocityDownMps = 0.0,
    double groundSpeedMps = std::numeric_limits<double>::quiet_NaN())
{
    SwarmTelemetrySnapshot value;
    value.lease = vehicle;
    value.lastMessageMs = observedMs;
    value.heartbeatObservedMs = observedMs;
    value.heartbeatValid = true;
    value.autopilot = autopilot;
    value.vehicleType = vehicleType;
    value.baseMode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    value.customMode = guided ? 4U : 5U;
    value.positionValid = true;
    value.positionObservedMs = observedMs;
    value.latitudeDegrees = position.latitudeDegrees;
    value.longitudeDegrees = position.longitudeDegrees;
    value.relativeAltitudeM = position.relativeAltitudeM;
    value.velocityValid = true;
    value.velocityNorthMps = velocityNorthMps;
    value.velocityEastMps = velocityEastMps;
    value.velocityDownMps = velocityDownMps;
    if (std::isfinite(groundSpeedMps)) {
        value.vfrHudValid = true;
        value.vfrHudObservedMs = observedMs;
        value.groundSpeedMps = groundSpeedMps;
    }
    return value;
}

SwarmFollowLeaderPlan plan(
    const SwarmVehicleInstanceLease &ground,
    const SwarmVehicleInstanceLease &air,
    const QVector<SwarmFollowLeaderFollower> &followers = {})
{
    SwarmFollowLeaderPlan value;
    value.groundMaster = ground;
    value.airMaster = air;
    value.followers = followers;
    return value;
}

void seedTrail(SwarmFollowPathTrail *trail, double endM,
               double altitudeM = 2.0)
{
    for (double offset = 0.0; offset <= endM; offset += 5.0) {
        trail->record(offsetPoint(offset, 0.0, altitudeM));
    }
}

} // namespace

class SwarmFollowLeaderCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndAirOnlyGeometryMatchOfficialController();
    void waitsForWholeTrailThenOrdersFollowerTargets();
    void appliesExactNearWaypointTurnCorrection();
    void invalidMissionTurnHintFallsBackToGroundVelocityBearing();
    void rejectsInvalidRolesAndFollowerOrders();
    void rejectsEveryOutOfRangeSettingBeforeRecordingTrail();
    void rejectsStalePositionHeartbeatAndGroundVelocity();
    void rejectsReplacementAndMalformedExactSnapshotGroups();
    void commandedAirRolesRequireExactGuidedMode();
    void groundGpsJumpStopsAndResetsTrail();
    void acceptsTwentyOneFollowersAndRejectsTwentyTwo();
};

void SwarmFollowLeaderCoreTest::
defaultsAndAirOnlyGeometryMatchOfficialController()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    const SwarmFollowLeaderSettings defaults;
    QCOMPARE(defaults.separationM, 5.0);
    QCOMPARE(defaults.leadM, 20.0);
    QCOMPARE(defaults.altitudeM, 10.0);

    // Ground observation is intentionally PX4: only commanded air roles are
    // restricted to ArduCopter by the MP10 workflow.
    const SwarmTelemetrySnapshot ground = snapshot(
        groundLease, offsetPoint(0.0, 0.0, 3.0), 100,
        MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false,
        4.0, 0.0, -1.0);
    const SwarmTelemetrySnapshot air = snapshot(
        airLease, offsetPoint(20.0, 0.0, 10.0));
    SwarmFollowPathTrail trail;

    const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
        plan(groundLease, airLease), {ground, air}, 100, &trail);

    QCOMPARE(int(tick.state), int(SwarmFollowLeaderTick::State::Active));
    QCOMPARE(tick.commands.size(), 1);
    const SwarmFollowLeaderCommand &command = tick.commands.constFirst();
    QVERIFY(command.lease.sameInstance(airLease));
    QCOMPARE(command.role, QStringLiteral("Air master"));
    QCOMPARE(command.order, 0);
    QCOMPARE(command.distanceBehindM, -5.0);
    const std::pair<double, double> target = eastNorthFromBase(command.target);
    QVERIFY(std::abs(target.first) < 0.02);
    QVERIFY(std::abs(target.second - 5.0) < 0.02);
    QCOMPARE(command.target.relativeAltitudeM, 10.0);
    QCOMPARE(command.velocity.northMps, 2.4);
    QCOMPARE(command.velocity.eastMps, 0.0);
    QCOMPARE(command.velocity.downMps, -0.6);
    QCOMPARE(trail.count(), 1);

    SwarmFollowLeaderPlan differentLead = plan(groundLease, airLease);
    differentLead.settings.leadM = -99999.0;
    SwarmFollowPathTrail otherTrail;
    const SwarmFollowLeaderTick other = SwarmFollowLeaderCore::buildTick(
        differentLead, {ground, air}, 100, &otherTrail);
    QCOMPARE(int(other.state), int(SwarmFollowLeaderTick::State::Active));
    QCOMPARE(other.commands.constFirst().target.latitudeDegrees,
             command.target.latitudeDegrees);
    QCOMPARE(other.commands.constFirst().target.longitudeDegrees,
             command.target.longitudeDegrees);
    QCOMPARE(other.commands.constFirst().velocity.northMps,
             command.velocity.northMps);
}

void SwarmFollowLeaderCoreTest::
waitsForWholeTrailThenOrdersFollowerTargets()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    const SwarmVehicleInstanceLease firstLease = lease(3, 3);
    const SwarmVehicleInstanceLease secondLease = lease(4, 4);
    const SwarmFollowLeaderPlan value = plan(
        groundLease, airLease, {{secondLease, 2}, {firstLease, 1}});
    SwarmFollowPathTrail trail;
    QList<SwarmTelemetrySnapshot> snapshots = {
        snapshot(groundLease, offsetPoint(0.0, 0.0), 100,
                 MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false,
                 0.0, 2.0, 0.0),
        snapshot(airLease, offsetPoint(20.0, 0.0)),
        snapshot(firstLease, offsetPoint(-5.0, 0.0)),
        snapshot(secondLease, offsetPoint(-10.0, 0.0))};

    const SwarmFollowLeaderTick waiting = SwarmFollowLeaderCore::buildTick(
        value, snapshots, 100, &trail);
    QCOMPARE(int(waiting.state),
             int(SwarmFollowLeaderTick::State::WaitingForTrail));
    QVERIFY(waiting.commands.isEmpty());
    QCOMPARE(waiting.requiredTrailM, 5.0);

    snapshots[0] = snapshot(
        groundLease, offsetPoint(6.0, 0.0), 200,
        MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false,
        0.0, 2.0, 0.0);
    snapshots[1].heartbeatObservedMs = 200;
    snapshots[1].positionObservedMs = 200;
    snapshots[2].heartbeatObservedMs = 200;
    snapshots[2].positionObservedMs = 200;
    snapshots[3].heartbeatObservedMs = 200;
    snapshots[3].positionObservedMs = 200;
    const SwarmFollowLeaderTick active = SwarmFollowLeaderCore::buildTick(
        value, snapshots, 200, &trail);

    QCOMPARE(int(active.state), int(SwarmFollowLeaderTick::State::Active));
    QCOMPARE(active.commands.size(), 3);
    QCOMPARE(active.commands.at(1).order, 1);
    QVERIFY(active.commands.at(1).lease.sameInstance(firstLease));
    QCOMPARE(active.commands.at(1).distanceBehindM, 0.0);
    const std::pair<double, double> first =
        eastNorthFromBase(active.commands.at(1).target);
    QVERIFY(std::abs(first.first - 6.0) < 0.02);
    QCOMPARE(active.commands.at(1).target.relativeAltitudeM, 12.0);
    QCOMPARE(active.commands.at(1).velocity.eastMps, 1.0);

    QCOMPARE(active.commands.at(2).order, 2);
    QVERIFY(active.commands.at(2).lease.sameInstance(secondLease));
    QCOMPARE(active.commands.at(2).distanceBehindM, 5.0);
    const std::pair<double, double> second =
        eastNorthFromBase(active.commands.at(2).target);
    QVERIFY(std::abs(second.first - 1.0) < 0.03);
    QCOMPARE(active.commands.at(2).target.relativeAltitudeM, 12.0);

    // The shared trail coalesces movement below 0.1 m, but MP10's first
    // follower still receives the exact current ground-master point.
    snapshots[0] = snapshot(
        groundLease, offsetPoint(6.05, 0.0), 300,
        MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false,
        0.0, 2.0, 0.0);
    for (int index = 1; index < snapshots.size(); ++index) {
        snapshots[index].heartbeatObservedMs = 300;
        snapshots[index].positionObservedMs = 300;
    }
    const SwarmFollowLeaderTick coalesced = SwarmFollowLeaderCore::buildTick(
        value, snapshots, 300, &trail);
    QCOMPARE(int(coalesced.state),
             int(SwarmFollowLeaderTick::State::Active));
    const std::pair<double, double> current =
        eastNorthFromBase(coalesced.commands.at(1).target);
    QVERIFY(std::abs(current.first - 6.05) < 0.02);
}

void SwarmFollowLeaderCoreTest::appliesExactNearWaypointTurnCorrection()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    SwarmFollowLeaderPlan value = plan(groundLease, airLease);
    value.missionTurnHint = SwarmFollowLeaderMissionTurnHint{
        groundLease, 1.0,
        offsetPoint(0.0, 0.0), offsetPoint(100.0, 0.0)};
    const SwarmTelemetrySnapshot ground = snapshot(
        groundLease, offsetPoint(0.0, 0.0), 100,
        MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false,
        -2.0, 0.0, -1.0, 3.0);
    SwarmFollowPathTrail trail;

    const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
        value, {ground, snapshot(airLease, offsetPoint(20.0, 0.0))},
        100, &trail);

    QCOMPARE(int(tick.state), int(SwarmFollowLeaderTick::State::Active));
    const SwarmFollowLeaderCommand &command = tick.commands.constFirst();
    const std::pair<double, double> target = eastNorthFromBase(command.target);
    QVERIFY(std::abs(target.first - 5.0) < 0.02);
    QVERIFY(std::abs(target.second) < 0.02);
    QVERIFY(std::abs(command.velocity.northMps) < 0.001);
    QVERIFY(std::abs(command.velocity.eastMps - 1.8) < 0.001);
    QCOMPARE(command.velocity.downMps, -0.6);
}

void SwarmFollowLeaderCoreTest::
invalidMissionTurnHintFallsBackToGroundVelocityBearing()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    const SwarmTelemetrySnapshot ground = snapshot(
        groundLease, offsetPoint(0.0, 0.0), 100,
        MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false,
        -3.0, 0.0, 0.0);

    QVector<SwarmFollowLeaderMissionTurnHint> invalidHints = {
        {lease(1, 1, 2, 2), 1.0,
         offsetPoint(0.0, 0.0), offsetPoint(100.0, 0.0)},
        {groundLease, 7.5,
         offsetPoint(0.0, 0.0), offsetPoint(100.0, 0.0)},
        {groundLease, 1.0,
         offsetPoint(0.0, 0.0), offsetPoint(0.0, 0.0)}};
    for (const SwarmFollowLeaderMissionTurnHint &hint : invalidHints) {
        SwarmFollowLeaderPlan value = plan(groundLease, airLease);
        value.missionTurnHint = hint;
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            value, {ground, snapshot(airLease, offsetPoint(20.0, 0.0))},
            100, &trail);

        QCOMPARE(int(tick.state), int(SwarmFollowLeaderTick::State::Active));
        const SwarmFollowLeaderCommand &command = tick.commands.constFirst();
        const std::pair<double, double> target =
            eastNorthFromBase(command.target);
        QVERIFY(std::abs(target.first) < 0.02);
        QVERIFY(std::abs(target.second + 5.0) < 0.02);
        QCOMPARE(command.velocity.northMps, -1.8);
        QCOMPARE(command.velocity.eastMps, 0.0);
    }
}

void SwarmFollowLeaderCoreTest::rejectsInvalidRolesAndFollowerOrders()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    const SwarmVehicleInstanceLease followerLease = lease(3, 3);
    const SwarmTelemetrySnapshot ground = snapshot(
        groundLease, offsetPoint(0.0, 0.0), 100,
        MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false, 1.0);
    const SwarmTelemetrySnapshot air = snapshot(
        airLease, offsetPoint(5.0, 0.0));
    const SwarmTelemetrySnapshot follower = snapshot(
        followerLease, offsetPoint(-5.0, 0.0));

    {
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, groundLease), {ground, air}, 100, &trail);
        QCOMPARE(int(tick.state), int(SwarmFollowLeaderTick::State::Stopped));
        QVERIFY(tick.detail.contains(QStringLiteral("different")));
        QCOMPARE(trail.count(), 0);
    }
    {
        SwarmFollowPathTrail trail;
        const SwarmTelemetrySnapshot roverAir = snapshot(
            airLease, offsetPoint(5.0, 0.0), 100,
            MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_ARDUPILOTMEGA, false);
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease), {ground, roverAir}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("ArduCopter")));
        QCOMPARE(trail.count(), 0);
    }
    {
        SwarmFollowPathTrail trail;
        SwarmTelemetrySnapshot gcsGround = ground;
        gcsGround.vehicleType = MAV_TYPE_GCS;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease), {gcsGround, air}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("live autopilot")));
        QCOMPARE(trail.count(), 0);
    }
    {
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease, {{followerLease, 2}}),
            {ground, air, follower}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("contiguous")));
        QCOMPARE(trail.count(), 0);
    }
    {
        const SwarmVehicleInstanceLease secondFollowerLease = lease(4, 4);
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease,
                 {{followerLease, 1}, {secondFollowerLease, 1}}),
            {ground, air, follower,
             snapshot(secondFollowerLease, offsetPoint(-10.0, 0.0))},
            100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("contiguous")));
        QCOMPARE(trail.count(), 0);
    }
    {
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease, {{airLease, 1}}),
            {ground, air, follower}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("role")));
        QCOMPARE(trail.count(), 0);
    }
    {
        SwarmFollowPathTrail trail;
        SwarmTelemetrySnapshot roverFollower = follower;
        roverFollower.vehicleType = MAV_TYPE_GROUND_ROVER;
        roverFollower.customMode = 15;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease, {{followerLease, 1}}),
            {ground, air, roverFollower}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("ArduCopter")));
        QCOMPARE(trail.count(), 0);
    }
}

void SwarmFollowLeaderCoreTest::
rejectsEveryOutOfRangeSettingBeforeRecordingTrail()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    const QList<SwarmTelemetrySnapshot> snapshots = {
        snapshot(groundLease, offsetPoint(0.0, 0.0), 100,
                 MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false, 1.0),
        snapshot(airLease, offsetPoint(5.0, 0.0))};
    QVector<SwarmFollowLeaderSettings> invalid = {
        {0.99, 20.0, 10.0},
        {500.01, 20.0, 10.0},
        {std::numeric_limits<double>::quiet_NaN(), 20.0, 10.0},
        {5.0, -100000.01, 10.0},
        {5.0, 100000.01, 10.0},
        {5.0, std::numeric_limits<double>::infinity(), 10.0},
        {5.0, 20.0, 0.99},
        {5.0, 20.0, 10000.01}};

    for (const SwarmFollowLeaderSettings &settings : invalid) {
        SwarmFollowLeaderPlan value = plan(groundLease, airLease);
        value.settings = settings;
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            value, snapshots, 100, &trail);
        QCOMPARE(int(tick.state), int(SwarmFollowLeaderTick::State::Stopped));
        QVERIFY(tick.commands.isEmpty());
        QCOMPARE(trail.count(), 0);
    }

    for (const SwarmFollowLeaderSettings &settings :
         QVector<SwarmFollowLeaderSettings>{{1.0, -100000.0, 1.0},
                                             {500.0, 100000.0, 10000.0}}) {
        SwarmFollowLeaderPlan value = plan(groundLease, airLease);
        value.settings = settings;
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            value, snapshots, 100, &trail);
        QCOMPARE(int(tick.state), int(SwarmFollowLeaderTick::State::Active));
    }
}

void SwarmFollowLeaderCoreTest::
rejectsStalePositionHeartbeatAndGroundVelocity()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    const SwarmFollowLeaderPlan value = plan(groundLease, airLease);
    const SwarmTelemetrySnapshot validGround = snapshot(
        groundLease, offsetPoint(0.0, 0.0), 6000,
        MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false, 1.0);
    const SwarmTelemetrySnapshot validAir = snapshot(
        airLease, offsetPoint(5.0, 0.0), 6000);

    QVector<std::pair<SwarmTelemetrySnapshot, QString>> groundCases;
    SwarmTelemetrySnapshot staleHeartbeat = validGround;
    staleHeartbeat.heartbeatObservedMs = 0;
    groundCases.append({staleHeartbeat, QStringLiteral("live autopilot")});
    SwarmTelemetrySnapshot stalePosition = validGround;
    stalePosition.positionObservedMs = 0;
    groundCases.append({stalePosition, QStringLiteral("position")});
    SwarmTelemetrySnapshot missingVelocity = validGround;
    missingVelocity.velocityValid = false;
    groundCases.append({missingVelocity, QStringLiteral("velocity")});
    SwarmTelemetrySnapshot nonFiniteVelocity = validGround;
    nonFiniteVelocity.velocityEastMps =
        std::numeric_limits<double>::infinity();
    groundCases.append({nonFiniteVelocity, QStringLiteral("velocity")});

    for (const auto &testCase : groundCases) {
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            value, {testCase.first, validAir}, 6000, &trail);
        QCOMPARE(int(tick.state), int(SwarmFollowLeaderTick::State::Stopped));
        QVERIFY(tick.detail.contains(testCase.second));
        QCOMPARE(trail.count(), 0);
    }

    SwarmTelemetrySnapshot staleAir = validAir;
    staleAir.positionObservedMs = 0;
    SwarmFollowPathTrail trail;
    const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
        value, {validGround, staleAir}, 6000, &trail);
    QVERIFY(tick.detail.contains(QStringLiteral("air-master position")));
    QCOMPARE(trail.count(), 0);

    const SwarmVehicleInstanceLease followerLease = lease(3, 3);
    SwarmTelemetrySnapshot staleFollower = snapshot(
        followerLease, offsetPoint(-5.0, 0.0), 6000);
    staleFollower.positionObservedMs = 0;
    SwarmFollowPathTrail followerTrail;
    const SwarmFollowLeaderTick followerTick =
        SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease, {{followerLease, 1}}),
            {validGround, validAir, staleFollower}, 6000, &followerTrail);
    QVERIFY(followerTick.detail.contains(QStringLiteral("position")));
    QVERIFY(followerTick.detail.contains(QStringLiteral("stale")));
    QCOMPARE(followerTrail.count(), 0);
}

void SwarmFollowLeaderCoreTest::
rejectsReplacementAndMalformedExactSnapshotGroups()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2, 1, 7);
    const SwarmVehicleInstanceLease replacement = lease(2, 2, 2, 8);
    const SwarmTelemetrySnapshot ground = snapshot(
        groundLease, offsetPoint(0.0, 0.0), 100,
        MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false, 1.0);
    const SwarmTelemetrySnapshot air = snapshot(
        airLease, offsetPoint(5.0, 0.0));

    {
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease),
            {ground, snapshot(replacement, offsetPoint(5.0, 0.0))},
            100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("reconnected")));
        QCOMPARE(trail.count(), 0);
    }
    {
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease), {ground}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("does not match")));
        QCOMPARE(trail.count(), 0);
    }
    {
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease, {{lease(3, 3), 1}}),
            {ground, air, air}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("duplicate endpoint")));
        QCOMPARE(trail.count(), 0);
    }
    {
        SwarmFollowPathTrail trail;
        const SwarmTelemetrySnapshot unrelated = snapshot(
            lease(9, 9), offsetPoint(9.0, 0.0));
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease), {ground, unrelated}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("missing")));
        QCOMPARE(trail.count(), 0);
    }
}

void SwarmFollowLeaderCoreTest::commandedAirRolesRequireExactGuidedMode()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    const SwarmVehicleInstanceLease followerLease = lease(3, 3);
    const SwarmTelemetrySnapshot ground = snapshot(
        groundLease, offsetPoint(0.0, 0.0), 100,
        MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false, 1.0);
    SwarmTelemetrySnapshot air = snapshot(
        airLease, offsetPoint(5.0, 0.0));
    air.customMode = 5;
    air.baseMode = static_cast<quint8>(
        MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | MAV_MODE_FLAG_GUIDED_ENABLED);
    {
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease), {ground, air}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("exact")));
        QVERIFY(tick.detail.contains(QStringLiteral("Loiter")));
        QCOMPARE(trail.count(), 0);
    }

    air.customMode = 4;
    SwarmTelemetrySnapshot follower = snapshot(
        followerLease, offsetPoint(-5.0, 0.0));
    follower.baseMode = MAV_MODE_FLAG_GUIDED_ENABLED;
    {
        SwarmFollowPathTrail trail;
        const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
            plan(groundLease, airLease, {{followerLease, 1}}),
            {ground, air, follower}, 100, &trail);
        QVERIFY(tick.detail.contains(QStringLiteral("exact")));
        QCOMPARE(trail.count(), 0);
    }
}

void SwarmFollowLeaderCoreTest::groundGpsJumpStopsAndResetsTrail()
{
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    SwarmFollowPathTrail trail;
    seedTrail(&trail, 20.0);

    const SwarmFollowLeaderTick tick = SwarmFollowLeaderCore::buildTick(
        plan(groundLease, airLease),
        {snapshot(groundLease, offsetPoint(600.1, 0.0), 100,
                  MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false, 1.0),
         snapshot(airLease, offsetPoint(5.0, 0.0))},
        100, &trail);

    QCOMPARE(int(tick.state), int(SwarmFollowLeaderTick::State::Stopped));
    QVERIFY(tick.commands.isEmpty());
    QVERIFY(tick.detail.contains(QStringLiteral("jumped")));
    QCOMPARE(trail.count(), 1);
    QCOMPARE(trail.lengthM(), 0.0);
}

void SwarmFollowLeaderCoreTest::
acceptsTwentyOneFollowersAndRejectsTwentyTwo()
{
    QCOMPARE(SwarmFollowLeaderCore::MaximumFollowers, 21);
    const SwarmVehicleInstanceLease groundLease = lease(1, 1);
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    SwarmFollowLeaderPlan value = plan(groundLease, airLease);
    QList<SwarmTelemetrySnapshot> snapshots = {
        snapshot(groundLease, offsetPoint(105.0, 0.0), 100,
                 MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4, false, 1.0),
        snapshot(airLease, offsetPoint(110.0, 0.0))};
    for (int order = 1; order <= SwarmFollowLeaderCore::MaximumFollowers;
         ++order) {
        const SwarmVehicleInstanceLease followerLease =
            lease(order + 2, order + 2);
        value.followers.append({followerLease, order});
        snapshots.append(snapshot(
            followerLease, offsetPoint(105.0 - order * 5.0, 0.0)));
    }
    SwarmFollowPathTrail trail;
    seedTrail(&trail, 100.0);

    const SwarmFollowLeaderTick accepted = SwarmFollowLeaderCore::buildTick(
        value, snapshots, 100, &trail);
    QCOMPARE(int(accepted.state), int(SwarmFollowLeaderTick::State::Active));
    QCOMPARE(accepted.commands.size(), 22);
    QCOMPARE(accepted.commands.constLast().order, 21);
    QCOMPARE(accepted.commands.constLast().distanceBehindM, 100.0);

    const SwarmVehicleInstanceLease twentySecond = lease(24, 24);
    value.followers.append({twentySecond, 22});
    snapshots.append(snapshot(twentySecond, offsetPoint(0.0, 0.0)));
    SwarmFollowPathTrail rejectedTrail;
    const SwarmFollowLeaderTick rejected = SwarmFollowLeaderCore::buildTick(
        value, snapshots, 100, &rejectedTrail);
    QCOMPARE(int(rejected.state), int(SwarmFollowLeaderTick::State::Stopped));
    QVERIFY(rejected.detail.contains(QStringLiteral("at most 21")));
    QCOMPARE(rejectedTrail.count(), 0);
}

QTEST_MAIN(SwarmFollowLeaderCoreTest)

#include "test_swarmfollowleadercore.moc"
