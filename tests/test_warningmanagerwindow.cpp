#include "services/WarningEngine.h"
#include "ui/WarningManagerWindow.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest/QTest>

namespace
{

void flushRebuild()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

QString idName(const char *prefix, quint64 id)
{
    return QString::fromLatin1(prefix) + QString::number(id);
}

} // namespace

class WarningManagerWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void modelessSurfaceAndImmediateEditsSaveAndReopen();
    void importedThresholdOutsideMpEditorRangeIsNotClamped();
    void childRemovalSplicesChainAndRootRemovalDropsChain();
    void serviceDeletionFailsClosed();
    void reentrantListenerMayDeleteServiceDuringSetRules();
};

void WarningManagerWindowTest::modelessSurfaceAndImmediateEditsSaveAndReopen()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("warnings.json"));
    WarningEngine engine(path, []() {
        return WarningEngine::Values{{QStringLiteral("battery_voltage"), 12.4}};
    });
    QWidget owner;
    owner.setGeometry(100, 100, 1400, 900);
    auto *window = new WarningManagerWindow(
        &engine,
        {QStringLiteral("ground_speed"), QStringLiteral("battery_voltage")},
        &owner);

    QCOMPARE(window->objectName(), QStringLiteral("WarningManagerWindow"));
    QCOMPARE(window->windowTitle(), QStringLiteral("Warning Manager"));
    QCOMPARE(window->windowType(), Qt::Window);
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->size(), QSize(1250, 620));
    QCOMPARE(window->minimumWidth(), 1000);
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(window->engine(), &engine);
    QVERIFY(window->findChild<QWidget *>(QStringLiteral("WarningRules")));
    QVERIFY(window->findChild<QLabel *>(
        QStringLiteral("WarningEmptyInstructions")));

    auto *add = window->findChild<QPushButton *>(
        QStringLiteral("AddWarningButton"));
    auto *save = window->findChild<QPushButton *>(
        QStringLiteral("SaveWarningsButton"));
    QVERIFY(add);
    QVERIFY(save);
    add->click();
    flushRebuild();
    QCOMPARE(engine.rules().size(), 1);
    const quint64 id = engine.rules().first().id;
    QVERIFY(id != 0);

    auto *source = window->findChild<QComboBox *>(
        idName("WarningSource_", id));
    auto *condition = window->findChild<QComboBox *>(
        idName("WarningCondition_", id));
    auto *threshold = window->findChild<QLineEdit *>(
        idName("WarningThreshold_", id));
    auto *type = window->findChild<QComboBox *>(
        idName("WarningType_", id));
    auto *color = window->findChild<QComboBox *>(
        idName("WarningColor_", id));
    auto *repeat = window->findChild<QSpinBox *>(
        idName("WarningRepeat_", id));
    auto *text = window->findChild<QLineEdit *>(
        idName("WarningText_", id));
    QVERIFY(source && source->isEditable());
    QVERIFY(condition);
    QVERIFY(threshold);
    QVERIFY(type);
    QVERIFY(color);
    QVERIFY(repeat);
    QVERIFY(text);
    QLabel *const telemetry = window->findChild<QLabel *>(
        idName("WarningTelemetry_", id));
    QVERIFY(telemetry);
    QVERIFY(telemetry->text().contains(QStringLiteral("Inactive")));

    condition->setCurrentIndex(condition->findData(
        static_cast<int>(CustomWarning::GTEQ)));
    QVERIFY(telemetry->text().contains(QStringLiteral("Unavailable")));
    window->setTelemetryValues(
        {{QStringLiteral("battery_voltage"), 12.4}});
    QVERIFY(telemetry->text().contains(QStringLiteral("Current: 12.4 V")));
    QVERIFY(threshold->toolTip().contains(QStringLiteral("V")));
    QLabel *const help = window->findChild<QLabel *>(
        QStringLiteral("WarningManagerHelp"));
    QVERIFY(help);
    QVERIFY(help->text().contains(QStringLiteral("warnings.xml")));
    QVERIFY(help->text().contains(QStringLiteral("feet or knots")));

    window->show();
    source->lineEdit()->setFocus();
    QPointer<QComboBox> stableSource(source);
    source->setEditText(QStringLiteral("legacy.unknown_field"));
    QCOMPARE(window->findChild<QComboBox *>(idName("WarningSource_", id)),
             stableSource.data());
    QCOMPARE(engine.rules().first().name,
             QStringLiteral("legacy.unknown_field"));
    QCOMPARE(source->currentText(), QStringLiteral("legacy.unknown_field"));
    QVERIFY(telemetry->text().contains(QStringLiteral("Unknown source")));

    threshold->setText(QStringLiteral("17.25"));
    QVERIFY(QMetaObject::invokeMethod(threshold, "editingFinished"));
    type->setCurrentIndex(type->findData(
        static_cast<int>(CustomWarning::Coloring)));
    color->setCurrentText(QStringLiteral("Red"));
    repeat->setValue(42);
    text->setText(QStringLiteral("Check {name}: {value}"));
    const CustomWarning edited = engine.rules().first();
    QCOMPARE(edited.condition, CustomWarning::GTEQ);
    QCOMPARE(edited.threshold, 17.25);
    QCOMPARE(edited.type, CustomWarning::Coloring);
    QCOMPARE(edited.color, QStringLiteral("Red"));
    QCOMPARE(edited.repeatSeconds, 42);
    QCOMPARE(edited.text, QStringLiteral("Check {name}: {value}"));
    QVERIFY(engine.dirty());

    QPointer<QLineEdit> rejectedEditor(text);
    text->setText(QString(5000, QLatin1Char('x')));
    flushRebuild();
    QVERIFY(rejectedEditor.isNull());
    QLineEdit *const restoredText = window->findChild<QLineEdit *>(
        idName("WarningText_", id));
    QVERIFY(restoredText);
    QCOMPARE(restoredText->text(), QStringLiteral("Check {name}: {value}"));
    QCOMPARE(engine.rules().first().text,
             QStringLiteral("Check {name}: {value}"));

    save->click();
    QVERIFY(!engine.dirty());
    QLabel *const status = window->findChild<QLabel *>(
        QStringLiteral("WarningStatus"));
    QVERIFY(status);
    QVERIFY(status->text().contains(QStringLiteral("Saved 1")));

    WarningEngine reopened(path);
    QString error;
    QVERIFY2(reopened.load(&error), qPrintable(error));
    QCOMPARE(reopened.rules().size(), 1);
    QCOMPARE(reopened.rules().first().name,
             QStringLiteral("legacy.unknown_field"));
    WarningManagerWindow reopenedWindow(
        &reopened, {QStringLiteral("battery_voltage")});
    QComboBox *const unknown = reopenedWindow.findChild<QComboBox *>(
        idName("WarningSource_", reopened.rules().first().id));
    QVERIFY(unknown);
    QCOMPARE(unknown->currentText(), QStringLiteral("legacy.unknown_field"));
    QVERIFY(unknown->findText(QStringLiteral("legacy.unknown_field"),
                              Qt::MatchExactly) >= 0);

    delete window;
    QCOMPARE(engine.parent(), nullptr);
}

void WarningManagerWindowTest::importedThresholdOutsideMpEditorRangeIsNotClamped()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    WarningEngine engine(directory.filePath(QStringLiteral("warnings.xml")));
    CustomWarning imported;
    imported.name = QStringLiteral("altitude");
    imported.condition = CustomWarning::GT;
    imported.threshold = 1.234567890123456e100;
    QString error;
    QVERIFY2(engine.setRules(QVector<CustomWarning>{imported}, &error),
             qPrintable(error));
    const double activeThreshold = engine.rules().first().threshold;

    WarningManagerWindow window(&engine, {QStringLiteral("altitude")});
    QLineEdit *const editor = window.findChild<QLineEdit *>(
        idName("WarningThreshold_", engine.rules().first().id));
    QVERIFY(editor);
    bool parsed = false;
    const double displayed = QLocale::c().toDouble(editor->text(), &parsed);
    QVERIFY(parsed);
    QCOMPARE(displayed, activeThreshold);
    QCOMPARE(engine.rules().first().threshold, activeThreshold);
    QVERIFY(editor->toolTip().contains(QStringLiteral("-999999..999999")));
}

void WarningManagerWindowTest::childRemovalSplicesChainAndRootRemovalDropsChain()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    WarningEngine engine(directory.filePath(QStringLiteral("warnings.json")));
    WarningManagerWindow window(
        &engine, {QStringLiteral("altitude"), QStringLiteral("ground_speed")});
    QPushButton *const rootAdd = window.findChild<QPushButton *>(
        QStringLiteral("AddWarningButton"));
    QVERIFY(rootAdd);
    rootAdd->click();
    flushRebuild();

    quint64 rootId = engine.rules().first().id;
    QPushButton *addChild = window.findChild<QPushButton *>(
        idName("WarningAddChild_", rootId));
    QVERIFY(addChild);
    QVERIFY(addChild->isEnabled());
    addChild->click();
    flushRebuild();
    QCOMPARE(engine.rules().first().child.size(), 1);
    const quint64 childId = engine.rules().first().child.first().id;
    addChild = window.findChild<QPushButton *>(
        idName("WarningAddChild_", childId));
    QVERIFY(addChild);
    addChild->click();
    flushRebuild();
    QCOMPARE(engine.rules().first().child.first().child.size(), 1);
    const quint64 grandchildId =
        engine.rules().first().child.first().child.first().id;

    QPushButton *const rootPlus = window.findChild<QPushButton *>(
        idName("WarningAddChild_", rootId));
    QVERIFY(rootPlus);
    QVERIFY(!rootPlus->isEnabled());
    QPushButton *removeChild = window.findChild<QPushButton *>(
        idName("WarningRemove_", childId));
    QVERIFY(removeChild);
    removeChild->click();
    flushRebuild();
    QCOMPARE(engine.rules().first().child.size(), 1);
    QCOMPARE(engine.rules().first().child.first().id, grandchildId);
    QVERIFY(!window.findChild<QWidget *>(idName("WarningRule_", childId)));
    QVERIFY(window.findChild<QWidget *>(idName("WarningRule_", grandchildId)));

    QPushButton *const removeRoot = window.findChild<QPushButton *>(
        idName("WarningRemove_", rootId));
    QVERIFY(removeRoot);
    removeRoot->click();
    flushRebuild();
    QVERIFY(engine.rules().isEmpty());
    QVERIFY(window.findChild<QLabel *>(
        QStringLiteral("WarningEmptyInstructions")));
}

void WarningManagerWindowTest::serviceDeletionFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto *engine = new WarningEngine(
        directory.filePath(QStringLiteral("warnings.json")));
    auto *window = new WarningManagerWindow(
        engine, {QStringLiteral("altitude")});
    QCOMPARE(window->engine(), engine);
    delete engine;
    QCOMPARE(window->engine(), nullptr);
    QLabel *const status = window->findChild<QLabel *>(
        QStringLiteral("WarningStatus"));
    QVERIFY(status);
    QVERIFY(status->text().contains(QStringLiteral("unavailable"),
                                    Qt::CaseInsensitive));
    QVERIFY(!window->findChild<QPushButton *>(
        QStringLiteral("AddWarningButton"))->isEnabled());
    QVERIFY(!window->findChild<QPushButton *>(
        QStringLiteral("SaveWarningsButton"))->isEnabled());
    flushRebuild();
    QVERIFY(window->findChild<QLabel *>(
        QStringLiteral("WarningEmptyInstructions")));
    delete window;
}

void WarningManagerWindowTest::reentrantListenerMayDeleteServiceDuringSetRules()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto *engine = new WarningEngine(
        directory.filePath(QStringLiteral("warnings.xml")));
    auto *window = new WarningManagerWindow(
        engine, {QStringLiteral("altitude")});
    QPointer<WarningEngine> guardedEngine(engine);
    connect(engine, &WarningEngine::rulesChanged, window,
            [engine]() {
        delete engine;
    });
    QPushButton *const add = window->findChild<QPushButton *>(
        QStringLiteral("AddWarningButton"));
    QVERIFY(add);
    add->click();
    QVERIFY(guardedEngine.isNull());
    QCOMPARE(window->engine(), nullptr);
    QLabel *const status = window->findChild<QLabel *>(
        QStringLiteral("WarningStatus"));
    QVERIFY(status);
    QVERIFY(status->text().contains(QStringLiteral("unavailable"),
                                    Qt::CaseInsensitive));
    delete window;
}

QTEST_MAIN(WarningManagerWindowTest)
#include "test_warningmanagerwindow.moc"
