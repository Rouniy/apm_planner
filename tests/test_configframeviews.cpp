#include <QtTest>

#include "ui/configuration/ConfigFrameClassTypeView.h"
#include "ui/configuration/ConfigFrameTypeView.h"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>

namespace {
QList<ConfigFriendlyParameterValue> modernSnapshot(
    int frameClass = 4, int frameType = 3)
{
    return {{1, QStringLiteral("FRAME_CLASS"), frameClass},
            {1, QStringLiteral("FRAME_TYPE"), frameType}};
}

QList<ConfigFriendlyParameterValue> legacySnapshot(int frame = 1)
{
    return {{1, QStringLiteral("FRAME"), frame}};
}
} // namespace

class ConfigFrameViewsTest final : public QObject
{
    Q_OBJECT

private slots:
    void modernSurfaceIsNonEmptyBeforeSnapshot();
    void modernHydratesPreviewAndArmedStateWithoutWrites();
    void modernPartialWriteDisablesEditsUntilRefresh();
    void legacySurfaceHasSixOptionsAndHydratesWithoutWrites();
    void legacyUserSelectionEmitsOneBatch();
    void destructionNeverWrites();
};

void ConfigFrameViewsTest::modernSurfaceIsNonEmptyBeforeSnapshot()
{
    ConfigFrameClassTypeView view;
    view.resize(600, 520);
    view.show();
    QApplication::processEvents();

    QCOMPARE(view.objectName(), QStringLiteral("ConfigFrameClassTypeView"));
    QLabel *title = view.findChild<QLabel *>(
        QStringLiteral("frameClassTypeTitle"));
    QPushButton *refresh = view.findChild<QPushButton *>(
        QStringLiteral("frameClassTypeRefresh"));
    QComboBox *frameClass = view.findChild<QComboBox *>(
        QStringLiteral("frameClassCombo"));
    QComboBox *frameType = view.findChild<QComboBox *>(
        QStringLiteral("frameTypeCombo"));
    QLabel *preview = view.findChild<QLabel *>(
        QStringLiteral("framePreview"));
    QLabel *status = view.findChild<QLabel *>(
        QStringLiteral("frameClassTypeStatus"));
    QVERIFY(title && refresh && frameClass && frameType && preview && status);
    QCOMPARE(title->text(), QStringLiteral("Frame Class / Type"));
    QCOMPARE(frameClass->count(), 14);
    QCOMPARE(frameType->count(), 0);
    QVERIFY(!frameClass->isEnabled());
    QVERIFY(!frameType->isEnabled());
    QVERIFY(!refresh->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("not present")));

    view.setConnected(true);
    QVERIFY(refresh->isEnabled());
    QVERIFY(!frameClass->isEnabled());
    QVERIFY(!frameType->isEnabled());
}

void ConfigFrameViewsTest::modernHydratesPreviewAndArmedStateWithoutWrites()
{
    ConfigFrameClassTypeView view;
    QSignalSpy writes(&view, &ConfigFrameClassTypeView::writeRequested);
    view.setParameterSnapshot(modernSnapshot());
    view.setConnected(true);
    view.show();
    QApplication::processEvents();

    QComboBox *frameClass = view.findChild<QComboBox *>(
        QStringLiteral("frameClassCombo"));
    QComboBox *frameType = view.findChild<QComboBox *>(
        QStringLiteral("frameTypeCombo"));
    QLabel *preview = view.findChild<QLabel *>(
        QStringLiteral("framePreview"));
    QPushButton *refresh = view.findChild<QPushButton *>(
        QStringLiteral("frameClassTypeRefresh"));
    QVERIFY(frameClass && frameType && preview && refresh);
    QCOMPARE(frameClass->currentData().toInt(), 4);
    QCOMPARE(frameType->currentData().toInt(), 3);
    QCOMPARE(preview->property("frameImageKey").toString(),
             QStringLiteral("type_h"));
    QVERIFY(frameClass->isEnabled());
    QVERIFY(frameType->isEnabled());
    QCOMPARE(writes.count(), 0);

    view.setArmed(true);
    QVERIFY(!frameClass->isEnabled());
    QVERIFY(!frameType->isEnabled());
    QVERIFY(refresh->isEnabled());
    QCOMPARE(writes.count(), 0);
}

void ConfigFrameViewsTest::modernPartialWriteDisablesEditsUntilRefresh()
{
    ConfigFrameClassTypeView view;
    view.setParameterSnapshot(modernSnapshot(1, 4));
    view.setConnected(true);
    QSignalSpy writes(&view, &ConfigFrameClassTypeView::writeRequested);
    QSignalSpy refreshes(&view, &ConfigFrameClassTypeView::refreshRequested);

    QVERIFY(view.viewModel()->selectClass(2));
    QCOMPARE(writes.count(), 1);
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    view.parameterWriteSubmitted(requestId, 121);
    view.parameterWriteFailed(121, 1, QStringLiteral("FRAME_TYPE"),
                              QStringLiteral("denied"));
    view.parameterBatchCompleted(121, 1, 1);
    QApplication::processEvents();

    QComboBox *frameClass = view.findChild<QComboBox *>(
        QStringLiteral("frameClassCombo"));
    QComboBox *frameType = view.findChild<QComboBox *>(
        QStringLiteral("frameTypeCombo"));
    QPushButton *refresh = view.findChild<QPushButton *>(
        QStringLiteral("frameClassTypeRefresh"));
    QVERIFY(frameClass && frameType && refresh);
    QVERIFY(!frameClass->isEnabled());
    QVERIFY(!frameType->isEnabled());
    QCOMPARE(frameClass->currentIndex(), -1);
    QCOMPARE(frameType->currentIndex(), -1);
    QVERIFY(refresh->isEnabled());
    QCOMPARE(refreshes.count(), 1);
}

void ConfigFrameViewsTest::legacySurfaceHasSixOptionsAndHydratesWithoutWrites()
{
    ConfigFrameTypeView view;
    QSignalSpy writes(&view, &ConfigFrameTypeView::writeRequested);
    view.resize(560, 520);
    view.show();
    QApplication::processEvents();

    QCOMPARE(view.objectName(), QStringLiteral("ConfigFrameTypeView"));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("frameLegacyTitle")));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("frameLegacyIntro")));
    const QList<QRadioButton *> radios = view.findChildren<QRadioButton *>();
    QCOMPARE(radios.size(), 6);
    for (QRadioButton *radio : radios) {
        QVERIFY(!radio->isEnabled());
    }
    QRadioButton *vTail = view.findChild<QRadioButton *>(
        QStringLiteral("frameLegacyVTail"));
    QVERIFY(vTail);
    QCOMPARE(vTail->property("frameValue").toInt(), 4);

    view.setParameterSnapshot(legacySnapshot(10));
    view.setConnected(true);
    QApplication::processEvents();
    QRadioButton *y6b = view.findChild<QRadioButton *>(
        QStringLiteral("frameLegacyY6B"));
    QVERIFY(y6b && y6b->isChecked());
    for (QRadioButton *radio : radios) {
        QVERIFY(radio->isEnabled());
    }
    QCOMPARE(writes.count(), 0);

    view.setArmed(true);
    for (QRadioButton *radio : radios) {
        QVERIFY(!radio->isEnabled());
    }
}

void ConfigFrameViewsTest::legacyUserSelectionEmitsOneBatch()
{
    ConfigFrameTypeView view;
    view.setParameterSnapshot(legacySnapshot(1));
    view.setConnected(true);
    view.show();
    QApplication::processEvents();
    QSignalSpy writes(&view, &ConfigFrameTypeView::writeRequested);
    QRadioButton *vTail = view.findChild<QRadioButton *>(
        QStringLiteral("frameLegacyVTail"));
    QVERIFY(vTail && vTail->isEnabled());
    QTest::mouseClick(vTail, Qt::LeftButton);
    QCOMPARE(writes.count(), 1);
    const QVariantList changes = writes.at(0).at(2).toList();
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changes.at(0).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("FRAME"));
    QCOMPARE(changes.at(0).toMap().value(QStringLiteral("value")).toInt(), 4);
}

void ConfigFrameViewsTest::destructionNeverWrites()
{
    auto *modern = new ConfigFrameClassTypeView;
    modern->setParameterSnapshot(modernSnapshot());
    modern->setConnected(true);
    QSignalSpy modernWrites(
        modern, &ConfigFrameClassTypeView::writeRequested);
    delete modern;
    QCOMPARE(modernWrites.count(), 0);

    auto *legacy = new ConfigFrameTypeView;
    legacy->setParameterSnapshot(legacySnapshot());
    legacy->setConnected(true);
    QSignalSpy legacyWrites(legacy, &ConfigFrameTypeView::writeRequested);
    delete legacy;
    QCOMPARE(legacyWrites.count(), 0);
}

QTEST_MAIN(ConfigFrameViewsTest)
#include "test_configframeviews.moc"
