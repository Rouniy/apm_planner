#include <QtTest>

#include "ui/configuration/ConfigOSDLayoutCanvas.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalSpy>

namespace {

ConfigOSDLayoutItem item(const QString &key, const QString &caption,
                         bool enabled, int x, int y)
{
    ConfigOSDLayoutItem result;
    result.key = key;
    result.caption = caption;
    result.name = key;
    result.enabled = enabled;
    result.x = x;
    result.y = y;
    return result;
}

} // namespace

class ConfigOSDLayoutCanvasTest final : public QObject
{
    Q_OBJECT

private slots:
    void geometryHelpersCoverTheThirtyBySixteenGrid();
    void clickSelectsTheTopmostItem();
    void disabledItemsExposeTheirPaintOpacity();
    void dragPreservesPointerOffsetAndClampsToGrid();
    void unchangedReleaseDoesNotEmitAnEdit();
    void selectionSignalsAllowReentrantMutationAndDeletion();
};

void ConfigOSDLayoutCanvasTest::geometryHelpersCoverTheThirtyBySixteenGrid()
{
    ConfigOSDLayoutCanvas canvas;

    QCOMPARE(canvas.objectName(), QStringLiteral("ConfigOSDLayoutCanvas"));
    QCOMPARE(ConfigOSDLayoutCanvas::canvasSize(), QSize(780, 384));
    QCOMPARE(canvas.sizeHint(), QSize(780, 384));
    QCOMPARE(canvas.minimumSizeHint(), QSize(780, 384));
    QCOMPARE(canvas.minimumSize(), QSize(780, 384));
    QCOMPARE(canvas.maximumSize(), QSize(780, 384));

    QVERIFY(ConfigOSDLayoutCanvas::isCellInBounds(0, 0));
    QVERIFY(ConfigOSDLayoutCanvas::isCellInBounds(29, 15));
    QVERIFY(!ConfigOSDLayoutCanvas::isCellInBounds(-1, 0));
    QVERIFY(!ConfigOSDLayoutCanvas::isCellInBounds(30, 15));
    QCOMPARE(ConfigOSDLayoutCanvas::cellOrigin(29, 15), QPoint(754, 360));
    QCOMPARE(ConfigOSDLayoutCanvas::cellRect(0, 0), QRect(0, 0, 26, 24));
    QCOMPARE(ConfigOSDLayoutCanvas::cellRect(29, 15),
             QRect(754, 360, 26, 24));
    QVERIFY(ConfigOSDLayoutCanvas::cellRect(30, 0).isEmpty());

    QCOMPARE(ConfigOSDLayoutCanvas::cellForPosition(QPoint(0, 0)),
             QPoint(0, 0));
    QCOMPARE(ConfigOSDLayoutCanvas::cellForPosition(QPoint(779, 383)),
             QPoint(29, 15));
    QCOMPARE(ConfigOSDLayoutCanvas::cellForPosition(QPoint(-1, 0)),
             QPoint(-1, -1));
    QCOMPARE(ConfigOSDLayoutCanvas::cellForPosition(QPoint(780, 383)),
             QPoint(-1, -1));
    QCOMPARE(ConfigOSDLayoutCanvas::clampedCellForPosition(
                 QPoint(-100, -100)), QPoint(0, 0));
    QCOMPARE(ConfigOSDLayoutCanvas::clampedCellForPosition(
                 QPoint(5000, 5000)), QPoint(29, 15));

    canvas.setItems({item(QStringLiteral("bounded"), QStringLiteral("B"),
                          true, 100, -20)});
    const ConfigOSDLayoutItem bounded = canvas.items().constFirst();
    QCOMPARE(QPoint(bounded.x, bounded.y), QPoint(29, 0));
    QVERIFY(bounded.clipped);
}

void ConfigOSDLayoutCanvasTest::clickSelectsTheTopmostItem()
{
    ConfigOSDLayoutCanvas canvas;
    canvas.setItems({
        item(QStringLiteral("lower"), QStringLiteral("LOW"), true, 2, 3),
        item(QStringLiteral("upper"), QStringLiteral("UP"), true, 2, 3)
    });
    QSignalSpy selections(&canvas,
                          &ConfigOSDLayoutCanvas::selectionChanged);

    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier,
                      ConfigOSDLayoutCanvas::cellRect(2, 3).center());
    QCOMPARE(canvas.selectedKey(), QStringLiteral("upper"));
    QCOMPARE(selections.count(), 1);
    QCOMPARE(selections.first().at(0).toString(), QStringLiteral("upper"));

    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier,
                      ConfigOSDLayoutCanvas::cellRect(20, 12).center());
    QVERIFY(canvas.selectedKey().isEmpty());
    QCOMPARE(selections.count(), 2);
}

void ConfigOSDLayoutCanvasTest::disabledItemsExposeTheirPaintOpacity()
{
    ConfigOSDLayoutCanvas canvas;
    canvas.setItems({
        item(QStringLiteral("enabled"), QStringLiteral("E"), true, 0, 0),
        item(QStringLiteral("disabled"), QStringLiteral("D"), false, 1, 0)
    });

    QCOMPARE(ConfigOSDLayoutCanvas::visualOpacity(true), 1.0);
    QCOMPARE(ConfigOSDLayoutCanvas::visualOpacity(false), 0.35);
    QCOMPARE(canvas.itemVisualOpacity(QStringLiteral("enabled")), 1.0);
    QCOMPARE(canvas.itemVisualOpacity(QStringLiteral("disabled")), 0.35);
    QCOMPARE(canvas.itemVisualOpacity(QStringLiteral("missing")), -1.0);

    // Disabled items remain selectable and draggable in the layout editor.
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier,
                      ConfigOSDLayoutCanvas::cellRect(1, 0).center());
    QCOMPARE(canvas.selectedKey(), QStringLiteral("disabled"));
}

void ConfigOSDLayoutCanvasTest::dragPreservesPointerOffsetAndClampsToGrid()
{
    ConfigOSDLayoutCanvas canvas;
    canvas.setItems({item(QStringLiteral("altitude"),
                          QStringLiteral("ALT"), true, 2, 3)});
    QSignalSpy edits(&canvas, &ConfigOSDLayoutCanvas::positionEdited);

    const QPoint press = ConfigOSDLayoutCanvas::cellOrigin(2, 3)
        + QPoint(20, 18);
    const QPoint release = ConfigOSDLayoutCanvas::cellOrigin(8, 10)
        + QPoint(5, 5);
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, press);
    QMouseEvent move(QEvent::MouseMove, release, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    QCOMPARE(QPoint(canvas.items().constFirst().x,
                    canvas.items().constFirst().y), QPoint(7, 9));
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, release);

    QCOMPARE(edits.count(), 1);
    QCOMPARE(edits.first().at(0).toString(), QStringLiteral("altitude"));
    QCOMPARE(edits.first().at(1).toInt(), 7);
    QCOMPARE(edits.first().at(2).toInt(), 9);

    const QPoint secondPress = ConfigOSDLayoutCanvas::cellOrigin(7, 9)
        + QPoint(10, 10);
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier,
                      secondPress);
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier,
                        QPoint(-500, -500));
    QCOMPARE(QPoint(canvas.items().constFirst().x,
                    canvas.items().constFirst().y), QPoint(0, 0));

    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier,
                      QPoint(5, 5));
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier,
                        QPoint(5000, 5000));
    QCOMPARE(QPoint(canvas.items().constFirst().x,
                    canvas.items().constFirst().y), QPoint(29, 15));
    QCOMPARE(edits.count(), 3);
}

void ConfigOSDLayoutCanvasTest::unchangedReleaseDoesNotEmitAnEdit()
{
    ConfigOSDLayoutCanvas canvas;
    canvas.setItems({item(QStringLiteral("speed"),
                          QStringLiteral("SPD"), true, 4, 5)});
    QSignalSpy edits(&canvas, &ConfigOSDLayoutCanvas::positionEdited);

    const QPoint press = ConfigOSDLayoutCanvas::cellOrigin(4, 5)
        + QPoint(8, 8);
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, press);
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, press);
    QCOMPARE(edits.count(), 0);

    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, press);
    QMouseEvent away(QEvent::MouseMove, press + QPoint(52, 24),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &away);
    QMouseEvent back(QEvent::MouseMove, press, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &back);
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, press);
    QCOMPARE(edits.count(), 0);
    QCOMPARE(QPoint(canvas.items().constFirst().x,
                    canvas.items().constFirst().y), QPoint(4, 5));
}

void ConfigOSDLayoutCanvasTest::selectionSignalsAllowReentrantMutationAndDeletion()
{
    ConfigOSDLayoutCanvas canvas;
    canvas.setItems({item(QStringLiteral("old"), QStringLiteral("OLD"),
                          true, 2, 3)});
    QObject::connect(&canvas, &ConfigOSDLayoutCanvas::selectionChanged,
                     &canvas, [&canvas](const QString &key) {
        if (key == QStringLiteral("old")) {
            canvas.setItems({item(QStringLiteral("new"),
                                  QStringLiteral("NEW"), true, 5, 6)});
        }
    });
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier,
                      ConfigOSDLayoutCanvas::cellRect(2, 3).center());
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier,
                        ConfigOSDLayoutCanvas::cellRect(2, 3).center());
    QCOMPARE(canvas.items().size(), 1);
    QCOMPARE(canvas.items().constFirst().key, QStringLiteral("new"));

    QPointer<ConfigOSDLayoutCanvas> doomed(new ConfigOSDLayoutCanvas);
    doomed->setItems({item(QStringLiteral("anything"),
                           QStringLiteral("ANY"), true, 0, 0)});
    QObject::connect(doomed.data(),
                     &ConfigOSDLayoutCanvas::selectionChanged,
                     doomed.data(), [&doomed](const QString &) {
        delete doomed.data();
    });
    doomed->setSelectedKey(QStringLiteral("anything"));
    QVERIFY(doomed.isNull());
}

QTEST_MAIN(ConfigOSDLayoutCanvasTest)

#include "test_configosdlayoutcanvas.moc"
