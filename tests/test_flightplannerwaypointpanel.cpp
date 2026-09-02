#include "ui/flightplanner/FlightPlannerWaypointPanel.h"

#include "QGCMAVLink.h"
#include "ui/flightplanner/FlightPlannerMissionModel.h"
#include "ui/flightplanner/FlightPlannerViewModel.h"

#include <QAction>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QStyledItemDelegate>
#include <QItemSelectionModel>
#include <QLabel>
#include <QSignalSpy>
#include <QSlider>
#include <QTableView>
#include <QtTest>

#include <cmath>

class FlightPlannerWaypointPanelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesMissionPlannerNamesAndModel();
    void toolbarAndDeleteKeyEditSelectedWaypoint();
    void missionStoreResetInvalidatesSelection();
    void commandEditorFollowsMissionType();
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
    auto *radiusLabel = panel.findChild<QLabel *>(QStringLiteral("LBL_WPRad"));
    auto *radiusEditor = panel.findChild<QDoubleSpinBox *>(
        QStringLiteral("TXT_WPRad"));
    QVERIFY(radiusLabel);
    QVERIFY(radiusEditor);
    QCOMPARE(radiusEditor->value(), 90.0);
    viewModel.setWpRadius(35.0);
    QCOMPARE(radiusEditor->value(), 35.0);
    radiusEditor->setValue(27.5);
    QCOMPARE(viewModel.WpRadius(), 27.5);

    auto *loiter = panel.findChild<QDoubleSpinBox *>(
        QStringLiteral("TXT_loiterrad"));
    auto *defaultAltitude = panel.findChild<QDoubleSpinBox *>(
        QStringLiteral("TXT_DefaultAlt"));
    auto *altMode = panel.findChild<QComboBox *>(
        QStringLiteral("CMB_altmode"));
    auto *altWarn = panel.findChild<QDoubleSpinBox *>(
        QStringLiteral("TXT_altwarn"));
    auto *spline = panel.findChild<QCheckBox *>(
        QStringLiteral("CHK_splinedefault"));
    auto *verifyHeight = panel.findChild<QCheckBox *>(
        QStringLiteral("CHK_verifyheight"));
    auto *zoom = panel.findChild<QSlider *>(QStringLiteral("ZoomSlider"));
    QVERIFY(loiter);
    QVERIFY(defaultAltitude);
    QVERIFY(altMode);
    QVERIFY(altWarn);
    QVERIFY(spline);
    QVERIFY(verifyHeight);
    QVERIFY(zoom);
    QCOMPARE(loiter->value(), 100.0);
    QCOMPARE(defaultAltitude->value(), 100.0);
    QCOMPARE(altMode->currentText(), QStringLiteral("Relative"));
    QCOMPARE(altWarn->value(), 0.0);
    QVERIFY(!spline->isChecked());
    QVERIFY(!verifyHeight->isChecked());

    loiter->setValue(-55.0);
    defaultAltitude->setValue(125.0);
    altMode->setCurrentText(QStringLiteral("Terrain"));
    altWarn->setValue(20.0);
    spline->setChecked(true);
    verifyHeight->setChecked(true);
    QCOMPARE(viewModel.LoiterRadius(), -55.0);
    QCOMPARE(viewModel.DefaultAltitude(), 125.0);
    QCOMPARE(viewModel.DefaultFrame(), QStringLiteral("Terrain"));
    QCOMPARE(viewModel.AltWarn(), 20.0);
    QVERIFY(viewModel.SplineDefault());
    QVERIFY(viewModel.VerifyHeight());

    viewModel.setAltUnits(QStringLiteral("Feet"));
    QCOMPARE(defaultAltitude->suffix(), QStringLiteral(" ft"));
    QCOMPARE(altWarn->suffix(), QStringLiteral(" ft"));
    QVERIFY(std::abs(defaultAltitude->value()
                     - viewModel.DefaultAltitudeDisplay()) < 0.01);
    QVERIFY(std::abs(altWarn->value() - viewModel.AltWarnDisplay()) < 0.01);
    QCOMPARE(table->model()->headerData(
                 FlightPlannerMissionModel::AltColumn,
                 Qt::Horizontal).toString(),
             QStringLiteral("Alt (ft)"));
    defaultAltitude->setValue(328.08);
    altWarn->setValue(65.62);
    QVERIFY(std::abs(viewModel.DefaultAltitude() - 100.0) < 0.01);
    QVERIFY(std::abs(viewModel.AltWarn() - 20.0) < 0.01);
    viewModel.setAltUnits(QStringLiteral("Meters"));
    QCOMPARE(defaultAltitude->suffix(), QStringLiteral(" m"));
    QCOMPARE(altWarn->suffix(), QStringLiteral(" m"));

    auto *distance = panel.findChild<QLabel *>(QStringLiteral("lbl_distance"));
    auto *homeDistance = panel.findChild<QLabel *>(
        QStringLiteral("lbl_homedist"));
    auto *previousDistance = panel.findChild<QLabel *>(
        QStringLiteral("lbl_prevdist"));
    QVERIFY(distance);
    QVERIFY(homeDistance);
    QVERIFY(previousDistance);

    viewModel.setDistUnits(QStringLiteral("Feet"));
    QCOMPARE(radiusEditor->suffix(), QStringLiteral(" ft"));
    QCOMPARE(loiter->suffix(), QStringLiteral(" ft"));
    QCOMPARE(distance->text(), QStringLiteral("Dist: 0.0000 miles"));
    QCOMPARE(homeDistance->text(), QStringLiteral("Home: 0.00 ft"));
    QCOMPARE(previousDistance->text(), QStringLiteral("Prev: 0.00 ft"));
    QVERIFY(std::abs(radiusEditor->value()
                     - viewModel.WpRadiusDisplay()) < 0.01);
    QVERIFY(std::abs(loiter->value()
                     - viewModel.LoiterRadiusDisplay()) < 0.01);
    radiusEditor->setValue(328.08);
    loiter->setValue(-164.04);
    QVERIFY(std::abs(viewModel.WpRadius() - 100.0) < 0.01);
    QVERIFY(std::abs(viewModel.LoiterRadius() + 50.0) < 0.01);
    viewModel.setDistUnits(QStringLiteral("Meters"));
    QCOMPARE(radiusEditor->suffix(), QStringLiteral(" m"));
    QCOMPARE(loiter->suffix(), QStringLiteral(" m"));
    QVERIFY(std::abs(radiusEditor->value() - 100.0) < 0.01);
    QVERIFY(std::abs(loiter->value() + 50.0) < 0.01);

    auto *status = panel.findChild<QLabel *>(QStringLiteral("lbl_status"));
    QVERIFY(status);
    QCOMPARE(distance->text(), QStringLiteral("Dist: 0.0000 km"));
    QCOMPARE(homeDistance->text(), QStringLiteral("Home: 0.00 m"));
    QCOMPARE(previousDistance->text(), QStringLiteral("Prev: 0.00 m"));
    viewModel.setStatus(QStringLiteral("planner status"));
    QCOMPARE(status->text(), QStringLiteral("planner status"));

    QSignalSpy zoomRequested(
        &panel, &FlightPlannerWaypointPanel::zoomLevelRequested);
    panel.setZoomRange(4, 18);
    panel.setZoomLevel(9);
    QCOMPARE(panel.zoomLevel(), 9);
    QCOMPARE(zoomRequested.count(), 0);
    zoom->setValue(10);
    QCOMPARE(zoomRequested.count(), 1);
    QCOMPARE(zoomRequested.takeFirst().at(0).toInt(), 10);

    QVERIFY(!radiusEditor->isHidden());
    QVERIFY(!loiter->isHidden());
    viewModel.setVehicleType(MAV_TYPE_QUADROTOR);
    QVERIFY(radiusEditor->isHidden());
    QVERIFY(loiter->isHidden());
    viewModel.setVehicleType(MAV_TYPE_FIXED_WING);
    QVERIFY(!radiusEditor->isHidden());
    QVERIFY(!loiter->isHidden());

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

void FlightPlannerWaypointPanelTest::commandEditorFollowsMissionType()
{
    FlightPlannerViewModel viewModel;
    FlightPlannerWaypointPanel panel(&viewModel);
    QTableView *table = panel.waypointTable();
    viewModel.setHomeLat(41.0);
    viewModel.setHomeLng(29.0);

    const auto editorCommands = [table]() {
        const QModelIndex index = table->model()->index(
            0, FlightPlannerMissionModel::CommandColumn);
        QWidget parent;
        QStyleOptionViewItem option;
        QWidget *editor = table->itemDelegateForColumn(
            FlightPlannerMissionModel::CommandColumn)->createEditor(
                &parent, option, index);
        auto *combo = qobject_cast<QComboBox *>(editor);
        Q_ASSERT(combo);
        QStringList items;
        for (int item = 0; item < combo->count(); ++item) {
            items.append(combo->itemText(item));
        }
        return items;
    };

    viewModel.AddWaypointAt(41.0, 29.0);
    QVERIFY(editorCommands().size() > 100);

    viewModel.setMissionType(QStringLiteral("Fence"));
    viewModel.AddWaypointAt(41.0, 29.0);
    const QStringList fenceCommands = editorCommands();
    QCOMPARE(fenceCommands.size(), 5);
    QVERIFY(fenceCommands.contains(WpRow::CommandNameFor(
        MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION)));
    QVERIFY(!fenceCommands.contains(WpRow::CommandNameFor(
        MAV_CMD_NAV_WAYPOINT)));

    viewModel.setMissionType(QStringLiteral("Rally"));
    viewModel.AddWaypointAt(41.0, 29.0);
    QCOMPARE(editorCommands(), QStringList{
        WpRow::CommandNameFor(MAV_CMD_NAV_RALLY_POINT)});
}

QTEST_MAIN(FlightPlannerWaypointPanelTest)
#include "test_flightplannerwaypointpanel.moc"
