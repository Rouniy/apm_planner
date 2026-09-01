#include "ui/FlightDataView.h"
#include "ui/FlightPlannerView.h"

#include <QApplication>
#include <QAction>
#include <QLabel>
#include <QStackedWidget>
#if !defined(APM_HAS_KDDOCKWIDGETS)
#include <QDockWidget>
#include <QMainWindow>
#endif
#include <QtTest/QTest>

class FlightViewsTest final : public QObject
{
    Q_OBJECT

private slots:
    void flightDataUsesStableMissionPlannerNames();
    void flightPlannerUsesStableMissionPlannerNames();
    void flightViewsRenderEveryDefaultPanel();
    void flightViewsRemainVisibleAfterStackSwitch();
};

void FlightViewsTest::flightDataUsesStableMissionPlannerNames()
{
    FlightDataView view;
    QCOMPARE(view.objectName(), QStringLiteral("FlightDataView"));

    QVERIFY(view.setHudWidget(new QLabel(QStringLiteral("hud"))));
    QVERIFY(view.setMapWidget(new QLabel(QStringLiteral("map"))));
    QVERIFY(view.setInfoView(new QLabel(QStringLiteral("info"))));
    auto *duplicate = new QLabel(QStringLiteral("duplicate"));
    QVERIFY(!view.setMapWidget(duplicate));
    delete duplicate;

    QCOMPARE(view.panelIds(),
             QStringList({FlightDataView::hudPanelId(),
                          FlightDataView::mapPanelId(),
                          FlightDataView::infoPanelId()}));
    QVERIFY(view.panelToggleAction(FlightDataView::infoPanelId()));
    const QByteArray layout = view.saveLayout();
    QVERIFY(!layout.isEmpty());
    QVERIFY(view.restoreLayout(layout));
}

void FlightViewsTest::flightPlannerUsesStableMissionPlannerNames()
{
    FlightPlannerView view;
    QCOMPARE(view.objectName(), QStringLiteral("FlightPlannerView"));

    auto *map = new QLabel(QStringLiteral("map"));
    auto *waypoints = new QLabel(QStringLiteral("waypoints"));
    auto *actions = new QLabel(QStringLiteral("actions"));
    QVERIFY(view.setMapWidget(map));
    QVERIFY(view.setWaypointPanel(waypoints));
    QVERIFY(view.setActionPanel(actions));

    QCOMPARE(view.panelIds(),
             QStringList({FlightPlannerView::mapPanelId(),
                          FlightPlannerView::waypointPanelId(),
                          FlightPlannerView::actionPanelId()}));
    QVERIFY(view.setPanelVisible(FlightPlannerView::actionPanelId(), false));
    QVERIFY(view.setPanelVisible(FlightPlannerView::waypointPanelId(), false));
    QVERIFY(!view.setPanelVisible(FlightPlannerView::mapPanelId(), false));
    const QByteArray layout = view.saveLayout();
    QVERIFY(!layout.isEmpty());
    QVERIFY(view.restoreLayout(layout));

    // Old versions allowed every dock to be closed and persisted a completely
    // white page. A restored all-closed layout must reopen the full surface.
#if defined(APM_HAS_KDDOCKWIDGETS)
    for (const QString &panelId : view.panelIds()) {
        view.panelToggleAction(panelId)->setChecked(false);
    }
#else
    for (QWidget *content : {static_cast<QWidget *>(map),
                             static_cast<QWidget *>(waypoints),
                             static_cast<QWidget *>(actions)}) {
        auto *dock = qobject_cast<QDockWidget *>(content->parentWidget());
        QVERIFY(dock);
        dock->hide();
    }
#endif
    const QByteArray allClosedLayout = view.saveLayout();
    QVERIFY(!allClosedLayout.isEmpty());
    QVERIFY(view.restoreLayout(allClosedLayout));
    for (const QString &panelId : view.panelIds()) {
        QVERIFY(view.isPanelOpen(panelId));
    }
}

void FlightViewsTest::flightViewsRenderEveryDefaultPanel()
{
    FlightDataView dataView;
    dataView.resize(1280, 800);
    auto *dataMap = new QLabel(QStringLiteral("map"));
    auto *hud = new QLabel(QStringLiteral("hud"));
    auto *info = new QLabel(QStringLiteral("info"));
    QVERIFY(dataView.setHudWidget(hud));
    QVERIFY(dataView.setMapWidget(dataMap));
    QVERIFY(dataView.setInfoView(info));
    dataView.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dataView));
    QCoreApplication::processEvents();

    for (QWidget *panel : {static_cast<QWidget *>(dataMap),
                           static_cast<QWidget *>(hud),
                           static_cast<QWidget *>(info)}) {
        QVERIFY2(panel->isVisibleTo(&dataView), panel->objectName().toUtf8());
        QVERIFY2(panel->width() > 0 && panel->height() > 0,
                 panel->objectName().toUtf8());
#if !defined(APM_HAS_KDDOCKWIDGETS)
        auto *dock = qobject_cast<QDockWidget *>(panel->parentWidget());
        QVERIFY(dock);
        QVERIFY(!dock->isFloating());
#endif
    }

    const QPoint hudCenter = hud->mapTo(&dataView, hud->rect().center());
    const QPoint infoCenter = info->mapTo(&dataView, info->rect().center());
    const QPoint mapCenter = dataMap->mapTo(&dataView, dataMap->rect().center());
    QVERIFY(hudCenter.x() < mapCenter.x());
    QVERIFY(infoCenter.x() < mapCenter.x());
    QVERIFY(hudCenter.y() < infoCenter.y());
    FlightPlannerView plannerView;
    plannerView.resize(1280, 800);
    auto *plannerMap = new QLabel(QStringLiteral("map"));
    auto *waypoints = new QLabel(QStringLiteral("waypoints"));
    auto *actions = new QLabel(QStringLiteral("actions"));
    QVERIFY(plannerView.setMapWidget(plannerMap));
    QVERIFY(plannerView.setWaypointPanel(waypoints));
    QVERIFY(plannerView.setActionPanel(actions));
    plannerView.show();
    QVERIFY(QTest::qWaitForWindowExposed(&plannerView));
    QCoreApplication::processEvents();

    for (QWidget *panel : {static_cast<QWidget *>(plannerMap),
                           static_cast<QWidget *>(waypoints),
                           static_cast<QWidget *>(actions)}) {
        QVERIFY2(panel->isVisibleTo(&plannerView), panel->objectName().toUtf8());
        QVERIFY2(panel->width() > 0 && panel->height() > 0,
                 panel->objectName().toUtf8());
#if !defined(APM_HAS_KDDOCKWIDGETS)
        auto *dock = qobject_cast<QDockWidget *>(panel->parentWidget());
        QVERIFY(dock);
        QVERIFY(!dock->isFloating());
#endif
    }

#if !defined(APM_HAS_KDDOCKWIDGETS)
    auto *dataHost = dataView.findChild<QMainWindow *>(
        QStringLiteral("FlightDataViewFallbackDockHost"));
    auto *plannerHost = plannerView.findChild<QMainWindow *>(
        QStringLiteral("FlightPlannerViewFallbackDockHost"));
    QVERIFY(dataHost);
    QVERIFY(plannerHost);
    QVERIFY(!dataHost->isWindow());
    QVERIFY(!plannerHost->isWindow());
#endif
}

void FlightViewsTest::flightViewsRemainVisibleAfterStackSwitch()
{
    QStackedWidget stack;
    stack.resize(1280, 800);

    auto *dataView = new FlightDataView;
    auto *hud = new QLabel(QStringLiteral("hud"));
    auto *dataMap = new QLabel(QStringLiteral("map"));
    auto *info = new QLabel(QStringLiteral("info"));
    QVERIFY(dataView->setHudWidget(hud));
    QVERIFY(dataView->setMapWidget(dataMap));
    QVERIFY(dataView->setInfoView(info));
    stack.addWidget(dataView);

    auto *plannerView = new FlightPlannerView;
    auto *plannerMap = new QLabel(QStringLiteral("map"));
    auto *waypoints = new QLabel(QStringLiteral("waypoints"));
    auto *actions = new QLabel(QStringLiteral("actions"));
    QVERIFY(plannerView->setMapWidget(plannerMap));
    QVERIFY(plannerView->setWaypointPanel(waypoints));
    QVERIFY(plannerView->setActionPanel(actions));
    stack.addWidget(plannerView);

    stack.setCurrentWidget(dataView);
    stack.show();
    QVERIFY(QTest::qWaitForWindowExposed(&stack));
    QCoreApplication::processEvents();

    const auto verifySurface = [&stack](QWidget *page,
                                        const QList<QWidget *> &panels) {
        QCOMPARE(stack.currentWidget(), page);
        for (QWidget *panel : panels) {
            QVERIFY2(panel->isVisibleTo(&stack), panel->objectName().toUtf8());
            QVERIFY2(panel->width() > 0 && panel->height() > 0,
                     panel->objectName().toUtf8());
        }
    };

    verifySurface(dataView, {hud, dataMap, info});
    stack.setCurrentWidget(plannerView);
    QCoreApplication::processEvents();
    verifySurface(plannerView, {plannerMap, waypoints, actions});
    stack.setCurrentWidget(dataView);
    QCoreApplication::processEvents();
    verifySurface(dataView, {hud, dataMap, info});
}

QTEST_MAIN(FlightViewsTest)
#include "test_flightviews.moc"
