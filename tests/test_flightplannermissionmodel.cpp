#include "ui/flightplanner/FlightPlannerMissionModel.h"

#include "QGCMAVLink.h"

#include <QSignalSpy>
#include <QtTest>

#include <cmath>

class FlightPlannerMissionModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void wpRowMatchesMissionPlannerContract();
    void coordinateRepresentationsRoundTrip();
    void storesRemainIndependentAndPreserveIdentity();
    void structuralEditsRenumberRows();
    void tableContractMatchesWaypointGrid();
};

void FlightPlannerMissionModelTest::wpRowMatchesMissionPlannerContract()
{
    WpRow row;
    QCOMPARE(row.Seq(), 0);
    QCOMPARE(row.DisplayNumber(), 1);
    QCOMPARE(row.Command(), quint16(0));
    QCOMPARE(row.Frame(), quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QCOMPARE(row.FrameName(), QStringLiteral("Relative"));
    QCOMPARE(WpRow::FrameList(),
             QStringList({QStringLiteral("Relative"),
                          QStringLiteral("Absolute"),
                          QStringLiteral("Terrain")}));
    QVERIFY(WpRow::CommandList().size() > 100);
    QVERIFY(WpRow::CommandList().contains(QStringLiteral("DO_CHANGE_SPEED")));
    QVERIFY(WpRow::CommandList().contains(
        QStringLiteral("FENCE_CIRCLE_EXCLUSION")));

    QSignalSpy sequenceChanged(&row, &WpRow::seqChanged);
    QSignalSpy displayChanged(&row, &WpRow::displayNumberChanged);
    row.setSeq(4);
    QCOMPARE(row.Seq(), 4);
    QCOMPARE(row.DisplayNumber(), 5);
    QCOMPARE(sequenceChanged.count(), 1);
    QCOMPARE(displayChanged.count(), 1);

    row.setCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH);
    QCOMPARE(row.CommandName(), QStringLiteral("RETURN_TO_LAUNCH"));
    row.setCommandName(QStringLiteral("LAND"));
    QCOMPARE(row.Command(), quint16(MAV_CMD_NAV_LAND));
    row.setFrameName(QStringLiteral("Terrain"));
    QCOMPARE(row.Frame(), quint8(MAV_FRAME_GLOBAL_TERRAIN_ALT));
    QCOMPARE(row.FrameName(), QStringLiteral("Terrain"));

    row.setP1(1.0);
    row.setP2(2.0);
    row.setP3(3.0);
    row.setP4(4.0);
    row.setAltDisplay(123.5);
    const WpRowData saved = row.toData();
    QCOMPARE(saved.P1, 1.0);
    QCOMPARE(saved.P4, 4.0);
    QCOMPARE(saved.Alt, 123.5);
}

void FlightPlannerMissionModelTest::coordinateRepresentationsRoundTrip()
{
    constexpr double latitude = -35.363261;
    constexpr double longitude = 149.165230;
    WpRow source;
    source.setLat(latitude);
    source.setLng(longitude);
    QVERIFY(!source.Zone().isEmpty());
    QVERIFY(!source.Easting().isEmpty());
    QVERIFY(!source.Northing().isEmpty());
    QVERIFY(!source.Mgrs().isEmpty());

    WpRow fromUtm;
    fromUtm.setZone(source.Zone());
    fromUtm.setEasting(source.Easting());
    fromUtm.setNorthing(source.Northing());
    QVERIFY(std::abs(fromUtm.Lat() - latitude) < 0.00001);
    QVERIFY(std::abs(fromUtm.Lng() - longitude) < 0.00001);

    WpRow fromMgrs;
    fromMgrs.setMgrs(source.Mgrs());
    QVERIFY(std::abs(fromMgrs.Lat() - latitude) < 0.001);
    QVERIFY(std::abs(fromMgrs.Lng() - longitude) < 0.001);

    WpRow westernHighLatitude;
    westernHighLatitude.setLat(75.0);
    westernHighLatitude.setLng(-100.0);
    QCOMPARE(westernHighLatitude.Zone(), QStringLiteral("14"));
    WpRow svalbard;
    svalbard.setLat(75.0);
    svalbard.setLng(20.0);
    QCOMPARE(svalbard.Zone(), QStringLiteral("33"));

    QString invalidMgrs = source.Mgrs();
    int bandIndex = 0;
    while (bandIndex < invalidMgrs.size()
           && invalidMgrs.at(bandIndex).isDigit()) {
        ++bandIndex;
    }
    QVERIFY(bandIndex < invalidMgrs.size());
    bool rejectedIncompatibleBand = false;
    const QString bands = QStringLiteral("CDEFGHJKLMNPQRSTUVWX");
    for (QChar band : bands) {
        invalidMgrs[bandIndex] = band;
        WpRow candidate;
        candidate.setMgrs(invalidMgrs);
        if (candidate.Lat() == 0.0 && candidate.Lng() == 0.0) {
            rejectedIncompatibleBand = true;
            break;
        }
    }
    QVERIFY(rejectedIncompatibleBand);

    WpRowData partialCoordinates;
    partialCoordinates.Zone = QStringLiteral("55S");
    partialCoordinates.Easting = QStringLiteral("partial");
    WpRow preserved(partialCoordinates);
    QCOMPARE(preserved.Zone(), partialCoordinates.Zone);
    QCOMPARE(preserved.Easting(), partialCoordinates.Easting);
}

void FlightPlannerMissionModelTest::storesRemainIndependentAndPreserveIdentity()
{
    FlightPlannerMissionModel model;
    QObject marker;
    WpRowData mission;
    mission.Command = MAV_CMD_NAV_WAYPOINT;
    mission.Lat = 40.0;
    mission.Lng = 28.0;
    mission.Tag = QVariant::fromValue(static_cast<QObject *>(&marker));
    WpRow *missionIdentity = model.appendRow(mission);
    QCOMPARE(model.MissionType(), QStringLiteral("Mission"));
    QCOMPARE(model.rowCount(), 1);

    QSignalSpy rowsChanged(&model, &FlightPlannerMissionModel::rowsChanged);
    model.setMissionType(QStringLiteral("Rally"));
    QCOMPARE(model.rowCount(), 0);
    missionIdentity->setAlt(75.0);
    QCOMPARE(rowsChanged.count(), 1);
    QCOMPARE(rowsChanged.takeFirst().at(0).value<
                 FlightPlannerMissionModel::MissionStore>(),
             FlightPlannerMissionModel::MissionStore::Mission);
    WpRowData rally;
    rally.Command = MAV_CMD_NAV_RALLY_POINT;
    rally.Lat = 41.0;
    rally.Lng = 29.0;
    model.appendRow(rally);
    QCOMPARE(model.rowCount(), 1);

    model.setMissionType(QStringLiteral("Mission"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.rowAt(0), missionIdentity);
    QCOMPARE(model.rowAt(0)->Tag().value<QObject *>(), &marker);
    QCOMPARE(model.rowAt(0)->Lat(), 40.0);
    QCOMPARE(model.rowAt(0)->Alt(), 75.0);
    QCOMPARE(model.storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Rally), 1);

    QVector<WpRowData> fence(2);
    fence[0].Seq = 17;
    fence[0].Command = MAV_CMD_NAV_FENCE_RETURN_POINT;
    fence[1].Seq = 24;
    fence[1].Command = MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION;
    model.replaceStore(FlightPlannerMissionModel::MissionStore::Fence, fence);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Fence), 2);
    const QVector<WpRowData> savedFence = model.rows(
        FlightPlannerMissionModel::MissionStore::Fence);
    QCOMPARE(savedFence.at(0).Seq, 0);
    QCOMPARE(savedFence.at(1).Seq, 1);
}

void FlightPlannerMissionModelTest::structuralEditsRenumberRows()
{
    FlightPlannerMissionModel model;
    for (int index = 0; index < 3; ++index) {
        WpRowData row;
        row.Command = MAV_CMD_NAV_WAYPOINT;
        row.Lat = 10.0 + index;
        model.appendRow(row);
    }
    QCOMPARE(model.rowCount(), 3);
    QVERIFY(model.moveWaypointUp(2));
    QCOMPARE(model.rowAt(1)->Lat(), 12.0);
    QVERIFY(model.moveWaypointDown(0));
    QCOMPARE(model.rowAt(1)->Lat(), 10.0);
    QVERIFY(model.removeRows(1, 1));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.rowAt(0)->Seq(), 0);
    QCOMPARE(model.rowAt(1)->Seq(), 1);

    WpRowData inserted;
    inserted.Lat = 99.0;
    model.insertRowData(1, inserted);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.rowAt(1)->Lat(), 99.0);
    for (int row = 0; row < model.rowCount(); ++row) {
        QCOMPARE(model.rowAt(row)->Seq(), row);
        QCOMPARE(model.rowAt(row)->DisplayNumber(), row + 1);
    }
}

void FlightPlannerMissionModelTest::tableContractMatchesWaypointGrid()
{
    FlightPlannerMissionModel model;
    model.appendRow();
    QCOMPARE(model.columnCount(),
             int(FlightPlannerMissionModel::ColumnCount));
    QCOMPARE(model.headerData(0, Qt::Horizontal).toString(),
             QStringLiteral("#"));
    QCOMPARE(model.headerData(1, Qt::Horizontal).toString(),
             QStringLiteral("Command"));
    QCOMPARE(model.headerData(17, Qt::Horizontal).toString(),
             QStringLiteral("MGRS"));
    QVERIFY(!(model.flags(model.index(0,
        FlightPlannerMissionModel::NumberColumn)) & Qt::ItemIsEditable));
    QVERIFY(model.flags(model.index(0,
        FlightPlannerMissionModel::CommandColumn)) & Qt::ItemIsEditable);

    QVERIFY(model.setData(model.index(0,
        FlightPlannerMissionModel::CommandColumn),
        QStringLiteral("LAND")));
    QCOMPARE(model.rowAt(0)->Command(), quint16(MAV_CMD_NAV_LAND));
    QVERIFY(model.setData(model.index(0,
        FlightPlannerMissionModel::FrameColumn),
        QStringLiteral("Absolute")));
    QCOMPARE(model.rowAt(0)->Frame(), quint8(MAV_FRAME_GLOBAL));
    QCOMPARE(model.data(model.index(0,
        FlightPlannerMissionModel::CommandColumn)).toString(),
        QStringLiteral("LAND"));
    QVERIFY(!model.setData(model.index(0,
        FlightPlannerMissionModel::CommandColumn),
        QStringLiteral("NOT_A_MAV_COMMAND")));
    QVERIFY(!model.setData(model.index(0,
        FlightPlannerMissionModel::P1Column), QStringLiteral("invalid")));
    QVERIFY(!model.setData(model.index(0,
        FlightPlannerMissionModel::FrameColumn), QStringLiteral("Moon")));
}

QTEST_MAIN(FlightPlannerMissionModelTest)
#include "test_flightplannermissionmodel.moc"
