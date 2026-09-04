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

// The production application supplies this through the integration unit.
SwarmTelemetryRegistry *SwarmSequenceApplicationRegistry()
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
}

class SwarmSequenceWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void rendersCompleteOfflineEditorWithoutVehicles();
    void editsLoadsAndAtomicallySavesOfficialDocument();
    void discoversOnlyExactLiveArduCopters();
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
        QStringLiteral("exact multi-endpoint command sender")));
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
