#include "ui/flightplanner/FlightPlannerViewModel.h"

#include "QGCMAVLink.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class FlightPlannerViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndTypedWaypointCreation();
    void wplRoundTripKeepsHomeOutsideMissionRows();
    void appendKeepsHomeAndMalformedLoadIsAtomic();
    void homeValidityIsPreservedAcrossWplOperations();
};

void FlightPlannerViewModelTest::defaultsAndTypedWaypointCreation()
{
    FlightPlannerViewModel viewModel;
    QCOMPARE(viewModel.Status(), QStringLiteral("ready"));
    QCOMPARE(viewModel.MissionType(), QStringLiteral("Mission"));
    QCOMPARE(viewModel.DefaultAltitude(), 100.0);
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

QTEST_MAIN(FlightPlannerViewModelTest)
#include "test_flightplannerviewmodel.moc"
