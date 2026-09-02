#include <QtTest>

#include "ui/configuration/ConfigRadioOutputView.h"

#include <QAbstractSpinBox>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QProgressBar>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTimer>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:SERVO1_FUNCTION"
               humanName="Servo output function"
               documentation="Function assigned to this servo.">
          <values>
            <value code="0">Disabled</value>
            <value code="1">RCPassThru</value>
            <value code="4">Aileron</value>
          </values>
        </param>
        <param name="ArduCopter:SERVO1_REVERSED"
               humanName="Servo reverse" />
        <param name="ArduCopter:SERVO1_MIN"
               humanName="Minimum PWM" />
        <param name="ArduCopter:SERVO1_TRIM"
               humanName="Trim PWM" />
        <param name="ArduCopter:SERVO1_MAX"
               humanName="Maximum PWM" />
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> completeServo(
    int number, int componentId = 1)
{
    const QString prefix = QStringLiteral("SERVO%1_").arg(number);
    return {
        {componentId, prefix + QStringLiteral("REVERSED"), 0},
        {componentId, prefix + QStringLiteral("FUNCTION"), 1},
        {componentId, prefix + QStringLiteral("MIN"), 1000},
        {componentId, prefix + QStringLiteral("TRIM"), 1500},
        {componentId, prefix + QStringLiteral("MAX"), 2000}
    };
}

void appendParameters(QList<ConfigFriendlyParameterValue> *target,
                      const QList<ConfigFriendlyParameterValue> &source)
{
    for (const ConfigFriendlyParameterValue &parameter : source) {
        target->append(parameter);
    }
}

ServoOutputRow servoRow(const ConfigRadioOutputViewModel &model, int number)
{
    const QList<ServoOutputRow> rows = model.Rows();
    if (number < 1 || number > rows.size()) {
        return {};
    }
    return rows.at(number - 1);
}

void verifyFieldNames(const ServoOutputRow &row, int number)
{
    const QString prefix = QStringLiteral("SERVO%1_").arg(number);
    QCOMPARE(row.Number, number);
    QCOMPARE(row.Reversed.name, prefix + QStringLiteral("REVERSED"));
    QCOMPARE(row.Function.name, prefix + QStringLiteral("FUNCTION"));
    QCOMPARE(row.Min.name, prefix + QStringLiteral("MIN"));
    QCOMPARE(row.Trim.name, prefix + QStringLiteral("TRIM"));
    QCOMPARE(row.Max.name, prefix + QStringLiteral("MAX"));
}

int rowWidgetCount(const ConfigRadioOutputView &view)
{
    int count = 0;
    for (QFrame *frame : view.findChildren<QFrame *>()) {
        if (frame->property("servoOutputRow").toBool()) {
            ++count;
        }
    }
    return count;
}
}

class ConfigRadioOutputViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void createsSixteenOrThirtyTwoRowsWithExactFieldNames();
    void hydratesMetadataAndMissingAvailabilityWithoutWrites();
    void userWriteAndEchoDoNotLoop();
    void newestEchoClosesSupersededWriteWindow();
    void coalescesServoOutputTelemetryDeterministically();
    void widgetMatchesMissionPlannerContractAndTimerLifecycle();
};

void ConfigRadioOutputViewTest::
    createsSixteenOrThirtyTwoRowsWithExactFieldNames()
{
    ConfigRadioOutputViewModel model;
    model.setCatalog(catalogFixture());

    QList<ConfigFriendlyParameterValue> parameters = {
        {1, QStringLiteral("SERVO_32_ENABLE"), 0}
    };
    appendParameters(&parameters, completeServo(1));
    appendParameters(&parameters, completeServo(16));
    appendParameters(&parameters, completeServo(17));
    appendParameters(&parameters, completeServo(32));
    model.setParameterSnapshot(parameters);

    QCOMPARE(model.Rows().size(), 16);
    verifyFieldNames(servoRow(model, 1), 1);
    verifyFieldNames(servoRow(model, 16), 16);

    model.parameterChanged(
        1, QStringLiteral("SERVO_32_ENABLE"), 1);
    QCOMPARE(model.Rows().size(), 32);
    verifyFieldNames(servoRow(model, 17), 17);
    verifyFieldNames(servoRow(model, 32), 32);

    model.parameterChanged(
        1, QStringLiteral("SERVO_32_ENABLE"), 0);
    QCOMPARE(model.Rows().size(), 16);
}

void ConfigRadioOutputViewTest::
    hydratesMetadataAndMissingAvailabilityWithoutWrites()
{
    ConfigRadioOutputViewModel model;
    QSignalSpy writes(&model,
                     &ConfigRadioOutputViewModel::writeRequested);
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot({
        {1, QStringLiteral("SERVO1_REVERSED"), 1},
        {1, QStringLiteral("SERVO1_FUNCTION"), 4},
        {1, QStringLiteral("SERVO1_MIN"), 980},
        {1, QStringLiteral("SERVO1_TRIM"), 1490}
    });

    QCOMPARE(writes.count(), 0);
    const ServoOutputRow first = servoRow(model, 1);
    QCOMPARE(first.Reversed.value.toInt(), 1);
    QCOMPARE(first.Function.value.toInt(), 4);
    QCOMPARE(first.Min.value.toInt(), 980);
    QCOMPARE(first.Trim.value.toInt(), 1490);
    QCOMPARE(first.Max.value.toInt(), 1500);
    QVERIFY(!first.Reversed.readOnly);
    QVERIFY(!first.Function.readOnly);
    QVERIFY(!first.Min.readOnly);
    QVERIFY(!first.Trim.readOnly);
    QVERIFY(first.Max.readOnly);
    QCOMPARE(first.Function.options.size(), 3);
    QCOMPARE(first.Function.options.at(0).value.toInt(), 0);
    QCOMPARE(first.Function.options.at(0).text,
             QStringLiteral("Disabled"));
    QCOMPARE(first.Function.options.at(2).value.toInt(), 4);
    QCOMPARE(first.Function.options.at(2).text,
             QStringLiteral("Aileron"));

    const ServoOutputRow second = servoRow(model, 2);
    QVERIFY(second.Reversed.readOnly);
    QVERIFY(second.Function.readOnly);
    QVERIFY(second.Min.readOnly);
    QVERIFY(second.Trim.readOnly);
    QVERIFY(second.Max.readOnly);
    // Row-specific metadata falls back to the SERVO1 template, but a missing
    // vehicle parameter remains disabled.
    QCOMPARE(second.Function.options.size(), 3);
}

void ConfigRadioOutputViewTest::userWriteAndEchoDoNotLoop()
{
    ConfigRadioOutputView view(catalogFixture());
    QSignalSpy writes(&view, &ConfigRadioOutputView::writeRequested);
    view.setParameterSnapshot({
        {1, QStringLiteral("SERVO1_REVERSED"), 0},
        {1, QStringLiteral("SERVO1_MIN"), 1000}
    });
    QCOMPARE(writes.count(), 0);

    auto *reversed = view.findChild<QCheckBox *>(
        QStringLiteral("servoReversed_1"));
    QVERIFY(reversed);
    QVERIFY(reversed->isEnabled());
    QVERIFY(!reversed->isChecked());
    reversed->click();

    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toInt(), 1);
    QCOMPARE(writes.first().at(1).toString(),
             QStringLiteral("SERVO1_REVERSED"));
    QCOMPARE(writes.first().at(2).toInt(), 1);

    view.parameterChanged(
        1, QStringLiteral("SERVO1_REVERSED"), 1);
    QCOMPARE(writes.count(), 1);
    QVERIFY(reversed->isChecked());

    QVERIFY(view.viewModel()->setMin(1, 1100));
    QCOMPARE(writes.count(), 2);
    QCOMPARE(writes.at(1).at(1).toString(),
             QStringLiteral("SERVO1_MIN"));
    QCOMPARE(writes.at(1).at(2).toInt(), 1100);
    view.parameterChanged(1, QStringLiteral("SERVO1_MIN"), 1100);
    QCOMPARE(writes.count(), 2);
    QCOMPARE(servoRow(*view.viewModel(), 1).Min.value.toInt(), 1100);
}

void ConfigRadioOutputViewTest::newestEchoClosesSupersededWriteWindow()
{
    ConfigRadioOutputViewModel model;
    model.setParameterSnapshot({
        {1, QStringLiteral("SERVO1_MIN"), 1000}
    });
    QSignalSpy writes(&model,
                     &ConfigRadioOutputViewModel::writeRequested);

    QVERIFY(model.setMin(1, 1100));
    QVERIFY(model.setMin(1, 1200));
    QCOMPARE(writes.count(), 2);

    model.parameterChanged(1, QStringLiteral("SERVO1_MIN"), 1100);
    QCOMPARE(servoRow(model, 1).Min.value.toInt(), 1200);
    model.parameterChanged(1, QStringLiteral("SERVO1_MIN"), 1200);
    QCOMPARE(servoRow(model, 1).Min.value.toInt(), 1200);
    // Once the newest write is acknowledged, a later wire value is a fresh
    // authoritative update. Do not suppress real changes from another GCS.
    model.parameterChanged(1, QStringLiteral("SERVO1_MIN"), 1100);
    QCOMPARE(servoRow(model, 1).Min.value.toInt(), 1100);
}

void ConfigRadioOutputViewTest::
    coalescesServoOutputTelemetryDeterministically()
{
    ConfigRadioOutputViewModel model;
    QSignalSpy rowsChanged(&model,
                          &ConfigRadioOutputViewModel::rowChanged);

    model.setServoOutput(1, 1200);
    model.setServoOutput(1, 1300);
    model.setServoOutput(2, 1400);
    model.setServoOutput(0, 1700);
    model.setServoOutput(17, 1700);
    QCOMPARE(rowsChanged.count(), 0);
    QCOMPARE(servoRow(model, 1).Pwm, 1500);
    QCOMPARE(servoRow(model, 2).Pwm, 1500);

    model.flushServoOutputs();
    QCOMPARE(rowsChanged.count(), 2);
    QCOMPARE(servoRow(model, 1).Pwm, 1300);
    QCOMPARE(servoRow(model, 2).Pwm, 1400);

    model.flushServoOutputs();
    QCOMPARE(rowsChanged.count(), 2);
    model.setServoOutput(1, 1300);
    model.flushServoOutputs();
    QCOMPARE(rowsChanged.count(), 2);
}

void ConfigRadioOutputViewTest::
    widgetMatchesMissionPlannerContractAndTimerLifecycle()
{
    ConfigRadioOutputView view(catalogFixture());
    view.resize(900, 700);
    view.setParameterSnapshot({
        {1, QStringLiteral("SERVO1_REVERSED"), 0},
        {1, QStringLiteral("SERVO1_FUNCTION"), 1},
        {1, QStringLiteral("SERVO1_MIN"), 1000},
        {1, QStringLiteral("SERVO1_TRIM"), 1500}
    });

    QCOMPARE(view.objectName(), QStringLiteral("ConfigRadioOutputView"));
    QLabel *title = view.findChild<QLabel *>(
        QStringLiteral("servoOutputTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("Servo Output"));
    QStringList headers;
    for (int column = 0; column < 7; ++column) {
        QLabel *header = view.findChild<QLabel *>(
            QStringLiteral("servoOutputHeader%1").arg(column));
        QVERIFY(header);
        headers.append(header->text());
    }
    QCOMPARE(headers, QStringList({QStringLiteral("#"),
                                   QStringLiteral("Position"),
                                   QStringLiteral("Reverse"),
                                   QStringLiteral("Function"),
                                   QStringLiteral("Min"),
                                   QStringLiteral("Trim"),
                                   QStringLiteral("Max")}));
    QVERIFY(view.findChild<QScrollArea *>(
        QStringLiteral("servoOutputScroll")));
    QVERIFY(view.findChild<QWidget *>(
        QStringLiteral("servoOutputRows")));
    QCOMPARE(rowWidgetCount(view), 16);

    QFrame *firstRow = view.findChild<QFrame *>(
        QStringLiteral("servoOutputRow_1"));
    QVERIFY(firstRow);
    QCOMPARE(firstRow->property("servoNumber").toInt(), 1);
    QLabel *number = firstRow->findChild<QLabel *>(
        QStringLiteral("servoNumber_1"));
    QVERIFY(number);
    QCOMPARE(number->text(), QStringLiteral("1"));

    auto *position = firstRow->findChild<QProgressBar *>(
        QStringLiteral("servoPosition_1"));
    auto *reversed = firstRow->findChild<QCheckBox *>(
        QStringLiteral("servoReversed_1"));
    auto *function = firstRow->findChild<QComboBox *>(
        QStringLiteral("servoFunction_1"));
    auto *minimum = firstRow->findChild<QSpinBox *>(
        QStringLiteral("servoMin_1"));
    auto *trim = firstRow->findChild<QSpinBox *>(
        QStringLiteral("servoTrim_1"));
    auto *maximum = firstRow->findChild<QSpinBox *>(
        QStringLiteral("servoMax_1"));
    QVERIFY(position);
    QVERIFY(reversed);
    QVERIFY(function);
    QVERIFY(minimum);
    QVERIFY(trim);
    QVERIFY(maximum);

    QCOMPARE(position->minimum(), 800);
    QCOMPARE(position->maximum(), 2200);
    QCOMPARE(position->value(), 1500);
    QCOMPARE(position->format(), QStringLiteral("1500"));
    QCOMPARE(position->height(), 22);
    QCOMPARE(function->minimumWidth(), 160);
    QCOMPARE(function->count(), 3);
    QCOMPARE(function->itemText(0), QStringLiteral("Disabled"));
    QCOMPARE(function->itemData(2).toInt(), 4);
    QVERIFY(reversed->isEnabled());
    QVERIFY(function->isEnabled());
    QVERIFY(minimum->isEnabled());
    QVERIFY(trim->isEnabled());
    QVERIFY(!maximum->isEnabled());

    for (QSpinBox *spin : {minimum, trim, maximum}) {
        QCOMPARE(spin->minimum(), 800);
        QCOMPARE(spin->maximum(), 2200);
        QCOMPARE(spin->singleStep(), 1);
        QCOMPARE(spin->buttonSymbols(), QAbstractSpinBox::NoButtons);
        QCOMPARE(spin->width(), 66);
    }
    auto *missingFunction = view.findChild<QComboBox *>(
        QStringLiteral("servoFunction_2"));
    QVERIFY(missingFunction);
    QVERIFY(!missingFunction->isEnabled());

    QTimer *telemetryTimer = view.findChild<QTimer *>(
        QStringLiteral("servoOutputTimer"));
    QVERIFY(telemetryTimer);
    QCOMPARE(telemetryTimer->interval(), 100);
    QVERIFY(!view.telemetryTimerActive());
    view.servoOutputChanged(1, 1234);
    QCOMPARE(position->value(), 1500);
    view.show();
    QTRY_VERIFY(view.telemetryTimerActive());
    QTRY_COMPARE(position->value(), 1234);

    view.servoOutputChanged(1, 1300);
    QTRY_COMPARE(position->value(), 1300);
    view.hide();
    QTRY_VERIFY(!view.telemetryTimerActive());
    view.servoOutputChanged(1, 1400);
    QTest::qWait(150);
    QCOMPARE(position->value(), 1300);

    view.show();
    QTRY_VERIFY(view.telemetryTimerActive());
    QTRY_COMPARE(position->value(), 1400);
    view.hide();

    view.setParameterSnapshot({
        {1, QStringLiteral("SERVO_32_ENABLE"), 1}
    });
    QCOMPARE(rowWidgetCount(view), 32);
    QVERIFY(view.findChild<QFrame *>(
        QStringLiteral("servoOutputRow_32")));
}

QTEST_MAIN(ConfigRadioOutputViewTest)
#include "test_configradiooutputview.moc"
