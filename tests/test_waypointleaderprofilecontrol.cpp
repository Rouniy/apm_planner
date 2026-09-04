#include "ui/WaypointLeaderProfileControl.h"

#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QSignalSpy>
#include <QTimer>
#include <QWidget>
#include <QtTest/QTest>

#include <cmath>
#include <limits>

namespace
{
QImage renderControl(WaypointLeaderProfileControl *control)
{
    QImage image(control->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    control->render(&painter);
    return image;
}

bool neighborhoodContains(const QImage &image, const QPointF &center,
                          const QColor &expected, int radius = 3)
{
    const int centerX = qRound(center.x());
    const int centerY = qRound(center.y());
    for (int y = centerY - radius; y <= centerY + radius; ++y) {
        for (int x = centerX - radius; x <= centerX + radius; ++x) {
            if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
                continue;
            }
            const QColor actual = image.pixelColor(x, y);
            if (qAbs(actual.red() - expected.red()) <= 2
                && qAbs(actual.green() - expected.green()) <= 2
                && qAbs(actual.blue() - expected.blue()) <= 2) {
                return true;
            }
        }
    }
    return false;
}
} // namespace

class WaypointLeaderProfileControlTest final : public QObject
{
    Q_OBJECT

private slots:
    void apiNormalizesBoundsAndPreservesExactMarkerData();
    void paintMatchesMp10ProfileAndRoleColours();
    void emptyAndExtremeBoundsPaintDeterministically();
    void lifecycleIsParentOwnedAndTimerFree();
};

void WaypointLeaderProfileControlTest::
apiNormalizesBoundsAndPreservesExactMarkerData()
{
    WaypointLeaderProfileControl control;
    QCOMPARE(control.objectName(), QStringLiteral("WaypointLeaderProfileControl"));
    QCOMPARE(control.minimumSizeHint(), QSize(260, 180));
    QCOMPARE(control.sizeHint(), QSize(620, 300));
    QVERIFY(!control.hasDrawableProfile());

    QSignalSpy profileChanged(&control,
                              &WaypointLeaderProfileControl::profileChanged);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    control.setProfile({{100.0, 25.0},
                        {nan, 12.0},
                        {0.0, 10.0},
                        {50.0, infinity},
                        {40.0, 20.0}});
    QCOMPARE(profileChanged.count(), 1);
    QCOMPARE(control.profile().size(), 3);
    QCOMPARE(control.profile().at(0).distanceM, 0.0);
    QCOMPARE(control.profile().at(1).distanceM, 40.0);
    QCOMPARE(control.profile().at(2).distanceM, 100.0);
    QVERIFY(control.hasDrawableProfile());
    control.setProfile(control.profile());
    QCOMPARE(profileChanged.count(), 1);

    QVector<SwarmWaypointLeaderProfilePoint> oversized;
    oversized.reserve(WaypointLeaderProfileControl::MaximumProfilePoints + 300);
    for (int index = 0;
         index < WaypointLeaderProfileControl::MaximumProfilePoints + 300;
         ++index) {
        oversized.append({double(index), double(index % 30)});
    }
    control.setProfile(oversized);
    QCOMPARE(control.profile().size(),
             WaypointLeaderProfileControl::MaximumProfilePoints);
    QCOMPARE(control.profile().constFirst().distanceM, 0.0);
    QCOMPARE(control.profile().constLast().distanceM,
             double(oversized.size() - 1));

    QSignalSpy markersChanged(
        &control, &WaypointLeaderProfileControl::vehicleMarkersChanged);
    const QVector<WaypointLeaderVehicleMarker> exact = {
        {WaypointLeaderVehicleRole::GroundMaster,
         QStringLiteral("ground@link-A/1:1"), -3.5, 8.25},
        {WaypointLeaderVehicleRole::AirMaster,
         QStringLiteral("air@link-B/2:1"), 45.75, 23.5},
        {WaypointLeaderVehicleRole::Follower,
         QStringLiteral("follower@link-C/3:1"), 41.25, 19.125},
        {WaypointLeaderVehicleRole::Follower,
         QStringLiteral("nonfinite"), nan, 1.0}
    };
    control.setVehicleMarkers(exact);
    QCOMPARE(markersChanged.count(), 1);
    QCOMPARE(control.vehicleMarkers().size(), 3);
    QVERIFY(control.vehicleMarkers().at(0) == exact.at(0));
    QVERIFY(control.vehicleMarkers().at(1) == exact.at(1));
    QVERIFY(control.vehicleMarkers().at(2) == exact.at(2));
    control.setVehicleMarkers(control.vehicleMarkers());
    QCOMPARE(markersChanged.count(), 1);

    QVector<WaypointLeaderVehicleMarker> tooMany;
    for (int index = 0;
         index < WaypointLeaderProfileControl::MaximumVehicleMarkers + 5;
         ++index) {
        tooMany.append({WaypointLeaderVehicleRole::Follower,
                        QStringLiteral("vehicle-%1").arg(index),
                        double(index), double(index)});
    }
    control.setVehicleMarkers(tooMany);
    QCOMPARE(control.vehicleMarkers().size(),
             WaypointLeaderProfileControl::MaximumVehicleMarkers);
    QCOMPARE(control.vehicleMarkers().constLast().label,
             QStringLiteral("vehicle-23"));
}

void WaypointLeaderProfileControlTest::
paintMatchesMp10ProfileAndRoleColours()
{
    WaypointLeaderProfileControl control;
    control.resize(720, 360);
    control.setProfile({{0.0, 10.0}, {50.0, 30.0}, {100.0, 20.0}});
    const QVector<WaypointLeaderVehicleMarker> markers = {
        {WaypointLeaderVehicleRole::GroundMaster,
         QStringLiteral("G 1:1"), 10.0, 12.0},
        {WaypointLeaderVehicleRole::AirMaster,
         QStringLiteral("A 2:1"), 50.0, 30.0},
        {WaypointLeaderVehicleRole::Follower,
         QStringLiteral("F 3:1"), 90.0, 18.0}
    };
    control.setVehicleMarkers(markers);

    QCOMPARE(control.plotBounds(), QRectF(56.0, 26.0, 648.0, 292.0));
    QCOMPARE(control.profilePointPosition(0).x(), 56.0);
    QCOMPARE(control.profilePointPosition(2).x(), 704.0);
    QVERIFY(std::isnan(control.profilePointPosition(-1).x()));
    QVERIFY(std::isnan(control.vehicleMarkerPosition(3).x()));

    const QImage image = renderControl(&control);
    QCOMPARE(image.pixelColor(1, 1), QColor(QStringLiteral("#151817")));
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            QCOMPARE(qAlpha(image.pixel(x, y)), 255);
        }
    }

    QVERIFY(neighborhoodContains(
        image, control.profilePointPosition(0),
        QColor(QStringLiteral("#FF5B72")), 4));
    QVERIFY(neighborhoodContains(
        image, control.vehicleMarkerPosition(0),
        QColor(QStringLiteral("#32CD32"))));
    QVERIFY(neighborhoodContains(
        image, control.vehicleMarkerPosition(1),
        QColor(QStringLiteral("#FFD700"))));
    QVERIFY(neighborhoodContains(
        image, control.vehicleMarkerPosition(2),
        QColor(QStringLiteral("#00BFFF"))));
}

void WaypointLeaderProfileControlTest::
emptyAndExtremeBoundsPaintDeterministically()
{
    WaypointLeaderProfileControl control;
    control.resize(620, 300);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    control.setProfile({{nan, 0.0}, {0.0, infinity}});
    control.setVehicleMarkers({
        {WaypointLeaderVehicleRole::Follower,
         QStringLiteral("bad-distance"), infinity, 1.0},
        {WaypointLeaderVehicleRole::Follower,
         QStringLiteral("bad-altitude"), 1.0, nan}
    });
    QVERIFY(control.profile().isEmpty());
    QVERIFY(control.vehicleMarkers().isEmpty());
    QVERIFY(!control.hasDrawableProfile());
    QCOMPARE(renderControl(&control), renderControl(&control));

    const double maximum = std::numeric_limits<double>::max();
    control.setProfile({{0.0, -maximum},
                        {maximum / 2.0, 0.0},
                        {maximum, maximum}});
    control.setVehicleMarkers({
        {WaypointLeaderVehicleRole::Follower,
         QStringLiteral("extreme"), maximum, -maximum}
    });
    QVERIFY(control.hasDrawableProfile());
    for (int index = 0; index < control.profile().size(); ++index) {
        const QPointF point = control.profilePointPosition(index);
        QVERIFY(std::isfinite(point.x()));
        QVERIFY(std::isfinite(point.y()));
        QVERIFY(control.plotBounds().contains(point));
    }
    const QPointF marker = control.vehicleMarkerPosition(0);
    QVERIFY(std::isfinite(marker.x()));
    QVERIFY(std::isfinite(marker.y()));
    QVERIFY(control.plotBounds().contains(marker));
    QCOMPARE(renderControl(&control), renderControl(&control));

    control.resize(1, 1);
    const QImage tiny = renderControl(&control);
    QCOMPARE(tiny.size(), QSize(1, 1));
    QCOMPARE(tiny.pixelColor(0, 0), QColor(QStringLiteral("#151817")));
}

void WaypointLeaderProfileControlTest::lifecycleIsParentOwnedAndTimerFree()
{
    auto *owner = new QWidget;
    auto *control = new WaypointLeaderProfileControl(owner);
    QPointer<WaypointLeaderProfileControl> guarded(control);
    QCOMPARE(control->parentWidget(), owner);
    QVERIFY(!control->findChild<QTimer *>());

    control->setProfile({{0.0, 10.0}, {100.0, 20.0}});
    control->show();
    QCoreApplication::processEvents();
    control->hide();
    control->setVehicleMarkers({
        {WaypointLeaderVehicleRole::AirMaster,
         QStringLiteral("2:1"), 40.0, 16.0}
    });
    QCOMPARE(control->vehicleMarkers().size(), 1);

    delete owner;
    QVERIFY(guarded.isNull());
}

QTEST_MAIN(WaypointLeaderProfileControlTest)
#include "test_waypointleaderprofilecontrol.moc"
