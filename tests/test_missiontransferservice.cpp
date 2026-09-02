#include "comm/MissionTransferService.h"

#include <QtTest/QTest>

class MissionTransferServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void downloadsMissionIntInSequence();
    void uploadsMissionIntInSequence();
    void strictlyFiltersTransactionKey();
    void supportsFloatFallbackAndDownloadRejection();
    void preservesMissionFrameCoordinatesAndRepeatsExpectedRequest();
    void retriesTimesOutAndCancelsDeterministically();
    void handlesEmptyTransfersAndProtocolErrors();
};

namespace
{
constexpr quint8 LocalSystemId = 255;
constexpr quint8 LocalComponentId = MAV_COMP_ID_PRIMARY;

MissionTransferService::Key missionKey(
        MAV_MISSION_TYPE type = MAV_MISSION_TYPE_MISSION)
{
    return {42, 1, type};
}

template<typename T>
void addressToGcs(T &message)
{
    message.target_system = LocalSystemId;
    message.target_component = LocalComponentId;
}

template<typename T>
const T &payload(const MissionTransferService::Transition &transition)
{
    Q_ASSERT(transition.outbound);
    return std::get<T>(transition.outbound->payload);
}

mavlink_mission_item_int_t item(quint16 sequence, MAV_MISSION_TYPE type)
{
    mavlink_mission_item_int_t result{};
    result.seq = sequence;
    result.command = MAV_CMD_NAV_WAYPOINT;
    result.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    result.x = -353632610;
    result.y = 1491652300;
    result.z = 50.0f + sequence;
    result.mission_type = static_cast<quint8>(type);
    addressToGcs(result);
    return result;
}
}

void MissionTransferServiceTest::downloadsMissionIntInSequence()
{
    MissionTransferService service;
    const auto key = missionKey(MAV_MISSION_TYPE_FENCE);

    auto transition = service.startDownload(key);
    QVERIFY(transition.handled);
    QCOMPARE(service.state(), MissionTransferService::State::WaitingForCount);
    QCOMPARE(transition.outbound->type,
             MissionTransferService::MessageType::MissionRequestList);
    const auto &list = payload<mavlink_mission_request_list_t>(transition);
    QCOMPARE(list.target_system, key.systemId);
    QCOMPARE(list.target_component, key.componentId);
    QCOMPARE(list.mission_type, static_cast<quint8>(key.missionType));

    mavlink_mission_count_t count{};
    count.count = 2;
    count.mission_type = MAV_MISSION_TYPE_FENCE;
    addressToGcs(count);
    transition = service.handleMissionCount(42, 1, count);
    QVERIFY(transition.handled);
    QCOMPARE(service.state(), MissionTransferService::State::WaitingForItemInt);
    QCOMPARE(payload<mavlink_mission_request_int_t>(transition).seq, quint16(0));

    const auto first = item(0, MAV_MISSION_TYPE_FENCE);
    transition = service.handleMissionItemInt(42, 1, first);
    QVERIFY(transition.handled);
    QCOMPARE(payload<mavlink_mission_request_int_t>(transition).seq, quint16(1));
    QCOMPARE(service.downloadedItems().size(), 1);

    const auto second = item(1, MAV_MISSION_TYPE_FENCE);
    transition = service.handleMissionItemInt(42, 1, second);
    QVERIFY(transition.handled);
    QCOMPARE(service.state(), MissionTransferService::State::Complete);
    QCOMPARE(transition.outbound->type,
             MissionTransferService::MessageType::MissionAck);
    QCOMPARE(payload<mavlink_mission_ack_t>(transition).type,
             quint8(MAV_MISSION_ACCEPTED));
    QCOMPARE(service.downloadedItems().size(), 2);
    QCOMPARE(service.downloadedItems().at(1).z, second.z);
}

void MissionTransferServiceTest::uploadsMissionIntInSequence()
{
    MissionTransferService service;
    const auto key = missionKey(MAV_MISSION_TYPE_RALLY);
    QVector<mavlink_mission_item_int_t> items{
        item(8, MAV_MISSION_TYPE_MISSION),
        item(9, MAV_MISSION_TYPE_MISSION)};

    auto transition = service.startUpload(key, items);
    QVERIFY(transition.handled);
    QCOMPARE(service.state(), MissionTransferService::State::WaitingForRequestInt);
    const auto &count = payload<mavlink_mission_count_t>(transition);
    QCOMPARE(count.count, quint16(2));
    QCOMPARE(count.target_system, key.systemId);
    QCOMPARE(count.mission_type, quint8(MAV_MISSION_TYPE_RALLY));

    mavlink_mission_request_int_t request{};
    request.mission_type = MAV_MISSION_TYPE_RALLY;
    request.seq = 0;
    addressToGcs(request);
    transition = service.handleMissionRequestInt(42, 1, request);
    QVERIFY(transition.handled);
    const auto &first = payload<mavlink_mission_item_int_t>(transition);
    QCOMPARE(first.seq, quint16(0));
    QCOMPARE(first.target_system, key.systemId);
    QCOMPARE(first.target_component, key.componentId);
    QCOMPARE(first.mission_type, quint8(MAV_MISSION_TYPE_RALLY));

    transition = service.handleMissionRequestInt(42, 1, request);
    QVERIFY(transition.handled);
    QCOMPARE(payload<mavlink_mission_item_int_t>(transition).seq, quint16(0));
    QCOMPARE(service.nextSequence(), quint16(1));

    request.seq = 1;
    transition = service.handleMissionRequestInt(42, 1, request);
    QVERIFY(transition.handled);
    QCOMPARE(service.state(), MissionTransferService::State::WaitingForAck);
    QCOMPARE(payload<mavlink_mission_item_int_t>(transition).seq, quint16(1));

    mavlink_mission_ack_t ack{};
    ack.type = MAV_MISSION_ACCEPTED;
    ack.mission_type = MAV_MISSION_TYPE_RALLY;
    addressToGcs(ack);
    transition = service.handleMissionAck(42, 1, ack);
    QVERIFY(transition.handled);
    QVERIFY(!transition.outbound);
    QCOMPARE(service.state(), MissionTransferService::State::Complete);
}

void MissionTransferServiceTest::strictlyFiltersTransactionKey()
{
    MissionTransferService service;
    service.startDownload(missionKey(MAV_MISSION_TYPE_FENCE));

    mavlink_mission_count_t count{};
    count.count = 1;
    count.mission_type = MAV_MISSION_TYPE_FENCE;
    addressToGcs(count);
    QVERIFY(!service.handleMissionCount(41, 1, count).handled);
    QVERIFY(!service.handleMissionCount(42, 2, count).handled);
    count.mission_type = MAV_MISSION_TYPE_MISSION;
    QVERIFY(!service.handleMissionCount(42, 1, count).handled);
    count.mission_type = MAV_MISSION_TYPE_FENCE;
    count.target_system = 254;
    QVERIFY(!service.handleMissionCount(42, 1, count).handled);
    addressToGcs(count);
    QCOMPARE(service.state(), MissionTransferService::State::WaitingForCount);
    QCOMPARE(service.retryCount(), 0);

    count.mission_type = MAV_MISSION_TYPE_FENCE;
    QVERIFY(service.handleMissionCount(42, 1, count).handled);
    mavlink_mission_item_int_t wrong = item(0, MAV_MISSION_TYPE_FENCE);
    QVERIFY(!service.handleMissionItemInt(42, 2, wrong).handled);
    wrong.mission_type = MAV_MISSION_TYPE_RALLY;
    QVERIFY(!service.handleMissionItemInt(42, 1, wrong).handled);
    QCOMPARE(service.state(), MissionTransferService::State::WaitingForItemInt);
}

void MissionTransferServiceTest::supportsFloatFallbackAndDownloadRejection()
{
    MissionTransferService service;
    const auto key = missionKey();
    service.startDownload(key);

    mavlink_mission_count_t count{};
    count.count = 2;
    count.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(count);
    auto transition = service.handleMissionCount(42, 1, count);
    QCOMPARE(transition.outbound->type,
             MissionTransferService::MessageType::MissionRequestInt);

    mavlink_mission_item_t first{};
    first.seq = 0;
    first.command = MAV_CMD_NAV_WAYPOINT;
    first.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    first.x = -35.363261f;
    first.y = 149.165230f;
    first.z = 60.0f;
    first.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(first);
    transition = service.handleMissionItem(42, 1, first);
    QVERIFY(transition.handled);
    QCOMPARE(transition.outbound->type,
             MissionTransferService::MessageType::MissionRequest);
    QCOMPARE(payload<mavlink_mission_request_t>(transition).seq, quint16(1));

    mavlink_mission_item_t second = first;
    second.seq = 1;
    second.z = 75.0f;
    transition = service.handleMissionItem(42, 1, second);
    QCOMPARE(service.state(), MissionTransferService::State::Complete);
    QCOMPARE(transition.outbound->type,
             MissionTransferService::MessageType::MissionAck);
    QCOMPARE(service.downloadedItems().at(0).frame,
             quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT_INT));
    QVERIFY(qAbs(service.downloadedItems().at(0).x / 1.0e7 - first.x)
            < 0.000001);

    QVector<mavlink_mission_item_int_t> uploadItems{
        item(0, MAV_MISSION_TYPE_MISSION)};
    service.startUpload(key, uploadItems);
    mavlink_mission_request_t request{};
    request.seq = 0;
    request.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(request);
    transition = service.handleMissionRequest(42, 1, request);
    QVERIFY(transition.handled);
    QCOMPARE(transition.outbound->type,
             MissionTransferService::MessageType::MissionItem);
    const auto &floatItem = payload<mavlink_mission_item_t>(transition);
    QCOMPARE(floatItem.frame, quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QVERIFY(qAbs(floatItem.x - uploadItems.at(0).x / 1.0e7) < 0.000005);

    service.reset();
    service.startDownload(key);
    mavlink_mission_ack_t rejection{};
    rejection.type = MAV_MISSION_DENIED;
    rejection.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(rejection);
    transition = service.handleMissionAck(42, 1, rejection);
    QVERIFY(transition.handled);
    QCOMPARE(service.state(), MissionTransferService::State::Error);
    QCOMPARE(service.missionResult(), MAV_MISSION_DENIED);
}

void MissionTransferServiceTest::preservesMissionFrameCoordinatesAndRepeatsExpectedRequest()
{
    MissionTransferService service;
    const auto key = missionKey();
    service.startDownload(key);

    mavlink_mission_count_t count{};
    count.count = 2;
    count.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(count);
    QVERIFY(service.handleMissionCount(42, 1, count).handled);

    mavlink_mission_item_t first{};
    first.seq = 0;
    first.command = MAV_CMD_DO_CHANGE_SPEED;
    first.frame = MAV_FRAME_MISSION;
    first.x = 12.0f;
    first.y = -4.0f;
    first.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(first);
    auto transition = service.handleMissionItem(42, 1, first);
    QCOMPARE(payload<mavlink_mission_request_t>(transition).seq, quint16(1));
    QCOMPARE(service.downloadedItems().at(0).x, qint32(12));
    QCOMPARE(service.downloadedItems().at(0).y, qint32(-4));

    transition = service.handleMissionItem(42, 1, first);
    QVERIFY(transition.handled);
    QCOMPARE(service.state(), MissionTransferService::State::WaitingForItemInt);
    QCOMPARE(service.downloadedItems().size(), 1);
    QCOMPARE(transition.outbound->type,
             MissionTransferService::MessageType::MissionRequest);
    QCOMPARE(payload<mavlink_mission_request_t>(transition).seq, quint16(1));

    service.reset();
    mavlink_mission_item_int_t uploadItem{};
    uploadItem.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    uploadItem.command = MAV_CMD_DO_CHANGE_SPEED;
    uploadItem.x = 37;
    uploadItem.y = -9;
    uploadItem.mission_type = MAV_MISSION_TYPE_MISSION;
    transition = service.startUpload(key, {uploadItem});
    QVERIFY(transition.handled);

    mavlink_mission_request_t request{};
    request.seq = 0;
    request.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(request);
    transition = service.handleMissionRequest(42, 1, request);
    const auto &converted = payload<mavlink_mission_item_t>(transition);
    QCOMPARE(converted.frame, quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QCOMPARE(converted.x, 37.0f);
    QCOMPARE(converted.y, -9.0f);
}

void MissionTransferServiceTest::retriesTimesOutAndCancelsDeterministically()
{
    MissionTransferService service(2);
    service.startDownload(missionKey());

    auto transition = service.onTimeout();
    QVERIFY(transition.handled);
    QVERIFY(transition.outbound->retry);
    QCOMPARE(service.retryCount(), 1);
    QCOMPARE(transition.outbound->type,
             MissionTransferService::MessageType::MissionRequestList);

    transition = service.onTimeout();
    QVERIFY(transition.handled);
    QVERIFY(transition.outbound->retry);
    QCOMPARE(service.retryCount(), 2);

    transition = service.onTimeout();
    QVERIFY(transition.handled);
    QVERIFY(!transition.outbound);
    QCOMPARE(service.state(), MissionTransferService::State::Error);
    QCOMPARE(service.missionResult(), MAV_MISSION_ERROR);
    QVERIFY(!service.errorString().isEmpty());

    service.reset();
    service.startDownload(missionKey(MAV_MISSION_TYPE_RALLY));
    transition = service.cancel(QStringLiteral("user cancelled"));
    QVERIFY(transition.handled);
    QCOMPARE(service.state(), MissionTransferService::State::Cancelled);
    QCOMPARE(payload<mavlink_mission_ack_t>(transition).type,
             quint8(MAV_MISSION_OPERATION_CANCELLED));
    QCOMPARE(service.errorString(), QStringLiteral("user cancelled"));
    QVERIFY(!service.onTimeout().handled);
}

void MissionTransferServiceTest::handlesEmptyTransfersAndProtocolErrors()
{
    MissionTransferService service;
    auto transition = service.startDownload(missionKey());
    mavlink_mission_count_t count{};
    count.count = 0;
    count.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(count);
    transition = service.handleMissionCount(42, 1, count);
    QCOMPARE(service.state(), MissionTransferService::State::Complete);
    QCOMPARE(payload<mavlink_mission_ack_t>(transition).type,
             quint8(MAV_MISSION_ACCEPTED));

    transition = service.startUpload(missionKey(), {});
    QCOMPARE(service.state(), MissionTransferService::State::WaitingForAck);
    QCOMPARE(payload<mavlink_mission_count_t>(transition).count, quint16(0));
    mavlink_mission_ack_t ack{};
    ack.type = MAV_MISSION_ACCEPTED;
    ack.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(ack);
    QVERIFY(service.handleMissionAck(42, 1, ack).handled);
    QCOMPARE(service.state(), MissionTransferService::State::Complete);

    QVector<mavlink_mission_item_int_t> one{item(0, MAV_MISSION_TYPE_MISSION)};
    service.startUpload(missionKey(), one);
    mavlink_mission_request_int_t badRequest{};
    badRequest.seq = 1;
    badRequest.mission_type = MAV_MISSION_TYPE_MISSION;
    addressToGcs(badRequest);
    transition = service.handleMissionRequestInt(42, 1, badRequest);
    QVERIFY(transition.handled);
    QCOMPARE(service.state(), MissionTransferService::State::Error);
    QCOMPARE(service.missionResult(), MAV_MISSION_INVALID_SEQUENCE);

    service.startDownload(missionKey());
    count.count = 2;
    service.handleMissionCount(42, 1, count);
    auto outOfOrder = item(1, MAV_MISSION_TYPE_MISSION);
    transition = service.handleMissionItemInt(42, 1, outOfOrder);
    QCOMPARE(service.state(), MissionTransferService::State::Error);
    QCOMPARE(payload<mavlink_mission_ack_t>(transition).type,
             quint8(MAV_MISSION_INVALID_SEQUENCE));
}

QTEST_APPLESS_MAIN(MissionTransferServiceTest)
#include "test_missiontransferservice.moc"
