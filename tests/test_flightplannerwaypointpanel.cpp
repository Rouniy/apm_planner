#include "ui/flightplanner/FlightPlannerWaypointPanel.h"

#include "QGCMAVLink.h"
#include "ui/flightplanner/FlightPlannerMissionModel.h"
#include "ui/flightplanner/FlightPlannerViewModel.h"

#include <QAction>
#include <QItemSelectionModel>
#include <QSignalSpy>
#include <QTableView>
#include <QtTest>

class FlightPlannerWaypointPanelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesMissionPlannerNamesAndModel();
    void toolbarAndDeleteKeyEditSelectedWaypoint();
    void missionStoreResetInvalidatesSelection();
};

namespace {
QAction *panelAction(FlightPlannerWaypointPanel *panel, const char *name)
{
    QAction *action = panel->findChild<QAction *>(QString::fromLatin1(name));
    Q_ASSERT(action);
    return action;
}

void selectRow(QTableView *table, int row)
{
    const QModelIndex index = table->model()->index(row, 0);
    table->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::ClearAndSelect
               | QItemSelectionModel::Rows);
}
}

void FlightPlannerWaypointPanelTest::exposesMissionPlannerNamesAndModel()
{
    FlightPlannerViewModel viewModel;
    FlightPlannerWaypointPanel panel(&viewModel);

    QCOMPARE(panel.objectName(), QStringLiteral("WaypointPanel"));
    QTableView *table = panel.findChild<QTableView *>(QStringLiteral("WpGrid"));
    QVERIFY(table);
    QCOMPARE(table, panel.waypointTable());
    QCOMPARE(panel.viewModel(), &viewModel);
    QCOMPARE(table->model(),
             static_cast<QAbstractItemModel *>(viewModel.Waypoints()));
    QCOMPARE(table->selectionBehavior(), QAbstractItemView::SelectRows);
    QCOMPARE(table->selectionMode(), QAbstractItemView::SingleSelection);
    QCOMPARE(table->columnWidth(FlightPlannerMissionModel::CommandColumn), 190);
    QCOMPARE(panel.selectedWaypoint(), -1);

    QVERIFY(panelAction(&panel, "AddWaypoint")->isEnabled());
    QVERIFY(!panelAction(&panel, "DeleteWaypoint")->isEnabled());
    QVERIFY(!panelAction(&panel, "MoveWaypointUp")->isEnabled());
    QVERIFY(!panelAction(&panel, "MoveWaypointDown")->isEnabled());
}

void FlightPlannerWaypointPanelTest::toolbarAndDeleteKeyEditSelectedWaypoint()
{
    FlightPlannerViewModel viewModel;
    viewModel.setDefaultAltitude(73.0);
    FlightPlannerWaypointPanel panel(&viewModel);
    QTableView *table = panel.waypointTable();
    QAction *add = panelAction(&panel, "AddWaypoint");
    QAction *remove = panelAction(&panel, "DeleteWaypoint");
    QAction *up = panelAction(&panel, "MoveWaypointUp");
    QAction *down = panelAction(&panel, "MoveWaypointDown");
    QSignalSpy selectionChanged(
        &panel, &FlightPlannerWaypointPanel::selectedWaypointChanged);

    viewModel.setHomeLat(10.0);
    viewModel.setHomeLng(28.0);
    add->trigger();
    viewModel.setHomeLat(20.0);
    viewModel.setHomeLng(29.0);
    add->trigger();
    viewModel.setHomeLat(30.0);
    viewModel.setHomeLng(30.0);
    add->trigger();

    QCOMPARE(viewModel.Waypoints()->rowCount(), 3);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Lat(), 10.0);
    QCOMPARE(viewModel.Waypoints()->rowAt(2)->Lng(), 30.0);
    QCOMPARE(viewModel.Waypoints()->rowAt(2)->Alt(), 73.0);
    QCOMPARE(panel.selectedWaypoint(), 2);
    QVERIFY(remove->isEnabled());
    QVERIFY(up->isEnabled());
    QVERIFY(!down->isEnabled());

    up->trigger();
    QCOMPARE(panel.selectedWaypoint(), 1);
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Lat(), 30.0);
    QVERIFY(down->isEnabled());
    down->trigger();
    QCOMPARE(panel.selectedWaypoint(), 2);
    QCOMPARE(viewModel.Waypoints()->rowAt(2)->Lat(), 30.0);

    remove->trigger();
    QCOMPARE(viewModel.Waypoints()->rowCount(), 2);
    QCOMPARE(panel.selectedWaypoint(), 1);
    QCOMPARE(viewModel.Waypoints()->rowAt(1)->Lat(), 20.0);

    selectRow(table, 0);
    QCOMPARE(panel.selectedWaypoint(), 0);
    QTest::keyClick(table, Qt::Key_Delete);
    QCOMPARE(viewModel.Waypoints()->rowCount(), 1);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Lat(), 20.0);
    QCOMPARE(panel.selectedWaypoint(), 0);
    QVERIFY(selectionChanged.count() > 0);
}

void FlightPlannerWaypointPanelTest::missionStoreResetInvalidatesSelection()
{
    FlightPlannerViewModel viewModel;
    FlightPlannerWaypointPanel panel(&viewModel);
    QTableView *table = panel.waypointTable();
    QAction *add = panelAction(&panel, "AddWaypoint");
    QAction *remove = panelAction(&panel, "DeleteWaypoint");
    QAction *up = panelAction(&panel, "MoveWaypointUp");
    QAction *down = panelAction(&panel, "MoveWaypointDown");
    QSignalSpy selectionChanged(
        &panel, &FlightPlannerWaypointPanel::selectedWaypointChanged);

    add->trigger();
    QCOMPARE(panel.selectedWaypoint(), 0);
    table->selectionModel()->setCurrentIndex(
        QModelIndex(), QItemSelectionModel::ClearAndSelect);
    QCOMPARE(panel.selectedWaypoint(), -1);
    QVERIFY(!remove->isEnabled());
    QVERIFY(!up->isEnabled());
    QVERIFY(!down->isEnabled());

    selectRow(table, 0);
    selectionChanged.clear();
    viewModel.setMissionType(QStringLiteral("Fence"));
    QCOMPARE(viewModel.MissionType(), QStringLiteral("Fence"));
    QCOMPARE(viewModel.Waypoints()->rowCount(), 0);
    QCOMPARE(panel.selectedWaypoint(), -1);
    QVERIFY(!table->currentIndex().isValid());
    QVERIFY(!selectionChanged.isEmpty());
    QCOMPARE(selectionChanged.last().at(0).toInt(), -1);

    viewModel.setHomeLat(41.0);
    viewModel.setHomeLng(29.0);
    add->trigger();
    QCOMPARE(viewModel.Waypoints()->rowCount(), 1);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Command(),
             quint16(MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION));

    viewModel.setMissionType(QStringLiteral("Rally"));
    QCOMPARE(panel.selectedWaypoint(), -1);
    add->trigger();
    QCOMPARE(viewModel.Waypoints()->rowCount(), 1);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Command(),
             quint16(MAV_CMD_NAV_RALLY_POINT));

    viewModel.setMissionType(QStringLiteral("Mission"));
    QCOMPARE(panel.selectedWaypoint(), -1);
    QCOMPARE(viewModel.Waypoints()->rowCount(), 1);
    QCOMPARE(viewModel.Waypoints()->rowAt(0)->Command(),
             quint16(MAV_CMD_NAV_WAYPOINT));
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Fence), 1);
    QCOMPARE(viewModel.Waypoints()->storeRowCount(
                 FlightPlannerMissionModel::MissionStore::Rally), 1);
}

QTEST_MAIN(FlightPlannerWaypointPanelTest)
#include "test_flightplannerwaypointpanel.moc"
