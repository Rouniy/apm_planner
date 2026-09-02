#include <QtTest>

#include "ui/configuration/ConfigParachuteView.h"

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTimer>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:CHUTE_ENABLED"
               humanName="Parachute enabled"
               documentation="Enables the parachute release system.">
          <values>
            <value code="0">Disabled</value>
            <value code="1">Enabled</value>
          </values>
        </param>
        <param name="ArduCopter:CHUTE_TYPE"
               humanName="Release type"
               documentation="Select relay or servo release." />
        <param name="ArduCopter:CHUTE_SERVO_ON"
               humanName="Servo ON PWM"
               documentation="Release PWM.">
          <field name="Units">PWM</field>
        </param>
        <param name="ArduCopter:CHUTE_SERVO_OFF"
               humanName="Servo OFF PWM"
               documentation="Idle PWM.">
          <field name="Units">PWM</field>
        </param>
        <param name="ArduCopter:CHUTE_ALT_MIN"
               humanName="Minimum altitude">
          <field name="Units">m</field>
        </param>
        <param name="ArduCopter:CHUTE_DELAY_MS"
               humanName="Release delay">
          <field name="Units">ms</field>
        </param>
        <param name="ArduCopter:CHUTE_CRT_SINK"
               humanName="Critical sink speed">
          <field name="Units">m/s</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> parameterFixture(
    int component = 1)
{
    return {
        {component, QStringLiteral("CHUTE_ENABLED"), 1},
        {component, QStringLiteral("CHUTE_TYPE"), 10},
        {component, QStringLiteral("CHUTE_SERVO_ON"), 1300},
        {component, QStringLiteral("CHUTE_SERVO_OFF"), 1100},
        {component, QStringLiteral("CHUTE_ALT_MIN"), 10},
        {component, QStringLiteral("CHUTE_DELAY_MS"), 500},
        {component, QStringLiteral("CHUTE_CRT_SINK"), 0},
        {component, QStringLiteral("SERVO9_FUNCTION"), 27},
        {component, QStringLiteral("RC9_FUNCTION"), 0},
        {component, QStringLiteral("SERVO10_FUNCTION"), 0},
        {component, QStringLiteral("RC10_FUNCTION"), 27},
        {component, QStringLiteral("RC11_FUNCTION"), 0},
        {component, QStringLiteral("SERVO12_FUNCTION"), 0},
        {component, QStringLiteral("SERVO13_FUNCTION"), 0},
        {component, QStringLiteral("SERVO14_FUNCTION"), 0}
    };
}

ParamField fieldNamed(const ConfigParachuteViewModel &model,
                      const QString &name)
{
    for (const ParamField &field : model.Fields()) {
        if (field.name == name) {
            return field;
        }
    }
    return {};
}
} // namespace

class ConfigParachuteViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactFieldsRangesOptionsAndHydration();
    void servoResolutionPrefersServoAndFallsBackToRc();
    void assignmentDisablesOldBeforeExactSelectedEcho();
    void staleSnapshotDisconnectAndWriteFailureStopTheSequence();
    void normalFieldWritesRequireCurrentComponentExactEcho();
    void widgetMatchesMissionPlannerContractAndRendersOpaque();
};

void ConfigParachuteViewTest::exactFieldsRangesOptionsAndHydration()
{
    ConfigParachuteViewModel model;
    QSignalSpy writes(&model,
                     &ConfigParachuteViewModel::writeRequested);
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(parameterFixture());

    QCOMPARE(writes.count(), 0);
    QCOMPARE(ConfigParachuteViewModel::WriteTimeoutMs(), 5000);
    QCOMPARE(ConfigParachuteViewModel::FieldNames(), QStringList({
        QStringLiteral("CHUTE_ENABLED"),
        QStringLiteral("CHUTE_TYPE"),
        QStringLiteral("CHUTE_SERVO_ON"),
        QStringLiteral("CHUTE_SERVO_OFF"),
        QStringLiteral("CHUTE_ALT_MIN"),
        QStringLiteral("CHUTE_DELAY_MS"),
        QStringLiteral("CHUTE_CRT_SINK")
    }));
    QCOMPARE(model.Fields().size(), 7);
    QCOMPARE(model.ServoOptions(), QStringList({
        QStringLiteral("RC9"), QStringLiteral("RC10"),
        QStringLiteral("RC11"), QStringLiteral("RC12"),
        QStringLiteral("RC13"), QStringLiteral("RC14")
    }));

    const ParamField enabled = fieldNamed(model, QStringLiteral("CHUTE_ENABLED"));
    QCOMPARE(enabled.label, QStringLiteral("Parachute enabled"));
    QCOMPARE(enabled.description,
             QStringLiteral("Enables the parachute release system."));
    QVERIFY(enabled.editorKind == ParamField::EditorKind::Combo);
    QCOMPARE(enabled.options.size(), 2);
    QCOMPARE(enabled.options.at(0).text, QStringLiteral("Disabled"));
    QCOMPARE(enabled.options.at(1).value.toInt(), 1);

    const ParamField type = fieldNamed(model, QStringLiteral("CHUTE_TYPE"));
    QVERIFY(type.editorKind == ParamField::EditorKind::Combo);
    QCOMPARE(type.options.size(), 5);
    QCOMPARE(type.options.at(0).text, QStringLiteral("First Relay"));
    QCOMPARE(type.options.at(1).text, QStringLiteral("Second Relay"));
    QCOMPARE(type.options.at(2).text, QStringLiteral("Third Relay"));
    QCOMPARE(type.options.at(3).text, QStringLiteral("Fourth Relay"));
    QCOMPARE(type.options.at(4).text, QStringLiteral("Servo"));
    QCOMPARE(type.options.at(4).value.toInt(), 10);

    const struct {
        const char *name;
        double minimum;
        double maximum;
    } ranges[] = {
        {"CHUTE_SERVO_ON", 1000.0, 2000.0},
        {"CHUTE_SERVO_OFF", 1000.0, 2000.0},
        {"CHUTE_ALT_MIN", 0.0, 32000.0},
        {"CHUTE_DELAY_MS", 0.0, 5000.0},
        {"CHUTE_CRT_SINK", 0.0, 15.0}
    };
    for (const auto &expected : ranges) {
        const ParamField field = fieldNamed(
            model, QString::fromLatin1(expected.name));
        QVERIFY(field.editorKind == ParamField::EditorKind::Numeric);
        QVERIFY(field.hasRange);
        QVERIFY(field.enforceRange);
        QCOMPARE(field.minimum, expected.minimum);
        QCOMPARE(field.maximum, expected.maximum);
        QCOMPARE(field.increment, 1.0);
        QVERIFY(!field.readOnly);
    }
    QCOMPARE(model.SelectedServo(), QStringLiteral("RC9"));
    QCOMPARE(fieldNamed(model, QStringLiteral("CHUTE_ENABLED")).status,
             QString());

    model.setConnected(true);
    QVERIFY(model.Refresh());
    QCOMPARE(model.Status(), QStringLiteral("Refreshing parameters…"));
    model.setParameterSnapshot(parameterFixture());
    QCOMPARE(model.Status(), QString());

    QList<ConfigFriendlyParameterValue> missing = parameterFixture();
    for (auto iterator = missing.begin(); iterator != missing.end();) {
        if (iterator->name == QLatin1String("CHUTE_CRT_SINK")) {
            iterator = missing.erase(iterator);
        } else {
            ++iterator;
        }
    }
    model.setParameterSnapshot(missing);
    QCOMPARE(fieldNamed(model, QStringLiteral("CHUTE_CRT_SINK")).status,
             QStringLiteral("n/a"));
}

void ConfigParachuteViewTest::
    servoResolutionPrefersServoAndFallsBackToRc()
{
    ConfigParachuteViewModel model;
    model.setParameterSnapshot(parameterFixture());
    QCOMPARE(model.DetectServo(), QStringLiteral("RC9"));

    QList<ConfigFriendlyParameterValue> fallback = parameterFixture();
    for (auto iterator = fallback.begin(); iterator != fallback.end();) {
        if (iterator->name == QLatin1String("SERVO9_FUNCTION")) {
            iterator = fallback.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (ConfigFriendlyParameterValue &parameter : fallback) {
        if (parameter.name == QLatin1String("RC9_FUNCTION")) {
            parameter.value = 27;
        }
        if (parameter.name == QLatin1String("RC10_FUNCTION")) {
            parameter.value = 0;
        }
    }
    model.setParameterSnapshot(fallback);
    QCOMPARE(model.DetectServo(), QStringLiteral("RC9"));
}

void ConfigParachuteViewTest::
    assignmentDisablesOldBeforeExactSelectedEcho()
{
    ConfigParachuteViewModel model;
    model.setParameterSnapshot(parameterFixture());
    model.setConnected(true);
    QSignalSpy writes(&model,
                     &ConfigParachuteViewModel::writeRequested);

    QCOMPARE(model.EnsureDisabled(QStringLiteral("RC10")),
             QStringList({QStringLiteral("SERVO9_FUNCTION")}));
    QVERIFY(model.AssignServo(QStringLiteral("RC10")));
    QVERIFY(model.Busy());
    QCOMPARE(model.SelectedServo(), QStringLiteral("RC10"));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(0).toInt(), 1);
    QCOMPARE(writes.at(0).at(1).toString(),
             QStringLiteral("SERVO9_FUNCTION"));
    QCOMPARE(writes.at(0).at(2).toInt(), 0);

    model.parameterChanged(2, QStringLiteral("SERVO9_FUNCTION"), 0);
    QCOMPARE(writes.count(), 1);
    QVERIFY(model.Busy());
    model.parameterChanged(1, QStringLiteral("SERVO9_FUNCTION"), 0);
    QCOMPARE(writes.count(), 2);
    QCOMPARE(writes.at(1).at(1).toString(),
             QStringLiteral("SERVO10_FUNCTION"));
    QCOMPARE(writes.at(1).at(2).toInt(), 27);
    QVERIFY(model.Busy());

    model.parameterChanged(1, QStringLiteral("SERVO10_FUNCTION"), 27.0);
    QVERIFY(!model.Busy());
    QCOMPARE(model.SelectedServo(), QStringLiteral("RC10"));
    QCOMPARE(model.ServoStatus(), QStringLiteral("✓"));
    QCOMPARE(writes.count(), 2);
}

void ConfigParachuteViewTest::
    staleSnapshotDisconnectAndWriteFailureStopTheSequence()
{
    ConfigParachuteViewModel model;
    model.setParameterSnapshot(parameterFixture());
    model.setConnected(true);
    QSignalSpy writes(&model,
                     &ConfigParachuteViewModel::writeRequested);

    QVERIFY(model.AssignServo(QStringLiteral("RC10")));
    QCOMPARE(writes.count(), 1);
    QList<ConfigFriendlyParameterValue> replacement = parameterFixture();
    for (ConfigFriendlyParameterValue &parameter : replacement) {
        if (parameter.name == QLatin1String("SERVO9_FUNCTION")) {
            parameter.value = 0;
        }
        if (parameter.name == QLatin1String("SERVO10_FUNCTION")) {
            parameter.value = 27;
        }
    }
    model.setParameterSnapshot(replacement);
    QVERIFY(!model.Busy());
    QCOMPARE(model.SelectedServo(), QStringLiteral("RC10"));
    model.parameterChanged(1, QStringLiteral("SERVO9_FUNCTION"), 0);
    QCOMPARE(writes.count(), 1);

    QVERIFY(model.AssignServo(QStringLiteral("RC11")));
    QCOMPARE(writes.count(), 2);
    model.parameterWriteFailed(
        2, writes.last().at(1).toString(), QStringLiteral("stale UAS"));
    QVERIFY(model.Busy());
    model.parameterWriteFailed(
        1, writes.last().at(1).toString(), QStringLiteral("link failed"));
    QVERIFY(!model.Busy());
    QCOMPARE(model.ServoStatus(), QStringLiteral("link failed"));

    QVERIFY(model.AssignServo(QStringLiteral("RC11")));
    QCOMPARE(writes.count(), 3);
    model.setConnected(false);
    QVERIFY(!model.Busy());
    QVERIFY(!model.SnapshotReady());
    QCOMPARE(model.SelectedServo(), QString());
    QCOMPARE(model.ServoStatus(), QStringLiteral("offline"));
    model.parameterChanged(
        1, writes.last().at(1).toString(), writes.last().at(2));
    QCOMPARE(writes.count(), 3);
}

void ConfigParachuteViewTest::
    normalFieldWritesRequireCurrentComponentExactEcho()
{
    ConfigParachuteViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(parameterFixture());
    model.setConnected(true);
    QSignalSpy writes(&model,
                     &ConfigParachuteViewModel::writeRequested);

    QVERIFY(!model.setFieldValue(QStringLiteral("CHUTE_SERVO_ON"), 999));
    QVERIFY(!model.setFieldValue(QStringLiteral("CHUTE_DELAY_MS"), 5001));
    QVERIFY(!model.setFieldValue(QStringLiteral("CHUTE_TYPE"), 9));
    QCOMPARE(writes.count(), 0);

    QVERIFY(model.setFieldValue(QStringLiteral("CHUTE_SERVO_ON"), 1400));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(1).toString(),
             QStringLiteral("CHUTE_SERVO_ON"));
    model.parameterChanged(2, QStringLiteral("CHUTE_SERVO_ON"), 1400);
    QVERIFY(model.Busy());
    model.parameterChanged(1, QStringLiteral("CHUTE_SERVO_ON"), 1350);
    QVERIFY(!model.Busy());
    QCOMPARE(fieldNamed(model, QStringLiteral("CHUTE_SERVO_ON"))
                 .value.toInt(), 1350);
    QCOMPARE(model.Status(), QStringLiteral("write mismatch"));
    QCOMPARE(fieldNamed(model, QStringLiteral("CHUTE_SERVO_ON")).status,
             QStringLiteral("write mismatch"));

    QVERIFY(model.setFieldValue(QStringLiteral("CHUTE_SERVO_ON"), 1400));
    model.parameterChanged(1, QStringLiteral("CHUTE_SERVO_ON"), 1400);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QString());
    QCOMPARE(fieldNamed(model, QStringLiteral("CHUTE_SERVO_ON")).status,
             QStringLiteral("✓"));
    QCOMPARE(writes.count(), 2);
}

void ConfigParachuteViewTest::
    widgetMatchesMissionPlannerContractAndRendersOpaque()
{
    ConfigParachuteView view(catalogFixture());
    view.resize(800, 560);
    view.setParameterSnapshot(parameterFixture());
    view.setConnected(true);

    QCOMPARE(view.objectName(), QStringLiteral("ConfigParachuteView"));
    QCOMPARE(view.sizeHint(), QSize(800, 560));
    QCOMPARE(ConfigParachuteView::ArmedRefreshWarningTitle(),
             QStringLiteral("Refresh Params"));
    QCOMPARE(ConfigParachuteView::ArmedRefreshWarningText(),
             QStringLiteral(
                 "Update Params\nDON'T DO THIS IF YOU ARE IN THE AIR\n"));
    QVERIFY(view.testAttribute(Qt::WA_OpaquePaintEvent));
    QVERIFY(view.autoFillBackground());
    QVERIFY(view.findChild<QScrollArea *>(
        QStringLiteral("parachuteScroll")));

    QLabel *title = view.findChild<QLabel *>(
        QStringLiteral("parachuteTitle"));
    QLabel *intro = view.findChild<QLabel *>(
        QStringLiteral("parachuteIntro"));
    QPushButton *refresh = view.findChild<QPushButton *>(
        QStringLiteral("parachuteRefreshButton"));
    QComboBox *servo = view.findChild<QComboBox *>(
        QStringLiteral("ServoOptions"));
    QLabel *servoStatus = view.findChild<QLabel *>(
        QStringLiteral("ServoStatus"));
    QVERIFY(title);
    QVERIFY(intro);
    QVERIFY(refresh);
    QVERIFY(servo);
    QVERIFY(servoStatus);
    QCOMPARE(title->text(), QStringLiteral("Parachute"));
    QCOMPARE(intro->text(), QStringLiteral(
        "Configure parachute release. Ensure props are removed before testing."));
    QCOMPARE(refresh->text(), QStringLiteral("Refresh Params"));
    QCOMPARE(servo->count(), 6);
    QCOMPARE(servo->itemText(0), QStringLiteral("RC9"));
    QCOMPARE(servo->itemText(5), QStringLiteral("RC14"));
    QCOMPARE(servo->currentText(), QStringLiteral("RC9"));
    QVERIFY(servo->isEnabled());

    QSignalSpy refreshRequests(
        &view, &ConfigParachuteView::refreshRequested);
    view.setArmed(true);
    QTimer::singleShot(0, []() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                message->done(QMessageBox::No);
            }
        }
    });
    refresh->click();
    QCOMPARE(refreshRequests.count(), 0);
    view.setArmed(false);
    refresh->click();
    QCOMPARE(refreshRequests.count(), 1);
    view.refreshCanceled();

    for (const QString &name : ConfigParachuteViewModel::FieldNames()) {
        QWidget *row = view.findChild<QWidget *>(
            QStringLiteral("parachuteFieldRow_%1").arg(name));
        QLabel *label = view.findChild<QLabel *>(
            QStringLiteral("parachuteFieldLabel_%1").arg(name));
        QLabel *status = view.findChild<QLabel *>(
            QStringLiteral("parachuteFieldStatus_%1").arg(name));
        QVERIFY(row);
        QVERIFY(label);
        QVERIFY(status);
        QCOMPARE(row->property("parameterName").toString(), name);
        if (name == QLatin1String("CHUTE_ENABLED")
            || name == QLatin1String("CHUTE_TYPE")) {
            QVERIFY(view.findChild<QComboBox *>(
                QStringLiteral("parachuteFieldEditor_%1").arg(name)));
        } else {
            QDoubleSpinBox *editor = view.findChild<QDoubleSpinBox *>(
                QStringLiteral("parachuteFieldEditor_%1").arg(name));
            QVERIFY(editor);
            QVERIFY(editor->isEnabled());
        }
    }

    QImage rendered(view.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    view.render(&rendered);
    for (int y = 0; y < rendered.height(); y += 17) {
        for (int x = 0; x < rendered.width(); x += 17) {
            QCOMPARE(qAlpha(rendered.pixel(x, y)), 255);
        }
    }

    view.setConnected(false);
    QVERIFY(!servo->isEnabled());
    QCOMPARE(servoStatus->text(), QStringLiteral("offline"));
}

QTEST_MAIN(ConfigParachuteViewTest)
#include "test_configparachuteview.moc"
