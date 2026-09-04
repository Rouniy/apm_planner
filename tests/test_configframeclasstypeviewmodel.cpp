#include <QtTest>

#include "ui/configuration/ConfigFrameClassTypeViewModel.h"

#include <QSet>
#include <QSignalSpy>
#include <QVariantMap>

namespace {
QList<ConfigFriendlyParameterValue> snapshot(
    int component = 1, int frameClass = 1, int frameType = 1)
{
    return {{component, QStringLiteral("FRAME_CLASS"), frameClass},
            {component, QStringLiteral("FRAME_TYPE"), frameType}};
}

QVariantList emittedChanges(const QSignalSpy &spy, int emission = 0)
{
    return spy.at(emission).at(2).toList();
}

QVariantMap changeAt(const QVariantList &changes, int index)
{
    return changes.at(index).toMap();
}

QList<int> optionValues(const QList<ParamOption> &options)
{
    QList<int> result;
    for (const ParamOption &option : options) {
        result.append(option.value.toInt());
    }
    return result;
}
} // namespace

class ConfigFrameClassTypeViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactMp10CatalogAndOrder();
    void snapshotRequiresOneCompleteComponent();
    void hydrationNeverWritesAndUnknownTypeCanBeCorrected();
    void classChangeUsesOneOrTwoItemBatch();
    void classWithoutSubtypeWritesOnlyClass();
    void exactBatchOwnsTerminalLifecycleAndRollback();
    void partialTwoItemBatchRequiresReconciliation();
    void armedBlocksWritesButNotRefresh();
};

void ConfigFrameClassTypeViewModelTest::exactMp10CatalogAndOrder()
{
    const QList<FrameClassTypeEntry> valid =
        ConfigFrameClassTypeViewModel::ValidList();
    QCOMPARE(valid.size(), 29);

    ConfigFrameClassTypeViewModel model;
    const QList<ParamOption> classes = model.ClassOptions();
    QCOMPARE(classes.size(), 14);
    QCOMPARE(optionValues(classes),
             QList<int>({1, 2, 3, 4, 12, 5, 6, 7,
                         8, 9, 10, 11, 13, 0}));
    QCOMPARE(classes.at(4).text, QStringLiteral("DODECAHEXA"));
    QCOMPARE(classes.constLast().text, QStringLiteral("UNDEFINED"));

    QHash<int, QList<int>> expected{
        {1, {0, 1, 2, 3, 4, 5}}, {2, {0, 1}},
        {3, {0, 1, 2, 3}}, {4, {0, 1, 2, 3}},
        {12, {0, 1}}, {5, {10, 1}},
        {6, {}}, {7, {}}, {8, {}}, {9, {}},
        {10, {}}, {11, {}}, {13, {}}, {0, {}}
    };
    QHash<int, QList<int>> actual;
    for (const FrameClassTypeEntry &entry : valid) {
        if (entry.frameType.isValid()
            && !actual[entry.frameClass].contains(entry.frameType.toInt())) {
            actual[entry.frameClass].append(entry.frameType.toInt());
        } else if (!actual.contains(entry.frameClass)) {
            actual.insert(entry.frameClass, {});
        }
    }
    QVERIFY(actual == expected);
}

void ConfigFrameClassTypeViewModelTest::snapshotRequiresOneCompleteComponent()
{
    ConfigFrameClassTypeViewModel model;
    QSignalSpy writes(&model,
                      &ConfigFrameClassTypeViewModel::writeRequested);
    model.setParameterSnapshot({
        {7, QStringLiteral("FRAME_CLASS"), 1},
        {8, QStringLiteral("FRAME_TYPE"), 1}
    }, 7);
    QVERIFY(!model.SnapshotReady());
    QCOMPARE(model.ComponentId(), 7);
    QVERIFY(model.Status().contains(QStringLiteral("not present")));

    QList<ConfigFriendlyParameterValue> values = snapshot(42, 4, 3);
    values.append({1, QStringLiteral("FRAME_CLASS"), 2});
    model.setParameterSnapshot(values, 42);
    QVERIFY(model.SnapshotReady());
    QCOMPARE(model.ComponentId(), 42);
    QCOMPARE(model.SelectedClass().toInt(), 4);
    QCOMPARE(model.SelectedType().toInt(), 3);
    QCOMPARE(writes.count(), 0);
}

void ConfigFrameClassTypeViewModelTest::hydrationNeverWritesAndUnknownTypeCanBeCorrected()
{
    ConfigFrameClassTypeViewModel model;
    QSignalSpy writes(&model,
                      &ConfigFrameClassTypeViewModel::writeRequested);
    model.setParameterSnapshot(snapshot(1, 1, 99));
    model.setConnected(true);
    QCOMPARE(writes.count(), 0);
    QCOMPARE(model.SelectedType().toInt(), 99);
    QCOMPARE(optionValues(model.TypeOptions()),
             QList<int>({0, 1, 2, 3, 4, 5}));

    QVERIFY(model.selectType(0));
    QCOMPARE(writes.count(), 1);
    const QVariantList changes = emittedChanges(writes);
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changeAt(changes, 0).value(QStringLiteral("name")).toString(),
             QStringLiteral("FRAME_TYPE"));
    QCOMPARE(changeAt(changes, 0).value(QStringLiteral("value")).toInt(), 0);
    model.parameterWriteSubmissionFailed(
        writes.at(0).at(0).toULongLong(), QStringLiteral("target changed"));
    QCOMPARE(model.SelectedType().toInt(), 99);
    QVERIFY(model.selectType(0));
    QCOMPARE(writes.count(), 2);
}

void ConfigFrameClassTypeViewModelTest::classChangeUsesOneOrTwoItemBatch()
{
    ConfigFrameClassTypeViewModel retained;
    retained.setParameterSnapshot(snapshot(1, 1, 1));
    retained.setConnected(true);
    QSignalSpy retainedWrites(
        &retained, &ConfigFrameClassTypeViewModel::writeRequested);
    QVERIFY(retained.selectClass(2));
    QVariantList changes = emittedChanges(retainedWrites);
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changeAt(changes, 0).value(QStringLiteral("name")).toString(),
             QStringLiteral("FRAME_CLASS"));
    QCOMPARE(retained.SelectedType().toInt(), 1);

    ConfigFrameClassTypeViewModel replaced;
    replaced.setParameterSnapshot(snapshot(1, 1, 4));
    replaced.setConnected(true);
    QSignalSpy replacedWrites(
        &replaced, &ConfigFrameClassTypeViewModel::writeRequested);
    QVERIFY(replaced.selectClass(2));
    changes = emittedChanges(replacedWrites);
    QCOMPARE(changes.size(), 2);
    QCOMPARE(changeAt(changes, 0).value(QStringLiteral("name")).toString(),
             QStringLiteral("FRAME_CLASS"));
    QCOMPARE(changeAt(changes, 0).value(QStringLiteral("value")).toInt(), 2);
    QCOMPARE(changeAt(changes, 1).value(QStringLiteral("name")).toString(),
             QStringLiteral("FRAME_TYPE"));
    QCOMPARE(changeAt(changes, 1).value(QStringLiteral("value")).toInt(), 0);
    QCOMPARE(replaced.SelectedType().toInt(), 0);
}

void ConfigFrameClassTypeViewModelTest::classWithoutSubtypeWritesOnlyClass()
{
    ConfigFrameClassTypeViewModel model;
    model.setParameterSnapshot(snapshot());
    model.setConnected(true);
    QSignalSpy writes(&model,
                      &ConfigFrameClassTypeViewModel::writeRequested);

    QVERIFY(model.selectClass(6));
    const QVariantList changes = emittedChanges(writes);
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changeAt(changes, 0).value(QStringLiteral("name")).toString(),
             QStringLiteral("FRAME_CLASS"));
    QVERIFY(!model.TypeEnabled());
    QVERIFY(!model.SelectedType().isValid());
}

void ConfigFrameClassTypeViewModelTest::exactBatchOwnsTerminalLifecycleAndRollback()
{
    ConfigFrameClassTypeViewModel model;
    model.setParameterSnapshot(snapshot(5, 1, 1), 5);
    model.setConnected(true);
    QSignalSpy writes(&model,
                      &ConfigFrameClassTypeViewModel::writeRequested);

    QVERIFY(model.selectType(2));
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    model.parameterChanged(5, QStringLiteral("FRAME_TYPE"), 2);
    QVERIFY(model.HasPendingWrites());
    model.parameterWriteSubmitted(requestId + 1, 70);
    model.parameterWriteSubmitted(requestId, 71);
    model.parameterBatchCompleted(70, 1, 0);
    model.parameterWriteFailed(71, 6, QStringLiteral("FRAME_TYPE"),
                               QStringLiteral("wrong component"));
    model.parameterWriteFailed(71, 5, QStringLiteral("FRAME_CLASS"),
                               QStringLiteral("wrong parameter"));
    QVERIFY(model.HasPendingWrites());
    QCOMPARE(model.SelectedType().toInt(), 2);
    model.parameterWriteCancelled(
        71, 5, QStringLiteral("FRAME_TYPE"));
    QVERIFY(!model.HasPendingWrites());
    QCOMPARE(model.SelectedType().toInt(), 1);
    QVERIFY(model.Status().contains(QStringLiteral("cancelled")));

    QVERIFY(model.selectType(3));
    const quint64 secondRequest = writes.at(1).at(0).toULongLong();
    model.parameterWriteSubmitted(secondRequest, 72);
    model.parameterBatchCompleted(72, 1, 0);
    QVERIFY(!model.HasPendingWrites());
    QCOMPARE(model.SelectedType().toInt(), 3);

    QVERIFY(model.selectType(0));
    model.parameterWriteSubmissionFailed(
        writes.at(2).at(0).toULongLong(), QStringLiteral("target changed"));
    QCOMPARE(model.SelectedType().toInt(), 3);
}

void ConfigFrameClassTypeViewModelTest::partialTwoItemBatchRequiresReconciliation()
{
    ConfigFrameClassTypeViewModel model;
    model.setParameterSnapshot(snapshot(5, 1, 4), 5);
    model.setConnected(true);
    QSignalSpy writes(&model,
                      &ConfigFrameClassTypeViewModel::writeRequested);
    QSignalSpy refreshes(&model,
                         &ConfigFrameClassTypeViewModel::refreshRequested);

    QVERIFY(model.selectClass(2));
    QCOMPARE(emittedChanges(writes).size(), 2);
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 91);

    // ParameterService applies batch items sequentially. FRAME_CLASS may have
    // succeeded before FRAME_TYPE reports failure, so the optimistic pair must
    // stay pending until the exact terminal batch result arrives.
    model.parameterChanged(5, QStringLiteral("FRAME_CLASS"), 2);
    model.parameterWriteFailed(91, 5, QStringLiteral("FRAME_TYPE"),
                               QStringLiteral("denied"));
    QVERIFY(model.HasPendingWrites());
    QCOMPARE(model.SelectedClass().toInt(), 2);

    model.parameterBatchCompleted(91, 1, 1);
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(model.ReconciliationRequired());
    QVERIFY(!model.SnapshotReady());
    QVERIFY(!model.SelectedClass().isValid());
    QVERIFY(!model.SelectedType().isValid());
    QCOMPARE(refreshes.count(), 1);
    QVERIFY(!model.selectClass(1));

    model.refreshFailed(QStringLiteral("telemetry timeout"));
    QVERIFY(model.ReconciliationRequired());
    QVERIFY(model.Status().contains(QStringLiteral("uncertain")));

    // A complete refreshed snapshot is the only operation that clears the
    // uncertainty. Preserve an incompatible actual subtype so it can then be
    // corrected explicitly.
    model.setParameterSnapshot(snapshot(5, 2, 4), 5);
    QVERIFY(!model.ReconciliationRequired());
    QVERIFY(model.SnapshotReady());
    QCOMPARE(model.SelectedClass().toInt(), 2);
    QCOMPARE(model.SelectedType().toInt(), 4);
    QVERIFY(model.selectType(0));

    ConfigFrameClassTypeViewModel disconnected;
    disconnected.setParameterSnapshot(snapshot(5, 1, 4), 5);
    disconnected.setConnected(true);
    QSignalSpy disconnectedWrites(
        &disconnected, &ConfigFrameClassTypeViewModel::writeRequested);
    QVERIFY(disconnected.selectClass(2));
    disconnected.parameterWriteSubmitted(
        disconnectedWrites.at(0).at(0).toULongLong(), 92);
    disconnected.setConnected(false);
    QVERIFY(disconnected.ReconciliationRequired());
    QVERIFY(disconnected.Status().contains(QStringLiteral("uncertain")));
    disconnected.setConnected(true);
    QVERIFY(disconnected.Status().contains(QStringLiteral("uncertain")));
}

void ConfigFrameClassTypeViewModelTest::armedBlocksWritesButNotRefresh()
{
    ConfigFrameClassTypeViewModel model;
    model.setParameterSnapshot(snapshot());
    model.setConnected(true);
    model.setArmed(true);
    QSignalSpy writes(&model,
                      &ConfigFrameClassTypeViewModel::writeRequested);
    QSignalSpy refreshes(&model,
                         &ConfigFrameClassTypeViewModel::refreshRequested);
    QVERIFY(!model.selectClass(2));
    QVERIFY(!model.selectType(0));
    QCOMPARE(writes.count(), 0);
    QVERIFY(model.Refresh());
    QCOMPARE(refreshes.count(), 1);
    model.setArmed(false);
    QVERIFY(model.Status().isEmpty());

    ConfigFrameClassTypeViewModel missing;
    missing.setParameterSnapshot({});
    missing.setConnected(true);
    QSignalSpy missingRefreshes(
        &missing, &ConfigFrameClassTypeViewModel::refreshRequested);
    QVERIFY(missing.Refresh());
    QCOMPARE(missingRefreshes.count(), 1);
    missing.setArmed(true);
    missing.setArmed(false);
    QVERIFY(missing.Status().contains(QStringLiteral("not present")));
}

QTEST_APPLESS_MAIN(ConfigFrameClassTypeViewModelTest)
#include "test_configframeclasstypeviewmodel.moc"
