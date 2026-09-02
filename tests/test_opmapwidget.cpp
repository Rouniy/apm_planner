#include "opmapwidget.h"
#include "opmaps.h"
#include "waypointitem.h"

#include <QGraphicsScene>
#include <QSignalSpy>
#include <QtTest/QTest>

namespace {
class InspectableMapWidget final : public mapcontrol::OPMapWidget
{
public:
    QRectF mapRect() const
    {
        // MapGraphicItem adds a one-pixel bounding margin around maprect.
        return map->boundingRect().adjusted(1, 1, -1, -1);
    }

    mapcontrol::WayPointItem *addWaypoint(double altitude)
    {
        auto *item = new mapcontrol::WayPointItem(
            internals::PointLatLng(47.0, 8.0), altitude, map, this,
            QStringLiteral("Unit test"));
        item->setParentItem(map);
        return item;
    }
};
}

class OPMapWidgetTest final : public QObject
{
    Q_OBJECT

private slots:
    void firstShowAndLaterResizeUseCurrentSceneGeometry();
    void waypointTooltipConvertsAltitudePresentationOnly();
};

void OPMapWidgetTest::firstShowAndLaterResizeUseCurrentSceneGeometry()
{
    core::OPMaps *maps = core::OPMaps::Instance();
    const core::AccessMode::Types previousMode = maps->GetAccessMode();
    maps->setAccessMode(core::AccessMode::CacheOnly);

    {
        InspectableMapWidget widget;
        widget.resize(640, 480);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        QCoreApplication::processEvents();

        QCOMPARE(widget.mapRect().size().toSize(),
                 widget.scene()->sceneRect().size().toSize());

        widget.resize(900, 620);
        QTRY_COMPARE(widget.mapRect().size().toSize(),
                     widget.scene()->sceneRect().size().toSize());

        widget.hide();
        widget.show();
        QCoreApplication::processEvents();
        QCOMPARE(widget.mapRect().size().toSize(),
                 widget.scene()->sceneRect().size().toSize());
    }

    maps->setAccessMode(previousMode);
}

void OPMapWidgetTest::waypointTooltipConvertsAltitudePresentationOnly()
{
    InspectableMapWidget widget;
    mapcontrol::WayPointItem *waypoint = widget.addWaypoint(30.48);
    QVERIFY(waypoint);
    QSignalSpy valuesChanged(
        waypoint, &mapcontrol::WayPointItem::WPValuesChanged);
    QCOMPARE(waypoint->Altitude(), 30.48);
    QVERIFY(waypoint->toolTip().contains(QStringLiteral("Altitude: 30.48 m")));

    waypoint->SetAltitudePresentation(3.280839895013123,
                                      QStringLiteral("ft"));
    QCOMPARE(waypoint->Altitude(), 30.48);
    QVERIFY(waypoint->toolTip().contains(QStringLiteral("Altitude: 100 ft")));
    QCOMPARE(valuesChanged.count(), 0);

    waypoint->SetAltitude(60.96);
    QCOMPARE(waypoint->Altitude(), 60.96);
    QVERIFY(waypoint->toolTip().contains(QStringLiteral("Altitude: 200 ft")));
    QCOMPARE(valuesChanged.count(), 1);

    waypoint->SetAltitudePresentation(1.0, QStringLiteral("m"));
    QCOMPARE(waypoint->Altitude(), 60.96);
    QVERIFY(waypoint->toolTip().contains(QStringLiteral("Altitude: 60.96 m")));
    QCOMPARE(valuesChanged.count(), 1);

    const QString tooltip = waypoint->toolTip();
    waypoint->SetAltitudePresentation(0.0, QStringLiteral("invalid"));
    QCOMPARE(waypoint->toolTip(), tooltip);
}

QTEST_MAIN(OPMapWidgetTest)
#include "test_opmapwidget.moc"
