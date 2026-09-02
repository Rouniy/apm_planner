#include <QtTest>

#include "ui/configuration/ConfigHWOSDView.h"
#include "ui/configuration/ConfigHWOSDViewModel.h"

#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QVariantMap>

namespace {

const QStringList kExpectedNames{
    QStringLiteral("SR0_EXT_STAT"), QStringLiteral("SR0_EXTRA1"),   QStringLiteral("SR0_EXTRA2"),
    QStringLiteral("SR0_EXTRA3"),   QStringLiteral("SR0_POSITION"), QStringLiteral("SR0_RAW_CTRL"),
    QStringLiteral("SR0_RAW_SENS"), QStringLiteral("SR0_RC_CHAN"),  QStringLiteral("SR1_EXT_STAT"),
    QStringLiteral("SR1_EXTRA1"),   QStringLiteral("SR1_EXTRA2"),   QStringLiteral("SR1_EXTRA3"),
    QStringLiteral("SR1_POSITION"), QStringLiteral("SR1_RAW_CTRL"), QStringLiteral("SR1_RAW_SENS"),
    QStringLiteral("SR1_RC_CHAN"),  QStringLiteral("SR3_EXT_STAT"), QStringLiteral("SR3_EXTRA1"),
    QStringLiteral("SR3_EXTRA2"),   QStringLiteral("SR3_EXTRA3"),   QStringLiteral("SR3_POSITION"),
    QStringLiteral("SR3_RAW_CTRL"), QStringLiteral("SR3_RAW_SENS"), QStringLiteral("SR3_RC_CHAN")};

ConfigFriendlyParameterValue value(const QString &name, int componentId = 1, int rate = 0)
{
    ConfigFriendlyParameterValue result;
    result.componentId = componentId;
    result.name = name;
    result.value = rate;
    return result;
}

QList<ConfigFriendlyParameterValue> fullSnapshot(int componentId = 1)
{
    QList<ConfigFriendlyParameterValue> parameters;
    // Reverse order plus distractors: the batch order must not depend on it.
    for (int index = kExpectedNames.size() - 1; index >= 0; --index) {
        parameters.append(value(kExpectedNames.at(index), componentId));
    }
    parameters.append(value(QStringLiteral("SR0_PARAMS"), componentId, 50));
    parameters.append(value(QStringLiteral("SR1_PARAMS"), componentId, 50));
    parameters.append(value(QStringLiteral("SR3_PARAMS"), componentId, 50));
    parameters.append(value(QStringLiteral("SR2_EXTRA1"), componentId));
    parameters.append(value(QStringLiteral("SR0_ADSB"), componentId));
    parameters.append(value(QStringLiteral("SYSID_THISMAV"), componentId));
    return parameters;
}

QStringList changeNames(const QVariantList &changes)
{
    QStringList names;
    for (const QVariant &change : changes) {
        names.append(change.toMap().value(QStringLiteral("name")).toString());
    }
    return names;
}

bool allValuesAre(const QVariantList &changes, int rate)
{
    for (const QVariant &change : changes) {
        const QVariantMap map = change.toMap();
        if (map.size() != 2 || map.value(QStringLiteral("value")).toInt() != rate) {
            return false;
        }
    }
    return !changes.isEmpty();
}

struct Fixture
{
    ConfigHWOSDView view;
    QSignalSpy requests;

    Fixture()
        : requests(&view, &ConfigHWOSDView::writeParamsRequested)
    {
    }

    ConfigHWOSDViewModel *model() const { return view.viewModel(); }
    QString status() const { return view.viewModel()->Status(); }
    QPushButton *button() const
    {
        return view.findChild<QPushButton *>(QStringLiteral("hwOsdEnableTelemetry"));
    }
    QLabel *statusLabel() const { return view.findChild<QLabel *>(QStringLiteral("hwOsdStatus")); }
    QVariantList lastChanges() const { return requests.last().at(1).toList(); }
};

} // namespace

class ConfigHWOSDViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactNamesOrderAndValue();
    void inventoryTexts();
    void offlineUnreadyAndEmptySnapshots();
    void partialSnapshotWritesOnlyPresentParameters();
    void fullBatchLifecycle();
    void failuresCancellationAndForeignBatches();
    void targetChangeDisconnectAndTimeout();
    void reentrantDeletionIsSafe();
};

void ConfigHWOSDViewTest::exactNamesOrderAndValue()
{
    QCOMPARE(ConfigHWOSDViewModel::ParameterNames(), kExpectedNames);
    QCOMPARE(ConfigHWOSDViewModel::ParameterNames().size(), 24);
    QCOMPARE(ConfigHWOSDViewModel::ParameterCount(), 24);
    QCOMPARE(ConfigHWOSDViewModel::Streams(),
             (QStringList{QStringLiteral("SR0"), QStringLiteral("SR1"), QStringLiteral("SR3")}));
    QCOMPARE(ConfigHWOSDViewModel::Suffixes().size(), 8);
    QVERIFY(!ConfigHWOSDViewModel::ParameterNames().contains(QStringLiteral("SR0_PARAMS")));
    QVERIFY(ConfigHWOSDViewModel::ParameterNames().filter(QStringLiteral("SR2_")).isEmpty());
    QVERIFY(ConfigHWOSDViewModel::ParameterNames().filter(QStringLiteral("_PARAMS")).isEmpty());
    QCOMPARE(ConfigHWOSDViewModel::TelemetryRateHz(), 2);

    const QVariantList changes = ConfigHWOSDViewModel::BuildChanges(kExpectedNames);
    QCOMPARE(changes.size(), 24);
    QCOMPARE(changeNames(changes), kExpectedNames);
    QVERIFY(allValuesAre(changes, 2));
    QCOMPARE(changes.first().toMap().value(QStringLiteral("value")), QVariant(2));

    // MP10 texts.
    QCOMPARE(ConfigHWOSDViewModel::Title(), QStringLiteral("OSD"));
    QCOMPARE(ConfigHWOSDViewModel::Intro(), QStringLiteral("MinimOSD telemetry helper."));
    QCOMPARE(ConfigHWOSDViewModel::Note(),
             QStringLiteral(
                 "You only need to use this if you are having issue with your OSD not updating."));
    QCOMPARE(ConfigHWOSDViewModel::EnableTelemetryText(), QStringLiteral("Enable Telemetry"));
    QCOMPARE(ConfigHWOSDViewModel::OfflineStatus(), QStringLiteral("offline — connect first."));
    QCOMPARE(ConfigHWOSDViewModel::SettingStatus(), QStringLiteral("Setting stream rates…"));
    QCOMPARE(ConfigHWOSDViewModel::SuccessStatus(),
             QStringLiteral("✓ Telemetry streams enabled (2 Hz)."));
    QCOMPARE(ConfigHWOSDViewModel::FailureStatus(QString()),
             QStringLiteral("Failed to set OSD rates."));
}

void ConfigHWOSDViewTest::inventoryTexts()
{
    ConfigHWOSDView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigHWOSDView"));
    QCOMPARE(view.sizeHint(), QSize(800, 560));
    QVERIFY(view.viewModel());

    auto *title = view.findChild<QLabel *>(QStringLiteral("hwOsdTitle"));
    auto *intro = view.findChild<QLabel *>(QStringLiteral("hwOsdIntro"));
    auto *button = view.findChild<QPushButton *>(QStringLiteral("hwOsdEnableTelemetry"));
    auto *status = view.findChild<QLabel *>(QStringLiteral("hwOsdStatus"));
    auto *note = view.findChild<QLabel *>(QStringLiteral("hwOsdNote"));
    QVERIFY(title && intro && button && status && note);
    QCOMPARE(title->text(), QStringLiteral("OSD"));
    QCOMPARE(intro->text(), QStringLiteral("MinimOSD telemetry helper."));
    QCOMPARE(button->text(), QStringLiteral("Enable Telemetry"));
    QVERIFY(status->text().isEmpty()); // MP10 starts with an empty status
    QCOMPARE(note->text(),
             QStringLiteral(
                 "You only need to use this if you are having issue with your OSD not updating."));
    QVERIFY(!button->isEnabled()); // offline: the action can do nothing yet
    QVERIFY(!view.findChild<QLabel *>(QStringLiteral("hwOsdImage")));
}

void ConfigHWOSDViewTest::offlineUnreadyAndEmptySnapshots()
{
    Fixture fixture;
    ConfigHWOSDViewModel *model = fixture.model();

    // Offline: MP10 "offline — connect first." and no batch.
    QVERIFY(!model->EnableTelemetry());
    QCOMPARE(fixture.status(), QStringLiteral("offline — connect first."));
    QCOMPARE(fixture.statusLabel()->text(), QStringLiteral("offline — connect first."));
    QVERIFY(!fixture.button()->isEnabled());
    fixture.button()->click();
    QCOMPARE(fixture.requests.size(), 0);

    // Connected without a committed snapshot: unready, still no batch.
    fixture.view.setConnected(true);
    QVERIFY(model->Connected());
    QVERIFY(!model->SnapshotReady());
    QVERIFY(!fixture.button()->isEnabled());
    QVERIFY(!model->EnableTelemetry());
    QCOMPARE(fixture.status(),
             QStringLiteral("parameters unavailable — refresh the parameter list first."));
    QCOMPARE(fixture.requests.size(), 0);

    // A snapshot without any stream-rate parameter: explicit missing status.
    fixture.view.setParameterSnapshot({value(QStringLiteral("SR0_PARAMS"), 1, 50),
                                       value(QStringLiteral("SYSID_THISMAV"), 1, 1)});
    QVERIFY(model->SnapshotReady());
    QVERIFY(fixture.status().isEmpty());
    QVERIFY(model->PresentParameterNames().isEmpty());
    QCOMPARE(model->MissingParameterNames(), kExpectedNames);
    QVERIFY(!fixture.button()->isEnabled());
    QVERIFY(!model->EnableTelemetry());
    QCOMPARE(fixture.status(),
             QStringLiteral(
                 "No SR0/SR1/SR3 stream-rate parameters on component 1 — nothing to enable."));
    QCOMPARE(fixture.requests.size(), 0);
    QVERIFY(!model->Busy());

    // Disconnecting drops the snapshot and reports offline again.
    fixture.view.setConnected(false);
    QVERIFY(!model->SnapshotReady());
    QCOMPARE(fixture.status(), QStringLiteral("offline — connect first."));
}

void ConfigHWOSDViewTest::partialSnapshotWritesOnlyPresentParameters()
{
    Fixture fixture;
    ConfigHWOSDViewModel *model = fixture.model();
    fixture.view.setConnected(true);

    // Five present names out of order, with lower-case spelling, a foreign
    // component, SRx_PARAMS and SR2 distractors.
    QList<ConfigFriendlyParameterValue> parameters{
        value(QStringLiteral("SR3_RC_CHAN")),
        value(QStringLiteral("sr0_extra1")),
        value(QStringLiteral("SR1_POSITION")),
        value(QStringLiteral("SR0_EXT_STAT")),
        value(QStringLiteral("SR1_EXTRA3")),
        value(QStringLiteral("SR0_PARAMS"), 1, 50),
        value(QStringLiteral("SR2_EXTRA1")),
        value(QStringLiteral("SR0_RAW_SENS"), 2), // another component: not ours
    };
    fixture.view.setParameterSnapshot(parameters, 1);
    QCOMPARE(model->ComponentId(), 1);
    const QStringList expectedPresent{QStringLiteral("SR0_EXT_STAT"), QStringLiteral("SR0_EXTRA1"),
                                      QStringLiteral("SR1_EXTRA3"), QStringLiteral("SR1_POSITION"),
                                      QStringLiteral("SR3_RC_CHAN")};
    QCOMPARE(model->PresentParameterNames(), expectedPresent);
    QCOMPARE(model->MissingParameterNames().size(), 19);
    QVERIFY(fixture.button()->isEnabled());

    fixture.button()->click();
    QCOMPARE(fixture.requests.size(), 1);
    QCOMPARE(fixture.requests.last().at(0).toInt(), 1);
    const QVariantList changes = fixture.lastChanges();
    QCOMPARE(changeNames(changes), expectedPresent);
    QVERIFY(allValuesAre(changes, 2));
    QVERIFY(model->Busy());
    QVERIFY(!fixture.button()->isEnabled());
    QCOMPARE(fixture.status(), QStringLiteral("Setting stream rates…"));
    QCOMPARE(model->PendingBatchId(), qulonglong(0));

    // Clicking again while busy is inert.
    fixture.button()->click();
    QVERIFY(!model->EnableTelemetry());
    QCOMPARE(fixture.requests.size(), 1);

    fixture.view.parameterBatchSubmitted(1, 77);
    QCOMPARE(model->PendingBatchId(), qulonglong(77));
    fixture.view.parameterBatchProgress(77, 2, 5, 2, 0);
    QCOMPARE(fixture.status(), QStringLiteral("Setting stream rates… (2/5)"));
    fixture.view.parameterBatchCompleted(77, 5, 0);
    QVERIFY(!model->Busy());
    QVERIFY(fixture.button()->isEnabled());
    QVERIFY2(fixture.status().startsWith(
                 QStringLiteral("✓ Telemetry streams enabled (2 Hz) for 5 of 24 parameters; "
                                "not on this vehicle: SR0_EXTRA2, SR0_EXTRA3, ")),
             qPrintable(fixture.status()));
    QVERIFY(fixture.status().endsWith(QStringLiteral("SR3_RAW_SENS.")));
    QVERIFY(!fixture.status().contains(QStringLiteral("SR0_EXTRA1,")));

    // The preferred component wins when it has the parameters; otherwise the
    // component that has them is chosen.
    fixture.view.setParameterSnapshot({value(QStringLiteral("SR0_EXTRA1"), 1),
                                       value(QStringLiteral("SR0_EXTRA1"), 3)},
                                      3);
    QCOMPARE(model->ComponentId(), 3);
    fixture.view.setParameterSnapshot({value(QStringLiteral("SR0_EXTRA1"), 5)}, 1);
    QCOMPARE(model->ComponentId(), 5);
    fixture.view.setParameterSnapshot({value(QStringLiteral("SR0_EXTRA1"), 7),
                                       value(QStringLiteral("SR0_EXTRA1"), 4)},
                                      1);
    QCOMPARE(model->ComponentId(), 4);
    fixture.view.setParameterSnapshot({value(QStringLiteral("SYSID_THISMAV"), 9)}, 2);
    QCOMPARE(model->ComponentId(), 2);
    QVERIFY(model->PresentParameterNames().isEmpty());
}

void ConfigHWOSDViewTest::fullBatchLifecycle()
{
    Fixture fixture;
    ConfigHWOSDViewModel *model = fixture.model();
    fixture.view.setConnected(true);
    fixture.view.setParameterSnapshot(fullSnapshot(), 1);
    QCOMPARE(model->PresentParameterNames(), kExpectedNames);
    QVERIFY(model->MissingParameterNames().isEmpty());

    QVERIFY(model->EnableTelemetry());
    QCOMPARE(fixture.requests.size(), 1);
    const QVariantList changes = fixture.lastChanges();
    QCOMPARE(changes.size(), 24);
    QCOMPARE(changeNames(changes), kExpectedNames);
    QVERIFY(allValuesAre(changes, 2));
    QVERIFY(model->Busy());

    // Results for the wrong component or before submission are ignored.
    fixture.view.parameterBatchSubmitted(2, 5);
    QCOMPARE(model->PendingBatchId(), qulonglong(0));
    fixture.view.parameterBatchSubmitted(1, 0);
    QCOMPARE(model->PendingBatchId(), qulonglong(0));
    fixture.view.parameterBatchSubmitted(1, 5);
    QCOMPARE(model->PendingBatchId(), qulonglong(5));
    fixture.view.parameterBatchSubmitted(1, 6); // a second id never replaces the first
    QCOMPARE(model->PendingBatchId(), qulonglong(5));
    fixture.view.parameterBatchCompleted(6, 24, 0); // foreign batch
    QVERIFY(model->Busy());
    fixture.view.parameterBatchCompleted(5, 24, 0);
    QVERIFY(!model->Busy());
    QCOMPARE(fixture.status(), QStringLiteral("✓ Telemetry streams enabled (2 Hz)."));
    QVERIFY(fixture.button()->isEnabled());

    // A second run is a fresh batch; a late completion of the old one is ignored.
    QVERIFY(model->EnableTelemetry());
    QCOMPARE(fixture.requests.size(), 2);
    fixture.view.parameterBatchCompleted(5, 24, 0);
    QVERIFY(model->Busy());
    QCOMPARE(fixture.status(), QStringLiteral("Setting stream rates…"));

    // Synchronous owners may complete before reporting the id.
    fixture.view.parameterBatchCompleted(9, 24, 0);
    QVERIFY(model->Busy());
    fixture.view.parameterBatchSubmitted(1, 9);
    QVERIFY(!model->Busy());
    QCOMPARE(fixture.status(), QStringLiteral("✓ Telemetry streams enabled (2 Hz)."));
}

void ConfigHWOSDViewTest::failuresCancellationAndForeignBatches()
{
    Fixture fixture;
    ConfigHWOSDViewModel *model = fixture.model();
    fixture.view.setConnected(true);
    fixture.view.setParameterSnapshot(fullSnapshot(), 1);

    // Per-write failures are named in the failure status.
    QVERIFY(model->EnableTelemetry());
    fixture.view.parameterBatchSubmitted(1, 11);
    fixture.view.parameterWriteFailed(11, 1, QStringLiteral("SR1_EXTRA1"), QStringLiteral("nak"));
    fixture.view.parameterWriteFailed(11, 1, QStringLiteral("SR3_RC_CHAN"), QStringLiteral("nak"));
    fixture.view.parameterWriteFailed(11, 1, QStringLiteral("SR3_RC_CHAN"), QStringLiteral("dup"));
    fixture.view.parameterWriteFailed(12, 1, QStringLiteral("SR0_EXTRA1"), QStringLiteral("foreign"));
    fixture.view.parameterBatchCompleted(11, 22, 2);
    QVERIFY(!model->Busy());
    QCOMPARE(fixture.status(),
             QStringLiteral("Failed to set OSD rates. 2 of 24 writes failed: SR1_EXTRA1, SR3_RC_CHAN."));
    QVERIFY(fixture.button()->isEnabled());

    // Fewer successes than writes without a failure count is still a failure.
    QVERIFY(model->EnableTelemetry());
    fixture.view.parameterBatchSubmitted(1, 13);
    fixture.view.parameterBatchCompleted(13, 20, 0);
    QCOMPARE(fixture.status(), QStringLiteral("Failed to set OSD rates. 4 of 24 writes failed."));

    // Cancellation.
    QVERIFY(model->EnableTelemetry());
    fixture.view.parameterBatchSubmitted(1, 14);
    fixture.view.parameterBatchCancelled(15); // foreign
    QVERIFY(model->Busy());
    fixture.view.parameterBatchCancelled(14);
    QVERIFY(!model->Busy());
    QCOMPARE(fixture.status(),
             QStringLiteral("Failed to set OSD rates. The parameter writes were cancelled."));

    // Submission failure before any id.
    QVERIFY(model->EnableTelemetry());
    fixture.view.parameterWriteSubmissionFailed(QStringLiteral("not connected to the selected target"));
    QVERIFY(!model->Busy());
    QCOMPARE(fixture.status(),
             QStringLiteral("Failed to set OSD rates. not connected to the selected target"));
    fixture.view.parameterWriteSubmissionFailed(QStringLiteral("late")); // idle: ignored
    QCOMPARE(fixture.status(),
             QStringLiteral("Failed to set OSD rates. not connected to the selected target"));
    QVERIFY(model->EnableTelemetry());
    fixture.view.parameterBatchSubmitted(1, 16);
    fixture.view.parameterWriteSubmissionFailed(QStringLiteral("too late")); // submitted: ignored
    QVERIFY(model->Busy());
    fixture.view.parameterBatchCompleted(16, 24, 0);
    QCOMPARE(fixture.status(), QStringLiteral("✓ Telemetry streams enabled (2 Hz)."));
    QCOMPARE(fixture.requests.size(), 5);
}

void ConfigHWOSDViewTest::targetChangeDisconnectAndTimeout()
{
    Fixture fixture;
    ConfigHWOSDViewModel *model = fixture.model();
    fixture.view.setConnected(true);
    fixture.view.setParameterSnapshot(fullSnapshot(), 1);

    // The target changes while the batch runs: the page settles at once and
    // the old batch's results are ignored, including a late submission id.
    QVERIFY(model->EnableTelemetry());
    const quint64 revision = model->SnapshotRevision();
    fixture.view.parameterTargetChanged();
    QVERIFY(model->SnapshotRevision() > revision);
    QVERIFY(!model->Busy());
    QVERIFY(!model->SnapshotReady());
    QVERIFY(!fixture.button()->isEnabled());
    QCOMPARE(fixture.status(),
             QStringLiteral("The selected target changed before the stream rates were confirmed."));
    fixture.view.parameterBatchSubmitted(1, 21);
    fixture.view.parameterBatchCompleted(21, 24, 0);
    QVERIFY(!model->Busy());
    QCOMPARE(model->PendingBatchId(), qulonglong(0));
    QCOMPARE(fixture.status(),
             QStringLiteral("The selected target changed before the stream rates were confirmed."));
    QVERIFY(!model->EnableTelemetry()); // unready until a new snapshot arrives
    QCOMPARE(fixture.status(),
             QStringLiteral("parameters unavailable — refresh the parameter list first."));

    // A new snapshot while a batch runs abandons it quietly.
    fixture.view.setParameterSnapshot(fullSnapshot(), 1);
    QVERIFY(model->EnableTelemetry());
    fixture.view.parameterBatchSubmitted(1, 22);
    fixture.view.setParameterSnapshot(fullSnapshot(), 1);
    QVERIFY(!model->Busy());
    QVERIFY(fixture.status().isEmpty());
    fixture.view.parameterBatchCompleted(22, 24, 0);
    QVERIFY(fixture.status().isEmpty());

    // Disconnecting mid-batch reports offline and drops the batch.
    QVERIFY(model->EnableTelemetry());
    fixture.view.parameterBatchSubmitted(1, 23);
    fixture.view.setConnected(false);
    QVERIFY(!model->Busy());
    QCOMPARE(fixture.status(), QStringLiteral("offline — connect first."));
    fixture.view.parameterBatchCompleted(23, 24, 0);
    QCOMPARE(fixture.status(), QStringLiteral("offline — connect first."));

    // A batch nobody completes times out instead of locking the page.
    fixture.view.setConnected(true);
    fixture.view.setParameterSnapshot(fullSnapshot(), 1);
    model->setBatchTimeoutForTesting(30);
    QVERIFY(model->EnableTelemetry());
    fixture.view.parameterBatchSubmitted(1, 24);
    QTRY_VERIFY_WITH_TIMEOUT(!model->Busy(), 1000);
    QCOMPARE(fixture.status(),
             QStringLiteral("Failed to set OSD rates. The vehicle did not confirm the writes in time."));
    QVERIFY(fixture.button()->isEnabled());
    // The stale timer of a batch that already finished does nothing.
    QVERIFY(model->EnableTelemetry());
    fixture.view.parameterBatchSubmitted(1, 25);
    fixture.view.parameterBatchCompleted(25, 24, 0);
    QCOMPARE(fixture.status(), QStringLiteral("✓ Telemetry streams enabled (2 Hz)."));
    QTest::qWait(60);
    QCOMPARE(fixture.status(), QStringLiteral("✓ Telemetry streams enabled (2 Hz)."));
    QVERIFY(!model->Busy());
}

void ConfigHWOSDViewTest::reentrantDeletionIsSafe()
{
    // The owner deletes the page from inside the batch request (for example
    // because the target vanished while the click was handled).
    QPointer<ConfigHWOSDView> view(new ConfigHWOSDView);
    view->setConnected(true);
    view->setParameterSnapshot(fullSnapshot(), 1);
    int requests = 0;
    QObject::connect(view.data(), &ConfigHWOSDView::writeParamsRequested, view.data(),
                     [&requests, view](int, const QVariantList &changes) {
                         ++requests;
                         QCOMPARE(changes.size(), 24);
                         delete view.data();
                     });
    view->findChild<QPushButton *>(QStringLiteral("hwOsdEnableTelemetry"))->click();
    QCOMPARE(requests, 1);
    QVERIFY(view.isNull());

    // Deleting the page while a batch is pending needs no settlement; late
    // owner callbacks simply have no receiver.
    QPointer<ConfigHWOSDView> pending(new ConfigHWOSDView);
    pending->setConnected(true);
    pending->setParameterSnapshot(fullSnapshot(), 1);
    QVERIFY(pending->viewModel()->EnableTelemetry());
    pending->parameterBatchSubmitted(1, 31);
    QVERIFY(pending->viewModel()->Busy());
    delete pending.data();
    QVERIFY(pending.isNull());
    QTest::qWait(10);
}

QTEST_MAIN(ConfigHWOSDViewTest)
#include "test_confighwosdview.moc"
