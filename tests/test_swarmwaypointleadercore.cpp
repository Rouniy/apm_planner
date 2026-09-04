#include "ui/tools/SwarmWaypointLeaderCore.h"

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
                                 double altitudeM = 10.0)
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
    const double distanceM = EarthRadiusM * 2.0 * std::atan2(
        std::sqrt(haversine), std::sqrt(1.0 - haversine));
    const double bearing = std::atan2(
        std::sin(deltaLongitude) * std::cos(toLatitude),
        std::cos(fromLatitude) * std::sin(toLatitude)
            - std::sin(fromLatitude) * std::cos(toLatitude)
                * std::cos(deltaLongitude));
    return {distanceM * std::sin(bearing),
            distanceM * std::cos(bearing)};
}

double distance(const SwarmFollowPathPoint &left,
                const SwarmFollowPathPoint &right)
{
    const double leftLatitude = left.latitudeDegrees * Pi / 180.0;
    const double rightLatitude = right.latitudeDegrees * Pi / 180.0;
    const double deltaLatitude = rightLatitude - leftLatitude;
    const double deltaLongitude =
        (right.longitudeDegrees - left.longitudeDegrees) * Pi / 180.0;
    double haversine = std::sin(deltaLatitude / 2.0)
            * std::sin(deltaLatitude / 2.0)
        + std::cos(leftLatitude) * std::cos(rightLatitude)
            * std::sin(deltaLongitude / 2.0)
            * std::sin(deltaLongitude / 2.0);
    haversine = std::max(0.0, std::min(1.0, haversine));
    return EarthRadiusM * 2.0 * std::atan2(
        std::sqrt(haversine), std::sqrt(1.0 - haversine));
}

SwarmWaypointLeaderMissionItem missionItem(
    int sequence, const SwarmFollowPathPoint &point,
    quint16 command = MAV_CMD_NAV_WAYPOINT,
    int frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT)
{
    SwarmWaypointLeaderMissionItem item;
    item.sequence = sequence;
    item.command = command;
    item.frame = frame;
    item.latitudeE7 = static_cast<qint32>(
        qRound64(point.latitudeDegrees * 1.0e7));
    item.longitudeE7 = static_cast<qint32>(
        qRound64(point.longitudeDegrees * 1.0e7));
    item.relativeAltitudeM = static_cast<float>(point.relativeAltitudeM);
    return item;
}

SwarmWaypointLeaderMissionSnapshot mission(
    const SwarmVehicleInstanceLease &air, double lengthM = 400.0)
{
    SwarmWaypointLeaderMissionSnapshot value;
    value.airMaster = air;
    value.missionType = MAV_MISSION_TYPE_MISSION;
    value.contentGeneration = 7;
    value.contentDigest = QByteArrayLiteral("exact-main-mission-digest");
    value.items = {
        missionItem(0, offsetPoint(0.0, 0.0, 10.0)),
        missionItem(1, offsetPoint(lengthM / 2.0, 0.0, 20.0),
                    MAV_CMD_NAV_SPLINE_WAYPOINT),
        missionItem(2, offsetPoint(lengthM, 0.0, 30.0))
    };
    return value;
}

SwarmWaypointLeaderVehicleState vehicle(
    const SwarmVehicleInstanceLease &vehicleLease,
    const SwarmFollowPathPoint &position,
    qint64 observedMs = 100,
    int vehicleType = MAV_TYPE_QUADROTOR,
    int autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA,
    bool armed = false,
    quint32 customMode = 4U,
    double northMps = 0.0,
    double eastMps = 0.0,
    double downMps = 0.0,
    const QSet<QString> &parameters = {})
{
    SwarmWaypointLeaderVehicleState value;
    value.telemetry.lease = vehicleLease;
    value.telemetry.lastMessageMs = observedMs;
    value.telemetry.heartbeatObservedMs = observedMs;
    value.telemetry.heartbeatValid = true;
    value.telemetry.armed = armed;
    value.telemetry.autopilot = autopilot;
    value.telemetry.vehicleType = vehicleType;
    value.telemetry.baseMode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    if (armed) {
        value.telemetry.baseMode |= MAV_MODE_FLAG_SAFETY_ARMED;
    }
    value.telemetry.customMode = customMode;
    value.telemetry.positionValid = true;
    value.telemetry.positionObservedMs = observedMs;
    value.telemetry.latitudeDegrees = position.latitudeDegrees;
    value.telemetry.longitudeDegrees = position.longitudeDegrees;
    value.telemetry.relativeAltitudeM = position.relativeAltitudeM;
    value.telemetry.velocityValid = true;
    value.telemetry.velocityNorthMps = northMps;
    value.telemetry.velocityEastMps = eastMps;
    value.telemetry.velocityDownMps = downMps;
    value.availableParameters = parameters;
    return value;
}

struct Fixture
{
    SwarmVehicleInstanceLease groundLease = lease(1, 1);
    SwarmVehicleInstanceLease airLease = lease(2, 2);
    SwarmVehicleInstanceLease followerLease = lease(3, 3);
    SwarmWaypointLeaderMissionSnapshot missionData = mission(airLease);
    SwarmWaypointLeaderPlan plan;
    QList<SwarmWaypointLeaderVehicleState> vehicles;

    explicit Fixture(bool includeFollower = false)
    {
        plan.groundMaster = groundLease;
        plan.airMaster = airLease;
        if (includeFollower) {
            plan.followers.append({followerLease, 1});
        }
        plan.missionSignature =
            SwarmWaypointLeaderMissionPath::signatureOf(missionData.items);
        plan.missionContentGeneration = missionData.contentGeneration;
        plan.missionContentDigest = missionData.contentDigest;
        vehicles = {
            vehicle(groundLease, offsetPoint(20.0, 0.0, 1.0), 100,
                    MAV_TYPE_GROUND_ROVER, MAV_AUTOPILOT_PX4,
                    false, 0U, 6.0, 3.0, -0.9),
            vehicle(airLease, offsetPoint(40.0, 0.0, 0.0), 100,
                    MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
                    false, 5U, 0.0, 0.0, 0.0,
                    {QStringLiteral("RTL_ALT"),
                     QStringLiteral("WPNAV_ACCEL")})
        };
        if (includeFollower) {
            vehicles.append(vehicle(
                followerLease, offsetPoint(35.0, 0.0, 0.0), 100,
                MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
                false, 5U, 0.0, 0.0, 0.0,
                {QStringLiteral("RTL_ALT_M"), QStringLiteral("WP_ACC")}));
        }
    }

    SwarmWaypointLeaderVehicleState &ground() { return vehicles[0]; }
    SwarmWaypointLeaderVehicleState &air() { return vehicles[1]; }
    SwarmWaypointLeaderVehicleState &follower() { return vehicles[2]; }
};

QVector<SwarmWaypointLeaderCommandIntent> intentsOfKind(
    const SwarmWaypointLeaderTick &tick,
    SwarmWaypointLeaderCommandIntent::Kind kind)
{
    QVector<SwarmWaypointLeaderCommandIntent> result;
    for (const SwarmWaypointLeaderCommandIntent &intent : tick.intents) {
        if (intent.kind == kind) {
            result.append(intent);
        }
    }
    return result;
}

void succeed(SwarmWaypointLeaderCore *core,
             const SwarmWaypointLeaderTick &tick)
{
    QVERIFY(core);
    QVERIFY(tick.hasIntents());
    QVERIFY(core->completeBatch(
        tick.batchId, SwarmWaypointLeaderBatchResult::Succeeded));
}

void initialize(
    SwarmWaypointLeaderCore *core, Fixture *fixture)
{
    SwarmWaypointLeaderTick tick = core->tick(
        fixture->plan, fixture->vehicles, fixture->missionData, 100);
    succeed(core, tick);
    QCOMPARE(int(core->mode()), int(SwarmWaypointLeaderMode::Takeoff));
}

void setPosition(SwarmWaypointLeaderVehicleState *vehicleState,
                 const SwarmFollowPathPoint &point)
{
    vehicleState->telemetry.latitudeDegrees = point.latitudeDegrees;
    vehicleState->telemetry.longitudeDegrees = point.longitudeDegrees;
    vehicleState->telemetry.relativeAltitudeM = point.relativeAltitudeM;
}

void setFlightReady(SwarmWaypointLeaderVehicleState *vehicleState,
                    const SwarmFollowPathPoint &point,
                    quint32 mode = 4U)
{
    setPosition(vehicleState, point);
    vehicleState->telemetry.armed = true;
    vehicleState->telemetry.baseMode =
        MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | MAV_MODE_FLAG_SAFETY_ARMED;
    vehicleState->telemetry.customMode = mode;
}

} // namespace

class SwarmWaypointLeaderCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsRangesAndIntentAckPolicyMatchMp10();
    void missionPathIsCompactInterpolatedAndSignatureExact();
    void missionFramesFilteringAndBoundsAreFailClosed();
    void closestLineAndVGeometryAreDeterministic();
    void exactGroupRolesMissionAndFreshFieldsAreValidatedBeforeIntents();
    void initializationRequestsFiveHertzAndUsesParameterAliases();
    void completionGateRejectsWrongIdAndFailsClosedOnPartialResult();
    void stagedTakeoffCommandsOnlyAirRolesAndAdvancesAsOneGroup();
    void flyToGroundMasterEntersFollowThroughAckGatedAcceleration();
    void followUsesOneThirdVelocityVFormationAndAltitudeInterleave();
    void offPathTransitionAndReturnAlongMissionPreserveFullBatch();
    void separatedLandingAltitudesDoNotRatchetAndRtlCompletes();
    void collisionOverridePreemptsNormalStateWithoutMutatingIt();
    void capturedPlanChangeStopsBeforeAnotherBatch();
    void requestedModesResetOnlyTheirOfficialState();
};

void SwarmWaypointLeaderCoreTest::
defaultsRangesAndIntentAckPolicyMatchMp10()
{
    const SwarmWaypointLeaderSettings defaults;
    QCOMPARE(defaults.separationM, 5.0);
    QCOMPARE(defaults.leadM, 20.0);
    QCOMPARE(defaults.offPathTriggerM, 10.0);
    QCOMPARE(defaults.takeoffLandAltitudeSeparationM, 2.0);
    QCOMPARE(defaults.navigationAccelerationMps2, 1.0);
    QVERIFY(!defaults.vFormation);
    QVERIFY(!defaults.altitudeInterleave);
    QCOMPARE(SwarmWaypointLeaderCore::PositionStreamRateHz, 5);
    QCOMPARE(SwarmWaypointLeaderCore::ControlRateHz, 10);
    QCOMPARE(SwarmWaypointLeaderCore::MaximumOrder, 20);

    QString error;
    QVERIFY(SwarmWaypointLeaderCore::validateSettings(defaults, &error));
    QVERIFY(error.isEmpty());
    SwarmWaypointLeaderSettings boundary = defaults;
    boundary.separationM = 2.0;
    boundary.leadM = -500.0;
    boundary.offPathTriggerM = 1.0;
    boundary.takeoffLandAltitudeSeparationM = 100.0;
    boundary.navigationAccelerationMps2 = 100.0;
    QVERIFY(SwarmWaypointLeaderCore::validateSettings(boundary, &error));
    boundary.separationM = 1.999;
    QVERIFY(!SwarmWaypointLeaderCore::validateSettings(boundary, &error));
    boundary = defaults;
    boundary.leadM = 5000.1;
    QVERIFY(!SwarmWaypointLeaderCore::validateSettings(boundary, &error));
    boundary = defaults;
    boundary.offPathTriggerM = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!SwarmWaypointLeaderCore::validateSettings(boundary, &error));
    boundary = defaults;
    boundary.takeoffLandAltitudeSeparationM = 0.99;
    QVERIFY(!SwarmWaypointLeaderCore::validateSettings(boundary, &error));
    boundary = defaults;
    boundary.navigationAccelerationMps2 = 0.09;
    QVERIFY(!SwarmWaypointLeaderCore::validateSettings(boundary, &error));

    SwarmWaypointLeaderCommandIntent intent;
    intent.kind = SwarmWaypointLeaderCommandIntent::Kind::PositionTarget;
    QVERIFY(!intent.requiresVehicleAcknowledgement());
    intent.kind = SwarmWaypointLeaderCommandIntent::Kind::RequestPositionStream;
    QVERIFY(!intent.requiresVehicleAcknowledgement());
    intent.kind = SwarmWaypointLeaderCommandIntent::Kind::SetParameter;
    QVERIFY(intent.requiresVehicleAcknowledgement());
    intent.kind = SwarmWaypointLeaderCommandIntent::Kind::SetModeGuided;
    QVERIFY(intent.requiresVehicleAcknowledgement());
    intent.kind = SwarmWaypointLeaderCommandIntent::Kind::Arm;
    QVERIFY(intent.requiresVehicleAcknowledgement());
    intent.kind = SwarmWaypointLeaderCommandIntent::Kind::Takeoff;
    QVERIFY(intent.requiresVehicleAcknowledgement());
    intent.kind = SwarmWaypointLeaderCommandIntent::Kind::SetModeRtl;
    QVERIFY(intent.requiresVehicleAcknowledgement());
    QCOMPARE(SwarmWaypointLeaderCommandIntent::PositionVelocityTypeMask,
             quint16(0x0DC0));
}

void SwarmWaypointLeaderCoreTest::
missionPathIsCompactInterpolatedAndSignatureExact()
{
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    SwarmWaypointLeaderMissionSnapshot missionData;
    missionData.airMaster = airLease;
    missionData.items = {
        {0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
         350000000, 330000000, 10.0F},
        {1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
         350001000, 330000000, 20.0F}
    };
    SwarmWaypointLeaderMissionPath path;
    QString error;
    QVERIFY2(SwarmWaypointLeaderMissionPath::build(
        missionData, &path, &error), qPrintable(error));
    QVERIFY(path.isValid());
    QCOMPARE(path.profile().size(), 2);
    QCOMPARE(path.signature(), QStringLiteral(
        "5D76BF356F118B44F0D34C5402357B310E89786CF65F2296969F5896A3F99CD6"));

    SwarmFollowPathPoint middle;
    QVERIFY(path.pointAt(path.lengthM() / 2.0, &middle));
    QVERIFY(std::abs(middle.relativeAltitudeM - 15.0) < 0.01);
    QCOMPARE(path.profile().constFirst().distanceM, 0.0);
    QVERIFY(path.profile().constLast().distanceM > 11.0);

    const QString signature = path.signature();
    std::reverse(missionData.items.begin(), missionData.items.end());
    QVERIFY(SwarmWaypointLeaderMissionPath::build(
        missionData, &path, &error));
    QCOMPARE(path.signature(), signature);
    missionData.items[0].relativeAltitudeM = 21.0F;
    QVERIFY(SwarmWaypointLeaderMissionPath::build(
        missionData, &path, &error));
    QVERIFY(path.signature() != signature);
}

void SwarmWaypointLeaderCoreTest::
missionFramesFilteringAndBoundsAreFailClosed()
{
    const SwarmVehicleInstanceLease airLease = lease(2, 2);
    QString error;
    SwarmWaypointLeaderMissionPath path;

    SwarmWaypointLeaderMissionSnapshot homeOrigin;
    homeOrigin.airMaster = airLease;
    homeOrigin.items = {
        missionItem(0, offsetPoint(0.0, 0.0, 1200.0),
                    MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL),
        missionItem(1, offsetPoint(20.0, 0.0, 30.0),
                    MAV_CMD_NAV_WAYPOINT,
                    MAV_FRAME_GLOBAL_RELATIVE_ALT),
        missionItem(2, offsetPoint(30.0, 0.0, 40.0),
                    MAV_CMD_DO_CHANGE_SPEED,
                    MAV_FRAME_GLOBAL_TERRAIN_ALT_INT),
        missionItem(3, offsetPoint(40.0, 0.0, 50.0))
    };
    QVERIFY2(SwarmWaypointLeaderMissionPath::build(
        homeOrigin, &path, &error), qPrintable(error));
    QCOMPARE(path.profile().size(), 3);
    QCOMPARE(path.start().relativeAltitudeM, 30.0);
    // The non-navigation item does not become the next leg's origin.
    QVERIFY(std::abs(path.lengthM() - 40.0) < 0.3);

    SwarmWaypointLeaderMissionSnapshot relativeZero;
    relativeZero.airMaster = airLease;
    relativeZero.items = {
        missionItem(0, offsetPoint(0.0, 0.0, 0.0)),
        missionItem(1, offsetPoint(20.0, 0.0, 30.0),
                    MAV_CMD_NAV_WAYPOINT,
                    MAV_FRAME_GLOBAL_RELATIVE_ALT)
    };
    QVERIFY2(SwarmWaypointLeaderMissionPath::build(
        relativeZero, &path, &error), qPrintable(error));
    QCOMPARE(path.start().relativeAltitudeM, 0.0);

    const QVector<int> rejectedDestinationFrames{
        MAV_FRAME_GLOBAL,
        MAV_FRAME_GLOBAL_INT,
        MAV_FRAME_GLOBAL_TERRAIN_ALT,
        MAV_FRAME_GLOBAL_TERRAIN_ALT_INT,
        MAV_FRAME_LOCAL_NED
    };
    for (int frame : rejectedDestinationFrames) {
        SwarmWaypointLeaderMissionSnapshot unsupportedDestination;
        unsupportedDestination.airMaster = airLease;
        unsupportedDestination.items = {
            missionItem(0, offsetPoint(0.0, 0.0, 10.0)),
            missionItem(1, offsetPoint(20.0, 0.0, 30.0),
                        MAV_CMD_NAV_WAYPOINT, frame)
        };
        QVERIFY(!SwarmWaypointLeaderMissionPath::build(
            unsupportedDestination, &path, &error));
        QCOMPARE(error, QStringLiteral(
            "The air-master mission navigation destination at sequence 1 uses unsupported frame %1; only GLOBAL_RELATIVE_ALT and GLOBAL_RELATIVE_ALT_INT are accepted.")
            .arg(frame));
        QVERIFY(!path.isValid());
    }

    SwarmWaypointLeaderMissionSnapshot unsupportedNonHomeOrigin;
    unsupportedNonHomeOrigin.airMaster = airLease;
    unsupportedNonHomeOrigin.items = {
        missionItem(1, offsetPoint(0.0, 0.0, 10.0),
                    MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL),
        missionItem(2, offsetPoint(20.0, 0.0, 30.0))
    };
    QVERIFY(!SwarmWaypointLeaderMissionPath::build(
        unsupportedNonHomeOrigin, &path, &error));
    QCOMPARE(error, QStringLiteral(
        "The air-master mission starts at sequence 1 in unsupported frame 0; only sequence 0 may provide a horizontal origin."));

    SwarmWaypointLeaderMissionSnapshot longLeg;
    longLeg.airMaster = airLease;
    longLeg.items = {
        missionItem(0, offsetPoint(0.0, 0.0, 10.0)),
        missionItem(1, offsetPoint(6000.0, 0.0, 10.0)),
        missionItem(2, offsetPoint(100.0, 0.0, 10.0))
    };
    QVERIFY(!SwarmWaypointLeaderMissionPath::build(
        longLeg, &path, &error));
    QCOMPARE(error, QStringLiteral(
        "The air-master mission leg ending at sequence 1 exceeds the 5000 m maximum."));
    QVERIFY(!path.isValid());

    SwarmWaypointLeaderMissionSnapshot duplicate = mission(airLease);
    duplicate.items[1].sequence = 0;
    QVERIFY(!SwarmWaypointLeaderMissionPath::build(
        duplicate, &path, &error));
    QCOMPARE(error, QStringLiteral(
        "The air-master mission contains a duplicate sequence."));

    SwarmWaypointLeaderMissionSnapshot invalid;
    invalid.airMaster = airLease;
    invalid.items = {
        {0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
         0, 0, 10.0F},
        {1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
         0, 100, 10.0F}
    };
    QVERIFY(!SwarmWaypointLeaderMissionPath::build(
        invalid, &path, &error));
    QCOMPARE(error, QStringLiteral(
        "The air-master mission contains an invalid waypoint coordinate or altitude."));
}

void SwarmWaypointLeaderCoreTest::
closestLineAndVGeometryAreDeterministic()
{
    const SwarmWaypointLeaderMissionSnapshot missionData =
        mission(lease(2, 2), 300.0);
    SwarmWaypointLeaderMissionPath path;
    QString error;
    QVERIFY2(SwarmWaypointLeaderMissionPath::build(
        missionData, &path, &error), qPrintable(error));

    double alongM = 0.0;
    double offPathM = 0.0;
    QVERIFY(path.closest(offsetPoint(75.0, 12.0, 14.0),
                         &alongM, &offPathM));
    QVERIFY(std::abs(alongM - 75.0) < 0.5);
    QVERIFY(std::abs(offPathM - 12.0) < 0.5);

    QVector<SwarmFollowPathPoint> line;
    QVERIFY(path.lineTargets(offsetPoint(40.0, 0.0),
                             20.0, 5.0, 3, &line));
    QCOMPARE(line.size(), 3);
    QCOMPARE(qRound(eastNorthFromBase(line.at(0)).first), 60);
    QCOMPARE(qRound(eastNorthFromBase(line.at(1)).first), 55);
    QCOMPARE(qRound(eastNorthFromBase(line.at(2)).first), 50);

    QVector<SwarmFollowPathPoint> v;
    QVERIFY(path.vTargets(offsetPoint(40.0, 0.0),
                          20.0, 10.0, 5, &v));
    QCOMPARE(v.size(), 5);
    const std::pair<double, double> firstWing =
        eastNorthFromBase(v.at(1));
    const std::pair<double, double> secondWing =
        eastNorthFromBase(v.at(2));
    QVERIFY(firstWing.second * secondWing.second < 0.0);
    QVERIFY(distance(v.at(1), v.at(2)) > 9.5);
    QVERIFY(!path.lineTargets(offsetPoint(299.0, 0.0),
                              20.0, 5.0, 2, &line));
    QVERIFY(line.isEmpty());
}

void SwarmWaypointLeaderCoreTest::
exactGroupRolesMissionAndFreshFieldsAreValidatedBeforeIntents()
{
    {
        Fixture fixture(true);
        fixture.plan.followers[0].order = 20;
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QVERIFY2(tick.hasIntents(), qPrintable(tick.status));
    }
    {
        Fixture fixture(true);
        fixture.plan.followers.append({lease(4, 4), 1});
        fixture.vehicles.append(vehicle(
            lease(4, 4), offsetPoint(30.0, 0.0)));
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("unique")));
        QVERIFY(tick.intents.isEmpty());
    }
    {
        Fixture fixture;
        fixture.air().telemetry.lease = lease(2, 2, 2, 9);
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("reconnected")));
        QVERIFY(tick.intents.isEmpty());
    }
    {
        Fixture fixture;
        fixture.air().telemetry.vehicleType = MAV_TYPE_FIXED_WING;
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("ArduCopter")));
        QVERIFY(tick.intents.isEmpty());
    }
    {
        Fixture fixture;
        fixture.ground().telemetry.positionObservedMs = 0;
        fixture.ground().telemetry.heartbeatObservedMs = 6000;
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 6000);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("ground-master position")));
        QVERIFY(tick.intents.isEmpty());
    }
    {
        Fixture fixture;
        fixture.air().telemetry.velocityValid = false;
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("air-master velocity")));
        QVERIFY(tick.intents.isEmpty());
    }
    {
        Fixture fixture;
        fixture.missionData.items[2].relativeAltitudeM = 31.0F;
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("mission changed")));
        QVERIFY(tick.intents.isEmpty());
    }
    {
        Fixture fixture;
        fixture.missionData.airMaster = lease(2, 2, 2, 2);
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("replaced air-master")));
        QVERIFY(tick.intents.isEmpty());
    }
    {
        Fixture fixture;
        fixture.missionData.missionType = MAV_MISSION_TYPE_RALLY;
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("main mission")));
        QVERIFY(tick.intents.isEmpty());
    }
    {
        Fixture fixture;
        fixture.missionData.contentGeneration++;
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("cache lease")));
        QVERIFY(tick.intents.isEmpty());
    }
    {
        Fixture fixture;
        fixture.missionData.contentDigest =
            QByteArrayLiteral("different-exact-mission-digest");
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick tick = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(tick.state), int(SwarmWaypointLeaderTick::State::Stopped));
        QVERIFY(tick.status.contains(QStringLiteral("cache lease")));
        QVERIFY(tick.intents.isEmpty());
    }
}

void SwarmWaypointLeaderCoreTest::
initializationRequestsFiveHertzAndUsesParameterAliases()
{
    Fixture fixture(true);
    SwarmWaypointLeaderCore core;
    const SwarmWaypointLeaderTick first = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QVERIFY(first.hasIntents());
    QCOMPARE(int(first.mode), int(SwarmWaypointLeaderMode::Idle));

    const QVector<SwarmWaypointLeaderCommandIntent> streams = intentsOfKind(
        first, SwarmWaypointLeaderCommandIntent::Kind::RequestPositionStream);
    QCOMPARE(streams.size(), 3);
    QVERIFY(streams.at(0).lease.sameInstance(fixture.groundLease));
    QVERIFY(streams.at(1).lease.sameInstance(fixture.airLease));
    QVERIFY(streams.at(2).lease.sameInstance(fixture.followerLease));
    for (const SwarmWaypointLeaderCommandIntent &stream : streams) {
        QCOMPARE(stream.streamRateHz, 5);
        QVERIFY(!stream.requiresVehicleAcknowledgement());
    }

    const QVector<SwarmWaypointLeaderCommandIntent> parameters = intentsOfKind(
        first, SwarmWaypointLeaderCommandIntent::Kind::SetParameter);
    QCOMPARE(parameters.size(), 4);
    QCOMPARE(parameters.at(0).parameterName, QStringLiteral("RTL_ALT"));
    QCOMPARE(parameters.at(0).parameterValue, 0.0);
    QCOMPARE(parameters.at(1).parameterName, QStringLiteral("WPNAV_ACCEL"));
    QCOMPARE(parameters.at(1).parameterValue, 100.0);
    QCOMPARE(parameters.at(2).parameterName, QStringLiteral("RTL_ALT_M"));
    QCOMPARE(parameters.at(3).parameterName, QStringLiteral("WP_ACC"));
    QCOMPARE(parameters.at(3).parameterValue, 1.0);
    QVERIFY(std::none_of(parameters.cbegin(), parameters.cend(),
                         [&fixture](const SwarmWaypointLeaderCommandIntent &intent) {
        return intent.lease.sameInstance(fixture.groundLease);
    }));

    const SwarmWaypointLeaderTick waiting = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(waiting.state), int(SwarmWaypointLeaderTick::State::
        WaitingForCommandCompletion));
    QCOMPARE(waiting.batchId, first.batchId);
    QVERIFY(waiting.intents.isEmpty());

    succeed(&core, first);
    QCOMPARE(int(core.mode()), int(SwarmWaypointLeaderMode::Takeoff));
}

void SwarmWaypointLeaderCoreTest::
completionGateRejectsWrongIdAndFailsClosedOnPartialResult()
{
    Fixture fixture;
    SwarmWaypointLeaderCore core;
    const SwarmWaypointLeaderTick first = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QVERIFY(first.hasIntents());
    QVERIFY(!core.completeBatch(first.batchId + 1,
                                SwarmWaypointLeaderBatchResult::Succeeded));
    QCOMPARE(core.pendingBatchId(), first.batchId);
    QVERIFY(core.completeBatch(first.batchId,
                               SwarmWaypointLeaderBatchResult::Partial,
                               QStringLiteral("1 of 3 frames sent")));
    QVERIFY(core.isStopped());

    const SwarmWaypointLeaderTick stopped = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(stopped.state), int(SwarmWaypointLeaderTick::State::Stopped));
    QVERIFY(stopped.status.contains(QStringLiteral("partially")));
    QVERIFY(stopped.status.contains(QStringLiteral("1 of 3")));
    QVERIFY(stopped.intents.isEmpty());
}

void SwarmWaypointLeaderCoreTest::
stagedTakeoffCommandsOnlyAirRolesAndAdvancesAsOneGroup()
{
    Fixture fixture(true);
    SwarmWaypointLeaderCore core;
    initialize(&core, &fixture);

    SwarmWaypointLeaderTick takeoff = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(takeoff.mode), int(SwarmWaypointLeaderMode::Takeoff));
    QCOMPARE(intentsOfKind(takeoff,
        SwarmWaypointLeaderCommandIntent::Kind::SetModeGuided).size(), 2);
    QCOMPARE(intentsOfKind(takeoff,
        SwarmWaypointLeaderCommandIntent::Kind::Arm).size(), 0);
    QCOMPARE(intentsOfKind(takeoff,
        SwarmWaypointLeaderCommandIntent::Kind::Takeoff).size(), 0);
    QCOMPARE(intentsOfKind(takeoff,
        SwarmWaypointLeaderCommandIntent::Kind::PositionTarget).size(), 0);
    QVERIFY(std::none_of(takeoff.intents.cbegin(), takeoff.intents.cend(),
                         [&fixture](const SwarmWaypointLeaderCommandIntent &intent) {
        return intent.lease.sameInstance(fixture.groundLease);
    }));
    succeed(&core, takeoff);

    // ACK alone is insufficient: the next stage waits for a newer heartbeat.
    SwarmWaypointLeaderTick staleGuided = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QVERIFY(!staleGuided.hasIntents());
    QVERIFY(staleGuided.status.contains(QStringLiteral("fresh heartbeat")));

    fixture.air().telemetry.customMode = 4U;
    fixture.follower().telemetry.customMode = 4U;
    fixture.air().telemetry.heartbeatObservedMs = 101;
    fixture.follower().telemetry.heartbeatObservedMs = 101;
    SwarmWaypointLeaderTick arm = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    QCOMPARE(intentsOfKind(arm,
        SwarmWaypointLeaderCommandIntent::Kind::SetModeGuided).size(), 0);
    QCOMPARE(intentsOfKind(arm,
        SwarmWaypointLeaderCommandIntent::Kind::Arm).size(), 2);
    QCOMPARE(intentsOfKind(arm,
        SwarmWaypointLeaderCommandIntent::Kind::Takeoff).size(), 0);
    succeed(&core, arm);

    SwarmWaypointLeaderTick staleArmed = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    QVERIFY(!staleArmed.hasIntents());
    QVERIFY(staleArmed.status.contains(QStringLiteral("fresh heartbeat")));

    fixture.air().telemetry.armed = true;
    fixture.follower().telemetry.armed = true;
    fixture.air().telemetry.baseMode |= MAV_MODE_FLAG_SAFETY_ARMED;
    fixture.follower().telemetry.baseMode |= MAV_MODE_FLAG_SAFETY_ARMED;
    fixture.air().telemetry.heartbeatObservedMs = 102;
    fixture.follower().telemetry.heartbeatObservedMs = 102;
    SwarmWaypointLeaderTick takeoffCommands = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 102);
    const QVector<SwarmWaypointLeaderCommandIntent> takeoffs = intentsOfKind(
        takeoffCommands, SwarmWaypointLeaderCommandIntent::Kind::Takeoff);
    QCOMPARE(takeoffs.size(), 2);
    SwarmWaypointLeaderMissionPath takeoffPath;
    QString takeoffPathError;
    QVERIFY2(SwarmWaypointLeaderMissionPath::build(
        fixture.missionData, &takeoffPath, &takeoffPathError),
        qPrintable(takeoffPathError));
    QVector<SwarmFollowPathPoint> baseTakeoffTargets;
    QVERIFY(takeoffPath.lineTargets(
        takeoffPath.start(), 3.0 * fixture.plan.settings.separationM,
        fixture.plan.settings.separationM, 2, &baseTakeoffTargets));
    QVERIFY(std::abs(takeoffs.at(0).takeoffAltitudeM
                     - baseTakeoffTargets.at(0).relativeAltitudeM) < 0.001);
    QVERIFY(std::abs(takeoffs.at(1).takeoffAltitudeM
                     - baseTakeoffTargets.at(1).relativeAltitudeM
                     - fixture.plan.settings.takeoffLandAltitudeSeparationM)
            < 0.001);
    succeed(&core, takeoffCommands);

    // Accepted TAKEOFF is issued once; subsequent low-altitude ticks wait
    // rather than creating duplicate commands.
    SwarmWaypointLeaderTick low = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 102);
    QVERIFY(!low.hasIntents());
    QCOMPARE(int(low.state), int(SwarmWaypointLeaderTick::State::Active));

    fixture.air().telemetry.relativeAltitudeM = 50.0;
    fixture.follower().telemetry.relativeAltitudeM = 50.0;
    SwarmWaypointLeaderTick positioning = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 102);
    const QVector<SwarmWaypointLeaderCommandIntent> firstTargets = intentsOfKind(
        positioning, SwarmWaypointLeaderCommandIntent::Kind::PositionTarget);
    QCOMPARE(firstTargets.size(), 2);
    setPosition(&fixture.air(), firstTargets.at(0).target);
    setPosition(&fixture.follower(), firstTargets.at(1).target);
    succeed(&core, positioning);
    QCOMPARE(int(core.mode()), int(SwarmWaypointLeaderMode::Takeoff));

    SwarmWaypointLeaderTick settled = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 102);
    QCOMPARE(intentsOfKind(settled,
        SwarmWaypointLeaderCommandIntent::Kind::PositionTarget).size(), 2);
    succeed(&core, settled);
    QCOMPARE(int(core.mode()),
             int(SwarmWaypointLeaderMode::FlyToGroundMaster));
}

void SwarmWaypointLeaderCoreTest::
flyToGroundMasterEntersFollowThroughAckGatedAcceleration()
{
    Fixture fixture;
    SwarmWaypointLeaderCore core;
    initialize(&core, &fixture);

    SwarmWaypointLeaderTick takeoff = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    succeed(&core, takeoff);
    setFlightReady(&fixture.air(), offsetPoint(40.0, 0.0, 50.0));
    fixture.air().telemetry.heartbeatObservedMs = 101;
    SwarmWaypointLeaderTick position = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    const SwarmFollowPathPoint staging = intentsOfKind(
        position, SwarmWaypointLeaderCommandIntent::Kind::PositionTarget)
            .constFirst().target;
    setPosition(&fixture.air(), staging);
    succeed(&core, position);
    SwarmWaypointLeaderTick settled = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    succeed(&core, settled);
    QCOMPARE(int(core.mode()),
             int(SwarmWaypointLeaderMode::FlyToGroundMaster));

    // Ground is at 20 m and Lead is 20 m, so x=40 m is the exact follow
    // handover point.
    setFlightReady(&fixture.air(), offsetPoint(40.0, 0.0, 12.0));
    const SwarmWaypointLeaderTick handover = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    QCOMPARE(int(handover.mode),
             int(SwarmWaypointLeaderMode::FlyToGroundMaster));
    QCOMPARE(intentsOfKind(handover,
        SwarmWaypointLeaderCommandIntent::Kind::PositionTarget).size(), 1);
    const QVector<SwarmWaypointLeaderCommandIntent> acceleration =
        intentsOfKind(handover,
            SwarmWaypointLeaderCommandIntent::Kind::SetParameter);
    QCOMPARE(acceleration.size(), 1);
    QCOMPARE(acceleration.constFirst().parameterName,
             QStringLiteral("WPNAV_ACCEL"));
    QCOMPARE(acceleration.constFirst().parameterValue, 100.0);
    QCOMPARE(intentsOfKind(handover,
        SwarmWaypointLeaderCommandIntent::Kind::SetModeGuided).size(), 0);

    const SwarmWaypointLeaderTick waiting = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    QCOMPARE(int(waiting.state), int(SwarmWaypointLeaderTick::State::
        WaitingForCommandCompletion));
    QCOMPARE(int(core.mode()),
             int(SwarmWaypointLeaderMode::FlyToGroundMaster));
    succeed(&core, handover);
    QCOMPARE(int(core.mode()),
             int(SwarmWaypointLeaderMode::FollowGroundMaster));
}

void SwarmWaypointLeaderCoreTest::
followUsesOneThirdVelocityVFormationAndAltitudeInterleave()
{
    Fixture fixture(true);
    fixture.plan.settings.vFormation = true;
    fixture.plan.settings.altitudeInterleave = true;
    SwarmWaypointLeaderCore core;
    initialize(&core, &fixture);

    SwarmWaypointLeaderTick takeoff = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    succeed(&core, takeoff);
    setFlightReady(&fixture.air(), offsetPoint(80.0, 0.0, 50.0));
    setFlightReady(&fixture.follower(), offsetPoint(75.0, 0.0, 50.0));
    fixture.air().telemetry.heartbeatObservedMs = 101;
    fixture.follower().telemetry.heartbeatObservedMs = 101;
    SwarmWaypointLeaderTick positioning = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    const QVector<SwarmWaypointLeaderCommandIntent> staging = intentsOfKind(
        positioning, SwarmWaypointLeaderCommandIntent::Kind::PositionTarget);
    QCOMPARE(staging.size(), 2);
    setPosition(&fixture.air(), staging.at(0).target);
    setPosition(&fixture.follower(), staging.at(1).target);
    succeed(&core, positioning);
    SwarmWaypointLeaderTick settled = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    succeed(&core, settled);
    QCOMPARE(int(core.mode()),
             int(SwarmWaypointLeaderMode::FlyToGroundMaster));

    setPosition(&fixture.ground(), offsetPoint(60.0, 0.0, 1.0));
    setFlightReady(&fixture.air(), offsetPoint(80.0, 0.0, 14.0));
    setFlightReady(&fixture.follower(), offsetPoint(75.0, 3.0, 16.0));
    SwarmWaypointLeaderTick handover = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    QVERIFY(handover.hasIntents());
    succeed(&core, handover);
    QCOMPARE(int(core.mode()),
             int(SwarmWaypointLeaderMode::FollowGroundMaster));

    fixture.ground().telemetry.velocityNorthMps = 6.0;
    fixture.ground().telemetry.velocityEastMps = 3.0;
    fixture.ground().telemetry.velocityDownMps = -0.9;
    const SwarmWaypointLeaderTick follow = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    const QVector<SwarmWaypointLeaderCommandIntent> commands = intentsOfKind(
        follow, SwarmWaypointLeaderCommandIntent::Kind::PositionTarget);
    QCOMPARE(commands.size(), 2);
    QVERIFY(commands.at(0).lease.sameInstance(fixture.airLease));
    QVERIFY(commands.at(1).lease.sameInstance(fixture.followerLease));
    QCOMPARE(commands.at(1).order, 1);
    QCOMPARE(commands.at(0).velocity.northMps, 2.0);
    QCOMPARE(commands.at(0).velocity.eastMps, 1.0);
    QVERIFY(std::abs(commands.at(0).velocity.downMps + 0.3) < 1.0e-12);
    QCOMPARE(commands.at(1).velocity.northMps, 2.0);
    const std::pair<double, double> front =
        eastNorthFromBase(commands.at(0).target);
    const std::pair<double, double> wing =
        eastNorthFromBase(commands.at(1).target);
    QVERIFY(std::abs(front.first - 80.0) < 0.6);
    QVERIFY(std::abs(wing.first - 75.0) < 0.6);
    QVERIFY(std::abs(wing.second) > 2.0);
    QVERIFY(commands.at(1).target.relativeAltitudeM
        > commands.at(0).target.relativeAltitudeM + 1.0);

    succeed(&core, follow);
    setPosition(&fixture.ground(), offsetPoint(60.0, 20.0, 1.0));
    const SwarmWaypointLeaderTick offPath = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 101);
    QCOMPARE(int(offPath.mode),
             int(SwarmWaypointLeaderMode::ReturnAlongMission));
    QVERIFY(!offPath.hasIntents());
    QVERIFY(offPath.status.contains(QStringLiteral("off path")));
}

void SwarmWaypointLeaderCoreTest::
offPathTransitionAndReturnAlongMissionPreserveFullBatch()
{
    Fixture fixture;
    SwarmWaypointLeaderCore core;
    initialize(&core, &fixture);
    fixture.air().telemetry.customMode = 4U;
    QString error;
    QVERIFY(core.requestMode(
        SwarmWaypointLeaderMode::ReturnAlongMission, &error));

    SwarmWaypointLeaderTick returning = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(returning.mode),
             int(SwarmWaypointLeaderMode::ReturnAlongMission));
    QCOMPARE(intentsOfKind(returning,
        SwarmWaypointLeaderCommandIntent::Kind::SetParameter).size(), 1);
    QCOMPARE(intentsOfKind(returning,
        SwarmWaypointLeaderCommandIntent::Kind::PositionTarget).size(), 1);
    QVERIFY(std::none_of(returning.intents.cbegin(), returning.intents.cend(),
                         [&fixture](const SwarmWaypointLeaderCommandIntent &intent) {
        return intent.lease.sameInstance(fixture.groundLease);
    }));
    succeed(&core, returning);

    setFlightReady(&fixture.air(), offsetPoint(399.0, 0.0, 30.0));
    const SwarmWaypointLeaderTick atEnd = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(atEnd.mode), int(SwarmWaypointLeaderMode::LandAltitude));
    QVERIFY(!atEnd.hasIntents());

    fixture.air().telemetry.armed = false;
    const SwarmWaypointLeaderTick rtl = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(intentsOfKind(rtl,
        SwarmWaypointLeaderCommandIntent::Kind::PositionTarget).size(), 1);
    QCOMPARE(intentsOfKind(rtl,
        SwarmWaypointLeaderCommandIntent::Kind::SetModeRtl).size(), 1);
    succeed(&core, rtl);
    QCOMPARE(int(core.mode()), int(SwarmWaypointLeaderMode::Landing));

    const SwarmWaypointLeaderTick completed = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(completed.state),
             int(SwarmWaypointLeaderTick::State::Completed));
    QVERIFY(!completed.shouldContinue());
    QVERIFY(completed.status.contains(QStringLiteral("completed")));
}

void SwarmWaypointLeaderCoreTest::
separatedLandingAltitudesDoNotRatchetAndRtlCompletes()
{
    Fixture fixture(true);
    SwarmWaypointLeaderCore core;
    initialize(&core, &fixture);
    fixture.air().telemetry.armed = true;
    fixture.follower().telemetry.armed = true;
    fixture.air().telemetry.customMode = 4U;
    fixture.follower().telemetry.customMode = 4U;
    QString error;
    QVERIFY(core.requestMode(SwarmWaypointLeaderMode::LandAltitude, &error));

    SwarmWaypointLeaderTick first = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QVector<SwarmWaypointLeaderCommandIntent> firstTargets = intentsOfKind(
        first, SwarmWaypointLeaderCommandIntent::Kind::PositionTarget);
    QCOMPARE(firstTargets.size(), 2);
    QCOMPARE(intentsOfKind(first,
        SwarmWaypointLeaderCommandIntent::Kind::SetModeRtl).size(), 0);
    const double firstAirAltitude = firstTargets.at(0).target.relativeAltitudeM;
    const double firstFollowerAltitude =
        firstTargets.at(1).target.relativeAltitudeM;
    QVERIFY(std::abs(firstFollowerAltitude
        - fixture.follower().telemetry.relativeAltitudeM - 2.0) < 1.0e-9);
    succeed(&core, first);

    SwarmWaypointLeaderTick second = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QVector<SwarmWaypointLeaderCommandIntent> secondTargets = intentsOfKind(
        second, SwarmWaypointLeaderCommandIntent::Kind::PositionTarget);
    QCOMPARE(secondTargets.size(), 2);
    QCOMPARE(secondTargets.at(0).target.relativeAltitudeM, firstAirAltitude);
    QCOMPARE(secondTargets.at(1).target.relativeAltitudeM,
             firstFollowerAltitude);
    succeed(&core, second);

    setPosition(&fixture.air(), firstTargets.at(0).target);
    setPosition(&fixture.follower(), firstTargets.at(1).target);
    SwarmWaypointLeaderTick rtl = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(intentsOfKind(rtl,
        SwarmWaypointLeaderCommandIntent::Kind::SetModeRtl).size(), 2);
    succeed(&core, rtl);
    QCOMPARE(int(core.mode()), int(SwarmWaypointLeaderMode::Landing));

    // Landing repairs at most one armed vehicle that has not actually
    // reported RTL, even though the preceding all-vehicle batch succeeded.
    SwarmWaypointLeaderTick airCorrection = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QVector<SwarmWaypointLeaderCommandIntent> corrections = intentsOfKind(
        airCorrection, SwarmWaypointLeaderCommandIntent::Kind::SetModeRtl);
    QCOMPARE(corrections.size(), 1);
    QVERIFY(corrections.constFirst().lease.sameInstance(fixture.airLease));
    succeed(&core, airCorrection);
    fixture.air().telemetry.customMode = 6U;

    SwarmWaypointLeaderTick followerCorrection = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    corrections = intentsOfKind(
        followerCorrection, SwarmWaypointLeaderCommandIntent::Kind::SetModeRtl);
    QCOMPARE(corrections.size(), 1);
    QVERIFY(corrections.constFirst().lease.sameInstance(
        fixture.followerLease));
    succeed(&core, followerCorrection);
    fixture.follower().telemetry.customMode = 6U;

    const SwarmWaypointLeaderTick descending = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(descending.state), int(SwarmWaypointLeaderTick::State::Active));
    QVERIFY(descending.intents.isEmpty());

    fixture.air().telemetry.armed = false;
    fixture.follower().telemetry.armed = false;
    const SwarmWaypointLeaderTick completed = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(completed.state),
             int(SwarmWaypointLeaderTick::State::Completed));
}

void SwarmWaypointLeaderCoreTest::
collisionOverridePreemptsNormalStateWithoutMutatingIt()
{
    {
        Fixture fixture(true);
        setFlightReady(&fixture.air(), offsetPoint(40.0, 0.0, 10.0));
        setFlightReady(&fixture.follower(), offsetPoint(40.5, 0.0, 10.0));
        SwarmWaypointLeaderCore core;

        const SwarmWaypointLeaderTick collision = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(collision.mode), int(SwarmWaypointLeaderMode::Idle));
        QVERIFY(collision.status.contains(QStringLiteral("Collision")));
        QCOMPARE(intentsOfKind(collision,
            SwarmWaypointLeaderCommandIntent::Kind::RequestPositionStream).size(), 0);
        const QVector<SwarmWaypointLeaderCommandIntent> commands = intentsOfKind(
            collision, SwarmWaypointLeaderCommandIntent::Kind::PositionTarget);
        QCOMPARE(commands.size(), 1);
        QVERIFY(collision.urgent);
        QCOMPARE(collision.batchId, quint64(0));
        QCOMPARE(collision.cancelBatchId, quint64(0));
        QVERIFY(commands.constFirst().lease.sameInstance(
            fixture.followerLease));
        QCOMPARE(commands.constFirst().target.relativeAltitudeM, 12.0);
        QCOMPARE(int(core.mode()), int(SwarmWaypointLeaderMode::Idle));

        setPosition(&fixture.follower(), offsetPoint(55.0, 0.0, 12.0));
        const SwarmWaypointLeaderTick initialization = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(intentsOfKind(initialization,
            SwarmWaypointLeaderCommandIntent::Kind::RequestPositionStream).size(), 3);
    }
    {
        Fixture fixture(true);
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick initialization = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QVERIFY(initialization.hasIntents());

        setFlightReady(&fixture.air(), offsetPoint(40.0, 0.0, 10.0));
        setFlightReady(&fixture.follower(), offsetPoint(40.5, 0.0, 10.0));
        const SwarmWaypointLeaderTick emergency = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(emergency.state),
                 int(SwarmWaypointLeaderTick::State::Cancelling));
        QVERIFY(emergency.urgent);
        QCOMPARE(emergency.cancelBatchId, initialization.batchId);
        QCOMPARE(core.pendingBatchId(), initialization.batchId);
        QCOMPARE(intentsOfKind(emergency,
            SwarmWaypointLeaderCommandIntent::Kind::PositionTarget).size(), 1);

        QVERIFY(core.completeBatch(
            initialization.batchId,
            SwarmWaypointLeaderBatchResult::Cancelled));
        QCOMPARE(core.pendingBatchId(), quint64(0));
        QVERIFY(!core.isStopped());
    }
    {
        Fixture fixture(true);
        SwarmWaypointLeaderCore core;
        const SwarmWaypointLeaderTick initialization = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QVERIFY(initialization.hasIntents());

        setFlightReady(&fixture.air(), offsetPoint(40.0, 0.0, 10.0), 5U);
        setFlightReady(&fixture.follower(), offsetPoint(40.5, 0.0, 10.0));
        const SwarmWaypointLeaderTick rejected = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        QCOMPARE(int(rejected.state),
                 int(SwarmWaypointLeaderTick::State::Cancelling));
        QVERIFY(!rejected.urgent);
        QCOMPARE(rejected.cancelBatchId, initialization.batchId);
        QCOMPARE(core.pendingBatchId(), initialization.batchId);
        QVERIFY(rejected.status.contains(QStringLiteral("exact GUIDED")));
        QVERIFY(intentsOfKind(rejected,
            SwarmWaypointLeaderCommandIntent::Kind::PositionTarget).isEmpty());
        QVERIFY(core.completeBatch(
            initialization.batchId,
            SwarmWaypointLeaderBatchResult::Cancelled));
        QVERIFY(core.isStopped());
    }
    {
        Fixture fixture(true);
        setFlightReady(&fixture.air(), offsetPoint(0.0, 0.0, 10.0));
        setFlightReady(&fixture.follower(), offsetPoint(10.0, 0.0, 10.0));
        fixture.air().telemetry.velocityEastMps = 5.0;
        fixture.follower().telemetry.velocityEastMps = -5.0;
        SwarmWaypointLeaderCore core;

        const SwarmWaypointLeaderTick collision = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        const QVector<SwarmWaypointLeaderCommandIntent> commands = intentsOfKind(
            collision, SwarmWaypointLeaderCommandIntent::Kind::PositionTarget);
        QCOMPARE(commands.size(), 2);
        QVERIFY(commands.at(0).lease.sameInstance(fixture.airLease));
        QCOMPARE(commands.at(0).target.relativeAltitudeM, 12.0);
        QVERIFY(commands.at(1).lease.sameInstance(fixture.followerLease));
        QCOMPARE(commands.at(1).target.relativeAltitudeM, 10.0);
    }
    {
        Fixture fixture(true);
        setFlightReady(&fixture.air(), offsetPoint(0.0, 0.0, 10.0));
        setFlightReady(&fixture.follower(), offsetPoint(7.0, 0.0, 10.0));
        fixture.air().telemetry.velocityEastMps = 10.0;
        fixture.follower().telemetry.velocityEastMps = 5.0;
        fixture.air().telemetry.vfrHudValid = true;
        fixture.air().telemetry.vfrHudObservedMs = 100;
        fixture.air().telemetry.groundSpeedMps = 10.0;
        SwarmWaypointLeaderCore core;

        const SwarmWaypointLeaderTick collision = core.tick(
            fixture.plan, fixture.vehicles, fixture.missionData, 100);
        const QVector<SwarmWaypointLeaderCommandIntent> commands = intentsOfKind(
            collision, SwarmWaypointLeaderCommandIntent::Kind::PositionTarget);
        QCOMPARE(commands.size(), 1);
        QVERIFY(commands.constFirst().lease.sameInstance(fixture.airLease));
        QVERIFY(distance(commands.constFirst().target,
                         offsetPoint(0.0, 0.0, 10.0)) < 0.01);
    }
}

void SwarmWaypointLeaderCoreTest::
capturedPlanChangeStopsBeforeAnotherBatch()
{
    Fixture fixture;
    SwarmWaypointLeaderCore core;
    const SwarmWaypointLeaderTick first = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QVERIFY(first.hasIntents());

    fixture.plan.settings.leadM += 1.0;
    const SwarmWaypointLeaderTick cancelling = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(cancelling.state),
             int(SwarmWaypointLeaderTick::State::Cancelling));
    QCOMPARE(cancelling.cancelBatchId, first.batchId);
    QCOMPARE(core.pendingBatchId(), first.batchId);
    QVERIFY(cancelling.status.contains(QStringLiteral("confirmed plan changed")));
    QVERIFY(cancelling.intents.isEmpty());
    QVERIFY(core.completeBatch(
        first.batchId, SwarmWaypointLeaderBatchResult::Cancelled));
    QVERIFY(core.isStopped());
    const SwarmWaypointLeaderTick stopped = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(stopped.state), int(SwarmWaypointLeaderTick::State::Stopped));
    QCOMPARE(core.pendingBatchId(), quint64(0));
}

void SwarmWaypointLeaderCoreTest::
requestedModesResetOnlyTheirOfficialState()
{
    Fixture fixture;
    SwarmWaypointLeaderCore core;
    initialize(&core, &fixture);
    QString error;
    QVERIFY(!core.requestMode(
        SwarmWaypointLeaderMode::FollowGroundMaster, &error));
    QVERIFY(error.contains(QStringLiteral("Only Idle")));

    fixture.air().telemetry.customMode = 4U;
    QVERIFY(core.requestMode(
        SwarmWaypointLeaderMode::ReturnAlongMission, &error));
    SwarmWaypointLeaderTick returning = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(returning.mode),
             int(SwarmWaypointLeaderMode::ReturnAlongMission));
    QVERIFY(returning.hasIntents());

    // A reset request is queued behind the in-flight batch and cannot make
    // the previous ACK belong to a different state.
    QVERIFY(core.requestMode(SwarmWaypointLeaderMode::Idle, &error));
    SwarmWaypointLeaderTick waiting = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(waiting.state), int(SwarmWaypointLeaderTick::State::
        WaitingForCommandCompletion));
    QCOMPARE(int(core.mode()),
             int(SwarmWaypointLeaderMode::ReturnAlongMission));
    succeed(&core, returning);

    SwarmWaypointLeaderTick restarted = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(int(restarted.mode), int(SwarmWaypointLeaderMode::Idle));
    QCOMPARE(intentsOfKind(restarted,
        SwarmWaypointLeaderCommandIntent::Kind::RequestPositionStream).size(), 0);
    QCOMPARE(intentsOfKind(restarted,
        SwarmWaypointLeaderCommandIntent::Kind::SetParameter).size(), 2);
    succeed(&core, restarted);
    QCOMPARE(int(core.mode()), int(SwarmWaypointLeaderMode::Takeoff));

    core.reset();
    QCOMPARE(int(core.mode()), int(SwarmWaypointLeaderMode::Idle));
    QCOMPARE(core.pendingBatchId(), quint64(0));
    SwarmWaypointLeaderTick newRun = core.tick(
        fixture.plan, fixture.vehicles, fixture.missionData, 100);
    QCOMPARE(intentsOfKind(newRun,
        SwarmWaypointLeaderCommandIntent::Kind::RequestPositionStream).size(), 2);
}

QTEST_APPLESS_MAIN(SwarmWaypointLeaderCoreTest)

#include "test_swarmwaypointleadercore.moc"
