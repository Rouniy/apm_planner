#include <QtTest>

#include "ui/configuration/ConfigESCCalibrationView.h"

#include <QBuffer>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:MOT_PWM_TYPE"
               humanName="Output PWM type"
               documentation="Selects the motor output PWM protocol.">
          <values>
            <value code="0">Normal</value>
            <value code="1">OneShot</value>
            <value code="2">OneShot125</value>
            <value code="3">Brushed</value>
            <value code="4">DShot150</value>
            <value code="5">DShot300</value>
            <value code="6">DShot600</value>
            <value code="7">DShot1200</value>
            <value code="8">PWMRange</value>
          </values>
          <field name="RebootRequired">True</field>
        </param>
        <param name="ArduCopter:MOT_PWM_MIN"
               humanName="PWM output minimum"
               documentation="Minimum motor PWM output.">
          <field name="Units">PWM</field>
          <field name="Range">0 2000</field>
          <field name="Increment">1</field>
        </param>
        <param name="ArduCopter:MOT_PWM_MAX"
               humanName="PWM output maximum"
               documentation="Maximum motor PWM output.">
          <field name="Units">PWM</field>
          <field name="Range">0 2000</field>
          <field name="Increment">1</field>
        </param>
        <param name="ArduCopter:MOT_SPIN_ARM"
               humanName="Motor Spin armed"
               documentation="Motor idle point while armed.">
          <values>
            <value code="0.0">Low</value>
            <value code="0.1">Default</value>
            <value code="0.2">High</value>
          </values>
        </param>
        <param name="ArduCopter:MOT_SPIN_MIN"
               humanName="Motor Spin minimum"
               documentation="Minimum motor speed in flight." />
        <param name="ArduCopter:MOT_SPIN_MAX"
               humanName="Motor Spin maximum"
               documentation="Maximum motor speed in flight." />
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> parameterFixture(
    bool includeCalibration = true, bool includeSpinMaximum = true)
{
    QList<ConfigFriendlyParameterValue> parameters = {
        {1, QStringLiteral("MOT_PWM_TYPE"), 0},
        {1, QStringLiteral("MOT_PWM_MIN"), 1000},
        {1, QStringLiteral("MOT_PWM_MAX"), 2000},
        {1, QStringLiteral("MOT_SPIN_ARM"), 0.10f},
        {1, QStringLiteral("MOT_SPIN_MIN"), 0.15f}
    };
    if (includeSpinMaximum) {
        parameters.append(
            {1, QStringLiteral("MOT_SPIN_MAX"), 0.95f});
    }
    if (includeCalibration) {
        parameters.append(
            {1, QStringLiteral("ESC_CALIBRATION"), 0});
    }
    return parameters;
}

ParamField fieldNamed(const ConfigESCCalibrationViewModel &model,
                      const QString &name)
{
    const QString normalized = name.trimmed().toUpper();
    for (const ParamField &field : model.Fields()) {
        if (field.name == normalized) {
            return field;
        }
    }
    return {};
}

void prepareCalibrationModel(ConfigESCCalibrationViewModel *model)
{
    model->setCatalog(catalogFixture());
    model->setParameterSnapshot(parameterFixture());
}
} // namespace

class ConfigESCCalibrationViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactFieldsMetadataAndHydrationNeverWrite();
    void normalEditEchoAndFailureAreDeterministic();
    void calibrationSafetyAndAcknowledgementContract();
    void widgetMatchesMissionPlannerContract();
    void confirmationTextStatesFullSafetyContract();
};

void ConfigESCCalibrationViewTest::
    exactFieldsMetadataAndHydrationNeverWrite()
{
    ConfigESCCalibrationViewModel model;
    QSignalSpy writes(&model,
                     &ConfigESCCalibrationViewModel::writeRequested);

    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(parameterFixture());
    QCOMPARE(writes.count(), 0);
    QCOMPARE(model.ComponentId(), 1);

    const QStringList expectedNames = {
        QStringLiteral("MOT_PWM_TYPE"),
        QStringLiteral("MOT_PWM_MIN"),
        QStringLiteral("MOT_PWM_MAX"),
        QStringLiteral("MOT_SPIN_ARM"),
        QStringLiteral("MOT_SPIN_MIN"),
        QStringLiteral("MOT_SPIN_MAX")
    };
    QCOMPARE(ConfigESCCalibrationViewModel::FieldNames(), expectedNames);
    const QList<ParamField> fields = model.Fields();
    QCOMPARE(fields.size(), expectedNames.size());
    for (int index = 0; index < fields.size(); ++index) {
        QCOMPARE(fields.at(index).name, expectedNames.at(index));
        QCOMPARE(fields.at(index).componentId, 1);
        QVERIFY(!fields.at(index).readOnly);
        if (index == 0) {
            QVERIFY(fields.at(index).editorKind
                    == ParamField::EditorKind::Combo);
        } else {
            QVERIFY(fields.at(index).editorKind
                    == ParamField::EditorKind::Numeric);
        }
    }

    const ParamField pwmType = fieldNamed(model, QStringLiteral("MOT_PWM_TYPE"));
    QCOMPARE(pwmType.label, QStringLiteral("Output PWM type"));
    QCOMPARE(pwmType.description,
             QStringLiteral("Selects the motor output PWM protocol."));
    QCOMPARE(pwmType.options.size(), 9);
    QCOMPARE(pwmType.options.first().value.toInt(), 0);
    QCOMPARE(pwmType.options.first().text, QStringLiteral("Normal"));
    QCOMPARE(pwmType.options.last().value.toInt(), 8);
    QCOMPARE(pwmType.options.last().text, QStringLiteral("PWMRange"));

    const ParamField pwmMin = fieldNamed(model, QStringLiteral("MOT_PWM_MIN"));
    QCOMPARE(pwmMin.label, QStringLiteral("PWM output minimum"));
    QCOMPARE(pwmMin.units, QStringLiteral("PWM"));
    QCOMPARE(pwmMin.value.toInt(), 1000);
    QVERIFY(pwmMin.hasRange);
    QVERIFY(pwmMin.enforceRange);
    QCOMPARE(pwmMin.minimum, 0.0);
    QCOMPARE(pwmMin.maximum, 2000.0);
    QCOMPARE(pwmMin.increment, 1.0);

    const ParamField spinArm = fieldNamed(
        model, QStringLiteral("MOT_SPIN_ARM"));
    QCOMPARE(spinArm.label, QStringLiteral("Motor Spin armed"));
    QVERIFY(spinArm.editorKind == ParamField::EditorKind::Numeric);
    QVERIFY(spinArm.options.isEmpty());
    QCOMPARE(spinArm.minimum, 0.0);
    QCOMPARE(spinArm.maximum, 1.0);
    QCOMPARE(spinArm.increment, 0.01);
    QCOMPARE(spinArm.value.toFloat(), 0.10f);

    model.parameterChanged(
        1, QStringLiteral("MOT_SPIN_MIN"), 0.18f);
    QCOMPARE(writes.count(), 0);
    QCOMPARE(fieldNamed(model, QStringLiteral("MOT_SPIN_MIN"))
                 .value.toFloat(),
             0.18f);

    model.setParameterSnapshot(parameterFixture(true, false));
    QCOMPARE(writes.count(), 0);
    const ParamField missing = fieldNamed(
        model, QStringLiteral("MOT_SPIN_MAX"));
    QVERIFY(missing.readOnly);
    QCOMPARE(missing.value.toDouble(), 0.0);
    QVERIFY(!fieldNamed(model, QStringLiteral("MOT_SPIN_MIN")).readOnly);
}

void ConfigESCCalibrationViewTest::
    normalEditEchoAndFailureAreDeterministic()
{
    ConfigESCCalibrationViewModel model;
    prepareCalibrationModel(&model);
    model.setConnected(true);
    QSignalSpy writes(&model,
                     &ConfigESCCalibrationViewModel::writeRequested);

    QVERIFY(!model.setFieldValue(QStringLiteral("MOT_PWM_MIN"), -1));
    QVERIFY(!model.setFieldValue(QStringLiteral("MOT_PWM_MAX"), 2001));
    QVERIFY(!model.setFieldValue(QStringLiteral("MOT_SPIN_MIN"), 1.01));
    QVERIFY(!model.setFieldValue(QStringLiteral("MOT_PWM_TYPE"), 99));
    QVERIFY(!model.setFieldValue(QStringLiteral("MOT_PWM_MIN"), 1000));
    QCOMPARE(writes.count(), 0);

    QVERIFY(model.setFieldValue(QStringLiteral("MOT_PWM_MIN"), 1100));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toInt(), 1);
    QCOMPARE(writes.first().at(1).toString(),
             QStringLiteral("MOT_PWM_MIN"));
    QCOMPARE(writes.first().at(2).toInt(), 1100);
    QVERIFY(model.HasPendingWrites());
    QCOMPARE(fieldNamed(model, QStringLiteral("MOT_PWM_MIN"))
                 .value.toInt(),
             1100);
    QVERIFY(!model.setFieldValue(QStringLiteral("MOT_PWM_MIN"), 1200));
    QVERIFY(!model.setFieldValue(QStringLiteral("MOT_PWM_MAX"), 1900));
    QCOMPARE(writes.count(), 1);

    model.parameterChanged(2, QStringLiteral("MOT_PWM_MIN"), 1100);
    QVERIFY(model.HasPendingWrites());
    model.parameterChanged(1, QStringLiteral("MOT_PWM_MIN"), 1100);
    QVERIFY(!model.HasPendingWrites());
    QCOMPARE(writes.count(), 1);
    QCOMPARE(fieldNamed(model, QStringLiteral("MOT_PWM_MIN"))
                 .value.toInt(),
             1100);

    QVERIFY(model.setFieldValue(QStringLiteral("MOT_PWM_MAX"), 1900));
    QCOMPARE(writes.count(), 2);
    model.parameterWriteFailed(
        2, QStringLiteral("MOT_PWM_MAX"), QStringLiteral("wrong vehicle"));
    QVERIFY(model.HasPendingWrites());
    model.parameterWriteFailed(
        1, QStringLiteral("MOT_PWM_MAX"), QStringLiteral("link lost"));
    QVERIFY(!model.HasPendingWrites());
    QCOMPARE(fieldNamed(model, QStringLiteral("MOT_PWM_MAX"))
                 .value.toInt(),
             2000);
    QVERIFY(model.Status().contains(QStringLiteral("link lost")));

    model.parameterChanged(1, QStringLiteral("MOT_PWM_MAX"), 1950);
    QCOMPARE(writes.count(), 2);
    QCOMPARE(fieldNamed(model, QStringLiteral("MOT_PWM_MAX"))
                 .value.toInt(),
             1950);

    model.setArmed(true);
    QVERIFY(!model.setFieldValue(QStringLiteral("MOT_PWM_MIN"), 1200));
    QCOMPARE(writes.count(), 2);
}

void ConfigESCCalibrationViewTest::
    calibrationSafetyAndAcknowledgementContract()
{
    {
        ConfigESCCalibrationViewModel model;
        prepareCalibrationModel(&model);
        QSignalSpy writes(&model,
                         &ConfigESCCalibrationViewModel::writeRequested);
        QVERIFY(!model.CalibrateEsc(true));
        QCOMPARE(writes.count(), 0);
        QCOMPARE(model.Status(),
                 QStringLiteral("Connect to a vehicle first."));

        model.setConnected(true);
        QVERIFY(!model.CalibrateEsc(false));
        QCOMPARE(writes.count(), 0);
        model.setArmed(true);
        QVERIFY(!model.CalibrateEsc(true));
        QCOMPARE(writes.count(), 0);
        QCOMPARE(model.Status(),
                 QStringLiteral("Disarm the vehicle before ESC calibration."));
    }

    {
        ConfigESCCalibrationViewModel model;
        model.setCatalog(catalogFixture());
        model.setParameterSnapshot(parameterFixture(false));
        model.setConnected(true);
        QSignalSpy writes(&model,
                         &ConfigESCCalibrationViewModel::writeRequested);
        QVERIFY(!model.CanCalibrate());
        QVERIFY(!model.CalibrateEsc(true));
        QCOMPARE(writes.count(), 0);
        QVERIFY(model.Status().contains(QStringLiteral("AC 3.3+")));
    }

    {
        ConfigESCCalibrationViewModel model;
        prepareCalibrationModel(&model);
        model.setConnected(true);
        QSignalSpy writes(&model,
                         &ConfigESCCalibrationViewModel::writeRequested);

        QVERIFY(model.CanCalibrate());
        QVERIFY(model.setFieldValue(QStringLiteral("MOT_PWM_MIN"), 1100));
        QVERIFY(!model.CanCalibrate());
        QVERIFY(!model.CalibrateEsc(true));
        model.parameterChanged(1, QStringLiteral("MOT_PWM_MIN"), 1100);
        QVERIFY(model.CanCalibrate());
        QVERIFY(model.CalibrateEsc(true));
        QCOMPARE(writes.count(), 2);
        QCOMPARE(writes.at(1).at(0).toInt(), 1);
        QCOMPARE(writes.at(1).at(1).toString(),
                 QStringLiteral("ESC_CALIBRATION"));
        QCOMPARE(writes.at(1).at(2).toInt(), 3);
        QVERIFY(model.Busy());
        QVERIFY(model.HasPendingWrites());
        QVERIFY(!model.CalibrationComplete());
        QVERIFY(!model.CanCalibrate());
        QVERIFY(!model.CalibrateEsc(true));
        QCOMPARE(writes.count(), 2);

        model.parameterChanged(2, QStringLiteral("ESC_CALIBRATION"), 3);
        model.parameterChanged(1, QStringLiteral("MOT_PWM_MIN"), 1001);
        QVERIFY(model.Busy());
        QVERIFY(model.HasPendingWrites());
        QVERIFY(!model.CalibrationComplete());

        model.parameterChanged(1, QStringLiteral("ESC_CALIBRATION"), 3);
        QVERIFY(!model.Busy());
        QVERIFY(!model.HasPendingWrites());
        QVERIFY(model.CalibrationComplete());
        QCOMPARE(model.CalButtonText(), QStringLiteral("Done"));
        QVERIFY(model.Status().contains(
            QStringLiteral("power-cycle"), Qt::CaseInsensitive));
        QCOMPARE(writes.count(), 2);
    }

    {
        ConfigESCCalibrationViewModel model;
        prepareCalibrationModel(&model);
        model.setConnected(true);
        QVERIFY(model.CalibrateEsc(true));
        model.parameterChanged(1, QStringLiteral("ESC_CALIBRATION"), 2);
        QVERIFY(!model.Busy());
        QVERIFY(!model.HasPendingWrites());
        QVERIFY(!model.CalibrationComplete());
        QVERIFY(model.Status().contains(QStringLiteral("Set param error")));
        model.parameterChanged(1, QStringLiteral("ESC_CALIBRATION"), 3);
        QVERIFY(!model.CalibrationComplete());
        QCOMPARE(model.CalButtonText(), QStringLiteral("Calibrate ESCs"));
    }

    {
        ConfigESCCalibrationViewModel model;
        prepareCalibrationModel(&model);
        model.setConnected(true);
        QVERIFY(model.CalibrateEsc(true));
        model.parameterWriteFailed(
            1, QStringLiteral("ESC_CALIBRATION"),
            QStringLiteral("parameter unavailable"));
        QVERIFY(!model.Busy());
        QVERIFY(!model.HasPendingWrites());
        QVERIFY(!model.CalibrationComplete());
        QVERIFY(model.CanCalibrate());
    }

    {
        ConfigESCCalibrationViewModel model;
        prepareCalibrationModel(&model);
        model.setConnected(true);
        QVERIFY(model.CalibrateEsc(true));
        model.setConnected(false);
        QVERIFY(!model.Busy());
        QVERIFY(!model.HasPendingWrites());
        QVERIFY(!model.CalibrationComplete());
        QVERIFY(!model.CanCalibrate());

        model.parameterChanged(1, QStringLiteral("ESC_CALIBRATION"), 3);
        QVERIFY(!model.CalibrationComplete());
        QCOMPARE(model.CalButtonText(), QStringLiteral("Calibrate ESCs"));
    }
}

void ConfigESCCalibrationViewTest::widgetMatchesMissionPlannerContract()
{
    ConfigESCCalibrationView view(catalogFixture());
    QSignalSpy writes(&view, &ConfigESCCalibrationView::writeRequested);
    view.setParameterSnapshot(parameterFixture(true, false));
    view.setConnected(true);
    QCOMPARE(writes.count(), 0);

    QCOMPARE(view.objectName(), QStringLiteral("ConfigESCCalibrationView"));
    QCOMPARE(view.sizeHint(), QSize(820, 560));
    QVERIFY(view.findChild<QScrollArea *>(
        QStringLiteral("escCalibrationScroll")));
    QVERIFY(view.findChild<QWidget *>(
        QStringLiteral("escCalibrationContent")));
    QVERIFY(view.findChild<QWidget *>(
        QStringLiteral("escCalibrationActionRow")));
    QVERIFY(view.findChild<QWidget *>(
        QStringLiteral("escCalibrationInstructions")));
    QVERIFY(view.findChild<QWidget *>(
        QStringLiteral("escCalibrationFields")));
    QVERIFY(view.findChild<QWidget *>(
        QStringLiteral("escCalibrationBottomRow")));

    QLabel *title = view.findChild<QLabel *>(
        QStringLiteral("escCalibrationTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("ESC Calibration (AC3.3+)"));

    QPushButton *calibrate = view.findChild<QPushButton *>(
        QStringLiteral("escCalibrateButton"));
    QPushButton *refresh = view.findChild<QPushButton *>(
        QStringLiteral("escRefreshButton"));
    QLabel *status = view.findChild<QLabel *>(
        QStringLiteral("escCalibrationStatus"));
    QVERIFY(calibrate);
    QVERIFY(refresh);
    QVERIFY(status);
    QCOMPARE(calibrate->text(), QStringLiteral("Calibrate ESCs"));
    QCOMPARE(refresh->text(), QStringLiteral("Refresh Params"));
    QCOMPARE(status->text(), QString());
    QVERIFY(calibrate->isEnabled());
    QVERIFY(refresh->isEnabled());

    const QStringList instructionNames = {
        QStringLiteral("escRemoveProps"),
        QStringLiteral("escInstruction_1"),
        QStringLiteral("escInstruction_2"),
        QStringLiteral("escInstruction_3"),
        QStringLiteral("escInstruction_4"),
        QStringLiteral("escInstruction_5"),
        QStringLiteral("escInstruction_6")
    };
    const QStringList instructionTexts = {
        QStringLiteral("Remove Props!"),
        QStringLiteral("After pushing this button:"),
        QStringLiteral("-Disconnect USB and battery"),
        QStringLiteral("-Plug in battery"),
        QStringLiteral("-when LEDs flash, push Saftey Switch (if present)"),
        QStringLiteral("-ESCs should beep as they are calibrated"),
        QStringLiteral("- restart flight controller normally")
    };
    for (int index = 0; index < instructionNames.size(); ++index) {
        QLabel *label = view.findChild<QLabel *>(instructionNames.at(index));
        QVERIFY(label);
        QCOMPARE(label->text(), instructionTexts.at(index));
    }

    const QStringList names = ConfigESCCalibrationViewModel::FieldNames();
    const QStringList labels = {
        QStringLiteral("ESC Type:"),
        QStringLiteral("Output PWM Min"),
        QStringLiteral("Output PWM Max"),
        QStringLiteral("Spin when Armed"),
        QStringLiteral("Spin minimum"),
        QStringLiteral("Spin Maximum")
    };
    const QStringList descriptions = {
        QString(),
        QStringLiteral("Leave as 0 to use RX input range"),
        QStringLiteral("Leave as 0 to use RX input range"),
        QStringLiteral(
            "speed when motors are armed but throttle is at zero (idle)"),
        QStringLiteral(
            "minimum speed of motors while in flight (slightly higher than "
            "\"Spin when Armed\")"),
        QStringLiteral(
            "maximum speed of motors while in flight (almost all escs have "
            "a deadzone at the top)")
    };
    for (int index = 0; index < names.size(); ++index) {
        QWidget *row = view.findChild<QWidget *>(
            QStringLiteral("escFieldRow_%1").arg(names.at(index)));
        QLabel *label = view.findChild<QLabel *>(
            QStringLiteral("escFieldLabel_%1").arg(names.at(index)));
        QLabel *description = view.findChild<QLabel *>(
            QStringLiteral("escFieldDescription_%1").arg(names.at(index)));
        QVERIFY(row);
        QVERIFY(label);
        QVERIFY(description);
        QCOMPARE(row->property("parameterName").toString(), names.at(index));
        QCOMPARE(label->text(), labels.at(index));
        QCOMPARE(description->text(), descriptions.at(index));
    }

    QComboBox *pwmType = view.findChild<QComboBox *>(
        QStringLiteral("escFieldEditor_MOT_PWM_TYPE"));
    QVERIFY(pwmType);
    QCOMPARE(pwmType->minimumWidth(), 200);
    QCOMPARE(pwmType->count(), 9);
    QCOMPARE(pwmType->itemText(0), QStringLiteral("Normal"));
    QCOMPARE(pwmType->itemData(8).toInt(), 8);
    QVERIFY(pwmType->isEnabled());

    for (const QString &name : names.mid(1)) {
        QDoubleSpinBox *editor = view.findChild<QDoubleSpinBox *>(
            QStringLiteral("escFieldEditor_%1").arg(name));
        QVERIFY(editor);
        QCOMPARE(editor->decimals(), 6);
        if (name == QLatin1String("MOT_PWM_MIN")
            || name == QLatin1String("MOT_PWM_MAX")) {
            QCOMPARE(editor->minimum(), 0.0);
            QCOMPARE(editor->maximum(), 2000.0);
            QCOMPARE(editor->singleStep(), 1.0);
        } else {
            QCOMPARE(editor->minimum(), 0.0);
            QCOMPARE(editor->maximum(), 1.0);
            QCOMPARE(editor->singleStep(), 0.01);
        }
        QCOMPARE(editor->isEnabled(),
                 name != QLatin1String("MOT_SPIN_MAX"));
    }

    view.setArmed(true);
    QVERIFY(!calibrate->isEnabled());
    QVERIFY(!pwmType->isEnabled());
    for (const QString &name : names.mid(1)) {
        QDoubleSpinBox *editor = view.findChild<QDoubleSpinBox *>(
            QStringLiteral("escFieldEditor_%1").arg(name));
        QVERIFY(editor);
        QVERIFY(!editor->isEnabled());
    }
}

void ConfigESCCalibrationViewTest::
    confirmationTextStatesFullSafetyContract()
{
    QCOMPARE(ConfigESCCalibrationView::CalibrationConfirmationTitle(),
             QStringLiteral("ESC Calibration Safety Warning"));
    const QString text =
        ConfigESCCalibrationView::CalibrationConfirmationText();
    QVERIFY2(text.contains(QStringLiteral("REMOVE ALL PROPELLERS")),
             qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("ESC_CALIBRATION = 3")),
             qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("power-cycle"),
                           Qt::CaseInsensitive),
             qPrintable(text));
}

QTEST_MAIN(ConfigESCCalibrationViewTest)
#include "test_configesccalibrationview.moc"
