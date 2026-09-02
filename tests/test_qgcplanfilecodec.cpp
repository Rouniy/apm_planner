#include "ui/flightplanner/QgcPlanFileCodec.h"

#include "QGCMAVLink.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <limits>

using namespace MissionPlanner;

namespace
{

WpRowData row(int sequence, quint16 command, quint8 frame,
              double p1, double p2, double p3, double p4,
              double latitude, double longitude, double altitude)
{
    WpRowData result;
    result.Seq = sequence;
    result.Command = command;
    result.Frame = frame;
    result.P1 = p1;
    result.P2 = p2;
    result.P3 = p3;
    result.P4 = p4;
    result.Lat = latitude;
    result.Lng = longitude;
    result.Alt = altitude;
    return result;
}

QgcPlanFileCodec::PlanData completePlan()
{
    QgcPlanFileCodec::PlanData plan;
    plan.Home = {40.123456789, 28.987654321, 143.25};
    plan.CruiseSpeed = 17.5;
    plan.HoverSpeed = 6.25;
    plan.FirmwareType = 3;
    plan.VehicleType = 2;
    plan.Mission = {
        row(4, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT,
            1.0, 2.0, 3.0, 4.0, 40.12, 28.98, 50.0),
        row(9, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_TERRAIN_ALT,
            5.0, 6.0, 7.0, 8.0, 40.13, 28.99, 62.5),
    };

    Fence fence;
    fence.HasReturn = true;
    fence.ReturnPoint.Return = {40.11, 28.91, 31.0};
    fence.ReturnPoint.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    FencePolygon inclusion;
    inclusion.Points = {
        {40.10, 28.90, 11.0},
        {40.20, 28.90, 12.0},
        {40.15, 29.00, 13.0},
    };
    fence.Polygons.append(inclusion);
    FencePolygon exclusion;
    exclusion.Mode = FencePolygon::PolyType::Exclusive;
    exclusion.Points = {
        {40.13, 28.93, 21.0},
        {40.16, 28.93, 22.0},
        {40.145, 28.96, 23.0},
    };
    fence.Polygons.append(exclusion);
    FenceCircle circle;
    circle.Center = {40.17, 28.95, 44.0};
    circle.Radius = 125.75;
    circle.Mode = FenceCircle::PolyType::Exclusive;
    fence.Circles.append(circle);
    QString error;
    plan.Fence = fence.FenceToLocation(&error);
    Q_ASSERT(error.isEmpty());
    plan.Fence[0].P2 = 91.0;
    plan.Fence[1].Frame = MAV_FRAME_GLOBAL_TERRAIN_ALT;
    plan.Fence.last().P4 = 72.0;

    RallyPoints rally;
    RallyPoint first;
    first.Position = {40.31, 29.11, 80.0};
    first.BreakAltitude = 45.0;
    first.LandHeading = 12345.0;
    first.Flags = 3;
    first.Frame = MAV_FRAME_GLOBAL_TERRAIN_ALT;
    rally.Points.append(first);
    RallyPoint second;
    second.Position = {40.32, 29.12, 90.0};
    second.BreakAltitude = 55.0;
    second.LandHeading = 27000.0;
    second.Flags = 7;
    second.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    rally.Points.append(second);
    plan.Rally = rally.RallyToLocation(&error);
    Q_ASSERT(error.isEmpty());
    plan.Rally[0].P4 = 19.0;
    return plan;
}

void compareRow(const WpRowData &actual, const WpRowData &expected)
{
    QCOMPARE(actual.Seq, expected.Seq);
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

void compareRows(const QVector<WpRowData> &actual,
                 const QVector<WpRowData> &expected)
{
    QCOMPARE(actual.size(), expected.size());
    for (int index = 0; index < actual.size(); ++index) {
        compareRow(actual.at(index), expected.at(index));
    }
}

} // namespace

class QgcPlanFileCodecTest final : public QObject
{
    Q_OBJECT

private slots:
    void losslessRoundTripPreservesAllStoresAndHome();
    void readsStandardQgcPlanWithoutLosslessExtension();
    void readsLegacyCoordinateAndNullParameters();
    void readsExpandedQgcSurveyComplexItem();
    void rejectsComplexItemWithoutLosslessExpansion();
    void rejectsMalformedSchemaAndRanges_data();
    void rejectsMalformedSchemaAndRanges();
    void rejectsMismatchedLosslessExtension();
    void invalidSaveDoesNotOverwriteExistingPlan();
};

void QgcPlanFileCodecTest::losslessRoundTripPreservesAllStoresAndHome()
{
    const QgcPlanFileCodec::PlanData source = completePlan();
    const auto encoded = QgcPlanFileCodec::EncodePlan(source);
    QVERIFY2(encoded.ok, qPrintable(encoded.error));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
            encoded.data, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    const QJsonObject root = document.object();
    QCOMPARE(root.value(QStringLiteral("fileType")).toString(),
             QStringLiteral("Plan"));
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 1);
    QVERIFY(root.value(QStringLiteral("mission")).isObject());
    QVERIFY(root.value(QStringLiteral("geoFence")).isObject());
    QVERIFY(root.value(QStringLiteral("rallyPoints")).isObject());
    QVERIFY(root.value(QStringLiteral("apmPlanner")).isObject());
    QCOMPARE(root.value(QStringLiteral("mission")).toObject()
                 .value(QStringLiteral("items")).toArray().size(), 2);

    const auto decoded = QgcPlanFileCodec::DecodePlan(encoded.data);
    QVERIFY2(decoded.ok, qPrintable(decoded.error));
    QCOMPARE(decoded.plan.Home.Latitude, source.Home.Latitude);
    QCOMPARE(decoded.plan.Home.Longitude, source.Home.Longitude);
    QCOMPARE(decoded.plan.Home.Altitude, source.Home.Altitude);
    QCOMPARE(decoded.plan.CruiseSpeed, source.CruiseSpeed);
    QCOMPARE(decoded.plan.HoverSpeed, source.HoverSpeed);
    QCOMPARE(decoded.plan.FirmwareType, source.FirmwareType);
    QCOMPARE(decoded.plan.VehicleType, source.VehicleType);
    compareRows(decoded.plan.Mission, source.Mission);
    compareRows(decoded.plan.Fence, source.Fence);
    compareRows(decoded.plan.Rally, source.Rally);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("complete.plan"));
    const auto saved = QgcPlanFileCodec::SavePlan(path, source);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    const auto loaded = QgcPlanFileCodec::LoadPlan(path);
    QVERIFY2(loaded.ok, qPrintable(loaded.error));
    compareRows(loaded.plan.Mission, source.Mission);
    compareRows(loaded.plan.Fence, source.Fence);
    compareRows(loaded.plan.Rally, source.Rally);
}

void QgcPlanFileCodecTest::readsStandardQgcPlanWithoutLosslessExtension()
{
    const QgcPlanFileCodec::PlanData source = completePlan();
    const auto encoded = QgcPlanFileCodec::EncodePlan(source);
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    QJsonObject root = QJsonDocument::fromJson(encoded.data).object();
    root.remove(QStringLiteral("apmPlanner"));

    const auto decoded = QgcPlanFileCodec::DecodePlan(
            QJsonDocument(root).toJson(QJsonDocument::Compact));
    QVERIFY2(decoded.ok, qPrintable(decoded.error));
    QCOMPARE(decoded.plan.Mission.size(), source.Mission.size());
    QCOMPARE(decoded.plan.Mission.at(0).Command, source.Mission.at(0).Command);
    QCOMPARE(decoded.plan.Mission.at(0).P4, source.Mission.at(0).P4);
    QCOMPARE(decoded.plan.Fence.size(), source.Fence.size());
    QCOMPARE(decoded.plan.Rally.size(), source.Rally.size());

    // These are the intentional limitations of the interoperable QGC
    // sections.  Our apmPlanner block restores the exact values above.
    QCOMPARE(decoded.plan.Fence.at(1).Alt, 0.0);
    QCOMPARE(decoded.plan.Rally.at(0).P1, 0.0);
    QCOMPARE(decoded.plan.Rally.at(0).Frame,
             quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
}

void QgcPlanFileCodecTest::readsLegacyCoordinateAndNullParameters()
{
    const QByteArray legacy = R"json({
        "fileType": "Plan",
        "groundStation": "QGroundControl",
        "version": 1,
        "mission": {
            "cruiseSpeed": 15,
            "firmwareType": 12,
            "hoverSpeed": 5,
            "items": [{
                "autoContinue": true,
                "command": 16,
                "coordinate": [47.63369112, -122.08925023, 20],
                "doJumpId": 1,
                "frame": 3,
                "params": [0, 0, 0, null],
                "type": "SimpleItem"
            }],
            "plannedHomePosition": [47.63338975, -122.090763, 20],
            "vehicleType": 2,
            "version": 2
        },
        "geoFence": {"circles": [], "polygons": [], "version": 2},
        "rallyPoints": {"points": [], "version": 2}
    })json";

    const auto decoded = QgcPlanFileCodec::DecodePlan(legacy);
    QVERIFY2(decoded.ok, qPrintable(decoded.error));
    QCOMPARE(decoded.plan.Mission.size(), 1);
    QCOMPARE(decoded.plan.Mission.first().P4, 0.0);
    QCOMPARE(decoded.plan.Mission.first().Lat, 47.63369112);
    QCOMPARE(decoded.plan.Mission.first().Lng, -122.08925023);
    QCOMPARE(decoded.plan.Mission.first().Alt, 20.0);
}

void QgcPlanFileCodecTest::readsExpandedQgcSurveyComplexItem()
{
    // QGC 5 SurveyComplexItem::save writes its generated MAVLink items into
    // TransectStyleComplexItem.Items.  Import those exact rows so survey
    // geometry/camera calculations do not need to be reimplemented here.
    const QByteArray plan = R"json({
        "fileType": "Plan",
        "groundStation": "QGroundControl",
        "version": 1,
        "mission": {
            "cruiseSpeed": 15,
            "firmwareType": 12,
            "hoverSpeed": 5,
            "items": [{
                "angle": 27,
                "complexItemType": "survey",
                "entryLocation": 0,
                "flyAlternateTransects": false,
                "polygon": [[47.0, 8.0], [47.0, 8.01], [47.01, 8.01]],
                "splitConcavePolygons": false,
                "TransectStyleComplexItem": {
                    "Items": [{
                        "autoContinue": true,
                        "command": 16,
                        "doJumpId": 1,
                        "frame": 3,
                        "params": [0, 0, 0, null, 47.001, 8.002, 65],
                        "type": "SimpleItem"
                    }, {
                        "autoContinue": true,
                        "command": 206,
                        "doJumpId": 2,
                        "frame": 2,
                        "params": [25, 0, 1, 0, 0, 0, 0],
                        "type": "SimpleItem"
                    }, {
                        "autoContinue": true,
                        "command": 16,
                        "doJumpId": 3,
                        "frame": 3,
                        "params": [0, 0, 0, null, 47.009, 8.008, 65],
                        "type": "SimpleItem"
                    }],
                    "version": 2
                },
                "type": "ComplexItem",
                "version": 5
            }],
            "plannedHomePosition": [47.0, 8.0, 500],
            "vehicleType": 2,
            "version": 2
        },
        "geoFence": {"circles": [], "polygons": [], "version": 2},
        "rallyPoints": {"points": [], "version": 2}
    })json";

    const auto decoded = QgcPlanFileCodec::DecodePlan(plan);
    QVERIFY2(decoded.ok, qPrintable(decoded.error));
    QCOMPARE(decoded.plan.Mission.size(), 3);
    QCOMPARE(decoded.plan.Mission.at(0).Seq, 0);
    QCOMPARE(decoded.plan.Mission.at(0).Command,
             quint16(MAV_CMD_NAV_WAYPOINT));
    QCOMPARE(decoded.plan.Mission.at(0).P4, 0.0);
    QCOMPARE(decoded.plan.Mission.at(0).Lat, 47.001);
    QCOMPARE(decoded.plan.Mission.at(1).Seq, 1);
    QCOMPARE(decoded.plan.Mission.at(1).Command,
             quint16(MAV_CMD_DO_SET_CAM_TRIGG_DIST));
    QCOMPARE(decoded.plan.Mission.at(1).P1, 25.0);
    QCOMPARE(decoded.plan.Mission.at(2).Seq, 2);
    QCOMPARE(decoded.plan.Mission.at(2).Lng, 8.008);
}

void QgcPlanFileCodecTest::rejectsComplexItemWithoutLosslessExpansion()
{
    const QByteArray plan = R"json({
        "fileType": "Plan",
        "groundStation": "QGroundControl",
        "version": 1,
        "mission": {
            "cruiseSpeed": 15,
            "firmwareType": 12,
            "hoverSpeed": 5,
            "items": [{
                "complexItemType": "survey",
                "TransectStyleComplexItem": {"Items": [], "version": 2},
                "type": "ComplexItem",
                "version": 5
            }],
            "plannedHomePosition": [47.0, 8.0, 500],
            "vehicleType": 2,
            "version": 2
        },
        "geoFence": {"circles": [], "polygons": [], "version": 2},
        "rallyPoints": {"points": [], "version": 2}
    })json";

    const auto decoded = QgcPlanFileCodec::DecodePlan(plan);
    QVERIFY(!decoded.ok);
    QVERIFY2(decoded.error.contains(QStringLiteral("must not be empty")),
             qPrintable(decoded.error));
}

void QgcPlanFileCodecTest::rejectsMalformedSchemaAndRanges_data()
{
    QTest::addColumn<QString>("mutation");
    QTest::addColumn<QString>("errorFragment");

    QTest::newRow("wrong root version")
            << QStringLiteral("root-version") << QStringLiteral("version");
    QTest::newRow("short mission params")
            << QStringLiteral("short-params") << QStringLiteral("7 numbers");
    QTest::newRow("negative circle radius")
            << QStringLiteral("negative-radius") << QStringLiteral("positive");
    QTest::newRow("rally latitude out of range")
            << QStringLiteral("rally-latitude") << QStringLiteral("latitude");
    QTest::newRow("home longitude out of range")
            << QStringLiteral("home-longitude") << QStringLiteral("longitude");
}

void QgcPlanFileCodecTest::rejectsMalformedSchemaAndRanges()
{
    QFETCH(QString, mutation);
    QFETCH(QString, errorFragment);

    const auto encoded = QgcPlanFileCodec::EncodePlan(completePlan());
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    QJsonObject root = QJsonDocument::fromJson(encoded.data).object();
    root.remove(QStringLiteral("apmPlanner"));

    if (mutation == QStringLiteral("root-version")) {
        root.insert(QStringLiteral("version"), 2);
    } else if (mutation == QStringLiteral("short-params")) {
        QJsonObject mission = root.value(QStringLiteral("mission")).toObject();
        QJsonArray items = mission.value(QStringLiteral("items")).toArray();
        QJsonObject first = items.at(0).toObject();
        first.insert(QStringLiteral("params"), QJsonArray{1.0, 2.0});
        items.replace(0, first);
        mission.insert(QStringLiteral("items"), items);
        root.insert(QStringLiteral("mission"), mission);
    } else if (mutation == QStringLiteral("negative-radius")) {
        QJsonObject fence = root.value(QStringLiteral("geoFence")).toObject();
        QJsonArray circles = fence.value(QStringLiteral("circles")).toArray();
        QJsonObject first = circles.at(0).toObject();
        QJsonObject geometry = first.value(QStringLiteral("circle")).toObject();
        geometry.insert(QStringLiteral("radius"), -1.0);
        first.insert(QStringLiteral("circle"), geometry);
        circles.replace(0, first);
        fence.insert(QStringLiteral("circles"), circles);
        root.insert(QStringLiteral("geoFence"), fence);
    } else if (mutation == QStringLiteral("rally-latitude")) {
        QJsonObject rally = root.value(QStringLiteral("rallyPoints")).toObject();
        QJsonArray points = rally.value(QStringLiteral("points")).toArray();
        QJsonArray first = points.at(0).toArray();
        first.replace(0, 91.0);
        points.replace(0, first);
        rally.insert(QStringLiteral("points"), points);
        root.insert(QStringLiteral("rallyPoints"), rally);
    } else if (mutation == QStringLiteral("home-longitude")) {
        QJsonObject mission = root.value(QStringLiteral("mission")).toObject();
        QJsonArray home = mission.value(
                QStringLiteral("plannedHomePosition")).toArray();
        home.replace(1, 181.0);
        mission.insert(QStringLiteral("plannedHomePosition"), home);
        root.insert(QStringLiteral("mission"), mission);
    }

    const auto decoded = QgcPlanFileCodec::DecodePlan(
            QJsonDocument(root).toJson(QJsonDocument::Compact));
    QVERIFY(!decoded.ok);
    QVERIFY2(decoded.error.contains(errorFragment, Qt::CaseInsensitive),
             qPrintable(decoded.error));
}

void QgcPlanFileCodecTest::rejectsMismatchedLosslessExtension()
{
    const auto encoded = QgcPlanFileCodec::EncodePlan(completePlan());
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    QJsonObject root = QJsonDocument::fromJson(encoded.data).object();
    QJsonObject extension = root.value(QStringLiteral("apmPlanner")).toObject();
    QJsonArray rallies = extension.value(QStringLiteral("rallyItems")).toArray();
    QJsonObject first = rallies.at(0).toObject();
    QJsonArray params = first.value(QStringLiteral("params")).toArray();
    params.replace(4, 40.99);
    first.insert(QStringLiteral("params"), params);
    rallies.replace(0, first);
    extension.insert(QStringLiteral("rallyItems"), rallies);
    root.insert(QStringLiteral("apmPlanner"), extension);

    const auto decoded = QgcPlanFileCodec::DecodePlan(
            QJsonDocument(root).toJson(QJsonDocument::Compact));
    QVERIFY(!decoded.ok);
    QVERIFY(decoded.error.contains(QStringLiteral("do not match")));
}

void QgcPlanFileCodecTest::invalidSaveDoesNotOverwriteExistingPlan()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("atomic.plan"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("original contents"), qint64(17));
    file.close();

    QgcPlanFileCodec::PlanData invalid = completePlan();
    invalid.Home.Latitude = std::numeric_limits<double>::quiet_NaN();
    const auto saved = QgcPlanFileCodec::SavePlan(path, invalid);
    QVERIFY(!saved.ok);
    QVERIFY(saved.error.contains(QStringLiteral("Home")));

    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("original contents"));
    file.close();

    const auto malformed = QgcPlanFileCodec::LoadPlan(path);
    QVERIFY(!malformed.ok);
    QVERIFY(!malformed.error.isEmpty());
}

QTEST_APPLESS_MAIN(QgcPlanFileCodecTest)
#include "test_qgcplanfilecodec.moc"
