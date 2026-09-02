#include <QtTest/QtTest>

#include "ui/flightplanner/MissionElevationProfile.h"
#include "QGCMAVLink.h"

#include <cmath>

namespace {
WpRowData waypoint(int sequence, quint16 command, quint8 frame,
                   double latitude, double longitude, double altitude)
{
    WpRowData row;
    row.Seq = sequence;
    row.Command = command;
    row.Frame = frame;
    row.Lat = latitude;
    row.Lng = longitude;
    row.Alt = altitude;
    return row;
}
}

class MissionElevationProfileTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizesFramesAndIncludesHome();
    void filtersNonFlightPathRows();
    void matchesMissionPlannerRouteCommandFilter();
    void terrainFrameFollowsInterpolatedClearance();
    void missingTerrainProducesGaps();
    void providerFailuresProduceGaps();
    void datelineUsesShortPathAndCapsSamples();
};

void MissionElevationProfileTest::normalizesFramesAndIncludesHome()
{
    const MissionElevationHome home{true, 1.0, 1.0, 100.0};
    const QVector<WpRowData> rows{
        waypoint(0, MAV_CMD_NAV_WAYPOINT,
                 MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
                 1.0, 1.001, 20.0),
        waypoint(1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_INT,
                 1.0, 1.002, 150.0),
        waypoint(2, MAV_CMD_NAV_WAYPOINT,
                 MAV_FRAME_GLOBAL_TERRAIN_ALT_INT,
                 1.0, 1.003, 30.0),
    };
    const auto terrain = [](double, double, double *altitude) {
        *altitude = 100.0;
        return true;
    };

    const MissionElevationProfileResult result =
        MissionElevationProfile::Build(rows, home, terrain, 1000.0);
    QVERIFY(result.hasRoute());
    QCOMPARE(result.routePointCount, 4);
    QCOMPARE(result.samples.size(), 4);
    QCOMPARE(result.markers.size(), 4);
    QCOMPARE(result.markers.at(0).label, QStringLiteral("H"));
    QCOMPARE(result.markers.at(1).label, QStringLiteral("1"));
    QCOMPARE(result.markers.at(2).label, QStringLiteral("2"));
    QCOMPARE(result.markers.at(3).label, QStringLiteral("3"));
    QCOMPARE(result.samples.at(0).plannedAltitudeAmslMeters, 100.0);
    QCOMPARE(result.samples.at(1).plannedAltitudeAmslMeters, 120.0);
    QCOMPARE(result.samples.at(2).plannedAltitudeAmslMeters, 150.0);
    QCOMPARE(result.samples.at(3).plannedAltitudeAmslMeters, 130.0);
    QCOMPARE(result.missingTerrainSampleCount, 0);
    QVERIFY(result.homeReferenceAvailable);
}

void MissionElevationProfileTest::filtersNonFlightPathRows()
{
    const QVector<WpRowData> rows{
        waypoint(0, MAV_CMD_DO_SET_ROI, MAV_FRAME_GLOBAL_RELATIVE_ALT,
                 2.0, 2.001, 10.0),
        waypoint(1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_LOCAL_NED,
                 2.0, 2.002, 10.0),
        waypoint(2, MAV_CMD_NAV_WAYPOINT,
                 MAV_FRAME_GLOBAL_RELATIVE_ALT,
                 2.0, 2.003, 10.0),
        waypoint(3, MAV_CMD_NAV_WAYPOINT,
                 MAV_FRAME_GLOBAL_RELATIVE_ALT,
                 2.0, 2.004, 20.0),
    };
    const auto terrain = [](double, double, double *altitude) {
        *altitude = 50.0;
        return true;
    };

    const MissionElevationProfileResult result =
        MissionElevationProfile::Build(rows, {}, terrain, 1000.0);
    QVERIFY(result.hasRoute());
    QCOMPARE(result.routePointCount, 2);
    QCOMPARE(result.markers.size(), 2);
    QCOMPARE(result.markers.at(0).label, QStringLiteral("3"));
    QCOMPARE(result.markers.at(1).label, QStringLiteral("4"));
    QVERIFY(std::isnan(
        result.samples.at(0).plannedAltitudeAmslMeters));
    QVERIFY(std::isnan(
        result.samples.last().plannedAltitudeAmslMeters));
    QVERIFY(!result.homeReferenceAvailable);

    const MissionElevationProfileResult tooShort =
        MissionElevationProfile::Build({rows.last()}, {}, terrain);
    QVERIFY(!tooShort.hasRoute());
    QVERIFY(tooShort.samples.isEmpty());
}

void MissionElevationProfileTest::matchesMissionPlannerRouteCommandFilter()
{
    const QVector<WpRowData> rows{
        waypoint(0, MAV_CMD_NAV_FOLLOW, MAV_FRAME_GLOBAL,
                 2.0, 2.001, 100.0),
        waypoint(1, 36, MAV_FRAME_GLOBAL,
                 2.0, 2.002, 110.0), // MAV_CMD_NAV_ARC_WAYPOINT
        waypoint(2, 188, MAV_FRAME_GLOBAL,
                 2.0, 2.003, 120.0), // MAV_CMD_DO_RETURN_PATH_START
        waypoint(3, MAV_CMD_DO_LAND_START, MAV_FRAME_GLOBAL,
                 2.0, 2.004, 130.0),
        waypoint(4, MAV_CMD_DO_REPOSITION, MAV_FRAME_GLOBAL,
                 2.0, 2.005, 140.0),
        waypoint(5, MAV_CMD_DO_SET_ROI, MAV_FRAME_GLOBAL,
                 2.0, 2.006, 150.0),
    };
    const auto terrain = [](double, double, double *altitude) {
        *altitude = 50.0;
        return true;
    };

    const MissionElevationProfileResult result =
        MissionElevationProfile::Build(rows, {}, terrain, 1000.0);
    QCOMPARE(result.routePointCount, 4);
    QCOMPARE(result.markers.size(), 4);
    QCOMPARE(result.markers.at(0).label, QStringLiteral("1"));
    QCOMPARE(result.markers.at(3).label, QStringLiteral("4"));
    QCOMPARE(result.samples.first().plannedAltitudeAmslMeters, 100.0);
    QCOMPARE(result.samples.last().plannedAltitudeAmslMeters, 130.0);
}

void MissionElevationProfileTest::terrainFrameFollowsInterpolatedClearance()
{
    const MissionElevationHome home{true, 0.5, 0.0, 100.0};
    const QVector<WpRowData> rows{
        waypoint(0, MAV_CMD_NAV_WAYPOINT,
                 MAV_FRAME_GLOBAL_TERRAIN_ALT,
                 0.5, 0.001, 30.0),
    };
    const auto terrain = [](double, double longitude, double *altitude) {
        *altitude = 100.0 + longitude * 10000.0;
        return true;
    };

    const MissionElevationProfileResult result =
        MissionElevationProfile::Build(rows, home, terrain, 60.0);
    QVERIFY(result.hasRoute());
    QCOMPARE(result.samples.size(), 3);
    const MissionElevationSample middle = result.samples.at(1);
    QVERIFY(std::abs(middle.longitude - 0.0005) < 1e-9);
    QVERIFY(std::abs(middle.terrainAltitudeAmslMeters - 105.0) < 1e-9);
    QVERIFY(std::abs(middle.plannedAltitudeAmslMeters - 120.0) < 1e-9);
    QCOMPARE(result.samples.last().plannedAltitudeAmslMeters, 140.0);
}

void MissionElevationProfileTest::missingTerrainProducesGaps()
{
    const QVector<WpRowData> terrainRows{
        waypoint(0, MAV_CMD_NAV_WAYPOINT,
                 MAV_FRAME_GLOBAL_TERRAIN_ALT, 3.0, 3.001, 20.0),
        waypoint(1, MAV_CMD_NAV_WAYPOINT,
                 MAV_FRAME_GLOBAL_TERRAIN_ALT, 3.0, 3.002, 20.0),
    };
    const auto unavailable = [](double, double, double *) {
        return false;
    };

    const MissionElevationProfileResult terrainResult =
        MissionElevationProfile::Build(
            terrainRows, {}, unavailable, 1000.0);
    QVERIFY(terrainResult.hasRoute());
    QCOMPARE(terrainResult.missingTerrainSampleCount,
             terrainResult.terrainSampleCount);
    QVERIFY(std::isnan(
        terrainResult.samples.first().terrainAltitudeAmslMeters));
    QVERIFY(std::isnan(
        terrainResult.samples.first().plannedAltitudeAmslMeters));

    QVector<WpRowData> absoluteRows = terrainRows;
    absoluteRows[0].Frame = MAV_FRAME_GLOBAL;
    absoluteRows[0].Alt = 120.0;
    absoluteRows[1].Frame = MAV_FRAME_GLOBAL;
    absoluteRows[1].Alt = 140.0;
    const MissionElevationProfileResult absoluteResult =
        MissionElevationProfile::Build(
            absoluteRows, {}, unavailable, 1000.0);
    QVERIFY(std::isnan(
        absoluteResult.samples.first().terrainAltitudeAmslMeters));
    QCOMPARE(absoluteResult.samples.first().plannedAltitudeAmslMeters,
             120.0);
    QCOMPARE(absoluteResult.samples.last().plannedAltitudeAmslMeters,
             140.0);

    QVector<WpRowData> mixedRows = terrainRows;
    mixedRows[1].Frame = MAV_FRAME_GLOBAL;
    mixedRows[1].Alt = 145.0;
    const MissionElevationProfileResult mixedResult =
        MissionElevationProfile::Build(
            mixedRows, {}, unavailable, 1000.0);
    QVERIFY(std::isnan(
        mixedResult.samples.first().plannedAltitudeAmslMeters));
    QCOMPARE(mixedResult.samples.last().plannedAltitudeAmslMeters,
             145.0);
}

void MissionElevationProfileTest::providerFailuresProduceGaps()
{
    const QVector<WpRowData> rows{
        waypoint(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL,
                 4.0, 4.001, 100.0),
        waypoint(1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL,
                 4.0, 4.002, 120.0),
    };
    const auto throwingProvider = [](double, double, double *) -> bool {
        throw 42;
    };

    const MissionElevationProfileResult result =
        MissionElevationProfile::Build(
            rows, {}, throwingProvider, 1000.0);
    QVERIFY(result.hasRoute());
    QCOMPARE(result.missingTerrainSampleCount,
             result.terrainSampleCount);
    QVERIFY(std::isnan(
        result.samples.first().terrainAltitudeAmslMeters));
    QCOMPARE(result.samples.first().plannedAltitudeAmslMeters, 100.0);
    QCOMPARE(result.samples.last().plannedAltitudeAmslMeters, 120.0);
}

void MissionElevationProfileTest::datelineUsesShortPathAndCapsSamples()
{
    const QVector<WpRowData> rows{
        waypoint(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL,
                 0.5, 179.999, 100.0),
        waypoint(1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL,
                 0.5, -179.999, 100.0),
    };
    const auto terrain = [](double, double, double *altitude) {
        *altitude = 0.0;
        return true;
    };

    const MissionElevationProfileResult result =
        MissionElevationProfile::Build(rows, {}, terrain, 1.0, 50);
    QVERIFY(result.hasRoute());
    QVERIFY(result.totalDistanceMeters > 200.0);
    QVERIFY(result.totalDistanceMeters < 250.0);
    QVERIFY(result.samples.size() <= 50);
    QVERIFY(result.sampleSpacingMeters > 1.0);
    for (const MissionElevationSample &sample : result.samples) {
        QVERIFY(sample.longitude >= -180.0);
        QVERIFY(sample.longitude <= 180.0);
    }
}

QTEST_APPLESS_MAIN(MissionElevationProfileTest)

#include "test_missionelevationprofile.moc"
