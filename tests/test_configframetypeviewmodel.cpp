#include <QtTest>

#include "ui/configuration/ConfigFrameTypeViewModel.h"

#include <QSignalSpy>
#include <QVariantMap>

namespace {
QList<ConfigFriendlyParameterValue> snapshot(
    int component = 1, int frame = 1)
{
    return {{component, QStringLiteral("FRAME"), frame}};
}
} // namespace

class ConfigFrameTypeViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactLegacyInventoryAndComponentSelection();
    void hydrationDoesNotWrite();
    void exactBatchOwnsTerminalLifecycleAndRollback();
    void armedBlocksWritesButNotRefresh();
};

void ConfigFrameTypeViewModelTest::exactLegacyInventoryAndComponentSelection()
{
    const QList<ParamOption> options = ConfigFrameTypeViewModel::FrameOptions();
    QCOMPARE(options.size(), 6);
    QCOMPARE(options.at(0).value.toInt(), 0);
    QCOMPARE(options.at(1).value.toInt(), 1);
    QCOMPARE(options.at(2).value.toInt(), 2);
    QCOMPARE(options.at(3).value.toInt(), 3);
    QCOMPARE(options.at(4).value.toInt(), 10);
    QCOMPARE(options.at(5).value.toInt(), 4);
    QCOMPARE(options.at(5).text, QStringLiteral("'V-Tail'"));

    ConfigFrameTypeViewModel model;
    model.setParameterSnapshot(snapshot(42, 10), 1);
    QVERIFY(model.SnapshotReady());
    QCOMPARE(model.ComponentId(), 42);
    QCOMPARE(model.SelectedFrame().toInt(), 10);
    model.setParameterSnapshot({}, 42);
    QVERIFY(!model.SnapshotReady());
    QVERIFY(model.Status().contains(QStringLiteral("not present")));
}

void ConfigFrameTypeViewModelTest::hydrationDoesNotWrite()
{
    ConfigFrameTypeViewModel model;
    QSignalSpy writes(&model, &ConfigFrameTypeViewModel::writeRequested);
    model.setParameterSnapshot(snapshot(1, 4));
    model.setConnected(true);
    QCOMPARE(model.SelectedFrame().toInt(), 4);
    QCOMPARE(writes.count(), 0);
}

void ConfigFrameTypeViewModelTest::exactBatchOwnsTerminalLifecycleAndRollback()
{
    ConfigFrameTypeViewModel model;
    model.setParameterSnapshot(snapshot(9, 1), 9);
    model.setConnected(true);
    QSignalSpy writes(&model, &ConfigFrameTypeViewModel::writeRequested);

    QVERIFY(model.selectFrame(10));
    QCOMPARE(writes.count(), 1);
    const QVariantList changes = writes.at(0).at(2).toList();
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changes.at(0).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("FRAME"));
    QCOMPARE(changes.at(0).toMap().value(QStringLiteral("value")).toInt(),
             10);
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    model.parameterChanged(9, QStringLiteral("FRAME"), 10);
    QVERIFY(model.HasPendingWrites());
    model.parameterWriteSubmitted(requestId, 81);
    model.parameterBatchCompleted(80, 1, 0);
    model.parameterWriteFailed(81, 8, QStringLiteral("FRAME"),
                               QStringLiteral("wrong component"));
    QVERIFY(model.HasPendingWrites());
    model.parameterWriteCancelled(81, 9, QStringLiteral("FRAME"));
    QVERIFY(!model.HasPendingWrites());
    QCOMPARE(model.SelectedFrame().toInt(), 1);

    QVERIFY(model.selectFrame(4));
    model.parameterWriteSubmitted(writes.at(1).at(0).toULongLong(), 82);
    model.parameterBatchCompleted(82, 1, 0);
    QCOMPARE(model.SelectedFrame().toInt(), 4);

    QVERIFY(model.selectFrame(0));
    model.parameterWriteSubmissionFailed(
        writes.at(2).at(0).toULongLong(), QStringLiteral("target changed"));
    QCOMPARE(model.SelectedFrame().toInt(), 4);
}

void ConfigFrameTypeViewModelTest::armedBlocksWritesButNotRefresh()
{
    ConfigFrameTypeViewModel model;
    model.setParameterSnapshot(snapshot());
    model.setConnected(true);
    model.setArmed(true);
    QSignalSpy writes(&model, &ConfigFrameTypeViewModel::writeRequested);
    QSignalSpy refreshes(&model, &ConfigFrameTypeViewModel::refreshRequested);
    QVERIFY(!model.selectFrame(2));
    QCOMPARE(writes.count(), 0);
    QVERIFY(model.Refresh());
    QCOMPARE(refreshes.count(), 1);
    model.setArmed(false);
    QVERIFY(model.Status().isEmpty());

    ConfigFrameTypeViewModel missing;
    missing.setParameterSnapshot({});
    missing.setConnected(true);
    missing.setArmed(true);
    missing.setArmed(false);
    QVERIFY(missing.Status().contains(QStringLiteral("not present")));
}

QTEST_APPLESS_MAIN(ConfigFrameTypeViewModelTest)
#include "test_configframetypeviewmodel.moc"
