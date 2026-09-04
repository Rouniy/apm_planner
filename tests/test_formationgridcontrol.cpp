#include "ui/FormationGridControl.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QWheelEvent>
#include <QtTest/QTest>

#include <cmath>

namespace
{
constexpr int kTestWidth = 848;
constexpr int kTestHeight = 856;

QImage render(FormationGridControl &control)
{
    QImage image(control.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    control.render(&image);
    return image;
}

void sendWheel(FormationGridControl &control, int delta)
{
    QWheelEvent event(QPointF(10.0, 10.0), QPointF(10.0, 10.0),
                      QPoint(), QPoint(0, delta), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&control, &event);
    QVERIFY(event.isAccepted());
}

FormationGridItem item(const QString &key, int systemId, double x, double y,
                       bool included, bool eligible, bool leader,
                       double z = 0.0)
{
    FormationGridItem result;
    result.instanceKey = key;
    result.systemId = systemId;
    result.componentId = 1;
    result.x = x;
    result.y = y;
    result.z = z;
    result.included = included;
    result.eligible = eligible;
    result.leader = leader;
    return result;
}
} // namespace

class FormationGridControlTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndMultiplicativeZoomBounds();
    void rendersLeaderIncludedAndExcludedMarkers();
    void markerCoordinatesUsePositiveXRightAndPositiveYUp();
    void dragSelectsNearestEligibleIncludedNonLeader();
    void dragClampsRoundsAndPreservesAltitude();
    void outsideMarkerCanBeDraggedBackIntoView();
    void equalDistanceKeepsFirstEligibleFollower();
};

void FormationGridControlTest::defaultsAndMultiplicativeZoomBounds()
{
    FormationGridControl control;
    QCOMPARE(control.objectName(), QStringLiteral("FormationGrid"));
    QCOMPARE(control.halfSpanMeters(), 25.0);
    QVERIFY(control.items().isEmpty());

    QSignalSpy changes(&control,
                       &FormationGridControl::halfSpanMetersChanged);
    sendWheel(control, 120);
    QCOMPARE(control.halfSpanMeters(), 20.0);
    sendWheel(control, -120);
    QCOMPARE(control.halfSpanMeters(), 25.0);
    QCOMPARE(changes.count(), 2);

    control.setHalfSpanMeters(5.0);
    sendWheel(control, 120);
    QCOMPARE(control.halfSpanMeters(), 5.0);
    control.setHalfSpanMeters(1000.0);
    sendWheel(control, -120);
    QCOMPARE(control.halfSpanMeters(), 1000.0);
    control.setHalfSpanMeters(-1.0);
    QCOMPARE(control.halfSpanMeters(), 5.0);
    control.setHalfSpanMeters(5000.0);
    QCOMPARE(control.halfSpanMeters(), 1000.0);
}

void FormationGridControlTest::rendersLeaderIncludedAndExcludedMarkers()
{
    FormationGridControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setItems({
        item(QStringLiteral("leader"), 11, -5.0, 0.0, true, true, true),
        item(QStringLiteral("included"), 22, 0.0, 0.0, true, true, false),
        item(QStringLiteral("excluded"), 33, 5.0, 0.0, false, true, false),
        item(QStringLiteral("unsupported"), 44, 0.0, 5.0, true, false, false),
    });

    const QImage image = render(control);
    QCOMPARE(image.pixelColor(control.markerPosition(0).toPoint()),
             QColor(QStringLiteral("#FFD700")));
    QCOMPARE(image.pixelColor(control.markerPosition(1).toPoint()),
             QColor(QStringLiteral("#00BFFF")));
    QCOMPARE(image.pixelColor(control.markerPosition(2).toPoint()),
             QColor(QStringLiteral("#696969")));
    const QColor unsupported = image.pixelColor(
        control.markerPosition(3).toPoint());
    QVERIFY(unsupported != QColor(QStringLiteral("#FFD700")));
    QVERIFY(unsupported != QColor(QStringLiteral("#00BFFF")));
    QVERIFY(unsupported != QColor(QStringLiteral("#696969")));
}

void FormationGridControlTest::markerCoordinatesUsePositiveXRightAndPositiveYUp()
{
    FormationGridControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setItems({
        item(QStringLiteral("origin"), 1, 0.0, 0.0, true, true, false),
        item(QStringLiteral("east"), 2, 2.0, 0.0, true, true, false),
        item(QStringLiteral("north"), 3, 0.0, 2.0, true, true, false),
    });

    const QPointF origin = control.markerPosition(0);
    const QPointF east = control.markerPosition(1);
    const QPointF north = control.markerPosition(2);
    QVERIFY(east.x() > origin.x());
    QCOMPARE(east.y(), origin.y());
    QCOMPARE(north.x(), origin.x());
    QVERIFY(north.y() < origin.y());

    const QPointF invalid = control.markerPosition(99);
    QVERIFY(std::isnan(invalid.x()));
    QVERIFY(std::isnan(invalid.y()));
}

void FormationGridControlTest::dragSelectsNearestEligibleIncludedNonLeader()
{
    FormationGridControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setItems({
        item(QStringLiteral("leader"), 1, 0.0, 0.0, true, true, true),
        item(QStringLiteral("excluded"), 2, 0.0, 0.0, false, true, false),
        item(QStringLiteral("ineligible"), 3, 0.0, 0.0, true, false, false),
        item(QStringLiteral("near"), 4, 0.5, 0.0, true, true, false, 9.0),
        item(QStringLiteral("far"), 5, 1.0, 0.0, true, true, false),
    });

    QSignalSpy dragged(&control, &FormationGridControl::itemDragged);
    const QPoint press = control.markerPosition(3).toPoint();
    const QPoint release = control.plotBounds().center().toPoint()
        + QPoint(32, -48);
    QTest::mousePress(&control, Qt::LeftButton, Qt::NoModifier, press);
    QMouseEvent move(QEvent::MouseMove, release, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&control, &move);
    QTest::mouseRelease(&control, Qt::LeftButton, Qt::NoModifier, release);

    QCOMPARE(dragged.count(), 1);
    QCOMPARE(dragged.first().at(0).toString(), QStringLiteral("near"));
    QCOMPARE(control.items().at(3).x, 2.0);
    QCOMPARE(control.items().at(3).y, 3.0);
    QCOMPARE(control.items().at(3).z, 9.0);
    QCOMPARE(control.items().at(4).x, 1.0);
}

void FormationGridControlTest::dragClampsRoundsAndPreservesAltitude()
{
    FormationGridControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setItems({
        item(QStringLiteral("follower"), 8, 0.0, 0.0,
             true, true, false, 7.25),
    });
    QSignalSpy dragged(&control, &FormationGridControl::itemDragged);
    QSignalSpy changed(&control, &FormationGridControl::itemsChanged);
    const QPoint center = control.plotBounds().center().toPoint();

    QTest::mousePress(&control, Qt::LeftButton, Qt::NoModifier, center);
    const QPoint rounded = center + QPoint(19, -37);
    QMouseEvent firstMove(QEvent::MouseMove, rounded, Qt::NoButton,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&control, &firstMove);
    QCOMPARE(control.items().first().x, 1.2);
    QCOMPARE(control.items().first().y, 2.3);
    QCOMPARE(control.items().first().z, 7.25);

    const QPoint outside = center + QPoint(10000, 10000);
    QMouseEvent secondMove(QEvent::MouseMove, outside, Qt::NoButton,
                           Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&control, &secondMove);
    QTest::mouseRelease(&control, Qt::LeftButton, Qt::NoModifier, outside);
    QCOMPARE(control.items().first().x, 25.0);
    QCOMPARE(control.items().first().y, -25.0);
    QCOMPARE(control.items().first().z, 7.25);
    QCOMPARE(dragged.count(), 2);
    QCOMPARE(changed.count(), 2);
}

void FormationGridControlTest::outsideMarkerCanBeDraggedBackIntoView()
{
    FormationGridControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setItems({
        item(QStringLiteral("outside"), 8, 100.0, 0.0,
             true, true, false),
    });
    const QPoint visibleMarker = control.markerPosition(0).toPoint();
    QVERIFY(control.plotBounds().contains(visibleMarker));

    QSignalSpy dragged(&control, &FormationGridControl::itemDragged);
    const QPoint center = control.plotBounds().center().toPoint();
    QTest::mousePress(
        &control, Qt::LeftButton, Qt::NoModifier, visibleMarker);
    QMouseEvent move(QEvent::MouseMove, center, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&control, &move);
    QTest::mouseRelease(
        &control, Qt::LeftButton, Qt::NoModifier, center);

    QCOMPARE(dragged.count(), 1);
    QCOMPARE(control.items().first().x, 0.0);
    QCOMPARE(control.items().first().y, 0.0);
}

void FormationGridControlTest::equalDistanceKeepsFirstEligibleFollower()
{
    FormationGridControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setItems({
        item(QStringLiteral("first"), 7, 0.0, 0.0, true, true, false),
        item(QStringLiteral("second"), 8, 0.0, 0.0, true, true, false),
    });
    QSignalSpy dragged(&control, &FormationGridControl::itemDragged);
    const QPoint center = control.plotBounds().center().toPoint();
    const QPoint release = center + QPoint(16, -16);
    QTest::mousePress(&control, Qt::LeftButton, Qt::NoModifier, center);
    QMouseEvent move(QEvent::MouseMove, release, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&control, &move);
    QTest::mouseRelease(&control, Qt::LeftButton, Qt::NoModifier, release);

    QCOMPARE(dragged.count(), 1);
    QCOMPARE(dragged.first().at(0).toString(), QStringLiteral("first"));
    QCOMPARE(control.items().at(0).x, 1.0);
    QCOMPARE(control.items().at(0).y, 1.0);
    QCOMPARE(control.items().at(1).x, 0.0);
    QCOMPARE(control.items().at(1).y, 0.0);
}

QTEST_MAIN(FormationGridControlTest)
#include "test_formationgridcontrol.moc"
