#include <QtTest>

#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/MavlinkComponentRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "ui/configuration/CameraProbeController.h"

#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <memory>

namespace
{
class Fixture
{
public:
    explicit Fixture(bool connected = true)
        : transmitter([this](int linkId, const QByteArray &frame) {
            if (linkId != LinkId)
                return false;
            MAVLinkFrameParser parser;
            mavlink_message_t message{};
            unsigned framing = MAVLINK_FRAMING_INCOMPLETE;
            for (char byte : frame)
                framing = parser.parseByte(quint8(byte), &message);
            if (framing != MAVLINK_FRAMING_OK
                || message.msgid != MAVLINK_MSG_ID_COMMAND_LONG) {
                malformedFrame = true;
                return false;
            }
            mavlink_command_long_t command{};
            mavlink_msg_command_long_decode(&message, &command);
            sent.append(command);
            if (autoAcknowledge) {
                const quint16 commandId = command.command;
                QTimer::singleShot(0, &commands, [this, commandId]() {
                    acknowledge(commandId, MAV_RESULT_ACCEPTED);
                });
            }
            return true;
        })
        , commands(&targets, &transmitter)
    {
        transmitter.setLinkSessionEpoch(LinkId, LinkEpoch);
        commands.setLocalIdentity(250, MAV_COMP_ID_MISSIONPLANNER);
        configured = commands.configureComponentExactTransactions(
            [this](const MavlinkComponentInstanceLease &lease) {
                return components.validateLease(lease);
            }, [](const MavlinkComponentInstanceLease &, QString *) {
                return true;
            });
        components.beginLinkSession(LinkId, LinkEpoch);
        if (connected) {
            VehicleEndpoint target;
            target.linkId = LinkId;
            target.systemId = 1;
            target.componentId = 1;
            target.linkName = QStringLiteral("Exact telemetry");
            targets.observeEndpoint(target, true);
            mavlink_message_t heartbeat{};
            mavlink_msg_heartbeat_pack(
                CameraSystem, MAV_COMP_ID_CAMERA, &heartbeat,
                MAV_TYPE_CAMERA, MAV_AUTOPILOT_INVALID, 0, 0,
                MAV_STATE_ACTIVE);
            components.observeMessage(LinkId, LinkEpoch, heartbeat);
        }
        service.reset(new CameraProbeService(
            &targets, &components, &commands,
            [](const MavlinkComponentInstanceLease &, QString *) {
                return true;
            }));
    }

    void acknowledge(quint16 command, MAV_RESULT result)
    {
        mavlink_command_ack_t payload{};
        payload.command = command;
        payload.result = static_cast<quint8>(result);
        payload.progress = 255;
        payload.target_system = 250;
        payload.target_component = MAV_COMP_ID_MISSIONPLANNER;
        mavlink_message_t message{};
        mavlink_msg_command_ack_encode(
            CameraSystem, MAV_COMP_ID_CAMERA, &message, &payload);
        commands.observeComponentMessage(LinkId, LinkEpoch, message);
    }

    static constexpr int LinkId = 7;
    static constexpr quint64 LinkEpoch = 9;
    static constexpr quint8 CameraSystem = 42;
    VehicleTargetManager targets;
    MavlinkComponentRegistry components;
    bool autoAcknowledge = false;
    bool malformedFrame = false;
    QList<mavlink_command_long_t> sent;
    ExactLinkTransmitter transmitter;
    VehicleCommandService commands;
    bool configured = false;
    std::unique_ptr<CameraProbeService> service;
};

QMessageBox *confirmation(QWidget *owner)
{
    for (auto *dialog : owner->findChildren<QMessageBox *>(
             QStringLiteral("DeveloperCameraProbeConfirmation"))) {
        if (dialog->isVisible()) return dialog;
    }
    return nullptr;
}

QProgressDialog *progress(QWidget *owner)
{
    return owner->findChild<QProgressDialog *>(
        QStringLiteral("DeveloperCameraProbeProgressDialog"));
}

void acceptProbe(QWidget *owner)
{
    QMessageBox *dialog = confirmation(owner);
    QVERIFY(dialog);
    auto *yes = dialog->button(QMessageBox::Yes);
    QVERIFY(yes);
    yes->click();
}
} // namespace

class CameraProbeControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void offlineAndMissingContextAreInformative();
    void confirmationIsExplicitAndDefaultCancel();
    void confirmedProbeRunsSixExactCommands();
    void cancellationStopsRemainingCommandsAndWaitsForTerminal();
    void staleConsentAndOwnerCloseSendNothingFurther();
    void callbacksMayDeleteController();
};

void CameraProbeControllerTest::initTestCase()
{
    qRegisterMetaType<CameraProbeService::Report>();
}

void CameraProbeControllerTest::offlineAndMissingContextAreInformative()
{
    Fixture fixture(false);
    QVERIFY(fixture.configured);
    QWidget owner;
    CameraProbeController controller(fixture.service.get(), &owner);
    QSignalSpy logs(&controller, &CameraProbeController::logMessage);
    controller.start();
    QVERIFY(!controller.busy());
    QCOMPARE(fixture.sent.size(), 0);
    QVERIFY(!logs.isEmpty());
    QVERIFY(logs.last().at(0).toString().contains(
        QStringLiteral("connect"), Qt::CaseInsensitive));
    QVERIFY(!confirmation(&owner));
}

void CameraProbeControllerTest::confirmationIsExplicitAndDefaultCancel()
{
    Fixture fixture;
    QVERIFY(fixture.configured);
    QWidget owner;
    owner.show();
    CameraProbeController controller(fixture.service.get(), &owner);
    QSignalSpy logs(&controller, &CameraProbeController::logMessage);
    controller.start();
    QMessageBox *dialog = confirmation(&owner);
    QVERIFY(dialog);
    QVERIFY(controller.busy());
    QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
    QCOMPARE(dialog->escapeButton(), dialog->button(QMessageBox::Cancel));
    QVERIFY(dialog->text().contains(QStringLiteral("42:100")));
    QVERIFY(dialog->text().contains(
        QStringLiteral("mode 0"), Qt::CaseInsensitive));
    QVERIFY(dialog->text().contains(
        QStringLiteral("stream 0"), Qt::CaseInsensitive));
    dialog->button(QMessageBox::Cancel)->click();
    QVERIFY(!controller.busy());
    QCOMPARE(fixture.sent.size(), 0);
    QVERIFY(!logs.isEmpty());
    QVERIFY(logs.last().at(0).toString().contains(
        QStringLiteral("no probe command"), Qt::CaseInsensitive));
}

void CameraProbeControllerTest::confirmedProbeRunsSixExactCommands()
{
    Fixture fixture;
    QVERIFY(fixture.configured);
    fixture.autoAcknowledge = true;
    QWidget owner;
    owner.show();
    CameraProbeController controller(fixture.service.get(), &owner);
    QSignalSpy finished(
        fixture.service.get(), &CameraProbeService::operationFinished);
    controller.start();
    acceptProbe(&owner);
    QProgressDialog *dialog = progress(&owner);
    QVERIFY(dialog);
    QCOMPARE(dialog->minimum(), 0);
    QCOMPARE(dialog->maximum(), 6);
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!controller.busy());
    QCOMPARE(fixture.sent.size(), 6);
    QVERIFY(!fixture.malformedFrame);
    const QList<MAV_CMD> expected = CameraProbeService::Commands();
    QCOMPARE(expected.size(), fixture.sent.size());
    for (int index = 0; index < expected.size(); ++index) {
        const mavlink_command_long_t &command = fixture.sent.at(index);
        QCOMPARE(command.target_system, Fixture::CameraSystem);
        QCOMPARE(command.target_component,
                 static_cast<quint8>(MAV_COMP_ID_CAMERA));
        QCOMPARE(command.command, static_cast<quint16>(expected.at(index)));
        QCOMPARE(command.confirmation, static_cast<quint8>(0));
        QCOMPARE(command.param1, 0.0f);
        QCOMPARE(command.param2, 0.0f);
        QCOMPARE(command.param3, 0.0f);
        QCOMPARE(command.param4, 0.0f);
        QCOMPARE(command.param5, 0.0f);
        QCOMPARE(command.param6, 0.0f);
        QCOMPARE(command.param7, 0.0f);
    }
    const CameraProbeService::Report report =
        qvariant_cast<CameraProbeService::Report>(finished.first().first());
    QCOMPARE(report.steps.size(), 6);
    QVERIFY(!report.cancelled);
    for (const CameraProbeService::StepResult &step : report.steps)
        QCOMPARE(step.outcome, CameraProbeService::StepOutcome::Accepted);
}

void CameraProbeControllerTest::
cancellationStopsRemainingCommandsAndWaitsForTerminal()
{
    Fixture fixture;
    QVERIFY(fixture.configured);
    // Leave enough time for the event loop to observe the first queued write
    // before its acknowledgement deadline on slower CI machines.
    fixture.service->setTimeoutsForTesting(1000, 1200);
    QWidget owner;
    owner.show();
    CameraProbeController controller(fixture.service.get(), &owner);
    QSignalSpy finished(
        fixture.service.get(), &CameraProbeService::operationFinished);
    controller.start();
    acceptProbe(&owner);
    QProgressDialog *dialog = progress(&owner);
    QVERIFY(dialog);
    QPushButton *cancelButton = dialog->findChild<QPushButton *>();
    QVERIFY(cancelButton);
    QTRY_COMPARE(fixture.sent.size(), 1);
    cancelButton->click();
    QVERIFY(controller.busy());
    QVERIFY(dialog->labelText().contains(
        QStringLiteral("already sent"), Qt::CaseInsensitive));
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!controller.busy());
    QCOMPARE(fixture.sent.size(), 1);
    const CameraProbeService::Report report =
        qvariant_cast<CameraProbeService::Report>(finished.first().first());
    QVERIFY(report.cancelled);
    QCOMPARE(report.steps.size(), 6);
    QCOMPARE(report.steps.first().outcome,
             CameraProbeService::StepOutcome::OutcomeUncertain);
    for (int index = 1; index < report.steps.size(); ++index) {
        QCOMPARE(report.steps.at(index).outcome,
                 CameraProbeService::StepOutcome::NotSent);
    }
}

void CameraProbeControllerTest::staleConsentAndOwnerCloseSendNothingFurther()
{
    Fixture fixture;
    QVERIFY(fixture.configured);
    fixture.service->setTimeoutsForTesting(1000, 1200);
    auto *owner = new QWidget;
    auto *controller = new CameraProbeController(fixture.service.get(), owner);
    owner->show();
    controller->start();
    QVERIFY(confirmation(owner));
    fixture.targets.clearTarget();
    acceptProbe(owner);
    QCOMPARE(fixture.sent.size(), 0);
    QVERIFY(!controller->busy());

    QVERIFY(fixture.targets.selectTarget(
        Fixture::LinkId, 1, 1));
    controller->start();
    QVERIFY(confirmation(owner));
    acceptProbe(owner);
    QTRY_COMPARE(fixture.sent.size(), 1);
    owner->close();
    QTRY_VERIFY(!fixture.service->busy());
    QCOMPARE(fixture.sent.size(), 1);
    delete owner;
}

void CameraProbeControllerTest::callbacksMayDeleteController()
{
    Fixture fixture;
    QVERIFY(fixture.configured);
    QWidget owner;
    QPointer<CameraProbeController> controller =
        new CameraProbeController(fixture.service.get(), &owner);
    connect(controller, &CameraProbeController::busyChanged,
            controller, [controller](bool busy) {
        if (controller && busy)
            delete controller;
    }, Qt::DirectConnection);
    controller->start();
    QVERIFY(controller.isNull());
    QCOMPARE(fixture.sent.size(), 0);
    QVERIFY(!fixture.service->busy());
}

QTEST_MAIN(CameraProbeControllerTest)
#include "test_cameraprobecontroller.moc"
