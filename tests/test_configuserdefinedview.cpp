#include <QtTest>

#include "ui/configuration/ConfigFriendlyParamsView.h"
#include "ui/configuration/ConfigUserDefinedView.h"

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTimer>
#include <QToolButton>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:GAIN" humanName="Gain" documentation="Gain description">
          <field name="Units">%</field><field name="Range">-1 1</field>
          <field name="Increment">0.1</field>
        </param>
        <param name="ArduCopter:MODE" humanName="Mode">
          <values><value code="0">Off</value><value code="2">Auto</value></values>
        </param>
        <param name="ArduCopter:HYBRID" humanName="Hybrid">
          <values><value code="0">None</value><value code="1">First</value></values>
          <field name="Bitmask">0:First,2:Third</field>
        </param>
        <param name="ArduCopter:MASK_ONLY" humanName="Mask only">
          <field name="Bitmask">0:First,2:Third</field>
        </param>
        <param name="ArduCopter:LOCKED" humanName="Locked">
          <field name="ReadOnly">true</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QWidget *parameterRow(QWidget &view, const QString &name)
{
    const QList<QWidget *> widgets = view.findChildren<QWidget *>();
    for (QWidget *widget : widgets) {
        if (widget->property("parameterName").toString() == name) {
            return widget;
        }
    }
    return nullptr;
}

QString statusText(QWidget *row)
{
    return row->findChild<QLabel *>(QStringLiteral("statusLabel"))->text();
}
}

class ConfigUserDefinedViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void defaultsParserPersistenceAndEmptyRoundTrip();
    void customRowsPreserveOrderAndReferenceEditorKinds();
    void modifyAndEmbeddedLayoutMatchMissionPlanner();
    void typedWritesAndMetadataSafetyRules();
    void latestWriteWinsAcrossLateAcknowledgementsAndFailures();
    void refreshUsesThePinnedComponent();
};

void ConfigUserDefinedViewTest::initTestCase()
{
    QCoreApplication::setOrganizationName(
        QStringLiteral("APMPlannerUserParamsTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("ConfigUserDefinedView"));
}

void ConfigUserDefinedViewTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void ConfigUserDefinedViewTest::defaultsParserPersistenceAndEmptyRoundTrip()
{
    const QStringList expected = {
        QStringLiteral("CH6_OPT"), QStringLiteral("CH7_OPT"),
        QStringLiteral("CH8_OPT"), QStringLiteral("CH9_OPT"),
        QStringLiteral("CH10_OPT"), QStringLiteral("CH11_OPT"),
        QStringLiteral("CH12_OPT"), QStringLiteral("CH13_OPT"),
        QStringLiteral("CH14_OPT"), QStringLiteral("CH15_OPT"),
        QStringLiteral("CH16_OPT"), QStringLiteral("RC6_OPTION"),
        QStringLiteral("RC7_OPTION"), QStringLiteral("RC8_OPTION"),
        QStringLiteral("RC9_OPTION"), QStringLiteral("RC10_OPTION"),
        QStringLiteral("RC11_OPTION"), QStringLiteral("RC12_OPTION"),
        QStringLiteral("RC13_OPTION"), QStringLiteral("RC14_OPTION"),
        QStringLiteral("RC15_OPTION"), QStringLiteral("RC16_OPTION")
    };
    QCOMPARE(ConfigUserDefinedViewModel::DefaultOptions(), expected);

    ConfigUserDefinedViewModel defaults;
    QCOMPARE(defaults.Options(), expected);
    QCOMPARE(defaults.OptionsText(), expected.join(QStringLiteral("\r\n")));

    const QStringList parsed = ConfigUserDefinedViewModel::ParseOptions(
        QStringLiteral(" gain,MODE\r\nraw\tGAIN bad-name "
                       "ABCDEFGHIJKLMNOP ABCDEFGHIJKLMNOPQ"));
    QCOMPARE(parsed, QStringList({QStringLiteral("GAIN"),
                                  QStringLiteral("MODE"),
                                  QStringLiteral("RAW"),
                                  QStringLiteral("ABCDEFGHIJKLMNOP")}));

    defaults.ApplyOptions(QStringLiteral("raw, mode\nGAIN"));
    QCOMPARE(defaults.Options(), QStringList({QStringLiteral("RAW"),
                                              QStringLiteral("MODE"),
                                              QStringLiteral("GAIN")}));
    QCOMPARE(QSettings().value(QStringLiteral("UserParams")).toString(),
             QStringLiteral("RAW,MODE,GAIN"));
    ConfigUserDefinedViewModel restored;
    QCOMPARE(restored.Options(), defaults.Options());

    restored.ApplyOptions(QString());
    QVERIFY(restored.Options().isEmpty());
    QVERIFY(QSettings().contains(QStringLiteral("UserParams")));
    QCOMPARE(QSettings().value(QStringLiteral("UserParams")).toString(),
             QString());
    ConfigUserDefinedViewModel restoredEmpty;
    QVERIFY(restoredEmpty.Options().isEmpty());

    QSettings().setValue(QStringLiteral("UserParams"),
                         QStringLiteral("bad-name"));
    ConfigUserDefinedViewModel invalidPersisted;
    QVERIFY(invalidPersisted.Options().isEmpty());
    invalidPersisted.ApplyOptions(QString());
    QCOMPARE(QSettings().value(QStringLiteral("UserParams")).toString(),
             QString());
}

void ConfigUserDefinedViewTest::customRowsPreserveOrderAndReferenceEditorKinds()
{
    ConfigUserDefinedView view(catalogFixture());
    QSignalSpy writes(&view, &ConfigUserDefinedView::writeRequested);
    view.ApplyOptions(QStringLiteral(
        "HYBRID MASK_ONLY RAW MISSING GAIN MODE LOCKED"));
    view.setParameterSnapshot({
        {1, QStringLiteral("GAIN"), 0.5},
        {1, QStringLiteral("MODE"), 2},
        {1, QStringLiteral("HYBRID"), 1},
        {1, QStringLiteral("MASK_ONLY"), 5},
        {1, QStringLiteral("RAW"), 9},
        {1, QStringLiteral("LOCKED"), 3},
        {154, QStringLiteral("RAW"), 77},
        {1, QStringLiteral("UNRELATED"), 12}
    }, 1);
    QCOMPARE(writes.count(), 0);

    auto *editor = view.findChild<ConfigFriendlyParamsView *>(
        QStringLiteral("userParamsEditor"));
    QVERIFY(editor);
    QStringList names;
    for (const ParamField &field : editor->viewModel()->fields()) {
        names.append(field.name);
    }
    QCOMPARE(names, QStringList({QStringLiteral("HYBRID"),
                                 QStringLiteral("MASK_ONLY"),
                                 QStringLiteral("RAW"),
                                 QStringLiteral("GAIN"),
                                 QStringLiteral("MODE"),
                                 QStringLiteral("LOCKED")}));

    QCOMPARE(parameterRow(view, QStringLiteral("HYBRID"))
                 ->property("editorKind").toString(),
             QStringLiteral("combo"));
    QCOMPARE(parameterRow(view, QStringLiteral("MASK_ONLY"))
                 ->property("editorKind").toString(),
             QStringLiteral("numeric"));
    QWidget *raw = parameterRow(view, QStringLiteral("RAW"));
    QVERIFY(raw);
    QCOMPARE(raw->property("editorKind").toString(),
             QStringLiteral("numeric"));
    QCOMPARE(raw->findChild<QLabel *>(QStringLiteral("fieldLabel"))->text(),
             QStringLiteral("RAW"));
    QVERIFY(!parameterRow(view, QStringLiteral("MISSING")));
    QVERIFY(!parameterRow(view, QStringLiteral("UNRELATED")));

    QWidget *gain = parameterRow(view, QStringLiteral("GAIN"));
    auto *gainLabel = gain->findChild<QLabel *>(QStringLiteral("fieldLabel"));
    QCOMPARE(gainLabel->text(), QStringLiteral("Gain"));
    QCOMPARE(gainLabel->toolTip(), QStringLiteral("Gain description"));
    QCOMPARE(gain->findChild<QLabel *>(QStringLiteral("unitsLabel"))->text(),
             QStringLiteral("%"));
    QCOMPARE(gainLabel->minimumWidth(), 220);
    QCOMPARE(gainLabel->maximumWidth(), 220);
    auto *gainEditor = gain->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"));
    QCOMPARE(gainEditor->parentWidget()->minimumWidth(), 200);
    QCOMPARE(gainEditor->parentWidget()->maximumWidth(), 200);

    view.setCatalog(catalogFixture(), true);
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("emptyLabel"))->text(),
             QStringLiteral(
                 "No selected parameters are available on this vehicle."));
}

void ConfigUserDefinedViewTest::modifyAndEmbeddedLayoutMatchMissionPlanner()
{
    ConfigUserDefinedView view(catalogFixture());
    view.ApplyOptions(QStringLiteral("RAW MODE"));
    view.setParameterSnapshot({
        {1, QStringLiteral("RAW"), 3},
        {1, QStringLiteral("MODE"), 0}
    });
    QCOMPARE(view.objectName(), QStringLiteral("ConfigUserDefinedView"));
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("userParamsTitle"))->text(),
             QStringLiteral("User Params"));
    auto *modify = view.findChild<QPushButton *>(
        QStringLiteral("modifyButton"));
    auto *refresh = view.findChild<QPushButton *>(
        QStringLiteral("refreshUserParamsButton"));
    QVERIFY(modify);
    QVERIFY(refresh);
    QCOMPARE(modify->text(), QStringLiteral("Modify"));
    QCOMPARE(refresh->text(), QStringLiteral("Refresh Params"));

    auto *editor = view.findChild<ConfigFriendlyParamsView *>(
        QStringLiteral("userParamsEditor"));
    QVERIFY(editor);
    QVERIFY(editor->findChild<QLabel *>(
        QStringLiteral("friendlyParamsTitle"))->isHidden());
    QVERIFY(editor->findChild<QPushButton *>(
        QStringLiteral("refreshButton"))->isHidden());
    QVERIFY(editor->findChild<QLineEdit *>(
        QStringLiteral("searchBox"))->isHidden());
    QVERIFY(editor->findChild<QComboBox *>(
        QStringLiteral("componentSelector"))->isHidden());
    for (QToolButton *favorite : editor->findChildren<QToolButton *>(
             QStringLiteral("favoriteButton"))) {
        QVERIFY(favorite->isHidden());
    }

    const QStringList before = view.viewModel()->Options();
    bool dialogInspected = false;
    QTimer::singleShot(0, this, [&]() {
        QDialog *dialog = nullptr;
        for (QWidget *topLevel : QApplication::topLevelWidgets()) {
            if (topLevel->objectName()
                == QStringLiteral("userParamsModifyDialog")) {
                dialog = qobject_cast<QDialog *>(topLevel);
                break;
            }
        }
        if (!dialog) {
            return;
        }
        dialogInspected = true;
        QCOMPARE(dialog->objectName(),
                 QStringLiteral("userParamsModifyDialog"));
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Params"));
        QCOMPARE(dialog->size(), QSize(360, 320));
        QCOMPARE(dialog->findChild<QLabel *>(
                     QStringLiteral("userParamsPrompt"))->text(),
                 QStringLiteral(
                     "Enter Param Names (comma or newline separated)"));
        auto *textEdit = dialog->findChild<QPlainTextEdit *>(
            QStringLiteral("userParamsTextEdit"));
        QCOMPARE(textEdit->toPlainText(), QStringLiteral("RAW\nMODE"));
        QCOMPARE(textEdit->lineWrapMode(), QPlainTextEdit::NoWrap);
        textEdit->setPlainText(QStringLiteral("GAIN"));
        dialog->reject();
    });
    QTimer::singleShot(1000, this, []() {
        for (QWidget *topLevel : QApplication::topLevelWidgets()) {
            if (topLevel->objectName()
                == QStringLiteral("userParamsModifyDialog")) {
                topLevel->close();
            }
        }
    });
    modify->click();
    QVERIFY(dialogInspected);
    QCOMPARE(view.viewModel()->Options(), before);
    QCOMPARE(QSettings().value(QStringLiteral("UserParams")).toString(),
             QStringLiteral("RAW,MODE"));
}

void ConfigUserDefinedViewTest::typedWritesAndMetadataSafetyRules()
{
    ConfigUserDefinedView view(catalogFixture());
    view.ApplyOptions(QStringLiteral("MODE RAW GAIN LOCKED"));
    QSignalSpy writes(&view, &ConfigUserDefinedView::writeRequested);
    view.setParameterSnapshot({
        {1, QStringLiteral("MODE"), 0},
        {1, QStringLiteral("RAW"), 4},
        {1, QStringLiteral("GAIN"), 0.5},
        {1, QStringLiteral("LOCKED"), 7}
    });
    QCOMPARE(writes.count(), 0);

    QWidget *mode = parameterRow(view, QStringLiteral("MODE"));
    auto *combo = mode->findChild<QComboBox *>(
        QStringLiteral("valueComboBox"));
    combo->setCurrentIndex(combo->findData(2));
    QVERIFY(QMetaObject::invokeMethod(
        combo, "activated", Q_ARG(int, combo->currentIndex())));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toInt(), 1);
    QCOMPARE(writes.first().at(1).toString(), QStringLiteral("MODE"));
    QCOMPARE(writes.first().at(2).toInt(), 2);

    QWidget *raw = parameterRow(view, QStringLiteral("RAW"));
    auto *numeric = raw->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"));
    numeric->setValue(6);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    QCOMPARE(writes.count(), 2);
    QCOMPARE(writes.last().at(2).userType(), int(QMetaType::Int));
    QCOMPARE(writes.last().at(2).toInt(), 6);

    QWidget *gain = parameterRow(view, QStringLiteral("GAIN"));
    auto *gainEditor = gain->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"));
    gainEditor->setValue(2.0);
    QVERIFY(QMetaObject::invokeMethod(gainEditor, "editingFinished"));
    QCOMPARE(writes.count(), 2);
    QCOMPARE(statusText(gain), QStringLiteral("out of range"));
    QVERIFY(gainEditor->parentWidget()->property("outOfRange").toBool());

    QWidget *locked = parameterRow(view, QStringLiteral("LOCKED"));
    QVERIFY(!locked->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"))->isEnabled());
}

void ConfigUserDefinedViewTest::latestWriteWinsAcrossLateAcknowledgementsAndFailures()
{
    ConfigUserDefinedView view(catalogFixture());
    view.ApplyOptions(QStringLiteral("RAW"));
    view.setParameterSnapshot({{1, QStringLiteral("RAW"), 1}});
    QSignalSpy writes(&view, &ConfigUserDefinedView::writeRequested);
    QWidget *row = parameterRow(view, QStringLiteral("RAW"));
    auto *numeric = row->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"));

    numeric->setValue(2);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    numeric->setValue(3);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    QCOMPARE(writes.count(), 2);

    view.setCatalog(catalogFixture(), true);
    view.ApplyOptions(QStringLiteral("MODE RAW"));
    view.setParameterSnapshot({{1, QStringLiteral("RAW"), 1}});
    row = parameterRow(view, QStringLiteral("RAW"));
    numeric = row->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"));
    QCOMPARE(numeric->value(), 3.0);
    QCOMPARE(statusText(row), QStringLiteral("…"));
    QCOMPARE(writes.count(), 2);

    view.parameterChanged(1, QStringLiteral("RAW"), 2);
    QCOMPARE(numeric->value(), 3.0);
    QCOMPARE(statusText(row), QStringLiteral("…"));
    view.parameterChanged(1, QStringLiteral("RAW"), 3);
    QCOMPARE(statusText(row), QStringLiteral("✓"));
    view.parameterChanged(1, QStringLiteral("RAW"), 2);
    QCOMPARE(numeric->value(), 3.0);
    QCOMPARE(statusText(row), QStringLiteral("…"));
    QTRY_COMPARE_WITH_TIMEOUT(writes.count(), 3, 250);
    QCOMPARE(writes.last().at(2).toInt(), 3);
    view.parameterChanged(1, QStringLiteral("RAW"), 3);
    QCOMPARE(statusText(row), QStringLiteral("✓"));

    view.setParameterSnapshot({{1, QStringLiteral("RAW"), 2}});
    row = parameterRow(view, QStringLiteral("RAW"));
    numeric = row->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"));
    QCOMPARE(numeric->value(), 3.0);
    QCOMPARE(statusText(row), QStringLiteral("…"));
    QTRY_COMPARE_WITH_TIMEOUT(writes.count(), 4, 250);
    QCOMPARE(writes.last().at(2).toInt(), 3);
    view.parameterChanged(1, QStringLiteral("RAW"), 3);
    QCOMPARE(statusText(row), QStringLiteral("✓"));

    // Reverting to the authoritative value still sends a correction because
    // the older in-flight value may reach the vehicle afterwards.
    numeric->setValue(4);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    numeric->setValue(3);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    QCOMPARE(writes.count(), 6);
    QCOMPARE(writes.last().at(2).toInt(), 3);
    view.parameterChanged(1, QStringLiteral("RAW"), 4);
    QCOMPARE(numeric->value(), 3.0);
    view.parameterChanged(1, QStringLiteral("RAW"), 3);
    QCOMPARE(statusText(row), QStringLiteral("✓"));

    numeric->setValue(6);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    numeric->setValue(7);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    view.parameterWriteFailed(1, QStringLiteral("RAW"), 6,
                              QStringLiteral("old failure"));
    QCOMPARE(statusText(row), QStringLiteral("…"));
    view.parameterWriteFailed(1, QStringLiteral("RAW"), 7,
                              QStringLiteral("latest failure"));
    QCOMPARE(statusText(row), QStringLiteral("latest failure"));

    numeric->setValue(8);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    view.parameterChanged(1, QStringLiteral("RAW"), 9);
    QCOMPARE(numeric->value(), 9.0);
    QCOMPARE(statusText(row), QStringLiteral("write mismatch"));

    const int beforeCyclicWrites = writes.count();
    numeric->setValue(10);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    numeric->setValue(11);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    numeric->setValue(10);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    QCOMPARE(writes.count(), beforeCyclicWrites + 3);
    view.parameterChanged(1, QStringLiteral("RAW"), 10);
    QCOMPARE(statusText(row), QStringLiteral("✓"));
    const int afterCyclicAck = writes.count();
    view.parameterChanged(1, QStringLiteral("RAW"), 10);
    QCoreApplication::processEvents();
    QCOMPARE(writes.count(), afterCyclicAck);
    QVERIFY(statusText(row).isEmpty());
}

void ConfigUserDefinedViewTest::refreshUsesThePinnedComponent()
{
    ConfigUserDefinedView view(catalogFixture());
    view.ApplyOptions(QStringLiteral("RAW"));
    view.setParameterSnapshot({{154, QStringLiteral("RAW"), 12}}, 154);
    QSignalSpy refresh(&view, &ConfigUserDefinedView::refreshRequested);
    view.findChild<QPushButton *>(
        QStringLiteral("refreshUserParamsButton"))->click();
    QCOMPARE(refresh.count(), 1);
    QCOMPARE(refresh.first().at(0).toInt(), 154);
}

QTEST_MAIN(ConfigUserDefinedViewTest)

#include "test_configuserdefinedview.moc"
