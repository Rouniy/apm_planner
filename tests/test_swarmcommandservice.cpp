#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/SwarmCommandService.h"

#include <QHash>
#include <QSignalSpy>
#include <QtTest>

#include <functional>

namespace
{

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId, int systemId,
                         int componentId = MAV_COMP_ID_AUTOPILOT1)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = componentId;
    value.linkName = QStringLiteral("Link %1").arg(linkId);
    value.componentName = VehicleEndpoint::defaultComponentName(componentId);
    return value;
}

mavlink_message_t heartbeat(int systemId,
                            int componentId = MAV_COMP_ID_AUTOPILOT1,
                            quint32 customMode = 17,
                            quint8 baseMode = 0)
{
    mavlink_heartbeat_t payload{};
    payload.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    payload.type = MAV_TYPE_QUADROTOR;
    payload.base_mode = baseMode;
    payload.custom_mode = customMode;
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

mavlink_message_t vfrHud(int systemId)
{
    mavlink_vfr_hud_t payload{};
    payload.airspeed = 12.5F;
    payload.groundspeed = 10.25F;
    payload.heading = 91;
    payload.throttle = 42;
    payload.alt = 124.0F;
    payload.climb = -0.75F;
    mavlink_message_t message{};
    mavlink_msg_vfr_hud_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_message_t attitude(int systemId, quint32 bootTimeMs = 1010)
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

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    if (state != MAVLINK_FRAMING_OK) {
        return {};
    }
    return message;
}

SwarmCommandMember member(
    int slotId, const SwarmVehicleInstanceLease &lease,
    SwarmTelemetryRequirements::Fields fields =
        SwarmTelemetryRequirements::NoFields)
{
    SwarmCommandMember value;
    value.slotId = slotId;
    value.lease = lease;
    value.required.fields = fields;
    return value;
}

class Fixture
{
public:
    Fixture()
        : registry([this]() { return registryNowMs; })
        , transmitter([this](int linkId, const QByteArray &bytes) {
            ++writeAttempts;
            frames.append({linkId, bytes});
            if (onWrite) {
                onWrite();
            }
            return failWriteAttempt <= 0
                || writeAttempts != failWriteAttempt;
        })
        , service(
              &registry, &transmitter,
              [this](const SwarmVehicleInstanceLease &lease, QString *error) {
                  if (routeHook) {
                      return routeHook(lease, error);
                  }
                  if (error) {
                      error->clear();
                  }
                  return true;
              },
              [this]() { return serviceNowMs; })
    {
    }

    SwarmVehicleInstanceLease addVehicle(int linkId, int systemId,
                                         bool withAllTelemetry = false)
    {
        const quint64 session = registry.beginLinkSession(
            linkId, QStringLiteral("Link %1").arg(linkId));
        sessions.insert(linkId, session);
        if (!registry.observeMessage(linkId, session,
                                     heartbeat(systemId))) {
            return {};
        }
        if (withAllTelemetry) {
            observeAll(linkId, systemId);
        }
        return registry.acquireVehicle(endpoint(linkId, systemId));
    }

    void observeAll(int linkId, int systemId)
    {
        const quint64 session = sessions.value(linkId);
        registry.observeMessage(linkId, session, globalPosition(systemId));
        registry.observeMessage(linkId, session, vfrHud(systemId));
        registry.observeMessage(linkId, session, attitude(systemId));
        registry.observeMessage(linkId, session, extendedState(systemId));
    }

    qint64 registryNowMs = 100;
    qint64 serviceNowMs = 1000;
    int writeAttempts = 0;
    int failWriteAttempt = 0;
    std::function<void()> onWrite;
    SwarmCommandService::RouteValidator routeHook;
    QVector<CapturedFrame> frames;
    QHash<int, quint64> sessions;
    SwarmTelemetryRegistry registry;
    ExactLinkTransmitter transmitter;
    SwarmCommandService service;
};

} // namespace

class SwarmCommandServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void reservationRequiresUniqueSlotsEndpointsAndOwner();
    void routeEligibilityIsAReadOnlyPreflight();
    void requiredTelemetryMustBeFreshBeforeCommands_data();
    void requiredTelemetryMustBeFreshBeforeCommands();
    void positionTargetsUseExactMasksTargetsAndPhysicalLinks();
    void positionTargetsRejectNullIslandAfterWireRounding();
    void positionBatchRateLimitUsesInjectedClock();
    void secondTransportFailureReportsPartialBatch();
    void synchronousCancellationCannotInterleaveNewSession();
    void routeCancellationCannotLeakPhysicalFrame();
    void guidedModeLossDuringRouteValidationCannotLeakPhysicalFrame();
    void ownerDestructionAndEndpointRetirementCancelSession();
    void streamRequestsDecodeExactlyPerPhysicalLink();
    void positionOnlyStreamRequestsExcludeAttitude();
};

void SwarmCommandServiceTest::routeEligibilityIsAReadOnlyPreflight()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease lease = fixture.addVehicle(7, 42);
    QString error;
    QVERIFY(fixture.service.routeIsEligible(lease, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(!fixture.service.hasActiveSession());

    fixture.routeHook = [](const SwarmVehicleInstanceLease &, QString *reason) {
        if (reason) {
            *reason = QStringLiteral("Listening UDP is not exact.");
        }
        return false;
    };
    QVERIFY(!fixture.service.routeIsEligible(lease, &error));
    QVERIFY(error.contains(QStringLiteral("UDP")));
    QVERIFY(!fixture.service.hasActiveSession());

    fixture.registryNowMs += 5001;
    QVERIFY(!fixture.service.routeIsEligible(lease, &error));
    QVERIFY(error.contains(QStringLiteral("stale"), Qt::CaseInsensitive));
}

void SwarmCommandServiceTest::
reservationRequiresUniqueSlotsEndpointsAndOwner()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease first = fixture.addVehicle(7, 42);
    const SwarmVehicleInstanceLease second = fixture.addVehicle(19, 42);
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());

    QObject owner;
    QObject competingOwner;
    SwarmCommandSessionToken token;
    QString error;

    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, first), member(1, second)}, 4,
                 &token, &error),
             SwarmCommandService::Result::InvalidPlan);
    QVERIFY(!token.isValid());

    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, first), member(2, first)}, 4,
                 &token, &error),
             SwarmCommandService::Result::InvalidPlan);
    QVERIFY(!token.isValid());

    SwarmCommandMember unknownFields = member(1, first);
    unknownFields.required.fields = SwarmTelemetryRequirements::Fields(
        static_cast<SwarmTelemetryRequirements::Field>(0x40));
    QCOMPARE(fixture.service.reserve(
                 &owner, {unknownFields}, 4, &token, &error),
             SwarmCommandService::Result::InvalidPlan);
    QVERIFY(!token.isValid());

    SwarmVehicleInstanceLease forged = first;
    ++forged.instanceEpoch;
    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, first), member(2, forged)}, 4,
                 &token, &error),
             SwarmCommandService::Result::InvalidPlan);
    QVERIFY(!token.isValid());

    QCOMPARE(fixture.service.reserve(
                 &owner, {member(20, second), member(10, first)}, 4,
                 &token, &error),
             SwarmCommandService::Result::Reserved);
    QVERIFY(token.isValid());
    QVERIFY(fixture.service.hasActiveSession());
    QCOMPARE(fixture.service.activeSessionId(), token.id);

    SwarmCommandSessionToken competingToken;
    QCOMPARE(fixture.service.reserve(
                 &competingOwner, {member(1, first)}, 4,
                 &competingToken, &error),
             SwarmCommandService::Result::Busy);
    QVERIFY(!competingToken.isValid());
    QCOMPARE(fixture.service.release(token),
             SwarmCommandService::Result::Cancelled);

    const quint64 replacementSession = fixture.registry.beginLinkSession(7);
    QVERIFY(replacementSession != first.linkSessionEpoch);
    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, first)}, 4, &token, &error),
             SwarmCommandService::Result::StaleLease);
    QVERIFY(!token.isValid());
    QCOMPARE(fixture.frames.size(), 0);
}

void SwarmCommandServiceTest::requiredTelemetryMustBeFreshBeforeCommands_data()
{
    QTest::addColumn<int>("requiredField");

    QTest::newRow("position")
        << int(SwarmTelemetryRequirements::Position);
    QTest::newRow("velocity")
        << int(SwarmTelemetryRequirements::Velocity);
    QTest::newRow("heading")
        << int(SwarmTelemetryRequirements::Heading);
    QTest::newRow("attitude")
        << int(SwarmTelemetryRequirements::Attitude);
    QTest::newRow("vfr-hud")
        << int(SwarmTelemetryRequirements::VfrHud);
    QTest::newRow("extended-system-state")
        << int(SwarmTelemetryRequirements::ExtendedSystemState);
}

void SwarmCommandServiceTest::requiredTelemetryMustBeFreshBeforeCommands()
{
    QFETCH(int, requiredField);

    Fixture fixture;
    const SwarmVehicleInstanceLease lease = fixture.addVehicle(4, 44, true);
    QVERIFY(lease.isValid());

    SwarmCommandMember required = member(
        1, lease,
        SwarmTelemetryRequirements::Fields(
            static_cast<SwarmTelemetryRequirements::Field>(requiredField)));
    switch (requiredField) {
    case SwarmTelemetryRequirements::Position:
        required.required.positionMaximumAgeMs = 50;
        break;
    case SwarmTelemetryRequirements::Velocity:
        required.required.velocityMaximumAgeMs = 50;
        break;
    case SwarmTelemetryRequirements::Heading:
        required.required.headingMaximumAgeMs = 50;
        break;
    case SwarmTelemetryRequirements::Attitude:
        required.required.attitudeMaximumAgeMs = 50;
        break;
    case SwarmTelemetryRequirements::VfrHud:
        required.required.vfrHudMaximumAgeMs = 50;
        break;
    case SwarmTelemetryRequirements::ExtendedSystemState:
        required.required.extendedSystemStateMaximumAgeMs = 50;
        break;
    default:
        QFAIL("Unknown telemetry requirement in test data");
    }
    required.required.heartbeatMaximumAgeMs = 500;

    fixture.registryNowMs = 151;
    QVERIFY(fixture.registry.observeMessage(
        4, fixture.sessions.value(4), heartbeat(44)));
    QObject owner;
    SwarmCommandSessionToken token;
    QString error;
    QCOMPARE(fixture.service.reserve(
                 &owner, {required}, 4, &token, &error),
             SwarmCommandService::Result::Reserved);
    QVERIFY(token.isValid());

    SwarmPositionTarget target;
    target.slotId = 1;
    target.latitudeDegrees = 35.0;
    target.longitudeDegrees = 33.0;
    target.relativeAltitudeM = 20.0F;
    const SwarmCommandService::BatchReport stale =
        fixture.service.sendPositionTargets(token, {target});
    QCOMPARE(stale.result, SwarmCommandService::Result::TelemetryStale);
    QVERIFY(stale.detail.contains(QStringLiteral("telemetry"),
                                  Qt::CaseInsensitive));
    QCOMPARE(fixture.frames.size(), 0);

    switch (requiredField) {
    case SwarmTelemetryRequirements::Position:
    case SwarmTelemetryRequirements::Velocity:
        QVERIFY(fixture.registry.observeMessage(
            4, fixture.sessions.value(4), globalPosition(44, 2000)));
        break;
    case SwarmTelemetryRequirements::Heading:
    case SwarmTelemetryRequirements::VfrHud:
        QVERIFY(fixture.registry.observeMessage(
            4, fixture.sessions.value(4), vfrHud(44)));
        break;
    case SwarmTelemetryRequirements::Attitude:
        QVERIFY(fixture.registry.observeMessage(
            4, fixture.sessions.value(4), attitude(44, 2000)));
        break;
    case SwarmTelemetryRequirements::ExtendedSystemState:
        QVERIFY(fixture.registry.observeMessage(
            4, fixture.sessions.value(4), extendedState(44)));
        break;
    default:
        QFAIL("Unknown telemetry requirement in test data");
    }

    QCOMPARE(fixture.service.sendPositionTargets(token, {target}).result,
             SwarmCommandService::Result::SentAll);
    QCOMPARE(fixture.frames.size(), 1);
}

void SwarmCommandServiceTest::
positionTargetsUseExactMasksTargetsAndPhysicalLinks()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease first = fixture.addVehicle(7, 42);
    const SwarmVehicleInstanceLease second = fixture.addVehicle(19, 42);
    QObject owner;
    SwarmCommandSessionToken token;
    QCOMPARE(fixture.service.reserve(
                 &owner, {member(10, first), member(20, second)}, 10,
                 &token),
             SwarmCommandService::Result::Reserved);

    SwarmPositionTarget velocityTarget;
    velocityTarget.slotId = 20;
    velocityTarget.latitudeDegrees = -35.1234567;
    velocityTarget.longitudeDegrees = 149.7654321;
    velocityTarget.relativeAltitudeM = 82.25F;
    velocityTarget.velocityNorthMps = 1.25F;
    velocityTarget.velocityEastMps = -2.5F;
    velocityTarget.velocityDownMps = 0.75F;
    velocityTarget.useVelocity = true;

    SwarmPositionTarget positionOnlyTarget;
    positionOnlyTarget.slotId = 10;
    positionOnlyTarget.latitudeDegrees = 12.3456789;
    positionOnlyTarget.longitudeDegrees = -45.6789123;
    positionOnlyTarget.relativeAltitudeM = 14.5F;
    positionOnlyTarget.velocityNorthMps = 9.0F;
    positionOnlyTarget.velocityEastMps = 8.0F;
    positionOnlyTarget.velocityDownMps = 7.0F;
    positionOnlyTarget.useVelocity = false;

    const SwarmCommandService::BatchReport report =
        fixture.service.sendPositionTargets(
            token, {velocityTarget, positionOnlyTarget});
    QCOMPARE(report.result, SwarmCommandService::Result::SentAll);
    QCOMPARE(report.members.size(), 2);
    QCOMPARE(fixture.frames.size(), 2);
    // The batch is canonicalized by slot, independent of caller order.
    QCOMPARE(fixture.frames.at(0).linkId, 7);
    QCOMPARE(fixture.frames.at(1).linkId, 19);

    constexpr quint16 positionVelocityMask =
        POSITION_TARGET_TYPEMASK_AX_IGNORE
        | POSITION_TARGET_TYPEMASK_AY_IGNORE
        | POSITION_TARGET_TYPEMASK_AZ_IGNORE
        | POSITION_TARGET_TYPEMASK_YAW_IGNORE
        | POSITION_TARGET_TYPEMASK_YAW_RATE_IGNORE;
    constexpr quint16 positionOnlyMask = positionVelocityMask
        | POSITION_TARGET_TYPEMASK_VX_IGNORE
        | POSITION_TARGET_TYPEMASK_VY_IGNORE
        | POSITION_TARGET_TYPEMASK_VZ_IGNORE;
    QCOMPARE(positionVelocityMask, quint16(0x0dc0));
    QCOMPARE(positionOnlyMask, quint16(0x0df8));

    const mavlink_message_t positionMessage =
        decodeFrame(fixture.frames.at(0).bytes);
    QCOMPARE(positionMessage.msgid,
             quint32(MAVLINK_MSG_ID_SET_POSITION_TARGET_GLOBAL_INT));
    QCOMPARE(positionMessage.sysid, quint8(255));
    QCOMPARE(positionMessage.compid,
             quint8(MAV_COMP_ID_MISSIONPLANNER));
    mavlink_set_position_target_global_int_t positionPayload{};
    mavlink_msg_set_position_target_global_int_decode(
        &positionMessage, &positionPayload);
    QCOMPARE(positionPayload.target_system, quint8(42));
    QCOMPARE(positionPayload.target_component,
             quint8(MAV_COMP_ID_AUTOPILOT1));
    QCOMPARE(positionPayload.coordinate_frame,
             quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT_INT));
    QCOMPARE(positionPayload.type_mask, positionOnlyMask);
    QCOMPARE(positionPayload.time_boot_ms, quint32(0));
    QCOMPARE(positionPayload.lat_int, qint32(123456789));
    QCOMPARE(positionPayload.lon_int, qint32(-456789123));
    QCOMPARE(positionPayload.alt, 14.5F);
    QCOMPARE(positionPayload.vx, 9.0F);
    QCOMPARE(positionPayload.vy, 8.0F);
    QCOMPARE(positionPayload.vz, 7.0F);

    const mavlink_message_t velocityMessage =
        decodeFrame(fixture.frames.at(1).bytes);
    mavlink_set_position_target_global_int_t velocityPayload{};
    mavlink_msg_set_position_target_global_int_decode(
        &velocityMessage, &velocityPayload);
    QCOMPARE(velocityPayload.target_system, quint8(42));
    QCOMPARE(velocityPayload.target_component,
             quint8(MAV_COMP_ID_AUTOPILOT1));
    QCOMPARE(velocityPayload.coordinate_frame,
             quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT_INT));
    QCOMPARE(velocityPayload.type_mask, positionVelocityMask);
    QCOMPARE(velocityPayload.time_boot_ms, quint32(0));
    QCOMPARE(velocityPayload.lat_int, qint32(-351234567));
    QCOMPARE(velocityPayload.lon_int, qint32(1497654321));
    QCOMPARE(velocityPayload.alt, 82.25F);
    QCOMPARE(velocityPayload.vx, 1.25F);
    QCOMPARE(velocityPayload.vy, -2.5F);
    QCOMPARE(velocityPayload.vz, 0.75F);
}

void SwarmCommandServiceTest::
positionTargetsRejectNullIslandAfterWireRounding()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease lease = fixture.addVehicle(5, 51);
    QObject owner;
    SwarmCommandSessionToken token;
    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, lease)}, 10, &token),
             SwarmCommandService::Result::Reserved);

    SwarmPositionTarget target;
    target.slotId = 1;
    target.latitudeDegrees = 0.000000001;
    target.longitudeDegrees = -0.000000001;
    target.relativeAltitudeM = 25.0F;
    const SwarmCommandService::BatchReport report =
        fixture.service.sendPositionTargets(token, {target});
    QCOMPARE(report.result, SwarmCommandService::Result::InvalidPayload);
    QCOMPARE(fixture.frames.size(), 0);
}

void SwarmCommandServiceTest::positionBatchRateLimitUsesInjectedClock()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease lease = fixture.addVehicle(5, 51);
    QObject owner;
    SwarmCommandSessionToken token;
    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, lease)}, 4, &token),
             SwarmCommandService::Result::Reserved);

    SwarmPositionTarget target;
    target.slotId = 1;
    target.latitudeDegrees = 35.0;
    target.longitudeDegrees = 33.0;
    target.relativeAltitudeM = 25.0F;

    QCOMPARE(fixture.service.sendPositionTargets(token, {target}).result,
             SwarmCommandService::Result::SentAll);
    QCOMPARE(fixture.frames.size(), 1);

    fixture.serviceNowMs = 1249;
    QCOMPARE(fixture.service.sendPositionTargets(token, {target}).result,
             SwarmCommandService::Result::RateLimited);
    QCOMPARE(fixture.frames.size(), 1);

    fixture.serviceNowMs = 1250;
    QCOMPARE(fixture.service.sendPositionTargets(token, {target}).result,
             SwarmCommandService::Result::SentAll);
    QCOMPARE(fixture.frames.size(), 2);

    fixture.serviceNowMs = 1240;
    QCOMPARE(fixture.service.sendPositionTargets(token, {target}).result,
             SwarmCommandService::Result::RateLimited);
    QCOMPARE(fixture.frames.size(), 2);
}

void SwarmCommandServiceTest::secondTransportFailureReportsPartialBatch()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease first = fixture.addVehicle(3, 31);
    const SwarmVehicleInstanceLease second = fixture.addVehicle(6, 61);
    const SwarmVehicleInstanceLease third = fixture.addVehicle(9, 91);
    QObject owner;
    SwarmCommandSessionToken token;
    QCOMPARE(fixture.service.reserve(
                 &owner,
                 {member(1, first), member(2, second), member(3, third)},
                 10, &token),
             SwarmCommandService::Result::Reserved);

    SwarmPositionTarget firstTarget;
    firstTarget.slotId = 1;
    firstTarget.latitudeDegrees = 35.0;
    firstTarget.longitudeDegrees = 33.0;
    firstTarget.relativeAltitudeM = 20.0F;
    SwarmPositionTarget secondTarget = firstTarget;
    secondTarget.slotId = 2;
    SwarmPositionTarget thirdTarget = firstTarget;
    thirdTarget.slotId = 3;

    fixture.failWriteAttempt = 2;
    const SwarmCommandService::BatchReport report =
        fixture.service.sendPositionTargets(
            token, {firstTarget, secondTarget, thirdTarget});
    QCOMPARE(report.result, SwarmCommandService::Result::PartialSend);
    QCOMPARE(report.members.size(), 3);
    QCOMPARE(report.members.at(0).slotId, 1);
    QCOMPARE(report.members.at(0).result,
             SwarmCommandService::Result::SentAll);
    QCOMPARE(report.members.at(1).slotId, 2);
    QCOMPARE(report.members.at(1).result,
             SwarmCommandService::Result::TransportUnavailable);
    QCOMPARE(report.members.at(2).slotId, 3);
    QCOMPARE(report.members.at(2).result,
             SwarmCommandService::Result::RejectedBeforeSend);
    QCOMPARE(fixture.writeAttempts, 2);
    QCOMPARE(fixture.frames.size(), 2);
    QCOMPARE(fixture.frames.at(0).linkId, 3);
    QCOMPARE(fixture.frames.at(1).linkId, 6);
}

void SwarmCommandServiceTest::
synchronousCancellationCannotInterleaveNewSession()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease first = fixture.addVehicle(3, 31);
    const SwarmVehicleInstanceLease second = fixture.addVehicle(6, 61);
    QObject owner;
    QObject replacementOwner;
    SwarmCommandSessionToken token;
    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, first), member(2, second)}, 10, &token),
             SwarmCommandService::Result::Reserved);

    SwarmCommandSessionToken attemptedReplacement;
    SwarmCommandService::Result replacementResult =
        SwarmCommandService::Result::InvalidPlan;
    bool didReenter = false;
    fixture.onWrite = [&]() {
        if (didReenter) {
            return;
        }
        didReenter = true;
        QCOMPARE(fixture.service.release(token),
                 SwarmCommandService::Result::Cancelled);
        replacementResult = fixture.service.reserve(
            &replacementOwner, {member(1, first)}, 10,
            &attemptedReplacement);
    };

    SwarmPositionTarget firstTarget;
    firstTarget.slotId = 1;
    firstTarget.latitudeDegrees = 35.0;
    firstTarget.longitudeDegrees = 33.0;
    firstTarget.relativeAltitudeM = 20.0F;
    SwarmPositionTarget secondTarget = firstTarget;
    secondTarget.slotId = 2;

    const SwarmCommandService::BatchReport report =
        fixture.service.sendPositionTargets(token, {firstTarget, secondTarget});
    QCOMPARE(report.result, SwarmCommandService::Result::PartialSend);
    QCOMPARE(fixture.writeAttempts, 1);
    QCOMPARE(replacementResult, SwarmCommandService::Result::Busy);
    QVERIFY(!attemptedReplacement.isValid());
    QVERIFY(!fixture.service.hasActiveSession());

    QCOMPARE(fixture.service.reserve(
                 &replacementOwner, {member(1, first)}, 10,
                 &attemptedReplacement),
             SwarmCommandService::Result::Reserved);
    QVERIFY(attemptedReplacement.isValid());
}

void SwarmCommandServiceTest::routeCancellationCannotLeakPhysicalFrame()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease lease = fixture.addVehicle(3, 31);
    QObject owner;
    SwarmCommandSessionToken token;
    int routeCalls = 0;
    SwarmCommandService::Result releaseResult =
        SwarmCommandService::Result::InvalidSession;
    fixture.routeHook = [&](const SwarmVehicleInstanceLease &, QString *error) {
        ++routeCalls;
        if (error) {
            error->clear();
        }
        if (routeCalls == 3) {
            releaseResult = fixture.service.release(token);
        }
        return true;
    };
    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, lease)}, 10, &token),
             SwarmCommandService::Result::Reserved);

    SwarmPositionTarget target;
    target.slotId = 1;
    target.latitudeDegrees = 35.0;
    target.longitudeDegrees = 33.0;
    target.relativeAltitudeM = 20.0F;
    const SwarmCommandService::BatchReport report =
        fixture.service.sendPositionTargets(token, {target});

    QCOMPARE(routeCalls, 3);
    QCOMPARE(releaseResult, SwarmCommandService::Result::Cancelled);
    QCOMPARE(report.result,
             SwarmCommandService::Result::RejectedBeforeSend);
    QCOMPARE(fixture.writeAttempts, 0);
    QVERIFY(!fixture.service.hasActiveSession());
}

void SwarmCommandServiceTest::
guidedModeLossDuringRouteValidationCannotLeakPhysicalFrame()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease lease = fixture.addVehicle(3, 31);
    const quint8 misleadingGuidedFlags = static_cast<quint8>(
        MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | MAV_MODE_FLAG_GUIDED_ENABLED);
    QVERIFY(fixture.registry.observeMessage(
        3, fixture.sessions.value(3),
        heartbeat(31, MAV_COMP_ID_AUTOPILOT1, 4, misleadingGuidedFlags)));

    SwarmCommandMember guidedMember = member(1, lease);
    guidedMember.flightMode =
        SwarmCommandMember::FlightModeRequirement::ArduPilotGuided;
    QObject owner;
    SwarmCommandSessionToken token;
    int routeCalls = 0;
    bool modeUpdateAccepted = false;
    fixture.routeHook = [&](const SwarmVehicleInstanceLease &, QString *error) {
        ++routeCalls;
        if (error) {
            error->clear();
        }
        if (routeCalls == 3) {
            // ArduCopter reports GUIDED_ENABLED in LOITER too. The exact
            // custom mode must be rechecked after this injected callback.
            modeUpdateAccepted = fixture.registry.observeMessage(
                3, fixture.sessions.value(3),
                heartbeat(31, MAV_COMP_ID_AUTOPILOT1, 5,
                          misleadingGuidedFlags));
        }
        return true;
    };
    QCOMPARE(fixture.service.reserve(
                 &owner, {guidedMember}, 10, &token),
             SwarmCommandService::Result::Reserved);

    SwarmPositionTarget target;
    target.slotId = 1;
    target.latitudeDegrees = 35.0;
    target.longitudeDegrees = 33.0;
    target.relativeAltitudeM = 20.0F;
    const SwarmCommandService::BatchReport report =
        fixture.service.sendPositionTargets(token, {target});

    QCOMPARE(routeCalls, 3);
    QVERIFY(modeUpdateAccepted);
    QCOMPARE(report.result,
             SwarmCommandService::Result::RejectedBeforeSend);
    QVERIFY(report.detail.contains(QStringLiteral("Loiter")));
    QCOMPARE(fixture.writeAttempts, 0);
}

void SwarmCommandServiceTest::
ownerDestructionAndEndpointRetirementCancelSession()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease lease = fixture.addVehicle(12, 72);
    QVERIFY(lease.isValid());
    QSignalSpy cancelled(
        &fixture.service, &SwarmCommandService::sessionCancelled);

    QObject *owner = new QObject;
    SwarmCommandSessionToken firstToken;
    QCOMPARE(fixture.service.reserve(
                 owner, {member(1, lease)}, 4, &firstToken),
             SwarmCommandService::Result::Reserved);
    const quint64 firstSessionId = firstToken.id;
    delete owner;
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(cancelled.at(0).at(0).toULongLong(), firstSessionId);
    QVERIFY(cancelled.at(0).at(1).toString().contains(
        QStringLiteral("owner"), Qt::CaseInsensitive));
    QVERIFY(!fixture.service.hasActiveSession());

    SwarmPositionTarget target;
    target.slotId = 1;
    target.latitudeDegrees = 35.0;
    target.longitudeDegrees = 33.0;
    target.relativeAltitudeM = 20.0F;
    QCOMPARE(fixture.service.sendPositionTargets(
                 firstToken, {target}).result,
             SwarmCommandService::Result::InvalidSession);
    QCOMPARE(fixture.frames.size(), 0);

    QObject secondOwner;
    SwarmCommandSessionToken secondToken;
    QCOMPARE(fixture.service.reserve(
                 &secondOwner, {member(1, lease)}, 4, &secondToken),
             SwarmCommandService::Result::Reserved);
    QVERIFY(fixture.registry.endLinkSession(
        12, fixture.sessions.value(12)));
    QCOMPARE(cancelled.count(), 2);
    QCOMPARE(cancelled.at(1).at(0).toULongLong(), secondToken.id);
    QVERIFY(cancelled.at(1).at(1).toString().contains(
        QStringLiteral("retired"), Qt::CaseInsensitive));
    QVERIFY(!fixture.service.hasActiveSession());
    QCOMPARE(fixture.service.release(secondToken),
             SwarmCommandService::Result::InvalidSession);
}

void SwarmCommandServiceTest::streamRequestsDecodeExactlyPerPhysicalLink()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease first = fixture.addVehicle(6, 42);
    const SwarmVehicleInstanceLease second = fixture.addVehicle(8, 42);
    QObject owner;
    SwarmCommandSessionToken token;
    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, first), member(2, second)}, 4,
                 &token),
             SwarmCommandService::Result::Reserved);

    const SwarmCommandService::BatchReport report =
        fixture.service.requestPositionAndAttitudeStreams(
            token, {2, 1}, 7);
    QCOMPARE(report.result, SwarmCommandService::Result::SentAll);
    QCOMPARE(report.members.size(), 2);
    QCOMPARE(report.members.at(0).slotId, 1);
    QCOMPARE(report.members.at(0).framesPlanned, 2);
    QCOMPARE(report.members.at(0).framesSent, 2);
    QCOMPARE(report.members.at(1).slotId, 2);
    QCOMPARE(report.members.at(1).framesPlanned, 2);
    QCOMPARE(report.members.at(1).framesSent, 2);
    QCOMPARE(fixture.frames.size(), 4);

    const QVector<int> expectedLinks{6, 6, 8, 8};
    const QVector<int> expectedStreams{
        MAV_DATA_STREAM_POSITION, MAV_DATA_STREAM_EXTRA1,
        MAV_DATA_STREAM_POSITION, MAV_DATA_STREAM_EXTRA1};
    for (int index = 0; index < fixture.frames.size(); ++index) {
        QCOMPARE(fixture.frames.at(index).linkId,
                 expectedLinks.at(index));
        const mavlink_message_t message =
            decodeFrame(fixture.frames.at(index).bytes);
        QCOMPARE(message.msgid,
                 quint32(MAVLINK_MSG_ID_REQUEST_DATA_STREAM));
        QCOMPARE(message.sysid, quint8(255));
        QCOMPARE(message.compid,
                 quint8(MAV_COMP_ID_MISSIONPLANNER));
        mavlink_request_data_stream_t payload{};
        mavlink_msg_request_data_stream_decode(&message, &payload);
        QCOMPARE(payload.target_system, quint8(42));
        QCOMPARE(payload.target_component,
                 quint8(MAV_COMP_ID_AUTOPILOT1));
        QCOMPARE(payload.req_stream_id,
                 quint8(expectedStreams.at(index)));
        QCOMPARE(payload.req_message_rate, quint16(7));
        QCOMPARE(payload.start_stop, quint8(1));
    }

    const mavlink_message_t firstOnFirstLink =
        decodeFrame(fixture.frames.at(0).bytes);
    const mavlink_message_t firstOnSecondLink =
        decodeFrame(fixture.frames.at(2).bytes);
    QCOMPARE(firstOnSecondLink.seq, quint8(0));
    QCOMPARE(firstOnFirstLink.seq, quint8(0));
}

void SwarmCommandServiceTest::positionOnlyStreamRequestsExcludeAttitude()
{
    Fixture fixture;
    const SwarmVehicleInstanceLease leader = fixture.addVehicle(6, 41);
    const SwarmVehicleInstanceLease follower = fixture.addVehicle(8, 42);
    QObject owner;
    SwarmCommandSessionToken token;
    QCOMPARE(fixture.service.reserve(
                 &owner, {member(1, leader), member(2, follower)}, 5,
                 &token),
             SwarmCommandService::Result::Reserved);

    const SwarmCommandService::BatchReport report =
        fixture.service.requestPositionStreams(token, {2, 1}, 5);
    QCOMPARE(report.result, SwarmCommandService::Result::SentAll);
    QCOMPARE(report.members.size(), 2);
    QCOMPARE(report.members.at(0).framesPlanned, 1);
    QCOMPARE(report.members.at(0).framesSent, 1);
    QCOMPARE(report.members.at(1).framesPlanned, 1);
    QCOMPARE(report.members.at(1).framesSent, 1);
    QCOMPARE(fixture.frames.size(), 2);

    const QVector<int> expectedLinks{6, 8};
    for (int index = 0; index < fixture.frames.size(); ++index) {
        QCOMPARE(fixture.frames.at(index).linkId,
                 expectedLinks.at(index));
        const mavlink_message_t message =
            decodeFrame(fixture.frames.at(index).bytes);
        QCOMPARE(message.msgid,
                 quint32(MAVLINK_MSG_ID_REQUEST_DATA_STREAM));
        mavlink_request_data_stream_t payload{};
        mavlink_msg_request_data_stream_decode(&message, &payload);
        QCOMPARE(payload.req_stream_id,
                 quint8(MAV_DATA_STREAM_POSITION));
        QCOMPARE(payload.req_message_rate, quint16(5));
        QCOMPARE(payload.start_stop, quint8(1));
    }
}

QTEST_MAIN(SwarmCommandServiceTest)

#include "test_swarmcommandservice.moc"
