#include <QtTest>

#include "ui/configuration/ConfigOSDLayoutCanvas.h"
#include "ui/configuration/ConfigOSDView.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QVariantMap>

namespace {

ConfigFriendlyParameterValue parameter(
    const QString &name, const QVariant &value, int componentId = 1)
{
    ConfigFriendlyParameterValue result;
    result.componentId = componentId;
    result.name = name;
    result.value = value;
    return result;
}

QList<ConfigFriendlyParameterValue> snapshot()
{
    return {
        parameter(QStringLiteral("OSD1_ALT_EN"), 0),
        parameter(QStringLiteral("OSD1_ALT_X"), 2),
        parameter(QStringLiteral("OSD1_ALT_Y"), 3),
        parameter(QStringLiteral("OSD1_BAT_EN"), 1),
        parameter(QStringLiteral("OSD1_BAT_X"), 7),
        parameter(QStringLiteral("OSD1_BAT_Y"), 8),
        parameter(QStringLiteral("OSD2_GPS_EN"), 1),
        parameter(QStringLiteral("OSD2_GPS_X"), 4),
        parameter(QStringLiteral("OSD2_GPS_Y"), 5)
    };
}

template<typename T>
T *required(QObject *owner, const QString &name)
{
    T *result = owner->findChild<T *>(name);
    Q_ASSERT(result);
    return result;
}

} // namespace

class ConfigOSDViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesThePhaseOneControlInventory();
    void snapshotPopulatesScreensCanvasAndItemEditors();
    void editorChangesStageWriteAndDiscard();
    void canvasPositionSignalStagesBothCoordinates();
    void realCanvasDragStagesCoordinates();
    void forwardsRefreshAndBatchLifecycle();
    void reconnectShowsAReadyStatus();
    void hdCoordinateRemainsTruthfulInThePhaseOneEditor();
};

void ConfigOSDViewTest::exposesThePhaseOneControlInventory()
{
    ConfigOSDView view;

    QCOMPARE(view.objectName(), QStringLiteral("ConfigOSDView"));
    QCOMPARE(required<QLabel>(&view, QStringLiteral("configOsdTitle"))->text(),
             QStringLiteral("Onboard OSD"));
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("configOsdIntro"))->text(),
             ConfigOSDViewModel::Intro());
    QCOMPARE(required<QPushButton>(
                 &view, QStringLiteral("configOsdRefreshParams"))->text(),
             QStringLiteral("Refresh Params"));
    QVERIFY(required<QLabel>(&view, QStringLiteral("configOsdScreenLabel")));
    QVERIFY(required<QComboBox>(&view, QStringLiteral("configOsdScreen")));
    QCOMPARE(required<QPushButton>(
                 &view, QStringLiteral("configOsdEnableAll"))->text(),
             QStringLiteral("Enable All"));
    QCOMPARE(required<QPushButton>(
                 &view, QStringLiteral("configOsdDisableAll"))->text(),
             QStringLiteral("Disable All"));

    QPushButton *tuning = required<QPushButton>(
        &view, QStringLiteral("configOsdTuningSlots"));
    QCOMPARE(tuning->text(), QStringLiteral("OSD 5/6 Tuning Slots…"));
    QVERIFY(!tuning->isEnabled());
    QVERIFY(tuning->toolTip().contains(QStringLiteral("not ported yet")));
    QVERIFY(tuning->toolTip().contains(QStringLiteral("phase 1")));

    QLabel *status = required<QLabel>(
        &view, QStringLiteral("configOsdStatus"));
    QCOMPARE(status->text(), ConfigOSDViewModel::OfflineStatus());
    QVERIFY(!status->text().isEmpty());
    auto *canvas = required<ConfigOSDLayoutCanvas>(
        &view, QStringLiteral("ConfigOSDLayoutCanvas"));
    QCOMPARE(canvas->size(), QSize(780, 384));
    QVERIFY(canvas->items().isEmpty());
    QVERIFY(required<QScrollArea>(
        &view, QStringLiteral("configOsdCanvasScroll")));
    QVERIFY(required<QScrollArea>(
        &view, QStringLiteral("configOsdItemsScroll")));
    QVERIFY(required<QLabel>(
        &view, QStringLiteral("configOsdEmptyItems")));

    QCOMPARE(required<QPushButton>(
                 &view, QStringLiteral("configOsdWriteCustomization"))->text(),
             QStringLiteral("Write customization"));
    QCOMPARE(required<QPushButton>(
                 &view, QStringLiteral("configOsdDiscardAllChanges"))->text(),
             QStringLiteral("Discard all changes"));
    QVERIFY(!required<QPushButton>(
        &view, QStringLiteral("configOsdWriteCustomization"))->isEnabled());
}

void ConfigOSDViewTest::snapshotPopulatesScreensCanvasAndItemEditors()
{
    ConfigOSDView view;
    view.setConnected(true);
    view.setParameterSnapshot(snapshot());

    QComboBox *screens = required<QComboBox>(
        &view, QStringLiteral("configOsdScreen"));
    QCOMPARE(screens->count(), 2);
    QCOMPARE(screens->itemText(0), QStringLiteral("Screen 1"));
    QCOMPARE(screens->itemData(0).toInt(), 1);
    QCOMPARE(screens->itemText(1), QStringLiteral("Screen 2"));
    QCOMPARE(screens->currentData().toInt(), 1);

    ConfigOSDLayoutCanvas *canvas = required<ConfigOSDLayoutCanvas>(
        &view, QStringLiteral("ConfigOSDLayoutCanvas"));
    QCOMPARE(canvas->items().size(), 2);
    QCOMPARE(canvas->items().at(0).key, QStringLiteral("1/ALT"));
    QCOMPARE(canvas->items().at(0).caption, QStringLiteral("ALT"));
    QCOMPARE(QPoint(canvas->items().at(0).x, canvas->items().at(0).y),
             QPoint(2, 3));
    QVERIFY(!canvas->items().at(0).enabled);

    QCheckBox *altEnabled = required<QCheckBox>(
        &view, QStringLiteral("configOsdItemEnabled_1_ALT"));
    QLabel *altName = required<QLabel>(
        &view, QStringLiteral("configOsdItemName_1_ALT"));
    QSpinBox *altX = required<QSpinBox>(
        &view, QStringLiteral("configOsdItemX_1_ALT"));
    QSpinBox *altY = required<QSpinBox>(
        &view, QStringLiteral("configOsdItemY_1_ALT"));
    QVERIFY(!altEnabled->isChecked());
    QCOMPARE(altName->text(), QStringLiteral("ALT"));
    QCOMPARE(altX->value(), 2);
    QCOMPARE(altY->value(), 3);
    QCOMPARE(altX->maximum(), 29);
    QCOMPARE(altY->maximum(), 15);

    screens->setCurrentIndex(1);
    QCOMPARE(view.viewModel()->SelectedScreen(), 2);
    QCOMPARE(canvas->items().size(), 1);
    QCOMPARE(canvas->items().constFirst().key, QStringLiteral("2/GPS"));
    QVERIFY(required<QSpinBox>(
        &view, QStringLiteral("configOsdItemX_2_GPS")));
}

void ConfigOSDViewTest::editorChangesStageWriteAndDiscard()
{
    ConfigOSDView view;
    view.setConnected(true);
    view.setParameterSnapshot(snapshot());

    QCheckBox *enabled = required<QCheckBox>(
        &view, QStringLiteral("configOsdItemEnabled_1_ALT"));
    QSpinBox *x = required<QSpinBox>(
        &view, QStringLiteral("configOsdItemX_1_ALT"));
    QPushButton *write = required<QPushButton>(
        &view, QStringLiteral("configOsdWriteCustomization"));
    QPushButton *discard = required<QPushButton>(
        &view, QStringLiteral("configOsdDiscardAllChanges"));
    QPushButton *disableAll = required<QPushButton>(
        &view, QStringLiteral("configOsdDisableAll"));

    QVERIFY(enabled->isEnabled());
    QVERIFY(!write->isEnabled());
    enabled->setChecked(true);
    x->setValue(10);
    QCOMPARE(view.viewModel()->DirtyFieldCount(), 2);
    QVERIFY(write->isEnabled());
    QVERIFY(discard->isEnabled());

    disableAll->click();
    ConfigOSDItem bat;
    QVERIFY(view.viewModel()->Item(1, QStringLiteral("BAT"), &bat));
    QVERIFY(!bat.enabled);
    QVERIFY(view.viewModel()->HasDirtyChanges());

    discard->click();
    QCOMPARE(view.viewModel()->DirtyFieldCount(), 0);
    QVERIFY(!write->isEnabled());
    QVERIFY(!discard->isEnabled());
    ConfigOSDItem alt;
    QVERIFY(view.viewModel()->Item(1, QStringLiteral("ALT"), &alt));
    QVERIFY(!alt.enabled);
    QCOMPARE(alt.x, 2);
    QVERIFY(view.viewModel()->Item(1, QStringLiteral("BAT"), &bat));
    QVERIFY(bat.enabled);
}

void ConfigOSDViewTest::canvasPositionSignalStagesBothCoordinates()
{
    ConfigOSDView view;
    view.setConnected(true);
    view.setParameterSnapshot(snapshot());
    ConfigOSDLayoutCanvas *canvas = required<ConfigOSDLayoutCanvas>(
        &view, QStringLiteral("ConfigOSDLayoutCanvas"));

    QVERIFY(QMetaObject::invokeMethod(
        canvas, "positionEdited", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("1/ALT")), Q_ARG(int, 12),
        Q_ARG(int, 13)));

    ConfigOSDItem alt;
    QVERIFY(view.viewModel()->Item(1, QStringLiteral("ALT"), &alt));
    QCOMPARE(QPoint(alt.x, alt.y), QPoint(12, 13));
    QCOMPARE(view.viewModel()->DirtyFieldCount(), 2);
    QCOMPARE(required<QSpinBox>(
                 &view, QStringLiteral("configOsdItemX_1_ALT"))->value(), 12);
    QCOMPARE(required<QSpinBox>(
                 &view, QStringLiteral("configOsdItemY_1_ALT"))->value(), 13);
    QCOMPARE(QPoint(canvas->items().at(0).x, canvas->items().at(0).y),
             QPoint(12, 13));
}

void ConfigOSDViewTest::realCanvasDragStagesCoordinates()
{
    ConfigOSDView view;
    view.setConnected(true);
    view.setParameterSnapshot(snapshot());
    ConfigOSDLayoutCanvas *canvas = required<ConfigOSDLayoutCanvas>(
        &view, QStringLiteral("ConfigOSDLayoutCanvas"));

    const QPoint press = ConfigOSDLayoutCanvas::cellRect(2, 3).center();
    const QPoint release = ConfigOSDLayoutCanvas::cellRect(9, 11).center();
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, press);
    QMouseEvent move(QEvent::MouseMove, release, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, release);

    ConfigOSDItem alt;
    QVERIFY(view.viewModel()->Item(1, QStringLiteral("ALT"), &alt));
    QCOMPARE(QPoint(alt.x, alt.y), QPoint(9, 11));
    QCOMPARE(view.viewModel()->DirtyFieldCount(), 2);
}

void ConfigOSDViewTest::forwardsRefreshAndBatchLifecycle()
{
    ConfigOSDView view;
    view.setConnected(true);
    view.setParameterSnapshot(snapshot());
    QSignalSpy refreshes(&view, &ConfigOSDView::refreshRequested);
    QSignalSpy writes(&view, &ConfigOSDView::writeParamsRequested);

    required<QPushButton>(
        &view, QStringLiteral("configOsdRefreshParams"))->click();
    QCOMPARE(refreshes.count(), 1);
    QCOMPARE(refreshes.first().at(0).toInt(), 1);

    required<QCheckBox>(
        &view, QStringLiteral("configOsdItemEnabled_1_ALT"))->setChecked(true);
    required<QPushButton>(
        &view, QStringLiteral("configOsdWriteCustomization"))->click();
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toInt(), 1);
    QCOMPARE(writes.first().at(1).toList().size(), 1);
    QVERIFY(view.viewModel()->Busy());
    QVERIFY(!required<QPushButton>(
        &view, QStringLiteral("configOsdRefreshParams"))->isEnabled());
    QVERIFY(!required<QCheckBox>(
        &view, QStringLiteral("configOsdItemEnabled_1_ALT"))->isEnabled());

    view.parameterBatchSubmitted(1, 77);
    view.parameterBatchProgress(77, 1, 1, 1, 0);
    QVERIFY(required<QLabel>(
        &view, QStringLiteral("configOsdStatus"))->text().contains(
            QStringLiteral("1/1")));
    view.parameterBatchCompleted(77, 1, 0);
    QVERIFY(!view.viewModel()->Busy());
    QCOMPARE(view.viewModel()->DirtyFieldCount(), 0);
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("configOsdStatus"))->text(),
             ConfigOSDViewModel::SuccessStatus());

    view.parameterTargetChanged();
    QVERIFY(!view.viewModel()->SnapshotReady());
    QVERIFY(required<ConfigOSDLayoutCanvas>(
        &view, QStringLiteral("ConfigOSDLayoutCanvas"))->items().isEmpty());
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("configOsdStatus"))->text(),
             ConfigOSDViewModel::TargetChangedStatus());
}

void ConfigOSDViewTest::reconnectShowsAReadyStatus()
{
    ConfigOSDView view;
    view.setConnected(true);
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("configOsdStatus"))->text(),
             ConfigOSDViewModel::UnreadyStatus());
    view.setParameterSnapshot({});
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("configOsdStatus"))->text(),
             ConfigOSDViewModel::NoParametersStatus());
    view.setConnected(false);
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("configOsdStatus"))->text(),
             ConfigOSDViewModel::OfflineStatus());
    view.setConnected(true);
    QCOMPARE(required<QLabel>(
                 &view, QStringLiteral("configOsdStatus"))->text(),
             ConfigOSDViewModel::UnreadyStatus());
}

void ConfigOSDViewTest::hdCoordinateRemainsTruthfulInThePhaseOneEditor()
{
    ConfigOSDView view;
    view.setConnected(true);
    view.setParameterSnapshot({
        parameter(QStringLiteral("OSD1_ALT_EN"), 1),
        parameter(QStringLiteral("OSD1_ALT_X"), 40),
        parameter(QStringLiteral("OSD1_ALT_Y"), 18)
    });

    QSpinBox *x = required<QSpinBox>(
        &view, QStringLiteral("configOsdItemX_1_ALT"));
    QSpinBox *y = required<QSpinBox>(
        &view, QStringLiteral("configOsdItemY_1_ALT"));
    QCOMPARE(x->value(), 40);
    QCOMPARE(x->maximum(), 40);
    QCOMPARE(y->value(), 18);
    QCOMPARE(y->maximum(), 18);
    const ConfigOSDLayoutItem canvasItem =
        required<ConfigOSDLayoutCanvas>(
            &view, QStringLiteral("ConfigOSDLayoutCanvas"))
            ->items().constFirst();
    QCOMPARE(QPoint(canvasItem.x, canvasItem.y), QPoint(29, 15));
    QVERIFY(canvasItem.clipped);

    x->setValue(39);
    ConfigOSDItem modelItem;
    QVERIFY(view.viewModel()->Item(1, QStringLiteral("ALT"), &modelItem));
    QCOMPARE(modelItem.acceptedX, 40);
    QCOMPARE(modelItem.x, 29);
    QVERIFY(modelItem.xDirty());
}

QTEST_MAIN(ConfigOSDViewTest)

#include "test_configosdview.moc"
