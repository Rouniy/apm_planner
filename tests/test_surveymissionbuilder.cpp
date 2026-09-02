#include "ui/flightplanner/SurveyMissionBuilder.h"

#include "QGCMAVLink.h"

#include <QtTest>

namespace {

SurveyGridPoint point(double latitude, double longitude,
                      SurveyGridPointType type)
{
    SurveyGridPoint result;
    result.coordinate = {latitude, longitude, 100.0};
    result.type = type;
    return result;
}

QVector<SurveyGridPoint> oneStrip()
{
    return {
        point(1, 1, SurveyGridPointType::StripStart),
        point(1, 2, SurveyGridPointType::SurveyStart),
        point(1, 3, SurveyGridPointType::Photo),
        point(1, 4, SurveyGridPointType::SurveyEnd),
        point(2, 4, SurveyGridPointType::StripEnd),
    };
}

QVector<SurveyGridPoint> twoStrips()
{
    QVector<SurveyGridPoint> result = oneStrip();
    result += QVector<SurveyGridPoint>{
        point(2, 5, SurveyGridPointType::StripStart),
        point(2, 4, SurveyGridPointType::SurveyStart),
        point(2, 3, SurveyGridPointType::Photo),
        point(2, 2, SurveyGridPointType::SurveyEnd),
        point(2, 1, SurveyGridPointType::StripEnd),
    };
    return result;
}

} // namespace

class SurveyMissionBuilderTest final : public QObject
{
    Q_OBJECT

private slots:
    void continuousDistanceTriggerSkipsPhotoMarkersAndStops();
    void stopStartDistanceTriggerWrapsStrip();
    void buildsSelectedFlightAndCameraCommands();
    void splitsOnlyAtCompleteStripBoundaries();
    void rejectsUnsafeSplitMission();
};

void SurveyMissionBuilderTest::
continuousDistanceTriggerSkipsPhotoMarkersAndStops()
{
    SurveyMissionOptions options;
    options.triggerMode = SurveyMissionOptions::TriggerMode::Distance;
    options.triggerDistance = 25.0;

    const SurveyMissionPlan plan = SurveyMissionBuilder::Build(
        oneStrip(), {1, 2, 3}, options);

    QVERIFY2(plan.success, qPrintable(plan.error));
    QCOMPARE(plan.navigationCount, 3);
    QCOMPARE(plan.cameraCommandCount, 2);
    QCOMPARE(plan.commands.size(), 5);
    QCOMPARE(plan.commands.at(0).Command,
             static_cast<quint16>(MAV_CMD_NAV_WAYPOINT));
    QCOMPARE(plan.commands.at(1).Command,
             static_cast<quint16>(MAV_CMD_DO_SET_CAM_TRIGG_DIST));
    QCOMPARE(plan.commands.at(1).P1, 25.0);
    QCOMPARE(plan.commands.last().Command,
             static_cast<quint16>(MAV_CMD_DO_SET_CAM_TRIGG_DIST));
    QCOMPARE(plan.commands.last().P1, 0.0);
}

void SurveyMissionBuilderTest::stopStartDistanceTriggerWrapsStrip()
{
    SurveyMissionOptions options;
    options.triggerMode = SurveyMissionOptions::TriggerMode::Distance;
    options.triggerDistance = 25.0;
    options.stopTriggerAtStripEnds = true;

    const SurveyMissionPlan plan = SurveyMissionBuilder::Build(
        oneStrip(), {}, options);

    QVERIFY2(plan.success, qPrintable(plan.error));
    QCOMPARE(plan.navigationCount, 4);
    QCOMPARE(plan.cameraCommandCount, 3);
    QCOMPARE(plan.commands.at(2).P1, 25.0);
    QCOMPARE(plan.commands.at(4).P1, 0.0);
    QCOMPARE(plan.commands.last().P1, 0.0);
}

void SurveyMissionBuilderTest::buildsSelectedFlightAndCameraCommands()
{
    SurveyMissionOptions options;
    options.triggerMode = SurveyMissionOptions::TriggerMode::Digicam;
    options.useSpeed = true;
    options.flyingSpeed = 8.0;
    options.addTakeoff = true;
    options.takeoffAltitude = 35.0;
    options.finishAction = SurveyMissionOptions::FinishAction::Land;
    options.useSplineWaypoints = true;
    options.holdHeading = true;
    options.heading = 370.0;

    const SurveyMissionPlan plan = SurveyMissionBuilder::Build(
        oneStrip(), {9, 10, 11}, options);

    QVERIFY2(plan.success, qPrintable(plan.error));
    QCOMPARE(plan.commands.first().Command,
             static_cast<quint16>(MAV_CMD_NAV_TAKEOFF));
    QCOMPARE(plan.commands.first().Alt, 35.0);
    QCOMPARE(plan.commands.at(1).Command,
             static_cast<quint16>(MAV_CMD_DO_CHANGE_SPEED));
    int digicamCount = 0;
    int splineCount = 0;
    for (const WpRowData &row : plan.commands) {
        if (row.Command == MAV_CMD_DO_DIGICAM_CONTROL) ++digicamCount;
        if (row.Command == MAV_CMD_NAV_SPLINE_WAYPOINT) ++splineCount;
        if (row.Command == MAV_CMD_CONDITION_YAW) QCOMPARE(row.P1, 10.0);
    }
    QCOMPARE(digicamCount, 1);
    QVERIFY(splineCount > 0);
    QCOMPARE(plan.commands.last().Command,
             static_cast<quint16>(MAV_CMD_NAV_LAND));
    QCOMPARE(plan.commands.last().Lat, 9.0);
    QCOMPARE(plan.commands.last().Lng, 10.0);
}

void SurveyMissionBuilderTest::splitsOnlyAtCompleteStripBoundaries()
{
    SurveyMissionOptions options;
    options.triggerMode = SurveyMissionOptions::TriggerMode::Distance;
    options.addTakeoff = true;
    options.finishAction = SurveyMissionOptions::FinishAction::ReturnToLaunch;
    options.splitCount = 2;

    QString error;
    const QVector<QPair<int, int>> ranges =
        SurveyMissionBuilder::SplitRanges(twoStrips(), 2, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QVector<QPair<int, int>> expectedRanges{{0, 5}, {5, 10}};
    QCOMPARE(ranges, expectedRanges);

    const SurveyMissionPlan plan = SurveyMissionBuilder::Build(
        twoStrips(), {}, options);
    QVERIFY2(plan.success, qPrintable(plan.error));
    QCOMPARE(plan.segmentCount, 2);
    QCOMPARE(plan.navigationCount, 6);
    QVERIFY(plan.jumpTargetsAreRelative);
    QCOMPARE(plan.commands.at(0).Command,
             static_cast<quint16>(MAV_CMD_DO_JUMP));
    QCOMPARE(plan.commands.at(1).Command,
             static_cast<quint16>(MAV_CMD_DO_JUMP));
}

void SurveyMissionBuilderTest::rejectsUnsafeSplitMission()
{
    SurveyMissionOptions options;
    options.splitCount = 2;
    options.addTakeoff = true;

    const SurveyMissionPlan plan = SurveyMissionBuilder::Build(
        twoStrips(), {}, options);
    QVERIFY(!plan.success);
    QVERIFY(plan.error.contains(QStringLiteral("RTL or Land")));
}

QTEST_APPLESS_MAIN(SurveyMissionBuilderTest)

#include "test_surveymissionbuilder.moc"
