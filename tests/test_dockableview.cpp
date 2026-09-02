#include "ui/docking/DockableView.h"

#include <QAction>
#include <QDockWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QSignalSpy>
#include <QSplitter>
#include <QTabWidget>
#include <QTest>

class DockableViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void createsFixedSplitterAndTabbedLayout();
    void honorsAllRelativeSplitDirections();
    void createsNestedMissionPlannerSplitters();
    void rejectsForeignCorruptAndUnsafeLayouts();
};

void DockableViewTest::createsFixedSplitterAndTabbedLayout()
{
    DockableView view(QStringLiteral("flight-data"));
    auto *map = new QLabel(QStringLiteral("Map"));
    auto *hud = new QLabel(QStringLiteral("HUD"));
    auto *quick = new QLabel(QStringLiteral("Quick"));

    QVERIFY(view.addPanel(QStringLiteral("FdMap"), QStringLiteral("Map"), map,
                          DockableView::PanelLocation::Left,
                          QString(), QSize(), true));
    QVERIFY(view.addPanel(QStringLiteral("HudHost"), QStringLiteral("HUD"), hud,
                          DockableView::PanelLocation::Right,
                          QStringLiteral("FdMap"), QSize(320, 0)));
    QVERIFY(view.addPanel(QStringLiteral("QuickHost"), QStringLiteral("Quick"),
                          quick, DockableView::PanelLocation::Tabbed,
                          QStringLiteral("HudHost")));
    auto *duplicate = new QLabel;
    QVERIFY(!view.addPanel(QStringLiteral("FdMap"), QStringLiteral("Duplicate"),
                           duplicate, DockableView::PanelLocation::Left));
    delete duplicate;

    QCOMPARE(view.panelIds(),
             QStringList({QStringLiteral("FdMap"),
                          QStringLiteral("HudHost"),
                          QStringLiteral("QuickHost")}));
    QCOMPARE(map->objectName(), QStringLiteral("FdMap"));
    QCOMPARE(map->property("dockId").toString(), QStringLiteral("FdMap"));
    QCOMPARE(map->parentWidget()->objectName(), QStringLiteral("FdMap"));
    QVERIFY(view.findChildren<QDockWidget *>().isEmpty());
    QVERIFY(view.findChildren<QMainWindow *>().isEmpty());

    const QList<QSplitter *> splitters = view.findChildren<QSplitter *>();
    QCOMPARE(splitters.size(), 1);
    QCOMPARE(splitters.constFirst()->orientation(), Qt::Horizontal);
    const QList<QTabWidget *> tabs = view.findChildren<QTabWidget *>();
    QCOMPARE(tabs.size(), 1);
    QCOMPARE(tabs.constFirst()->count(), 2);
    QCOMPARE(tabs.constFirst()->tabText(0), QStringLiteral("HUD"));
    QCOMPARE(tabs.constFirst()->tabText(1), QStringLiteral("Quick"));

    QAction *quickAction = view.panelToggleAction(QStringLiteral("QuickHost"));
    QVERIFY(quickAction);
    QVERIFY(quickAction->isCheckable());
    QVERIFY(quickAction->isChecked());
    quickAction->setChecked(false);
    QVERIFY(!view.isPanelOpen(QStringLiteral("QuickHost")));
    QVERIFY(!quickAction->isChecked());
    quickAction->setChecked(true);
    QVERIFY(view.isPanelOpen(QStringLiteral("QuickHost")));

    QAction *mapAction = view.panelToggleAction(QStringLiteral("FdMap"));
    QVERIFY(mapAction);
    mapAction->setChecked(false);
    QVERIFY(mapAction->isChecked());
    QVERIFY(view.isPanelDocked(QStringLiteral("FdMap")));

    QVERIFY(view.setPanelVisible(QStringLiteral("QuickHost"), false));
    QVERIFY(view.setPanelVisible(QStringLiteral("HudHost"), false));
    QVERIFY(!view.setPanelVisible(QStringLiteral("FdMap"), false));
    QVERIFY(view.setPanelVisible(QStringLiteral("HudHost"), true));
    QVERIFY(view.setPanelVisible(QStringLiteral("QuickHost"), true));
    QVERIFY(view.setPanelSizeWeights(
        QStringList{QStringLiteral("FdMap"), QStringLiteral("HudHost")},
        QList<int>{3, 2}, Qt::Horizontal));

    QVERIFY(view.setPanelVisible(QStringLiteral("QuickHost"), false));
    const QByteArray saved = view.saveLayout();
    QVERIFY(!saved.isEmpty());
    const QJsonObject envelope = QJsonDocument::fromJson(saved).object();
    QCOMPARE(envelope.value(QStringLiteral("schema")).toString(),
             QStringLiteral("apmplanner-fixed-panel-layout"));
    QCOMPARE(envelope.value(QStringLiteral("version")).toInt(), 2);
    QCOMPARE(envelope.value(QStringLiteral("viewId")).toString(),
             QStringLiteral("flight-data"));
    QCOMPARE(envelope.value(QStringLiteral("panels")).toArray().size(), 3);
    QVERIFY(envelope.value(QStringLiteral("layout")).isObject());

    QVERIFY(view.setPanelVisible(QStringLiteral("QuickHost"), true));
    QVERIFY(view.restoreLayout(saved));
    QVERIFY(!view.isPanelOpen(QStringLiteral("QuickHost")));
    QVERIFY(view.isPanelOpen(QStringLiteral("FdMap")));
}

void DockableViewTest::honorsAllRelativeSplitDirections()
{
    DockableView view(QStringLiteral("relative-layout"));
    QVERIFY(view.addPanel(QStringLiteral("Center"), QStringLiteral("Center"),
                          new QLabel, DockableView::PanelLocation::Left));
    QVERIFY(view.addPanel(QStringLiteral("Left"), QStringLiteral("Left"),
                          new QLabel, DockableView::PanelLocation::Left,
                          QStringLiteral("Center")));
    QVERIFY(view.addPanel(QStringLiteral("Right"), QStringLiteral("Right"),
                          new QLabel, DockableView::PanelLocation::Right,
                          QStringLiteral("Center")));
    QVERIFY(view.addPanel(QStringLiteral("Top"), QStringLiteral("Top"),
                          new QLabel, DockableView::PanelLocation::Top,
                          QStringLiteral("Center")));
    QVERIFY(view.addPanel(QStringLiteral("Bottom"), QStringLiteral("Bottom"),
                          new QLabel, DockableView::PanelLocation::Bottom,
                          QStringLiteral("Center")));

    QSplitter *horizontal = nullptr;
    QSplitter *vertical = nullptr;
    for (QSplitter *splitter : view.findChildren<QSplitter *>()) {
        if (splitter->orientation() == Qt::Horizontal) {
            horizontal = splitter;
        } else {
            vertical = splitter;
        }
    }
    QVERIFY(horizontal);
    QVERIFY(vertical);
    QCOMPARE(horizontal->count(), 3);
    QCOMPARE(vertical->count(), 3);
    QCOMPARE(horizontal->widget(1), static_cast<QWidget *>(vertical));
    QCOMPARE(horizontal->widget(0)->objectName(), QStringLiteral("Left"));
    QCOMPARE(horizontal->widget(2)->objectName(), QStringLiteral("Right"));
    QCOMPARE(vertical->widget(0)->objectName(), QStringLiteral("Top"));
    QCOMPARE(vertical->widget(1)->objectName(), QStringLiteral("Center"));
    QCOMPARE(vertical->widget(2)->objectName(), QStringLiteral("Bottom"));
}

void DockableViewTest::createsNestedMissionPlannerSplitters()
{
    DockableView view(QStringLiteral("FlightPlannerView"));
    QVERIFY(view.addPanel(QStringLiteral("Map"), QStringLiteral("Map"),
                          new QLabel, DockableView::PanelLocation::Left,
                          QString(), QSize(932, 430), true));
    QVERIFY(view.addPanel(QStringLiteral("WaypointPanel"),
                          QStringLiteral("Waypoints"), new QLabel,
                          DockableView::PanelLocation::Bottom,
                          QStringLiteral("Map"), QSize(932, 210)));
    QVERIFY(view.addPanel(QStringLiteral("ActionPanel"),
                          QStringLiteral("Actions"), new QLabel,
                          DockableView::PanelLocation::Right,
                          QStringLiteral("Map"), QSize(168, 640)));

    auto *horizontal =
        view.findChild<QSplitter *>(QStringLiteral("HorizontalDockSplitter"));
    auto *vertical =
        view.findChild<QSplitter *>(QStringLiteral("VerticalDockSplitter"));
    QVERIFY(horizontal);
    QVERIFY(vertical);
    QCOMPARE(horizontal->orientation(), Qt::Horizontal);
    QCOMPARE(vertical->orientation(), Qt::Vertical);
    QCOMPARE(horizontal->handleWidth(), 4);
    QCOMPARE(vertical->handleWidth(), 4);
    QCOMPARE(vertical->widget(0), static_cast<QWidget *>(horizontal));

    QVERIFY(view.setPanelSizeWeights(
        QStringList{QStringLiteral("Map"), QStringLiteral("ActionPanel")},
        QList<int>{932, 168}, Qt::Horizontal));
    QVERIFY(view.setPanelSizeWeights(
        QStringList{QStringLiteral("Map"), QStringLiteral("WaypointPanel")},
        QList<int>{430, 210}, Qt::Vertical));
    QVERIFY(!view.setPanelSizeWeights(
        QStringList{QStringLiteral("Map"), QStringLiteral("ActionPanel")},
        QList<int>{1, 0}, Qt::Horizontal));
}

void DockableViewTest::rejectsForeignCorruptAndUnsafeLayouts()
{
    DockableView view(QStringLiteral("flight-planner"));
    QVERIFY(view.addPanel(QStringLiteral("Map"), QStringLiteral("Map"),
                          new QLabel, DockableView::PanelLocation::Left,
                          QString(), QSize(), true));
    QVERIFY(view.addPanel(QStringLiteral("Actions"), QStringLiteral("Actions"),
                          new QLabel, DockableView::PanelLocation::Right,
                          QStringLiteral("Map")));
    QSignalSpy rejected(&view, &DockableView::layoutRestoreRejected);

    QVERIFY(view.setPanelVisible(QStringLiteral("Actions"), false));
    QVERIFY(!view.restoreLayout(QByteArrayLiteral("not-json")));
    QCOMPARE(rejected.count(), 1);
    QVERIFY(view.isPanelOpen(QStringLiteral("Map")));
    QVERIFY(view.isPanelOpen(QStringLiteral("Actions")));

    QJsonObject foreign = QJsonDocument::fromJson(view.saveLayout()).object();
    foreign.insert(QStringLiteral("viewId"), QStringLiteral("flight-data"));
    QVERIFY(!view.restoreLayout(
        QJsonDocument(foreign).toJson(QJsonDocument::Compact)));
    QCOMPARE(rejected.count(), 2);

    QJsonObject obsolete = QJsonDocument::fromJson(view.saveLayout()).object();
    obsolete.insert(QStringLiteral("schema"),
                    QStringLiteral("apmplanner-qt-dock-layout"));
    obsolete.insert(QStringLiteral("version"), 1);
    QVERIFY(!view.restoreLayout(
        QJsonDocument(obsolete).toJson(QJsonDocument::Compact)));
    QCOMPARE(rejected.count(), 3);

    QJsonObject allHidden = QJsonDocument::fromJson(view.saveLayout()).object();
    QJsonArray states = allHidden.value(QStringLiteral("panelStates")).toArray();
    for (int index = 0; index < states.size(); ++index) {
        QJsonObject state = states.at(index).toObject();
        state.insert(QStringLiteral("open"), false);
        states.replace(index, state);
    }
    allHidden.insert(QStringLiteral("panelStates"), states);
    QVERIFY(!view.restoreLayout(
        QJsonDocument(allHidden).toJson(QJsonDocument::Compact)));
    QCOMPARE(rejected.count(), 4);
    for (const QString &panelId : view.panelIds()) {
        QVERIFY(view.isPanelOpen(panelId));
    }

    QJsonObject corruptTopology =
        QJsonDocument::fromJson(view.saveLayout()).object();
    QJsonObject tree = corruptTopology.value(QStringLiteral("layout")).toObject();
    tree.insert(QStringLiteral("orientation"), QStringLiteral("vertical"));
    corruptTopology.insert(QStringLiteral("layout"), tree);
    QVERIFY(!view.restoreLayout(
        QJsonDocument(corruptTopology).toJson(QJsonDocument::Compact)));
    QCOMPARE(rejected.count(), 5);

    const QByteArray beforeNewPanel = view.saveLayout();
    QVERIFY(view.addPanel(QStringLiteral("WaypointPanel"),
                          QStringLiteral("Mission"), new QLabel,
                          DockableView::PanelLocation::Bottom,
                          QStringLiteral("Map")));
    QVERIFY(!view.restoreLayout(beforeNewPanel));
    QCOMPARE(rejected.count(), 6);
    for (const QString &panelId : view.panelIds()) {
        QVERIFY(view.isPanelOpen(panelId));
    }
}

QTEST_MAIN(DockableViewTest)
#include "test_dockableview.moc"
