#include <QtTest>

#include "ui/configuration/ConfigExtendedTuningViewModel.h"

#include <QBuffer>
#include <QSet>
#include <QSignalSpy>
#include <QVariantMap>

#include <algorithm>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduPlane">
        <param name="ArduPlane:Q_A_RAT_RLL_P" humanName="Quad Roll P"
               documentation="QuadPlane roll proportional gain">
          <field name="Units">gain</field>
          <field name="Range">0 2</field>
          <field name="Increment">0.05</field>
        </param>
        <param name="ArduPlane:TUNE" humanName="Tuning Selector">
          <values>
            <value code="0">None</value>
            <value code="1">Rate Roll/Pitch P</value>
          </values>
        </param>
        <param name="ArduPlane:INS_HNTCH_OPTS" humanName="Notch Options">
          <field name="Bitmask">0:Double notch,1:Dynamic harmonics</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduPlane"));
}

QList<ConfigFriendlyParameterValue> fullSnapshot(
    int component = 1, int qEnable = 1)
{
    QList<ConfigFriendlyParameterValue> result;
    result.append({component, QStringLiteral("Q_ENABLE"), qEnable});
    for (const ExtendedTuningRow &row :
         ConfigExtendedTuningViewModel::ReferenceRows()) {
        const QVariant value =
            row.field.editorKind == ParamField::EditorKind::Combo
            ? QVariant(0) : QVariant(1.0);
        result.append({component, row.candidates.constFirst(), value});
    }
    return result;
}

void setParameter(QList<ConfigFriendlyParameterValue> *parameters,
                  const QString &name, const QVariant &value,
                  int component = 1)
{
    for (ConfigFriendlyParameterValue &parameter : *parameters) {
        if (parameter.componentId == component
            && parameter.name == name) {
            parameter.value = value;
            return;
        }
    }
    parameters->append({component, name, value});
}

void removeParameter(QList<ConfigFriendlyParameterValue> *parameters,
                     const QString &name, int component = 1)
{
    parameters->erase(std::remove_if(
        parameters->begin(), parameters->end(),
        [&name, component](const ConfigFriendlyParameterValue &parameter) {
            return parameter.componentId == component
                && parameter.name == name;
        }), parameters->end());
}

ExtendedTuningRow rowResolved(
    const ConfigExtendedTuningViewModel &model, const QString &name)
{
    for (const ExtendedTuningRow &row : model.Rows()) {
        if (row.resolvedName == name) {
            return row;
        }
    }
    return {};
}

ExtendedTuningRow rowWithCandidate(
    const ConfigExtendedTuningViewModel &model, const QString &name)
{
    for (const ExtendedTuningRow &row : model.Rows()) {
        if (row.candidates.contains(name)) {
            return row;
        }
    }
    return {};
}

void makeEditable(ConfigExtendedTuningViewModel *model,
                  const QList<ConfigFriendlyParameterValue> &values =
                      fullSnapshot())
{
    model->setConnected(true);
    model->setParameterSnapshot(values, 1, true);
    model->setHeartbeat(true, false);
    QVERIFY(model->CanEdit());
}

QStringList changeNames(const QVariantList &changes)
{
    QStringList result;
    for (const QVariant &entry : changes) {
        result.append(entry.toMap()
                          .value(QStringLiteral("name")).toString());
    }
    return result;
}

QVariant changeValue(const QVariantList &changes, const QString &name)
{
    for (const QVariant &entry : changes) {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("name")).toString() == name) {
            return item.value(QStringLiteral("value"));
        }
    }
    return {};
}
} // namespace

class ConfigExtendedTuningViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactMp10GroupsRowsAndPlaneAliasPriority();
    void exactSnapshotDeterminesQuadPlaneStateAndGates();
    void metadataUnknownEnumRangeAndMissingRows();
    void rollPitchLockMirrorsDescriptorsWithoutHydrationWrites();
    void changedOnlyBatchAndLargeIncreaseConfirmation();
    void terminalCorrelationAndSingleFailureKeepStagedEdits();
    void multiWriteFailureSnapshotRaceAndDisconnectReconcile();
};

void ConfigExtendedTuningViewModelTest::
exactMp10GroupsRowsAndPlaneAliasPriority()
{
    const QList<ExtendedTuningGroupDescriptor> groups =
        ConfigExtendedTuningViewModel::ReferenceGroups();
    const QList<ExtendedTuningRow> rows =
        ConfigExtendedTuningViewModel::ReferenceRows();
    QCOMPARE(groups.size(), 17);
    QCOMPARE(rows.size(), 68);
    const QStringList expectedTitles{
        QStringLiteral("Transmitter Tuning"),
        QStringLiteral("Rate Roll"),
        QStringLiteral("Rate Pitch"),
        QStringLiteral("Rate Yaw"),
        QStringLiteral("Stabilize Roll (Error to Rate)"),
        QStringLiteral("Stabilize Pitch (Error to Rate)"),
        QStringLiteral("Stabilize Yaw (Error to Rate)"),
        QStringLiteral("Throttle Accel (Accel to motor)"),
        QStringLiteral("Throttle Rate (VSpd to accel)"),
        QStringLiteral("Altitude Hold (Alt to climbrate)"),
        QStringLiteral("Velocity XY (Vel to Accel)"),
        QStringLiteral("Position XY (Dist to Speed)"),
        QStringLiteral("WPNav (cm's)"),
        QStringLiteral("Basic Filters"),
        QStringLiteral("Static Notch Filter"),
        QStringLiteral("Harmonic Notch Filter"),
        QStringLiteral("Filter Logs")};
    const QList<int> expectedCounts{8, 7, 7, 7, 2, 2, 2, 4, 1,
                                    1, 4, 2, 5, 2, 4, 8, 2};
    int expectedFirst = 0;
    for (int index = 0; index < groups.size(); ++index) {
        QCOMPARE(groups.at(index).title, expectedTitles.at(index));
        QCOMPARE(groups.at(index).firstRow, expectedFirst);
        QCOMPARE(groups.at(index).rowCount, expectedCounts.at(index));
        expectedFirst += groups.at(index).rowCount;
    }
    QCOMPARE(expectedFirst, 68);
    QCOMPARE(rows.constFirst().candidates.constFirst(),
             QStringLiteral("TUNE"));
    QCOMPARE(rows.constLast().candidates.constFirst(),
             QStringLiteral("INS_LOG_BAT_OPT"));
    QCOMPARE(rows.at(8).candidates,
             (QStringList{QStringLiteral("Q_A_RAT_RLL_P"),
                          QStringLiteral("RATE_RLL_P"),
                          QStringLiteral("ATC_RAT_RLL_P")}));
    QCOMPARE(rows.at(47).candidates,
             (QStringList{QStringLiteral("Q_WP_SPEED"),
                          QStringLiteral("Q_WP_SPD"),
                          QStringLiteral("WPNAV_SPEED"),
                          QStringLiteral("WP_SPD")}));
    QCOMPARE(rows.at(3).candidates.constLast(),
             QStringLiteral("RC6_OPTION"));
    QCOMPARE(rows.at(52).candidates.constFirst(),
             QStringLiteral("INS_GYRO_FILTER"));
    QCOMPARE(rows.at(8).mirrorTargetRow, 15);
    QCOMPARE(rows.at(9).mirrorTargetRow, 16);
    QCOMPARE(rows.at(11).mirrorTargetRow, 18);

    QList<ConfigFriendlyParameterValue> values = fullSnapshot(7, 1);
    values.append({7, QStringLiteral("RATE_RLL_P"), 9.0});
    values.append({7, QStringLiteral("WPNAV_SPEED"), 900.0});
    ConfigExtendedTuningViewModel model;
    model.setParameterSnapshot(values, 7, true);
    QCOMPARE(model.ComponentId(), 7);
    QCOMPARE(model.Rows().at(8).resolvedName,
             QStringLiteral("Q_A_RAT_RLL_P"));
    QCOMPARE(model.Rows().at(8).acceptedValue.toDouble(), 1.0);
    QCOMPARE(model.Rows().at(47).resolvedName,
             QStringLiteral("Q_WP_SPEED"));

    removeParameter(&values, QStringLiteral("Q_A_RAT_RLL_P"), 7);
    model.setParameterSnapshot(values, 7, true);
    QCOMPARE(model.Rows().at(8).resolvedName,
             QStringLiteral("RATE_RLL_P"));
    QCOMPARE(model.Rows().at(8).acceptedValue.toDouble(), 9.0);

    model.setParameterSnapshot(values, 9, true);
    QCOMPARE(model.ComponentId(), 9);
    QCOMPARE(model.QuadPlaneModeState(),
             ConfigExtendedTuningViewModel::QuadPlaneState::Unavailable);
    QVERIFY(!model.Rows().at(8).exists);
}

void ConfigExtendedTuningViewModelTest::
exactSnapshotDeterminesQuadPlaneStateAndGates()
{
    ConfigExtendedTuningViewModel model;
    QCOMPARE(model.Rows().size(), 68);
    QCOMPARE(model.QuadPlaneModeState(),
             ConfigExtendedTuningViewModel::QuadPlaneState::Unknown);
    QVERIFY(!model.CanEdit());

    model.setConnected(true);
    model.setHeartbeat(true, false);
    model.setParameterSnapshot(fullSnapshot(1, 2), 1, false);
    QCOMPARE(model.QuadPlaneModeState(),
             ConfigExtendedTuningViewModel::QuadPlaneState::Unknown);
    QVERIFY(!model.SnapshotReady());
    QVERIFY(!model.CanEdit());
    const QList<ExtendedTuningRow> incompleteRows = model.Rows();
    QVERIFY(std::all_of(incompleteRows.constBegin(),
                        incompleteRows.constEnd(),
                        [](const ExtendedTuningRow &row) {
        return !row.exists && row.field.status == QLatin1String("n/a");
    }));

    model.setParameterSnapshot(fullSnapshot(1, 0), 1, true);
    QCOMPARE(model.QuadPlaneModeState(),
             ConfigExtendedTuningViewModel::QuadPlaneState::Disabled);
    QVERIFY(model.SnapshotReady());
    QVERIFY(model.CanEdit());
    QVERIFY(!model.CanEditRow(8));
    QVERIFY(model.CanEditRow(3));
    QVERIFY(model.CanEditRow(52));

    model.setParameterSnapshot(fullSnapshot(1, 2), 1, true);
    QCOMPARE(model.QuadPlaneModeState(),
             ConfigExtendedTuningViewModel::QuadPlaneState::Enabled);
    QVERIFY(model.CanEdit());
    model.parameterChanged(1, QStringLiteral("Q_ENABLE"), 0);
    QCOMPARE(model.QuadPlaneModeState(),
             ConfigExtendedTuningViewModel::QuadPlaneState::Disabled);
    QVERIFY(model.CanEdit());
    QVERIFY(!model.CanEditRow(8));
    QVERIFY(model.CanEditRow(3));
    model.parameterChanged(2, QStringLiteral("Q_ENABLE"), 2);
    QVERIFY(!model.CanEditRow(8));
    model.parameterChanged(1, QStringLiteral("Q_ENABLE"), 2);
    QCOMPARE(model.QuadPlaneModeState(),
             ConfigExtendedTuningViewModel::QuadPlaneState::Enabled);
    QVERIFY(model.CanEdit());
    model.setArmed(true);
    QVERIFY(!model.CanEdit());
    model.setArmed(false);
    model.setHeartbeatFresh(false);
    QVERIFY(!model.CanEdit());
    model.setHeartbeat(true, false);
    QVERIFY(model.CanEdit());

    QList<ConfigFriendlyParameterValue> inferred{
        {3, QStringLiteral("Q_A_RAT_RLL_P"), 0.2}};
    model.setParameterSnapshot(inferred, 3, true);
    QCOMPARE(model.QuadPlaneModeState(),
             ConfigExtendedTuningViewModel::QuadPlaneState::Enabled);
    QVERIFY(model.Rows().at(8).exists);
    QVERIFY(!model.Rows().at(9).exists);
    QCOMPARE(model.Rows().at(9).field.status, QStringLiteral("n/a"));

    model.setParameterSnapshot(
        {{3, QStringLiteral("INS_GYRO_FILTER"), 20}}, 3, true);
    QCOMPARE(model.QuadPlaneModeState(),
             ConfigExtendedTuningViewModel::QuadPlaneState::Unavailable);
    QVERIFY(model.CanEdit());
    QVERIFY(!model.CanEditRow(8));
    QVERIFY(model.CanEditRow(52));
}

void ConfigExtendedTuningViewModelTest::
metadataUnknownEnumRangeAndMissingRows()
{
    QList<ConfigFriendlyParameterValue> values = fullSnapshot();
    setParameter(&values, QStringLiteral("TUNE"), 9);
    setParameter(&values, QStringLiteral("Q_A_RAT_RLL_P"), 1.0);
    ConfigExtendedTuningViewModel model;
    model.setCatalog(catalogFixture());
    makeEditable(&model, values);

    const ExtendedTuningRow roll = rowResolved(
        model, QStringLiteral("Q_A_RAT_RLL_P"));
    QCOMPARE(roll.label, QStringLiteral("P"));
    QCOMPARE(roll.field.units, QStringLiteral("gain"));
    QCOMPARE(roll.field.description,
             QStringLiteral("QuadPlane roll proportional gain"));
    QVERIFY(roll.field.hasRange);
    QVERIFY(roll.field.enforceRange);
    QCOMPARE(roll.field.minimum, 0.0);
    QCOMPARE(roll.field.maximum, 2.0);
    QCOMPARE(roll.field.increment, 0.05);
    model.setLockRollPitch(false);
    QVERIFY(!model.stageValue(8, 2.1));
    QVERIFY(model.stageValue(8, 1.5));

    const ExtendedTuningRow tune = rowResolved(
        model, QStringLiteral("TUNE"));
    QCOMPARE(tune.field.editorKind, ParamField::EditorKind::Combo);
    QCOMPARE(tune.field.options.size(), 3);
    QCOMPARE(tune.field.options.constLast().value.toInt(), 9);
    QCOMPARE(tune.field.options.constLast().text,
             QStringLiteral("Unknown (9)"));
    QVERIFY(model.stageValue(QStringLiteral("TUNE"), 1));
    QVERIFY(!model.stageValue(QStringLiteral("TUNE"), 77));

    const ExtendedTuningRow notch = rowResolved(
        model, QStringLiteral("INS_HNTCH_OPTS"));
    QCOMPARE(notch.field.editorKind, ParamField::EditorKind::Bitmask);
    QCOMPARE(notch.field.bitOptions.size(), 2);
    QCOMPARE(notch.field.bitOptions.at(0).label,
             QStringLiteral("Double notch"));

    QList<ConfigFriendlyParameterValue> sparse{
        {1, QStringLiteral("Q_ENABLE"), 1},
        {1, QStringLiteral("Q_A_RAT_RLL_P"), 0.2}};
    model.setParameterSnapshot(sparse, 1, true);
    QCOMPARE(model.Rows().size(), 68);
    QVERIFY(rowWithCandidate(model,
                             QStringLiteral("Q_A_RAT_RLL_P")).exists);
    const ExtendedTuningRow missing = rowWithCandidate(
        model, QStringLiteral("Q_A_RAT_RLL_I"));
    QVERIFY(!missing.exists);
    QVERIFY(missing.field.readOnly);
    QCOMPARE(missing.field.status, QStringLiteral("n/a"));
}

void ConfigExtendedTuningViewModelTest::
rollPitchLockMirrorsDescriptorsWithoutHydrationWrites()
{
    auto *model = new ConfigExtendedTuningViewModel;
    QSignalSpy writes(model,
                     &ConfigExtendedTuningViewModel::writeRequested);
    makeEditable(model);
    QCOMPARE(writes.count(), 0);
    QVERIFY(model->LockRollPitch());

    QVERIFY(model->stageValue(8, 1.5));
    QCOMPARE(model->Rows().at(8).field.value.toDouble(), 1.5);
    QCOMPARE(model->Rows().at(15).field.value.toDouble(), 1.5);
    QCOMPARE(changeNames(model->DirtyChanges()),
             (QStringList{QStringLiteral("Q_A_RAT_RLL_P"),
                          QStringLiteral("Q_A_RAT_PIT_P")}));

    QVERIFY(model->stageValue(12, 1.25));
    QCOMPARE(model->Rows().at(19).field.value.toDouble(), 1.25);
    QVERIFY(model->stageValue(29, 1.25));
    QCOMPARE(model->Rows().at(31).field.value.toDouble(), 1.25);

    QVERIFY(model->Discard());
    model->setLockRollPitch(false);
    QVERIFY(model->stageValue(8, 1.25));
    QCOMPARE(model->Rows().at(8).field.value.toDouble(), 1.25);
    QCOMPARE(model->Rows().at(15).field.value.toDouble(), 1.0);
    QCOMPARE(writes.count(), 0);
    delete model;
    QCOMPARE(writes.count(), 0);
}

void ConfigExtendedTuningViewModelTest::
changedOnlyBatchAndLargeIncreaseConfirmation()
{
    ConfigExtendedTuningViewModel model;
    makeEditable(&model);
    model.setLockRollPitch(false);
    QSignalSpy writes(&model,
                     &ConfigExtendedTuningViewModel::writeRequested);
    QSignalSpy confirmations(
        &model,
        &ConfigExtendedTuningViewModel::
            largeIncreaseConfirmationRequested);

    QVERIFY(model.stageValue(8, 1.5));
    QVERIFY(model.stageValue(9, 1.25));
    QCOMPARE(changeNames(model.DirtyChanges()),
             (QStringList{QStringLiteral("Q_A_RAT_RLL_P"),
                          QStringLiteral("Q_A_RAT_RLL_I")}));
    QVERIFY(model.Save());
    QCOMPARE(writes.count(), 1);
    QCOMPARE(confirmations.count(), 0);
    const QVariantList changes = writes.at(0).at(2).toList();
    QCOMPARE(changeValue(changes,
                         QStringLiteral("Q_A_RAT_RLL_P")).toDouble(),
             1.5);
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 30);
    model.parameterBatchCompleted(30, 2, 0);
    QVERIFY(!model.Dirty());

    QVERIFY(model.stageValue(8, 4.0));
    QVERIFY(model.RequiresLargeIncreaseConfirmation());
    QVERIFY(!model.Save());
    QCOMPARE(confirmations.count(), 1);
    QCOMPARE(confirmations.at(0).at(0).toStringList(),
             (QStringList{QStringLiteral("Q_A_RAT_RLL_P")}));
    QCOMPARE(writes.count(), 1);
    QVERIFY(model.Save(true));
    QCOMPARE(writes.count(), 2);
}

void ConfigExtendedTuningViewModelTest::
terminalCorrelationAndSingleFailureKeepStagedEdits()
{
    ConfigExtendedTuningViewModel model;
    makeEditable(&model);
    model.setLockRollPitch(false);
    QSignalSpy writes(&model,
                     &ConfigExtendedTuningViewModel::writeRequested);
    QVERIFY(model.stageValue(8, 1.5));
    QVERIFY(model.Save());
    const quint64 requestId = writes.at(0).at(0).toULongLong();

    model.parameterChanged(1, QStringLiteral("Q_A_RAT_RLL_P"), 1.5);
    QVERIFY(model.HasPendingWrites());
    QCOMPARE(model.Rows().at(8).acceptedValue.toDouble(), 1.0);
    model.parameterWriteSubmitted(requestId + 1, 40);
    model.parameterWriteSubmitted(requestId, 41);
    model.parameterBatchCompleted(40, 1, 0);
    model.parameterWriteFailed(41, 2,
                               QStringLiteral("Q_A_RAT_RLL_P"),
                               QStringLiteral("wrong component"));
    model.parameterWriteFailed(41, 1,
                               QStringLiteral("Q_A_RAT_RLL_I"),
                               QStringLiteral("wrong name"));
    QVERIFY(model.HasPendingWrites());

    model.parameterWriteFailed(41, 1,
                               QStringLiteral("Q_A_RAT_RLL_P"),
                               QStringLiteral("denied"));
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(model.Dirty());
    QCOMPARE(model.Rows().at(8).acceptedValue.toDouble(), 1.0);
    QCOMPARE(model.Rows().at(8).field.value.toDouble(), 1.5);
    model.parameterBatchCompleted(41, 1, 0);
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

void ConfigExtendedTuningViewModelTest::
multiWriteFailureSnapshotRaceAndDisconnectReconcile()
{
    ConfigExtendedTuningViewModel model;
    makeEditable(&model);
    model.setLockRollPitch(false);
    QSignalSpy writes(&model,
                     &ConfigExtendedTuningViewModel::writeRequested);
    QSignalSpy refreshes(&model,
                        &ConfigExtendedTuningViewModel::refreshRequested);

    QVERIFY(model.stageValue(8, 1.5));
    QVERIFY(model.stageValue(9, 1.25));
    QVERIFY(model.Save());
    quint64 requestId = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 50);
    model.parameterWriteCancelled(50, 1,
                                  QStringLiteral("Q_A_RAT_RLL_P"));
    QVERIFY(model.HasPendingWrites());
    model.parameterBatchCompleted(50, 2, 0);
    QVERIFY(model.ReconciliationRequired());
    QVERIFY(!model.SnapshotReady());
    QVERIFY(!model.CanEdit());
    QCOMPARE(model.Rows().size(), 68);
    QCOMPARE(refreshes.count(), 1);

    model.setParameterSnapshot(fullSnapshot(), 1, false);
    QVERIFY(model.ReconciliationRequired());
    model.setParameterSnapshot(fullSnapshot(), 1, true);
    QVERIFY(!model.ReconciliationRequired());
    QVERIFY(model.CanEdit());

    QVERIFY(model.stageValue(8, 1.5));
    QVERIFY(model.stageValue(9, 1.25));
    QVERIFY(model.Save());
    requestId = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 51);
    model.setParameterSnapshot(fullSnapshot(), 1, true);
    QVERIFY(model.ReconciliationRequired());
    QCOMPARE(refreshes.count(), 2);
    model.parameterBatchCompleted(51, 2, 0);
    QVERIFY(model.ReconciliationRequired());

    model.setParameterSnapshot(fullSnapshot(), 1, true);
    QVERIFY(model.CanEdit());
    QVERIFY(model.stageValue(8, 1.5));
    QVERIFY(model.stageValue(9, 1.25));
    QVERIFY(model.Save());
    requestId = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 52);
    model.setConnected(false);
    QVERIFY(model.ReconciliationRequired());
    QCOMPARE(refreshes.count(), 2);
    model.parameterBatchCompleted(52, 2, 0);
    model.setConnected(true);
    QCOMPARE(refreshes.count(), 3);
    model.setHeartbeat(true, false);
    model.setParameterSnapshot(fullSnapshot(), 1, true);
    QVERIFY(model.CanEdit());

    // Before submission receives a batch id, disconnect keeps local edits and
    // ignores any late ownership callback rather than claiming success.
    QVERIFY(model.stageValue(8, 1.5));
    QVERIFY(model.stageValue(9, 1.25));
    QVERIFY(model.Save());
    requestId = writes.constLast().at(0).toULongLong();
    model.setConnected(false);
    QVERIFY(!model.ReconciliationRequired());
    QVERIFY(model.Dirty());
    model.parameterWriteSubmitted(requestId, 53);
    QVERIFY(!model.HasPendingWrites());
}

QTEST_GUILESS_MAIN(ConfigExtendedTuningViewModelTest)
#include "test_configextendedtuningviewmodel.moc"
