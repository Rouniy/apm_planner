#include <QtTest>

#include "comm/ExactLinkTransmitter.h"
#include "comm/GuidedTargetService.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "ui/ExternalGuidedWindow.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSemaphore>
#include <QVector>

#include <atomic>
#include <memory>

#include <mavlink.h>

namespace
{
ExternalGuidedFileResult waypointResult(
    const QString &path, double latitude = 34.1234567,
    double longitude = 33.1234567, double altitude = 50.0)
{
    ExternalGuidedFileResult result = ExternalGuidedFile::parse(
        QStringLiteral("%1,%2,%3")
            .arg(latitude, 0, 'f', 7)
            .arg(longitude, 0, 'f', 7)
            .arg(altitude, 0, 'f', 2));
    result.absolutePath = path;
    return result;
}

struct GuidedRig
{
    QVector<QByteArray> frames;
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter;
    VehicleCommandService commands;
    GuidedTargetService guided;
    VehicleEndpoint endpoint;

    explicit GuidedRig(bool selectVehicle = true)
        : transmitter(
              [this](int, const QByteArray &frame) {
                  frames.append(frame);
                  return true;
              })
        , commands(&targets, &transmitter)
        , guided(&targets, &commands)
    {
        endpoint.linkId = 7;
        endpoint.systemId = 42;
        endpoint.componentId = 1;
        endpoint.linkName = QStringLiteral("Test modem");
        endpoint.componentName = QStringLiteral("AUTOPILOT1");
        if (selectVehicle) {
            targets.observeEndpoint(endpoint, true);
            targets.observeHeartbeat(
                endpoint, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
                MAV_TYPE_QUADROTOR);
        }
        guided.setCommandTimeoutForTesting(250);
    }

    void acknowledge(quint8 result = MAV_RESULT_ACCEPTED)
    {
        mavlink_command_ack_t acknowledgement{};
        acknowledgement.command = MAV_CMD_DO_REPOSITION;
        acknowledgement.result = result;
        acknowledgement.progress = 100;
        acknowledgement.target_system = 255;
        acknowledgement.target_component = MAV_COMP_ID_MISSIONPLANNER;
        mavlink_message_t message{};
        mavlink_msg_command_ack_encode(
            static_cast<quint8>(endpoint.systemId),
            static_cast<quint8>(endpoint.componentId),
            &message, &acknowledgement);
        commands.observeMessage(endpoint.linkId, message);
    }
};

ExternalGuidedWindow::Dependencies dependenciesFor(
    GuidedRig *rig, int intervalMs = 1000)
{
    ExternalGuidedWindow::Dependencies dependencies;
    dependencies.targetManager = &rig->targets;
    dependencies.guidedService = &rig->guided;
    dependencies.updateIntervalMs = intervalMs;
    dependencies.readFile = [](const QString &path) {
        return waypointResult(path);
    };
    dependencies.confirmStart = [](
        QWidget *, const QString &, const QString &) { return true; };
    return dependencies;
}
}

class ExternalGuidedWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void rendersMissionPlannerContractAndIndependentWindows();
    void omittedConfirmationFailsClosed();
    void validatesPathAndRequiresExactTarget();
    void rereadsAfterConfirmationAndPollsOnlyAfterAck();
    void sharedSessionIsBusyAndTargetSwitchStopsOwner();
    void rejectedCommandStopsVisibly();
    void stopPendingRecoversWithoutPermanentBusy();
    void stopDuringDelayedReadDiscardsLateResult();
};

void ExternalGuidedWindowTest::
rendersMissionPlannerContractAndIndependentWindows()
{
    GuidedRig rig;
    ExternalGuidedWindow::Dependencies dependencies = dependenciesFor(&rig);
    auto *first = new ExternalGuidedWindow(dependencies);
    auto *second = new ExternalGuidedWindow(dependencies);
    QPointer<ExternalGuidedWindow> guardedFirst(first);
    QPointer<ExternalGuidedWindow> guardedSecond(second);

    QCOMPARE(first->objectName(), QStringLiteral("ExternalGuidedWindow"));
    QCOMPARE(first->windowTitle(), QStringLiteral("External Guided"));
    QCOMPARE(first->windowModality(), Qt::NonModal);
    QVERIFY(first->testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(first->size(), QSize(ExternalGuidedWindow::WindowWidth,
                                  ExternalGuidedWindow::WindowHeight));
    QCOMPARE(first->minimumSize(),
             QSize(ExternalGuidedWindow::MinimumWindowWidth,
                   ExternalGuidedWindow::MinimumWindowHeight));
    QVERIFY(first->styleSheet().contains(QStringLiteral("#434445")));

    const QStringList requiredControls = {
        QStringLiteral("externalGuidedHeader"),
        QStringLiteral("externalGuidedDescription"),
        QStringLiteral("externalGuidedTargetDescription"),
        QStringLiteral("externalGuidedFileLabel"),
        QStringLiteral("externalGuidedFilePath"),
        QStringLiteral("externalGuidedBrowse"),
        QStringLiteral("externalGuidedExample"),
        QStringLiteral("ToggleExternalGuidedButton"),
        QStringLiteral("externalGuidedStatusPanel"),
        QStringLiteral("externalGuidedStatus"),
        QStringLiteral("externalGuidedLastAcceptedCaption"),
        QStringLiteral("externalGuidedLocationLabel")
    };
    for (const QString &name : requiredControls) {
        QVERIFY2(first->findChild<QWidget *>(name), qPrintable(name));
    }
    auto *path = first->findChild<QLineEdit *>(
        QStringLiteral("externalGuidedFilePath"));
    auto *toggle = first->findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"));
    QVERIFY(path && path->isEnabled());
    QCOMPARE(path->maxLength(), ExternalGuidedWindow::MaximumPathCharacters);
    QVERIFY(toggle && toggle->isEnabled());
    QCOMPARE(toggle->text(), QStringLiteral("Start"));
    QCOMPARE(first->statusText(), QStringLiteral("Stopped."));
    QCOMPARE(first->lastAcceptedText(),
             QStringLiteral("No file target read."));

    first->show();
    second->show();
    QVERIFY(first->isWindow() && first->isVisible());
    QVERIFY(second->isWindow() && second->isVisible());
    QVERIFY(first != second);
    first->close();
    QTRY_VERIFY(guardedFirst.isNull());
    QVERIFY(guardedSecond && guardedSecond->isVisible());
    second->close();
    QTRY_VERIFY(guardedSecond.isNull());
}

void ExternalGuidedWindowTest::omittedConfirmationFailsClosed()
{
    GuidedRig rig;
    ExternalGuidedWindow::Dependencies dependencies = dependenciesFor(&rig);
    dependencies.confirmStart = {};
    ExternalGuidedWindow window(dependencies);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.findChild<QLineEdit *>(
        QStringLiteral("externalGuidedFilePath"))
        ->setText(QStringLiteral("/tmp/external-guided-safe-default.txt"));

    window.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"))->click();

    QTRY_VERIFY(window.statusText().contains(
        QStringLiteral("cancelled"), Qt::CaseInsensitive));
    QVERIFY(!window.isRunning());
    QCOMPARE(rig.frames.size(), 0);
    QVERIFY(!rig.guided.hasActiveSession());
}

void ExternalGuidedWindowTest::validatesPathAndRequiresExactTarget()
{
    GuidedRig rig;
    ExternalGuidedWindow::Dependencies dependencies = dependenciesFor(&rig);
    ExternalGuidedWindow window(dependencies);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    auto *path = window.findChild<QLineEdit *>(
        QStringLiteral("externalGuidedFilePath"));
    auto *toggle = window.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"));

    toggle->click();
    QVERIFY(window.statusText().contains(
        QStringLiteral("Select an existing")));

    dependencies.readFile = {};
    ExternalGuidedWindow missingFileWindow(dependencies);
    missingFileWindow.setAttribute(Qt::WA_DeleteOnClose, false);
    missingFileWindow.findChild<QLineEdit *>(
        QStringLiteral("externalGuidedFilePath"))
        ->setText(QStringLiteral(
            "/definitely/not/an/external-guided-target.txt"));
    missingFileWindow.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"))->click();
    QTRY_VERIFY(missingFileWindow.statusText().contains(
        QStringLiteral("does not exist"), Qt::CaseInsensitive));
    QCOMPARE(rig.frames.size(), 0);

    GuidedRig noTarget(false);
    int confirmations = 0;
    ExternalGuidedWindow::Dependencies noTargetDependencies =
        dependenciesFor(&noTarget);
    noTargetDependencies.confirmStart =
        [&confirmations](QWidget *, const QString &, const QString &) {
            ++confirmations;
            return true;
        };
    ExternalGuidedWindow noTargetWindow(noTargetDependencies);
    noTargetWindow.setAttribute(Qt::WA_DeleteOnClose, false);
    noTargetWindow.findChild<QLineEdit *>(
        QStringLiteral("externalGuidedFilePath"))
        ->setText(QStringLiteral("/tmp/external-guided-no-target.txt"));
    noTargetWindow.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"))->click();
    QTRY_VERIFY(noTargetWindow.statusText().contains(
        QStringLiteral("Connect and select")));
    QCOMPARE(confirmations, 0);
    QCOMPARE(noTarget.frames.size(), 0);
}

void ExternalGuidedWindowTest::
rereadsAfterConfirmationAndPollsOnlyAfterAck()
{
    GuidedRig rig;
    std::atomic_int reads{0};
    int confirmations = 0;
    ExternalGuidedWindow::Dependencies dependencies =
        dependenciesFor(&rig, 20);
    dependencies.readFile = [&reads](const QString &path) {
        const int number = ++reads;
        return waypointResult(path, 10.0 + qMin(number, 3), 22.0, 40.0);
    };
    dependencies.confirmStart =
        [&confirmations](QWidget *, const QString &, const QString &) {
            ++confirmations;
            return true;
        };
    ExternalGuidedWindow window(dependencies);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    auto *path = window.findChild<QLineEdit *>(
        QStringLiteral("externalGuidedFilePath"));
    auto *toggle = window.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"));
    path->setText(QStringLiteral("/tmp/external-guided-cadence.txt"));

    toggle->click();
    QTRY_COMPARE(rig.frames.size(), 1);
    QTRY_COMPARE(reads.load(), 2);
    QCOMPARE(confirmations, 1);
    QVERIFY(window.isRunning());
    QVERIFY(!path->isEnabled());
    QCOMPARE(toggle->text(), QStringLiteral("Stop"));
    QCOMPARE(window.lastAcceptedText(),
             QStringLiteral("No file target read."));

    // No periodic file read may race the first pending COMMAND_ACK.
    QTest::qWait(50);
    QCOMPARE(reads.load(), 2);
    QCOMPARE(rig.frames.size(), 1);

    rig.acknowledge();
    QTRY_VERIFY(window.lastAcceptedText().contains(
        QStringLiteral("12.0000000")));
    QTRY_VERIFY(reads.load() >= 3);
    QTRY_COMPARE(rig.frames.size(), 2);
    QVERIFY(window.lastAcceptedText().contains(
        QStringLiteral("12.0000000")));

    rig.acknowledge();
    QTRY_VERIFY(window.lastAcceptedText().contains(
        QStringLiteral("13.0000000")));
    toggle->click();
    QTRY_VERIFY(!window.isRunning());
    QCOMPARE(window.statusText(), QStringLiteral("Stopped."));
    QVERIFY(path->isEnabled());
}

void ExternalGuidedWindowTest::
sharedSessionIsBusyAndTargetSwitchStopsOwner()
{
    GuidedRig rig;
    ExternalGuidedWindow::Dependencies dependencies =
        dependenciesFor(&rig, 10000);
    ExternalGuidedWindow first(dependencies);
    ExternalGuidedWindow second(dependencies);
    first.setAttribute(Qt::WA_DeleteOnClose, false);
    second.setAttribute(Qt::WA_DeleteOnClose, false);
    first.findChild<QLineEdit *>(QStringLiteral("externalGuidedFilePath"))
        ->setText(QStringLiteral("/tmp/external-guided-first.txt"));
    second.findChild<QLineEdit *>(QStringLiteral("externalGuidedFilePath"))
        ->setText(QStringLiteral("/tmp/external-guided-second.txt"));

    first.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"))->click();
    QTRY_COMPARE(rig.frames.size(), 1);
    rig.acknowledge();
    QTRY_VERIFY(first.lastAcceptedText().contains(
        QStringLiteral("34.1234567")));

    second.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"))->click();
    QTRY_VERIFY(second.statusText().contains(
        QStringLiteral("another window"), Qt::CaseInsensitive));
    QVERIFY(first.isRunning());
    QVERIFY(!second.isRunning());
    QCOMPARE(rig.frames.size(), 1);

    VehicleEndpoint replacement = rig.endpoint;
    replacement.linkId = 8;
    replacement.linkName = QStringLiteral("Other modem");
    rig.targets.observeEndpoint(replacement, false);
    rig.targets.observeHeartbeat(
        replacement, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
        MAV_TYPE_QUADROTOR);
    QVERIFY(rig.targets.selectTarget(
        replacement.linkId, replacement.systemId,
        replacement.componentId));

    QTRY_VERIFY(!first.isRunning());
    QVERIFY(first.statusText().contains(
        QStringLiteral("changed"), Qt::CaseInsensitive));
    QCOMPARE(rig.frames.size(), 1);
}

void ExternalGuidedWindowTest::rejectedCommandStopsVisibly()
{
    GuidedRig rig;
    ExternalGuidedWindow::Dependencies dependencies = dependenciesFor(&rig);
    ExternalGuidedWindow window(dependencies);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.findChild<QLineEdit *>(QStringLiteral("externalGuidedFilePath"))
        ->setText(QStringLiteral("/tmp/external-guided-rejected.txt"));
    window.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"))->click();
    QTRY_COMPARE(rig.frames.size(), 1);

    rig.acknowledge(MAV_RESULT_UNSUPPORTED);

    QTRY_VERIFY(!window.isRunning());
    QVERIFY(window.statusText().contains(
        QStringLiteral("does not support"), Qt::CaseInsensitive)
        || window.statusText().contains(
            QStringLiteral("terminal result"), Qt::CaseInsensitive));
}

void ExternalGuidedWindowTest::stopPendingRecoversWithoutPermanentBusy()
{
    GuidedRig rig;
    rig.guided.setCommandTimeoutForTesting(500);
    rig.guided.setRecoveryQuarantineForTesting(40);
    ExternalGuidedWindow::Dependencies dependencies =
        dependenciesFor(&rig, 10000);
    ExternalGuidedWindow first(dependencies);
    ExternalGuidedWindow second(dependencies);
    first.setAttribute(Qt::WA_DeleteOnClose, false);
    second.setAttribute(Qt::WA_DeleteOnClose, false);
    auto *firstToggle = first.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"));
    auto *secondToggle = second.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"));
    first.findChild<QLineEdit *>(QStringLiteral("externalGuidedFilePath"))
        ->setText(QStringLiteral("/tmp/external-guided-stop-pending.txt"));
    second.findChild<QLineEdit *>(QStringLiteral("externalGuidedFilePath"))
        ->setText(QStringLiteral("/tmp/external-guided-recover.txt"));

    firstToggle->click();
    QTRY_COMPARE(rig.frames.size(), 1);
    QVERIFY(first.isRunning());
    firstToggle->click();
    QTRY_VERIFY(!first.isRunning());
    QCOMPARE(first.statusText(), QStringLiteral("Stopped."));
    QVERIFY(rig.guided.isOutcomeUncertain(rig.endpoint));

    secondToggle->click();
    QTRY_VERIFY(second.statusText().contains(
        QStringLiteral("uncertain"), Qt::CaseInsensitive));
    QVERIFY(!second.isRunning());
    QCOMPARE(rig.frames.size(), 1);

    QTRY_VERIFY_WITH_TIMEOUT(
        !rig.guided.isOutcomeUncertain(rig.endpoint), 250);
    secondToggle->click();
    QTRY_COMPARE(rig.frames.size(), 2);
    QVERIFY(second.isRunning());
    rig.acknowledge();
    QTRY_VERIFY(second.lastAcceptedText().contains(
        QStringLiteral("34.1234567")));
    secondToggle->click();
    QTRY_VERIFY(!second.isRunning());
}

void ExternalGuidedWindowTest::
stopDuringDelayedReadDiscardsLateResult()
{
    GuidedRig rig;
    auto entered = std::make_shared<QSemaphore>();
    auto release = std::make_shared<QSemaphore>();
    std::atomic_int reads{0};
    ExternalGuidedWindow::Dependencies dependencies =
        dependenciesFor(&rig, 10);
    dependencies.readFile =
        [entered, release, &reads](const QString &path) {
            const int number = ++reads;
            if (number >= 3) {
                entered->release();
                release->acquire();
            }
            return waypointResult(path, 30.0 + number, 20.0, 60.0);
        };
    ExternalGuidedWindow window(dependencies);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    auto *toggle = window.findChild<QPushButton *>(
        QStringLiteral("ToggleExternalGuidedButton"));
    window.findChild<QLineEdit *>(QStringLiteral("externalGuidedFilePath"))
        ->setText(QStringLiteral("/tmp/external-guided-delayed.txt"));
    toggle->click();
    QTRY_COMPARE(rig.frames.size(), 1);
    rig.acknowledge();
    QTRY_VERIFY_WITH_TIMEOUT(entered->available() > 0, 1000);
    QVERIFY(entered->tryAcquire());
    QCOMPARE(reads.load(), 3);

    QElapsedTimer elapsed;
    elapsed.start();
    toggle->click();
    QVERIFY(elapsed.elapsed() < 700);
    QVERIFY(!window.isRunning());
    const int stoppedAt = rig.frames.size();

    release->release();
    QTest::qWait(80);
    QCOMPARE(rig.frames.size(), stoppedAt);
    QCOMPARE(window.statusText(), QStringLiteral("Stopped."));
}

QTEST_MAIN(ExternalGuidedWindowTest)

#include "test_externalguidedwindow.moc"
