#include "ui/SequenceLayoutControl.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest/QTest>

namespace {
constexpr int kTestWidth = 848;
constexpr int kTestHeight = 852;

QPointF plotCenter()
{
    return QPointF(kTestWidth / 2.0, (28.0 + kTestHeight - 24.0) / 2.0);
}

QImage render(SequenceLayoutControl &control)
{
    QImage image(control.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    control.render(&image);
    return image;
}

void sendWheel(SequenceLayoutControl &control, int delta)
{
    QWheelEvent event(QPointF(10.0, 10.0), QPointF(10.0, 10.0),
                      QPoint(), QPoint(0, delta), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&control, &event);
    QVERIFY(event.isAccepted());
}
} // namespace

class SequenceLayoutControlTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndZoomBounds();
    void backgroundTransformUsesOnlySupportedSteps();
    void backgroundLoadsRendersAndClears();
    void oversizedBackgroundIsRejectedWithoutReplacingCurrentImage();
    void markersRenderEastRightNorthUpAndClampOutside();
    void nearestMarkerDragsWithCentimeterRounding();
    void equalDistanceKeepsFirstMarkerLikeMissionPlanner();
    void pressOutsideHitRadiusDoesNotDrag();
};

void SequenceLayoutControlTest::defaultsAndZoomBounds()
{
    SequenceLayoutControl control;
    QCOMPARE(control.objectName(), QStringLiteral("SequenceLayoutControl"));
    QCOMPARE(control.halfSpanMeters(), 20.0);
    QVERIFY(control.offsets().isEmpty());

    QSignalSpy changes(&control,
                       &SequenceLayoutControl::halfSpanMetersChanged);
    sendWheel(control, 120);
    QCOMPARE(control.halfSpanMeters(), 18.0);
    sendWheel(control, -120);
    QCOMPARE(control.halfSpanMeters(), 20.0);
    QCOMPARE(changes.count(), 2);

    control.setHalfSpanMeters(4.0);
    sendWheel(control, 120);
    QCOMPARE(control.halfSpanMeters(), 4.0);
    control.setHalfSpanMeters(50000.0);
    sendWheel(control, -120);
    QCOMPARE(control.halfSpanMeters(), 50000.0);

    control.setHalfSpanMeters(-100.0);
    QCOMPARE(control.halfSpanMeters(), 4.0);
    control.setHalfSpanMeters(90000.0);
    QCOMPARE(control.halfSpanMeters(), 50000.0);
}

void SequenceLayoutControlTest::backgroundTransformUsesOnlySupportedSteps()
{
    SequenceLayoutControl control;
    SequenceLayoutControl::BackgroundTransform transform =
        control.backgroundTransform();
    QCOMPARE(transform.x, 0.0);
    QCOMPARE(transform.y, 0.0);
    QCOMPARE(transform.width, 1.0);
    QCOMPARE(transform.height, 1.0);
    QCOMPARE(transform.step, 1.0);

    QSignalSpy changes(&control,
                       &SequenceLayoutControl::backgroundTransformChanged);
    control.moveBackground(-1.0, 1.0);
    control.resizeBackground(2.0, 3.0);
    transform = control.backgroundTransform();
    QCOMPARE(transform.x, -1.0);
    QCOMPARE(transform.y, 1.0);
    QCOMPARE(transform.width, 3.0);
    QCOMPARE(transform.height, 4.0);

    control.setBackgroundStep(0.1);
    control.moveBackground(1.0, -1.0);
    control.resizeBackground(-100.0, -100.0);
    transform = control.backgroundTransform();
    QCOMPARE(transform.x, -0.9);
    QCOMPARE(transform.y, 0.9);
    QCOMPARE(transform.width, 0.1);
    QCOMPARE(transform.height, 0.1);
    QCOMPARE(transform.step, 0.1);

    control.setBackgroundStep(0.5);
    QCOMPARE(control.backgroundStep(), 1.0);
    QVERIFY(changes.count() >= 5);

    // Image placement is intentionally session-only: another editor starts
    // with the MP10 defaults instead of inheriting this control's transform.
    SequenceLayoutControl anotherSession;
    transform = anotherSession.backgroundTransform();
    QCOMPARE(transform.x, 0.0);
    QCOMPARE(transform.y, 0.0);
    QCOMPARE(transform.width, 1.0);
    QCOMPARE(transform.height, 1.0);
    QCOMPARE(transform.step, 1.0);
}

void SequenceLayoutControlTest::backgroundLoadsRendersAndClears()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("background.png"));
    QImage source(16, 16, QImage::Format_RGB32);
    source.fill(QColor(QStringLiteral("#FF00FF")));
    QVERIFY(source.save(path));

    SequenceLayoutControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setHalfSpanMeters(20.0);
    QSignalSpy backgroundChanges(&control,
                                 &SequenceLayoutControl::backgroundChanged);
    QString error;
    QVERIFY2(control.loadBackground(path, &error), qPrintable(error));
    QVERIFY(control.hasBackground());
    QCOMPARE(backgroundChanges.count(), 1);
    QCOMPARE(backgroundChanges.first().at(0).toBool(), true);

    const QImage loaded = render(control);
    const QPoint center = plotCenter().toPoint();
    QCOMPARE(loaded.pixelColor(center + QPoint(4, 4)),
             QColor(QStringLiteral("#FF00FF")));

    QVERIFY(!control.loadBackground(
        directory.filePath(QStringLiteral("missing.png")), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(control.hasBackground());
    QCOMPARE(backgroundChanges.count(), 1);

    control.clearBackground();
    QVERIFY(!control.hasBackground());
    QCOMPARE(backgroundChanges.count(), 2);
    QCOMPARE(backgroundChanges.last().at(0).toBool(), false);
    const QImage cleared = render(control);
    QVERIFY(cleared.pixelColor(center + QPoint(4, 4))
            != QColor(QStringLiteral("#FF00FF")));
}

void SequenceLayoutControlTest::oversizedBackgroundIsRejectedWithoutReplacingCurrentImage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString safePath = directory.filePath(QStringLiteral("safe.png"));
    const QString oversizedPath = directory.filePath(QStringLiteral("oversized.png"));
    QImage safe(16, 16, QImage::Format_RGB32);
    safe.fill(Qt::green);
    QVERIFY(safe.save(safePath));
    QImage oversized(4097, 1, QImage::Format_RGB32);
    oversized.fill(Qt::red);
    QVERIFY(oversized.save(oversizedPath));

    SequenceLayoutControl control;
    QString error;
    QVERIFY(control.loadBackground(safePath, &error));
    QVERIFY(!control.loadBackground(oversizedPath, &error));
    QVERIFY(error.contains(QStringLiteral("safety limit")));
    QVERIFY(control.hasBackground());
}

void SequenceLayoutControlTest::markersRenderEastRightNorthUpAndClampOutside()
{
    SequenceLayoutControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setHalfSpanMeters(4.0);
    control.setOffsets({
        {11, 1.0, 0.0, 3.5},
        {22, 0.0, 1.0, -2.0},
        {33, 100.0, 0.0, 8.0},
    });

    const QImage image = render(control);
    const QPoint center = plotCenter().toPoint();
    const QColor marker(QStringLiteral("#00BFFF"));
    const QColor outside(QStringLiteral("#FF4500"));
    QCOMPARE(image.pixelColor(center + QPoint(100, 0)), marker);
    QCOMPARE(image.pixelColor(center + QPoint(0, -100)), marker);
    QCOMPARE(image.pixelColor(QPoint(kTestWidth - 24 - 8,
                                     center.y())), outside);
    QCOMPARE(image.pixelColor(QPoint(0, 0)),
             QColor(QStringLiteral("#191B1D")));
}

void SequenceLayoutControlTest::nearestMarkerDragsWithCentimeterRounding()
{
    SequenceLayoutControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setHalfSpanMeters(4.0);
    control.setOffsets({
        {41, 0.0, 0.0, 7.25},
        {42, 0.1, 0.0, 9.0},
    });

    QSignalSpy dragged(&control, &SequenceLayoutControl::offsetDragged);
    QSignalSpy changed(&control, &SequenceLayoutControl::offsetsChanged);
    const QPoint center = plotCenter().toPoint();
    // Both markers are inside the 18-pixel hit radius; pressing exactly on the
    // second marker proves the nearest item, rather than the first one, moves.
    const QPoint press = center + QPoint(10, 0);
    const QPoint release = center + QPoint(123, -234);
    QTest::mousePress(&control, Qt::LeftButton, Qt::NoModifier, press);
    QMouseEvent move(QEvent::MouseMove, release, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&control, &move);
    QTest::mouseRelease(&control, Qt::LeftButton, Qt::NoModifier, release);

    QCOMPARE(dragged.count(), 1);
    QCOMPARE(dragged.first().at(0).toInt(), 42);
    QCOMPARE(dragged.first().at(1).toDouble(), 1.23);
    QCOMPARE(dragged.first().at(2).toDouble(), 2.34);
    QCOMPARE(changed.count(), 1);
    const QVector<SequenceLayoutOffset> offsets = control.offsets();
    QCOMPARE(offsets.at(0).x, 0.0);
    QCOMPARE(offsets.at(0).y, 0.0);
    QCOMPARE(offsets.at(1).x, 1.23);
    QCOMPARE(offsets.at(1).y, 2.34);
    QCOMPARE(offsets.at(1).z, 9.0);
}

void SequenceLayoutControlTest::equalDistanceKeepsFirstMarkerLikeMissionPlanner()
{
    SequenceLayoutControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setHalfSpanMeters(4.0);
    control.setOffsets({
        {41, 0.0, 0.0, 7.25},
        {42, 0.0, 0.0, 9.0},
    });

    QSignalSpy dragged(&control, &SequenceLayoutControl::offsetDragged);
    const QPoint center = plotCenter().toPoint();
    const QPoint release = center + QPoint(100, -100);
    QTest::mousePress(&control, Qt::LeftButton, Qt::NoModifier, center);
    QMouseEvent move(QEvent::MouseMove, release, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&control, &move);
    QTest::mouseRelease(&control, Qt::LeftButton, Qt::NoModifier, release);

    QCOMPARE(dragged.count(), 1);
    QCOMPARE(dragged.first().at(0).toInt(), 41);
    QCOMPARE(control.offsets().at(0).x, 1.0);
    QCOMPARE(control.offsets().at(0).y, 1.0);
    QCOMPARE(control.offsets().at(1).x, 0.0);
    QCOMPARE(control.offsets().at(1).y, 0.0);
}

void SequenceLayoutControlTest::pressOutsideHitRadiusDoesNotDrag()
{
    SequenceLayoutControl control;
    control.resize(kTestWidth, kTestHeight);
    control.setHalfSpanMeters(4.0);
    control.setOffsets({{7, 0.0, 0.0, 1.0}});
    QSignalSpy dragged(&control, &SequenceLayoutControl::offsetDragged);

    const QPoint center = plotCenter().toPoint();
    const QPoint press = center + QPoint(19, 0);
    const QPoint release = center + QPoint(100, -100);
    QTest::mousePress(&control, Qt::LeftButton, Qt::NoModifier, press);
    QMouseEvent move(QEvent::MouseMove, release, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&control, &move);
    QTest::mouseRelease(&control, Qt::LeftButton, Qt::NoModifier, release);

    QCOMPARE(dragged.count(), 0);
    QCOMPARE(control.offsets().first().x, 0.0);
    QCOMPARE(control.offsets().first().y, 0.0);
}

QTEST_MAIN(SequenceLayoutControlTest)
#include "test_sequencelayoutcontrol.moc"
