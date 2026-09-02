#include "ui/configuration/BatteryMonitorInstanceModel.h"
#include "ui/configuration/ConfigBatteryMonitoring2View.h"
#include "ui/configuration/ConfigBatteryMonitoring2ViewModel.h"
#include "core/parameters/ParameterCodec.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QSignalSpy>
#include <QtTest>

#include <cmath>
#include <limits>

class BatteryMonitorModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void isolatesBatteryInstancesWithoutLegacyFallbacks();
    void resolvesRealAmpPerVoltSpellings();
    void appliesVoltageAndCurrentCalibrationFormulas();
    void rejectsCalibrationWithoutValidLiveValues();
    void decodesOnlySecondBatteryTelemetry();
    void writeLifecycleIsBatchOwnedAndDisconnectSafe();
    void exactComponentAndNumericTypeBoundsAreEnforced();
    void batterySpeechPolicyMatchesMissionPlanner();
    void viewUsesMetadataAndAuthoritativeSnapshots();
};

void BatteryMonitorModelTest::isolatesBatteryInstancesWithoutLegacyFallbacks()
{
    BatteryMonitorInstanceModel first(QStringLiteral("BATT"), 0);
    ConfigBatteryMonitoring2ViewModel second;

    QCOMPARE(first.MonitorParameter(), QStringLiteral("BATT_MONITOR"));
    QCOMPARE(first.VoltMultiplierParameter(),
             QStringLiteral("BATT_VOLT_MULT"));
    QCOMPARE(second.MonitorParameter(), QStringLiteral("BATT2_MONITOR"));
    QCOMPARE(second.CapacityParameter(), QStringLiteral("BATT2_CAPACITY"));
    QCOMPARE(second.VoltPinParameter(), QStringLiteral("BATT2_VOLT_PIN"));
    QCOMPARE(second.CurrPinParameter(), QStringLiteral("BATT2_CURR_PIN"));
    QCOMPARE(second.VoltMultiplierParameter(),
             QStringLiteral("BATT2_VOLT_MULT"));
    QCOMPARE(second.AmpOffsetParameter(),
             QStringLiteral("BATT2_AMP_OFFSET"));

    QVERIFY(first.AcceptsParameter(QStringLiteral("BATT_AMP_PERVLT")));
    QVERIFY(!first.AcceptsParameter(QStringLiteral("BATT2_MONITOR")));
    QVERIFY(!first.AcceptsParameter(QStringLiteral("VOLT_DIVIDER")));
    QVERIFY(!first.AcceptsParameter(QStringLiteral("AMP_PER_VOLT")));
    QVERIFY(second.AcceptsParameter(QStringLiteral("BATT2_AMP_PERVOL")));
    QVERIFY(!second.AcceptsParameter(QStringLiteral("BATT_MONITOR")));
    QVERIFY(!second.AcceptsParameter(QStringLiteral("BATT_AMP_PERVOL")));

    second.setConnected(true);
    second.parameterChanged(154, QStringLiteral("BATT2_MONITOR"), 4);
    QVERIFY(!second.Available());
    second.parameterChanged(MAV_COMP_ID_PRIMARY,
                            QStringLiteral("BATT2_MONITOR"), 4);
    QVERIFY(second.Available());
    QVERIFY(second.CanEdit());

    first.setConnected(true);
    first.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT_MONITOR"), 4);
    first.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT_VOLT_MULT"), 10.0);
    first.setLiveValues(12.0, true, 0.0, false);
    QSignalSpy firstWrites(
        &first, &BatteryMonitorInstanceModel::writeRequested);
    QVERIFY(first.ApplyVoltageCalibration(14.4));
    QCOMPARE(firstWrites.size(), 1);
    QCOMPARE(firstWrites.at(0).at(1).toString(),
             QStringLiteral("BATT_VOLT_MULT"));
    QCOMPARE(firstWrites.at(0).at(2).toDouble(), 12.0);
}

void BatteryMonitorModelTest::resolvesRealAmpPerVoltSpellings()
{
    const QString prefix = QStringLiteral("BATT2");
    QCOMPARE(BatteryMonitorInstanceModel::ResolveAmpPerVoltParameter(
                 prefix, {}), QStringLiteral("BATT2_AMP_PERVLT"));
    QCOMPARE(BatteryMonitorInstanceModel::ResolveAmpPerVoltParameter(
                 prefix, {QStringLiteral("BATT2_AMP_PERVOL")}),
             QStringLiteral("BATT2_AMP_PERVOL"));
    QCOMPARE(BatteryMonitorInstanceModel::ResolveAmpPerVoltParameter(
                 prefix, {QStringLiteral("BATT2_AMP_PERVOLT")}),
             QStringLiteral("BATT2_AMP_PERVOLT"));
    QCOMPARE(BatteryMonitorInstanceModel::ResolveAmpPerVoltParameter(
                 prefix, {QStringLiteral("BATT2_AMP_PERVOLT"),
                          QStringLiteral("BATT2_AMP_PERVOL"),
                          QStringLiteral("BATT2_AMP_PERVLT")}),
             QStringLiteral("BATT2_AMP_PERVLT"));

    ConfigBatteryMonitoring2ViewModel model;
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_AMP_PERVOL"), 17.0);
    QCOMPARE(model.AmpPerVoltParameter(),
             QStringLiteral("BATT2_AMP_PERVOL"));
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_AMP_PERVLT"), 18.0);
    QCOMPARE(model.AmpPerVoltParameter(),
             QStringLiteral("BATT2_AMP_PERVLT"));
}

void BatteryMonitorModelTest::appliesVoltageAndCurrentCalibrationFormulas()
{
    ConfigBatteryMonitoring2ViewModel model;
    model.setConnected(true);
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_MONITOR"), 4);
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_VOLT_MULT"), 10.0);
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_AMP_PERVOL"), 20.0);
    model.setLiveValues(12.0, true, 5.0, true);

    QSignalSpy writes(&model, &BatteryMonitorInstanceModel::writeRequested);
    QVERIFY(model.ApplyVoltageCalibration(14.4));
    QCOMPARE(writes.size(), 1);
    QCOMPARE(writes.at(0).at(0).toInt(),
             static_cast<int>(MAV_COMP_ID_PRIMARY));
    QCOMPARE(writes.at(0).at(1).toString(),
             QStringLiteral("BATT2_VOLT_MULT"));
    QCOMPARE(writes.at(0).at(2).toDouble(), 12.0);
    QVERIFY(model.Status().contains(QStringLiteral("12")));
    QVERIFY(model.HasPendingWrite());
    model.parameterBatchSubmitted(
        MAV_COMP_ID_PRIMARY, QStringLiteral("BATT2_VOLT_MULT"), 41);
    model.parameterWriteAcknowledged(
        MAV_COMP_ID_PRIMARY, QStringLiteral("BATT2_VOLT_MULT"), 12.0,
        int(ParameterType::Real32));
    QVERIFY(model.HasPendingWrite());
    model.parameterBatchCompleted(41, 1, 0);
    QVERIFY(!model.HasPendingWrite());
    QVERIFY(model.Status().contains(QStringLiteral("acknowledged")));

    QVERIFY(model.ApplyCurrentCalibration(10.0));
    QCOMPARE(writes.size(), 2);
    QCOMPARE(writes.at(1).at(1).toString(),
             QStringLiteral("BATT2_AMP_PERVOL"));
    QCOMPARE(writes.at(1).at(2).toDouble(), 40.0);
    QVERIFY(model.Status().contains(QStringLiteral("40")));
}

void BatteryMonitorModelTest::rejectsCalibrationWithoutValidLiveValues()
{
    ConfigBatteryMonitoring2ViewModel model;
    model.setConnected(true);
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_MONITOR"), 4);
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_VOLT_MULT"), 10.0);
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_AMP_PERVLT"), 20.0);
    QSignalSpy writes(&model, &BatteryMonitorInstanceModel::writeRequested);

    QVERIFY(!model.ApplyVoltageCalibration(14.0));
    QVERIFY(model.Status().contains(QStringLiteral("No live voltage")));
    model.setLiveValues(0.0, true, 0.0, true);
    QVERIFY(!model.ApplyVoltageCalibration(14.0));
    QVERIFY(!model.ApplyCurrentCalibration(10.0));
    QCOMPARE(writes.size(), 0);

    double result = 0.0;
    QVERIFY(!BatteryMonitorInstanceModel::CalibratedScale(
        12.0, 0.0, 10.0, &result));
    QVERIFY(!BatteryMonitorInstanceModel::CalibratedScale(
        std::numeric_limits<double>::quiet_NaN(), 12.0, 10.0, &result));
}

void BatteryMonitorModelTest::decodesOnlySecondBatteryTelemetry()
{
    ConfigBatteryMonitoring2ViewModel model;
    QSignalSpy liveSpy(&model,
                       &BatteryMonitorInstanceModel::liveValuesChanged);

    mavlink_battery_status_t status{};
    for (int index = 0;
         index < MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_LEN; ++index) {
        status.voltages[index] = std::numeric_limits<quint16>::max();
    }
    status.id = 0;
    status.voltages[0] = 11000;
    status.current_battery = 200;
    mavlink_message_t message{};
    mavlink_msg_battery_status_encode(1, MAV_COMP_ID_AUTOPILOT1,
                                      &message, &status);
    model.observeMavlinkMessage(message);
    QCOMPARE(liveSpy.size(), 0);
    QVERIFY(!model.HasLiveVoltage());

    mavlink_battery2_t battery2{};
    battery2.voltage = 12500;
    battery2.current_battery = 250;
    mavlink_msg_battery2_encode(1, MAV_COMP_ID_AUTOPILOT1,
                                &message, &battery2);
    model.observeMavlinkMessage(message);
    QCOMPARE(liveSpy.size(), 1);
    QCOMPARE(model.LiveVoltage(), 12.5);
    QCOMPARE(model.LiveCurrent(), 2.5);

    status.id = 1;
    status.voltages[0] = 4000;
    status.voltages[1] = 4000;
    status.voltages[2] = 4000;
    status.current_battery = 345;
    mavlink_msg_battery_status_encode(1, MAV_COMP_ID_AUTOPILOT1,
                                      &message, &status);
    model.observeMavlinkMessage(message);
    QCOMPARE(liveSpy.size(), 2);
    QVERIFY(model.HasLiveVoltage());
    QVERIFY(model.HasLiveCurrent());
    QCOMPARE(model.LiveVoltage(), 12.0);
    QVERIFY(qAbs(model.LiveCurrent() - 3.45) < 1.0e-9);

    status.current_battery = -1;
    mavlink_msg_battery_status_encode(1, MAV_COMP_ID_AUTOPILOT1,
                                      &message, &status);
    model.observeMavlinkMessage(message);
    QVERIFY(model.HasLiveCurrent());
    QVERIFY(qAbs(model.LiveCurrent() - 3.45) < 1.0e-9);
}

void BatteryMonitorModelTest::writeLifecycleIsBatchOwnedAndDisconnectSafe()
{
    ConfigBatteryMonitoring2ViewModel model;
    model.setConnected(true);
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_MONITOR"), 4);
    model.parameterChanged(MAV_COMP_ID_PRIMARY,
                           QStringLiteral("BATT2_CAPACITY"), 5000.0);
    QSignalSpy writes(&model, &BatteryMonitorInstanceModel::writeRequested);

    QVERIFY(model.setFieldValue(QStringLiteral("BATT2_CAPACITY"), 6000.0));
    QVERIFY(model.HasPendingWrite());
    QVERIFY(!model.CanEdit());
    QVERIFY(!model.setFieldValue(QStringLiteral("BATT2_CAPACITY"), 7000.0));
    QCOMPARE(writes.size(), 1);

    model.parameterBatchSubmitted(
        MAV_COMP_ID_PRIMARY, QStringLiteral("BATT2_CAPACITY"), 77);
    const QString beforeForeignFailure = model.Status();
    model.parameterWriteFailed(
        1, 99, MAV_COMP_ID_PRIMARY, QStringLiteral("BATT2_CAPACITY"),
        0, QStringLiteral("foreign"));
    model.parameterBatchCompleted(99, 0, 1);
    QCOMPARE(model.Status(), beforeForeignFailure);
    QVERIFY(model.HasPendingWrite());

    model.parameterWriteAcknowledged(
        MAV_COMP_ID_PRIMARY, QStringLiteral("BATT2_CAPACITY"), 6000.0,
        int(ParameterType::Real32));
    QVERIFY(model.HasPendingWrite());
    QCOMPARE(model.ParameterValue(QStringLiteral("BATT2_CAPACITY"))
                 .toDouble(), 6000.0);
    model.parameterBatchCompleted(77, 1, 0);
    QVERIFY(!model.HasPendingWrite());
    QVERIFY(model.CanEdit());
    QVERIFY(model.Status().contains(QStringLiteral("acknowledged")));

    QVERIFY(model.setFieldValue(QStringLiteral("BATT2_CAPACITY"), 6100.0));
    model.parameterWriteSubmissionFailed(
        MAV_COMP_ID_PRIMARY, QStringLiteral("BATT2_CAPACITY"),
        QStringLiteral("rejected"));
    QVERIFY(!model.HasPendingWrite());
    QVERIFY(model.Status().contains(QStringLiteral("rejected")));

    QVERIFY(model.setFieldValue(QStringLiteral("BATT2_CAPACITY"), 6200.0));
    model.parameterBatchSubmitted(
        MAV_COMP_ID_PRIMARY, QStringLiteral("BATT2_CAPACITY"), 78);
    model.parameterWriteFailed(
        2, 78, MAV_COMP_ID_PRIMARY, QStringLiteral("BATT2_CAPACITY"),
        0, QStringLiteral("timeout"));
    QVERIFY(model.HasPendingWrite());
    model.parameterBatchCompleted(78, 0, 1);
    QVERIFY(!model.HasPendingWrite());
    QVERIFY(model.Status().contains(QStringLiteral("timeout")));

    QVERIFY(model.setFieldValue(QStringLiteral("BATT2_CAPACITY"), 6300.0));
    QVERIFY(model.HasPendingWrite());
    model.setConnected(false);
    QVERIFY(!model.HasPendingWrite());
    QVERIFY(!model.CanEdit());
}

void BatteryMonitorModelTest::exactComponentAndNumericTypeBoundsAreEnforced()
{
    ConfigBatteryMonitoring2ViewModel model;
    model.Reset(42);
    model.setConnected(true);
    model.parameterChanged(1, QStringLiteral("BATT2_MONITOR"), qint32(4));
    QVERIFY(!model.Available());
    model.parameterChanged(42, QStringLiteral("BATT2_MONITOR"), qint32(4));
    model.parameterChanged(42, QStringLiteral("BATT2_CAPACITY"), quint32(5000));
    QVERIFY(model.Available());
    QCOMPARE(model.ComponentId(), 42);

    QSignalSpy writes(&model, &BatteryMonitorInstanceModel::writeRequested);
    QVERIFY(!model.setFieldValue(QStringLiteral("BATT2_CAPACITY"), -1.0));
    QVERIFY(!model.setFieldValue(QStringLiteral("BATT2_CAPACITY"), 1.5));
    QVERIFY(!model.setFieldValue(
        QStringLiteral("BATT2_CAPACITY"), 4294967296.0));
    QCOMPARE(writes.count(), 0);

    QVERIFY(model.setFieldValue(QStringLiteral("BATT2_CAPACITY"), 6000.0));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.constFirst().at(0).toInt(), 42);
    QCOMPARE(writes.constFirst().at(2).toUInt(), quint32(6000));
    model.parameterWriteSubmissionFailed(
        42, QStringLiteral("BATT2_CAPACITY"), QStringLiteral("test"));

    model.parameterChanged(
        42, QStringLiteral("BATT2_CAPACITY"),
        QVariant::fromValue(std::numeric_limits<qulonglong>::max()));
    QVERIFY(!model.setFieldValue(
        QStringLiteral("BATT2_CAPACITY"), std::ldexp(1.0, 64)));
    QCOMPARE(writes.count(), 1);
}

void BatteryMonitorModelTest::batterySpeechPolicyMatchesMissionPlanner()
{
    QVERIFY(BatteryMonitorInstanceModel::ShouldTriggerBatteryAlert(
        9.6, 80.0, 9.6, 20.0));
    QVERIFY(BatteryMonitorInstanceModel::ShouldTriggerBatteryAlert(
        12.0, 19.0, 9.6, 20.0));
    QVERIFY(!BatteryMonitorInstanceModel::ShouldTriggerBatteryAlert(
        4.9, 10.0, 9.6, 20.0));
    QVERIFY(!BatteryMonitorInstanceModel::ShouldTriggerBatteryAlert(
        12.0, 0.0, 9.6, 20.0));
    QVERIFY(!BatteryMonitorInstanceModel::ShouldTriggerBatteryAlert(
        std::numeric_limits<double>::quiet_NaN(),
        10.0, 9.6, 20.0));
    QCOMPARE(BatteryMonitorInstanceModel::FormatBatteryAlert(
                 QStringLiteral("Battery {batv} V, {batp}%"),
                 11.26, 19.4),
             QStringLiteral("Battery 11.26 V, 19%"));
}

void BatteryMonitorModelTest::viewUsesMetadataAndAuthoritativeSnapshots()
{
    QSettings settings;
    settings.remove(QStringLiteral("speechbatteryenabled"));
    settings.remove(QStringLiteral("speechenable"));
    settings.remove(QStringLiteral("speechbattery"));
    settings.remove(QStringLiteral("speechbatteryvolt"));
    settings.remove(QStringLiteral("speechbatterypercent"));
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:BATT2_MONITOR">
          <values><value code="0">Disabled</value>
            <value code="4">Analog</value><value code="24">DroneCAN</value>
          </values>
        </param>
        <param name="ArduCopter:BATT2_CAPACITY">
          <field name="Range">0 20000</field>
          <field name="Increment">50</field>
        </param>
        <param name="ArduCopter:BATT2_VOLT_PIN">
          <values><value code="2">Pixhawk</value>
            <value code="17">Cube Orange</value></values>
        </param>
        <param name="ArduCopter:BATT2_CURR_PIN">
          <values><value code="3">Pixhawk Current</value></values>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    const ParameterMetaDataCatalog catalog =
        ParameterMetaDataCatalog::fromPdef(
            &buffer, QStringLiteral("ArduCopter"));
    QVERIFY(catalog.isValid());

    ConfigBatteryMonitoring2View view(catalog);
    view.setConnected(true);
    view.viewModel()->setLiveValues(12.5, true, 3.25, true);
    view.setParameterSnapshot({
        {42, QStringLiteral("BATT2_MONITOR"), 24},
        {42, QStringLiteral("BATT2_CAPACITY"), 5000},
        {42, QStringLiteral("BATT2_VOLT_PIN"), 17},
        {42, QStringLiteral("BATT2_CURR_PIN"), 99}
    }, 42);

    auto *monitor = view.findChild<QComboBox *>(QStringLiteral("Monitor"));
    auto *voltPin = view.findChild<QComboBox *>(QStringLiteral("VoltPin"));
    auto *currPin = view.findChild<QComboBox *>(QStringLiteral("CurrPin"));
    auto *capacity = view.findChild<QDoubleSpinBox *>(
        QStringLiteral("Capacity"));
    QVERIFY(monitor);
    QVERIFY(voltPin);
    QVERIFY(currPin);
    QVERIFY(capacity);
    QCOMPARE(view.viewModel()->ComponentId(), 42);
    QCOMPARE(monitor->count(), 3);
    QCOMPARE(monitor->currentText(), QStringLiteral("DroneCAN"));
    QCOMPARE(voltPin->currentText(), QStringLiteral("Cube Orange"));
    QVERIFY(currPin->currentText().contains(QStringLiteral("Vehicle value")));
    QCOMPARE(capacity->singleStep(), 50.0);
    QCOMPARE(capacity->maximum(), 20000.0);

    QSignalSpy writes(&view,
                      &ConfigBatteryMonitoring2View::writeRequested);
    QVERIFY(view.viewModel()->setFieldValue(
        QStringLiteral("BATT2_CAPACITY"), 6000));
    QCOMPARE(writes.size(), 1);
    view.parameterWriteSubmissionFailed(
        42, QStringLiteral("BATT2_CAPACITY"),
        QStringLiteral("rejected"));
    QVERIFY(!view.viewModel()->HasPendingWrite());

    view.setParameterSnapshot({
        {42, QStringLiteral("BATT2_MONITOR"), 4}
    }, 42);
    QVERIFY(!view.viewModel()->HasParameter(
        QStringLiteral("BATT2_CAPACITY")));
    QCOMPARE(view.viewModel()->LiveVoltage(), 12.5);
    QCOMPARE(view.viewModel()->LiveCurrent(), 3.25);

    auto *alert = view.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechbattery"));
    QVERIFY(alert);
    QVERIFY(!alert->isChecked());
    alert->setChecked(true);
    QVERIFY(QSettings().value(
        QStringLiteral("speechbatteryenabled")).toBool());
    QVERIFY(QSettings().contains(QStringLiteral("speechbattery")));
}

QTEST_MAIN(BatteryMonitorModelTest)
#include "test_batterymonitor.moc"
