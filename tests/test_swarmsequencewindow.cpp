#include "comm/SwarmTelemetryRegistry.h"
#include "ui/SequenceLayoutControl.h"
#include "ui/SwarmSequenceWindow.h"

#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

#include <utility>

// The production application supplies this through the integration unit.
SwarmTelemetryRegistry *SwarmSequenceApplicationRegistry()
{
    return nullptr;
}

SwarmSequenceWindowInterface *SwarmSequenceApplicationInterface(QObject *)
{
    return nullptr;
}

namespace
{
template<typename Widget>
Widget *required(QWidget *root, const QString &name)
{
    Widget *result = root->findChild<Widget *>(name);
    if (!result) {
        QTest::qFail(qPrintable(QStringLiteral("Missing widget: %1").arg(name)),
                     __FILE__, __LINE__);
    }
    return result;
}

mavlink_message_t heartbeat(
    quint8 systemId, quint8 type = MAV_TYPE_QUADROTOR,
    quint8 autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(systemId, MAV_COMP_ID_AUTOPILOT1, &message,
        type, autopilot, MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, 0,
        MAV_STATE_ACTIVE);
    return message;
}

SwarmSequenceDocument sampleDocument()
{
    SwarmSequenceDocument document;
    SwarmSequenceLayout layout;
    layout.id = QStringLiteral("Line");
    layout.delayStart = 3;
    layout.delayEnd = 4;
    SwarmSequenceOffset seven;
    seven.x = 7.5;
    seven.y = -2.0;
    seven.z = 1.0;
    layout.offsets.insert(7, seven);
    SwarmSequenceOffset nine;
    nine.x = 9.0;
    layout.offsets.insert(9, nine);
    document.layouts.append(layout);
    document.steps << QStringLiteral("Line") << QStringLiteral("Line");
    return document;
}

class FakeSequenceInterface final : public SwarmSequenceWindowInterface
{
public:
    bool executorReady(QString *error) const override
    {
        if (error) {
            error->clear();
        }
        return ready && !active;
    }

    bool prepareRunStep(
        const SwarmSequenceRunStepRequest &request,
        SwarmSequencePreparedRunStep *prepared, QString *error) const override
    {
        ++prepareRunCalls;
        if (error) {
            error->clear();
        }
        if (!prepared || request.assignments.isEmpty()) {
            return false;
        }
        prepared->preparationId = 41;
        prepared->layoutId = request.layoutId;
        prepared->anchor = request.anchor;
        prepared->origin = request.origin;
        if (!prepared->origin.valid) {
            prepared->origin.valid = true;
            prepared->origin.latitude = 35.0;
            prepared->origin.longitude = 33.0;
        }
        prepared->assignments = request.assignments;
        for (const SwarmSequenceExactAssignment &assignment
             : request.assignments) {
            SwarmSequenceGeodeticPoint point;
            if (!SwarmSequenceGeometry::projectEastNorth(
                    prepared->origin.latitude, prepared->origin.longitude,
                    assignment.offset, &point)) {
                return false;
            }
            SwarmSequenceTarget target;
            target.systemId = assignment.systemId;
            target.lease = assignment.lease;
            target.latitude = point.latitude;
            target.longitude = point.longitude;
            target.relativeAltitudeM = point.altitudeM;
            prepared->targets.append(target);
        }
        return true;
    }

    bool runStep(const SwarmSequencePreparedRunStep &prepared,
                 QString *error) override
    {
        ++runCalls;
        ++generation;
        if (error) {
            error->clear();
        }
        report = {};
        report.operationGeneration = generation;
        report.operation = SwarmSequenceOperation::RunStep;
        report.result = runResult;
        report.origin = prepared.origin;
        report.targets = prepared.targets;
        report.description = runResult == SwarmSequenceOperationResult::SentAll
            ? QStringLiteral("Sequence layout sent by fake executor.")
            : QStringLiteral("Fake Sequence batch was partial.");
        notify();
        return runResult == SwarmSequenceOperationResult::SentAll;
    }

    bool prepareTakeoff(
        const QVector<SwarmSequenceTakeoffAssignment> &assignments,
        SwarmSequencePreparedTakeoff *prepared, QString *error) const override
    {
        ++prepareTakeoffCalls;
        if (error) {
            error->clear();
        }
        if (!prepared || assignments.isEmpty()) {
            return false;
        }
        prepared->preparationId = 42;
        prepared->assignments = assignments;
        prepared->altitudeM = 2.0;
        return true;
    }

    bool startTakeoff(const SwarmSequencePreparedTakeoff &prepared,
                      QString *error) override
    {
        ++takeoffCalls;
        lastTakeoff = prepared;
        ++generation;
        active = true;
        report = {};
        status = QStringLiteral("Taking off exact Sequence vehicles.");
        if (error) {
            error->clear();
        }
        notify();
        return true;
    }

    void cancelActiveOperation(const QString &) override
    {
        ++cancelCalls;
        active = false;
        report.operationGeneration = generation;
        report.operation = SwarmSequenceOperation::Takeoff;
        report.result = SwarmSequenceOperationResult::Cancelled;
        report.description = QStringLiteral("Fake takeoff cancelled.");
        notify();
    }

    bool isActive() const noexcept override { return active; }
    SwarmSequenceExecutor::State state() const noexcept override
    {
        return active ? SwarmSequenceExecutor::State::TakingOff
                      : SwarmSequenceExecutor::State::Idle;
    }
    QString statusText() const override { return status; }
    quint64 operationGeneration() const noexcept override
    {
        return generation;
    }
    SwarmSequenceOperationReport lastReport() const override
    {
        return report;
    }
    void setChangedHandler(ChangedHandler value) override
    {
        handler = std::move(value);
    }

    void finishTakeoff()
    {
        active = false;
        report.operationGeneration = generation;
        report.operation = SwarmSequenceOperation::Takeoff;
        report.result = SwarmSequenceOperationResult::SentAll;
        report.description = QStringLiteral("Fake takeoff complete.");
        status = report.description;
        notify();
    }

    void notify() const
    {
        if (handler) {
            handler();
        }
    }

    mutable int prepareRunCalls = 0;
    mutable int prepareTakeoffCalls = 0;
    int runCalls = 0;
    int takeoffCalls = 0;
    int cancelCalls = 0;
    bool ready = true;
    bool active = false;
    quint64 generation = 0;
    SwarmSequenceOperationResult runResult =
        SwarmSequenceOperationResult::SentAll;
    QString status = QStringLiteral("Fake Sequence executor is idle.");
    SwarmSequencePreparedTakeoff lastTakeoff;
    SwarmSequenceOperationReport report;
    ChangedHandler handler;
};
}

class SwarmSequenceWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void rendersCompleteOfflineEditorWithoutVehicles();
    void editsLoadsAndAtomicallySavesOfficialDocument();
    void discoversOnlyExactLiveArduCopters();
    void runsConfirmedStepAndStartsExactTakeoff();
    void rejectsConfirmationMutationAndCancelsOwnedTakeoffOnClose();
    void singletonReactivatesExistingWindow();
};

void SwarmSequenceWindowTest::rendersCompleteOfflineEditorWithoutVehicles()
{
    SwarmSequenceWindow window(nullptr, SwarmSequenceWindow::Dependencies());
    QCOMPARE(window.size(), QSize(1380, 820));
    QCOMPARE(window.minimumSize(), QSize(1080, 680));
    QVERIFY(window.isWindow());
    QVERIFY(window.isModal() == false);

    required<QLabel>(&window, QStringLiteral("sequenceDangerBanner"));
    required<QPushButton>(&window, QStringLiteral("sequenceLoad"));
    required<QPushButton>(&window, QStringLiteral("sequenceSave"));
    required<QComboBox>(&window, QStringLiteral("sequenceLayoutCombo"));
    QSpinBox *count = required<QSpinBox>(
        &window, QStringLiteral("sequenceVehicleCount"));
    QCOMPARE(count->minimum(), 1);
    QCOMPARE(count->maximum(), 255);
    QCOMPARE(count->value(), 1);
    required<QPushButton>(&window, QStringLiteral("sequenceNewLayout"));
    required<QPushButton>(&window, QStringLiteral("sequenceAddStep"));
    required<QPushButton>(&window, QStringLiteral("sequenceRefreshVehicles"));
    required<QPushButton>(&window, QStringLiteral("sequenceBackgroundImage"));
    required<SequenceLayoutControl>(
        &window, QStringLiteral("SequenceLayoutControl"));
    required<QTableWidget>(&window, QStringLiteral("sequenceOffsetTable"));
    required<QListWidget>(&window, QStringLiteral("sequenceSteps"));
    required<QComboBox>(&window, QStringLiteral("sequenceAnchor"));
    required<QTableWidget>(&window, QStringLiteral("sequenceAssignmentTable"));

    QPushButton *run = required<QPushButton>(
        &window, QStringLiteral("sequenceRunStep"));
    QPushButton *takeoff = required<QPushButton>(
        &window, QStringLiteral("sequenceTakeoff"));
    QPushButton *reset = required<QPushButton>(
        &window, QStringLiteral("sequenceReset"));
    QVERIFY(!run->isEnabled());
    QVERIFY(!takeoff->isEnabled());
    QVERIFY(reset->isEnabled());
    QVERIFY(run->toolTip().contains(
        QStringLiteral("application exact-link executor")));
    QCOMPARE(run->toolTip(), takeoff->toolTip());
    QCOMPARE(window.statusText(), QStringLiteral(
        "No live ArduCopter autopilots were found across open MAVLink links."));
    QCOMPARE(required<QLabel>(&window,
        QStringLiteral("sequenceStepDisplay"))->text(),
        QStringLiteral("Step 0 / 0"));
    QCOMPARE(required<QLabel>(&window,
        QStringLiteral("sequenceOriginDisplay"))->text(),
        QStringLiteral("Origin not captured"));
}

void SwarmSequenceWindowTest::runsConfirmedStepAndStartsExactTakeoff()
{
    qint64 now = 100;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 firstSession = registry.beginLinkSession(
        20, QStringLiteral("Sequence A"));
    const quint64 secondSession = registry.beginLinkSession(
        21, QStringLiteral("Sequence B"));
    QVERIFY(registry.observeMessage(20, firstSession, heartbeat(7)));
    QVERIFY(registry.observeMessage(21, secondSession, heartbeat(9)));

    FakeSequenceInterface interface;
    SwarmSequenceWindow::Dependencies dependencies;
    dependencies.confirmDangerous = [](
        QWidget *, const QString &, const QString &, const QString &) {
        return true;
    };
    SwarmSequenceWindow window(
        &registry, &interface, dependencies);
    QVERIFY(window.setDocument(sampleDocument()));
    QPushButton *run = required<QPushButton>(
        &window, QStringLiteral("sequenceRunStep"));
    QPushButton *takeoff = required<QPushButton>(
        &window, QStringLiteral("sequenceTakeoff"));
    QVERIFY(run->isEnabled());
    QVERIFY(takeoff->isEnabled());

    run->click();
    QCOMPARE(interface.runCalls, 1);
    QCOMPARE(required<QLabel>(&window,
        QStringLiteral("sequenceStepDisplay"))->text(),
        QStringLiteral("Step 2 / 2"));
    QVERIFY(required<QLabel>(&window,
        QStringLiteral("sequenceOriginDisplay"))->text().startsWith(
            QStringLiteral("Origin 35")));
    QTableWidget *table = required<QTableWidget>(
        &window, QStringLiteral("sequenceAssignmentTable"));
    QVERIFY(table->item(0, 2)->text() != QStringLiteral("—"));
    QVERIFY(table->item(1, 2)->text() != QStringLiteral("—"));

    window.refreshVehicles();
    QCOMPARE(required<QLabel>(&window,
        QStringLiteral("sequenceOriginDisplay"))->text(),
        QStringLiteral("Origin not captured"));
    QCOMPARE(required<QLabel>(&window,
        QStringLiteral("sequenceStepDisplay"))->text(),
        QStringLiteral("Step 2 / 2"));

    takeoff->click();
    QCOMPARE(interface.takeoffCalls, 1);
    QCOMPARE(interface.lastTakeoff.assignments.size(), 2);
    QVERIFY(interface.active);
    QVERIFY(!run->isEnabled());
    QVERIFY(!takeoff->isEnabled());
    interface.finishTakeoff();
    QVERIFY(run->isEnabled());
    QVERIFY(takeoff->isEnabled());
    QCOMPARE(window.statusText(), QStringLiteral("Fake takeoff complete."));
}

void SwarmSequenceWindowTest::
rejectsConfirmationMutationAndCancelsOwnedTakeoffOnClose()
{
    qint64 now = 100;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 firstSession = registry.beginLinkSession(
        20, QStringLiteral("Sequence A"));
    const quint64 secondSession = registry.beginLinkSession(
        21, QStringLiteral("Sequence B"));
    QVERIFY(registry.observeMessage(20, firstSession, heartbeat(7)));
    QVERIFY(registry.observeMessage(21, secondSession, heartbeat(9)));

    FakeSequenceInterface interface;
    SwarmSequenceWindow *window = nullptr;
    SwarmSequenceWindow::Dependencies dependencies;
    dependencies.confirmDangerous = [&window](
        QWidget *, const QString &, const QString &, const QString &) {
        QTableWidget *table = required<QTableWidget>(
            window, QStringLiteral("sequenceAssignmentTable"));
        auto *assignment = qobject_cast<QComboBox *>(
            table->cellWidget(0, 1));
        assignment->setCurrentIndex(0);
        return true;
    };
    SwarmSequenceWindow actual(
        &registry, &interface, dependencies);
    window = &actual;
    QVERIFY(actual.setDocument(sampleDocument()));
    required<QPushButton>(&actual,
        QStringLiteral("sequenceRunStep"))->click();
    QCOMPARE(interface.runCalls, 0);
    QVERIFY(actual.statusText().contains(
        QStringLiteral("changed while confirmation")));

    FakeSequenceInterface anchorInterface;
    SwarmSequenceWindow *anchorWindow = nullptr;
    SwarmSequenceWindow::Dependencies changeAnchor;
    changeAnchor.confirmDangerous = [&anchorWindow](
        QWidget *, const QString &, const QString &, const QString &) {
        QComboBox *anchor = required<QComboBox>(
            anchorWindow, QStringLiteral("sequenceAnchor"));
        const int original = anchor->currentIndex();
        anchor->setCurrentIndex(original == 0 ? 1 : 0);
        anchor->setCurrentIndex(original);
        return true;
    };
    SwarmSequenceWindow anchorActual(
        &registry, &anchorInterface, changeAnchor);
    anchorWindow = &anchorActual;
    QVERIFY(anchorActual.setDocument(sampleDocument()));
    required<QPushButton>(&anchorActual,
        QStringLiteral("sequenceRunStep"))->click();
    QCOMPARE(anchorInterface.runCalls, 0);
    QVERIFY(anchorActual.statusText().contains(
        QStringLiteral("changed while confirmation")));

    QTableWidget *table = required<QTableWidget>(
        &actual, QStringLiteral("sequenceAssignmentTable"));
    auto *assignment = qobject_cast<QComboBox *>(table->cellWidget(0, 1));
    assignment->setCurrentIndex(1);
    dependencies.confirmDangerous = {};
    // The window keeps its injected function, so start directly with a fresh
    // always-accepting window to exercise matching-operation close cleanup.
    SwarmSequenceWindow::Dependencies accept;
    accept.confirmDangerous = [](
        QWidget *, const QString &, const QString &, const QString &) {
        return true;
    };
    SwarmSequenceWindow closing(&registry, &interface, accept);
    QVERIFY(closing.setDocument(sampleDocument()));
    required<QPushButton>(&closing,
        QStringLiteral("sequenceTakeoff"))->click();
    QVERIFY(interface.active);
    closing.close();
    QCOMPARE(interface.cancelCalls, 1);
    QVERIFY(!interface.active);
}

void SwarmSequenceWindowTest::editsLoadsAndAtomicallySavesOfficialDocument()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString input = temporary.filePath(QStringLiteral("input.json"));
    const QString output = temporary.filePath(QStringLiteral("output.txt"));
    QVERIFY(SwarmSequenceFile::save(input, sampleDocument()));

    QString requestedName;
    SwarmSequenceWindow::Dependencies dependencies;
    dependencies.requestLayoutName = [&requestedName](QWidget *,
                                                       const QString &suggested) {
        requestedName = suggested;
        return QStringLiteral("Line Copy");
    };
    SwarmSequenceWindow window(nullptr, dependencies);
    QSignalSpy changed(&window, &SwarmSequenceWindow::documentChanged);
    QVERIFY(window.loadPath(input));
    QCOMPARE(window.document().layouts.size(), 1);
    QCOMPARE(window.document().steps.size(), 2);
    QCOMPARE(window.document().layouts.first().delayStart, 3);

    QTableWidget *offsets = required<QTableWidget>(
        &window, QStringLiteral("sequenceOffsetTable"));
    QCOMPARE(offsets->rowCount(), 2);
    offsets->item(0, 1)->setText(QStringLiteral("12.25"));
    QCOMPARE(window.document().layouts.first().offsets.value(7).x, 12.25);

    required<QPushButton>(&window,
        QStringLiteral("sequenceNewLayout"))->click();
    QCOMPARE(requestedName, QStringLiteral("Layout 1"));
    QCOMPARE(window.document().layouts.size(), 2);
    QCOMPARE(window.document().layouts.at(1).id, QStringLiteral("Line Copy"));
    QCOMPARE(window.document().layouts.at(1).offsets.value(7).x, 12.25);
    required<QPushButton>(&window,
        QStringLiteral("sequenceAddStep"))->click();
    QCOMPARE(window.document().steps.last(), QStringLiteral("Line Copy"));
    QCOMPARE(required<QListWidget>(&window,
        QStringLiteral("sequenceSteps"))->count(), 3);

    QVERIFY(window.savePath(output));
    QVERIFY(QFile::exists(output));
    const SwarmSequenceLoadResult roundTrip = SwarmSequenceFile::load(output);
    QVERIFY(roundTrip.isValid());
    QCOMPARE(roundTrip.document.layouts.size(), 2);
    QCOMPARE(roundTrip.document.steps.size(), 3);
    QVERIFY(changed.count() >= 3);

    const SwarmSequenceDocument before = window.document();
    const SwarmSequenceIssue rejected = window.loadPath(
        temporary.filePath(QStringLiteral("missing.json")));
    QVERIFY(!rejected);
    QCOMPARE(window.document().layouts.size(), before.layouts.size());
    QCOMPARE(window.document().steps, before.steps);

    required<QPushButton>(&window,
        QStringLiteral("sequenceReset"))->click();
    QVERIFY(window.statusText().contains(QStringLiteral("Sequence reset")));
    QCOMPARE(required<QLabel>(&window,
        QStringLiteral("sequenceStepDisplay"))->text(),
        QStringLiteral("Step 1 / 3"));
}

void SwarmSequenceWindowTest::discoversOnlyExactLiveArduCopters()
{
    qint64 now = 100;
    SwarmTelemetryRegistry registry([&now]() { return now; });
    const quint64 firstSession = registry.beginLinkSession(
        10, QStringLiteral("Radio A"));
    const quint64 secondSession = registry.beginLinkSession(
        11, QStringLiteral("Radio B"));
    const quint64 planeSession = registry.beginLinkSession(
        12, QStringLiteral("Plane"));
    const quint64 px4Session = registry.beginLinkSession(
        13, QStringLiteral("PX4"));
    QVERIFY(registry.observeMessage(10, firstSession, heartbeat(7)));
    QVERIFY(registry.observeMessage(11, secondSession, heartbeat(7)));
    QVERIFY(registry.observeMessage(12, planeSession,
                                    heartbeat(9, MAV_TYPE_FIXED_WING)));
    QVERIFY(registry.observeMessage(13, px4Session,
        heartbeat(9, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_PX4)));

    SwarmSequenceWindow window(&registry, SwarmSequenceWindow::Dependencies());
    QVERIFY(window.setDocument(sampleDocument()));
    window.refreshVehicles();
    QComboBox *anchor = required<QComboBox>(
        &window, QStringLiteral("sequenceAnchor"));
    QCOMPARE(anchor->count(), 2);
    QVERIFY(anchor->itemText(0).contains(QStringLiteral("Radio A")));
    QVERIFY(anchor->itemText(1).contains(QStringLiteral("Radio B")));

    QTableWidget *assignments = required<QTableWidget>(
        &window, QStringLiteral("sequenceAssignmentTable"));
    QCOMPARE(assignments->rowCount(), 2);
    auto *seven = qobject_cast<QComboBox *>(assignments->cellWidget(0, 1));
    auto *nine = qobject_cast<QComboBox *>(assignments->cellWidget(1, 1));
    QVERIFY(seven);
    QVERIFY(nine);
    QCOMPARE(seven->currentIndex(), 0); // duplicate sysid is never guessed
    QCOMPARE(nine->currentIndex(), 0);  // Plane and PX4 are excluded

    QVERIFY(registry.endLinkSession(11, secondSession));
    window.refreshVehicles();
    assignments = required<QTableWidget>(
        &window, QStringLiteral("sequenceAssignmentTable"));
    seven = qobject_cast<QComboBox *>(assignments->cellWidget(0, 1));
    QVERIFY(seven);
    QCOMPARE(seven->currentIndex(), 1); // unique exact endpoint auto-matches
    QVERIFY(window.statusText().contains(QStringLiteral("Found 1")));

    const qulonglong originalInstance = seven->currentData(
        SwarmSequenceWindow::InstanceEpochRole).toULongLong();
    QVERIFY(originalInstance != 0);
    QVERIFY(registry.observeMessage(
        10, firstSession, heartbeat(7, MAV_TYPE_FIXED_WING)));
    QCOMPARE(anchor->count(), 0); // eligibility follows the current heartbeat
    assignments = required<QTableWidget>(
        &window, QStringLiteral("sequenceAssignmentTable"));
    seven = qobject_cast<QComboBox *>(assignments->cellWidget(0, 1));
    QVERIFY(seven);
    QCOMPARE(seven->currentIndex(), 0);

    QVERIFY(registry.observeMessage(10, firstSession, heartbeat(7)));
    QCOMPARE(anchor->count(), 1);
    assignments = required<QTableWidget>(
        &window, QStringLiteral("sequenceAssignmentTable"));
    seven = qobject_cast<QComboBox *>(assignments->cellWidget(0, 1));
    QVERIFY(seven);
    QCOMPARE(seven->currentIndex(), 1);
    QCOMPARE(seven->currentData(
        SwarmSequenceWindow::InstanceEpochRole).toULongLong(),
        originalInstance);

    QVERIFY(registry.endLinkSession(10, firstSession));
    const quint64 replacementSession = registry.beginLinkSession(
        10, QStringLiteral("Radio A"));
    QVERIFY(registry.observeMessage(10, replacementSession, heartbeat(7)));
    assignments = required<QTableWidget>(
        &window, QStringLiteral("sequenceAssignmentTable"));
    seven = qobject_cast<QComboBox *>(assignments->cellWidget(0, 1));
    QVERIFY(seven);
    QCOMPARE(seven->currentIndex(), 1);
    QVERIFY(seven->currentData(
        SwarmSequenceWindow::InstanceEpochRole).toULongLong()
        != originalInstance);
}

void SwarmSequenceWindowTest::singletonReactivatesExistingWindow()
{
    QPointer<SwarmSequenceWindow> first = SwarmSequenceWindow::OpenWindow();
    QVERIFY(first);
    QCOMPARE(SwarmSequenceWindow::OpenWindow(), first.data());
    first->close();
    QTRY_VERIFY(first.isNull());
}

QTEST_MAIN(SwarmSequenceWindowTest)
#include "test_swarmsequencewindow.moc"
