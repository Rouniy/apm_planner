#include "ui/flightplanner/FlightPlannerViewModel.h"
#include "ui/flightplanner/QgcPlanFileCodec.h"

#include "QGCMAVLink.h"
#include "comm/MissionProtocolCoordinator.h"
#include "comm/MissionTransferController.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <limits>

namespace
{
constexpr quint8 PlannerLocalSystem = 250;
constexpr quint8 PlannerLocalComponent = 190;
constexpr quint8 PlannerRemoteSystem = 42;
constexpr quint8 PlannerRemoteComponent = 1;

class PlannerFakeTransport final : public MissionTransferTransport
{
public:
    quint8 localSystemId() const override { return PlannerLocalSystem; }
    quint8 localComponentId() const override { return PlannerLocalComponent; }
    MissionTransferService::Key missionKey(
            MAV_MISSION_TYPE type) const override
    {
        return {PlannerRemoteSystem, PlannerRemoteComponent, type};
    }
    bool beginOperation() override
    {
        if (active)
            return false;
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

mavlink_message_t plannerMissionCount(quint16 count)
{
    mavlink_mission_count_t payload{};
    payload.count = count;
    payload.target_system = PlannerLocalSystem;
    payload.target_component = PlannerLocalComponent;
    payload.mission_type = MAV_MISSION_TYPE_MISSION;
    mavlink_message_t message{};
    mavlink_msg_mission_count_encode(
            PlannerRemoteSystem, PlannerRemoteComponent, &message, &payload);
    return message;
}

mavlink_message_t plannerMissionItem(
        quint16 sequence, qint32 latitude, qint32 longitude, float altitude,
        quint8 frame = MAV_FRAME_GLOBAL_INT)
{
    mavlink_mission_item_int_t payload{};
    payload.seq = sequence;
    payload.command = MAV_CMD_NAV_WAYPOINT;
    payload.frame = frame;
    payload.x = latitude;
    payload.y = longitude;
    payload.z = altitude;
    payload.target_system = PlannerLocalSystem;
    payload.target_component = PlannerLocalComponent;
    payload.mission_type = MAV_MISSION_TYPE_MISSION;
    mavlink_message_t message{};
    mavlink_msg_mission_item_int_encode(
            PlannerRemoteSystem, PlannerRemoteComponent, &message, &payload);
    return message;
}

mavlink_message_t plannerMissionRequest(quint16 sequence)
{
    mavlink_mission_request_int_t payload{};
    payload.seq = sequence;
    payload.target_system = PlannerLocalSystem;
    payload.target_component = PlannerLocalComponent;
    payload.mission_type = MAV_MISSION_TYPE_MISSION;
    mavlink_message_t message{};
    mavlink_msg_mission_request_int_encode(
            PlannerRemoteSystem, PlannerRemoteComponent, &message, &payload);
    return message;
}

mavlink_message_t plannerMissionAck()
{
    mavlink_mission_ack_t payload{};
    payload.type = MAV_MISSION_ACCEPTED;
    payload.target_system = PlannerLocalSystem;
    payload.target_component = PlannerLocalComponent;
    payload.mission_type = MAV_MISSION_TYPE_MISSION;
    mavlink_message_t message{};
    mavlink_msg_mission_ack_encode(
            PlannerRemoteSystem, PlannerRemoteComponent, &message, &payload);
    return message;
}
}

class FlightPlannerViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndTypedWaypointCreation();
    void wplRoundTripKeepsHomeOutsideMissionRows();
    void appendKeepsHomeAndMalformedLoadIsAtomic();
    void homeValidityIsPreservedAcrossWplOperations();
    void transferAndVehicleBindingsAreUnavailableByDefault();
    void missionTransfersRoundTripThroughBoundController();
    void navigationParametersFollowArduPilot47Aliases();
    void surveyBoundaryAndPlanAppendUseMissionStore();
    void qgcPlanAppendRemapsJumpTargetsAndKeepsHome();
    void missionActionCommandsAndSetHomeValidation();
    void setFenceReturnRepairsDuplicatesAndIsStoreAwareUndoable();
    void explicitContextCommandsMatchMissionPlannerParameters();
    void currentVehiclePositionMatchesMissionPlannerContract();
    void insertReverseAndMoveKeepJumpTargetsValid();
    void modifyAltitudesAndUndoKeepHomeSeparate();
    void altitudeUnitsConvertPresentationOnly();
    void distanceUnitsConvertPresentationOnly();
    void elevationProfileUsesCanonicalMissionSnapshot();
    void missionActionsAreLockedDuringTransfer();
    void plannerSettingsDriveRouteMetricsAndAltitudeWarning();
    void terrainProviderMatchesMissionPlannerPlacementRules();
};

void FlightPlannerViewModelTest::defaultsAndTypedWaypointCreation()
{
    FlightPlannerViewModel viewModel;
    QCOMPARE(viewModel.Status(), QStringLiteral("ready"));
    QCOMPARE(viewModel.MissionType(), QStringLiteral("Mission"));
    QCOMPARE(viewModel.DefaultAltitude(), 100.0);
    QCOMPARE(viewModel.WpRadius(), 90.0);
    QCOMPARE(viewModel.WpAccelerationCms(), 100.0);
    QCOMPARE(viewModel.WpSpeedCms(), 600.0);
    QVERIFY(!viewModel.HomeValid());

    WpRow *mission = viewModel.AddWaypointAt(40.0, 28.0);
    QVERIFY(mission);
    QCOMPARE(mission->Command(), quint16(MAV_CMD_NAV_WAYPOINT));
    QCOMPARE(mission->Frame(), quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QCOMPARE(mission->Alt(), 100.0);

    viewModel.setMissionType(QStringLiteral("Fence"));
    WpRow *fence = viewModel.AddWaypointAt(41.0, 29.0, 80.0);
    QVERIFY(fence);
    QCOMPARE(fence->Command(),
             quint16(MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION));
    QCOMPARE(fence->Frame(), quint8(MAV_FRAME_GLOBAL));
    QCOMPARE(fence->Alt(), 0.0);

    viewModel.setMissionType(QStringLiteral("Rally"));
    WpRow *rally = viewModel.AddWaypointAt(42.0, 30.0, 75.0);
    QVERIFY(rally);
    QCOMPARE(rally->Command(), quint16(MAV_CMD_NAV_RALLY_POINT));
    QCOMPARE(rally->Alt(), 75.0);
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Mission), 1);
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Fence), 1);
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Rally), 1);
}

void FlightPlannerViewModelTest::wplRoundTripKeepsHomeOutsideMissionRows()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("mission.waypoints"));

    FlightPlannerViewModel saved;
    saved.SetHomeFromVehicle(-35.3, 149.2, 600.0);
    WpRow *waypoint = saved.AddWaypointAt(-35.2, 149.1, 75.0);
    QVERIFY(waypoint);
    waypoint->setP1(4.5);
    waypoint->setP4(180.0);
    QVERIFY(saved.SaveFile(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray contents = file.readAll();
    QVERIFY(contents.startsWith("QGC WPL 110\n"));
    QCOMPARE(contents.count('\n'), 3);

    FlightPlannerViewModel loaded;
    QVERIFY(loaded.LoadFile(path));
    QVERIFY(loaded.HomeValid());
    QCOMPARE(loaded.HomeLat(), -35.3);
    QCOMPARE(loaded.HomeLng(), 149.2);
    QCOMPARE(loaded.HomeAlt(), 600.0);
    QCOMPARE(loaded.Waypoints()->rowCount(), 1);
    QCOMPARE(loaded.Waypoints()->rowAt(0)->Seq(), 0);
    QCOMPARE(loaded.Waypoints()->rowAt(0)->Lat(), -35.2);
    QCOMPARE(loaded.Waypoints()->rowAt(0)->P1(), 4.5);
    QCOMPARE(loaded.Waypoints()->rowAt(0)->P4(), 180.0);
}

void FlightPlannerViewModelTest::appendKeepsHomeAndMalformedLoadIsAtomic()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString appendPath = directory.filePath(QStringLiteral("append.txt"));
    QFile appendFile(appendPath);
    QVERIFY(appendFile.open(QIODevice::WriteOnly | QIODevice::Text));
    appendFile.write("QGC WPL 110\n"
                     "# exported mission home and waypoint\n"
                     "0\t1\t0\t16\t0\t0\t0\t0\t1\t2\t3\t1\n"
                     "1\t0\t3\t16\t0\t0\t0\t0\t40\t28\t50\t1\n");
    appendFile.close();

    FlightPlannerViewModel viewModel;
    viewModel.SetHomeFromVehicle(10.0, 20.0, 30.0);
    QVERIFY(viewModel.HomeValid());
    viewModel.AddWaypointAt(41.0, 29.0, 60.0);
    QVERIFY(viewModel.LoadAndAppend(appendPath));
    QCOMPARE(viewModel.HomeLat(), 10.0);
    QCOMPARE(viewModel.HomeLng(), 20.0);
    QCOMPARE(viewModel.HomeAlt(), 30.0);
    QCOMPARE(viewModel.Waypoints()->rowCount(), 2);
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Seq(), 1);

    const QString badPath = directory.filePath(QStringLiteral("broken.txt"));
    QFile badFile(badPath);
    QVERIFY(badFile.open(QIODevice::WriteOnly | QIODevice::Text));
    badFile.write("QGC WPL 110\ninvalid row\n");
    badFile.close();
    QVERIFY(!viewModel.LoadFile(badPath));
    QCOMPARE(viewModel.Waypoints()->rowCount(), 2);
    QVERIFY(viewModel.Status().startsWith(QStringLiteral("Load failed")));

    viewModel.setMissionType(QStringLiteral("Fence"));
    const QString unsupportedPath = directory.filePath(
        QStringLiteral("fence.waypoints"));
    QVERIFY(!viewModel.SaveFile(unsupportedPath));
    QVERIFY(!QFile::exists(unsupportedPath));
}

void FlightPlannerViewModelTest::homeValidityIsPreservedAcrossWplOperations()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    FlightPlannerViewModel viewModel;
    viewModel.AddWaypointAt(40.0, 28.0, 50.0);
    const QString noHomeSave = directory.filePath(
        QStringLiteral("no-home.waypoints"));
    QVERIFY(!viewModel.SaveFile(noHomeSave));
    QVERIFY(!QFile::exists(noHomeSave));
    QVERIFY(viewModel.Status().contains(QStringLiteral("Home")));

    viewModel.SetHomeFromVehicle(10.0, 20.0, 30.0);
    const QString noHomeLoad = directory.filePath(
        QStringLiteral("no-home-input.waypoints"));
    QFile noHomeFile(noHomeLoad);
    QVERIFY(noHomeFile.open(QIODevice::WriteOnly | QIODevice::Text));
    noHomeFile.write("QGC WPL 110\n"
                     "0\t1\t3\t16\t0\t0\t0\t0\t41\t29\t60\t1\n");
    noHomeFile.close();

    QVERIFY(viewModel.LoadFile(noHomeLoad));
    QVERIFY(!viewModel.HomeValid());
    QCOMPARE(viewModel.HomeLat(), 0.0);
    QCOMPARE(viewModel.HomeLng(), 0.0);
    QCOMPARE(viewModel.HomeAlt(), 0.0);
    QCOMPARE(viewModel.Waypoints()->rowCount(), 1);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Lat(), 41.0);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Lng(), 29.0);
}

void FlightPlannerViewModelTest::transferAndVehicleBindingsAreUnavailableByDefault()
{
    FlightPlannerViewModel viewModel;
    QVERIFY(!viewModel.TransferBusy());
    QCOMPARE(viewModel.TransferProgress(), 0);
    QVERIFY(!viewModel.CanReadWaypoints());
    QVERIFY(!viewModel.CanWriteWaypoints());
    QVERIFY(!viewModel.CanCancelTransfer());
    QVERIFY(!viewModel.CanSetHomeFromVehicle());

    viewModel.ReadWaypoints();
    QVERIFY(viewModel.Status().startsWith(QStringLiteral("Read failed")));
    viewModel.WriteWaypoints();
    QVERIFY(viewModel.Status().startsWith(QStringLiteral("Write failed")));

    viewModel.setVehicleHomeProvider(
            [](double *latitude, double *longitude, double *altitudeAsl) {
        *latitude = 40.0;
        *longitude = 28.0;
        *altitudeAsl = 125.0;
        return true;
    });
    QVERIFY(viewModel.CanSetHomeFromVehicle());
    viewModel.SetHomeFromVehicle();
    QVERIFY(viewModel.HomeValid());
    QCOMPARE(viewModel.HomeLat(), 40.0);
    QCOMPARE(viewModel.HomeLng(), 28.0);
    QCOMPARE(viewModel.HomeAlt(), 125.0);
}

void FlightPlannerViewModelTest::missionTransfersRoundTripThroughBoundController()
{
    MissionProtocolCoordinator coordinator;
    PlannerFakeTransport transport;
    MissionTransferController controller(
            &coordinator, &transport, 60000, 2);
    FlightPlannerViewModel viewModel;
    QMap<QString, double> vehicleParameters{
        {QStringLiteral("WP_RADIUS_M"), 42.0},
    };
    QMap<QString, double> parameterWrites;
    viewModel.setVehicleParameterAccess(
        [&vehicleParameters](const QString &name, double *value) {
            const auto found = vehicleParameters.constFind(name);
            if (found == vehicleParameters.constEnd() || !value) return false;
            *value = found.value();
            return true;
        },
        [&parameterWrites](const QString &name, double value) {
            parameterWrites.insert(name, value);
            return true;
        });
    viewModel.setWpRadius(33.0);
    viewModel.setMissionTransferController(&controller);
    QVERIFY(viewModel.CanReadWaypoints());

    viewModel.ReadWaypoints();
    QVERIFY(viewModel.TransferBusy());
    QVERIFY(viewModel.CanCancelTransfer());
    transport.inject(plannerMissionCount(2));
    transport.inject(plannerMissionItem(0, 400000000, 280000000, 120.0f));
    transport.inject(plannerMissionItem(
            1, 401000000, 281000000, 75.0f,
            MAV_FRAME_GLOBAL_RELATIVE_ALT_INT));

    QVERIFY(!viewModel.TransferBusy());
    QVERIFY(viewModel.CanReadWaypoints());
    QCOMPARE(viewModel.TransferProgress(), 100);
    QVERIFY(viewModel.HomeValid());
    QCOMPARE(viewModel.HomeLat(), 40.0);
    QCOMPARE(viewModel.HomeLng(), 28.0);
    QCOMPARE(viewModel.HomeAlt(), 120.0);
    QCOMPARE(viewModel.Waypoints()->rowCount(), 1);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Lat(), 40.1);
    QVERIFY(viewModel.Status().startsWith(QStringLiteral("Read 1")));

    transport.sent.clear();
    viewModel.WriteWaypoints();
    QVERIFY(viewModel.TransferBusy());
    QCOMPARE(quint32(transport.sent.last().msgid),
             quint32(MAVLINK_MSG_ID_MISSION_COUNT));
    mavlink_mission_count_t count{};
    mavlink_msg_mission_count_decode(&transport.sent.last(), &count);
    QCOMPARE(count.count, quint16(2));

    transport.inject(plannerMissionRequest(0));
    mavlink_mission_item_int_t home{};
    mavlink_msg_mission_item_int_decode(&transport.sent.last(), &home);
    QCOMPARE(home.seq, quint16(0));
    QCOMPARE(home.current, quint8(1));
    QCOMPARE(home.x, qint32(400000000));

    transport.inject(plannerMissionRequest(1));
    mavlink_mission_item_int_t waypoint{};
    mavlink_msg_mission_item_int_decode(&transport.sent.last(), &waypoint);
    QCOMPARE(waypoint.seq, quint16(1));
    QCOMPARE(waypoint.current, quint8(0));
    transport.inject(plannerMissionAck());

    QVERIFY(!viewModel.TransferBusy());
    QVERIFY(viewModel.CanWriteWaypoints());
    QVERIFY(viewModel.Status().startsWith(QStringLiteral("Wrote 1")));
    QCOMPARE(parameterWrites.value(QStringLiteral("WP_RADIUS_M")), 33.0);
}

void FlightPlannerViewModelTest::navigationParametersFollowArduPilot47Aliases()
{
    QMap<QString, double> parameters{
        {QStringLiteral("WP_ACC"), 1.5},
        {QStringLiteral("WP_SPD"), 7.0},
        {QStringLiteral("WP_RADIUS"), 12.0},
        {QStringLiteral("WPNAV_RADIUS"), 2500.0},
        {QStringLiteral("WP_RADIUS_M"), 42.0},
    };
    QMap<QString, double> writes;
    FlightPlannerViewModel viewModel;
    viewModel.setVehicleParameterAccess(
        [&parameters](const QString &name, double *value) {
            const auto found = parameters.constFind(name);
            if (found == parameters.constEnd() || !value) return false;
            *value = found.value();
            return true;
        },
        [&writes](const QString &name, double value) {
            writes.insert(name, value);
            return true;
        });

    QCOMPARE(viewModel.WpAccelerationCms(), 150.0);
    QCOMPARE(viewModel.WpSpeedCms(), 700.0);
    QCOMPARE(viewModel.WpRadius(), 42.0);
    viewModel.setWpRadius(55.0);
    QCOMPARE(viewModel.WriteRadiusParams(), 3);
    QCOMPARE(writes.value(QStringLiteral("WP_RADIUS")), 55.0);
    QCOMPARE(writes.value(QStringLiteral("WP_RADIUS_M")), 55.0);
    QCOMPARE(writes.value(QStringLiteral("WPNAV_RADIUS")), 5500.0);

    parameters.insert(QStringLiteral("WPNAV_ACCEL"), 225.0);
    parameters.insert(QStringLiteral("WPNAV_SPEED"), 850.0);
    viewModel.UpdateVehicleParameter(QStringLiteral("WPNAV_ACCEL"), 225.0);
    viewModel.UpdateVehicleParameter(QStringLiteral("WPNAV_SPEED"), 850.0);
    QCOMPARE(viewModel.WpAccelerationCms(), 225.0);
    QCOMPARE(viewModel.WpSpeedCms(), 850.0);
}

void FlightPlannerViewModelTest::surveyBoundaryAndPlanAppendUseMissionStore()
{
    FlightPlannerViewModel viewModel;
    viewModel.AddWaypointAt(40.0, 28.0, 60.0);
    viewModel.AddWaypointAt(40.1, 28.0, 60.0);
    viewModel.AddWaypointAt(40.1, 28.1, 60.0);
    WpRowData nonNavigation;
    nonNavigation.Command = MAV_CMD_DO_CHANGE_SPEED;
    nonNavigation.P2 = 8.0;
    viewModel.Waypoints()->appendRow(nonNavigation);

    const QVector<SurveyGridCoordinate> boundary =
        viewModel.SurveyBoundary(125.0);
    QCOMPARE(boundary.size(), 3);
    QCOMPARE(boundary.first().altitude, 125.0);

    QVERIFY(viewModel.AddPolygonPoint(41.0, 29.0));
    QVERIFY(viewModel.AddPolygonPoint(41.0, 29.1));
    QVERIFY(viewModel.AddPolygonPoint(41.1, 29.1));
    QCOMPARE(viewModel.DrawnPolygon()->Count(), 3);
    const QVector<SurveyGridCoordinate> drawnBoundary =
        viewModel.SurveyBoundary(150.0);
    QCOMPARE(drawnBoundary.size(), 3);
    QCOMPARE(drawnBoundary.first().latitude, 41.0);
    QCOMPARE(drawnBoundary.first().altitude, 150.0);

    QVERIFY(viewModel.AddDrawnPolygonToFence(false));
    QCOMPARE(viewModel.MissionType(), QStringLiteral("Fence"));
    const QVector<WpRowData> fence = viewModel.Waypoints()->rows(
        FlightPlannerMissionModel::MissionStore::Fence);
    QCOMPARE(fence.size(), 3);
    QCOMPARE(fence.first().Command,
             quint16(MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION));
    QCOMPARE(fence.first().P1, 3.0);
    QCOMPARE(fence.first().Frame, quint8(MAV_FRAME_GLOBAL));

    SurveyMissionPlan plan;
    plan.success = true;
    plan.navigationCount = 1;
    plan.cameraCommandCount = 1;
    plan.jumpTargetsAreRelative = true;
    WpRowData jump;
    jump.Command = MAV_CMD_DO_JUMP;
    jump.P1 = 2.0;
    WpRowData camera;
    camera.Command = MAV_CMD_DO_SET_CAM_TRIGG_DIST;
    camera.P1 = 25.0;
    plan.commands = {jump, camera};

    viewModel.setMissionType(QStringLiteral("Rally"));
    QVERIFY(viewModel.AppendSurveyPlan(plan));
    QCOMPARE(viewModel.MissionType(), QStringLiteral("Mission"));
    QCOMPARE(viewModel.Waypoints()->rowCount(), 6);
    QCOMPARE(viewModel.Waypoints()->rowAt(4)->Command(),
             quint16(MAV_CMD_DO_JUMP));
    QCOMPARE(viewModel.Waypoints()->rowAt(4)->P1(), 6.0);
    QVERIFY(viewModel.Status().contains(QStringLiteral("Survey added")));
}

void FlightPlannerViewModelTest::qgcPlanAppendRemapsJumpTargetsAndKeepsHome()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("jump.plan"));

    MissionPlanner::QgcPlanFileCodec::PlanData plan;
    plan.Home = {1.0, 2.0, 3.0};
    WpRowData waypoint;
    waypoint.Seq = 0;
    waypoint.Command = MAV_CMD_NAV_WAYPOINT;
    waypoint.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    waypoint.Lat = 41.0;
    waypoint.Lng = 29.0;
    waypoint.Alt = 75.0;
    WpRowData jump;
    jump.Seq = 1;
    jump.Command = MAV_CMD_DO_JUMP;
    jump.Frame = MAV_FRAME_MISSION;
    // QGC/MAVLink mission indices include Home at zero. Target one is the
    // first command from this imported block.
    jump.P1 = 1.0;
    jump.P2 = 2.0;
    plan.Mission = {waypoint, jump};
    const auto saved = MissionPlanner::QgcPlanFileCodec::SavePlan(path, plan);
    QVERIFY2(saved.ok, qPrintable(saved.error));

    FlightPlannerViewModel viewModel;
    viewModel.SetHomeFromVehicle(40.0, 28.0, 100.0);
    QVERIFY(viewModel.AddWaypointAt(40.1, 28.1, 60.0));
    QVERIFY(viewModel.AddWaypointAt(40.2, 28.2, 65.0));
    QVERIFY(viewModel.LoadPlanFile(path, true));

    QCOMPARE(viewModel.HomeLat(), 40.0);
    QCOMPARE(viewModel.HomeLng(), 28.0);
    QCOMPARE(viewModel.HomeAlt(), 100.0);
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Mission), 4);
    const WpRow *appendedJump = viewModel.Waypoints()->rowAt(3);
    QVERIFY(appendedJump);
    QCOMPARE(appendedJump->Command(), quint16(MAV_CMD_DO_JUMP));
    QCOMPARE(appendedJump->P1(), 3.0);
}

void FlightPlannerViewModelTest::missionActionCommandsAndSetHomeValidation()
{
    FlightPlannerViewModel viewModel;
    QVERIFY(viewModel.SetHome(40.0, 28.0));
    QVERIFY(viewModel.HomeValid());
    QCOMPARE(viewModel.HomeAlt(), 0.0);
    viewModel.setHomeAlt(123.0);
    QVERIFY(viewModel.SetHome(41.0, 29.0));
    QCOMPARE(viewModel.HomeLat(), 41.0);
    QCOMPARE(viewModel.HomeLng(), 29.0);
    QCOMPARE(viewModel.HomeAlt(), 123.0);
    QVERIFY(!viewModel.SetHome(
        std::numeric_limits<double>::quiet_NaN(), 29.0));
    QCOMPARE(viewModel.HomeLat(), 41.0);

    QVERIFY(viewModel.AddTakeoff(41.1, 29.1, 25.0));
    QVERIFY(viewModel.AddLand(41.2, 29.2));
    QVERIFY(viewModel.AddRtl());
    QVERIFY(viewModel.AddRoi(41.3, 29.3));
    QVERIFY(viewModel.AddLoiterForever(41.4, 29.4));
    QCOMPARE(viewModel.Waypoints()->rowCount(), 5);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Command(),
             quint16(MAV_CMD_NAV_TAKEOFF));
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Alt(), 25.0);
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Command(),
             quint16(MAV_CMD_NAV_LAND));
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Alt(), 0.0);
    QCOMPARE(viewModel.Waypoints()->rowAt(2)->Command(),
             quint16(MAV_CMD_NAV_RETURN_TO_LAUNCH));
    QCOMPARE(viewModel.Waypoints()->rowAt(3)->Command(),
             quint16(MAV_CMD_DO_SET_ROI));
    QCOMPARE(viewModel.Waypoints()->rowAt(3)->Alt(),
             viewModel.DefaultAltitude());
    QCOMPARE(viewModel.Waypoints()->rowAt(4)->Command(),
             quint16(MAV_CMD_NAV_LOITER_UNLIM));

    const int beforeInvalid = viewModel.Waypoints()->rowCount();
    QVERIFY(!viewModel.AddLand(100.0, 29.0));
    QCOMPARE(viewModel.Waypoints()->rowCount(), beforeInvalid);
    viewModel.setMissionType(QStringLiteral("Fence"));
    QVERIFY(!viewModel.AddRtl());
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Mission),
             beforeInvalid);

    FlightPlannerViewModel mapActions;
    QVERIFY(mapActions.AddWaypointAt(40.0, 28.0, 50.0));
    QVERIFY(mapActions.AddWaypointAt(41.0, 29.0, 60.0));
    WpRowData nonNavigation;
    nonNavigation.Command = MAV_CMD_DO_CHANGE_SPEED;
    nonNavigation.Frame = MAV_FRAME_MISSION;
    nonNavigation.Lat = 41.00001;
    nonNavigation.Lng = 29.00001;
    mapActions.Waypoints()->appendRow(nonNavigation);
    QVERIFY(mapActions.MoveWaypoint(0, 40.2, 28.2));
    QCOMPARE(mapActions.Waypoints()->rowAt(0)->Lat(), 40.2);
    QVERIFY(mapActions.Undo());
    QCOMPARE(mapActions.Waypoints()->rowAt(0)->Lat(), 40.0);
    // Context-menu deletion targets the map marker hit by the user. It must
    // never infer a nearest waypoint from an empty-map click.
    QVERIFY(mapActions.DeleteWaypoint(1));
    QCOMPARE(mapActions.Waypoints()->rowCount(), 2);
    QCOMPARE(mapActions.Waypoints()->rowAt(0)->Lat(), 40.0);
    QCOMPARE(mapActions.Waypoints()->rowAt(1)->Command(),
             quint16(MAV_CMD_DO_CHANGE_SPEED));
}

void FlightPlannerViewModelTest::
setFenceReturnRepairsDuplicatesAndIsStoreAwareUndoable()
{
    FlightPlannerViewModel viewModel;
    viewModel.SetHomeFromVehicle(40.0, 28.0, 100.0);
    QVERIFY(viewModel.AddWaypointAt(40.1, 28.1, 50.0));

    QVector<WpRowData> fenceRows;
    for (int index = 0; index < 3; ++index) {
        WpRowData polygon;
        polygon.Command = MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION;
        polygon.Frame = MAV_FRAME_GLOBAL;
        polygon.P1 = 3.0;
        polygon.P2 = index + 0.25;
        polygon.P3 = index + 0.5;
        polygon.P4 = index + 0.75;
        polygon.Lat = 41.0 + (index == 1 ? 0.1 : 0.0);
        polygon.Lng = 29.0 + (index == 2 ? 0.1 : 0.0);
        fenceRows.append(polygon);
    }
    WpRowData oldReturn1;
    oldReturn1.Command = MAV_CMD_NAV_FENCE_RETURN_POINT;
    oldReturn1.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    oldReturn1.Lat = 41.01;
    oldReturn1.Lng = 29.01;
    oldReturn1.Alt = 12.0;
    WpRowData oldReturn2 = oldReturn1;
    oldReturn2.Lat = 41.02;
    oldReturn2.Lng = 29.02;
    fenceRows.insert(1, oldReturn1);
    fenceRows.append(oldReturn2);
    viewModel.Waypoints()->replaceStore(
        FlightPlannerMissionModel::MissionStore::Fence, fenceRows);
    const QVector<WpRowData> originalFence = viewModel.Waypoints()->rows(
        FlightPlannerMissionModel::MissionStore::Fence);

    WpRowData rally;
    rally.Command = MAV_CMD_NAV_RALLY_POINT;
    rally.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    rally.Lat = 42.0;
    rally.Lng = 30.0;
    rally.Alt = 75.0;
    viewModel.Waypoints()->replaceStore(
        FlightPlannerMissionModel::MissionStore::Rally, {rally});
    viewModel.setMissionType(QStringLiteral("Rally"));

    QVERIFY(viewModel.SetFenceReturn(41.5, 29.5));
    QCOMPARE(viewModel.MissionType(), QStringLiteral("Fence"));
    const QVector<WpRowData> repaired = viewModel.Waypoints()->rows(
        FlightPlannerMissionModel::MissionStore::Fence);
    QCOMPARE(repaired.size(), 4);
    int returnCount = 0;
    for (const WpRowData &row : repaired) {
        if (row.Command != MAV_CMD_NAV_FENCE_RETURN_POINT) {
            continue;
        }
        ++returnCount;
        QCOMPARE(row.Frame, quint8(MAV_FRAME_GLOBAL));
        QCOMPARE(row.Lat, 41.5);
        QCOMPARE(row.Lng, 29.5);
        QCOMPARE(row.Alt, 0.0);
        QCOMPARE(row.P1, 0.0);
        QCOMPARE(row.P2, 0.0);
        QCOMPARE(row.P3, 0.0);
        QCOMPARE(row.P4, 0.0);
    }
    QCOMPARE(returnCount, 1);
    QVector<WpRowData> originalGeometry;
    for (const WpRowData &row : originalFence) {
        if (row.Command != MAV_CMD_NAV_FENCE_RETURN_POINT) {
            originalGeometry.append(row);
        }
    }
    QCOMPARE(originalGeometry.size(), 3);
    for (int index = 0; index < originalGeometry.size(); ++index) {
        const WpRowData &actual = repaired.at(index);
        const WpRowData &expected = originalGeometry.at(index);
        QCOMPARE(actual.Seq, index);
        QCOMPARE(actual.Command, expected.Command);
        QCOMPARE(actual.Frame, expected.Frame);
        QCOMPARE(actual.P1, expected.P1);
        QCOMPARE(actual.P2, expected.P2);
        QCOMPARE(actual.P3, expected.P3);
        QCOMPARE(actual.P4, expected.P4);
        QCOMPARE(actual.Lat, expected.Lat);
        QCOMPARE(actual.Lng, expected.Lng);
        QCOMPARE(actual.Alt, expected.Alt);
    }
    QCOMPARE(repaired.last().Seq, 3);
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Mission), 1);
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Rally), 1);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString planPath = directory.filePath(QStringLiteral("return.plan"));
    QVERIFY(viewModel.SavePlanFile(planPath));
    QFile planFile(planPath);
    QVERIFY(planFile.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(
        planFile.readAll()).object();
    const QJsonArray breachReturn = root.value(QStringLiteral("geoFence"))
        .toObject().value(QStringLiteral("breachReturn")).toArray();
    QCOMPARE(breachReturn.size(), 3);
    QCOMPARE(breachReturn.at(0).toDouble(), 41.5);
    QCOMPARE(breachReturn.at(1).toDouble(), 29.5);
    QCOMPARE(breachReturn.at(2).toDouble(), 0.0);

    FlightPlannerViewModel loaded;
    QVERIFY(loaded.LoadPlanFile(planPath));
    const QVector<WpRowData> loadedFence = loaded.Waypoints()->rows(
        FlightPlannerMissionModel::MissionStore::Fence);
    int loadedReturnCount = 0;
    for (const WpRowData &row : loadedFence) {
        if (row.Command == MAV_CMD_NAV_FENCE_RETURN_POINT) {
            ++loadedReturnCount;
            QCOMPARE(row.Frame, quint8(MAV_FRAME_GLOBAL));
            QCOMPARE(row.Lat, 41.5);
            QCOMPARE(row.Lng, 29.5);
            QCOMPARE(row.Alt, 0.0);
        }
    }
    QCOMPARE(loadedReturnCount, 1);

    root.remove(QStringLiteral("apmPlanner"));
    const auto standardOnly = MissionPlanner::QgcPlanFileCodec::DecodePlan(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
    QVERIFY2(standardOnly.ok, qPrintable(standardOnly.error));
    QVERIFY(!standardOnly.plan.Fence.isEmpty());
    const WpRowData standardReturn = standardOnly.plan.Fence.first();
    QCOMPARE(standardReturn.Seq, 0);
    QCOMPARE(standardReturn.Command,
             quint16(MAV_CMD_NAV_FENCE_RETURN_POINT));
    QCOMPARE(standardReturn.Frame,
             quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QCOMPARE(standardReturn.Lat, 41.5);
    QCOMPARE(standardReturn.Lng, 29.5);
    QCOMPARE(standardReturn.Alt, 0.0);
    QCOMPARE(standardReturn.P1, 0.0);
    QCOMPARE(standardReturn.P2, 0.0);
    QCOMPARE(standardReturn.P3, 0.0);
    QCOMPARE(standardReturn.P4, 0.0);

    QVERIFY(viewModel.Undo());
    QCOMPARE(viewModel.MissionType(), QStringLiteral("Rally"));
    const QVector<WpRowData> restoredFence = viewModel.Waypoints()->rows(
        FlightPlannerMissionModel::MissionStore::Fence);
    QCOMPARE(restoredFence.size(), originalFence.size());
    for (int index = 0; index < restoredFence.size(); ++index) {
        QCOMPARE(restoredFence.at(index).Command,
                 originalFence.at(index).Command);
        QCOMPARE(restoredFence.at(index).Frame,
                 originalFence.at(index).Frame);
        QCOMPARE(restoredFence.at(index).P1, originalFence.at(index).P1);
        QCOMPARE(restoredFence.at(index).Lat, originalFence.at(index).Lat);
        QCOMPARE(restoredFence.at(index).Lng, originalFence.at(index).Lng);
        QCOMPARE(restoredFence.at(index).Alt, originalFence.at(index).Alt);
    }
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Mission), 1);
    QVERIFY(viewModel.Undo());
    QCOMPARE(viewModel.MissionType(), QStringLiteral("Mission"));
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Mission), 0);
    QVERIFY(viewModel.HomeValid());
    QCOMPARE(viewModel.HomeLat(), 40.0);
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Rally), 1);

    FlightPlannerViewModel rejected;
    rejected.setMissionType(QStringLiteral("Fence"));
    QVERIFY(!rejected.CanUndo());
    QVERIFY(!rejected.SetFenceReturn(
        std::numeric_limits<double>::quiet_NaN(), 29.0));
    QVERIFY(!rejected.SetFenceReturn(91.0, 29.0));
    QVERIFY(!rejected.SetFenceReturn(41.0, 181.0));
    QVERIFY(!rejected.CanUndo());
    QCOMPARE(rejected.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Fence), 0);

    MissionProtocolCoordinator coordinator;
    PlannerFakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 2);
    rejected.setMissionTransferController(&controller);
    rejected.ReadWaypoints();
    QVERIFY(rejected.TransferBusy());
    QVERIFY(!rejected.SetFenceReturn(41.0, 29.0));
    QVERIFY(!rejected.CanUndo());
    rejected.CancelTransfer();
}

void FlightPlannerViewModelTest::explicitContextCommandsMatchMissionPlannerParameters()
{
    FlightPlannerViewModel viewModel;
    viewModel.SetHomeFromVehicle(40.0, 28.0, 100.0);
    viewModel.setDefaultAltitude(75.0);
    viewModel.setSplineDefault(true);

    QVERIFY(viewModel.InsertRegularWaypointAt(0, 40.1, 28.1, 75.0));
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Command(),
             quint16(MAV_CMD_NAV_WAYPOINT));
    QVERIFY(viewModel.InsertSplineWaypointAt(1, 40.2, 28.2, 80.0));
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Command(),
             quint16(MAV_CMD_NAV_SPLINE_WAYPOINT));

    QVERIFY(viewModel.AddLoiterTime(40.3, 28.3, 5.0));
    const WpRow *loiterTime = viewModel.Waypoints()->rowAt(2);
    QVERIFY(loiterTime);
    QCOMPARE(loiterTime->Command(), quint16(MAV_CMD_NAV_LOITER_TIME));
    QCOMPARE(loiterTime->P1(), 5.0);
    QCOMPARE(loiterTime->Alt(), 75.0);

    QVERIFY(viewModel.AddLoiterTurns(40.4, 28.4, 3.0));
    const WpRow *loiterTurns = viewModel.Waypoints()->rowAt(3);
    QVERIFY(loiterTurns);
    QCOMPARE(loiterTurns->Command(), quint16(MAV_CMD_NAV_LOITER_TURNS));
    QCOMPARE(loiterTurns->P1(), 3.0);

    QVERIFY(viewModel.AddJump(1, 5));
    const WpRow *jumpStart = viewModel.Waypoints()->rowAt(4);
    QVERIFY(jumpStart);
    QCOMPARE(jumpStart->Command(), quint16(MAV_CMD_DO_JUMP));
    QCOMPARE(jumpStart->Frame(), quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QCOMPARE(jumpStart->P1(), 1.0);
    QCOMPARE(jumpStart->P2(), 5.0);

    QVERIFY(viewModel.AddJump(2, -1));
    const WpRow *jumpWaypoint = viewModel.Waypoints()->rowAt(5);
    QVERIFY(jumpWaypoint);
    QCOMPARE(jumpWaypoint->P1(), 2.0);
    QCOMPARE(jumpWaypoint->P2(), -1.0);

    const int rowCount = viewModel.Waypoints()->rowCount();
    QVERIFY(!viewModel.AddLoiterTime(40.5, 28.5, -0.1));
    QVERIFY(!viewModel.AddLoiterTurns(40.5, 28.5, 0.0));
    QVERIFY(!viewModel.AddJump(0, 5));
    QVERIFY(!viewModel.AddJump(rowCount + 2, 5));
    QVERIFY(!viewModel.AddJump(1, -2));
    QVERIFY(!viewModel.AddJump(1, 32768));
    QCOMPARE(viewModel.Waypoints()->rowCount(), rowCount);

    QVERIFY(viewModel.Undo());
    QCOMPARE(viewModel.Waypoints()->rowCount(), rowCount - 1);

    viewModel.setMissionType(QStringLiteral("Fence"));
    QVERIFY(!viewModel.AddJump(1, 5));
    QVERIFY(!viewModel.AddLoiterTime(40.5, 28.5, 5.0));
}

void FlightPlannerViewModelTest::currentVehiclePositionMatchesMissionPlannerContract()
{
    FlightPlannerViewModel viewModel;
    QVERIFY(!viewModel.AddWaypointAtCurrentPosition());
    QCOMPARE(viewModel.Waypoints()->rowCount(), 0);
    QVERIFY(viewModel.Status().contains(QStringLiteral("no vehicle")));

    int positionRequests = 0;
    viewModel.setVehiclePositionProvider(
        [&positionRequests](double *latitude, double *longitude,
                            double *altitudeRelative) {
            ++positionRequests;
            *latitude = 40.1234567;
            *longitude = 28.7654321;
            *altitudeRelative = 42.75;
            return true;
        });
    QVERIFY(viewModel.AddWaypointAtCurrentPosition());
    QCOMPARE(positionRequests, 1);
    const WpRow *regular = viewModel.Waypoints()->rowAt(0);
    QVERIFY(regular);
    QCOMPARE(regular->Command(), quint16(MAV_CMD_NAV_WAYPOINT));
    QCOMPARE(regular->Frame(), quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QCOMPARE(regular->Lat(), 40.1234567);
    QCOMPARE(regular->Lng(), 28.7654321);
    QCOMPARE(regular->Alt(), 42.0);

    viewModel.setSplineDefault(true);
    viewModel.setDefaultFrame(QStringLiteral("Absolute"));
    QVERIFY(viewModel.AddWaypointAtCurrentPosition());
    const WpRow *spline = viewModel.Waypoints()->rowAt(1);
    QVERIFY(spline);
    QCOMPARE(spline->Command(), quint16(MAV_CMD_NAV_SPLINE_WAYPOINT));
    QCOMPARE(spline->Frame(), quint8(MAV_FRAME_GLOBAL));
    QCOMPARE(spline->Alt(), 42.0);

    viewModel.setVehiclePositionProvider(
        [](double *latitude, double *longitude, double *altitudeRelative) {
            *latitude = 40.1234567;
            *longitude = 28.7654321;
            *altitudeRelative = 0.4;
            return true;
        });
    viewModel.setDefaultAltitude(61.0);
    QVERIFY(viewModel.AddWaypointAtCurrentPosition());
    QCOMPARE(viewModel.Waypoints()->rowAt(2)->Alt(), 61.0);

    viewModel.SetHomeFromVehicle(40.0, 28.0, 100.0);
    viewModel.setSplineDefault(false);
    viewModel.setDefaultFrame(QStringLiteral("Relative"));
    viewModel.setDefaultAltitude(75.0);
    viewModel.setVerifyHeight(true);
    viewModel.setTerrainAltitudeProvider(
        [](double latitude, double, double *altitude) {
            *altitude = latitude == 40.0 ? 100.0 : 125.0;
            return true;
        });
    QVERIFY(viewModel.AddWaypointAtCurrentPosition());
    const WpRow *verified = viewModel.Waypoints()->rowAt(3);
    QVERIFY(verified);
    QCOMPARE(verified->Alt(), 100.0);

    const int beforeInvalid = viewModel.Waypoints()->rowCount();
    viewModel.setVehiclePositionProvider(
        [](double *latitude, double *longitude, double *altitudeRelative) {
            *latitude = std::numeric_limits<double>::quiet_NaN();
            *longitude = 28.0;
            *altitudeRelative = 10.0;
            return true;
        });
    QVERIFY(!viewModel.AddWaypointAtCurrentPosition());
    QCOMPARE(viewModel.Waypoints()->rowCount(), beforeInvalid);
    QVERIFY(viewModel.Undo());
    QCOMPARE(viewModel.Waypoints()->rowCount(), beforeInvalid - 1);
}

void FlightPlannerViewModelTest::insertReverseAndMoveKeepJumpTargetsValid()
{
    FlightPlannerViewModel insertion;
    QVERIFY(insertion.AddWaypointAt(40.0, 28.0, 50.0));
    QVERIFY(insertion.AddWaypointAt(40.1, 28.1, 60.0));
    QVERIFY(insertion.AddJump(2, 2));
    QVERIFY(insertion.InsertRegularWaypointAt(1, 40.05, 28.05, 55.0));
    QCOMPARE(insertion.Waypoints()->rowAt(3)->P1(), 3.0);
    QVERIFY(insertion.Undo());
    QCOMPARE(insertion.Waypoints()->rowCount(), 3);
    QCOMPARE(insertion.Waypoints()->rowAt(2)->P1(), 2.0);

    FlightPlannerViewModel viewModel;
    QVERIFY(viewModel.AddWaypointAt(40.0, 28.0, 50.0));
    QVERIFY(viewModel.AddWaypointAt(40.1, 28.1, 60.0));
    QVERIFY(viewModel.AddWaypointAt(40.2, 28.2, 70.0));
    WpRowData jump;
    jump.Command = MAV_CMD_DO_JUMP;
    jump.Frame = MAV_FRAME_MISSION;
    jump.P1 = 1.0;
    jump.P2 = 2.0;
    viewModel.Waypoints()->appendRow(jump);

    QVERIFY(viewModel.InsertWaypointAt(1, 40.05, 28.05, 55.0));
    QCOMPARE(viewModel.Waypoints()->rowCount(), 5);
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Lat(), 40.05);
    QCOMPARE(viewModel.Waypoints()->rowAt(4)->P1(), 1.0);

    QVERIFY(viewModel.ReverseWaypoints());
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Command(),
             quint16(MAV_CMD_DO_JUMP));
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->P1(), 5.0);

    QVERIFY(viewModel.MoveWaypointDown(0));
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Command(),
             quint16(MAV_CMD_DO_JUMP));
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->P1(), 5.0);
    QVERIFY(viewModel.MoveWaypointUp(1));
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->P1(), 5.0);

    viewModel.Waypoints()->rowAt(0)->setP1(99.0);
    const QVector<WpRowData> before = viewModel.Waypoints()->rows(
        FlightPlannerMissionModel::MissionStore::Mission);
    QVERIFY(!viewModel.ReverseWaypoints());
    QCOMPARE(viewModel.Waypoints()->rowCount(), before.size());
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->P1(), 99.0);
    QVERIFY(viewModel.Status().contains(QStringLiteral("refused")));
}

void FlightPlannerViewModelTest::modifyAltitudesAndUndoKeepHomeSeparate()
{
    FlightPlannerViewModel viewModel;
    viewModel.SetHomeFromVehicle(40.0, 28.0, 321.0);
    QVERIFY(viewModel.AddWaypointAt(40.1, 28.1, 50.0));
    QVERIFY(viewModel.AddWaypointAt(40.2, 28.2, 75.0));
    QVERIFY(viewModel.CanUndo());

    QVERIFY(viewModel.ModifyAllAlt(QStringLiteral("+10")));
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Alt(), 60.0);
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Alt(), 85.0);
    QVERIFY(viewModel.Undo());
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Alt(), 50.0);
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Alt(), 75.0);
    QVERIFY(viewModel.HomeValid());
    QCOMPARE(viewModel.HomeLat(), 40.0);
    QCOMPARE(viewModel.HomeLng(), 28.0);
    QCOMPARE(viewModel.HomeAlt(), 321.0);

    QVERIFY(viewModel.ModifyAllAlt(QStringLiteral("*2")));
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Alt(), 100.0);
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Alt(), 150.0);
    QVERIFY(!viewModel.ModifyAllAlt(QStringLiteral("*invalid")));
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Alt(), 100.0);
    QVERIFY(viewModel.Undo());
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Alt(), 50.0);
    QCOMPARE(viewModel.HomeAlt(), 321.0);
}

void FlightPlannerViewModelTest::altitudeUnitsConvertPresentationOnly()
{
    constexpr double FeetPerMeter = 3.280839895013123;
    FlightPlannerViewModel viewModel;
    QCOMPARE(viewModel.AltitudeUnits(),
             QStringList({QStringLiteral("Meters"),
                          QStringLiteral("Feet")}));
    QCOMPARE(viewModel.AltUnits(), QStringLiteral("Meters"));
    QCOMPARE(viewModel.AltUnit(), QStringLiteral("m"));
    QCOMPARE(viewModel.HomeAltLabel(), QStringLiteral("m ASL"));

    viewModel.SetHomeFromVehicle(40.0, 28.0, 100.0);
    viewModel.setDefaultAltitude(75.0);
    viewModel.setAltWarn(20.0);
    QVERIFY(viewModel.AddWaypointAt(40.1, 28.1, 30.48));

    QSignalSpy unitsChanged(&viewModel,
                            &FlightPlannerViewModel::altUnitsChanged);
    viewModel.setAltUnits(QStringLiteral("feet"));
    QCOMPARE(unitsChanged.count(), 1);
    QCOMPARE(viewModel.AltUnits(), QStringLiteral("Feet"));
    QCOMPARE(viewModel.AltUnit(), QStringLiteral("ft"));
    QCOMPARE(viewModel.HomeAltLabel(), QStringLiteral("ft ASL"));
    QCOMPARE(viewModel.HomeAlt(), 100.0);
    QCOMPARE(viewModel.DefaultAltitude(), 75.0);
    QCOMPARE(viewModel.AltWarn(), 20.0);
    QVERIFY(std::abs(viewModel.HomeAltDisplay()
                     - 100.0 * FeetPerMeter) < 1e-9);
    QVERIFY(std::abs(viewModel.DefaultAltitudeDisplay()
                     - 75.0 * FeetPerMeter) < 1e-9);
    QVERIFY(std::abs(viewModel.AltWarnDisplay()
                     - 20.0 * FeetPerMeter) < 1e-9);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Alt(), 30.48);
    QVERIFY(std::abs(viewModel.Waypoints()->data(
        viewModel.Waypoints()->index(
            0, FlightPlannerMissionModel::AltColumn)).toDouble()
        - 100.0) < 1e-9);

    QVERIFY(viewModel.Waypoints()->setData(
        viewModel.Waypoints()->index(
            0, FlightPlannerMissionModel::AltColumn), 200.0));
    QVERIFY(std::abs(viewModel.Waypoints()->rowAt(0)->Alt() - 60.96)
            < 1e-9);
    viewModel.setHomeAltDisplay(328.0839895013123);
    viewModel.setDefaultAltitudeDisplay(246.06299212598424);
    viewModel.setAltWarnDisplay(65.61679790026247);
    QVERIFY(std::abs(viewModel.HomeAlt() - 100.0) < 1e-9);
    QVERIFY(std::abs(viewModel.DefaultAltitude() - 75.0) < 1e-9);
    QVERIFY(std::abs(viewModel.AltWarn() - 20.0) < 1e-9);

    QVERIFY(viewModel.ModifyAllAlt(QStringLiteral("+10")));
    QVERIFY(std::abs(viewModel.Waypoints()->rowAt(0)->Alt() - 64.008)
            < 1e-9);
    QVERIFY(viewModel.Status().endsWith(QStringLiteral("10 ft.")));
    QVERIFY(viewModel.Undo());
    QVERIFY(std::abs(viewModel.Waypoints()->rowAt(0)->Alt() - 60.96)
            < 1e-9);

    viewModel.setAltUnits(QStringLiteral("yards"));
    QCOMPARE(unitsChanged.count(), 1);
    QCOMPARE(viewModel.AltUnits(), QStringLiteral("Feet"));
}

void FlightPlannerViewModelTest::distanceUnitsConvertPresentationOnly()
{
    constexpr double FeetPerMeter = 3.280839895013123;
    FlightPlannerViewModel viewModel;
    QCOMPARE(viewModel.DistanceUnits(),
             QStringList({QStringLiteral("Meters"),
                          QStringLiteral("Feet")}));
    QCOMPARE(viewModel.DistUnits(), QStringLiteral("Meters"));
    QCOMPARE(viewModel.DistanceUnit(), QStringLiteral("m"));
    QCOMPARE(viewModel.DistanceMultiplier(), 1.0);

    viewModel.setWpRadius(30.48);
    viewModel.setLoiterRadius(-60.96);
    viewModel.SetHomeFromVehicle(0.0, 0.0, 0.0);
    QVERIFY(viewModel.AddWaypointAt(0.0, 0.001, 0.0));
    QVERIFY(viewModel.AddWaypointAt(0.001, 0.001, 0.0));
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Dist(),
             QStringLiteral("111.2"));
    QCOMPARE(viewModel.TotalDist(), QStringLiteral("0.1112 km"));
    QCOMPARE(viewModel.PrevDist(), QStringLiteral("111.19 m"));
    QCOMPARE(viewModel.HomeDist(), QStringLiteral("157.25 m"));

    QSignalSpy unitsChanged(&viewModel,
                            &FlightPlannerViewModel::distUnitsChanged);
    QSignalSpy presentationChanged(
        &viewModel, &FlightPlannerViewModel::distancePresentationChanged);
    viewModel.setDistUnits(QStringLiteral("feet"));

    QCOMPARE(unitsChanged.count(), 1);
    QCOMPARE(presentationChanged.count(), 1);
    QCOMPARE(viewModel.DistUnits(), QStringLiteral("Feet"));
    QCOMPARE(viewModel.DistanceUnit(), QStringLiteral("ft"));
    QCOMPARE(viewModel.DistanceMultiplier(), FeetPerMeter);
    QCOMPARE(viewModel.WpRadius(), 30.48);
    QCOMPARE(viewModel.LoiterRadius(), -60.96);
    QVERIFY(std::abs(viewModel.WpRadiusDisplay() - 100.0) < 1e-9);
    QVERIFY(std::abs(viewModel.LoiterRadiusDisplay() + 200.0) < 1e-9);
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Dist(),
             QStringLiteral("364.8"));
    QCOMPARE(viewModel.TotalDist(), QStringLiteral("0.0691 miles"));
    QCOMPARE(viewModel.PrevDist(), QStringLiteral("364.81 ft"));
    QCOMPARE(viewModel.HomeDist(), QStringLiteral("515.92 ft"));
    QCOMPARE(viewModel.Waypoints()->headerData(
                 FlightPlannerMissionModel::DistColumn,
                 Qt::Horizontal).toString(),
             QStringLiteral("Dist (ft)"));

    viewModel.setWpRadiusDisplay(200.0);
    viewModel.setLoiterRadiusDisplay(-100.0);
    QVERIFY(std::abs(viewModel.WpRadius() - 60.96) < 1e-9);
    QVERIFY(std::abs(viewModel.LoiterRadius() + 30.48) < 1e-9);

    viewModel.setDistUnits(QStringLiteral("yards"));
    QCOMPARE(unitsChanged.count(), 1);
    QCOMPARE(viewModel.DistUnits(), QStringLiteral("Feet"));
}

void FlightPlannerViewModelTest::elevationProfileUsesCanonicalMissionSnapshot()
{
    FlightPlannerViewModel viewModel;
    viewModel.setTerrainAltitudeProvider(
        [](double, double, double *altitudeAmslMeters) {
            *altitudeAmslMeters = 200.0;
            return true;
        });
    viewModel.SetHomeFromVehicle(47.0, 8.0, 500.0);
    QVERIFY(viewModel.AddWaypointAt(47.0, 8.001, 50.0));
    QVERIFY(viewModel.AddWaypointAt(47.0, 8.002, 60.0));
    WpRow *terrainWaypoint = viewModel.Waypoints()->rowAt(1);
    QVERIFY(terrainWaypoint);
    terrainWaypoint->setFrame(MAV_FRAME_GLOBAL_TERRAIN_ALT_INT);
    terrainWaypoint->setAlt(30.0);
    terrainWaypoint->setDist(QStringLiteral("not a domain value"));
    viewModel.setMissionType(QStringLiteral("Fence"));

    const MissionElevationProfileResult metric =
        viewModel.BuildElevationProfile(1000.0);
    QVERIFY(metric.hasRoute());
    QCOMPARE(metric.routePointCount, 3);
    QCOMPARE(metric.samples.size(), 3);
    QCOMPARE(metric.samples.at(0).plannedAltitudeAmslMeters, 500.0);
    QCOMPARE(metric.samples.at(1).plannedAltitudeAmslMeters, 550.0);
    QCOMPARE(metric.samples.at(2).plannedAltitudeAmslMeters, 230.0);

    viewModel.setAltUnits(QStringLiteral("Feet"));
    viewModel.setDistUnits(QStringLiteral("Feet"));
    const MissionElevationProfileResult imperial =
        viewModel.BuildElevationProfile(1000.0);
    QCOMPARE(imperial.samples.size(), metric.samples.size());
    QCOMPARE(imperial.totalDistanceMeters, metric.totalDistanceMeters);
    for (int index = 0; index < metric.samples.size(); ++index) {
        QCOMPARE(imperial.samples.at(index).distanceMeters,
                 metric.samples.at(index).distanceMeters);
        QCOMPARE(imperial.samples.at(index).terrainAltitudeAmslMeters,
                 metric.samples.at(index).terrainAltitudeAmslMeters);
        QCOMPARE(imperial.samples.at(index).plannedAltitudeAmslMeters,
                 metric.samples.at(index).plannedAltitudeAmslMeters);
    }
}

void FlightPlannerViewModelTest::missionActionsAreLockedDuringTransfer()
{
    MissionProtocolCoordinator coordinator;
    PlannerFakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 2);
    FlightPlannerViewModel viewModel;
    QVERIFY(viewModel.AddWaypointAt(40.0, 28.0, 50.0));
    viewModel.setMissionTransferController(&controller);
    viewModel.ReadWaypoints();
    QVERIFY(viewModel.TransferBusy());

    int terrainCalls = 0;
    int positionCalls = 0;
    viewModel.setTerrainAltitudeProvider(
        [&terrainCalls](double, double, double *altitude) {
            ++terrainCalls;
            *altitude = 100.0;
            return true;
        });
    viewModel.setVehiclePositionProvider(
        [&positionCalls](double *latitude, double *longitude,
                         double *altitudeRelative) {
            ++positionCalls;
            *latitude = 41.0;
            *longitude = 29.0;
            *altitudeRelative = 60.0;
            return true;
        });

    QVERIFY(!viewModel.SetHome(41.0, 29.0));
    QVERIFY(!viewModel.AddWaypointAt(41.0, 29.0));
    QVERIFY(!viewModel.AddWaypointAtCurrentPosition());
    QVERIFY(!viewModel.InsertWaypointAt(0, 41.0, 29.0, 60.0));
    QVERIFY(!viewModel.InsertRegularWaypointAt(0, 41.0, 29.0, 60.0));
    QVERIFY(!viewModel.InsertSplineWaypointAt(0, 41.0, 29.0, 60.0));
    QVERIFY(!viewModel.AddTakeoff(41.0, 29.0, 60.0));
    QVERIFY(!viewModel.AddLand(41.0, 29.0));
    QVERIFY(!viewModel.AddRtl());
    QVERIFY(!viewModel.AddRoi(41.0, 29.0));
    QVERIFY(!viewModel.AddLoiterForever(41.0, 29.0));
    QVERIFY(!viewModel.AddLoiterTime(41.0, 29.0, 5.0));
    QVERIFY(!viewModel.AddLoiterTurns(41.0, 29.0, 3.0));
    QVERIFY(!viewModel.AddJump(1, 5));
    QVERIFY(!viewModel.DeleteWaypoint(0));
    QVERIFY(!viewModel.MoveWaypoint(0, 41.0, 29.0));
    viewModel.ClearWaypoints();
    QVERIFY(!viewModel.ReverseWaypoints());
    QVERIFY(!viewModel.ModifyAllAlt(QStringLiteral("10")));
    QVERIFY(!viewModel.Undo());
    QCOMPARE(terrainCalls, 0);
    QCOMPARE(positionCalls, 0);
    QCOMPARE(viewModel.Waypoints()->rowCount(), 1);

    viewModel.CancelTransfer();
    QVERIFY(!viewModel.TransferBusy());
}

void FlightPlannerViewModelTest::plannerSettingsDriveRouteMetricsAndAltitudeWarning()
{
    FlightPlannerViewModel viewModel;
    QCOMPARE(viewModel.DefaultFrames(),
             QStringList({QStringLiteral("Relative"),
                          QStringLiteral("Absolute"),
                          QStringLiteral("Terrain")}));
    QCOMPARE(viewModel.DefaultFrame(), QStringLiteral("Relative"));
    QCOMPARE(viewModel.LoiterRadius(), 100.0);
    QCOMPARE(viewModel.AltWarn(), 0.0);
    QVERIFY(!viewModel.SplineDefault());
    QVERIFY(!viewModel.VerifyHeight());

    viewModel.setDefaultFrame(QStringLiteral("terrain"));
    viewModel.setSplineDefault(true);
    WpRow *spline = viewModel.AddWaypointAt(1.0, 1.0, 80.0);
    QVERIFY(spline);
    QCOMPARE(spline->Command(), quint16(MAV_CMD_NAV_SPLINE_WAYPOINT));
    QCOMPARE(spline->Frame(), quint8(MAV_FRAME_GLOBAL_TERRAIN_ALT));
    viewModel.ClearWaypoints();

    viewModel.setDefaultFrame(QStringLiteral("Relative"));
    viewModel.setSplineDefault(false);
    viewModel.SetHomeFromVehicle(0.0, 0.0, 0.0);
    QVERIFY(viewModel.AddWaypointAt(0.0, 0.001, 100.0));
    QVERIFY(viewModel.AddWaypointAt(0.001, 0.001, 100.0));
    QCOMPARE(viewModel.TotalDist(), QStringLiteral("0.1112 km"));
    QCOMPARE(viewModel.PrevDist(), QStringLiteral("111.19 m"));
    QCOMPARE(viewModel.HomeDist(), QStringLiteral("157.25 m"));
    QVERIFY(!viewModel.Waypoints()->rowAt(0)->Grad().isEmpty());
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Grad(), QStringLiteral("0.0"));
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Angle(), QStringLiteral("0.0"));
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Az(), QStringLiteral("0"));

    MissionProtocolCoordinator coordinator;
    PlannerFakeTransport transport;
    MissionTransferController controller(&coordinator, &transport, 60000, 2);
    viewModel.setMissionTransferController(&controller);
    viewModel.setAltWarn(101.0);
    viewModel.WriteWaypoints();
    QVERIFY(viewModel.Status().startsWith(QStringLiteral("Write blocked")));
    QVERIFY(transport.sent.isEmpty());
}

void FlightPlannerViewModelTest::terrainProviderMatchesMissionPlannerPlacementRules()
{
    FlightPlannerViewModel viewModel;
    int calls = 0;
    viewModel.setTerrainAltitudeProvider(
        [&calls](double latitude, double, double *altitude) {
            ++calls;
            *altitude = latitude * 10.0;
            return true;
        });

    QVERIFY(viewModel.SetHome(10.1234, 20.0));
    QCOMPARE(viewModel.HomeAlt(), 101.23);
    QVERIFY(calls > 0);

    viewModel.SetHomeFromVehicle(10.0, 20.0, 100.0);
    viewModel.setVerifyHeight(true);
    viewModel.setDefaultAltitude(100.0);
    viewModel.setDefaultFrame(QStringLiteral("Relative"));
    WpRow *relative = viewModel.AddWaypointAt(11.0, 20.0);
    QVERIFY(relative);
    QCOMPARE(relative->Alt(), 110.0);

    viewModel.setDefaultFrame(QStringLiteral("Absolute"));
    WpRow *absolute = viewModel.AddWaypointAt(12.0, 20.0);
    QVERIFY(absolute);
    QCOMPARE(absolute->Alt(), 220.0);

    viewModel.setDefaultFrame(QStringLiteral("Terrain"));
    WpRow *terrain = viewModel.AddWaypointAt(13.0, 20.0);
    QVERIFY(terrain);
    QCOMPARE(terrain->Alt(), 100.0);

    viewModel.setDefaultFrame(QStringLiteral("Relative"));
    QVERIFY(viewModel.AddRoi(14.0, 20.0));
    QCOMPARE(viewModel.Waypoints()->rowAt(3)->Alt(), 140.0);
    QVERIFY(viewModel.AddLoiterForever(15.0, 20.0));
    QCOMPARE(viewModel.Waypoints()->rowAt(4)->Alt(), 150.0);

    QVERIFY(viewModel.MoveWaypoint(0, 14.0, 20.0));
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Alt(), 140.0);

    const int beforeInvalid = calls;
    QVERIFY(!viewModel.AddWaypointAt(
        std::numeric_limits<double>::quiet_NaN(), 20.0));
    QCOMPARE(calls, beforeInvalid);

    viewModel.setMissionType(QStringLiteral("Fence"));
    const int beforeFence = calls;
    QVERIFY(viewModel.AddWaypointAt(15.0, 20.0));
    QCOMPARE(calls, beforeFence);

    viewModel.setTerrainAltitudeProvider(
        [](double, double, double *) { return false; });
    viewModel.setMissionType(QStringLiteral("Mission"));
    const double preserved = viewModel.HomeAlt();
    QVERIFY(viewModel.SetHome(16.0, 20.0));
    QCOMPARE(viewModel.HomeAlt(), preserved);

    viewModel.setTerrainAltitudeProvider(
        [](double, double, double *altitude) {
            *altitude = std::numeric_limits<double>::quiet_NaN();
            return true;
        });
    QVERIFY(viewModel.SetHome(17.0, 20.0));
    QCOMPARE(viewModel.HomeAlt(), preserved);

    FlightPlannerViewModel missingHomeTerrain;
    missingHomeTerrain.SetHomeFromVehicle(10.0, 20.0, 100.0);
    missingHomeTerrain.setVerifyHeight(true);
    missingHomeTerrain.setDefaultAltitude(80.0);
    missingHomeTerrain.setTerrainAltitudeProvider(
        [](double latitude, double, double *altitude) {
            if (latitude == 10.0) return false;
            *altitude = 250.0;
            return true;
        });
    WpRow *unverified = missingHomeTerrain.AddWaypointAt(11.0, 20.0);
    QVERIFY(unverified);
    QCOMPARE(unverified->Alt(), 80.0);
}

QTEST_MAIN(FlightPlannerViewModelTest)
#include "test_flightplannerviewmodel.moc"
