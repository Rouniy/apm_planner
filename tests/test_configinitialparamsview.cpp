#include <QtTest>

#include "ui/configuration/ConfigInitialParamsView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMetaType>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>

namespace {
ParamCompareRow result(const ConfigInitialParamsViewModel &model,
                       const QString &name)
{
    for (const ParamCompareRow &row : model.Results()) {
        if (row.name == name) {
            return row;
        }
    }
    return {};
}

bool containsResult(const ConfigInitialParamsViewModel &model,
                    const QString &name)
{
    return !result(model, name).name.isEmpty();
}
}

class ConfigInitialParamsViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndBatteryChemistryMatchMissionPlanner();
    void defaultCopterV4CalculationMatchesMissionPlanner();
    void namespaceVersionAndOptionalRowsMatchMissionPlanner();
    void formulaBoundsAndValidationAreSafe();
    void staleResultsAreInvalidated();
    void stagingUsesOnlySelectedLiveExistingRows();
    void writesWaitForAcknowledgementsAndCountFailures();
    void snapshotReplacementCancelsStagedState();
    void real32AckAndPlaneCompletionMessageAreCorrect();
    void viewMatchesInitialAndCalculatedStates();
};

void ConfigInitialParamsViewTest::defaultsAndBatteryChemistryMatchMissionPlanner()
{
    ConfigInitialParamsViewModel model;
    QCOMPARE(model.PropSize(), QStringLiteral("9"));
    QCOMPARE(model.CellCount(), QStringLiteral("4"));
    QCOMPARE(model.CellMax(), QStringLiteral("4.2"));
    QCOMPARE(model.CellMin(), QStringLiteral("3.3"));
    QCOMPARE(model.BatteryType(), QStringLiteral("LiPo"));
    QCOMPARE(ConfigInitialParamsViewModel::BatteryTypes(),
             QStringList({QStringLiteral("LiPo"),
                          QStringLiteral("LiPoHV"),
                          QStringLiteral("LiIon")}));
    QVERIFY(!model.TMotor());
    QVERIFY(!model.Suggested());
    QVERIFY(!model.HasResults());
    QVERIFY(model.Status().isEmpty());

    model.setBatteryType(QStringLiteral("LiPoHV"));
    QCOMPARE(model.CellMax(), QStringLiteral("4.35"));
    QCOMPARE(model.CellMin(), QStringLiteral("3.3"));
    model.setBatteryType(QStringLiteral("LiIon"));
    QCOMPARE(model.CellMax(), QStringLiteral("4.1"));
    QCOMPARE(model.CellMin(), QStringLiteral("2.8"));
    model.setBatteryType(QStringLiteral("LiPo"));
    QCOMPARE(model.CellMax(), QStringLiteral("4.2"));
    QCOMPARE(model.CellMin(), QStringLiteral("3.3"));
}

void ConfigInitialParamsViewTest::defaultCopterV4CalculationMatchesMissionPlanner()
{
    ConfigInitialParamsViewModel model;
    model.setVehicleContext(false, 4);
    model.setParameterSnapshot({
        {1, QStringLiteral("ATC_ACCEL_P_MAX"), 100000},
        {1, QStringLiteral("INS_GYRO_FILTER"), 40}
    });
    QVERIFY(model.Calculate());
    QCOMPARE(model.Results().size(), 23);
    QCOMPARE(result(model, QStringLiteral("ACRO_YAW_P")).value, 3.1);
    QCOMPARE(result(model, QStringLiteral("ATC_ACCEL_P_MAX")).value,
             125900.0);
    QCOMPARE(result(model, QStringLiteral("ATC_ACCEL_R_MAX")).value,
             125900.0);
    QCOMPARE(result(model, QStringLiteral("ATC_ACCEL_Y_MAX")).value,
             27900.0);
    QCOMPARE(result(model, QStringLiteral("ATC_RAT_PIT_FLTD")).value, 23.0);
    QCOMPARE(result(model, QStringLiteral("ATC_RAT_PIT_FLTE")).value, 0.0);
    QCOMPARE(result(model, QStringLiteral("ATC_RAT_PIT_FLTT")).value, 23.0);
    QCOMPARE(result(model, QStringLiteral("ATC_RAT_RLL_FLTD")).value, 23.0);
    QCOMPARE(result(model, QStringLiteral("ATC_RAT_YAW_FLTD")).value, 0.0);
    QCOMPARE(result(model, QStringLiteral("ATC_RAT_YAW_FLTE")).value, 2.0);
    QCOMPARE(result(model, QStringLiteral("ATC_RAT_YAW_FLTT")).value, 23.0);
    QCOMPARE(result(model, QStringLiteral("ATC_THR_MIX_MAN")).value, 0.1);
    QCOMPARE(result(model, QStringLiteral("INS_ACCEL_FILTER")).value, 10.0);
    QCOMPARE(result(model, QStringLiteral("INS_GYRO_FILTER")).value, 46.0);
    QCOMPARE(result(model, QStringLiteral("MOT_THST_EXPO")).value, 0.58);
    QCOMPARE(result(model, QStringLiteral("MOT_THST_HOVER")).value, 0.2);
    QCOMPARE(result(model, QStringLiteral("BATT_ARM_VOLT")).value, 14.7);
    QCOMPARE(result(model, QStringLiteral("BATT_CRT_VOLT")).value, 14.0);
    QCOMPARE(result(model, QStringLiteral("BATT_LOW_VOLT")).value, 14.4);
    QCOMPARE(result(model, QStringLiteral("MOT_BAT_VOLT_MAX")).value, 16.8);
    QCOMPARE(result(model, QStringLiteral("MOT_BAT_VOLT_MIN")).value, 13.2);
    QCOMPARE(result(model, QStringLiteral("INS_GYRO_FILTER")).current,
             QStringLiteral("40"));
    QVERIFY(result(model, QStringLiteral("INS_GYRO_FILTER")).use);
    QCOMPARE(result(model, QStringLiteral("MOT_THST_EXPO")).current,
             QStringLiteral("n/a"));
    QVERIFY(!result(model, QStringLiteral("MOT_THST_EXPO")).use);
    QCOMPARE(model.Status(),
             QStringLiteral("Review the values below, then Write to FC."));
}

void ConfigInitialParamsViewTest::namespaceVersionAndOptionalRowsMatchMissionPlanner()
{
    ConfigInitialParamsViewModel plane;
    plane.setVehicleContext(true, 4);
    plane.setParameterSnapshot({
        {1, QStringLiteral("Q_A_ACCEL_P_MAX"), 100000}
    });
    plane.setTMotor(true);
    plane.setSuggested(true);
    QVERIFY(plane.Calculate());
    QCOMPARE(plane.Results().size(), 25);
    QVERIFY(containsResult(plane, QStringLiteral("Q_A_ACCEL_P_MAX")));
    QVERIFY(containsResult(plane, QStringLiteral("Q_M_THST_EXPO")));
    QCOMPARE(result(plane, QStringLiteral("Q_M_THST_EXPO")).value, 0.2);
    QCOMPARE(result(plane, QStringLiteral("Q_M_PWM_MIN")).value, 1100.0);
    QCOMPARE(result(plane, QStringLiteral("Q_M_PWM_MAX")).value, 1940.0);
    QVERIFY(!containsResult(plane, QStringLiteral("FENCE_ENABLE")));

    ConfigInitialParamsViewModel otherVersion;
    otherVersion.setVehicleContext(false, 5);
    otherVersion.setParameterSnapshot({});
    QVERIFY(otherVersion.Calculate());
    QCOMPARE(otherVersion.Results().size(), 17);
    QCOMPARE(result(otherVersion, QStringLiteral("ATC_ACC_P_MAX")).value,
             1259.0);
    QVERIFY(containsResult(otherVersion,
                           QStringLiteral("ATC_RAT_PIT_FILT")));
    QVERIFY(!containsResult(otherVersion,
                            QStringLiteral("ATC_RAT_PIT_FLTD")));

    ConfigInitialParamsViewModel detectedModern;
    detectedModern.setVehicleContext(false, 5);
    detectedModern.setParameterSnapshot({
        {1, QStringLiteral("ATC_RAT_PIT_FLTD"), 20}
    });
    QVERIFY(detectedModern.Calculate());
    QCOMPARE(detectedModern.Results().size(), 23);
    QVERIFY(containsResult(detectedModern,
                           QStringLiteral("ATC_RAT_PIT_FLTD")));

    ConfigInitialParamsViewModel suggested;
    suggested.setVehicleContext(false, 4);
    suggested.setParameterSnapshot({});
    suggested.setTMotor(true);
    suggested.setSuggested(true);
    QVERIFY(suggested.Calculate());
    QCOMPARE(suggested.Results().size(), 32);
    QCOMPARE(result(suggested, QStringLiteral("BATT_FS_CRT_ACT")).value, 1.0);
    QCOMPARE(result(suggested, QStringLiteral("BATT_FS_LOW_ACT")).value, 2.0);
    QCOMPARE(result(suggested, QStringLiteral("FENCE_ACTION")).value, 3.0);
    QCOMPARE(result(suggested, QStringLiteral("FENCE_TYPE")).value, 7.0);
}

void ConfigInitialParamsViewTest::formulaBoundsAndValidationAreSafe()
{
    ConfigInitialParamsViewModel midpoint;
    midpoint.setVehicleContext(false, 4);
    midpoint.setPropSize(QStringLiteral("9.5"));
    QVERIFY(midpoint.Calculate());
    QCOMPARE(result(midpoint, QStringLiteral("ATC_ACC_Y_MAX")).value, 275.0);
    QCOMPARE(result(midpoint, QStringLiteral("ATC_ACC_P_MAX")).value, 1212.0);

    ConfigInitialParamsViewModel bounded;
    bounded.setVehicleContext(false, 4);
    bounded.setPropSize(QStringLiteral("100"));
    QVERIFY(bounded.Calculate());
    QCOMPARE(result(bounded, QStringLiteral("ATC_ACC_Y_MAX")).value, 80.0);
    QCOMPARE(result(bounded, QStringLiteral("ATC_ACC_P_MAX")).value, 100.0);
    QCOMPARE(result(bounded, QStringLiteral("INS_GYRO_FILTER")).value, 20.0);
    QCOMPARE(result(bounded, QStringLiteral("MOT_THST_EXPO")).value, 0.8);

    bounded.setCellCount(QStringLiteral("3.5"));
    QVERIFY(!bounded.Calculate());
    QVERIFY(!bounded.HasResults());
    QCOMPARE(bounded.Status(),
             QStringLiteral("Battery cell count must be at least 1."));
    bounded.setCellCount(QStringLiteral("4"));
    bounded.setCellMin(QStringLiteral("4.2"));
    bounded.setCellMax(QStringLiteral("4.1"));
    QVERIFY(!bounded.Calculate());
    QCOMPARE(bounded.Status(),
             QStringLiteral("Battery cell voltages are invalid."));
}

void ConfigInitialParamsViewTest::staleResultsAreInvalidated()
{
    ConfigInitialParamsViewModel model;
    model.setVehicleContext(false, 4);
    QVERIFY(model.Calculate());
    QVERIFY(model.HasResults());
    model.setPropSize(QString());
    QVERIFY(!model.HasResults());
    QVERIFY(!model.Calculate());
    QCOMPARE(model.Status(),
             QStringLiteral("Prop size must be larger than zero."));

    model.setPropSize(QStringLiteral("9"));
    QVERIFY(model.Calculate());
    model.setTMotor(true);
    QVERIFY(!model.HasResults());
}

void ConfigInitialParamsViewTest::stagingUsesOnlySelectedLiveExistingRows()
{
    const ParamCompareRow selected = {
        QStringLiteral("INS_GYRO_FILTER"), {}, {}, 46.0, true, true};
    const ParamCompareRow deselected = {
        QStringLiteral("MOT_THST_EXPO"), {}, {}, 0.58, true, false};
    const ParamCompareRow absent = {
        QStringLiteral("FENCE_ENABLE"), {}, {}, 1.0, false, true};
    const QList<ParamCompareRow> writable =
        ConfigInitialParamsViewModel::SelectWritableRows(
            {selected, deselected, absent},
            {QStringLiteral("ins_gyro_filter"),
             QStringLiteral("MOT_THST_EXPO"),
             QStringLiteral("FENCE_ENABLE")});
    QCOMPARE(writable.size(), 1);
    QCOMPARE(writable.first().name, QStringLiteral("INS_GYRO_FILTER"));
    QVERIFY(ConfigInitialParamsViewModel::SelectWritableRows(
                {selected}, {QStringLiteral("ARMING_CHECK")}).isEmpty());
}

void ConfigInitialParamsViewTest::writesWaitForAcknowledgementsAndCountFailures()
{
    ConfigInitialParamsViewModel model;
    model.setVehicleContext(false, 4);
    model.setParameterSnapshot({
        {1, QStringLiteral("INS_GYRO_FILTER"), 40},
        {1, QStringLiteral("MOT_THST_EXPO"), 0.5},
        {1, QStringLiteral("BATT_CRT_VOLT"), 14.0},
        {154, QStringLiteral("INS_GYRO_FILTER"), 12}
    });
    QVERIFY(model.Calculate());
    QSignalSpy writes(&model, &ConfigInitialParamsViewModel::writeRequested);
    QVERIFY(model.WriteToFc(
        {QStringLiteral("INS_GYRO_FILTER"),
         QStringLiteral("MOT_THST_EXPO"),
         QStringLiteral("BATT_CRT_VOLT")}, true));
    QCOMPARE(writes.count(), 1);
    QVERIFY(model.Writing());
    QVERIFY(model.Status().contains(QStringLiteral("Writing")));
    QCOMPARE(writes.at(0).at(0).toInt(), 1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QCOMPARE(writes.at(0).at(2).typeId(), int(QMetaType::Int));
#else
    QCOMPARE(writes.at(0).at(2).userType(), int(QMetaType::Int));
#endif

    model.parameterChanged(154, QStringLiteral("INS_GYRO_FILTER"), 46.0);
    QVERIFY(model.Writing());
    model.parameterChanged(1, QStringLiteral("INS_GYRO_FILTER"), 46.0);
    QVERIFY(model.Writing());
    QCOMPARE(writes.count(), 2);
    model.parameterChanged(1, QStringLiteral("MOT_THST_EXPO"), 0.4);
    QVERIFY(model.Writing());
    QVERIFY(model.Status().contains(QStringLiteral("Waiting")));
    model.parameterChanged(1, QStringLiteral("MOT_THST_EXPO"), 0.58);
    QVERIFY(!model.Writing());
    QCOMPARE(model.Status(), QStringLiteral(
        "3 initial parameter(s) successfully updated. Check parameters "
        "before flight! After test flight set ATC_THR_MIX_MAN to 0.5."));

    ConfigInitialParamsViewModel partialFailure;
    partialFailure.setVehicleContext(false, 4);
    partialFailure.setParameterSnapshot({
        {1, QStringLiteral("INS_GYRO_FILTER"), 40},
        {1, QStringLiteral("MOT_THST_EXPO"), 0.5}
    });
    QVERIFY(partialFailure.Calculate());
    QVERIFY(partialFailure.WriteToFc(
        {QStringLiteral("INS_GYRO_FILTER"),
         QStringLiteral("MOT_THST_EXPO")}, true));
    partialFailure.parameterChanged(
        1, QStringLiteral("INS_GYRO_FILTER"), 46.0);
    partialFailure.parameterWriteFailed(
        1, QStringLiteral("MOT_THST_EXPO"), QStringLiteral("offline"));
    QVERIFY(!partialFailure.Writing());
    QCOMPARE(partialFailure.Status(),
             QStringLiteral("1 parameter(s) failed to write."));

    QVERIFY(!partialFailure.WriteToFc({}, false));
    QCOMPARE(partialFailure.Status(), QStringLiteral("Not connected."));
}

void ConfigInitialParamsViewTest::snapshotReplacementCancelsStagedState()
{
    ConfigInitialParamsViewModel model;
    model.setVehicleContext(false, 4);
    model.setParameterSnapshot({
        {1, QStringLiteral("INS_GYRO_FILTER"), 40}
    });
    QVERIFY(model.Calculate());
    QVERIFY(model.WriteToFc(
        {QStringLiteral("INS_GYRO_FILTER")}, true));
    QVERIFY(model.Writing());

    model.setParameterSnapshot({
        {1, QStringLiteral("INS_GYRO_FILTER"), 41}
    });
    QVERIFY(!model.Writing());
    QVERIFY(!model.HasResults());
    QVERIFY(model.Status().contains(QStringLiteral("Recalculate")));
    model.parameterChanged(1, QStringLiteral("INS_GYRO_FILTER"), 46);
    QVERIFY(!model.Status().contains(QStringLiteral("successfully")));
}

void ConfigInitialParamsViewTest::real32AckAndPlaneCompletionMessageAreCorrect()
{
    ConfigInitialParamsViewModel battery;
    battery.setVehicleContext(false, 4);
    battery.setCellCount(QStringLiteral("8"));
    battery.setParameterSnapshot({
        {1, QStringLiteral("MOT_BAT_VOLT_MAX"), 30.0f}
    });
    QVERIFY(battery.Calculate());
    QCOMPARE(result(battery, QStringLiteral("MOT_BAT_VOLT_MAX")).value,
             33.6);
    QVERIFY(battery.WriteToFc(
        {QStringLiteral("MOT_BAT_VOLT_MAX")}, true));
    battery.parameterChanged(
        1, QStringLiteral("MOT_BAT_VOLT_MAX"),
        QVariant::fromValue(static_cast<float>(33.6)));
    QVERIFY(!battery.Writing());
    QVERIFY(battery.Status().contains(QStringLiteral("successfully")));

    ConfigInitialParamsViewModel plane;
    plane.setVehicleContext(true, 4);
    plane.setParameterSnapshot({
        {1, QStringLiteral("Q_A_THR_MIX_MAN"), 0.2f}
    });
    QVERIFY(plane.Calculate());
    QVERIFY(plane.WriteToFc(
        {QStringLiteral("Q_A_THR_MIX_MAN")}, true));
    plane.parameterChanged(
        1, QStringLiteral("Q_A_THR_MIX_MAN"), 0.1f);
    QVERIFY(!plane.Writing());
    QVERIFY(plane.Status().contains(QStringLiteral("Q_A_THR_MIX_MAN")));
    QVERIFY(!plane.Status().contains(QStringLiteral("ATC_THR_MIX_MAN")));
}

void ConfigInitialParamsViewTest::viewMatchesInitialAndCalculatedStates()
{
    ConfigInitialParamsView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigInitialParamsView"));
    QCOMPARE(view.findChild<QLineEdit *>(QStringLiteral("propSizeEdit"))
                 ->text(), QStringLiteral("9"));
    QCOMPARE(view.findChild<QLineEdit *>(QStringLiteral("cellCountEdit"))
                 ->text(), QStringLiteral("4"));
    QCOMPARE(view.findChild<QComboBox *>(QStringLiteral("batteryTypeCombo"))
                 ->currentText(), QStringLiteral("LiPo"));
    QVERIFY(!view.findChild<QCheckBox *>(QStringLiteral("tMotorCheck"))
                 ->isChecked());
    auto *write = view.findChild<QPushButton *>(
        QStringLiteral("writeInitialParamsButton"));
    auto *table = view.findChild<QTableWidget *>(
        QStringLiteral("initialParamsResults"));
    QVERIFY(!write->isEnabled());
    QVERIFY(!table->isVisibleTo(&view));
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Use"));
    QCOMPARE(table->horizontalHeaderItem(1)->text(),
             QStringLiteral("Parameter"));
    QCOMPARE(table->horizontalHeaderItem(2)->text(),
             QStringLiteral("Current"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("New"));

    view.setVehicleContext(false, 4);
    view.setParameterSnapshot({
        {1, QStringLiteral("INS_GYRO_FILTER"), 40}
    });
    view.findChild<QPushButton *>(
        QStringLiteral("calculateInitialParamsButton"))->click();
    QVERIFY(write->isEnabled());
    QVERIFY(table->isVisibleTo(&view));
    QCOMPARE(table->rowCount(), 23);
    QCOMPARE(table->minimumHeight(), 280);
    QCOMPARE(table->maximumHeight(), 280);
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("initialParamsDocsUrl"))
                 ->text(),
             QStringLiteral("https://ardupilot.org/copter/docs/"
                            "tuning-process-instructions.html"));

    QSignalSpy writes(&view, &ConfigInitialParamsView::writeRequested);
    QVERIFY(view.WriteToFc(
        {QStringLiteral("INS_GYRO_FILTER")}, true));
    QCOMPARE(writes.count(), 1);
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("calculateInitialParamsButton"))->isEnabled());
    QVERIFY(!table->isEnabled());
    view.parameterChanged(1, QStringLiteral("INS_GYRO_FILTER"), 46);
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("calculateInitialParamsButton"))->isEnabled());
}

QTEST_MAIN(ConfigInitialParamsViewTest)
#include "test_configinitialparamsview.moc"
