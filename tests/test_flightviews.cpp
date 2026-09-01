#include "ui/FlightDataView.h"
#include "ui/FlightPlannerView.h"

#include <QLabel>
#include <QtTest/QTest>

class FlightViewsTest final : public QObject
{
    Q_OBJECT

private slots:
    void flightDataUsesStableMissionPlannerNames();
    void flightPlannerUsesStableMissionPlannerNames();
};

void FlightViewsTest::flightDataUsesStableMissionPlannerNames()
{
    FlightDataView view;
    QCOMPARE(view.objectName(), QStringLiteral("FlightDataView"));

    QVERIFY(view.setMapWidget(new QLabel(QStringLiteral("map"))));
    QVERIFY(view.setPrimaryFlightDisplay(new QLabel(QStringLiteral("pfd"))));
    QVERIFY(view.setInfoView(new QLabel(QStringLiteral("info"))));
    auto *duplicate = new QLabel(QStringLiteral("duplicate"));
    QVERIFY(!view.setMapWidget(duplicate));
    delete duplicate;

    QCOMPARE(view.panelIds(),
             QStringList({FlightDataView::mapPanelId(),
                          FlightDataView::primaryFlightDisplayPanelId(),
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

    QVERIFY(view.setMapWidget(new QLabel(QStringLiteral("map"))));
    QVERIFY(view.setWaypointPanel(new QLabel(QStringLiteral("waypoints"))));
    QVERIFY(view.setActionPanel(new QLabel(QStringLiteral("actions"))));

    QCOMPARE(view.panelIds(),
             QStringList({FlightPlannerView::mapPanelId(),
                          FlightPlannerView::waypointPanelId(),
                          FlightPlannerView::actionPanelId()}));
    QVERIFY(view.setPanelVisible(FlightPlannerView::actionPanelId(), false));
    const QByteArray layout = view.saveLayout();
    QVERIFY(!layout.isEmpty());
    QVERIFY(view.restoreLayout(layout));
}

QTEST_MAIN(FlightViewsTest)
#include "test_flightviews.moc"
