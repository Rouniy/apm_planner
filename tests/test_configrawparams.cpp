#include <QtTest>

#include "ui/configuration/ConfigRawParams.h"

#include <QAbstractButton>
#include <QBuffer>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QTableView>
#include <QTimer>
#include <QTreeWidget>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:GAIN" humanName="Gain" documentation="Gain description">
          <field name="Units">%</field><field name="Range">0 10</field>
        </param>
        <param name="ArduCopter:LOCKED" humanName="Locked">
          <field name="ReadOnly">true</field>
          <values><value code="4">Four</value><value code="5">Five</value></values>
        </param>
        <param name="ArduCopter:MODE" humanName="Mode">
          <values><value code="0">Off</value><value code="2">Auto</value></values>
        </param>
        <param name="ArduCopter:MASK" humanName="Mask">
          <values><value code="0">None</value><value code="1">One</value></values>
          <field name="Bitmask">0:One,3:Eight,7:Sign</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

ParameterRecord record(const QString &name, const QVariant &value,
                       ParameterType type, int component = 1)
{
    ParameterRecord result;
    result.key.componentId = static_cast<quint8>(component);
    result.key.name = name;
    result.value = value;
    result.type = type;
    return result;
}

QStringList visibleNames(const ConfigRawParams &view)
{
    const auto *table = view.findChild<QTableView *>(QStringLiteral("Params"));
    QStringList result;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        result.append(table->model()->index(row, 0).data().toString());
    }
    return result;
}

int visibleRow(const ConfigRawParams &view, const QString &name)
{
    const auto *table = view.findChild<QTableView *>(QStringLiteral("Params"));
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (table->model()->index(row, 0).data().toString() == name) {
            return row;
        }
    }
    return -1;
}

QTreeWidgetItem *treeItem(QTreeWidget *tree, const QString &prefix)
{
    QList<QTreeWidgetItem *> pending;
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        pending.append(tree->topLevelItem(index));
    }
    while (!pending.isEmpty()) {
        QTreeWidgetItem *item = pending.takeFirst();
        if (item->data(0, Qt::UserRole).toString() == prefix) {
            return item;
        }
        for (int index = 0; index < item->childCount(); ++index) {
            pending.append(item->child(index));
        }
    }
    return nullptr;
}
} // namespace

class ConfigRawParamsTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void fileCodecMatchesMissionPlannerContract();
    void fileCodecRejectsMalformedValueAndSavesSorted();
    void widgetKeepsMissionPlannerNamesGeometryAndNaturalOrder();
    void stagedExpressionsRespectTypesRangesReadOnlyAndNoOps();
    void searchModifiedAndPrefixFiltersCompose();
    void liveAcknowledgementsClearOnlyTheMatchingStagedValue();
    void favoritesAreComponentScopedAndSortFirst();
    void targetChangeClearsVehicleOwnedState();
    void metadataAndSchemaRefreshRevalidateStagedValues();
    void componentSwitchClearsStagedValuesAndRangeApproval();
    void writeConfirmationRejectsConcurrentStagedMutation();
    void batchOwnershipDisconnectAndShortcutGuards();
    void targetChangeDuringRangeQuestionIsSafe();
    void approvedOutOfRangeValueCanBeSubmitted();
    void pageDeletionClosesModalEditorsSafely();
    void enumAndBitmaskDelegatesStageWithoutWriting();
};

void ConfigRawParamsTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("APMPlannerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ConfigRawParams"));
}

void ConfigRawParamsTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void ConfigRawParamsTest::fileCodecMatchesMissionPlannerContract()
{
    QByteArray input(
        "# comment\n"
        "GAIN,1.5\n"
        "MODE\t2 extra\n"
        "GAIN 3.25\n"
        "FORMAT_VERSION,120\n"
        "ignored-only-field\n");
    QBuffer buffer(&input);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QMap<QString, double> values;
    QString error;
    int line = -1;
    QVERIFY(ConfigRawParamsFileCodec::load(
        &buffer, &values, &error, &line));
    QVERIFY(error.isEmpty());
    QCOMPARE(line, 0);
    QCOMPARE(values.size(), 2);
    QCOMPARE(values.value(QStringLiteral("GAIN")), 3.25);
    QCOMPARE(values.value(QStringLiteral("MODE")), 2.0);
    QVERIFY(!values.contains(QStringLiteral("FORMAT_VERSION")));
}

void ConfigRawParamsTest::fileCodecRejectsMalformedValueAndSavesSorted()
{
    QByteArray invalid("GOOD,1\nBAD,nope\n");
    QBuffer input(&invalid);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QMap<QString, double> values;
    QString error;
    int line = 0;
    QVERIFY(!ConfigRawParamsFileCodec::load(
        &input, &values, &error, &line));
    QCOMPARE(line, 2);
    QVERIFY(error.contains(QStringLiteral("BAD")));
    QVERIFY(values.isEmpty());

    QByteArray output;
    QBuffer saved(&output);
    QVERIFY(saved.open(QIODevice::WriteOnly));
    QVERIFY(ConfigRawParamsFileCodec::save(
        &saved, {{QStringLiteral("Z"), 2.5},
                 {QStringLiteral("A"), -1}}, &error));
    QCOMPARE(output, QByteArray("A,-1\nZ,2.5\n"));
}

void ConfigRawParamsTest::widgetKeepsMissionPlannerNamesGeometryAndNaturalOrder()
{
    ConfigRawParams view(catalogFixture());
    view.setParameterSnapshot({
        record(QStringLiteral("SERVO10"), qint32(10), ParameterType::Int32),
        record(QStringLiteral("SERVO2"), qint32(2), ParameterType::Int32),
        record(QStringLiteral("GAIN"), 1.0F, ParameterType::Real32)
    });

    QCOMPARE(view.objectName(), QStringLiteral("ConfigRawParams"));
    auto *table = view.findChild<QTableView *>(QStringLiteral("Params"));
    QVERIFY(table);
    QVERIFY(view.findChild<QTreeWidget *>(QStringLiteral("treeView1")));
    QVERIFY(view.findChild<QWidget *>(QStringLiteral("splitContainer1")));
    QVERIFY(view.findChild<QPushButton *>(QStringLiteral("BUT_load")));
    QVERIFY(view.findChild<QPushButton *>(QStringLiteral("BUT_save")));
    QVERIFY(view.findChild<QPushButton *>(QStringLiteral("BUT_writePIDS")));
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("BUT_rerequestparams")));
    QVERIFY(view.findChild<QPushButton *>(QStringLiteral("BUT_compare")));
    QCOMPARE(table->model()->columnCount(), 7);
    QVERIFY(table->isColumnHidden(2));
    QCOMPARE(table->verticalHeader()->defaultSectionSize(), 36);
    QCOMPARE(visibleNames(view),
             QStringList({QStringLiteral("GAIN"), QStringLiteral("SERVO2"),
                          QStringLiteral("SERVO10")}));
}

void ConfigRawParamsTest::stagedExpressionsRespectTypesRangesReadOnlyAndNoOps()
{
    ConfigRawParams view(catalogFixture(), nullptr, true);
    view.setParameterSnapshot({
        record(QStringLiteral("COUNT"), qint32(2), ParameterType::Int32),
        record(QStringLiteral("GAIN"), 1.0F, ParameterType::Real32),
        record(QStringLiteral("LOCKED"), qint32(4), ParameterType::Int32),
        record(QStringLiteral("RC3_REV"), qint32(1), ParameterType::Int32)
    });
    QString error;
    QVERIFY(view.stageParameter(QStringLiteral("COUNT"),
                                QStringLiteral("1 + 2 * 3"), false, &error));
    QCOMPARE(view.stagedValue(QStringLiteral("COUNT")).toInt(), 7);
    QVERIFY(!view.stageParameter(QStringLiteral("COUNT"),
                                 QStringLiteral("1.5"), false, &error));
    QVERIFY(error.contains(QStringLiteral("MAVLink type")));

    QVERIFY(!view.stageParameter(QStringLiteral("GAIN"),
                                 QStringLiteral("12"), false, &error));
    QVERIFY(error.startsWith(QStringLiteral("out-of-range:")));
    QVERIFY(view.stageParameter(QStringLiteral("GAIN"),
                                QStringLiteral("12"), true, &error));
    QCOMPARE(view.stagedValue(QStringLiteral("GAIN")).toFloat(), 12.0F);

    QVERIFY(!view.stageParameter(QStringLiteral("LOCKED"),
                                 QStringLiteral("5"), false, &error));
    QVERIFY(error.contains(QStringLiteral("read-only")));
    QVERIFY(view.stageParameter(QStringLiteral("RC3_REV"),
                                QStringLiteral("0"), false, &error));
    QCOMPARE(view.stagedValue(QStringLiteral("RC3_REV")).toInt(), -1);

    QVERIFY(view.stageParameter(QStringLiteral("COUNT"),
                                QStringLiteral("2"), false, &error));
    QVERIFY(!view.stagedValue(QStringLiteral("COUNT")).isValid());
    QCOMPARE(view.stagedParameterCount(), 2);
}

void ConfigRawParamsTest::searchModifiedAndPrefixFiltersCompose()
{
    ConfigRawParams view(catalogFixture());
    view.setParameterSnapshot({
        record(QStringLiteral("ATC_RAT_RLL_P"), 1.0F,
               ParameterType::Real32),
        record(QStringLiteral("ATC_RAT_PIT_P"), 2.0F,
               ParameterType::Real32),
        record(QStringLiteral("BATT_MONITOR"), qint32(4),
               ParameterType::Int32),
        record(QStringLiteral("GAIN"), 1.0F, ParameterType::Real32)
    });
    QString error;
    QVERIFY(view.stageParameter(QStringLiteral("ATC_RAT_RLL_P"),
                                QStringLiteral("3"), false, &error));
    auto *modified = view.findChild<QCheckBox *>(
        QStringLiteral("chk_modified"));
    modified->setChecked(true);
    QCOMPARE(visibleNames(view),
             QStringList({QStringLiteral("ATC_RAT_RLL_P")}));

    auto *search = view.findChild<QLineEdit *>(QStringLiteral("txt_search"));
    search->setText(QStringLiteral("pit"));
    QTest::qWait(550);
    QCOMPARE(view.visibleParameterCount(), 0);
    modified->setChecked(false);
    QCOMPARE(visibleNames(view),
             QStringList({QStringLiteral("ATC_RAT_PIT_P")}));

    search->clear();
    QTest::qWait(550);
    auto *tree = view.findChild<QTreeWidget *>(QStringLiteral("treeView1"));
    QTreeWidgetItem *atc = treeItem(tree, QStringLiteral("ATC"));
    QVERIFY(atc);
    tree->setCurrentItem(atc);
    QCOMPARE(visibleNames(view),
             QStringList({QStringLiteral("ATC_RAT_PIT_P"),
                          QStringLiteral("ATC_RAT_RLL_P")}));

    tree->setCurrentItem(treeItem(tree, QString()));
    search->setText(QStringLiteral("0 10"));
    QTest::qWait(550);
    QCOMPARE(visibleNames(view), QStringList({QStringLiteral("GAIN")}));
    view.parameterChanged(1, QStringLiteral("GAIN"), 4.0F);
    QCOMPARE(visibleNames(view), QStringList({QStringLiteral("GAIN")}));
}

void ConfigRawParamsTest::liveAcknowledgementsClearOnlyTheMatchingStagedValue()
{
    ConfigRawParams view;
    view.setParameterSnapshot({
        record(QStringLiteral("P"), qint32(0), ParameterType::Int32)
    });
    QString error;
    QVERIFY(view.stageParameter(QStringLiteral("P"), QStringLiteral("1"),
                                false, &error));
    view.parameterChanged(1, QStringLiteral("P"), qint32(2));
    QCOMPARE(view.stagedValue(QStringLiteral("P")).toInt(), 1);
    view.parameterWriteAcknowledged(
        1, QStringLiteral("P"), qint32(1), int(ParameterType::Int32));
    QCOMPARE(view.stagedParameterCount(), 0);
    QCOMPARE(view.lastStatusText(), QStringLiteral("P acknowledged."));
}

void ConfigRawParamsTest::favoritesAreComponentScopedAndSortFirst()
{
    ConfigRawParams first;
    first.setParameterSnapshot({
        record(QStringLiteral("A"), qint32(0), ParameterType::Int32),
        record(QStringLiteral("Z"), qint32(0), ParameterType::Int32)
    });
    auto *table = first.findChild<QTableView *>(QStringLiteral("Params"));
    QModelIndex favorite = table->model()->index(1, 6);
    QVERIFY(table->model()->setData(favorite, Qt::Checked,
                                    Qt::CheckStateRole));
    QCoreApplication::processEvents();
    QCOMPARE(visibleNames(first).first(), QStringLiteral("Z"));

    ConfigRawParams restored;
    restored.setParameterSnapshot({
        record(QStringLiteral("A"), qint32(0), ParameterType::Int32),
        record(QStringLiteral("Z"), qint32(0), ParameterType::Int32)
    });
    QCOMPARE(visibleNames(restored).first(), QStringLiteral("Z"));

    ConfigRawParams otherComponent;
    otherComponent.setParameterSnapshot({
        record(QStringLiteral("A"), qint32(0), ParameterType::Int32, 2),
        record(QStringLiteral("Z"), qint32(0), ParameterType::Int32, 2)
    }, 2);
    QCOMPARE(visibleNames(otherComponent).first(), QStringLiteral("A"));
}

void ConfigRawParamsTest::targetChangeClearsVehicleOwnedState()
{
    ConfigRawParams view;
    view.setParameterSnapshot({
        record(QStringLiteral("P"), qint32(0), ParameterType::Int32)
    });
    QString error;
    QVERIFY(view.stageParameter(QStringLiteral("P"), QStringLiteral("1"),
                                false, &error));
    view.parameterTargetChanged();
    QCOMPARE(view.parameterCount(), 0);
    QCOMPARE(view.stagedParameterCount(), 0);
    QCOMPARE(view.visibleParameterCount(), 0);
    QVERIFY(view.lastStatusText().contains(QStringLiteral("target changed")));
}

void ConfigRawParamsTest::metadataAndSchemaRefreshRevalidateStagedValues()
{
    ConfigRawParams metadataView;
    metadataView.setParameterSnapshot({
        record(QStringLiteral("LOCKED"), qint32(4), ParameterType::Int32)
    });
    QString error;
    QVERIFY(metadataView.stageParameter(
        QStringLiteral("LOCKED"), QStringLiteral("5"), false, &error));
    metadataView.setCatalog(catalogFixture(), true);
    QCOMPARE(metadataView.stagedParameterCount(), 0);
    QVERIFY(metadataView.lastStatusText().contains(QStringLiteral("read-only")));

    ConfigRawParams schemaView;
    schemaView.setParameterSnapshot({
        record(QStringLiteral("U"), qint32(0), ParameterType::Int32)
    });
    QVERIFY(schemaView.stageParameter(
        QStringLiteral("U"), QStringLiteral("-1"), false, &error));
    schemaView.setParameterSnapshot({
        record(QStringLiteral("U"), quint32(0), ParameterType::UInt8)
    });
    QCOMPARE(schemaView.stagedParameterCount(), 0);
    QVERIFY(schemaView.lastStatusText().contains(
        QStringLiteral("incompatible type")));

    ConfigRawParams rangeView(catalogFixture(), nullptr, true);
    rangeView.setParameterSnapshot({
        record(QStringLiteral("GAIN"), 1.0F, ParameterType::Real32)
    });
    QVERIFY(rangeView.stageParameter(
        QStringLiteral("GAIN"), QStringLiteral("12"), true, &error));
    QCOMPARE(rangeView.stagedParameterCount(), 1);
    rangeView.setCatalog(catalogFixture(), true);
    QCOMPARE(rangeView.stagedParameterCount(), 0);
    QVERIFY(rangeView.lastStatusText().contains(QStringLiteral("outside range")));
}

void ConfigRawParamsTest::componentSwitchClearsStagedValuesAndRangeApproval()
{
    ConfigRawParams view(catalogFixture(), nullptr, true);
    view.setParameterSnapshot({
        record(QStringLiteral("GAIN"), 1.0F, ParameterType::Real32, 1)
    }, 1);
    QString error;
    QVERIFY(view.stageParameter(
        QStringLiteral("GAIN"), QStringLiteral("12"), true, &error));
    QCOMPARE(view.stagedParameterCount(), 1);

    view.setParameterSnapshot({
        record(QStringLiteral("GAIN"), 2.0F, ParameterType::Real32, 2)
    }, 2);
    QCOMPARE(view.stagedParameterCount(), 0);
    QVERIFY(!view.stageParameter(
        QStringLiteral("GAIN"), QStringLiteral("12"), false, &error));
    QVERIFY(error.startsWith(QStringLiteral("out-of-range:")));
}

void ConfigRawParamsTest::writeConfirmationRejectsConcurrentStagedMutation()
{
    ConfigRawParams view;
    view.setParameterSnapshot({
        record(QStringLiteral("A"), qint32(0), ParameterType::Int32),
        record(QStringLiteral("B"), qint32(0), ParameterType::Int32)
    });
    view.setConnected(true);
    QString error;
    QVERIFY(view.stageParameter(
        QStringLiteral("A"), QStringLiteral("1"), false, &error));
    QSignalSpy writes(&view, &ConfigRawParams::writeRequested);

    QTimer::singleShot(0, [&view]() {
        QString stagedError;
        QVERIFY(view.stageParameter(
            QStringLiteral("B"), QStringLiteral("2"), false,
            &stagedError));
        auto *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        QVERIFY(box);
        box->button(QMessageBox::Yes)->click();
    });
    QVERIFY(QMetaObject::invokeMethod(
        &view, "writeStagedParameters", Qt::DirectConnection));
    QCOMPARE(writes.count(), 0);
    QCOMPARE(view.stagedParameterCount(), 2);

    QTimer::singleShot(0, [&view]() {
        view.clearStagedChanges();
        auto *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        QVERIFY(box);
        box->button(QMessageBox::Yes)->click();
    });
    QVERIFY(QMetaObject::invokeMethod(
        &view, "writeStagedParameters", Qt::DirectConnection));
    QCOMPARE(writes.count(), 0);
    QCOMPARE(view.stagedParameterCount(), 0);
}

void ConfigRawParamsTest::batchOwnershipDisconnectAndShortcutGuards()
{
    ConfigRawParams view;
    view.setParameterSnapshot({
        record(QStringLiteral("P"), qint32(0), ParameterType::Int32)
    });
    view.setConnected(true);
    QString error;
    QVERIFY(view.stageParameter(QStringLiteral("P"), QStringLiteral("1"),
                                false, &error));
    QSignalSpy writes(&view, &ConfigRawParams::writeRequested);
    QTimer::singleShot(0, []() {
        auto *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (box && box->button(QMessageBox::Yes)) {
            box->button(QMessageBox::Yes)->click();
        }
    });
    QVERIFY(QMetaObject::invokeMethod(
        &view, "writeStagedParameters", Qt::DirectConnection));
    QCOMPARE(writes.count(), 1);

    view.parameterBatchSubmitted(42, 1);
    auto *shortcut = view.findChild<QShortcut *>(
        QStringLiteral("writeParamsShortcut"));
    QVERIFY(shortcut);
    QVERIFY(!shortcut->isEnabled());
    const QString activeStatus = view.lastStatusText();
    view.parameterBatchProgress(99, 1, 1, 1, 0);
    view.parameterBatchCompleted(99, 1, 0);
    QCOMPARE(view.lastStatusText(), activeStatus);
    QVERIFY(QMetaObject::invokeMethod(
        &view, "writeStagedParameters", Qt::DirectConnection));
    QCOMPARE(writes.count(), 1);

    view.setConnected(false);
    view.setConnected(true);
    QVERIFY(shortcut->isEnabled());
    QVERIFY(view.findChild<QTableView *>(QStringLiteral("Params"))->isEnabled());
}

void ConfigRawParamsTest::targetChangeDuringRangeQuestionIsSafe()
{
    ConfigRawParams view(catalogFixture(), nullptr, true);
    view.setParameterSnapshot({
        record(QStringLiteral("GAIN"), 1.0F, ParameterType::Real32)
    });
    auto *table = view.findChild<QTableView *>(QStringLiteral("Params"));
    QVERIFY(table);
    QTimer::singleShot(0, [&view]() {
        view.parameterTargetChanged();
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *box = qobject_cast<QMessageBox *>(widget)) {
                box->done(QMessageBox::No);
            }
        }
    });
    QVERIFY(table->model()->setData(
        table->model()->index(0, 1), QStringLiteral("12"), Qt::EditRole));
    QCOMPARE(view.parameterCount(), 0);
    QCOMPARE(view.stagedParameterCount(), 0);
}

void ConfigRawParamsTest::approvedOutOfRangeValueCanBeSubmitted()
{
    ConfigRawParams view(catalogFixture(), nullptr, true);
    view.setParameterSnapshot({
        record(QStringLiteral("GAIN"), 1.0F, ParameterType::Real32)
    });
    view.setConnected(true);
    QString error;
    QVERIFY(view.stageParameter(QStringLiteral("GAIN"),
                                QStringLiteral("12"), true, &error));
    QSignalSpy writes(&view, &ConfigRawParams::writeRequested);
    QTimer::singleShot(0, []() {
        auto *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (box && box->button(QMessageBox::Yes)) {
            box->button(QMessageBox::Yes)->click();
        }
    });
    QVERIFY(QMetaObject::invokeMethod(
        &view, "writeStagedParameters", Qt::DirectConnection));
    QCOMPARE(writes.count(), 1);
    const QVariantList changes = writes.constFirst().at(1).toList();
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changes.constFirst().toMap()
                 .value(QStringLiteral("value")).toFloat(), 12.0F);
}

void ConfigRawParamsTest::pageDeletionClosesModalEditorsSafely()
{
    auto *writeView = new ConfigRawParams;
    writeView->setParameterSnapshot({
        record(QStringLiteral("P"), qint32(0), ParameterType::Int32)
    });
    writeView->setConnected(true);
    QString error;
    QVERIFY(writeView->stageParameter(
        QStringLiteral("P"), QStringLiteral("1"), false, &error));
    QPointer<ConfigRawParams> writeGuard(writeView);
    QTimer::singleShot(0, writeView, [writeView]() { delete writeView; });
    QVERIFY(QMetaObject::invokeMethod(
        writeView, "writeStagedParameters", Qt::DirectConnection));
    QVERIFY(writeGuard.isNull());

    auto *bitmaskView = new ConfigRawParams(catalogFixture());
    bitmaskView->setParameterSnapshot({
        record(QStringLiteral("MASK"), qint32(1), ParameterType::Int8)
    });
    bitmaskView->show();
    QCoreApplication::processEvents();
    auto *table = bitmaskView->findChild<QTableView *>(
        QStringLiteral("Params"));
    QVERIFY(table);
    const QModelIndex options = table->model()->index(0, 4);
    QPointer<ConfigRawParams> bitmaskGuard(bitmaskView);
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(options).center());
    QTRY_VERIFY(QApplication::activeModalWidget());
    delete bitmaskView;
    QVERIFY(bitmaskGuard.isNull());
    QCoreApplication::processEvents();
    QVERIFY(!QApplication::activeModalWidget());

    ConfigRawParams refreshed(catalogFixture());
    refreshed.setParameterSnapshot({
        record(QStringLiteral("MASK"), qint32(1), ParameterType::Int8)
    });
    refreshed.show();
    QCoreApplication::processEvents();
    table = refreshed.findChild<QTableView *>(QStringLiteral("Params"));
    const QModelIndex refreshedOptions = table->model()->index(0, 4);
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(refreshedOptions).center());
    QTRY_VERIFY(QApplication::activeModalWidget());
    refreshed.parameterChanged(1, QStringLiteral("MASK"), qint32(32));
    QTRY_VERIFY(!QApplication::activeModalWidget());
    QCOMPARE(refreshed.stagedParameterCount(), 0);
}

void ConfigRawParamsTest::enumAndBitmaskDelegatesStageWithoutWriting()
{
    ConfigRawParams view(catalogFixture());
    view.setParameterSnapshot({
        record(QStringLiteral("MODE"), qint32(2), ParameterType::Int32),
        record(QStringLiteral("MASK"), qint32(32), ParameterType::Int8)
    });
    auto *table = view.findChild<QTableView *>(QStringLiteral("Params"));
    QVERIFY(table);
    QSignalSpy writes(&view, &ConfigRawParams::writeRequested);

    const int modeRow = visibleRow(view, QStringLiteral("MODE"));
    QVERIFY(modeRow >= 0);
    const QModelIndex modeOptions = table->model()->index(modeRow, 4);
    table->edit(modeOptions);
    QCoreApplication::processEvents();
    auto *combo = table->findChild<QComboBox *>(
        QStringLiteral("ConfigRawParamsEnumEditor"));
    QVERIFY(combo);
    QCOMPARE(combo->currentText(), QStringLiteral("Auto"));
    combo->setCurrentIndex(0);
    QVERIFY(QMetaObject::invokeMethod(
        combo, "activated", Qt::DirectConnection, Q_ARG(int, 0)));
    QCoreApplication::processEvents();
    QCOMPARE(view.stagedValue(QStringLiteral("MODE")).toInt(), 0);
    QCOMPARE(writes.count(), 0);

    const int maskRow = visibleRow(view, QStringLiteral("MASK"));
    QVERIFY(maskRow >= 0);
    const QModelIndex maskOptions = table->model()->index(maskRow, 4);
    view.show();
    QCoreApplication::processEvents();
    table->scrollTo(maskOptions);
    QTimer::singleShot(0, []() {
        auto *dialog = qobject_cast<QDialog *>(
            QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        for (QCheckBox *box : dialog->findChildren<QCheckBox *>()) {
            if (box->property("bitPosition").toInt() == 7) {
                box->setChecked(true);
            }
        }
        dialog->accept();
    });
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(maskOptions).center());
    // Bit 5 is not present in metadata and must survive editing. With bit 7
    // enabled, the Int8 two's-complement result is 0b10100000 == -96.
    QTRY_COMPARE(view.stagedValue(QStringLiteral("MASK")).toInt(), -96);
    QCOMPARE(writes.count(), 0);

    ConfigRawParams unknown(catalogFixture());
    unknown.setParameterSnapshot({
        record(QStringLiteral("MODE"), qint32(7), ParameterType::Int32)
    });
    auto *unknownTable = unknown.findChild<QTableView *>(
        QStringLiteral("Params"));
    unknownTable->edit(unknownTable->model()->index(0, 4));
    QCoreApplication::processEvents();
    auto *unknownCombo = unknownTable->findChild<QComboBox *>(
        QStringLiteral("ConfigRawParamsEnumEditor"));
    QVERIFY(unknownCombo);
    QCOMPARE(unknownCombo->currentIndex(), -1);
    QCOMPARE(unknown.stagedParameterCount(), 0);

    ConfigRawParams readOnly(catalogFixture());
    readOnly.setParameterSnapshot({
        record(QStringLiteral("LOCKED"), qint32(4), ParameterType::Int32)
    });
    auto *readOnlyTable = readOnly.findChild<QTableView *>(
        QStringLiteral("Params"));
    readOnlyTable->edit(readOnlyTable->model()->index(0, 4));
    QCoreApplication::processEvents();
    QVERIFY(!readOnlyTable->findChild<QComboBox *>(
        QStringLiteral("ConfigRawParamsEnumEditor")));
    QCOMPARE(readOnly.stagedParameterCount(), 0);
}

QTEST_MAIN(ConfigRawParamsTest)
#include "test_configrawparams.moc"
