#include <QtTest>

#include "ui/configuration/ConfigExtendedTuningView.h"

#include <QAction>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTimer>
#include <QToolButton>

namespace {
constexpr int kGroupCount = 17;
constexpr int kRowCount = 68;
constexpr int kRateRollPRow = 8;
constexpr int kChannelSixOptionRow = 3;
constexpr int kHarmonicNotchOptionsRow = 64;

QString editorName(int zeroBasedRow)
{
    return QStringLiteral("extendedTuningEditor%1")
        .arg(zeroBasedRow + 1, 2, 10, QLatin1Char('0'));
}

QString rowName(int zeroBasedRow)
{
    return QStringLiteral("extendedTuningRow%1")
        .arg(zeroBasedRow + 1, 2, 10, QLatin1Char('0'));
}

QString statusName(int zeroBasedRow)
{
    return QStringLiteral("extendedTuningRowStatus%1")
        .arg(zeroBasedRow + 1, 2, 10, QLatin1Char('0'));
}

QString selectedName(int zeroBasedRow)
{
    const QList<ExtendedTuningRow> rows =
        ConfigExtendedTuningViewModel::ReferenceRows();
    return rows.at(zeroBasedRow).candidates.constFirst();
}

ParameterMetaDataCatalog catalogFixture()
{
    const QByteArray rateRollName = selectedName(kRateRollPRow).toUtf8();
    const QByteArray channelName =
        selectedName(kChannelSixOptionRow).toUtf8();
    const QByteArray notchOptionsName =
        selectedName(kHarmonicNotchOptionsRow).toUtf8();
    QByteArray xml = QByteArrayLiteral(
        "<paramfile><vehicles><parameters name=\"ArduPlane\">");
    xml += "<param name=\"ArduPlane:" + rateRollName
        + "\" humanName=\"Roll rate P\" documentation=\"Roll gain description\">"
          "<field name=\"Units\">1/s</field>"
          "<field name=\"Range\">0 10</field>"
          "<field name=\"Increment\">0.05</field></param>";
    xml += "<param name=\"ArduPlane:" + channelName
        + "\" humanName=\"Channel six option\" documentation=\"Select a tuning function\">"
          "<values><value code=\"0\">Disabled</value>"
          "<value code=\"2\">AutoTune</value></values></param>";
    xml += "<param name=\"ArduPlane:" + notchOptionsName
        + "\" humanName=\"Harmonic notch options\" documentation=\"Notch option bits\">"
          "<field name=\"Bitmask\">0:Double notch,2:Dynamic frequency,5:Loop rate update</field>"
          "</param>";
    xml += QByteArrayLiteral(
        "</parameters></vehicles></paramfile>");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduPlane"));
}

QList<ConfigFriendlyParameterValue> completeSnapshot()
{
    QList<ConfigFriendlyParameterValue> result;
    result.append({1, QStringLiteral("Q_ENABLE"), 1});
    const QList<ExtendedTuningRow> rows =
        ConfigExtendedTuningViewModel::ReferenceRows();
    for (int index = 0; index < rows.size(); ++index) {
        QVariant value = 1.0;
        if (index == kChannelSixOptionRow) {
            value = 2;
        } else if (index == kHarmonicNotchOptionsRow) {
            value = 5;
        }
        result.append({1, rows.at(index).candidates.constFirst(), value});
    }
    return result;
}

void makeEditable(ConfigExtendedTuningView *view)
{
    view->setCatalog(catalogFixture());
    view->setParameterSnapshot(completeSnapshot(), 1, true);
    view->setConnected(true);
    view->setHeartbeat(true, false);
    view->show();
    QApplication::processEvents();
}

int cardCount(const ConfigExtendedTuningView &view)
{
    int count = 0;
    for (QGroupBox *group : view.findChildren<QGroupBox *>()) {
        if (group->property("extendedTuningCard").toBool()) {
            ++count;
        }
    }
    return count;
}
} // namespace

class ConfigExtendedTuningViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void surfaceKeepsAllGroupsAndRowsBeforeSnapshot();
    void metadataSelectsEditorsAndHydrationNeverWrites();
    void unavailableRowsRemainVisibleDisabledAndMarkedNA();
    void disabledQuadPlaneKeepsCommonPlaneFieldsEditable();
    void actionsForwardOnlyExplicitWriteAndRefresh();
    void largeIncreaseDefaultsToCancel();
    void destructionNeverWrites();
};

void ConfigExtendedTuningViewTest::surfaceKeepsAllGroupsAndRowsBeforeSnapshot()
{
    ConfigExtendedTuningView view;
    view.resize(1100, 800);
    view.show();
    QApplication::processEvents();

    QCOMPARE(view.objectName(), QStringLiteral("ConfigExtendedTuningView"));
    QCOMPARE(view.viewModel()->Groups().size(), kGroupCount);
    QCOMPARE(view.viewModel()->Rows().size(), kRowCount);
    QCOMPARE(cardCount(view), kGroupCount);
    const QList<ExtendedTuningGroupDescriptor> groups =
        view.viewModel()->Groups();
    for (int index = 0; index < groups.size(); ++index) {
        QGroupBox *card = nullptr;
        for (QGroupBox *candidate : view.findChildren<QGroupBox *>()) {
            if (candidate->property("sectionIndex").toInt() == index
                && candidate->property("extendedTuningCard").toBool()) {
                card = candidate;
                break;
            }
        }
        QVERIFY2(card, qPrintable(groups.at(index).title));
        QCOMPARE(card->title(), groups.at(index).title);
    }
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("extendedTuningTitle")));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("extendedTuningIntro")));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("extendedTuningStatus")));
    QVERIFY(view.findChild<QScrollArea *>(
        QStringLiteral("extendedTuningScroll")));
    QCOMPARE(view.findChild<QPushButton *>(
                 QStringLiteral("extendedTuningWrite"))->text(),
             QStringLiteral("Write Params"));
    QCOMPARE(view.findChild<QPushButton *>(
                 QStringLiteral("extendedTuningRefreshParams"))->text(),
             QStringLiteral("Refresh Params"));
    QCOMPARE(view.findChild<QPushButton *>(
                 QStringLiteral("extendedTuningRefreshScreen"))->text(),
             QStringLiteral("Refresh Screen"));
    QCOMPARE(view.findChild<QCheckBox *>(
                 QStringLiteral("extendedTuningLockRollPitch"))->text(),
             QStringLiteral("Lock Pitch and Roll Values"));

    for (int index = 0; index < kRowCount; ++index) {
        QWidget *row = view.findChild<QWidget *>(rowName(index));
        QWidget *editor = view.findChild<QWidget *>(editorName(index));
        QVERIFY2(row, qPrintable(rowName(index)));
        QVERIFY2(editor, qPrintable(editorName(index)));
        QCOMPARE(row->property("rowIndex").toInt(), index);
        QCOMPARE(view.findChild<QLabel *>(
                     QStringLiteral("extendedTuningLabel%1")
                         .arg(index + 1, 2, 10, QLatin1Char('0')))->text(),
                 view.viewModel()->Rows().at(index).label);
        QCOMPARE(qobject_cast<QGroupBox *>(row->parentWidget())->title(),
                 view.viewModel()->Rows().at(index).groupTitle);
        QVERIFY(!editor->isEnabled());
    }
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("extendedTuningWrite"))->isEnabled());
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("extendedTuningRefreshParams"))->isEnabled());

    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.render(&painter);
    painter.end();
    QVERIFY(image.pixelColor(0, 0).alpha() > 0);
}

void ConfigExtendedTuningViewTest::metadataSelectsEditorsAndHydrationNeverWrites()
{
    ConfigExtendedTuningView view;
    QSignalSpy writes(&view, &ConfigExtendedTuningView::writeRequested);
    makeEditable(&view);

    QCOMPARE(writes.count(), 0);
    QDoubleSpinBox *numeric = view.findChild<QDoubleSpinBox *>(
        editorName(kRateRollPRow));
    QComboBox *combo = view.findChild<QComboBox *>(
        editorName(kChannelSixOptionRow));
    QToolButton *bitmask = view.findChild<QToolButton *>(
        editorName(kHarmonicNotchOptionsRow));
    QVERIFY(numeric && combo && bitmask);
    QVERIFY(numeric->isEnabled());
    QCOMPARE(numeric->value(), 1.0);
    QCOMPARE(numeric->minimum(), 0.0);
    QCOMPARE(numeric->maximum(), 10.0);
    QCOMPARE(numeric->singleStep(), 0.05);
    QVERIFY(numeric->toolTip().contains(
        QStringLiteral("Roll gain description")));
    QCOMPARE(view.findChild<QLabel *>(
                 QStringLiteral("extendedTuningUnits09"))->text(),
             QStringLiteral("1/s"));
    QCOMPARE(combo->currentData().toInt(), 2);
    QCOMPARE(bitmask->menu()->actions().size(), 3);
    QVERIFY(bitmask->menu()->actions().at(0)->isChecked());
    QVERIFY(bitmask->menu()->actions().at(1)->isChecked());
    QVERIFY(!bitmask->menu()->actions().at(2)->isChecked());
    QCOMPARE(writes.count(), 0);
}

void ConfigExtendedTuningViewTest::unavailableRowsRemainVisibleDisabledAndMarkedNA()
{
    ConfigExtendedTuningView view;
    view.setParameterSnapshot(
        {{1, QStringLiteral("Q_ENABLE"), 1}}, 1, true);
    view.setConnected(true);
    view.setHeartbeat(true, false);
    view.show();
    QApplication::processEvents();

    QCOMPARE(view.viewModel()->Rows().size(), kRowCount);
    for (int index = 0; index < kRowCount; ++index) {
        QWidget *row = view.findChild<QWidget *>(rowName(index));
        QWidget *editor = view.findChild<QWidget *>(editorName(index));
        QLabel *status = view.findChild<QLabel *>(statusName(index));
        QVERIFY(row && editor && status);
        QVERIFY(!row->property("available").toBool());
        QVERIFY(!editor->isEnabled());
        QCOMPARE(status->text(), QStringLiteral("n/a"));
    }
}

void ConfigExtendedTuningViewTest::
disabledQuadPlaneKeepsCommonPlaneFieldsEditable()
{
    ConfigExtendedTuningView view;
    QList<ConfigFriendlyParameterValue> values = completeSnapshot();
    values[0].value = 0;
    view.setCatalog(catalogFixture());
    view.setParameterSnapshot(values, 1, true);
    view.setConnected(true);
    view.setHeartbeat(true, false);
    view.show();
    QApplication::processEvents();

    QVERIFY(!view.findChild<QDoubleSpinBox *>(
        editorName(kRateRollPRow))->isEnabled());
    QVERIFY(view.findChild<QComboBox *>(
        editorName(kChannelSixOptionRow))->isEnabled());
    QVERIFY(view.findChild<QDoubleSpinBox *>(
        editorName(52))->isEnabled());
    QVERIFY(view.findChild<QLabel *>(
        QStringLiteral("extendedTuningStatus"))->text().contains(
            QStringLiteral("Q_ENABLE")));
}

void ConfigExtendedTuningViewTest::actionsForwardOnlyExplicitWriteAndRefresh()
{
    ConfigExtendedTuningView view;
    makeEditable(&view);
    QSignalSpy writes(&view, &ConfigExtendedTuningView::writeRequested);
    QSignalSpy refreshes(&view, &ConfigExtendedTuningView::refreshRequested);

    QPushButton *refresh = view.findChild<QPushButton *>(
        QStringLiteral("extendedTuningRefreshParams"));
    QPushButton *refreshScreen = view.findChild<QPushButton *>(
        QStringLiteral("extendedTuningRefreshScreen"));
    QPushButton *write = view.findChild<QPushButton *>(
        QStringLiteral("extendedTuningWrite"));
    QDoubleSpinBox *numeric = view.findChild<QDoubleSpinBox *>(
        editorName(kRateRollPRow));
    QVERIFY(refresh && refreshScreen && write && numeric);

    refresh->click();
    QCOMPARE(refreshes.count(), 1);
    QCOMPARE(refreshes.at(0).at(0).toInt(), 1);
    QCOMPARE(writes.count(), 0);

    numeric->setValue(1.25);
    QVERIFY(view.viewModel()->Dirty());
    refreshScreen->click();
    QVERIFY(!view.viewModel()->Dirty());
    QCOMPARE(writes.count(), 0);

    numeric->setValue(1.5);
    QVERIFY(write->isEnabled());
    write->click();
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(1).toInt(), 1);
    QVERIFY(!writes.at(0).at(2).toList().isEmpty());
}

void ConfigExtendedTuningViewTest::largeIncreaseDefaultsToCancel()
{
    ConfigExtendedTuningView view;
    makeEditable(&view);
    QSignalSpy writes(&view, &ConfigExtendedTuningView::writeRequested);
    QDoubleSpinBox *numeric = view.findChild<QDoubleSpinBox *>(
        editorName(kRateRollPRow));
    QPushButton *write = view.findChild<QPushButton *>(
        QStringLiteral("extendedTuningWrite"));
    QVERIFY(numeric && write);
    numeric->setValue(3.0);
    QVERIFY(view.viewModel()->RequiresLargeIncreaseConfirmation());

    bool inspected = false;
    QTimer::singleShot(0, &view, [&inspected]() {
        auto *warning = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        QVERIFY(warning);
        QCOMPARE(warning->objectName(),
                 QStringLiteral("extendedTuningLargeValueWarning"));
        QCOMPARE(warning->standardButton(warning->defaultButton()),
                 QMessageBox::Cancel);
        inspected = true;
        warning->reject();
    });
    write->click();
    QVERIFY(inspected);
    QCOMPARE(writes.count(), 0);
    QVERIFY(view.viewModel()->Dirty());
}

void ConfigExtendedTuningViewTest::destructionNeverWrites()
{
    auto *view = new ConfigExtendedTuningView;
    QSignalSpy writes(view, &ConfigExtendedTuningView::writeRequested);
    makeEditable(view);
    view->viewModel()->stageValue(kRateRollPRow, 1.5);
    delete view;
    QCOMPARE(writes.count(), 0);
}

QTEST_MAIN(ConfigExtendedTuningViewTest)
#include "test_configextendedtuningview.moc"
