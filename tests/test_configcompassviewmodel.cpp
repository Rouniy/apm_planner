#include <QtTest>

#include "ui/configuration/ConfigCompassViewModel.h"

#include <QBuffer>
#include <QSignalSpy>
#include <QtMath>

#include <algorithm>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml = QByteArrayLiteral(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:COMPASS_EXTERNAL" humanName="Compass 1 External">
          <values><value code="0">Internal</value><value code="1">External</value></values>
        </param>
        <param name="ArduCopter:COMPASS_EXTERN2" humanName="Compass 2 External">
          <values><value code="0">Internal</value><value code="1">External</value></values>
        </param>
        <param name="ArduCopter:COMPASS_EXTERN3" humanName="Compass 3 External">
          <values><value code="0">Internal</value><value code="1">External</value></values>
        </param>
        <param name="ArduCopter:COMPASS_ORIENT" humanName="Compass 1 Orientation">
          <values><value code="0">None</value><value code="8">Roll 180</value></values>
        </param>
        <param name="ArduCopter:COMPASS_ORIENT2" humanName="Compass 2 Orientation">
          <values><value code="0">None</value><value code="2">Yaw 90</value></values>
        </param>
        <param name="ArduCopter:COMPASS_ORIENT3" humanName="Compass 3 Orientation">
          <values><value code="0">None</value><value code="2">Yaw 90</value></values>
        </param>
        <param name="ArduCopter:COMPASS_PRIMARY" humanName="Primary Compass">
          <values><value code="0">Compass 1</value><value code="1">Compass 2</value></values>
        </param>
        <param name="ArduCopter:COMPASS_AUTODEC" humanName="Automatic Declination">
          <values><value code="0">Disabled</value><value code="1">Enabled</value></values>
        </param>
        <param name="ArduCopter:COMPASS_CAL_FIT" humanName="Calibration Fitness">
          <field name="Range">5 25</field><field name="Increment">0.1</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> compassSnapshot(int component = 5)
{
    return {
        {component, QStringLiteral("COMPASS_PRIO1_ID"), 200u},
        {component, QStringLiteral("COMPASS_PRIO2_ID"), 999u},
        {component, QStringLiteral("COMPASS_PRIO3_ID"), 100u},
        {component, QStringLiteral("COMPASS_DEV_ID"), 100u},
        {component, QStringLiteral("COMPASS_DEV_ID2"), 200u},
        // Same physical device under another discovery parameter: one row.
        {component, QStringLiteral("COMPASS_DEV_ID3"), 100u},
        // Preserve the raw uint32 pattern and MP10 signed display.
        {component, QStringLiteral("COMPASS_DEV_ID4"), -1},
        {component, QStringLiteral("COMPASS_EXTERNAL"), 1},
        {component, QStringLiteral("COMPASS_EXTERN2"), 0},
        {component, QStringLiteral("COMPASS_EXTERN3"), 1},
        {component, QStringLiteral("COMPASS_ORIENT"), 8},
        {component, QStringLiteral("COMPASS_ORIENT2"), 2},
        {component, QStringLiteral("COMPASS_ORIENT3"), 3},
        {component, QStringLiteral("COMPASS_USE"), 1},
        // Exercise the second physical-slot alias used by newer firmware.
        {component, QStringLiteral("COMPASS2_USE"), 0},
        {component, QStringLiteral("COMPASS_USE3"), 1},
        {component, QStringLiteral("COMPASS_LEARN"), 0},
        {component, QStringLiteral("COMPASS_DEC"), qDegreesToRadians(10.0)},
        {component, QStringLiteral("COMPASS_PRIMARY"), 0},
        {component, QStringLiteral("COMPASS_AUTODEC"), 1},
        {component, QStringLiteral("COMPASS_CAL_FIT"), 8.0}
    };
}

ParamField fieldNamed(const ConfigCompassViewModel &model,
                      const QString &name)
{
    for (const ParamField &field : model.Fields()) {
        if (field.name == name) {
            return field;
        }
    }
    return {};
}

QVariantList emittedChanges(const QSignalSpy &spy, int emission = -1)
{
    const int index = emission < 0 ? spy.count() - 1 : emission;
    return spy.at(index).at(2).toList();
}

QString changeName(const QVariant &change)
{
    return change.toMap().value(QStringLiteral("name")).toString();
}

QVariant changeValue(const QVariant &change)
{
    return change.toMap().value(QStringLiteral("value"));
}

void completeLastWrite(ConfigCompassViewModel &model,
                       const QSignalSpy &writes, qulonglong batchId)
{
    const QList<QVariant> emission = writes.constLast();
    const quint64 requestId = emission.at(0).toULongLong();
    const int count = emission.at(2).toList().size();
    model.parameterWriteSubmitted(requestId, batchId);
    model.parameterBatchCompleted(batchId, count, 0);
}
} // namespace

class ConfigCompassViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void columnsAndCompleteSnapshotGate();
    void exactComponentBuildsPriorityMissingAliasesAndDedupe();
    void duplicatePrioritySlotsRemainVisible();
    void advancedFieldsUseMetadataAndSafeFallbacks();
    void incompletePrioritySchemaRejectsPriorityEditing();
    void writesRequireConnectedDisarmedCompleteState();
    void perItemFailureWaitsForBatchThenRequiresReconciliation();
    void partialPriorityBatchKeepsRebootRequired();
    void submissionRollbackAndDisconnectUncertainty();
    void controlsDeclinationAndQuickPixhawkUseExactBatches();
    void priorityRemovalAndRebootAcknowledgementContract();
};

void ConfigCompassViewModelTest::columnsAndCompleteSnapshotGate()
{
    ConfigCompassViewModel model;
    QCOMPARE(model.columnCount(), 11);
    const QStringList headers = {
        QStringLiteral("Priority"), QStringLiteral("DevID"),
        QStringLiteral("BusType"), QStringLiteral("Bus"),
        QStringLiteral("Address"), QStringLiteral("DevType"),
        QStringLiteral("Missing"), QStringLiteral("External"),
        QStringLiteral("Orientation"), QStringLiteral("Up"),
        QStringLiteral("Down")};
    for (int column = 0; column < headers.size(); ++column) {
        QCOMPARE(model.headerData(column, Qt::Horizontal).toString(),
                 headers.at(column));
    }
    QCOMPARE(model.Fields().size(), 9);
    QVERIFY(!model.SnapshotComplete());
    QVERIFY(!model.Available());
    QVERIFY(!model.Status().isEmpty());

    model.setParameterSnapshot(compassSnapshot(), 5, false);
    QVERIFY(!model.SnapshotComplete());
    QVERIFY(!model.SnapshotReady());
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.Fields().size(), 9);
    for (const ParamField &field : model.Fields()) {
        QVERIFY(field.readOnly);
    }

    QList<ConfigFriendlyParameterValue> noPriority = compassSnapshot();
    for (auto iterator = noPriority.begin(); iterator != noPriority.end();) {
        if (iterator->name == QLatin1String("COMPASS_PRIO1_ID")) {
            iterator = noPriority.erase(iterator);
        } else {
            ++iterator;
        }
    }
    model.setParameterSnapshot(noPriority, 5, true);
    QVERIFY(model.SnapshotComplete());
    QVERIFY(!model.Available());
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(model.Status().contains(QStringLiteral("COMPASS_PRIO1_ID")));
}

void ConfigCompassViewModelTest::exactComponentBuildsPriorityMissingAliasesAndDedupe()
{
    ConfigCompassViewModel model;
    model.setCatalog(catalogFixture());
    QList<ConfigFriendlyParameterValue> values = compassSnapshot(5);
    values.append(compassSnapshot(1));
    model.setParameterSnapshot(values, 5, true);

    QCOMPARE(model.ComponentId(), 5);
    QVERIFY(model.Available());
    QCOMPARE(model.rowCount(), 4);
    const QVector<CompassPriorityRow> rows = model.Rows();
    QCOMPARE(rows.at(0).rawDevId, quint32(200));
    QCOMPARE(rows.at(0).physicalSlot, 2);
    QVERIFY(!rows.at(0).missing);
    QVERIFY(!rows.at(0).external);
    QCOMPARE(rows.at(0).orientation, QStringLiteral("Yaw 90"));

    QCOMPARE(rows.at(1).rawDevId, quint32(999));
    QVERIFY(rows.at(1).missing);
    QCOMPARE(rows.at(1).physicalSlot, 0);
    QCOMPARE(rows.at(1).orientation, QStringLiteral("n/a"));

    QCOMPARE(rows.at(2).rawDevId, quint32(100));
    QCOMPARE(rows.at(2).physicalSlot, 1);
    QVERIFY(rows.at(2).external);
    QCOMPARE(rows.at(2).orientation, QStringLiteral("Roll 180"));

    QCOMPARE(rows.at(3).rawDevId, quint32(0xffffffffu));
    QCOMPARE(rows.at(3).devId, qint32(-1));
    QCOMPARE(rows.at(3).physicalSlot, 4);
    QCOMPARE(model.data(model.index(3, ConfigCompassViewModel::DevIDColumn))
                 .toString(), QStringLiteral("-1"));
    QVERIFY(!model.CompassStatus().isEmpty());

    QVERIFY(model.HasUseCompass(1));
    QVERIFY(model.HasUseCompass(2));
    QVERIFY(model.HasUseCompass(3));
    QVERIFY(model.UseCompass(1));
    QVERIFY(!model.UseCompass(2));
    QVERIFY(model.UseCompass(3));
    QVERIFY(model.HasLearn());
    QVERIFY(!model.LearnOffsets());
    QVERIFY(model.HasDeclination());
    QVERIFY(qAbs(model.DeclinationDegrees() - 10.0) < 1.0e-9);
}

void ConfigCompassViewModelTest::duplicatePrioritySlotsRemainVisible()
{
    QList<ConfigFriendlyParameterValue> values = compassSnapshot();
    for (ConfigFriendlyParameterValue &parameter : values) {
        if (parameter.name == QLatin1String("COMPASS_PRIO2_ID")) {
            parameter.value = 200u;
        }
    }

    ConfigCompassViewModel model;
    model.setParameterSnapshot(values, 5, true);
    model.setConnected(true);
    const QVector<CompassPriorityRow> rows = model.Rows();
    QCOMPARE(rows.size(), 4);
    QCOMPARE(rows.at(0).rawDevId, quint32(200));
    QCOMPARE(rows.at(1).rawDevId, quint32(200));
    QCOMPARE(rows.at(0).priority, 1);
    QCOMPARE(rows.at(1).priority, 2);

    QSignalSpy writes(&model, &ConfigCompassViewModel::writeRequested);
    QVERIFY(model.moveDown(1));
    const QVariantList changes = emittedChanges(writes);
    QCOMPARE(changes.size(), 3);
    QCOMPARE(changeValue(changes.at(0)).toUInt(), quint32(200));
    QCOMPARE(changeValue(changes.at(1)).toUInt(), quint32(100));
    QCOMPARE(changeValue(changes.at(2)).toUInt(), quint32(200));
}

void ConfigCompassViewModelTest::advancedFieldsUseMetadataAndSafeFallbacks()
{
    ConfigCompassViewModel model;
    QSignalSpy writes(&model, &ConfigCompassViewModel::writeRequested);
    model.setCatalog(catalogFixture(), true);
    model.setParameterSnapshot(compassSnapshot(), 5, true);

    QCOMPARE(ConfigCompassViewModel::AdvancedFieldNames().size(), 9);
    QCOMPARE(model.Fields().size(), 9);
    QCOMPARE(model.Fields().constFirst().name,
             QStringLiteral("COMPASS_EXTERNAL"));
    QCOMPARE(model.Fields().constLast().name,
             QStringLiteral("COMPASS_CAL_FIT"));

    const ParamField external =
        fieldNamed(model, QStringLiteral("COMPASS_EXTERNAL"));
    QCOMPARE(external.label, QStringLiteral("Compass 1 External"));
    QCOMPARE(external.editorKind, ParamField::EditorKind::Combo);
    QCOMPARE(external.options.size(), 2);
    const ParamField fit =
        fieldNamed(model, QStringLiteral("COMPASS_CAL_FIT"));
    QCOMPARE(fit.editorKind, ParamField::EditorKind::Numeric);
    QVERIFY(fit.hasRange);
    QVERIFY(fit.enforceRange);
    QCOMPARE(fit.minimum, 5.0);
    QCOMPARE(fit.maximum, 25.0);

    // Value 3 is newer than our ORIENT3 metadata but remains selected; merely
    // hydrating the field must not emit a corrective write.
    const ParamField unknown =
        fieldNamed(model, QStringLiteral("COMPASS_ORIENT3"));
    QCOMPARE(unknown.editorKind, ParamField::EditorKind::Combo);
    QVERIFY(std::any_of(unknown.options.constBegin(), unknown.options.constEnd(),
                        [](const ParamOption &option) {
        return option.value.toInt() == 3;
    }));
    QCOMPARE(writes.count(), 0);

    model.setConnected(true);
    QVERIFY(!model.setFieldValue(QStringLiteral("COMPASS_CAL_FIT"), 30.0));
    QCOMPARE(writes.count(), 0);

    ConfigCompassViewModel fallback;
    fallback.setParameterSnapshot(compassSnapshot(), 5, true);
    const ParamField fallbackExternal =
        fieldNamed(fallback, QStringLiteral("COMPASS_EXTERNAL"));
    const ParamField fallbackOrientation =
        fieldNamed(fallback, QStringLiteral("COMPASS_ORIENT"));
    QCOMPARE(fallbackExternal.editorKind, ParamField::EditorKind::Combo);
    QCOMPARE(fallbackExternal.options.size(), 2);
    QCOMPARE(fallbackOrientation.editorKind, ParamField::EditorKind::Numeric);
    QCOMPARE(fallbackOrientation.value.toInt(), 8);
}

void ConfigCompassViewModelTest::incompletePrioritySchemaRejectsPriorityEditing()
{
    QList<ConfigFriendlyParameterValue> values = compassSnapshot();
    for (auto iterator = values.begin(); iterator != values.end();) {
        if (iterator->name == QLatin1String("COMPASS_PRIO3_ID")) {
            iterator = values.erase(iterator);
        } else {
            ++iterator;
        }
    }

    ConfigCompassViewModel model;
    model.setParameterSnapshot(values, 5, true);
    model.setConnected(true);
    QSignalSpy writes(&model, &ConfigCompassViewModel::writeRequested);
    QVERIFY(model.Available()); // PRIO1 is still the page capability gate.
    QVERIFY(!model.moveDown(0));
    QVERIFY(!model.removeMissing());
    QCOMPARE(writes.count(), 0);
    QVERIFY(model.Status().contains(QStringLiteral("three"),
                                    Qt::CaseInsensitive));
}

void ConfigCompassViewModelTest::writesRequireConnectedDisarmedCompleteState()
{
    ConfigCompassViewModel model;
    QSignalSpy writes(&model, &ConfigCompassViewModel::writeRequested);
    model.setParameterSnapshot(compassSnapshot(), 5, true);

    QVERIFY(!model.setUseCompass(2, true));
    model.setConnected(true);
    model.setArmed(true);
    QVERIFY(!model.setUseCompass(2, true));
    model.setArmed(false);
    QVERIFY(model.setUseCompass(2, true));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(1).toInt(), 5);
    const QVariantList changes = emittedChanges(writes);
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changeName(changes.constFirst()), QStringLiteral("COMPASS2_USE"));
    QCOMPARE(changeValue(changes.constFirst()).toInt(), 1);
    QVERIFY(model.UseCompass(2)); // optimistic

    // Foreign component/raw echoes and foreign terminal events do not own it.
    model.parameterChanged(1, QStringLiteral("COMPASS2_USE"), 0);
    model.parameterChanged(5, QStringLiteral("COMPASS2_USE"), 0);
    model.parameterWriteSubmitted(9999, 20);
    model.parameterBatchCompleted(20, 1, 0);
    QVERIFY(model.HasPendingWrites());

    model.parameterWriteSubmissionFailed(
        writes.at(0).at(0).toULongLong(), QStringLiteral("rejected"));
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(!model.UseCompass(2));

    ConfigCompassViewModel partial;
    partial.setConnected(true);
    partial.setParameterSnapshot(compassSnapshot(), 5, false);
    QVERIFY(!partial.setLearnOffsets(true));
}

void ConfigCompassViewModelTest::perItemFailureWaitsForBatchThenRequiresReconciliation()
{
    ConfigCompassViewModel model;
    model.setParameterSnapshot(compassSnapshot(), 5, true);
    model.setConnected(true);
    QSignalSpy writes(&model, &ConfigCompassViewModel::writeRequested);
    QSignalSpy refreshes(&model, &ConfigCompassViewModel::refreshRequested);

    QVERIFY(model.quickPixhawk());
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    QCOMPARE(emittedChanges(writes).size(), 2);
    model.parameterWriteSubmitted(requestId, 41);
    model.parameterWriteFailed(41, 5, QStringLiteral("COMPASS_EXTERNAL"),
                               QStringLiteral("denied"));
    QVERIFY(model.HasPendingWrites());
    QVERIFY(!model.ReconciliationRequired());
    QCOMPARE(model.Rows().at(2).external, false); // optimistic remains stable

    model.parameterBatchCompleted(41, 1, 1);
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(model.ReconciliationRequired());
    QVERIFY(!model.SnapshotReady());
    QVERIFY(!model.SnapshotComplete());
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(refreshes.count(), 1);
    QCOMPARE(refreshes.at(0).at(0).toInt(), 5);
    for (const ParamField &field : model.Fields()) {
        QVERIFY(field.readOnly);
    }
    QVERIFY(!model.setLearnOffsets(true));

    // A partial refresh cannot clear uncertainty; a complete one can.
    model.setParameterSnapshot(compassSnapshot(), 5, false);
    QVERIFY(model.ReconciliationRequired());
    model.setParameterSnapshot(compassSnapshot(), 5, true);
    QVERIFY(!model.ReconciliationRequired());
    QVERIFY(model.SnapshotReady());
    QCOMPARE(model.rowCount(), 4);

    QVERIFY(model.setLearnOffsets(true));
    const quint64 cancelRequest = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmitted(cancelRequest, 42);
    model.parameterWriteCancelled(42, 5, QStringLiteral("COMPASS_LEARN"));
    QVERIFY(model.HasPendingWrites());
    model.parameterBatchCompleted(42, 0, 1);
    QVERIFY(model.ReconciliationRequired());
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(refreshes.count(), 2);
}

void ConfigCompassViewModelTest::partialPriorityBatchKeepsRebootRequired()
{
    ConfigCompassViewModel model;
    model.setParameterSnapshot(compassSnapshot(), 5, true);
    model.setConnected(true);
    QSignalSpy writes(&model, &ConfigCompassViewModel::writeRequested);

    QVERIFY(model.moveDown(0));
    const quint64 requestId = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 46);
    QVERIFY(model.RebootRequired());
    model.parameterWriteFailed(46, 5, QStringLiteral("COMPASS_PRIO2_ID"),
                               QStringLiteral("denied"));
    model.parameterBatchCompleted(46, 2, 1);
    QVERIFY(model.ReconciliationRequired());
    QVERIFY(model.RebootRequired());

    model.setParameterSnapshot(compassSnapshot(), 5, true);
    QVERIFY(!model.ReconciliationRequired());
    QVERIFY(model.RebootRequired());
}

void ConfigCompassViewModelTest::submissionRollbackAndDisconnectUncertainty()
{
    ConfigCompassViewModel model;
    model.setParameterSnapshot(compassSnapshot(), 5, true);
    model.setConnected(true);
    QSignalSpy writes(&model, &ConfigCompassViewModel::writeRequested);
    QSignalSpy refreshes(&model, &ConfigCompassViewModel::refreshRequested);

    QVERIFY(model.setUseCompass(2, true));
    QVERIFY(model.UseCompass(2));
    model.setConnected(false); // never submitted: exact rollback is safe
    QVERIFY(!model.UseCompass(2));
    QVERIFY(!model.ReconciliationRequired());
    QCOMPARE(refreshes.count(), 0);

    model.setConnected(true);
    QVERIFY(model.setUseCompass(2, true));
    const quint64 requestId = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 55);
    model.setConnected(false); // submitted outcome is now unknowable
    QVERIFY(model.ReconciliationRequired());
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(refreshes.count(), 1);
    QCOMPARE(refreshes.at(0).at(0).toInt(), 5);
}

void ConfigCompassViewModelTest::controlsDeclinationAndQuickPixhawkUseExactBatches()
{
    ConfigCompassViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(compassSnapshot(), 5, true);
    model.setConnected(true);
    QSignalSpy writes(&model, &ConfigCompassViewModel::writeRequested);

    QVERIFY(model.setUseCompass(2, true));
    completeLastWrite(model, writes, 61);
    QVERIFY(model.UseCompass(2));

    QVERIFY(model.setLearnOffsets(true));
    QCOMPARE(changeName(emittedChanges(writes).constFirst()),
             QStringLiteral("COMPASS_LEARN"));
    completeLastWrite(model, writes, 62);
    QVERIFY(model.LearnOffsets());

    QVERIFY(model.setFieldValue(QStringLiteral("COMPASS_PRIMARY"), 1));
    QCOMPARE(changeName(emittedChanges(writes).constFirst()),
             QStringLiteral("COMPASS_PRIMARY"));
    completeLastWrite(model, writes, 63);

    QVERIFY(model.writeDeclinationDegrees(-12.5));
    const QVariant declination = emittedChanges(writes).constFirst();
    QCOMPARE(changeName(declination), QStringLiteral("COMPASS_DEC"));
    QVERIFY(qAbs(changeValue(declination).toDouble()
                 - qDegreesToRadians(-12.5)) < 1.0e-12);
    completeLastWrite(model, writes, 64);
    QVERIFY(qAbs(model.DeclinationDegrees() + 12.5) < 1.0e-9);

    QVERIFY(model.quickPixhawk());
    const QVariantList quick = emittedChanges(writes);
    QCOMPARE(quick.size(), 2);
    QCOMPARE(changeName(quick.at(0)), QStringLiteral("COMPASS_EXTERNAL"));
    QCOMPARE(changeValue(quick.at(0)).toInt(), 0);
    QCOMPARE(changeName(quick.at(1)), QStringLiteral("COMPASS_ORIENT"));
    QCOMPARE(changeValue(quick.at(1)).toInt(), 0);
    QCOMPARE(writes.constLast().at(1).toInt(), 5);
    completeLastWrite(model, writes, 65);
    QCOMPARE(fieldNamed(model, QStringLiteral("COMPASS_EXTERNAL"))
                 .value.toInt(), 0);
    QCOMPARE(fieldNamed(model, QStringLiteral("COMPASS_ORIENT"))
                 .value.toInt(), 0);
}

void ConfigCompassViewModelTest::priorityRemovalAndRebootAcknowledgementContract()
{
    ConfigCompassViewModel model;
    model.setParameterSnapshot(compassSnapshot(), 5, true);
    model.setConnected(true);
    QSignalSpy writes(&model, &ConfigCompassViewModel::writeRequested);
    QSignalSpy reboots(&model, &ConfigCompassViewModel::rebootRequested);

    QVERIFY(model.moveDown(0));
    QVariantList changes = emittedChanges(writes);
    QCOMPARE(changes.size(), 3);
    QCOMPARE(changeName(changes.at(0)), QStringLiteral("COMPASS_PRIO1_ID"));
    QCOMPARE(changeValue(changes.at(0)).toUInt(), quint32(999));
    QCOMPARE(changeValue(changes.at(1)).toUInt(), quint32(200));
    QCOMPARE(model.Rows().at(0).rawDevId, quint32(999));
    completeLastWrite(model, writes, 71);
    QVERIFY(model.RebootRequired());

    QVERIFY(model.removeMissing());
    changes = emittedChanges(writes);
    QCOMPARE(changeValue(changes.at(0)).toUInt(), quint32(200));
    QCOMPARE(changeValue(changes.at(1)).toUInt(), quint32(100));
    // Match MP10 UpdateFirst3(): after the missing priority is removed, the
    // next discovered (previously unprioritized) device is promoted.  Keep
    // its complete uint32 bit pattern even though MP10 displays it as -1.
    QCOMPARE(changeValue(changes.at(2)).toUInt(), quint32(0xffffffffu));
    completeLastWrite(model, writes, 72);
    QCOMPARE(model.rowCount(), 3);
    QVERIFY(model.CompassStatus().isEmpty());

    QVERIFY(model.Reboot());
    QCOMPARE(reboots.count(), 1);
    const quint64 firstRequest = reboots.at(0).at(0).toULongLong();
    QVERIFY(model.RebootPending());
    model.rebootSubmitted(firstRequest + 1);
    model.rebootAcknowledged(firstRequest, true);
    QVERIFY(model.RebootPending()); // ACK before exact submission is ignored
    model.rebootSubmitted(firstRequest);
    model.rebootAcknowledged(firstRequest + 1, true);
    QVERIFY(model.RebootPending());
    model.rebootAcknowledged(firstRequest, false,
                             QStringLiteral("command denied"));
    QVERIFY(!model.RebootPending());
    QVERIFY(model.RebootRequired());

    QVERIFY(model.Reboot());
    const quint64 secondRequest = reboots.at(1).at(0).toULongLong();
    QVERIFY(secondRequest != firstRequest);
    model.rebootSubmitted(secondRequest);
    model.rebootAcknowledged(secondRequest, true);
    QVERIFY(!model.RebootPending());
    QVERIFY(!model.RebootRequired());
    // MP10 keeps manual reboot available even when no priority change is
    // latched; it still uses the same confirmation/ACK contract.
    QVERIFY(model.Reboot());
    const quint64 manualRequest = reboots.constLast().at(0).toULongLong();
    model.rebootSubmitted(manualRequest);
    model.rebootAcknowledged(manualRequest, true);
    QVERIFY(!model.RebootPending());
    QVERIFY(!model.RebootRequired());

    // A submitted command without a correlated terminal result is ambiguous:
    // retrying the same MAV_CMD could let the first command's late ACK confirm
    // the second attempt because MAVLink has no per-send transaction token.
    QVERIFY(model.moveDown(0));
    completeLastWrite(model, writes, 73);
    QVERIFY(model.Reboot());
    const quint64 ambiguousRequest = reboots.constLast().at(0).toULongLong();
    model.rebootSubmitted(ambiguousRequest);
    model.rebootCancelled(ambiguousRequest,
                          QStringLiteral("acknowledgement lost"));
    QVERIFY(model.RebootOutcomeUncertain());
    QVERIFY(model.RebootRequired());
    const int rebootCount = reboots.count();
    QVERIFY(!model.Reboot());
    QCOMPARE(reboots.count(), rebootCount);
    model.rebootAcknowledged(ambiguousRequest, true); // late old ACK
    QVERIFY(model.RebootRequired());
}

QTEST_MAIN(ConfigCompassViewModelTest)
#include "test_configcompassviewmodel.moc"
