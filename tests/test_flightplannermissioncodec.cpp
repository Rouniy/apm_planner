#include "ui/flightplanner/FlightPlannerMissionCodec.h"

#include <QtTest/QTest>

#include <limits>

class FlightPlannerMissionCodecTest final : public QObject
{
    Q_OBJECT

private slots:
    void missionHomeAndSequenceRoundTrip();
    void fenceAndRallyDoNotUseHome();
    void coordinateScalingPreservesProtocolSemantics();
    void rejectsInvalidMissionAndFence();
};

void FlightPlannerMissionCodecTest::missionHomeAndSequenceRoundTrip()
{
    WpRowData first;
    first.Command = MAV_CMD_NAV_TAKEOFF;
    first.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    first.Lat = -35.363261;
    first.Lng = 149.165230;
    first.Alt = 80.0;
    WpRowData second = first;
    second.Command = MAV_CMD_NAV_WAYPOINT;
    second.Lat += 0.01;

    const auto encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Mission,
            {first, second}, true, -35.0, 149.0, 600.0);
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    QCOMPARE(encoded.items.size(), 3);
    QCOMPARE(encoded.items.at(0).seq, quint16(0));
    QCOMPARE(encoded.items.at(0).current, quint8(1));
    QCOMPARE(encoded.items.at(0).autocontinue, quint8(1));
    QCOMPARE(encoded.items.at(1).seq, quint16(1));
    QCOMPARE(encoded.items.at(1).current, quint8(0));
    QCOMPARE(encoded.items.at(2).seq, quint16(2));

    const auto decoded = FlightPlannerMissionCodec::decode(
            FlightPlannerMissionModel::MissionStore::Mission, encoded.items);
    QVERIFY2(decoded.ok, qPrintable(decoded.error));
    QVERIFY(decoded.homeValid);
    QCOMPARE(decoded.homeLatitude, -35.0);
    QCOMPARE(decoded.homeLongitude, 149.0);
    QCOMPARE(decoded.homeAltitude, 600.0);
    QCOMPARE(decoded.rows.size(), 2);
    QCOMPARE(decoded.rows.at(0).Seq, 0);
    QCOMPARE(decoded.rows.at(1).Seq, 1);
}

void FlightPlannerMissionCodecTest::fenceAndRallyDoNotUseHome()
{
    QVector<WpRowData> polygon(3);
    for (int index = 0; index < polygon.size(); ++index) {
        polygon[index].Command = MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION;
        polygon[index].Frame = MAV_FRAME_GLOBAL;
        polygon[index].P1 = 3;
        polygon[index].P2 = 7;
        polygon[index].Lat = 10.0 + index;
        polygon[index].Lng = 20.0 + index;
    }
    auto encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Fence, polygon);
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    QCOMPARE(encoded.items.size(), 3);
    QCOMPARE(encoded.items.first().seq, quint16(0));
    QCOMPARE(encoded.items.first().mission_type, quint8(MAV_MISSION_TYPE_FENCE));
    QCOMPARE(encoded.items.first().current, quint8(0));

    QVector<WpRowData> adjacentPolygons = polygon;
    adjacentPolygons += polygon;
    for (int index = 3; index < adjacentPolygons.size(); ++index) {
        adjacentPolygons[index].Lat += 20.0;
        adjacentPolygons[index].Lng += 20.0;
    }
    encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Fence,
            adjacentPolygons);
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    QCOMPARE(encoded.items.size(), 6);

    QVector<WpRowData> mapCreatedPolygon = polygon;
    for (WpRowData &vertex : mapCreatedPolygon)
        vertex.P1 = 0;
    encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Fence,
            mapCreatedPolygon);
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    QCOMPARE(encoded.items.size(), 3);
    for (const mavlink_mission_item_int_t &vertex : encoded.items)
        QCOMPARE(vertex.param1, 3.0f);

    WpRowData rally;
    rally.Command = MAV_CMD_NAV_RALLY_POINT;
    rally.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    rally.Lat = 12.5;
    rally.Lng = 42.5;
    rally.Alt = 90.0;
    encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Rally, {rally});
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    QCOMPARE(encoded.items.size(), 1);
    QCOMPARE(encoded.items.first().seq, quint16(0));
    QCOMPARE(encoded.items.first().mission_type, quint8(MAV_MISSION_TYPE_RALLY));
}

void FlightPlannerMissionCodecTest::coordinateScalingPreservesProtocolSemantics()
{
    WpRowData global;
    global.Command = MAV_CMD_NAV_WAYPOINT;
    global.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    global.Lat = -35.363261;
    global.Lng = 149.165230;
    global.Alt = 50.0;
    WpRowData local = global;
    local.Frame = MAV_FRAME_LOCAL_NED;
    local.Lat = 12.3456;
    local.Lng = -8.7654;
    WpRowData raw = global;
    raw.Command = MAV_CMD_DO_CHANGE_SPEED;
    raw.Lat = 37.0;
    raw.Lng = -9.0;
    WpRowData fixedMagYaw = global;
    fixedMagYaw.Command = MAV_CMD_FIXED_MAG_CAL_YAW;
    fixedMagYaw.Lat = 21.0;
    fixedMagYaw.Lng = -7.0;

    const auto encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Mission,
            {global, local, raw, fixedMagYaw}, true, 1.0, 2.0, 3.0);
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    QCOMPARE(encoded.items.at(1).x, qint32(-353632610));
    QCOMPARE(encoded.items.at(2).x, qint32(123456));
    QCOMPARE(encoded.items.at(2).y, qint32(-87654));
    QCOMPARE(encoded.items.at(3).x, qint32(37));
    QCOMPARE(encoded.items.at(3).y, qint32(-9));
    QCOMPARE(encoded.items.at(4).x, qint32(21));
    QCOMPARE(encoded.items.at(4).y, qint32(-7));

    const auto decoded = FlightPlannerMissionCodec::decode(
            FlightPlannerMissionModel::MissionStore::Mission, encoded.items);
    QVERIFY(decoded.ok);
    QCOMPARE(decoded.rows.at(2).Lat, 37.0);
    QCOMPARE(decoded.rows.at(2).Lng, -9.0);
    QCOMPARE(decoded.rows.at(3).Lat, 21.0);
    QCOMPARE(decoded.rows.at(3).Lng, -7.0);
}

void FlightPlannerMissionCodecTest::rejectsInvalidMissionAndFence()
{
    auto encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Mission, {},
            true, 1.0, 2.0, 3.0);
    QVERIFY(!encoded.ok);
    WpRowData mission;
    mission.Command = MAV_CMD_NAV_WAYPOINT;
    mission.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Mission, {mission});
    QVERIFY(!encoded.ok);

    WpRowData fence;
    fence.Command = MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION;
    fence.Frame = MAV_FRAME_GLOBAL;
    fence.P1 = 0;
    encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Fence,
            {fence, fence});
    QVERIFY(!encoded.ok);
    QVERIFY(encoded.error.contains(QStringLiteral("P1")));

    WpRowData unknownFence = fence;
    unknownFence.Command = MAV_CMD_NAV_WAYPOINT;
    encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Fence,
            {unknownFence});
    QVERIFY(!encoded.ok);
    QVERIFY(encoded.error.contains(QStringLiteral("Unsupported fence")));

    WpRowData invalidRally;
    invalidRally.Command = MAV_CMD_NAV_WAYPOINT;
    invalidRally.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    encoded = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Rally,
            {invalidRally});
    QVERIFY(!encoded.ok);
    QVERIFY(encoded.error.contains(QStringLiteral("Unsupported rally")));

    auto validMission = FlightPlannerMissionCodec::encode(
            FlightPlannerMissionModel::MissionStore::Mission,
            {mission}, true, 1.0, 2.0, 3.0);
    QVERIFY(validMission.ok);
    validMission.items[0].z = std::numeric_limits<float>::quiet_NaN();
    const auto decoded = FlightPlannerMissionCodec::decode(
            FlightPlannerMissionModel::MissionStore::Mission,
            validMission.items);
    QVERIFY(!decoded.ok);
}

QTEST_APPLESS_MAIN(FlightPlannerMissionCodecTest)
#include "test_flightplannermissioncodec.moc"
