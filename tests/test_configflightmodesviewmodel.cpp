#include <QtTest>

#include "ui/configuration/ConfigFlightModesViewModel.h"

#include <QSignalSpy>
#include <QVariantMap>

namespace {
QList<ParamOption> options()
{
    return {{0, QStringLiteral("Stabilize")},
            {1, QStringLiteral("AltHold")},
            {2, QStringLiteral("Loiter")},
            {3, QStringLiteral("Auto")},
            {4, QStringLiteral("RTL")},
            {5, QStringLiteral("Land")}};
}

QList<ConfigFriendlyParameterValue> snapshot(
    ConfigFlightModesViewModel::Family family, int component = 1,
    const QList<int> &modes = {0, 1, 2, 3, 4, 5},
    int channel = 5, int simple = -1, int superSimple = -1)
{
    QList<ConfigFriendlyParameterValue> result;
    const QString prefix = ConfigFlightModesViewModel::ModePrefix(family);
    for (int row = 0; row < modes.size() && row < 6; ++row) {
        result.append({component, prefix + QString::number(row + 1),
                       modes.at(row)});
    }
    const QString switchParameter =
        ConfigFlightModesViewModel::SwitchParameter(family);
    if (!switchParameter.isEmpty() && channel >= 0) {
        result.append({component, switchParameter, channel});
    }
    if (simple >= 0) {
        result.append({component, QStringLiteral("SIMPLE"), simple});
    }
    if (superSimple >= 0) {
        result.append(
            {component, QStringLiteral("SUPER_SIMPLE"), superSimple});
    }
    return result;
}

void makeEditable(
    ConfigFlightModesViewModel *model,
    ConfigFlightModesViewModel::Family family =
        ConfigFlightModesViewModel::Family::Copter,
    const QList<int> &modes = {0, 1, 2, 3, 4, 5},
    int simple = -1, int superSimple = -1)
{
    model->setFamily(family, options());
    model->setConnected(true);
    model->setParameterSnapshot(
        snapshot(family, 1, modes, 5, simple, superSimple), 1, true);
    model->setHeartbeat(3, true, false);
    QVERIFY(model->CanEdit());
}

QVariantList emittedChanges(const QSignalSpy &spy, int emission = 0)
{
    return spy.at(emission).at(2).toList();
}

QStringList changeNames(const QVariantList &changes)
{
    QStringList result;
    for (const QVariant &item : changes) {
        result.append(item.toMap().value(
            QStringLiteral("name")).toString());
    }
    return result;
}

QVariant changeValue(const QVariantList &changes, const QString &name)
{
    for (const QVariant &item : changes) {
        const QVariantMap change = item.toMap();
        if (change.value(QStringLiteral("name")).toString() == name) {
            return change.value(QStringLiteral("value"));
        }
    }
    return {};
}

bool hasOption(const QList<ParamOption> &values, int value,
               const QString &text = QString())
{
    for (const ParamOption &option : values) {
        if (option.value.toInt() == value
            && (text.isEmpty() || option.text == text)) {
            return true;
        }
    }
    return false;
}
} // namespace

class ConfigFlightModesViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactFamiliesRowsAndSchemas();
    void completeSnapshotRequiresAllSixOnOneComponent();
    void hydrationPreservesUnknownAndNeverWrites();
    void heartbeatAndRcGateLivePresentation();
    void px4SlotEnumsStaySeparateFromHeartbeatCustomMode();
    void refreshPreservesStagedEdits();
    void stagesOneChangedOnlyBatchAndPreservesUpperBits();
    void exactTerminalCorrelationAndSingleFailure();
    void multiItemFailureWaitsAndRequiresReconciliation();
    void submittedMultiItemRejectsConcurrentSnapshot();
    void submittedMultiItemDisconnectIsUncertain();
};

void ConfigFlightModesViewModelTest::exactFamiliesRowsAndSchemas()
{
    ConfigFlightModesViewModel model;
    QCOMPARE(model.Rows().size(), 6);
    QCOMPARE(model.VehicleFamily(),
             ConfigFlightModesViewModel::Family::Unsupported);
    QVERIFY(!model.IsAvailable());

    const QStringList expectedBands{
        QStringLiteral("PWM 0 - 1230"),
        QStringLiteral("PWM 1231 - 1360"),
        QStringLiteral("PWM 1361 - 1490"),
        QStringLiteral("PWM 1491 - 1620"),
        QStringLiteral("PWM 1621 - 1749"),
        QStringLiteral("PWM 1750 +")};
    const int expectedMinimums[] = {0, 1231, 1361, 1491, 1621, 1750};
    const int expectedMaximums[] = {1230, 1360, 1490, 1620, 1749, -1};
    for (int row = 0; row < 6; ++row) {
        QCOMPARE(model.Rows().at(row).position, row + 1);
        QCOMPARE(model.Rows().at(row).pwmBand, expectedBands.at(row));
        QCOMPARE(model.Rows().at(row).minimumPwm, expectedMinimums[row]);
        QCOMPARE(model.Rows().at(row).maximumPwm, expectedMaximums[row]);
        QCOMPARE(ConfigFlightModesViewModel::PwmBand(row),
                 expectedBands.at(row));
    }

    struct Schema {
        ConfigFlightModesViewModel::Family family;
        QString prefix;
        QString channel;
    };
    const QList<Schema> schemas{
        {ConfigFlightModesViewModel::Family::Copter,
         QStringLiteral("FLTMODE"), QStringLiteral("FLTMODE_CH")},
        {ConfigFlightModesViewModel::Family::Plane,
         QStringLiteral("FLTMODE"), QStringLiteral("FLTMODE_CH")},
        {ConfigFlightModesViewModel::Family::Rover,
         QStringLiteral("MODE"), QStringLiteral("MODE_CH")},
        {ConfigFlightModesViewModel::Family::Px4,
         QStringLiteral("COM_FLTMODE"),
         QStringLiteral("COM_FLTMODE_CH")}};
    for (const Schema &schema : schemas) {
        model.setFamily(schema.family, options());
        QCOMPARE(ConfigFlightModesViewModel::ModePrefix(schema.family),
                 schema.prefix);
        QCOMPARE(ConfigFlightModesViewModel::SwitchParameter(schema.family),
                 schema.channel);
        QCOMPARE(model.Rows().size(), 6);
        for (int row = 0; row < 6; ++row) {
            QCOMPARE(model.Rows().at(row).parameterName,
                     schema.prefix + QString::number(row + 1));
        }
        QCOMPARE(hasOption(model.ModeOptions(), 31),
                 schema.family
                     == ConfigFlightModesViewModel::Family::Copter);
    }
    model.setFamily(ConfigFlightModesViewModel::Family::Copter, options());
    QVERIFY(hasOption(model.ModeOptions(), 31, QStringLiteral("ModelCal")));
}

void ConfigFlightModesViewModelTest::completeSnapshotRequiresAllSixOnOneComponent()
{
    ConfigFlightModesViewModel model;
    model.setFamily(ConfigFlightModesViewModel::Family::Copter, options());
    model.setConnected(true);
    model.setHeartbeat(0, true, false);

    QList<ConfigFriendlyParameterValue> split;
    for (int row = 1; row <= 3; ++row) {
        split.append({7, QStringLiteral("FLTMODE%1").arg(row), row});
    }
    for (int row = 4; row <= 6; ++row) {
        split.append({8, QStringLiteral("FLTMODE%1").arg(row), row});
    }
    model.setParameterSnapshot(split, 7, true);
    QVERIFY(model.SnapshotComplete());
    QVERIFY(!model.SnapshotReady());
    QVERIFY(!model.CanEdit());
    QCOMPARE(model.ComponentId(), 7);

    QList<ConfigFriendlyParameterValue> values = snapshot(
        ConfigFlightModesViewModel::Family::Copter, 42);
    values.append({1, QStringLiteral("FLTMODE1"), 99});
    model.setParameterSnapshot(values, 42, false);
    QVERIFY(!model.SnapshotComplete());
    QVERIFY(!model.SnapshotReady());

    model.setParameterSnapshot(values, 42, true);
    QVERIFY(model.SnapshotComplete());
    QVERIFY(model.SnapshotReady());
    QCOMPARE(model.ComponentId(), 42);
    QCOMPARE(model.Rows().at(0).mode.toInt(), 0);
    QVERIFY(model.CanEdit());

    // A complete Rover schema does not satisfy the selected Copter family.
    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Rover, 42), 42, true);
    QVERIFY(!model.SnapshotReady());
}

void ConfigFlightModesViewModelTest::hydrationPreservesUnknownAndNeverWrites()
{
    auto *model = new ConfigFlightModesViewModel;
    QSignalSpy writes(model,
                     &ConfigFlightModesViewModel::writeRequested);
    model->setFamily(ConfigFlightModesViewModel::Family::Copter,
                     {{0, QStringLiteral("Stabilize")}});
    model->setConnected(true);
    model->setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Copter, 9,
                 {99, 0, 0, 0, 0, 0}, 7, 0, 0),
        9, true);
    model->setHeartbeat(99, true, false);

    QCOMPARE(writes.count(), 0);
    QCOMPARE(model->Rows().at(0).mode.toInt(), 99);
    QVERIFY(hasOption(model->ModeOptions(), 99,
                      QStringLiteral("Unknown (99)")));
    QVERIFY(hasOption(model->ModeOptions(), 31, QStringLiteral("ModelCal")));
    QCOMPARE(model->CurrentModeText(), QStringLiteral("Unknown (99)"));
    QCOMPARE(model->SwitchChannel(), 7);
    QVERIFY(model->ShowSimple());
    QVERIFY(model->ShowSuperSimple());

    delete model;
    QCOMPARE(writes.count(), 0);
}

void ConfigFlightModesViewModelTest::heartbeatAndRcGateLivePresentation()
{
    ConfigFlightModesViewModel model;
    model.setFamily(ConfigFlightModesViewModel::Family::Copter, options());
    model.setConnected(true);
    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Copter, 1), 1, true);
    QVERIFY(!model.CanEdit());
    QVERIFY(model.CurrentModeText().isEmpty());

    model.setHeartbeat(3, true, false);
    QVERIFY(model.HeartbeatFresh());
    QVERIFY(model.CanEdit());
    QCOMPARE(model.CurrentModeText(), QStringLiteral("Auto"));
    model.setHeartbeat(77, true, false);
    QCOMPARE(model.CurrentModeText(), QStringLiteral("Unknown (77)"));
    model.setHeartbeat(3, true, false);
    QCOMPARE(model.SwitchChannel(), 5);
    QVERIFY(!model.setRcInput(4, 1490));
    QVERIFY(!model.setRcInput(5, 0));
    QCOMPARE(model.ActiveRow(), -1);

    const QList<int> pwm{1230, 1231, 1360, 1361, 1490,
                         1491, 1620, 1621, 1749, 1750, 2200};
    const QList<int> rows{0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
    for (int index = 0; index < pwm.size(); ++index) {
        QVERIFY(model.setRcInput(5, pwm.at(index)));
        QCOMPARE(model.ActiveRow(), rows.at(index));
        QVERIFY(model.Rows().at(rows.at(index)).active);
    }
    QCOMPARE(model.CurrentPwmText(),
             QStringLiteral("5: 2200"));
    model.clearRcInput();
    QCOMPARE(model.ActiveRow(), -1);
    QVERIFY(!model.HasCurrentPwm());
    QVERIFY(model.CurrentPwmText().isEmpty());

    model.setHeartbeatFresh(false);
    QVERIFY(!model.HeartbeatFresh());
    QVERIFY(model.CurrentModeText().isEmpty());
    QVERIFY(!model.CanEdit());
    model.setHeartbeat(1, true, true);
    QVERIFY(model.Armed());
    QVERIFY(!model.CanEdit());
    model.setHeartbeat(1, true, false);
    QVERIFY(model.CanEdit());
}

void ConfigFlightModesViewModelTest::px4SlotEnumsStaySeparateFromHeartbeatCustomMode()
{
    ConfigFlightModesViewModel model;
    const QList<ParamOption> slotOptions{
        {0, QStringLiteral("Unassigned")},
        {1, QStringLiteral("MANUAL")},
        {4, QStringLiteral("AUTO / MISSION")},
        {6, QStringLiteral("RETURN TO LAUNCH")},
        {12, QStringLiteral("AUTO / LAND")}};
    model.setFamily(ConfigFlightModesViewModel::Family::Px4,
                    slotOptions);
    model.setConnected(true);
    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Px4, 1,
                 {1, 4, 6, 12, 0, 1}),
        1, true);

    model.setHeartbeat((4U << 16U) | (5U << 24U), true, false);
    QCOMPARE(model.CurrentModeText(), QStringLiteral("RTL"));
    QVERIFY(model.stageMode(0, 12));
    const QVariantList changes = model.DirtyChanges();
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changeValue(changes, QStringLiteral("COM_FLTMODE1")).toInt(),
             12);
    QVERIFY(changeValue(changes, QStringLiteral("COM_FLTMODE1")).toInt()
            < (1 << 16));

    model.setHeartbeat(1, true, false);
    QCOMPARE(model.CurrentModeText(), QStringLiteral("Unknown (1)"));
}

void ConfigFlightModesViewModelTest::refreshPreservesStagedEdits()
{
    ConfigFlightModesViewModel model;
    makeEditable(&model, ConfigFlightModesViewModel::Family::Copter,
                 {0, 1, 2, 3, 4, 5}, 0, 0);
    QVERIFY(model.stageMode(0, 3));
    QVERIFY(model.stageSimple(1, true));
    QVERIFY(model.stageSuperSimple(2, true));

    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Copter, 1,
                 {0, 1, 2, 4, 5, 0}, 5, 0, 0),
        1, true, true);

    QVERIFY(model.Dirty());
    QCOMPARE(model.Rows().at(0).acceptedMode.toInt(), 0);
    QCOMPARE(model.Rows().at(0).mode.toInt(), 3);
    QVERIFY(model.Rows().at(1).simple);
    QVERIFY(model.Rows().at(2).superSimple);
    QVERIFY(model.Status().contains(QStringLiteral("preserved")));
}

void ConfigFlightModesViewModelTest::stagesOneChangedOnlyBatchAndPreservesUpperBits()
{
    ConfigFlightModesViewModel model;
    makeEditable(&model, ConfigFlightModesViewModel::Family::Copter,
                 {0, 1, 2, 3, 4, 5}, 0x145, 0x282);
    QSignalSpy writes(&model,
                     &ConfigFlightModesViewModel::writeRequested);

    QVERIFY(model.stageMode(0, 3));
    QVERIFY(model.stageSimple(1, true));
    QVERIFY(model.stageSuperSimple(0, true));
    QVERIFY(model.Dirty());
    QVERIFY(model.CanSave());
    QCOMPARE(writes.count(), 0);

    QVERIFY(model.Save());
    QCOMPARE(writes.count(), 1);
    QVERIFY(model.HasPendingWrites());
    QVERIFY(!model.CanEdit());
    const QVariantList changes = emittedChanges(writes);
    QCOMPARE(changeNames(changes),
             (QStringList{QStringLiteral("FLTMODE1"),
                          QStringLiteral("SIMPLE"),
                          QStringLiteral("SUPER_SIMPLE")}));
    QCOMPARE(changeValue(changes, QStringLiteral("FLTMODE1")).toInt(), 3);
    // Only bits 0..5 are editable; 0x140 and 0x280 remain intact.
    QCOMPARE(changeValue(changes, QStringLiteral("SIMPLE")).toUInt(),
             quint32(0x147));
    QCOMPARE(changeValue(changes,
                         QStringLiteral("SUPER_SIMPLE")).toUInt(),
             quint32(0x283));

    const quint64 requestId = writes.at(0).at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 70);
    model.parameterBatchCompleted(70, 3, 0);
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(!model.Dirty());
    QVERIFY(!model.CanSave());
    QCOMPARE(model.Rows().at(0).acceptedMode.toInt(), 3);
}

void ConfigFlightModesViewModelTest::exactTerminalCorrelationAndSingleFailure()
{
    ConfigFlightModesViewModel model;
    makeEditable(&model);
    QSignalSpy writes(&model,
                     &ConfigFlightModesViewModel::writeRequested);
    QVERIFY(model.stageMode(0, 1));
    QVERIFY(model.Save());
    const quint64 requestId = writes.at(0).at(0).toULongLong();

    // Raw echoes and unrelated lifecycle events are observations, not success.
    model.parameterChanged(1, QStringLiteral("FLTMODE1"), 1);
    QVERIFY(model.HasPendingWrites());
    model.parameterWriteSubmitted(requestId + 1, 80);
    model.parameterWriteSubmitted(requestId, 81);
    model.parameterBatchCompleted(80, 1, 0);
    model.parameterWriteFailed(81, 2, QStringLiteral("FLTMODE1"),
                               QStringLiteral("wrong component"));
    model.parameterWriteFailed(81, 1, QStringLiteral("FLTMODE2"),
                               QStringLiteral("wrong name"));
    QVERIFY(model.HasPendingWrites());

    model.parameterWriteFailed(81, 1, QStringLiteral("FLTMODE1"),
                               QStringLiteral("denied"));
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(model.Dirty());
    QCOMPARE(model.Rows().at(0).acceptedMode.toInt(), 0);
    QCOMPARE(model.Rows().at(0).mode.toInt(), 1);
    model.parameterBatchCompleted(81, 1, 0); // late terminal is ignored
    QVERIFY(model.Dirty());

    QVERIFY(model.Save());
    const quint64 secondRequest = writes.at(1).at(0).toULongLong();
    model.parameterWriteSubmissionFailed(secondRequest + 1,
                                         QStringLiteral("late"));
    QVERIFY(model.HasPendingWrites());
    model.parameterWriteSubmissionFailed(secondRequest,
                                         QStringLiteral("target changed"));
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(model.Dirty());
}

void ConfigFlightModesViewModelTest::multiItemFailureWaitsAndRequiresReconciliation()
{
    ConfigFlightModesViewModel model;
    makeEditable(&model);
    QSignalSpy writes(&model,
                     &ConfigFlightModesViewModel::writeRequested);
    QSignalSpy refreshes(&model,
                        &ConfigFlightModesViewModel::refreshRequested);
    QVERIFY(model.stageMode(0, 1));
    QVERIFY(model.stageMode(1, 2));
    QVERIFY(model.Save());
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 91);

    model.parameterWriteCancelled(91, 1, QStringLiteral("FLTMODE1"));
    QVERIFY(model.HasPendingWrites());
    QVERIFY(model.Status().contains(QStringLiteral("waiting")));
    // Even contradictory success counts cannot overrule a correlated item
    // failure; a multi-item outcome is uncertain until fully reconciled.
    model.parameterBatchCompleted(91, 2, 0);
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(model.ReconciliationRequired());
    QVERIFY(!model.IsAvailable());
    QVERIFY(!model.CanEdit());
    QCOMPARE(refreshes.count(), 1);
    QCOMPARE(model.Rows().size(), 6);
    QVERIFY(!model.Rows().at(0).mode.isValid());

    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Copter), 1, false);
    QVERIFY(model.ReconciliationRequired());
    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Copter, 1,
                 {0, 1, 2, 3, 4}),
        1, true);
    QVERIFY(model.ReconciliationRequired());
    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Copter), 1, true);
    QVERIFY(!model.ReconciliationRequired());
    QVERIFY(model.IsAvailable());
    QVERIFY(model.CanEdit());
}

void ConfigFlightModesViewModelTest::submittedMultiItemRejectsConcurrentSnapshot()
{
    ConfigFlightModesViewModel model;
    makeEditable(&model);
    QSignalSpy writes(&model,
                     &ConfigFlightModesViewModel::writeRequested);
    QSignalSpy refreshes(&model,
                        &ConfigFlightModesViewModel::refreshRequested);
    QVERIFY(model.stageMode(0, 1));
    QVERIFY(model.stageMode(1, 2));
    QVERIFY(model.Save());
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 96);

    // Even a nominally complete list can have been captured before the
    // submitted writes. It must not silently replace batch ownership.
    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Copter), 1, true);
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(model.ReconciliationRequired());
    QVERIFY(!model.SnapshotReady());
    QCOMPARE(refreshes.count(), 1);
    model.parameterBatchCompleted(96, 2, 0);
    QVERIFY(model.ReconciliationRequired());

    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Copter), 1, true);
    QVERIFY(!model.ReconciliationRequired());
    QVERIFY(model.SnapshotReady());
}

void ConfigFlightModesViewModelTest::submittedMultiItemDisconnectIsUncertain()
{
    ConfigFlightModesViewModel model;
    makeEditable(&model);
    QSignalSpy writes(&model,
                     &ConfigFlightModesViewModel::writeRequested);
    QSignalSpy refreshes(&model,
                        &ConfigFlightModesViewModel::refreshRequested);
    QVERIFY(model.stageMode(0, 1));
    QVERIFY(model.stageMode(1, 2));
    QVERIFY(model.Save());
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 101);
    model.setConnected(false);
    QVERIFY(model.ReconciliationRequired());
    QVERIFY(!model.IsAvailable());
    QCOMPARE(refreshes.count(), 0);

    model.parameterBatchCompleted(101, 2, 0);
    model.parameterWriteFailed(101, 1, QStringLiteral("FLTMODE1"),
                               QStringLiteral("late"));
    QVERIFY(model.ReconciliationRequired());

    model.setConnected(true);
    QCOMPARE(refreshes.count(), 1);
    model.setHeartbeat(0, true, false);
    QVERIFY(!model.CanEdit());
    model.setParameterSnapshot(
        snapshot(ConfigFlightModesViewModel::Family::Copter), 1, true);
    QVERIFY(model.CanEdit());

    // Before a request is assigned a batch id, disconnect leaves the local
    // staged edits intact and all later request callbacks are ignored.
    QVERIFY(model.stageMode(0, 1));
    QVERIFY(model.stageMode(1, 2));
    QVERIFY(model.Save());
    const quint64 unsubmittedRequest = writes.at(1).at(0).toULongLong();
    model.setConnected(false);
    QVERIFY(!model.ReconciliationRequired());
    QVERIFY(model.Dirty());
    model.parameterWriteSubmitted(unsubmittedRequest, 102);
    QVERIFY(!model.HasPendingWrites());
}

QTEST_GUILESS_MAIN(ConfigFlightModesViewModelTest)
#include "test_configflightmodesviewmodel.moc"
