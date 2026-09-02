#include "comm/MissionTransferController.h"

#include <QtTest/QTest>

#include <QStringList>

namespace
{
constexpr quint8 LocalSystemId = 250;
constexpr quint8 LocalComponentId = 190;
constexpr quint8 RemoteSystemId = 42;
constexpr quint8 RemoteComponentId = 1;

class FakeTransport final : public MissionTransferTransport
{
public:
    explicit FakeTransport(QObject *parent = nullptr)
        : MissionTransferTransport(parent)
    {
    }

    quint8 localSystemId() const override { return currentLocalSystemId; }
    quint8 localComponentId() const override { return currentLocalComponentId; }

    MissionTransferService::Key missionKey(
            MAV_MISSION_TYPE missionType) const override
    {
        return {RemoteSystemId, RemoteComponentId, missionType};
    }

    bool beginOperation() override
    {
        ++beginCount;
        events.append(QStringLiteral("begin"));
        if (!beginSucceeds)
            return false;
        operationActive = true;
        return true;
    }

    void endOperation() override
    {
        ++endCount;
        operationActive = false;
        events.append(QStringLiteral("end"));
    }

    bool sendMessage(const mavlink_message_t &message) override
    {
        attemptedMessages.append(message);
        events.append(sendSucceeds ? QStringLiteral("send")
                                   : QStringLiteral("send-failed"));
        if (sendSucceeds)
            sentMessages.append(message);
        return sendSucceeds;
    }

    void inject(const mavlink_message_t &message)
    {
        emit messageReceived(message);
    }

    void loseLink()
    {
        emit unavailable();
    }

    bool beginSucceeds = true;
    bool sendSucceeds = true;
    bool operationActive = false;
    quint8 currentLocalSystemId = LocalSystemId;
    quint8 currentLocalComponentId = LocalComponentId;
    int beginCount = 0;
    int endCount = 0;
    QVector<mavlink_message_t> attemptedMessages;
    QVector<mavlink_message_t> sentMessages;
    QStringList events;
};

mavlink_message_t missionCount(
        quint16 count,
        MAV_MISSION_TYPE type = MAV_MISSION_TYPE_MISSION,
        quint8 sourceSystem = RemoteSystemId,
        quint8 sourceComponent = RemoteComponentId,
        quint8 targetSystem = LocalSystemId,
        quint8 targetComponent = LocalComponentId)
{
    mavlink_mission_count_t payload{};
    payload.count = count;
    payload.target_system = targetSystem;
    payload.target_component = targetComponent;
    payload.mission_type = static_cast<quint8>(type);
    mavlink_message_t message{};
    mavlink_msg_mission_count_encode(
            sourceSystem, sourceComponent, &message, &payload);
    return message;
}

mavlink_message_t missionItemInt(
        quint16 sequence,
        MAV_MISSION_TYPE type = MAV_MISSION_TYPE_MISSION)
{
    mavlink_mission_item_int_t payload{};
    payload.seq = sequence;
    payload.command = MAV_CMD_NAV_WAYPOINT;
    payload.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    payload.x = -353632610 + sequence;
    payload.y = 1491652300 + sequence;
    payload.z = 50.0f + sequence;
    payload.target_system = LocalSystemId;
    payload.target_component = LocalComponentId;
    payload.mission_type = static_cast<quint8>(type);
    mavlink_message_t message{};
    mavlink_msg_mission_item_int_encode(
            RemoteSystemId, RemoteComponentId, &message, &payload);
    return message;
}

mavlink_message_t missionItemFloat(
        quint16 sequence,
        MAV_MISSION_TYPE type = MAV_MISSION_TYPE_MISSION)
{
    mavlink_mission_item_t payload{};
    payload.seq = sequence;
    payload.command = MAV_CMD_NAV_WAYPOINT;
    payload.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    payload.x = -35.363261f + sequence * 0.000001f;
    payload.y = 149.165230f + sequence * 0.000001f;
    payload.z = 60.0f + sequence;
    payload.target_system = LocalSystemId;
    payload.target_component = LocalComponentId;
    payload.mission_type = static_cast<quint8>(type);
    mavlink_message_t message{};
    mavlink_msg_mission_item_encode(
            RemoteSystemId, RemoteComponentId, &message, &payload);
    return message;
}

mavlink_message_t missionRequest(
        quint16 sequence, bool useInt,
        MAV_MISSION_TYPE type = MAV_MISSION_TYPE_MISSION)
{
    mavlink_message_t message{};
    if (useInt) {
        mavlink_mission_request_int_t payload{};
        payload.seq = sequence;
        payload.target_system = LocalSystemId;
        payload.target_component = LocalComponentId;
        payload.mission_type = static_cast<quint8>(type);
        mavlink_msg_mission_request_int_encode(
                RemoteSystemId, RemoteComponentId, &message, &payload);
    } else {
        mavlink_mission_request_t payload{};
        payload.seq = sequence;
        payload.target_system = LocalSystemId;
        payload.target_component = LocalComponentId;
        payload.mission_type = static_cast<quint8>(type);
        mavlink_msg_mission_request_encode(
                RemoteSystemId, RemoteComponentId, &message, &payload);
    }
    return message;
}

mavlink_message_t missionAck(
        MAV_MISSION_RESULT result,
        MAV_MISSION_TYPE type = MAV_MISSION_TYPE_MISSION)
{
    mavlink_mission_ack_t payload{};
    payload.target_system = LocalSystemId;
    payload.target_component = LocalComponentId;
    payload.type = static_cast<quint8>(result);
    payload.mission_type = static_cast<quint8>(type);
    mavlink_message_t message{};
    mavlink_msg_mission_ack_encode(
            RemoteSystemId, RemoteComponentId, &message, &payload);
    return message;
}

mavlink_mission_item_int_t uploadItem(quint16 sequence)
{
    mavlink_mission_item_int_t item{};
    item.seq = sequence;
    item.command = MAV_CMD_NAV_WAYPOINT;
    item.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    item.x = -353632610 + sequence;
    item.y = 1491652300 + sequence;
    item.z = 70.0f + sequence;
    item.mission_type = MAV_MISSION_TYPE_MISSION;
    return item;
}
}

class MissionTransferControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void downloadsMissionIntAndCleansTerminalState();
    void supportsFloatFallbackAndAllUploadEncodings();
    void rejectsLeaseCollisionAndFiltersForeignPackets();
    void refreshesLocalIdentityBetweenOperations();
    void preservesCompletedDownloadWhenTerminalAckFails();
    void retriesCancelsAndHandlesLinkLossOnce();
    void sendAndLifetimeFailuresReleaseEverything();
};

void MissionTransferControllerTest::downloadsMissionIntAndCleansTerminalState()
{
    MissionProtocolCoordinator coordinator;
    FakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 2);
    int finishedCount = 0;
    MissionTransferResult result;
    connect(&controller, &MissionTransferController::transferFinished,
            this, [&](const MissionTransferResult &terminal) {
        ++finishedCount;
        result = terminal;
    });

    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_FENCE));
    QVERIFY(controller.busy());
    QCOMPARE(controller.transferId(), quint64(1));
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(&controller));
    QVERIFY(transport.operationActive);
    QCOMPARE(transport.attemptedMessages.size(), 1);
    QCOMPARE(quint32(transport.attemptedMessages.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_REQUEST_LIST));
    mavlink_mission_request_list_t list{};
    mavlink_msg_mission_request_list_decode(
            &transport.attemptedMessages.last(), &list);
    QCOMPARE(list.target_system, RemoteSystemId);
    QCOMPARE(list.target_component, RemoteComponentId);
    QCOMPARE(list.mission_type, quint8(MAV_MISSION_TYPE_FENCE));
    QCOMPARE(transport.attemptedMessages.last().sysid, LocalSystemId);
    QCOMPARE(transport.attemptedMessages.last().compid, LocalComponentId);

    transport.inject(missionCount(2, MAV_MISSION_TYPE_FENCE));
    QCOMPARE(quint32(transport.attemptedMessages.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_REQUEST_INT));
    mavlink_mission_request_int_t request{};
    mavlink_msg_mission_request_int_decode(
            &transport.attemptedMessages.last(), &request);
    QCOMPARE(request.seq, quint16(0));

    transport.inject(missionItemInt(0, MAV_MISSION_TYPE_FENCE));
    QCOMPARE(controller.progress(), 50);
    mavlink_msg_mission_request_int_decode(
            &transport.attemptedMessages.last(), &request);
    QCOMPARE(request.seq, quint16(1));

    transport.events.clear();
    transport.inject(missionItemInt(1, MAV_MISSION_TYPE_FENCE));
    QCOMPARE(quint32(transport.attemptedMessages.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_ACK));
    mavlink_mission_ack_t ack{};
    mavlink_msg_mission_ack_decode(&transport.attemptedMessages.last(), &ack);
    QCOMPARE(ack.type, quint8(MAV_MISSION_ACCEPTED));
    QCOMPARE(transport.events,
             QStringList({QStringLiteral("send"), QStringLiteral("end")}));
    QCOMPARE(finishedCount, 1);
    QVERIFY(result.succeeded());
    QCOMPARE(result.transferId, quint64(1));
    QCOMPARE(result.missionType, MAV_MISSION_TYPE_FENCE);
    QCOMPARE(result.downloadedItems.size(), 2);
    QCOMPARE(controller.progress(), 100);
    QVERIFY(!controller.busy());
    QVERIFY(!transport.operationActive);
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(nullptr));

    const int sentAtCompletion = transport.attemptedMessages.size();
    transport.inject(missionCount(1, MAV_MISSION_TYPE_FENCE));
    QVERIFY(QMetaObject::invokeMethod(
            &controller, "timeout", Qt::DirectConnection));
    QCOMPARE(transport.attemptedMessages.size(), sentAtCompletion);
    QCOMPARE(finishedCount, 1);
}

void MissionTransferControllerTest::supportsFloatFallbackAndAllUploadEncodings()
{
    MissionProtocolCoordinator coordinator;
    FakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 2);
    int finishedCount = 0;
    MissionTransferResult result;
    connect(&controller, &MissionTransferController::transferFinished,
            this, [&](const MissionTransferResult &terminal) {
        ++finishedCount;
        result = terminal;
    });

    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_MISSION));
    transport.inject(missionCount(2));
    transport.inject(missionItemFloat(0));
    QCOMPARE(quint32(transport.attemptedMessages.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_REQUEST));
    mavlink_mission_request_t floatRequest{};
    mavlink_msg_mission_request_decode(
            &transport.attemptedMessages.last(), &floatRequest);
    QCOMPARE(floatRequest.seq, quint16(1));
    transport.inject(missionItemFloat(1));
    QCOMPARE(finishedCount, 1);
    QCOMPARE(result.downloadedItems.size(), 2);
    QCOMPARE(result.downloadedItems.first().frame,
             quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT_INT));
    QVERIFY(qAbs(result.downloadedItems.first().x / 1.0e7
                 - (-35.363261)) < 0.000005);

    transport.attemptedMessages.clear();
    QVector<mavlink_mission_item_int_t> upload{
        uploadItem(8), uploadItem(9)};
    QVERIFY(controller.startUpload(MAV_MISSION_TYPE_RALLY, upload));
    QCOMPARE(controller.transferId(), quint64(2));
    QCOMPARE(quint32(transport.attemptedMessages.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_COUNT));
    mavlink_mission_count_t count{};
    mavlink_msg_mission_count_decode(
            &transport.attemptedMessages.last(), &count);
    QCOMPARE(count.count, quint16(2));
    QCOMPARE(count.mission_type, quint8(MAV_MISSION_TYPE_RALLY));

    transport.inject(missionRequest(0, true, MAV_MISSION_TYPE_RALLY));
    QCOMPARE(quint32(transport.attemptedMessages.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_ITEM_INT));
    mavlink_mission_item_int_t intItem{};
    mavlink_msg_mission_item_int_decode(
            &transport.attemptedMessages.last(), &intItem);
    QCOMPARE(intItem.seq, quint16(0));
    QCOMPARE(intItem.x, upload.first().x);
    QCOMPARE(intItem.mission_type, quint8(MAV_MISSION_TYPE_RALLY));

    transport.inject(missionRequest(1, false, MAV_MISSION_TYPE_RALLY));
    QCOMPARE(quint32(transport.attemptedMessages.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_ITEM));
    mavlink_mission_item_t floatItem{};
    mavlink_msg_mission_item_decode(
            &transport.attemptedMessages.last(), &floatItem);
    QCOMPARE(floatItem.seq, quint16(1));
    QCOMPARE(floatItem.frame, quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QVERIFY(qAbs(floatItem.x - upload.at(1).x / 1.0e7) < 0.000005);
    QCOMPARE(controller.progress(), 99);

    transport.inject(missionAck(
            MAV_MISSION_ACCEPTED, MAV_MISSION_TYPE_RALLY));
    QCOMPARE(finishedCount, 2);
    QVERIFY(result.succeeded());
    QCOMPARE(result.direction, MissionTransferService::Direction::Upload);
    QVERIFY(result.downloadedItems.isEmpty());
    QCOMPARE(controller.progress(), 100);
}

void MissionTransferControllerTest::rejectsLeaseCollisionAndFiltersForeignPackets()
{
    MissionProtocolCoordinator coordinator;
    FakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 2);
    int finishedCount = 0;
    MissionTransferResult result;
    connect(&controller, &MissionTransferController::transferFinished,
            this, [&](const MissionTransferResult &terminal) {
        ++finishedCount;
        result = terminal;
    });

    QObject otherOwner;
    const auto otherLease = coordinator.tryAcquire(
            &otherOwner, MissionProtocolCoordinator::MissionType::Mission);
    QVERIFY(otherLease.isValid());
    QVERIFY(!controller.startDownload(MAV_MISSION_TYPE_MISSION));
    QCOMPARE(transport.beginCount, 0);
    QCOMPARE(controller.transferId(), quint64(0));
    QVERIFY(coordinator.release(otherLease));

    transport.beginSucceeds = false;
    QVERIFY(!controller.startDownload(MAV_MISSION_TYPE_MISSION));
    QCOMPARE(transport.beginCount, 1);
    QCOMPARE(transport.endCount, 0);
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(nullptr));
    QCOMPARE(finishedCount, 0);

    transport.beginSucceeds = true;
    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_MISSION));
    const int initialMessages = transport.attemptedMessages.size();
    transport.inject(missionCount(1, MAV_MISSION_TYPE_MISSION,
                                  RemoteSystemId + 1));
    transport.inject(missionCount(1, MAV_MISSION_TYPE_MISSION,
                                  RemoteSystemId, RemoteComponentId,
                                  LocalSystemId - 1));
    transport.inject(missionCount(1, MAV_MISSION_TYPE_FENCE));
    QCOMPARE(transport.attemptedMessages.size(), initialMessages);
    QCOMPARE(controller.progress(), 0);

    transport.inject(missionCount(1));
    QCOMPARE(transport.attemptedMessages.size(), initialMessages + 1);
    QVERIFY(controller.cancel(QStringLiteral("test cleanup")));
    QCOMPARE(finishedCount, 1);
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(nullptr));

    // Losing an exact lease must terminate the operation without allowing its
    // stale token to release a newer owner's generation.
    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_MISSION));
    const auto exposedLease = coordinator.tryAcquire(
            &controller, MissionProtocolCoordinator::MissionType::Mission);
    QVERIFY(exposedLease.isValid());
    QVERIFY(coordinator.release(exposedLease));
    QObject replacementOwner;
    const auto replacementLease = coordinator.tryAcquire(
            &replacementOwner,
            MissionProtocolCoordinator::MissionType::Mission);
    QVERIFY(replacementLease.isValid());
    transport.inject(missionCount(1));
    QCOMPARE(finishedCount, 2);
    QCOMPARE(result.state, MissionTransferService::State::Error);
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(&replacementOwner));
    QVERIFY(coordinator.release(replacementLease));
}

void MissionTransferControllerTest::refreshesLocalIdentityBetweenOperations()
{
    MissionProtocolCoordinator coordinator;
    FakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 2);

    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_MISSION));
    QCOMPARE(transport.attemptedMessages.last().sysid, LocalSystemId);
    QCOMPARE(transport.attemptedMessages.last().compid, LocalComponentId);
    QVERIFY(controller.cancel());

    transport.currentLocalSystemId = LocalSystemId - 1;
    transport.currentLocalComponentId = LocalComponentId - 1;
    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_FENCE));
    QCOMPARE(transport.attemptedMessages.last().sysid,
             transport.currentLocalSystemId);
    QCOMPARE(transport.attemptedMessages.last().compid,
             transport.currentLocalComponentId);

    transport.inject(missionCount(
            1, MAV_MISSION_TYPE_FENCE,
            RemoteSystemId, RemoteComponentId,
            transport.currentLocalSystemId,
            transport.currentLocalComponentId));
    QCOMPARE(quint32(transport.attemptedMessages.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_REQUEST_INT));
    QVERIFY(controller.cancel());
}

void MissionTransferControllerTest::preservesCompletedDownloadWhenTerminalAckFails()
{
    MissionProtocolCoordinator coordinator;
    FakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 2);
    MissionTransferResult result;
    int finishedCount = 0;
    connect(&controller, &MissionTransferController::transferFinished,
            this, [&](const MissionTransferResult &terminal) {
        result = terminal;
        ++finishedCount;
    });

    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_MISSION));
    transport.inject(missionCount(1));
    transport.sendSucceeds = false;
    transport.inject(missionItemInt(0));

    QCOMPARE(finishedCount, 1);
    QVERIFY(result.succeeded());
    QCOMPARE(result.downloadedItems.size(), 1);
    QCOMPARE(quint32(transport.attemptedMessages.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_ACK));
    QVERIFY(!controller.busy());
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(nullptr));
}

void MissionTransferControllerTest::retriesCancelsAndHandlesLinkLossOnce()
{
    MissionProtocolCoordinator coordinator;
    FakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 1);
    int finishedCount = 0;
    MissionTransferResult result;
    connect(&controller, &MissionTransferController::transferFinished,
            this, [&](const MissionTransferResult &terminal) {
        ++finishedCount;
        result = terminal;
    });

    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_MISSION));
    QCOMPARE(transport.attemptedMessages.size(), 1);
    QVERIFY(QMetaObject::invokeMethod(
            &controller, "timeout", Qt::DirectConnection));
    QCOMPARE(transport.attemptedMessages.size(), 2);
    QCOMPARE(quint32(transport.attemptedMessages.at(0).msgid),
             quint32(transport.attemptedMessages.at(1).msgid));
    mavlink_mission_request_list_t first{};
    mavlink_mission_request_list_t retry{};
    mavlink_msg_mission_request_list_decode(
            &transport.attemptedMessages.at(0), &first);
    mavlink_msg_mission_request_list_decode(
            &transport.attemptedMessages.at(1), &retry);
    QCOMPARE(first.target_system, retry.target_system);
    QCOMPARE(first.target_component, retry.target_component);
    QCOMPARE(first.mission_type, retry.mission_type);

    QVERIFY(QMetaObject::invokeMethod(
            &controller, "timeout", Qt::DirectConnection));
    QCOMPARE(finishedCount, 1);
    QCOMPARE(result.state, MissionTransferService::State::Error);
    QVERIFY(!controller.busy());
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(nullptr));
    const int afterTimeout = transport.attemptedMessages.size();
    QVERIFY(QMetaObject::invokeMethod(
            &controller, "timeout", Qt::DirectConnection));
    QCOMPARE(transport.attemptedMessages.size(), afterTimeout);
    QCOMPARE(finishedCount, 1);

    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_RALLY));
    transport.events.clear();
    QVERIFY(controller.cancel(QStringLiteral("user cancelled")));
    QCOMPARE(finishedCount, 2);
    QCOMPARE(result.state, MissionTransferService::State::Cancelled);
    QCOMPARE(result.result, MAV_MISSION_OPERATION_CANCELLED);
    QCOMPARE(transport.events,
             QStringList({QStringLiteral("send"), QStringLiteral("end")}));
    mavlink_mission_ack_t cancelled{};
    mavlink_msg_mission_ack_decode(
            &transport.attemptedMessages.last(), &cancelled);
    QCOMPARE(cancelled.type, quint8(MAV_MISSION_OPERATION_CANCELLED));

    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_FENCE));
    const int beforeLoss = transport.attemptedMessages.size();
    transport.events.clear();
    transport.loseLink();
    QCOMPARE(finishedCount, 3);
    QCOMPARE(result.state, MissionTransferService::State::Error);
    QCOMPARE(transport.attemptedMessages.size(), beforeLoss);
    QCOMPARE(transport.events, QStringList({QStringLiteral("end")}));
    QVERIFY(!controller.busy());
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(nullptr));
}

void MissionTransferControllerTest::sendAndLifetimeFailuresReleaseEverything()
{
    MissionProtocolCoordinator coordinator;
    FakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 2);
    int finishedCount = 0;
    MissionTransferResult result;
    connect(&controller, &MissionTransferController::transferFinished,
            this, [&](const MissionTransferResult &terminal) {
        ++finishedCount;
        result = terminal;
    });

    transport.sendSucceeds = false;
    QVERIFY(!controller.startDownload(MAV_MISSION_TYPE_MISSION));
    QCOMPARE(finishedCount, 1);
    QCOMPARE(result.state, MissionTransferService::State::Error);
    QVERIFY(result.downloadedItems.isEmpty());
    QCOMPARE(transport.events,
             QStringList({QStringLiteral("begin"),
                          QStringLiteral("send-failed"),
                          QStringLiteral("end")}));
    QVERIFY(!controller.busy());
    QVERIFY(!transport.operationActive);
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(nullptr));

    transport.sendSucceeds = true;
    transport.events.clear();
    QVERIFY(controller.startDownload(MAV_MISSION_TYPE_MISSION));
    transport.events.clear();
    transport.sendSucceeds = false;
    QVERIFY(!controller.cancel(QStringLiteral("cancel send failure")));
    QCOMPARE(finishedCount, 2);
    QCOMPARE(result.state, MissionTransferService::State::Cancelled);
    QCOMPARE(result.result, MAV_MISSION_OPERATION_CANCELLED);
    QCOMPARE(transport.events,
             QStringList({QStringLiteral("send-failed"),
                          QStringLiteral("end")}));
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(nullptr));

    auto *ephemeralTransport = new FakeTransport;
    MissionTransferController ephemeralController(
            &coordinator, ephemeralTransport, 60000, 2);
    int destroyedFinishedCount = 0;
    MissionTransferResult destroyedResult;
    connect(&ephemeralController,
            &MissionTransferController::transferFinished,
            this, [&](const MissionTransferResult &terminal) {
        ++destroyedFinishedCount;
        destroyedResult = terminal;
    });
    QVERIFY(ephemeralController.startDownload(MAV_MISSION_TYPE_MISSION));
    delete ephemeralTransport;
    QCOMPARE(destroyedFinishedCount, 1);
    QCOMPARE(destroyedResult.state, MissionTransferService::State::Error);
    QVERIFY(!ephemeralController.busy());
    QCOMPARE(coordinator.owner(), static_cast<QObject *>(nullptr));
}

QTEST_GUILESS_MAIN(MissionTransferControllerTest)
#include "test_missiontransfercontroller.moc"
