#include <QtTest>

#include "comm/CompassCalibrationService.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "ui/configuration/ConfigCompassMotView.h"
#include "ui/qcustomplot.h"

#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>

#include <cstring>

namespace {

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    return state == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

mavlink_message_t commandAck(MAV_RESULT result, bool final)
{
    mavlink_command_ack_t payload{};
    payload.command = MAV_CMD_PREFLIGHT_CALIBRATION;
    payload.result = static_cast<quint8>(result);
    payload.target_system = final ? 250 : 0;
    payload.target_component = final ? MAV_COMP_ID_MISSIONPLANNER : 0;
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(42, 1, &message, &payload);
    return message;
}

mavlink_message_t motorStatus()
{
    mavlink_message_t message{};
    mavlink_msg_compassmot_status_pack(
        42, 1, &message, 375, 12.5F, 27,
        1.25F, -2.5F, 3.75F);
    return message;
}

mavlink_message_t statusText(const char *text)
{
    mavlink_statustext_t payload{};
    payload.severity = MAV_SEVERITY_INFO;
    std::strncpy(payload.text, text, sizeof(payload.text));
    mavlink_message_t message{};
    mavlink_msg_statustext_encode(42, 1, &message, &payload);
    return message;
}

struct Harness
{
    VehicleTargetManager targets;
    QVector<QByteArray> frames;
    ExactLinkTransmitter transmitter;
    VehicleCommandService commands;
    CompassCalibrationService service;

    Harness()
        : transmitter([this](int, const QByteArray &bytes) {
              frames.append(bytes);
              return true;
          })
        , commands(&targets, &transmitter)
        , service(&targets, &commands, &transmitter)
    {
        service.setLocalIdentity(250, MAV_COMP_ID_MISSIONPLANNER);
        service.setMotorTimeoutsForTesting(500, 1000, 20);
        VehicleEndpoint endpoint;
        endpoint.linkId = 9;
        endpoint.systemId = 42;
        endpoint.componentId = 1;
        endpoint.linkName = QStringLiteral("test-link");
        targets.observeEndpoint(endpoint, true);
        targets.observeHeartbeat(
            endpoint, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
        transmitter.setMotorStopLinkEligible(9, true);
    }

    VehicleTargetLease lease() const { return targets.acquireTarget(); }

    void ack(MAV_RESULT result)
    {
        const mavlink_message_t message = commandAck(result, false);
        commands.observeMessage(9, message);
        service.observeMessage(9, message);
    }

    void finalAck(MAV_RESULT result)
    {
        const mavlink_message_t message = commandAck(result, true);
        commands.observeMessage(9, message);
        service.observeMessage(9, message);
    }
};

void prepareView(ConfigCompassMotView *view, Harness *harness)
{
    view->setCalibrationContext(&harness->service, harness->lease());
    view->setSupportedVehicle(true);
    view->setParameterSnapshotReady(true);
    view->setConnected(true);
    view->setArmed(false);
    view->resize(1000, 780);
    view->show();
    QApplication::processEvents();
}

} // namespace

class ConfigCompassMotViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void surfaceIsCompleteAndSafeByDefault();
    void confirmationDefaultsToNoAndAcceptedStartIsExact();
    void liveSampleAndDeactivateDriveFullLifecycle();
    void freshnessAndPinnedRecoveryStayActionable();
    void idleDestructionSendsNothing();
};

void ConfigCompassMotViewTest::surfaceIsCompleteAndSafeByDefault()
{
    ConfigCompassMotView view;
    view.resize(1000, 780);
    view.show();
    QApplication::processEvents();

    QCOMPARE(view.objectName(), QStringLiteral("ConfigCompassMotView"));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("compassMotTitle")));
    QLabel *instructions = view.findChild<QLabel *>(
        QStringLiteral("compassMotInstructions"));
    QVERIFY(instructions);
    QVERIFY(instructions->text().contains(
        QStringLiteral("REMOVE ALL PROPELLERS")));
    QVERIFY(instructions->wordWrap());
    QVERIFY(view.findChild<QCheckBox *>(
        QStringLiteral("compassMotSafetyCheck")));
    QVERIFY(view.findChild<QCustomPlot *>(QStringLiteral("compassMotPlot")));
    QVERIFY(view.findChild<QPlainTextEdit *>(
        QStringLiteral("compassMotLog"))->isReadOnly());

    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("compassMotThrottle"))
                 ->text(), QStringLiteral("0 %"));
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("compassMotCurrent"))
                 ->text(), QStringLiteral("0.00 A"));
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("compassMotInterference"))
                 ->text(), QStringLiteral("0 %"));
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("compassMotCompensation"))
                 ->text(), QStringLiteral("0.00, 0.00, 0.00"));
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("compassMotStart"))->isEnabled());
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("compassMotFinish"))->isEnabled());
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("compassMotClearUnsafe"))->isVisible());

    QCOMPARE(ConfigCompassMotView::SafetyConfirmationTitle(),
             QStringLiteral("Compass/Motor Safety Warning"));
    const QString warning = ConfigCompassMotView::SafetyConfirmationText();
    QVERIFY(warning.contains(QStringLiteral("ARMS THE MOTORS")));
    QVERIFY(warning.contains(QStringLiteral("REMOVE ALL PROPELLERS")));
    QVERIFY(warning.contains(QStringLiteral("dedicated direct link")));
}

void ConfigCompassMotViewTest::confirmationDefaultsToNoAndAcceptedStartIsExact()
{
    Harness harness;
    ConfigCompassMotView view;
    prepareView(&view, &harness);
    QCheckBox *safety = view.findChild<QCheckBox *>(
        QStringLiteral("compassMotSafetyCheck"));
    QPushButton *start = view.findChild<QPushButton *>(
        QStringLiteral("compassMotStart"));
    safety->setChecked(true);
    QVERIFY(start->isEnabled());

    bool inspectedNo = false;
    QTimer::singleShot(0, &view, [&inspectedNo]() {
        auto *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        QVERIFY(box);
        QCOMPARE(box->defaultButton(), box->button(QMessageBox::No));
        inspectedNo = true;
        box->button(QMessageBox::No)->click();
    });
    start->click();
    QVERIFY(inspectedNo);
    QCOMPARE(harness.frames.size(), 0);

    bool inspectedYes = false;
    QTimer::singleShot(0, &view, [&inspectedYes]() {
        auto *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        QVERIFY(box);
        inspectedYes = true;
        box->button(QMessageBox::Yes)->click();
    });
    start->click();
    QVERIFY(inspectedYes);
    QCOMPARE(harness.frames.size(), 1);
    const mavlink_message_t wire = decodeFrame(harness.frames.first());
    QCOMPARE(wire.msgid, quint32(MAVLINK_MSG_ID_COMMAND_LONG));
    mavlink_command_long_t command{};
    mavlink_msg_command_long_decode(&wire, &command);
    QCOMPARE(command.command, quint16(MAV_CMD_PREFLIGHT_CALIBRATION));
    QCOMPARE(command.target_system, quint8(42));
    QCOMPARE(command.target_component, quint8(1));
    QCOMPARE(command.param6, 1.0F);
    QVERIFY(!start->isEnabled());
}

void ConfigCompassMotViewTest::liveSampleAndDeactivateDriveFullLifecycle()
{
    Harness harness;
    ConfigCompassMotView view;
    prepareView(&view, &harness);
    QSignalSpy finished(&view, &ConfigCompassMotView::calibrationFinished);

    QCOMPARE(harness.service.startMotor(harness.lease(), false, true),
             CompassCalibrationService::RequestResult::Started);
    harness.ack(MAV_RESULT_ACCEPTED);
    harness.service.observeMessage(9, motorStatus());
    QApplication::processEvents();

    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("compassMotThrottle"))
                 ->text(), QStringLiteral("37.5 %"));
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("compassMotCurrent"))
                 ->text(), QStringLiteral("12.50 A"));
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("compassMotInterference"))
                 ->text(), QStringLiteral("27 %"));
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("compassMotCompensation"))
                 ->text(), QStringLiteral("1.25, -2.50, 3.75"));
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("compassMotFinish"))->isEnabled());

    const VehicleTargetLease activeLease = harness.lease();
    harness.targets.observeHeartbeat(
        activeLease.endpoint, true, MAV_AUTOPILOT_ARDUPILOTMEGA,
        MAV_TYPE_QUADROTOR);
    view.setArmed(true);
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("compassMotFinish"))->isEnabled());
    view.deactivate();
    QTRY_COMPARE_WITH_TIMEOUT(harness.frames.size(), 3, 200);
    for (int index = 1; index < 3; ++index) {
        const mavlink_message_t stop = decodeFrame(harness.frames.at(index));
        QCOMPARE(stop.msgid, quint32(MAVLINK_MSG_ID_COMMAND_ACK));
    }
    harness.service.observeMessage(
        9, statusText("Calibration successful"));
    harness.finalAck(MAV_RESULT_ACCEPTED);
    QTRY_COMPARE_WITH_TIMEOUT(
        harness.service.state(),
        CompassCalibrationService::State::MotorSucceeded, 200);
    QCOMPARE(finished.count(), 1);
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("compassMotState"))
        ->text().contains(QStringLiteral("succeeded")));
    QVERIFY(view.findChild<QPlainTextEdit *>(QStringLiteral("compassMotLog"))
        ->toPlainText().contains(QStringLiteral("Calibration successful")));
}

void ConfigCompassMotViewTest::freshnessAndPinnedRecoveryStayActionable()
{
    {
        Harness harness;
        harness.service.setMotorHeartbeatTimeoutForTesting(1);
        QTest::qWait(5);
        ConfigCompassMotView view;
        prepareView(&view, &harness);
        QCheckBox *safety = view.findChild<QCheckBox *>(
            QStringLiteral("compassMotSafetyCheck"));
        QPushButton *start = view.findChild<QPushButton *>(
            QStringLiteral("compassMotStart"));
        safety->setChecked(true);
        QVERIFY(!start->isEnabled());
        QVERIFY(view.findChild<QLabel *>(QStringLiteral("compassMotState"))
            ->text().contains(QStringLiteral("not ready")));

        const VehicleTargetLease lease = harness.lease();
        harness.targets.observeHeartbeat(
            lease.endpoint, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
        view.setConnected(true);
        QVERIFY(start->isEnabled());
    }

    {
        Harness harness;
        const VehicleTargetLease first = harness.lease();
        harness.service.startMotor(first, false, true);
        harness.ack(MAV_RESULT_ACCEPTED);
        harness.service.stopMotor();
        QTRY_COMPARE_WITH_TIMEOUT(harness.frames.size(), 3, 200);
        harness.finalAck(MAV_RESULT_DENIED);
        QTRY_COMPARE_WITH_TIMEOUT(
            harness.service.state(),
            CompassCalibrationService::State::MotorOutcomeUncertain, 200);

        VehicleEndpoint second;
        second.linkId = 10;
        second.systemId = 77;
        second.componentId = 1;
        second.linkName = QStringLiteral("second-link");
        harness.targets.observeEndpoint(second);
        harness.targets.observeHeartbeat(
            second, false, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
        harness.transmitter.setMotorStopLinkEligible(10, true);
        QVERIFY(harness.targets.selectTarget(10, 77, 1));

        ConfigCompassMotView view;
        view.setCalibrationContext(
            &harness.service, harness.targets.acquireTarget());
        view.setParameterSnapshotReady(true);
        view.setConnected(true);
        view.resize(1000, 780);
        view.show();
        QApplication::processEvents();
        QPushButton *finish = view.findChild<QPushButton *>(
            QStringLiteral("compassMotFinish"));
        QVERIFY(finish->isEnabled());
        QVERIFY(view.findChild<QLabel *>(QStringLiteral("compassMotTarget"))
            ->text().contains(QStringLiteral("system 42")));
        finish->click();
        QCOMPARE(harness.service.state(),
                 CompassCalibrationService::State::MotorStopPending);
        QCOMPARE(harness.frames.size(), 4);
    }
}

void ConfigCompassMotViewTest::idleDestructionSendsNothing()
{
    Harness harness;
    {
        ConfigCompassMotView view;
        prepareView(&view, &harness);
    }
    QCOMPARE(harness.frames.size(), 0);
}

QTEST_MAIN(ConfigCompassMotViewTest)
#include "test_configcompassmotview.moc"
