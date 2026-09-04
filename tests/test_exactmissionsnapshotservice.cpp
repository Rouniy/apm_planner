#include "comm/ExactLinkTransmitter.h"
#include "comm/ExactMissionSnapshotService.h"
#include "comm/MAVLinkFrameParser.h"

#include <QHash>
#include <QSignalSpy>
#include <QtTest>

#include <functional>

namespace
{

constexpr quint8 LocalSystemId = 255;
constexpr quint8 LocalComponentId = MAV_COMP_ID_MISSIONPLANNER;

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId, int systemId)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = MAV_COMP_ID_AUTOPILOT1;
    value.linkName = QStringLiteral("Link %1").arg(linkId);
    value.componentName = VehicleEndpoint::defaultComponentName(
        value.componentId);
    return value;
}

mavlink_message_t heartbeat(int systemId)
{
    mavlink_heartbeat_t payload{};
    payload.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
    payload.type = MAV_TYPE_QUADROTOR;
    payload.system_status = MAV_STATE_ACTIVE;
    payload.mavlink_version = 3;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_message_t missionCount(
    int systemId, int count,
    MAV_MISSION_TYPE missionType = MAV_MISSION_TYPE_MISSION)
{
    mavlink_mission_count_t payload{};
    payload.target_system = LocalSystemId;
    payload.target_component = LocalComponentId;
    payload.count = static_cast<quint16>(count);
    payload.mission_type = static_cast<quint8>(missionType);
    mavlink_message_t message{};
    mavlink_msg_mission_count_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &payload);
    return message;
}

mavlink_mission_item_int_t missionItem(
    quint16 sequence, qint32 latitudeE7, qint32 longitudeE7, float altitudeM,
    MAV_MISSION_TYPE missionType = MAV_MISSION_TYPE_MISSION)
{
    mavlink_mission_item_int_t item{};
    item.target_system = LocalSystemId;
    item.target_component = LocalComponentId;
    item.seq = sequence;
    item.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    item.command = MAV_CMD_NAV_WAYPOINT;
    item.current = sequence == 0 ? 1 : 0;
    item.autocontinue = 1;
    item.x = latitudeE7;
    item.y = longitudeE7;
    item.z = altitudeM;
    item.mission_type = static_cast<quint8>(missionType);
    return item;
}

mavlink_message_t missionItemMessage(
    int systemId, const mavlink_mission_item_int_t &item)
{
    mavlink_message_t message{};
    mavlink_msg_mission_item_int_encode(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1,
        &message, &item);
    return message;
}

mavlink_message_t missionAck(
    int systemId, MAV_MISSION_RESULT result,
    MAV_MISSION_TYPE missionType = MAV_MISSION_TYPE_MISSION)
{
    mavlink_mission_ack_t payload{};
    payload.target_system = LocalSystemId;
    payload.target_component = LocalComponentId;
    payload.type = static_cast<quint8>(result);
    payload.mission_type = static_cast<quint8>(missionType);
    mavlink_message_t message{};
    mavlink_msg_mission_ack_encode(
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

class Fixture
{
public:
    explicit Fixture(int timeoutMs = 60000, int maxRetries = 0)
        : registry([this]() { return registryNowMs; })
        , transmitter([this](int linkId, const QByteArray &bytes) {
            ++writeAttempts;
            frames.append({linkId, bytes});
            if (writeHook) {
                writeHook();
            }
            return failWriteAttempt <= 0
                || writeAttempts != failWriteAttempt;
        })
        , service(
              &registry, &transmitter,
              [this](const SwarmVehicleInstanceLease &lease,
                     QString *error) {
                  ++routeCalls;
                  if (routeHook) {
                      return routeHook(lease, error);
                  }
                  if (error) {
                      error->clear();
                  }
                  return true;
              },
              [this](const SwarmVehicleInstanceLease &) {
                  return &coordinator;
              },
              [this]() { return serviceNowMs; },
              timeoutMs, maxRetries)
    {
        service.setLocalIdentity(LocalSystemId, LocalComponentId);
    }

    SwarmVehicleInstanceLease addVehicle(int linkId, int systemId)
    {
        const quint64 session = registry.beginLinkSession(
            linkId, QStringLiteral("Link %1").arg(linkId));
        sessions.insert(linkId, session);
        if (!registry.observeMessage(linkId, session,
                                     heartbeat(systemId))) {
            return {};
        }
        return registry.acquireVehicle(endpoint(linkId, systemId));
    }

    void observe(const SwarmVehicleInstanceLease &vehicle,
                 const mavlink_message_t &message)
    {
        service.observeMessage(vehicle.endpoint.linkId,
                               vehicle.linkSessionEpoch, message);
    }

    qint64 registryNowMs = 100;
    qint64 serviceNowMs = 1000;
    int writeAttempts = 0;
    int failWriteAttempt = 0;
    int routeCalls = 0;
    std::function<void()> writeHook;
    ExactMissionSnapshotService::RouteValidator routeHook;
    QVector<CapturedFrame> frames;
    QHash<int, quint64> sessions;
    SwarmTelemetryRegistry registry;
    ExactLinkTransmitter transmitter;
    MissionProtocolCoordinator coordinator;
    ExactMissionSnapshotService service;
};

bool completeDownload(
    Fixture *fixture, QObject *owner,
    const SwarmVehicleInstanceLease &vehicle,
    const QVector<mavlink_mission_item_int_t> &items,
    ExactMissionTransferToken *token = nullptr)
{
    ExactMissionTransferToken started;
    const auto result = fixture->service.requestDownload(
        owner, vehicle, MAV_MISSION_TYPE_MISSION, &started);
    if (result != ExactMissionSnapshotService::StartResult::Started) {
        return false;
    }
    fixture->observe(
        vehicle, missionCount(vehicle.endpoint.systemId, items.size()));
    for (const mavlink_mission_item_int_t &item : items) {
        fixture->observe(
            vehicle,
            missionItemMessage(vehicle.endpoint.systemId, item));
    }
    if (token) {
        *token = started;
    }
    return !fixture->service.busy();
}

QVector<mavlink_mission_item_int_t> sampleMission()
{
    return {
        missionItem(0, 351000000, 331000000, 25.5F),
        missionItem(1, 351000100, 331000100, 40.0F)};
}

} // namespace

class ExactMissionSnapshotServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void routesAndCachesByExactInstance();
    void partialOrRejectedRefreshPreservesLastSnapshot();
    void contentGenerationAndMp10SignatureAreDeterministic();
    void endpointRetirementInvalidatesAndCancels();
    void routeRetirementDuringCallbackCannotLeakFrame();
    void ownerDestroyedDuringRouteValidationIsRejected();
    void recursiveStartDuringRouteValidationIsBusy();
    void ownerAndTokenCancellationAreBounded();
    void coordinatorContentionAndMutationInvalidateCache();
    void mavlinkOneRejectsExtensionMissionTypesAtEveryBarrier();
    void zeroItemMissionPublishesAtomically();
    void timeoutDoesNotReplaceLastCompleteSnapshot();
    void completeSnapshotSurvivesTerminalAckWriteFailure();
    void terminalOwnerDestructionAndBusyReentrancyPreserveOrdering();
};

void ExactMissionSnapshotServiceTest::routesAndCachesByExactInstance()
{
    Fixture fixture;
    QObject owner;
    const auto first = fixture.addVehicle(11, 42);
    const auto sameIdsDifferentLink = fixture.addVehicle(22, 42);
    QVERIFY(first.isValid());
    QVERIFY(sameIdsDifferentLink.isValid());
    QVERIFY(!first.sameInstance(sameIdsDifferentLink));

    ExactMissionTransferToken token;
    // A process-global channel left in MAVLink 1 mode must not corrupt this
    // exact link's mission_type extension during a generated first frame.
    mavlink_status_t *const globalStatus =
        mavlink_get_channel_status(MAVLINK_COMM_0);
    const quint8 previousGlobalFlags = globalStatus->flags;
    globalStatus->flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    const auto startResult = fixture.service.requestDownload(
        &owner, first, MAV_MISSION_TYPE_MISSION, &token);
    globalStatus->flags = previousGlobalFlags;
    QCOMPARE(
        static_cast<int>(startResult),
        static_cast<int>(
            ExactMissionSnapshotService::StartResult::Started));
    QVERIFY(token.isValid());
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(fixture.frames.first().linkId, first.endpoint.linkId);

    // Same sysid/compid from another physical link cannot advance this lease.
    fixture.service.observeMessage(
        sameIdsDifferentLink.endpoint.linkId,
        sameIdsDifferentLink.linkSessionEpoch,
        missionCount(42, 2));
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(fixture.service.busy());

    // Nor can the right link id combined with the wrong captured session.
    fixture.service.observeMessage(
        first.endpoint.linkId,
        sameIdsDifferentLink.linkSessionEpoch,
        missionCount(42, 2));
    QCOMPARE(fixture.frames.size(), 1);

    fixture.observe(first, missionCount(42, 2));
    fixture.observe(first, missionItemMessage(42, sampleMission().at(0)));
    fixture.observe(first, missionItemMessage(42, sampleMission().at(1)));
    QVERIFY(!fixture.service.busy());
    QCOMPARE(fixture.frames.size(), 4);

    ExactMissionSnapshot snapshot;
    ExactMissionSnapshotLease snapshotLease;
    QVERIFY(fixture.service.acquireSnapshot(
        first, MAV_MISSION_TYPE_MISSION, &snapshot, &snapshotLease));
    QCOMPARE(snapshot.items.size(), 2);
    QVERIFY(snapshot.key.vehicle.sameInstance(first));
    QVERIFY(snapshotLease.isValid());
    QVERIFY(fixture.service.validateSnapshotLease(snapshotLease));

    ExactMissionSnapshot absent;
    QVERIFY(!fixture.service.acquireSnapshot(
        sameIdsDifferentLink, MAV_MISSION_TYPE_MISSION, &absent));

    const mavlink_message_t outbound = decodeFrame(
        fixture.frames.first().bytes);
    QCOMPARE(outbound.magic, quint8(MAVLINK_STX));
    QCOMPARE(outbound.msgid, quint32(MAVLINK_MSG_ID_MISSION_REQUEST_LIST));
    mavlink_mission_request_list_t request{};
    mavlink_msg_mission_request_list_decode(&outbound, &request);
    QCOMPARE(request.target_system, quint8(42));
    QCOMPARE(request.target_component, quint8(MAV_COMP_ID_AUTOPILOT1));
    QCOMPARE(request.mission_type, quint8(MAV_MISSION_TYPE_MISSION));
}

void ExactMissionSnapshotServiceTest::partialOrRejectedRefreshPreservesLastSnapshot()
{
    Fixture fixture;
    QObject owner;
    const auto vehicle = fixture.addVehicle(7, 51);
    const auto mission = sampleMission();
    QVERIFY(completeDownload(&fixture, &owner, vehicle, mission));

    ExactMissionSnapshot before;
    ExactMissionSnapshotLease lease;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &before, &lease));
    QSignalSpy replaced(&fixture.service,
                        &ExactMissionSnapshotService::snapshotReplaced);

    ExactMissionTransferToken refresh;
    QCOMPARE(
        static_cast<int>(fixture.service.requestDownload(
            &owner, vehicle, MAV_MISSION_TYPE_MISSION, &refresh)),
        static_cast<int>(
            ExactMissionSnapshotService::StartResult::Started));
    fixture.observe(vehicle, missionCount(51, 2));
    fixture.observe(vehicle, missionItemMessage(51, mission.at(0)));
    QVERIFY(fixture.service.busy());

    // A partial candidate is private transaction state, not a cache update.
    ExactMissionSnapshot during;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &during));
    QCOMPARE(during.contentDigest, before.contentDigest);
    QCOMPARE(during.observationRevision, before.observationRevision);
    QVERIFY(fixture.service.validateSnapshotLease(lease));

    fixture.observe(vehicle, missionAck(51, MAV_MISSION_DENIED));
    QVERIFY(!fixture.service.busy());
    QCOMPARE(replaced.count(), 0);

    ExactMissionSnapshot after;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &after));
    QCOMPARE(after.contentGeneration, before.contentGeneration);
    QCOMPARE(after.observationRevision, before.observationRevision);
    QCOMPARE(after.contentDigest, before.contentDigest);
    QVERIFY(fixture.service.validateSnapshotLease(lease));
    QVERIFY(!fixture.service.cancel(refresh));
}

void ExactMissionSnapshotServiceTest::contentGenerationAndMp10SignatureAreDeterministic()
{
    Fixture fixture;
    QObject owner;
    const auto vehicle = fixture.addVehicle(3, 73);
    const auto mission = sampleMission();
    QVERIFY(completeDownload(&fixture, &owner, vehicle, mission));

    ExactMissionSnapshot first;
    ExactMissionSnapshotLease firstLease;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &first, &firstLease));
    QCOMPARE(
        first.waypointLeaderSignature,
        QByteArray("7821D438886B0DA2739DAAD6A2BE36FCF818C1D5C15CAB0DDE83D4F7402A591A"));
    QCOMPARE(first.items.first().seq, quint16(0));
    QCOMPARE(first.items.first().x, qint32(351000000));

    ++fixture.serviceNowMs;
    QVERIFY(completeDownload(&fixture, &owner, vehicle, mission));
    ExactMissionSnapshot identical;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &identical));
    QCOMPARE(identical.contentGeneration, first.contentGeneration);
    QVERIFY(identical.observationRevision > first.observationRevision);
    QCOMPARE(identical.contentDigest, first.contentDigest);
    QVERIFY(fixture.service.validateSnapshotLease(firstLease));

    auto behaviorChange = mission;
    behaviorChange[1].param1 = 7.5F;
    QVERIFY(completeDownload(&fixture, &owner, vehicle, behaviorChange));
    ExactMissionSnapshot changed;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &changed));
    QVERIFY(changed.contentGeneration > identical.contentGeneration);
    QVERIFY(changed.contentDigest != identical.contentDigest);
    // MP10's path signature deliberately covers path geometry only.
    QCOMPARE(changed.waypointLeaderSignature,
             identical.waypointLeaderSignature);
    QVERIFY(!fixture.service.validateSnapshotLease(firstLease));

    auto transportMetadataOnly = behaviorChange;
    transportMetadataOnly[0].target_system = 1;
    transportMetadataOnly[0].target_component = 99;
    QCOMPARE(
        ExactMissionSnapshotService::contentDigestFor(transportMetadataOnly),
        ExactMissionSnapshotService::contentDigestFor(behaviorChange));
}

void ExactMissionSnapshotServiceTest::endpointRetirementInvalidatesAndCancels()
{
    Fixture fixture;
    QObject owner;
    const auto vehicle = fixture.addVehicle(18, 42);
    QVERIFY(completeDownload(
        &fixture, &owner, vehicle, sampleMission()));

    QSignalSpy invalidated(
        &fixture.service,
        &ExactMissionSnapshotService::snapshotInvalidated);
    QSignalSpy finished(
        &fixture.service,
        &ExactMissionSnapshotService::transferFinished);
    ExactMissionTransferToken refresh;
    QCOMPARE(
        static_cast<int>(fixture.service.requestDownload(
            &owner, vehicle, MAV_MISSION_TYPE_MISSION, &refresh)),
        static_cast<int>(
            ExactMissionSnapshotService::StartResult::Started));
    fixture.observe(vehicle, missionCount(42, 2));
    fixture.observe(
        vehicle, missionItemMessage(42, sampleMission().first()));

    QVERIFY(fixture.registry.endLinkSession(
        vehicle.endpoint.linkId, vehicle.linkSessionEpoch));
    QVERIFY(!fixture.service.busy());
    QCOMPARE(invalidated.count(), 1);
    QCOMPARE(finished.count(), 1);
    const auto result = qvariant_cast<ExactMissionTransferResult>(
        finished.first().at(0));
    QCOMPARE(static_cast<int>(result.state),
             static_cast<int>(MissionTransferService::State::Error));
    QVERIFY(!result.snapshot.isValid());

    ExactMissionSnapshot absent;
    QVERIFY(!fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &absent));
    QVERIFY(!fixture.service.cancel(refresh));

    const auto replacement = fixture.addVehicle(18, 42);
    QVERIFY(replacement.isValid());
    QVERIFY(!replacement.sameInstance(vehicle));
    QVERIFY(!fixture.service.acquireSnapshot(
        replacement, MAV_MISSION_TYPE_MISSION, &absent));
}

void ExactMissionSnapshotServiceTest::routeRetirementDuringCallbackCannotLeakFrame()
{
    Fixture fixture;
    QObject owner;
    const auto vehicle = fixture.addVehicle(15, 64);
    QSignalSpy finished(
        &fixture.service,
        &ExactMissionSnapshotService::transferFinished);

    fixture.routeHook = [&fixture](
        const SwarmVehicleInstanceLease &lease, QString *error) {
        if (error) {
            error->clear();
        }
        // requestDownload preflights once. The second callback is the
        // immediate pre-send barrier after the owner/token became active.
        if (fixture.routeCalls == 2) {
            fixture.registry.endLinkSession(
                lease.endpoint.linkId, lease.linkSessionEpoch);
        }
        return true;
    };

    ExactMissionTransferToken token;
    QCOMPARE(
        static_cast<int>(fixture.service.requestDownload(
            &owner, vehicle, MAV_MISSION_TYPE_MISSION, &token)),
        static_cast<int>(
            ExactMissionSnapshotService::StartResult::Started));
    QCOMPARE(fixture.routeCalls, 2);
    QCOMPARE(fixture.frames.size(), 0);
    QVERIFY(!fixture.service.busy());
    QCOMPARE(finished.count(), 1);
    QVERIFY(!fixture.service.cancel(token));
}

void ExactMissionSnapshotServiceTest::ownerDestroyedDuringRouteValidationIsRejected()
{
    Fixture fixture;
    auto *owner = new QObject;
    const auto vehicle = fixture.addVehicle(16, 65);
    QSignalSpy finished(
        &fixture.service,
        &ExactMissionSnapshotService::transferFinished);
    fixture.routeHook = [&owner](
        const SwarmVehicleInstanceLease &, QString *error) {
        if (error) {
            error->clear();
        }
        delete owner;
        owner = nullptr;
        return true;
    };

    ExactMissionTransferToken token;
    QString error;
    QCOMPARE(
        static_cast<int>(fixture.service.requestDownload(
            owner, vehicle, MAV_MISSION_TYPE_MISSION, &token, &error)),
        static_cast<int>(
            ExactMissionSnapshotService::StartResult::InvalidOwner));
    QVERIFY(owner == nullptr);
    QVERIFY(!token.isValid());
    QVERIFY(!fixture.service.busy());
    QCOMPARE(fixture.frames.size(), 0);
    QCOMPARE(finished.count(), 0);
    QVERIFY(error.contains(QStringLiteral("owner"), Qt::CaseInsensitive));
}

void ExactMissionSnapshotServiceTest::recursiveStartDuringRouteValidationIsBusy()
{
    Fixture fixture;
    QObject outerOwner;
    QObject nestedOwner;
    const auto vehicle = fixture.addVehicle(17, 66);
    ExactMissionSnapshotService::StartResult nestedResult =
        ExactMissionSnapshotService::StartResult::Started;
    ExactMissionTransferToken nestedToken;
    bool attemptedNested = false;
    fixture.routeHook = [&fixture, &nestedOwner, &vehicle,
                         &nestedResult, &nestedToken, &attemptedNested](
        const SwarmVehicleInstanceLease &, QString *error) {
        if (error) {
            error->clear();
        }
        if (!attemptedNested) {
            attemptedNested = true;
            nestedResult = fixture.service.requestDownload(
                &nestedOwner, vehicle, MAV_MISSION_TYPE_MISSION,
                &nestedToken);
        }
        return true;
    };

    ExactMissionTransferToken outerToken;
    QCOMPARE(
        static_cast<int>(fixture.service.requestDownload(
            &outerOwner, vehicle, MAV_MISSION_TYPE_MISSION, &outerToken)),
        static_cast<int>(
            ExactMissionSnapshotService::StartResult::Started));
    QVERIFY(attemptedNested);
    QCOMPARE(static_cast<int>(nestedResult),
             static_cast<int>(
                 ExactMissionSnapshotService::StartResult::Busy));
    QVERIFY(!nestedToken.isValid());
    QVERIFY(outerToken.isValid());
    QVERIFY(fixture.service.busy());
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(fixture.service.cancel(outerToken));
}

void ExactMissionSnapshotServiceTest::ownerAndTokenCancellationAreBounded()
{
    Fixture fixture;
    auto *owner = new QObject;
    const auto vehicle = fixture.addVehicle(31, 80);
    QSignalSpy finished(
        &fixture.service,
        &ExactMissionSnapshotService::transferFinished);

    ExactMissionTransferToken token;
    QCOMPARE(
        static_cast<int>(fixture.service.requestDownload(
            owner, vehicle, MAV_MISSION_TYPE_MISSION, &token)),
        static_cast<int>(
            ExactMissionSnapshotService::StartResult::Started));
    QCOMPARE(fixture.frames.size(), 1);
    delete owner;

    QVERIFY(!fixture.service.busy());
    QCOMPARE(finished.count(), 1);
    QVERIFY(!fixture.service.cancel(token));
    QCOMPARE(fixture.frames.size(), 2);
    const mavlink_message_t cancelled = decodeFrame(
        fixture.frames.last().bytes);
    QCOMPARE(cancelled.msgid, quint32(MAVLINK_MSG_ID_MISSION_ACK));
    mavlink_mission_ack_t acknowledgement{};
    mavlink_msg_mission_ack_decode(&cancelled, &acknowledgement);
    QCOMPARE(acknowledgement.type,
             quint8(MAV_MISSION_OPERATION_CANCELLED));

    const auto result = qvariant_cast<ExactMissionTransferResult>(
        finished.first().at(0));
    QCOMPARE(static_cast<int>(result.state),
             static_cast<int>(MissionTransferService::State::Cancelled));
    QVERIFY(!result.snapshot.isValid());
}

void ExactMissionSnapshotServiceTest::
coordinatorContentionAndMutationInvalidateCache()
{
    Fixture fixture;
    QObject exactOwner;
    QObject legacyOwner;
    const auto vehicle = fixture.addVehicle(23, 88);

    const auto legacyLease = fixture.coordinator.tryAcquire(
        &legacyOwner, MissionProtocolCoordinator::MissionType::Mission);
    QVERIFY(legacyLease.isValid());
    ExactMissionTransferToken rejected;
    QCOMPARE(
        fixture.service.requestDownload(
            &exactOwner, vehicle, MAV_MISSION_TYPE_MISSION, &rejected),
        ExactMissionSnapshotService::StartResult::MissionOwnerBusy);
    QVERIFY(!rejected.isValid());
    QCOMPARE(fixture.frames.size(), 0);
    QVERIFY(fixture.coordinator.release(legacyLease));

    QVERIFY(completeDownload(
        &fixture, &exactOwner, vehicle, sampleMission()));
    ExactMissionSnapshot cached;
    ExactMissionSnapshotLease cacheLease;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &cached, &cacheLease));
    QVERIFY(fixture.service.validateSnapshotLease(cacheLease));

    QSignalSpy invalidated(
        &fixture.service,
        &ExactMissionSnapshotService::snapshotInvalidated);
    const auto uploadLease = fixture.coordinator.tryAcquire(
        &legacyOwner, MissionProtocolCoordinator::MissionType::Mission);
    QVERIFY(uploadLease.isValid());
    QCOMPARE(invalidated.count(), 1);
    QVERIFY(!fixture.service.validateSnapshotLease(cacheLease));
    QVERIFY(!fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &cached));
    QVERIFY(fixture.coordinator.release(uploadLease));

    QVERIFY(completeDownload(
        &fixture, &exactOwner, vehicle, sampleMission()));
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &cached, &cacheLease));
    fixture.observe(vehicle, missionAck(88, MAV_MISSION_ACCEPTED));
    QCOMPARE(invalidated.count(), 2);
    QVERIFY(!fixture.service.validateSnapshotLease(cacheLease));
}

void ExactMissionSnapshotServiceTest::
mavlinkOneRejectsExtensionMissionTypesAtEveryBarrier()
{
    {
        Fixture fixture;
        QObject owner;
        const auto vehicle = fixture.addVehicle(24, 89);
        fixture.transmitter.setOutboundVersion(vehicle.endpoint.linkId, 1U);
        ExactMissionTransferToken token;
        QString error;
        QCOMPARE(
            fixture.service.requestDownload(
                &owner, vehicle, MAV_MISSION_TYPE_FENCE, &token, &error),
            ExactMissionSnapshotService::StartResult::
                IncompatibleProtocolVersion);
        QVERIFY(!token.isValid());
        QCOMPARE(fixture.frames.size(), 0);
        QVERIFY(error.contains(QStringLiteral("MAVLink 2")));
    }
    {
        Fixture fixture;
        QObject owner;
        const auto vehicle = fixture.addVehicle(25, 90);
        fixture.routeHook = [&fixture](
            const SwarmVehicleInstanceLease &lease, QString *error) {
            if (error) {
                error->clear();
            }
            fixture.transmitter.setOutboundVersion(
                lease.endpoint.linkId, 1U);
            return true;
        };
        ExactMissionTransferToken token;
        QCOMPARE(
            fixture.service.requestDownload(
                &owner, vehicle, MAV_MISSION_TYPE_RALLY, &token),
            ExactMissionSnapshotService::StartResult::
                IncompatibleProtocolVersion);
        QVERIFY(!token.isValid());
        QCOMPARE(fixture.frames.size(), 0);
        QVERIFY(fixture.coordinator.owner() == nullptr);
    }
    {
        Fixture fixture;
        QObject owner;
        const auto vehicle = fixture.addVehicle(27, 92);
        ExactMissionTransferToken token;
        QSignalSpy finished(
            &fixture.service,
            &ExactMissionSnapshotService::transferFinished);
        QCOMPARE(
            fixture.service.requestDownload(
                &owner, vehicle, MAV_MISSION_TYPE_FENCE, &token),
            ExactMissionSnapshotService::StartResult::Started);
        QVERIFY(token.isValid());
        QCOMPARE(fixture.frames.size(), 1);
        QVERIFY(fixture.service.busy());

        // A live link may renegotiate/downgrade after the request-list frame.
        // No extension-bearing follow-up may escape after that transition.
        fixture.transmitter.setOutboundVersion(vehicle.endpoint.linkId, 1U);
        fixture.observe(
            vehicle,
            missionCount(vehicle.endpoint.systemId, 1,
                         MAV_MISSION_TYPE_FENCE));
        QVERIFY(!fixture.service.busy());
        QCOMPARE(fixture.frames.size(), 1);
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<ExactMissionTransferResult>(
            finished.first().at(0));
        QCOMPARE(result.state, MissionTransferService::State::Error);
        QVERIFY(result.errorString.contains(QStringLiteral("MAVLink 2")));
    }
}

void ExactMissionSnapshotServiceTest::zeroItemMissionPublishesAtomically()
{
    Fixture fixture;
    QObject owner;
    const auto vehicle = fixture.addVehicle(26, 91);
    QVERIFY(completeDownload(&fixture, &owner, vehicle, {}));

    ExactMissionSnapshot snapshot;
    ExactMissionSnapshotLease lease;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &snapshot, &lease));
    QVERIFY(snapshot.items.isEmpty());
    QVERIFY(snapshot.isValid());
    QVERIFY(lease.isValid());
    QVERIFY(fixture.service.validateSnapshotLease(lease));
}

void ExactMissionSnapshotServiceTest::timeoutDoesNotReplaceLastCompleteSnapshot()
{
    Fixture fixture(5, 0);
    QObject owner;
    const auto vehicle = fixture.addVehicle(9, 90);
    const auto mission = sampleMission();
    QVERIFY(completeDownload(&fixture, &owner, vehicle, mission));
    ExactMissionSnapshot before;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &before));

    QSignalSpy replaced(&fixture.service,
                        &ExactMissionSnapshotService::snapshotReplaced);
    QSignalSpy finished(&fixture.service,
                        &ExactMissionSnapshotService::transferFinished);
    ExactMissionTransferToken token;
    QCOMPARE(
        static_cast<int>(fixture.service.requestDownload(
            &owner, vehicle, MAV_MISSION_TYPE_MISSION, &token)),
        static_cast<int>(
            ExactMissionSnapshotService::StartResult::Started));
    fixture.observe(vehicle, missionCount(90, 2));
    fixture.observe(vehicle, missionItemMessage(90, mission.first()));
    QVERIFY(fixture.service.busy());

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    QVERIFY(!fixture.service.busy());
    QCOMPARE(replaced.count(), 0);
    ExactMissionSnapshot after;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &after));
    QCOMPARE(after.contentGeneration, before.contentGeneration);
    QCOMPARE(after.observationRevision, before.observationRevision);
    QCOMPARE(after.contentDigest, before.contentDigest);
    QVERIFY(!fixture.service.cancel(token));
}

void ExactMissionSnapshotServiceTest::completeSnapshotSurvivesTerminalAckWriteFailure()
{
    Fixture fixture;
    QObject owner;
    const auto vehicle = fixture.addVehicle(40, 99);
    // request-list, request item 0, request item 1, terminal ACK.
    fixture.failWriteAttempt = 4;
    QSignalSpy finished(&fixture.service,
                        &ExactMissionSnapshotService::transferFinished);

    QVERIFY(completeDownload(
        &fixture, &owner, vehicle, sampleMission()));
    QCOMPARE(fixture.writeAttempts, 4);
    QCOMPARE(finished.count(), 1);
    const auto result = qvariant_cast<ExactMissionTransferResult>(
        finished.first().at(0));
    QVERIFY(result.succeeded());
    QVERIFY(result.snapshot.isValid());

    ExactMissionSnapshot cached;
    QVERIFY(fixture.service.acquireSnapshot(
        vehicle, MAV_MISSION_TYPE_MISSION, &cached));
    QCOMPARE(cached.contentDigest, result.snapshot.contentDigest);
    QCOMPARE(cached.items.size(), 2);
}

void ExactMissionSnapshotServiceTest::
terminalOwnerDestructionAndBusyReentrancyPreserveOrdering()
{
    {
        Fixture fixture;
        auto *owner = new QObject;
        const auto vehicle = fixture.addVehicle(41, 100);
        fixture.writeHook = [&fixture, &owner]() {
            if (fixture.writeAttempts == 4 && owner) {
                delete owner;
                owner = nullptr;
            }
        };
        QSignalSpy finished(
            &fixture.service,
            &ExactMissionSnapshotService::transferFinished);

        QVERIFY(completeDownload(
            &fixture, owner, vehicle, sampleMission()));
        QVERIFY(owner == nullptr);
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<ExactMissionTransferResult>(
            finished.first().at(0));
        QVERIFY(result.succeeded());
        ExactMissionSnapshot cached;
        QVERIFY(fixture.service.acquireSnapshot(
            vehicle, MAV_MISSION_TYPE_MISSION, &cached));
    }

    {
        Fixture fixture;
        QObject owner;
        const auto vehicle = fixture.addVehicle(42, 101);
        QStringList events;
        connect(&fixture.service,
                &ExactMissionSnapshotService::snapshotReplaced,
                &fixture.service,
                [&events](const ExactMissionSnapshot &) {
                    events.append(QStringLiteral("replaced"));
                });
        connect(&fixture.service,
                &ExactMissionSnapshotService::snapshotInvalidated,
                &fixture.service,
                [&events](const ExactMissionKey &,
                          ExactMissionSnapshotService::InvalidationReason) {
                    events.append(QStringLiteral("invalidated"));
                });
        connect(&fixture.service,
                &ExactMissionSnapshotService::busyChanged,
                &fixture.service,
                [&fixture, vehicle](bool busy) {
                    if (!busy) {
                        fixture.registry.endLinkSession(
                            vehicle.endpoint.linkId,
                            vehicle.linkSessionEpoch);
                    }
                });

        QVERIFY(completeDownload(
            &fixture, &owner, vehicle, sampleMission()));
        QCOMPARE(events,
                 QStringList({QStringLiteral("replaced"),
                              QStringLiteral("invalidated")}));
        ExactMissionSnapshot absent;
        QVERIFY(!fixture.service.acquireSnapshot(
            vehicle, MAV_MISSION_TYPE_MISSION, &absent));
    }
}

QTEST_MAIN(ExactMissionSnapshotServiceTest)

#include "test_exactmissionsnapshotservice.moc"
