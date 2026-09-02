#include "ui/flightplanner/FenceRallyController.h"

#include "QGCMAVLink.h"
#include "comm/MissionProtocolCoordinator.h"
#include "comm/MissionTransferController.h"
#include "ui/flightplanner/FlightPlannerViewModel.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace MissionPlanner;

namespace
{
constexpr quint8 LocalSystem = 250;
constexpr quint8 LocalComponent = 190;
constexpr quint8 VehicleSystem = 42;
constexpr quint8 VehicleComponent = 1;

class FakeTransport final : public MissionTransferTransport
{
public:
    quint8 localSystemId() const override { return LocalSystem; }
    quint8 localComponentId() const override { return LocalComponent; }
    MissionTransferService::Key missionKey(
            MAV_MISSION_TYPE type) const override
    {
        return {VehicleSystem, VehicleComponent, type};
    }
    bool beginOperation() override
    {
        if (active) {
            return false;
        }
        active = true;
        return true;
    }
    void endOperation() override { active = false; }
    bool sendMessage(const mavlink_message_t &message) override
    {
        sent.append(message);
        return true;
    }
    void inject(const mavlink_message_t &message)
    {
        emit messageReceived(message);
    }

    bool active = false;
    QVector<mavlink_message_t> sent;
};

Fence sampleFence(double offset = 0.0)
{
    Fence fence;
    fence.HasReturn = true;
    fence.ReturnPoint.Return = {40.0 + offset, 28.0 + offset, 0.0};
    fence.ReturnPoint.Frame = MAV_FRAME_GLOBAL;
    FencePolygon polygon;
    polygon.Mode = FencePolygon::PolyType::Inclusive;
    polygon.Points = {
        {40.1 + offset, 28.1 + offset, 0.0},
        {40.2 + offset, 28.1 + offset, 0.0},
        {40.2 + offset, 28.2 + offset, 0.0},
    };
    fence.Polygons.append(polygon);
    return fence;
}

RallyPoints sampleRally(double offset = 0.0)
{
    RallyPoint point;
    point.Position = {41.0 + offset, 29.0 + offset, 75.0 + offset};
    point.BreakAltitude = 50.0 + offset;
    point.LandHeading = 12345.0;
    point.Flags = 3;
    point.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    RallyPoints rally;
    rally.Points.append(point);
    return rally;
}

mavlink_message_t missionCount(MAV_MISSION_TYPE type, quint16 count)
{
    mavlink_mission_count_t payload{};
    payload.count = count;
    payload.target_system = LocalSystem;
    payload.target_component = LocalComponent;
    payload.mission_type = static_cast<quint8>(type);
    mavlink_message_t message{};
    mavlink_msg_mission_count_encode(
            VehicleSystem, VehicleComponent, &message, &payload);
    return message;
}

mavlink_message_t missionRequest(MAV_MISSION_TYPE type, quint16 sequence)
{
    mavlink_mission_request_int_t payload{};
    payload.seq = sequence;
    payload.target_system = LocalSystem;
    payload.target_component = LocalComponent;
    payload.mission_type = static_cast<quint8>(type);
    mavlink_message_t message{};
    mavlink_msg_mission_request_int_encode(
            VehicleSystem, VehicleComponent, &message, &payload);
    return message;
}

mavlink_message_t missionAck(MAV_MISSION_TYPE type)
{
    mavlink_mission_ack_t payload{};
    payload.type = MAV_MISSION_ACCEPTED;
    payload.target_system = LocalSystem;
    payload.target_component = LocalComponent;
    payload.mission_type = static_cast<quint8>(type);
    mavlink_message_t message{};
    mavlink_msg_mission_ack_encode(
            VehicleSystem, VehicleComponent, &message, &payload);
    return message;
}
}

class FenceRallyControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void typedStoresRoundTripAndRejectInvalidMutation();
    void legacyFilesReplaceAppendAndRejectLossyFenceSave();
    void transfersUseFenceAndRallyMissionTypes();
};

void FenceRallyControllerTest::typedStoresRoundTripAndRejectInvalidMutation()
{
    FlightPlannerViewModel planner;
    FenceRallyController controller(&planner);

    Fence fence = sampleFence();
    FenceCircle circle;
    circle.Center = {40.15, 28.15, 12.0};
    circle.Radius = 125.5;
    circle.Mode = FenceCircle::PolyType::Exclusive;
    fence.Circles.append(circle);
    QVERIFY(controller.SetFence(fence));
    QCOMPARE(planner.MissionType(), QStringLiteral("Fence"));
    QCOMPARE(planner.Waypoints()->storeRowCount(
                     FlightPlannerMissionModel::MissionStore::Fence), 5);

    const Fence::DecodeResult fenceSnapshot = controller.CurrentFence();
    QVERIFY2(fenceSnapshot.ok, qPrintable(fenceSnapshot.error));
    QVERIFY(fenceSnapshot.fence.HasReturn);
    QCOMPARE(fenceSnapshot.fence.Polygons.size(), 1);
    QCOMPARE(fenceSnapshot.fence.Polygons.first().Points.size(), 3);
    QCOMPARE(fenceSnapshot.fence.Circles.size(), 1);
    QCOMPARE(fenceSnapshot.fence.Circles.first().Radius, 125.5);
    QCOMPARE(fenceSnapshot.fence.Circles.first().Center.Altitude, 12.0);

    Fence invalid = sampleFence();
    invalid.Polygons.first().Points.removeLast();
    QVERIFY(!controller.SetFence(invalid));
    QVERIFY(controller.LastError().contains(QStringLiteral("at least 3")));
    QCOMPARE(planner.Waypoints()->storeRowCount(
                     FlightPlannerMissionModel::MissionStore::Fence), 5);

    const RallyPoints rally = sampleRally();
    QVERIFY(controller.SetRally(rally));
    QCOMPARE(planner.MissionType(), QStringLiteral("Rally"));
    const RallyPoints::DecodeResult rallySnapshot = controller.CurrentRally();
    QVERIFY2(rallySnapshot.ok, qPrintable(rallySnapshot.error));
    QCOMPARE(rallySnapshot.rally.Points.size(), 1);
    QCOMPARE(rallySnapshot.rally.Points.first().Position.Altitude, 75.0);
    QCOMPARE(rallySnapshot.rally.Points.first().BreakAltitude, 50.0);
    QCOMPARE(rallySnapshot.rally.Points.first().LandHeading, 12345.0);
    QCOMPARE(rallySnapshot.rally.Points.first().Flags, quint8(3));

    QTemporaryDir frameLossDirectory;
    QVERIFY(frameLossDirectory.isValid());
    const QString frameLossPath = frameLossDirectory.filePath(
            QStringLiteral("frame-loss.ral"));
    planner.Waypoints()->rowAt(0)->setP4(1.0);
    QVERIFY(!controller.SaveLegacyRally(frameLossPath));
    QVERIFY(!QFile::exists(frameLossPath));
    QVERIFY(controller.LastError().contains(QStringLiteral("P4")));

    RallyPoints frameLoss = rally;
    frameLoss.Points.first().Frame = MAV_FRAME_GLOBAL;
    QVERIFY(controller.SetRally(frameLoss));
    QVERIFY(!controller.SaveLegacyRally(frameLossPath));
    QVERIFY(!QFile::exists(frameLossPath));
    QVERIFY(controller.LastError().contains(QStringLiteral("frame"),
                                             Qt::CaseInsensitive));
}

void FenceRallyControllerTest::legacyFilesReplaceAppendAndRejectLossyFenceSave()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fencePath = directory.filePath(QStringLiteral("one.fen"));
    const QString lossyPath = directory.filePath(QStringLiteral("lossy.fen"));
    const QString rallyPath = directory.filePath(QStringLiteral("points.ral"));

    FlightPlannerViewModel sourcePlanner;
    FenceRallyController source(&sourcePlanner);
    QVERIFY(source.SetFence(sampleFence()));
    QVERIFY(source.SaveLegacyFence(fencePath));
    QVERIFY(QFile::exists(fencePath));

    Fence altitudeLoss = sampleFence();
    altitudeLoss.Polygons.first().Points.first().Altitude = 10.0;
    QVERIFY(source.SetFence(altitudeLoss));
    QVERIFY(!source.SaveLegacyFence(lossyPath));
    QVERIFY(!QFile::exists(lossyPath));
    QVERIFY(source.LastError().contains(QStringLiteral("altitude"),
                                        Qt::CaseInsensitive));

    QVERIFY(source.SetRally(sampleRally()));
    QVERIFY(source.SaveLegacyRally(rallyPath));

    FlightPlannerViewModel loadedPlanner;
    FenceRallyController loaded(&loadedPlanner);
    QVERIFY(loaded.LoadLegacyFence(fencePath));
    Fence::DecodeResult fence = loaded.CurrentFence();
    QVERIFY2(fence.ok, qPrintable(fence.error));
    QCOMPARE(fence.fence.Polygons.size(), 1);
    QCOMPARE(fence.fence.ReturnPoint.Return.Latitude, 40.0);

    // Mission Planner append semantics keep the existing return point and add
    // the incoming polygon.  Such a model cannot be represented by legacy
    // .fen, so export must fail before creating/truncating the destination.
    QVERIFY(loaded.LoadLegacyFence(fencePath, true));
    fence = loaded.CurrentFence();
    QVERIFY2(fence.ok, qPrintable(fence.error));
    QCOMPARE(fence.fence.Polygons.size(), 2);
    QCOMPARE(fence.fence.ReturnPoint.Return.Latitude, 40.0);
    QVERIFY(!loaded.SaveLegacyFence(lossyPath));
    QVERIFY(!QFile::exists(lossyPath));
    QVERIFY(loaded.LastError().contains(QStringLiteral("exactly one")));

    QVERIFY(loaded.LoadLegacyRally(rallyPath));
    QVERIFY(loaded.LoadLegacyRally(rallyPath, true));
    const RallyPoints::DecodeResult rally = loaded.CurrentRally();
    QVERIFY2(rally.ok, qPrintable(rally.error));
    QCOMPARE(rally.rally.Points.size(), 2);
    QCOMPARE(rally.rally.Points.at(1).BreakAltitude, 50.0);
    QCOMPARE(rally.rally.Points.at(1).LandHeading, 12345.0);
    QCOMPARE(rally.rally.Points.at(1).Flags, quint8(3));
}

void FenceRallyControllerTest::transfersUseFenceAndRallyMissionTypes()
{
    MissionProtocolCoordinator coordinator;
    FakeTransport transport;
    MissionTransferController transfer(&coordinator, &transport, 60000, 2);
    FlightPlannerViewModel planner;
    planner.setMissionTransferController(&transfer);
    FenceRallyController controller(&planner);

    QVERIFY(controller.SetFence(sampleFence()));
    transport.sent.clear();
    QVERIFY(controller.DownloadFence());
    QVERIFY(!transport.sent.isEmpty());
    QCOMPARE(quint32(transport.sent.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_REQUEST_LIST));
    mavlink_mission_request_list_t requestList{};
    mavlink_msg_mission_request_list_decode(
            &transport.sent.last(), &requestList);
    QCOMPARE(requestList.mission_type, quint8(MAV_MISSION_TYPE_FENCE));
    transport.inject(missionCount(MAV_MISSION_TYPE_FENCE, 0));
    QVERIFY(!planner.TransferBusy());
    QCOMPARE(planner.MissionType(), QStringLiteral("Fence"));
    QCOMPARE(planner.Waypoints()->storeRowCount(
                     FlightPlannerMissionModel::MissionStore::Fence), 0);

    QVERIFY(controller.SetRally(sampleRally()));
    transport.sent.clear();
    QVERIFY(controller.UploadRally());
    QVERIFY(!transport.sent.isEmpty());
    QCOMPARE(quint32(transport.sent.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_COUNT));
    mavlink_mission_count_t count{};
    mavlink_msg_mission_count_decode(&transport.sent.last(), &count);
    QCOMPARE(count.mission_type, quint8(MAV_MISSION_TYPE_RALLY));
    QCOMPARE(count.count, quint16(1));

    transport.inject(missionRequest(MAV_MISSION_TYPE_RALLY, 0));
    QCOMPARE(quint32(transport.sent.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_ITEM_INT));
    mavlink_mission_item_int_t item{};
    mavlink_msg_mission_item_int_decode(&transport.sent.last(), &item);
    QCOMPARE(item.mission_type, quint8(MAV_MISSION_TYPE_RALLY));
    QCOMPARE(item.command, quint16(MAV_CMD_NAV_RALLY_POINT));
    QCOMPARE(item.param1, 50.0f);
    QCOMPARE(item.param2, 12345.0f);
    QCOMPARE(item.param3, 3.0f);

    transport.inject(missionAck(MAV_MISSION_TYPE_RALLY));
    QVERIFY(!planner.TransferBusy());
    QVERIFY(planner.Status().startsWith(QStringLiteral("Wrote 1 rally")));
}

QTEST_APPLESS_MAIN(FenceRallyControllerTest)

#include "test_fencerallycontroller.moc"
