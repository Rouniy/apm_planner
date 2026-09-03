#include <QtTest>

#include "ui/configuration/ConfigOSDViewModel.h"

#include <QPointer>
#include <QSignalSpy>
#include <QVariantMap>

#include <algorithm>

namespace {
ConfigFriendlyParameterValue parameter(
    const QString &name, const QVariant &value = 0, int componentId = 1)
{
    ConfigFriendlyParameterValue result;
    result.componentId = componentId;
    result.name = name;
    result.value = value;
    return result;
}

QList<ConfigFriendlyParameterValue> triplet(
    int screen, const QString &item, const QVariant &enabled,
    const QVariant &x, const QVariant &y, int componentId = 1,
    bool lowerCase = false)
{
    QString prefix = QStringLiteral("OSD%1_%2")
        .arg(screen).arg(item);
    if (lowerCase) {
        prefix = prefix.toLower();
    }
    return {
        parameter(prefix + (lowerCase ? QStringLiteral("_en")
                                      : QStringLiteral("_EN")),
                  enabled, componentId),
        parameter(prefix + (lowerCase ? QStringLiteral("_x")
                                      : QStringLiteral("_X")),
                  x, componentId),
        parameter(prefix + (lowerCase ? QStringLiteral("_y")
                                      : QStringLiteral("_Y")),
                  y, componentId)
    };
}

void append(QList<ConfigFriendlyParameterValue> *target,
            const QList<ConfigFriendlyParameterValue> &values)
{
    for (const ConfigFriendlyParameterValue &value : values) {
        target->append(value);
    }
}

QList<ConfigFriendlyParameterValue> basicSnapshot()
{
    QList<ConfigFriendlyParameterValue> result;
    append(&result, triplet(2, QStringLiteral("ZULU"), 1, 6, 7));
    append(&result, triplet(1, QStringLiteral("BRAVO"), 1, 3, 4));
    append(&result, triplet(1, QStringLiteral("alpha"), 0, 1, 2,
                            1, true));
    return result;
}

QStringList changeNames(const QVariantList &changes)
{
    QStringList names;
    for (const QVariant &change : changes) {
        names.append(change.toMap().value(
            QStringLiteral("name")).toString());
    }
    return names;
}

QVariant changeValue(const QVariantList &changes, const QString &name)
{
    for (const QVariant &change : changes) {
        const QVariantMap map = change.toMap();
        if (map.value(QStringLiteral("name")).toString()
                .compare(name, Qt::CaseInsensitive) == 0) {
            return map.value(QStringLiteral("value"));
        }
    }
    return {};
}
} // namespace

class ConfigOSDViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactSurfaceAndOfflineStates();
    void parsesOnlyCompleteTripletsDeterministically();
    void choosesPreferredComponentAndPreservesScreen();
    void stagesClampedDirtyFieldsAndDiscards();
    void writesOneOwnedDeterministicBatch();
    void partialFailureKeepsOnlyFailedFieldsDirty();
    void anonymousFailureCancelSubmissionAndTimeoutKeepChanges();
    void snapshotsTargetsAndDisconnectInvalidateOwnership();
    void identicalRefreshPreservesStagedChanges();
    void reentrantDeletionDuringRequestIsSafe();
};

void ConfigOSDViewModelTest::exactSurfaceAndOfflineStates()
{
    ConfigOSDViewModel model;
    QCOMPARE(ConfigOSDViewModel::Title(), QStringLiteral("Onboard OSD"));
    QCOMPARE(ConfigOSDViewModel::Columns(), 30);
    QCOMPARE(ConfigOSDViewModel::Rows(), 16);
    QCOMPARE(ConfigOSDViewModel::CellWidth(), 26);
    QCOMPARE(ConfigOSDViewModel::CellHeight(), 24);
    QCOMPARE(ConfigOSDViewModel::CanvasWidth(), 780);
    QCOMPARE(ConfigOSDViewModel::CanvasHeight(), 384);
    QVERIFY(ConfigOSDViewModel::Intro().startsWith(
        QStringLiteral("Drag items on the screen preview, or edit X/Y directly.")));
    QVERIFY(ConfigOSDViewModel::Intro().contains(
        QStringLiteral("staged until Write Changes")));
    QCOMPARE(ConfigOSDViewModel::NoParametersStatus(),
             QStringLiteral("No OSD parameters found. Connect and Refresh."));
    QCOMPARE(ConfigOSDViewModel::ScreenStatus(3, 7),
             QStringLiteral("Screen 3: 7 items."));

    QVERIFY(!model.Connected());
    QVERIFY(!model.SnapshotReady());
    QVERIFY(!model.CanEdit());
    QVERIFY(!model.CanWrite());
    QVERIFY(!model.WriteChanges());
    QCOMPARE(model.Status(), QStringLiteral("offline — connect first."));

    model.setConnected(true);
    QVERIFY(!model.WriteChanges());
    QCOMPARE(model.Status(), QStringLiteral(
        "parameters unavailable — refresh the parameter list first."));

    model.setParameterSnapshot({parameter(QStringLiteral("OSD1_ALT_EN"))});
    QVERIFY(model.SnapshotReady());
    QVERIFY(model.Screens().isEmpty());
    QCOMPARE(model.SelectedScreen(), 0);
    QCOMPARE(model.Status(), ConfigOSDViewModel::NoParametersStatus());
    QVERIFY(!model.CanEdit());
    QVERIFY(!model.WriteChanges());
    QCOMPARE(model.Status(), ConfigOSDViewModel::NoChangesStatus());
}

void ConfigOSDViewModelTest::parsesOnlyCompleteTripletsDeterministically()
{
    ConfigOSDViewModel model;
    model.setConnected(true);

    QList<ConfigFriendlyParameterValue> values;
    append(&values, triplet(2, QStringLiteral("Zulu"), 1, 40, -4));
    append(&values, triplet(1, QStringLiteral("bravo"), 0, 4.6, 8.4,
                            1, true));
    append(&values, triplet(1, QStringLiteral("ALPHA"), 2, 1, 2));
    values.append(parameter(QStringLiteral("OSD1_ORPHAN_EN"), 1));
    values.append(parameter(QStringLiteral("OSD1_HALF_EN"), 1));
    values.append(parameter(QStringLiteral("OSD1_HALF_X"), 5));
    values.append(parameter(QStringLiteral("OSD1_SPLIT_EN"), 1, 1));
    values.append(parameter(QStringLiteral("OSD1_SPLIT_X"), 2, 1));
    values.append(parameter(QStringLiteral("OSD1_SPLIT_Y"), 3, 2));
    append(&values, triplet(0, QStringLiteral("ZERO"), 1, 1, 1));
    values.append(parameter(QStringLiteral("NOT_OSD1_ALT_EN"), 1));
    std::reverse(values.begin(), values.end());

    model.setParameterSnapshot(values, 1);
    QCOMPARE(model.ComponentId(), 1);
    QCOMPARE(model.Screens(), (QList<int>{1, 2}));
    QCOMPARE(model.SelectedScreen(), 1);
    QCOMPARE(model.Status(), QStringLiteral("Screen 1: 2 items."));

    const QList<ConfigOSDItem> first = model.Items();
    QCOMPARE(first.size(), 2);
    QCOMPARE(first.at(0).name, QStringLiteral("ALPHA"));
    QCOMPARE(first.at(1).name, QStringLiteral("BRAVO"));
    QVERIFY(first.at(0).acceptedEnabled);
    QCOMPARE(first.at(0).acceptedX, 1);
    QCOMPARE(first.at(0).acceptedY, 2);
    QVERIFY(!first.at(1).acceptedEnabled);
    QCOMPARE(first.at(1).acceptedX, 5);
    QCOMPARE(first.at(1).acceptedY, 8);
    QCOMPARE(first.at(1).enableParameter, QStringLiteral("osd1_bravo_en"));
    QVERIFY(!first.at(0).dirty());
    QVERIFY(!first.at(1).dirty());

    QVERIFY(model.selectScreen(2));
    const QList<ConfigOSDItem> second = model.Items();
    QCOMPARE(second.size(), 1);
    QCOMPARE(second.first().name, QStringLiteral("ZULU"));
    QCOMPARE(second.first().acceptedX, 40);
    QCOMPARE(second.first().acceptedY, 0);
    QVERIFY(model.setItemX(2, QStringLiteral("ZULU"), 29));
    ConfigOSDItem zulu;
    QVERIFY(model.Item(2, QStringLiteral("ZULU"), &zulu));
    QCOMPARE(zulu.acceptedX, 40);
    QCOMPARE(zulu.x, 29);
    QVERIFY(zulu.xDirty());
    QCOMPARE(model.Status(), QStringLiteral("Screen 2: 1 items."));
    QVERIFY(!model.selectScreen(99));
}

void ConfigOSDViewModelTest::choosesPreferredComponentAndPreservesScreen()
{
    ConfigOSDViewModel model;
    model.setConnected(true);
    QList<ConfigFriendlyParameterValue> values;
    append(&values, triplet(3, QStringLiteral("THREE"), 0, 0, 0, 3));
    append(&values, triplet(5, QStringLiteral("FIVE"), 0, 0, 0, 5));

    model.setParameterSnapshot(values, 5);
    QCOMPARE(model.ComponentId(), 5);
    QCOMPARE(model.Screens(), (QList<int>{5}));

    model.setParameterSnapshot(values, 1);
    QCOMPARE(model.ComponentId(), 3); // lowest component with a complete item
    QCOMPARE(model.Screens(), (QList<int>{3}));

    QList<ConfigFriendlyParameterValue> twoScreens;
    append(&twoScreens, triplet(1, QStringLiteral("ONE"), 0, 0, 0));
    append(&twoScreens, triplet(2, QStringLiteral("TWO"), 0, 0, 0));
    model.setParameterSnapshot(twoScreens);
    QVERIFY(model.selectScreen(2));

    QList<ConfigFriendlyParameterValue> preserved;
    append(&preserved, triplet(2, QStringLiteral("TWO"), 1, 2, 3));
    append(&preserved, triplet(4, QStringLiteral("FOUR"), 0, 4, 5));
    model.setParameterSnapshot(preserved);
    QCOMPARE(model.SelectedScreen(), 2);

    model.setParameterSnapshot(triplet(
        4, QStringLiteral("FOUR"), 0, 4, 5));
    QCOMPARE(model.SelectedScreen(), 4);
}

void ConfigOSDViewModelTest::stagesClampedDirtyFieldsAndDiscards()
{
    ConfigOSDViewModel model;
    model.setConnected(true);
    model.setParameterSnapshot(basicSnapshot());
    QVERIFY(model.CanEdit());
    QVERIFY(!model.CanWrite());

    QVERIFY(model.setItemEnabled(1, QStringLiteral("ALPHA"), true));
    QVERIFY(model.setItemX(1, QStringLiteral("alpha"), 99));
    QVERIFY(model.setItemY(1, QStringLiteral("ALPHA"), -8));
    QVERIFY(!model.setItemX(1, QStringLiteral("ALPHA"), 29));
    QVERIFY(!model.setItemPosition(1, QStringLiteral("missing"), 2, 3));
    QCOMPARE(model.DirtyFieldCount(), 3);
    QVERIFY(model.HasDirtyChanges());
    QVERIFY(model.CanWrite());

    ConfigOSDItem alpha;
    QVERIFY(model.Item(1, QStringLiteral("alpha"), &alpha));
    QVERIFY(alpha.enabled);
    QCOMPARE(alpha.x, 29);
    QCOMPARE(alpha.y, 0);
    QCOMPARE(alpha.acceptedX, 1);
    QCOMPARE(alpha.acceptedY, 2);

    QVERIFY(model.selectScreen(2));
    QVERIFY(model.EnableAll(false)); // ZULU was accepted enabled
    QCOMPARE(model.DirtyFieldCount(), 4);
    QVERIFY(model.EnableAll(true)); // back to the accepted value
    QCOMPARE(model.DirtyFieldCount(), 3);

    QVERIFY(model.selectScreen(1));
    QVERIFY(model.EnableAll(false));
    // ALPHA returns to accepted false; BRAVO becomes dirty false.
    QCOMPARE(model.DirtyFieldCount(), 3);
    const QVariantList changes = model.DirtyChanges();
    QCOMPARE(changeNames(changes),
             (QStringList{QStringLiteral("osd1_alpha_x"),
                          QStringLiteral("osd1_alpha_y"),
                          QStringLiteral("OSD1_BRAVO_EN")}));
    QCOMPARE(changeValue(changes, QStringLiteral("OSD1_ALPHA_X")),
             QVariant(29));
    QCOMPARE(changeValue(changes, QStringLiteral("OSD1_ALPHA_Y")),
             QVariant(0));
    QCOMPARE(changeValue(changes, QStringLiteral("OSD1_BRAVO_EN")),
             QVariant(0));

    QVERIFY(model.Discard());
    QCOMPARE(model.DirtyFieldCount(), 0);
    QVERIFY(!model.HasDirtyChanges());
    QVERIFY(!model.CanWrite());
    QVERIFY(!model.Discard());
    QVERIFY(model.Item(1, QStringLiteral("ALPHA"), &alpha));
    QVERIFY(!alpha.enabled);
    QCOMPARE(alpha.x, 1);
    QCOMPARE(alpha.y, 2);
    QCOMPARE(model.Status(), QStringLiteral("Screen 1: 2 items."));
}

void ConfigOSDViewModelTest::writesOneOwnedDeterministicBatch()
{
    ConfigOSDViewModel model;
    model.setConnected(true);
    model.setParameterSnapshot(basicSnapshot());
    QVERIFY(model.setItemEnabled(1, QStringLiteral("ALPHA"), true));
    QVERIFY(model.setItemPosition(1, QStringLiteral("ALPHA"), 8, 9));
    QSignalSpy requests(&model, &ConfigOSDViewModel::writeParamsRequested);

    QVERIFY(model.WriteChanges());
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().at(0).toInt(), 1);
    const QVariantList changes = requests.first().at(1).toList();
    QCOMPARE(changeNames(changes),
             (QStringList{QStringLiteral("osd1_alpha_en"),
                          QStringLiteral("osd1_alpha_x"),
                          QStringLiteral("osd1_alpha_y")}));
    QVERIFY(model.Busy());
    QVERIFY(!model.CanEdit());
    QVERIFY(!model.CanWrite());
    QVERIFY(!model.Discard());
    QVERIFY(!model.setItemX(1, QStringLiteral("ALPHA"), 4));
    QCOMPARE(model.Status(), ConfigOSDViewModel::WritingStatus());

    model.parameterBatchSubmitted(2, 40);
    model.parameterBatchSubmitted(1, 0);
    QCOMPARE(model.PendingBatchId(), qulonglong(0));
    model.parameterBatchSubmitted(1, 40);
    QCOMPARE(model.PendingBatchId(), qulonglong(40));
    model.parameterBatchSubmitted(1, 41);
    QCOMPARE(model.PendingBatchId(), qulonglong(40));
    model.parameterBatchProgress(41, 2, 3, 2, 0);
    QCOMPARE(model.Status(), ConfigOSDViewModel::WritingStatus());
    model.parameterBatchProgress(40, 2, 3, 2, 0);
    QCOMPARE(model.Status(), QStringLiteral("Writing OSD changes… (2/3)"));
    model.parameterBatchCompleted(41, 3, 0);
    QVERIFY(model.Busy());
    model.parameterBatchCompleted(40, 3, 0);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), ConfigOSDViewModel::SuccessStatus());
    QCOMPARE(model.DirtyFieldCount(), 0);

    ConfigOSDItem alpha;
    QVERIFY(model.Item(1, QStringLiteral("ALPHA"), &alpha));
    QVERIFY(alpha.acceptedEnabled);
    QCOMPARE(alpha.acceptedX, 8);
    QCOMPARE(alpha.acceptedY, 9);

    // A synchronous owner may report field failure and completion before it
    // reports the batch id.
    QVERIFY(model.setItemEnabled(1, QStringLiteral("ALPHA"), false));
    QVERIFY(model.setItemX(1, QStringLiteral("ALPHA"), 12));
    QVERIFY(model.setItemY(1, QStringLiteral("ALPHA"), 14));
    QVERIFY(model.WriteChanges());
    model.parameterWriteFailed(51, 1, QStringLiteral("osd1_alpha_x"),
                               QStringLiteral("synchronous rejection"));
    model.parameterBatchCompleted(51, 2, 1);
    QVERIFY(model.Busy());
    model.parameterBatchSubmitted(1, 51);
    QVERIFY(!model.Busy());
    QCOMPARE(model.DirtyFieldCount(), 1);
    QCOMPARE(changeNames(model.DirtyChanges()),
             (QStringList{QStringLiteral("osd1_alpha_x")}));
    QVERIFY(model.Status().contains(QStringLiteral("osd1_alpha_x"),
                                    Qt::CaseInsensitive));
    QVERIFY(model.WriteChanges());
    model.parameterBatchSubmitted(1, 52);
    model.parameterBatchCompleted(52, 1, 0);
    QCOMPARE(model.DirtyFieldCount(), 0);
    model.parameterBatchCompleted(40, 3, 0); // stale completion
    QCOMPARE(model.Status(), ConfigOSDViewModel::SuccessStatus());
}

void ConfigOSDViewModelTest::partialFailureKeepsOnlyFailedFieldsDirty()
{
    ConfigOSDViewModel model;
    model.setConnected(true);
    model.setParameterSnapshot(basicSnapshot());
    QVERIFY(model.setItemEnabled(1, QStringLiteral("ALPHA"), true));
    QVERIFY(model.setItemPosition(1, QStringLiteral("ALPHA"), 8, 9));
    QVERIFY(model.WriteChanges());
    model.parameterBatchSubmitted(1, 70);

    model.parameterWriteFailed(71, 1, QStringLiteral("osd1_alpha_x"),
                               QStringLiteral("foreign"));
    model.parameterWriteFailed(70, 2, QStringLiteral("osd1_alpha_x"),
                               QStringLiteral("wrong component"));
    model.parameterWriteFailed(70, 1, QStringLiteral("OSD9_OTHER_X"),
                               QStringLiteral("not ours"));
    model.parameterWriteFailed(70, 1, QStringLiteral("OSD1_ALPHA_X"),
                               QStringLiteral("rejected"));
    model.parameterBatchCompleted(70, 2, 1);

    QVERIFY(!model.Busy());
    QCOMPARE(model.DirtyFieldCount(), 1);
    QCOMPARE(changeNames(model.DirtyChanges()),
             (QStringList{QStringLiteral("osd1_alpha_x")}));
    QVERIFY(model.Status().contains(QStringLiteral("1 of 3 writes failed")));
    QVERIFY(model.Status().contains(QStringLiteral("osd1_alpha_x"),
                                    Qt::CaseInsensitive));
    ConfigOSDItem alpha;
    QVERIFY(model.Item(1, QStringLiteral("ALPHA"), &alpha));
    QVERIFY(alpha.acceptedEnabled);
    QCOMPARE(alpha.acceptedX, 1); // failed and remains staged at 8
    QCOMPARE(alpha.x, 8);
    QCOMPARE(alpha.acceptedY, 9);

    QVERIFY(model.WriteChanges());
    QCOMPARE(model.DirtyChanges().size(), 1);
    model.parameterBatchCompleted(72, 1, 0);
    model.parameterBatchSubmitted(1, 72);
    QCOMPARE(model.DirtyFieldCount(), 0);
}

void ConfigOSDViewModelTest::anonymousFailureCancelSubmissionAndTimeoutKeepChanges()
{
    ConfigOSDViewModel model;
    model.setConnected(true);
    model.setParameterSnapshot(basicSnapshot());
    QVERIFY(model.setItemPosition(1, QStringLiteral("ALPHA"), 10, 11));

    QVERIFY(model.WriteChanges());
    model.parameterBatchSubmitted(1, 80);
    model.parameterBatchCompleted(80, 1, 1); // no failed parameter name
    QCOMPARE(model.DirtyFieldCount(), 2);
    QVERIFY(model.Status().contains(
        QStringLiteral("all changes remain staged")));

    QVERIFY(model.WriteChanges());
    model.parameterWriteSubmissionFailed(QStringLiteral("target unavailable"));
    QVERIFY(!model.Busy());
    QCOMPARE(model.DirtyFieldCount(), 2);
    QVERIFY(model.Status().contains(QStringLiteral("target unavailable")));

    QVERIFY(model.WriteChanges());
    model.parameterBatchSubmitted(1, 81);
    model.parameterWriteSubmissionFailed(QStringLiteral("too late"));
    QVERIFY(model.Busy());
    model.parameterBatchCancelled(82);
    QVERIFY(model.Busy());
    model.parameterBatchCancelled(81);
    QVERIFY(model.Busy());
    QCOMPARE(model.DirtyFieldCount(), 2);
    QCOMPARE(model.Status(), ConfigOSDViewModel::CancelledStatus());
    model.parameterBatchCompleted(81, 0, 2);
    QVERIFY(!model.Busy());
    QCOMPARE(model.DirtyFieldCount(), 2);

    model.setBatchTimeoutForTesting(20);
    QVERIFY(model.WriteChanges());
    model.parameterBatchSubmitted(1, 83);
    QTRY_COMPARE_WITH_TIMEOUT(model.Status(),
                              ConfigOSDViewModel::TimeoutStatus(), 1000);
    QVERIFY(model.Busy());
    QCOMPARE(model.DirtyFieldCount(), 2);
    QVERIFY(!model.Discard());
    model.parameterBatchCompleted(83, 0, 2);
    QVERIFY(!model.Busy());
    QCOMPARE(model.DirtyFieldCount(), 2);

    // A stale timeout belonging to a completed batch cannot overwrite success.
    model.setBatchTimeoutForTesting(40);
    QVERIFY(model.WriteChanges());
    model.parameterBatchSubmitted(1, 84);
    model.parameterBatchCompleted(84, 2, 0);
    QCOMPARE(model.Status(), ConfigOSDViewModel::SuccessStatus());
    QTest::qWait(80);
    QCOMPARE(model.Status(), ConfigOSDViewModel::SuccessStatus());
}

void ConfigOSDViewModelTest::snapshotsTargetsAndDisconnectInvalidateOwnership()
{
    ConfigOSDViewModel model;
    model.setConnected(true);
    model.setParameterSnapshot(basicSnapshot());
    QVERIFY(model.setItemX(1, QStringLiteral("ALPHA"), 6));
    QVERIFY(model.WriteChanges());
    model.parameterBatchSubmitted(1, 90);
    const quint64 firstRevision = model.SnapshotRevision();

    model.parameterTargetChanged();
    QVERIFY(model.SnapshotRevision() > firstRevision);
    QVERIFY(!model.Busy());
    QVERIFY(!model.SnapshotReady());
    QVERIFY(model.Screens().isEmpty());
    QCOMPARE(model.DirtyFieldCount(), 0);
    QCOMPARE(model.Status(), ConfigOSDViewModel::TargetChangedStatus());
    model.parameterBatchCompleted(90, 1, 0);
    QCOMPARE(model.Status(), ConfigOSDViewModel::TargetChangedStatus());

    // A snapshot cannot replace a batch whose writes remain queued.
    model.setParameterSnapshot(basicSnapshot());
    QVERIFY(model.setItemX(1, QStringLiteral("ALPHA"), 7));
    QVERIFY(model.WriteChanges());
    model.parameterBatchSubmitted(1, 91);
    QList<ConfigFriendlyParameterValue> replacement = basicSnapshot();
    for (ConfigFriendlyParameterValue &value : replacement) {
        if (value.name.compare(QStringLiteral("osd1_alpha_x"),
                               Qt::CaseInsensitive) == 0) {
            value.value = 13;
        }
    }
    model.setParameterSnapshot(replacement);
    QVERIFY(model.Busy());
    QVERIFY(model.SnapshotRevision() > firstRevision);
    ConfigOSDItem alpha;
    QVERIFY(model.Item(1, QStringLiteral("ALPHA"), &alpha));
    QCOMPARE(alpha.acceptedX, 1);
    QCOMPARE(alpha.x, 7);
    model.parameterBatchCompleted(91, 0, 1);
    QVERIFY(!model.Busy());

    // Once the transport is terminal, a changed accepted value wins over the
    // stale local edit.
    model.setParameterSnapshot(replacement);
    QVERIFY(model.Item(1, QStringLiteral("ALPHA"), &alpha));
    QCOMPARE(alpha.acceptedX, 13);
    QCOMPARE(alpha.x, 13);
    QCOMPARE(model.DirtyFieldCount(), 0);

    QVERIFY(model.setItemX(1, QStringLiteral("ALPHA"), 14));
    model.setConnected(false);
    QVERIFY(!model.SnapshotReady());
    QCOMPARE(model.DirtyFieldCount(), 0);
    QCOMPARE(model.Status(), ConfigOSDViewModel::OfflineStatus());
    QVERIFY(!model.CanEdit());
}

void ConfigOSDViewModelTest::identicalRefreshPreservesStagedChanges()
{
    ConfigOSDViewModel model;
    model.setConnected(true);
    model.setParameterSnapshot(basicSnapshot());
    QVERIFY(model.setItemEnabled(1, QStringLiteral("ALPHA"), true));
    QVERIFY(model.setItemPosition(1, QStringLiteral("ALPHA"), 12, 13));
    QCOMPARE(model.DirtyFieldCount(), 3);
    const quint64 oldRevision = model.SnapshotRevision();

    model.setParameterSnapshot(basicSnapshot());
    QVERIFY(model.SnapshotRevision() > oldRevision);
    QCOMPARE(model.DirtyFieldCount(), 3);
    ConfigOSDItem alpha;
    QVERIFY(model.Item(1, QStringLiteral("ALPHA"), &alpha));
    QVERIFY(alpha.enabled);
    QCOMPARE(alpha.x, 12);
    QCOMPARE(alpha.y, 13);
    QCOMPARE(alpha.acceptedX, 1);
    QCOMPARE(alpha.acceptedY, 2);

    QList<ConfigFriendlyParameterValue> changed = basicSnapshot();
    for (ConfigFriendlyParameterValue &value : changed) {
        if (value.name.compare(QStringLiteral("osd1_alpha_x"),
                               Qt::CaseInsensitive) == 0) {
            value.value = 9;
        }
    }
    model.setParameterSnapshot(changed);
    QVERIFY(model.Item(1, QStringLiteral("ALPHA"), &alpha));
    QVERIFY(!alpha.enabled);
    QCOMPARE(alpha.acceptedX, 9);
    QCOMPARE(alpha.x, 9);
    QCOMPARE(model.DirtyFieldCount(), 0);
}

void ConfigOSDViewModelTest::reentrantDeletionDuringRequestIsSafe()
{
    QPointer<ConfigOSDViewModel> model(new ConfigOSDViewModel);
    model->setConnected(true);
    model->setParameterSnapshot(basicSnapshot());
    QVERIFY(model->setItemX(1, QStringLiteral("ALPHA"), 8));
    int requests = 0;
    QObject::connect(model.data(), &ConfigOSDViewModel::writeParamsRequested,
                     model.data(), [&model, &requests](int componentId,
                                                       const QVariantList &changes) {
        QCOMPARE(componentId, 1);
        QCOMPARE(changes.size(), 1);
        ++requests;
        delete model.data();
    });
    QVERIFY(!model->WriteChanges());
    QCOMPARE(requests, 1);
    QVERIFY(model.isNull());
}

QTEST_GUILESS_MAIN(ConfigOSDViewModelTest)
#include "test_configosdviewmodel.moc"
