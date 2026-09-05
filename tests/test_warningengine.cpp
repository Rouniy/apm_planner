#include "services/WarningEngine.h"

#include <QFile>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <limits>

namespace {
CustomWarning spoken(const QString &name = QStringLiteral("alt"),
                     CustomWarning::Conditional condition = CustomWarning::GT,
                     double threshold = 10)
{
    CustomWarning rule;
    rule.name = name;
    rule.condition = condition;
    rule.threshold = threshold;
    return rule;
}

CustomWarning colored(const QString &name, const QString &color, double threshold = 10)
{
    CustomWarning rule = spoken(name, CustomWarning::GT, threshold);
    rule.type = CustomWarning::Coloring;
    rule.color = color;
    return rule;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray wrap(const QByteArray &fields)
{
    return QByteArray("<?xml version=\"1.0\"?><ArrayOfCustomWarning "
                      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                      "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><CustomWarning>")
        + fields + "</CustomWarning></ArrayOfCustomWarning>";
}
} // namespace

class WarningEngineTest : public QObject
{
    Q_OBJECT
private slots:
    void referenceDefaultsAndTokens();
    void predicates_data();
    void predicates();
    void andChainCooldownAndEpoch();
    void coloringOrderAndExpiry();
    void editsPreserveOtherCooldownsAndColors();
    void unavailableAndIndependentRoots();
    void xmlReferenceRoundTrip();
    void xmlRejectsWithoutReplacing_data();
    void xmlRejectsWithoutReplacing();
    void configurationBoundsAndIdentities();
    void persistenceFailureAndMissingFile();
    void reentrantProviderAndClock();
    void reentrantOutputsAndEdits();
    void timerStartsOnlyOnRequest();
};

void WarningEngineTest::referenceDefaultsAndTokens()
{
    const CustomWarning rule;
    QCOMPARE(rule.condition, CustomWarning::NONE);
    QCOMPARE(rule.threshold, 0.0);
    QCOMPARE(rule.type, CustomWarning::SpeakAndText);
    QCOMPARE(rule.repeatSeconds, 10);
    QCOMPARE(rule.text, QStringLiteral("WARNING: {name} is {value}"));
    QCOMPARE(rule.color, QStringLiteral("NoColor"));
    QCOMPARE(WarningEngine::conditionNames(), QStringList({"NONE", "LT", "LTEQ", "EQ", "GT", "GTEQ", "NEQ"}));
    QCOMPARE(WarningEngine::typeNames(), QStringList({"SpeakAndText", "Coloring"}));
    QCOMPARE(WarningEngine::colorNames(), QStringList({"NoColor", "Red", "OrangeRed", "Maroon", "Yellow", "Gold", "Goldenrod", "LawnGreen", "Green", "DarkGreen"}));
    auto textRule = spoken(QStringLiteral("voltage & резерв"), CustomWarning::LT, 10.126);
    textRule.text = QStringLiteral("{name}={value}; limit {warning}; {unknown}");
    QCOMPARE(WarningEngine::formatText(textRule, 9.1), QStringLiteral("voltage & резерв=9.1; limit 10.13; {unknown}"));
    QCOMPARE(WarningEngine::formatText(textRule, -0.001), QStringLiteral("voltage & резерв=0; limit 10.13; {unknown}"));
}

void WarningEngineTest::predicates_data()
{
    QTest::addColumn<int>("condition");
    QTest::addColumn<double>("value");
    QTest::addColumn<bool>("expected");
    for (int condition = 0; condition < 7; ++condition) {
        for (int value = 9; value <= 11; ++value) {
            const bool expected = condition == CustomWarning::LT ? value < 10
                : condition == CustomWarning::LTEQ ? value <= 10
                : condition == CustomWarning::EQ ? value == 10
                : condition == CustomWarning::GT ? value > 10
                : condition == CustomWarning::GTEQ ? value >= 10
                : condition == CustomWarning::NEQ ? value != 10 : false;
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(condition).arg(value)))
                << condition << double(value) << expected;
        }
    }
}

void WarningEngineTest::predicates()
{
    QFETCH(int, condition);
    QFETCH(double, value);
    QFETCH(bool, expected);
    WarningEngine engine({}, [value] { return WarningEngine::Values{{"alt", value}}; }, [] { return 1000; });
    QVERIFY(engine.setRules({spoken("alt", static_cast<CustomWarning::Conditional>(condition))}));
    QSignalSpy warnings(&engine, &WarningEngine::warningMessage);
    engine.tick();
    QCOMPARE(warnings.count(), expected ? 1 : 0);
}

void WarningEngineTest::andChainCooldownAndEpoch()
{
    qint64 now = 0;
    WarningEngine::Values values{{"alt", 20}, {"battery", 1}, {"satcount", 1}};
    WarningEngine engine({}, [&] { return values; }, [&] { return now; });
    CustomWarning root = spoken();
    root.text = QStringLiteral("root {value}");
    CustomWarning child = spoken("battery", CustomWarning::LT, 0);
    child.repeatSeconds = 86400; // Children are predicates, not separate actions/timers.
    child.text = QStringLiteral("must not speak child");
    child.child = {spoken("satcount", CustomWarning::GT, 0)};
    root.child = {child};
    QVERIFY(engine.setRules({root}));
    QSignalSpy warnings(&engine, &WarningEngine::warningMessage);
    engine.tick();
    QCOMPARE(warnings.count(), 0);
    values["battery"] = -1;
    now = 1;
    engine.tick(); // Failed AND did not consume the root's cooldown at time zero.
    QCOMPARE(warnings.count(), 1);
    QCOMPARE(warnings.at(0).at(0).toString(), QStringLiteral("root 20"));
    now = 10000;
    engine.tick();
    QCOMPARE(warnings.count(), 1);
    now = 10001;
    engine.tick();
    QCOMPARE(warnings.count(), 2);
    now = 2; // Monotonic-clock rollback does not produce a burst.
    engine.tick();
    QCOMPARE(warnings.count(), 2);
    engine.resetEpoch();
    engine.tick();
    QCOMPARE(warnings.count(), 3);
    root.repeatSeconds = 0;
    QVERIFY(engine.setRules({root}));
    engine.tick();
    engine.tick();
    QCOMPARE(warnings.count(), 5);
    values.remove("satcount");
    now += 100000;
    engine.tick();
    QCOMPARE(warnings.count(), 5);
}

void WarningEngineTest::coloringOrderAndExpiry()
{
    qint64 now = 0;
    WarningEngine::Values values{{"alt", 20}};
    WarningEngine engine({}, [&] { return values; }, [&] { return now; });
    QVERIFY(engine.setRules({colored("alt", "Red"), colored("alt", "Green", 30)}));
    QSignalSpy colors(&engine, &WarningEngine::colorsChanged);
    QSignalSpy warnings(&engine, &WarningEngine::warningMessage);
    engine.tick();
    QCOMPARE(engine.colors().value("alt"), QStringLiteral("Red")); // False later root does not erase red.
    engine.tick();
    QCOMPARE(colors.count(), 1); // Continuous color does not pulse off during a repeat interval.
    values["alt"] = 40;
    engine.tick();
    QCOMPARE(engine.colors().value("alt"), QStringLiteral("Green"));
    QCOMPARE(colors.count(), 2);
    values.clear(); // Provider expires telemetry; stale colors are removed on the next 250 ms tick.
    engine.tick();
    QVERIFY(engine.colors().isEmpty());
    QCOMPARE(colors.count(), 3);
    values["alt"] = 20;
    engine.tick();
    engine.resetEpoch();
    QVERIFY(engine.colors().isEmpty());
    engine.tick();
    QVERIFY(engine.setRules({}));
    QVERIFY(engine.colors().isEmpty());
    QVERIFY(engine.setRules({colored("alt", "Red"), colored("alt", "NoColor")}));
    engine.tick();
    QVERIFY(engine.colors().isEmpty()); // Last matching NoColor is an explicit reset.
    QCOMPARE(warnings.count(), 0);
    for (const QString &color : WarningEngine::colorNames()) {
        QVERIFY(engine.setRules({colored("alt", color)}));
        engine.tick();
        QCOMPARE(engine.colors().value("alt", "NoColor"), color);
    }
}

void WarningEngineTest::unavailableAndIndependentRoots()
{
    WarningEngine::Values values{{"nan", std::numeric_limits<double>::quiet_NaN()},
        {"inf", std::numeric_limits<double>::infinity()}, {"alt", 20}, {"zero", 0}};
    WarningEngine engine({}, [&] { return values; });
    QVERIFY(engine.setRules({spoken("unavailable", CustomWarning::EQ, 0),
        spoken("nan", CustomWarning::NEQ, 0), spoken("inf", CustomWarning::GT, 0),
        spoken("Unknown.CustomState.Name", CustomWarning::LT, 1), spoken(),
        spoken("zero", CustomWarning::EQ, 0)}));
    QSignalSpy warnings(&engine, &WarningEngine::warningMessage);
    engine.tick();
    QCOMPARE(warnings.count(), 2);
    QCOMPARE(warnings.at(0).at(0).toString(), QStringLiteral("WARNING: alt is 20"));
    QCOMPARE(warnings.at(1).at(0).toString(), QStringLiteral("WARNING: zero is 0"));
    WarningEngine failing({}, []() -> WarningEngine::Values { throw 7; });
    QVERIFY(failing.setRules({spoken("alt", CustomWarning::EQ, 0)}));
    QSignalSpy failed(&failing, &WarningEngine::warningMessage);
    failing.tick();
    QCOMPARE(failed.count(), 0);
}

void WarningEngineTest::xmlReferenceRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("предупреждения.xml"));
    const QByteArray source = wrap(
        "<Child><Child xsi:nil=\"true\"/><Name>battery_voltage</Name><Warning>10.125</Warning>"
        "<type>Coloring</type><color>Gold</color><RepeatTime>17</RepeatTime>"
        "<ConditionType>LTEQ</ConditionType><Text>child action preserved</Text></Child>"
        "<Name>Unknown.Future.Field</Name><Warning>-1.2345678901234567e-12</Warning>"
        "<type>SpeakAndText</type><color>Red</color><RepeatTime>0</RepeatTime>"
        "<ConditionType>NEQ</ConditionType><Text>{name} &amp; &lt; {warning} / {value}</Text>");
    QVERIFY(writeFile(path, source));
    WarningEngine engine(path);
    QString error;
    QVERIFY2(engine.load(&error), qPrintable(error));
    QVERIFY(!engine.dirty());
    QCOMPARE(engine.rules().size(), 1);
    const CustomWarning root = engine.rules().first();
    QVERIFY(root.id > 0);
    QCOMPARE(root.name, QStringLiteral("Unknown.Future.Field"));
    QCOMPARE(root.threshold, -1.2345678901234567e-12);
    QCOMPARE(root.condition, CustomWarning::NEQ);
    QCOMPARE(root.repeatSeconds, 0);
    QCOMPARE(root.child.size(), 1);
    QCOMPARE(root.child.first().type, CustomWarning::Coloring);
    QCOMPARE(root.child.first().color, QStringLiteral("Gold"));
    QCOMPARE(root.child.first().text, QStringLiteral("child action preserved"));
    QCOMPARE(root.child.first().repeatSeconds, 17);
    QVERIFY(root.child.first().id != root.id);
    QVERIFY(root.child.first().child.isEmpty());
    QVERIFY2(engine.save(&error), qPrintable(error));
    const QByteArray output = readFile(path);
    QVERIFY(output.contains("ArrayOfCustomWarning"));
    QVERIFY(!output.contains("<id>"));
    QVERIFY(output.contains("<Child>"));
    QVERIFY(output.contains("{name} &amp; &lt; {warning} / {value}"));
    QVERIFY2(engine.load(&error), qPrintable(error));
    QVERIFY(engine.rules().first().id > root.child.first().id);
    QCOMPARE(engine.rules().first().threshold, root.threshold);
    QCOMPARE(engine.rules().first().child.first().text, root.child.first().text);
    QVERIFY(writeFile(path, wrap("<Child xsi:nil=\"false\"><Name>armed</Name></Child><Name>alt</Name><color xsi:nil=\"true\"/>")));
    QVERIFY2(engine.load(&error), qPrintable(error));
    QCOMPARE(engine.rules().first().condition, CustomWarning::NONE);
    QCOMPARE(engine.rules().first().color, QStringLiteral("NoColor"));
    QCOMPARE(engine.rules().first().child.first().name, QStringLiteral("armed"));
    QVERIFY(writeFile(path, "<ArrayOfCustomWarning/>"));
    QVERIFY(engine.load());
    QVERIFY(engine.rules().isEmpty());
}

void WarningEngineTest::xmlRejectsWithoutReplacing_data()
{
    QTest::addColumn<QByteArray>("xml");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("truncated") << QByteArray("<ArrayOfCustomWarning><CustomWarning>");
    QTest::newRow("unknown-root") << QByteArray("<Warnings/>");
    QTest::newRow("second-root") << wrap("") + "<Other/>";
    QTest::newRow("garbage") << wrap("") + "trailing";
    QTest::newRow("dtd") << QByteArray("<!DOCTYPE ArrayOfCustomWarning [<!ENTITY x 'alt'>]>") + wrap("<Name>&x;</Name>");
    QTest::newRow("valid-dtd") << QByteArray("<!DOCTYPE ArrayOfCustomWarning>") + wrap("").mid(21);
    QTest::newRow("duplicate") << wrap("<Name>alt</Name><Name>pitch</Name>");
    QTest::newRow("unknown-property") << wrap("<CurrentValue>1</CurrentValue>");
    QTest::newRow("unknown-condition") << wrap("<ConditionType>Approximately</ConditionType>");
    QTest::newRow("numeric-condition") << wrap("<ConditionType>4</ConditionType>");
    QTest::newRow("unknown-action") << wrap("<type>SendCommand</type>");
    QTest::newRow("unknown-color") << wrap("<color>Magenta</color>");
    QTest::newRow("nan") << wrap("<Warning>NaN</Warning>");
    QTest::newRow("infinite") << wrap("<Warning>1e999</Warning>");
    QTest::newRow("grouped-double") << wrap("<Warning>1,234</Warning>");
    QTest::newRow("negative-repeat") << wrap("<RepeatTime>-1</RepeatTime>");
    QTest::newRow("large-repeat") << wrap("<RepeatTime>86401</RepeatTime>");
    QTest::newRow("fractional-repeat") << wrap("<RepeatTime>0.25</RepeatTime>");
    QTest::newRow("nested-scalar") << wrap("<Name><Hidden>alt</Hidden></Name>");
    QTest::newRow("nil-child-with-data") << wrap("<Child xsi:nil=\"true\"><Name>alt</Name></Child>");
    QTest::newRow("duplicate-child") << wrap("<Child/><Child/>");
    QTest::newRow("unknown-attribute") << wrap("<Name setting=\"true\">alt</Name>");
    QTest::newRow("namespace") << wrap("<Name xmlns=\"other\">alt</Name>");
    QTest::newRow("long-name") << wrap("<Name>" + QByteArray(257, 'a') + "</Name>");
    QTest::newRow("long-text") << wrap("<Text>" + QByteArray(4097, 'a') + "</Text>");
    QTest::newRow("oversize") << QByteArray(4 * 1024 * 1024 + 1, ' ');
    QByteArray deep;
    for (int i = 0; i < 16; ++i) deep += "<Child>";
    for (int i = 0; i < 16; ++i) deep += "</Child>";
    QTest::newRow("too-deep") << wrap(deep);
    QByteArray many("<ArrayOfCustomWarning>");
    for (int i = 0; i < 513; ++i) many += "<CustomWarning/>";
    many += "</ArrayOfCustomWarning>";
    QTest::newRow("too-many") << many;
}

void WarningEngineTest::xmlRejectsWithoutReplacing()
{
    QFETCH(QByteArray, xml);
    QTemporaryDir dir;
    WarningEngine engine(dir.filePath("warnings.xml"));
    QVERIFY(engine.setRules({spoken()}));
    const quint64 previousId = engine.rules().first().id;
    QVERIFY(writeFile(engine.configPath(), xml));
    QSignalSpy changed(&engine, &WarningEngine::rulesChanged);
    QString error;
    QVERIFY(!engine.load(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(engine.rules().size(), 1);
    QCOMPARE(engine.rules().first().id, previousId);
    QCOMPARE(engine.rules().first().name, QStringLiteral("alt"));
    QCOMPARE(changed.count(), 0);
    QVERIFY(engine.dirty());
    QCOMPARE(readFile(engine.configPath()), xml);
}

void WarningEngineTest::configurationBoundsAndIdentities()
{
    WarningEngine engine({});
    CustomWarning rule = spoken();
    rule.id = 500;
    QVERIFY(engine.setRules({spoken(), rule}));
    QCOMPARE(engine.rules().first().id, quint64(501));
    QCOMPARE(engine.rules().last().id, quint64(500));
    QVERIFY(!engine.setRules({rule, rule}));
    rule.id = std::numeric_limits<quint64>::max();
    QVERIFY(!engine.setRules({rule}));
    QVERIFY(engine.setRules({spoken()}));
    QCOMPARE(engine.rules().first().id, quint64(502));
    const auto reject = [&](const CustomWarning &bad) {
        QString error;
        const bool accepted = engine.setRules({bad}, &error);
        return !accepted && !error.isEmpty() && engine.rules().first().id == 502;
    };
    rule = spoken(); rule.threshold = std::numeric_limits<double>::infinity(); QVERIFY(reject(rule));
    rule = spoken(); rule.condition = static_cast<CustomWarning::Conditional>(55); QVERIFY(reject(rule));
    rule = spoken(); rule.type = static_cast<CustomWarning::WarningType>(55); QVERIFY(reject(rule));
    rule = spoken(); rule.repeatSeconds = -1; QVERIFY(reject(rule));
    rule = spoken(); rule.repeatSeconds = 86401; QVERIFY(reject(rule));
    rule = spoken(); rule.name = QString(257, 'x'); QVERIFY(reject(rule));
    rule = spoken(); rule.text = QString(4097, 'x'); QVERIFY(reject(rule));
    rule = spoken(); rule.text = QChar(1); QVERIFY(reject(rule));
    rule = spoken(); rule.name = QChar(0xd800); QVERIFY(reject(rule));
    rule = spoken(); rule.child = {spoken(), spoken()}; QVERIFY(reject(rule));
    rule = spoken();
    for (int i = 1; i < 16; ++i) { CustomWarning next = spoken(); next.child = {rule}; rule = next; }
    QVERIFY(engine.setRules({rule}));
    CustomWarning tooDeep = spoken(); tooDeep.child = {rule};
    QVERIFY(!engine.setRules({tooDeep}));
    QVERIFY(engine.setRules(QVector<CustomWarning>(512, spoken())));
    QVERIFY(!engine.setRules(QVector<CustomWarning>(513, spoken())));
}

void WarningEngineTest::persistenceFailureAndMissingFile()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("warnings.xml");
    WarningEngine engine(path);
    QVERIFY(engine.setRules({spoken()}));
    QVERIFY(engine.load()); // Missing path is a successful empty, clean config.
    QVERIFY(engine.rules().isEmpty());
    QVERIFY(!engine.dirty());
    QVERIFY(engine.setRules({spoken()}));
    QVERIFY(engine.save());
    QVERIFY(!engine.dirty());
    const QByteArray before = readFile(path);
    // This is valid bounded in-memory configuration, but XML escaping exceeds
    // the publication cap. Serialization failure must preserve the old file.
    CustomWarning large = spoken();
    large.text = QString(4096, '&');
    QVERIFY(engine.setRules(QVector<CustomWarning>(512, large)));
    QString error;
    QVERIFY(!engine.save(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(engine.dirty());
    QCOMPARE(readFile(path), before);
    WarningEngine badDirectory(dir.filePath("missing/warnings.xml"));
    QVERIFY(badDirectory.setRules({spoken()}));
    QVERIFY(!badDirectory.save(&error));
    QVERIFY(badDirectory.dirty());
}

void WarningEngineTest::reentrantProviderAndClock()
{
    WarningEngine *engine = nullptr;
    bool reset = true;
    engine = new WarningEngine({}, [&] {
        if (reset) engine->resetEpoch();
        return WarningEngine::Values{{"alt", 20}};
    });
    QVERIFY(engine->setRules({spoken()}));
    QSignalSpy warnings(engine, &WarningEngine::warningMessage);
    engine->tick();
    QCOMPARE(warnings.count(), 0);
    reset = false;
    engine->tick();
    QCOMPARE(warnings.count(), 1); // Aborted source callback released the tick guard.
    delete engine;

    engine = new WarningEngine({}, [&] {
        engine->setRules({});
        return WarningEngine::Values{{"alt", 20}};
    });
    QVERIFY(engine->setRules({spoken()}));
    QSignalSpy edited(engine, &WarningEngine::warningMessage);
    engine->tick();
    QCOMPARE(edited.count(), 0);
    QVERIFY(engine->rules().isEmpty());
    delete engine;

    engine = new WarningEngine({}, [&] {
        delete engine;
        return WarningEngine::Values{{"alt", 20}};
    });
    QVERIFY(engine->setRules({spoken()}));
    QPointer<WarningEngine> guard(engine);
    engine->tick();
    QVERIFY(!guard);

    engine = new WarningEngine({}, [] { return WarningEngine::Values{{"alt", 20}}; }, [&] {
        delete engine;
        return qint64(0);
    });
    QVERIFY(engine->setRules({spoken()}));
    guard = engine;
    engine->tick();
    QVERIFY(!guard);
}

void WarningEngineTest::reentrantOutputsAndEdits()
{
    const auto provider = [] { return WarningEngine::Values{{"alt", 20}}; };
    WarningEngine engine({}, provider);
    QVERIFY(engine.setRules({spoken(), spoken()}));
    QSignalSpy warnings(&engine, &WarningEngine::warningMessage);
    const auto connection = connect(&engine, &WarningEngine::warningMessage, &engine, [&] { engine.resetEpoch(); });
    engine.tick();
    QCOMPARE(warnings.count(), 1); // No second stale output after the epoch changes.
    disconnect(connection);
    const auto editConnection = connect(&engine, &WarningEngine::warningMessage, &engine, [&] { engine.setRules({}); });
    engine.tick();
    QCOMPARE(warnings.count(), 2);
    QVERIFY(engine.rules().isEmpty());
    disconnect(editConnection);

    QVERIFY(engine.setRules({colored("alt", "Red"), spoken()}));
    bool clearOnColor = true;
    const auto colorConnection = connect(&engine, &WarningEngine::colorsChanged, &engine, [&] {
        if (clearOnColor) { clearOnColor = false; engine.setRules({}); }
    });
    engine.tick();
    QCOMPARE(warnings.count(), 2);
    QVERIFY(engine.colors().isEmpty());
    disconnect(colorConnection);

    auto *deleted = new WarningEngine({}, provider);
    QVERIFY(deleted->setRules({spoken(), spoken()}));
    QPointer<WarningEngine> guard(deleted);
    connect(deleted, &WarningEngine::warningMessage, this, [deleted] { delete deleted; });
    deleted->tick();
    QVERIFY(!guard);

    deleted = new WarningEngine({}, provider);
    QVERIFY(deleted->setRules({colored("alt", "Red"), spoken()}));
    guard = deleted;
    connect(deleted, &WarningEngine::colorsChanged, this, [deleted] { delete deleted; });
    deleted->tick();
    QVERIFY(!guard);

    deleted = new WarningEngine({});
    guard = deleted;
    connect(deleted, &WarningEngine::rulesChanged, this, [deleted] { delete deleted; });
    QVERIFY(deleted->setRules({spoken()}));
    QVERIFY(!guard);

    QVERIFY(engine.setRules({colored("alt", "Red")}));
    engine.tick();
    QSignalSpy changed(&engine, &WarningEngine::rulesChanged);
    bool replace = true;
    connect(&engine, &WarningEngine::colorsChanged, &engine, [&] {
        if (replace) { replace = false; engine.setRules({spoken("inner")}); }
    });
    QVERIFY(engine.setRules({spoken("outer")}));
    QCOMPARE(engine.rules().first().name, QStringLiteral("inner"));
    QCOMPARE(changed.count(), 1); // Outer mutation cannot emit a stale rulesChanged.
}

void WarningEngineTest::timerStartsOnlyOnRequest()
{
    WarningEngine engine({});
    auto *timer = engine.findChild<QTimer *>();
    QVERIFY(timer);
    QCOMPARE(timer->interval(), 250);
    QVERIFY(!timer->isActive());
    engine.setRunning(true);
    QVERIFY(timer->isActive());
    engine.setRunning(false);
    QVERIFY(!timer->isActive());
}

void WarningEngineTest::editsPreserveOtherCooldownsAndColors()
{
    qint64 now = 0;
    WarningEngine engine("", [] { return WarningEngine::Values{{"alt", 20}}; }, [&] { return now; });
    QVERIFY(engine.setRules({spoken("alt", CustomWarning::GT, 10), colored("alt", "Yellow")}));
    QSignalSpy messages(&engine, &WarningEngine::warningMessage);
    QSignalSpy colors(&engine, &WarningEngine::colorsChanged);
    engine.tick();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(engine.colors().value("alt"), QString("Yellow"));
    auto rules = engine.rules();
    rules[1].text = "Editing unrelated color metadata";
    now = 100;
    QVERIFY(engine.setRules(rules));
    QCOMPARE(engine.colors().value("alt"), QString("Yellow"));
    engine.tick();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(colors.size(), 1);
    now = 10000;
    engine.tick();
    QCOMPARE(messages.size(), 2);
    rules.removeLast();
    QVERIFY(engine.setRules(rules));
    QVERIFY(engine.colors().isEmpty());
}

QTEST_GUILESS_MAIN(WarningEngineTest)
#include "test_warningengine.moc"
