#include <QtTest>

#include "ui/configuration/ConfigMotorTestView.h"

#include <QBuffer>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>

namespace {
constexpr int kAutopilotComponent = 1;
constexpr int kMotorTestCommand = 209;
constexpr int kAccepted = 0;
constexpr int kDenied = 2;
constexpr int kInProgress = 5;

ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:FRAME_CLASS" humanName="Frame class">
          <values>
            <value code="1">QUAD</value>
            <value code="2">HEXA</value>
          </values>
        </param>
        <param name="ArduCopter:FRAME_TYPE" humanName="Frame type">
          <values>
            <value code="0">PLUS</value>
            <value code="1">X</value>
          </values>
        </param>
        <param name="ArduCopter:Q_FRAME_CLASS" humanName="QuadPlane class">
          <values>
            <value code="2">HEXA</value>
            <value code="3">OCTA</value>
            <value code="6">HELI</value>
            <value code="7">TRI</value>
          </values>
        </param>
        <param name="ArduCopter:Q_FRAME_TYPE" humanName="QuadPlane type">
          <values><value code="1">X</value></values>
        </param>
        <param name="ArduCopter:MOT_SPIN_ARM"
               humanName="Motor Spin armed" />
        <param name="ArduCopter:MOT_SPIN_MIN"
               humanName="Motor Spin minimum" />
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QByteArray quadLayoutsFixture()
{
    return QByteArray(R"json(
      {
        "Version": "AP_Motors library test ver 1.2",
        "layouts": [
          {
            "Class": 1,
            "ClassName": "QUAD",
            "Type": 0,
            "TypeName": "PLUS",
            "motors": [
              {"Number": 1, "TestOrder": 2, "Rotation": "CCW"},
              {"Number": 2, "TestOrder": 4, "Rotation": "CCW"},
              {"Number": 3, "TestOrder": 1, "Rotation": "CW"},
              {"Number": 4, "TestOrder": 3, "Rotation": "CW"}
            ]
          },
          {
            "Class": 1,
            "ClassName": "QUAD",
            "Type": 1,
            "TypeName": "X",
            "motors": [
              {"Number": 1, "TestOrder": 1, "Rotation": "CCW"},
              {"Number": 2, "TestOrder": 3, "Rotation": "CCW"},
              {"Number": 3, "TestOrder": 4, "Rotation": "CW"},
              {"Number": 4, "TestOrder": 2, "Rotation": "CW"}
            ]
          }
        ]
      })json");
}

QList<ConfigFriendlyParameterValue> quadXParameters(
    double spinMinimum = 0.159)
{
    return {
        {1, QStringLiteral("FRAME"), 1},
        {1, QStringLiteral("FRAME_CLASS"), 1},
        {1, QStringLiteral("FRAME_TYPE"), 1},
        {1, QStringLiteral("MOT_SPIN_ARM"), 0.10},
        {1, QStringLiteral("MOT_SPIN_MIN"), spinMinimum}
    };
}

void verifyMotor(const QList<MotorTestItem> &motors, int index,
                 int testOrder, int motorNumber, const QString &label,
                 const QString &rotation)
{
    QVERIFY(index >= 0 && index < motors.size());
    const MotorTestItem &motor = motors.at(index);
    QCOMPARE(motor.TestOrder, testOrder);
    QCOMPARE(motor.MotorNumber, motorNumber);
    QCOMPARE(motor.Label, label);
    QCOMPARE(motor.Rotation, rotation);
}

void verifyCommand(const QList<QVariant> &arguments, int motor,
                   int throttle, int durationSec, int motorCount)
{
    QCOMPARE(arguments.size(), 7);
    QCOMPARE(arguments.at(0).toInt(), kAutopilotComponent);
    QCOMPARE(arguments.at(1).toInt(), motor);
    QCOMPARE(arguments.at(2).toInt(), 0); // MOTOR_TEST_THROTTLE_PERCENT
    QCOMPARE(arguments.at(3).toInt(), throttle);
    QCOMPARE(arguments.at(4).toInt(), durationSec);
    QCOMPARE(arguments.at(5).toInt(), motorCount);
    QCOMPARE(arguments.at(6).toInt(), 0); // MOTOR_TEST_ORDER_DEFAULT
}

void prepareRunnableModel(ConfigMotorTestViewModel *model)
{
    model->setCatalog(catalogFixture());
    model->setMotorLayoutJson(quadLayoutsFixture());
    model->setParameterSnapshot(quadXParameters());
    model->setConnected(true);
}
} // namespace

class ConfigMotorTestViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsRangesAndFallbackMotorCounts();
    void exactLayoutJsonOrderingAndMetadataHydrateWithoutWrites();
    void individualAllAndStopUseExactWireFields();
    void stopSendFailurePreservesEmergencyStateAndError();
    void roverAndBoatTestAllUseFourMotorSequence_data();
    void roverAndBoatTestAllUseFourMotorSequence();
    void safetyInterlocksBlockAccidentalCommands();
    void acknowledgementsFailuresAndTimeoutAreDeterministic();
    void spinFormulasEchoAndFailureMatchMissionPlanner();
    void widgetMatchesMissionPlannerContract();
};

void ConfigMotorTestViewTest::defaultsRangesAndFallbackMotorCounts()
{
    ConfigMotorTestViewModel model;

    QCOMPARE(model.Title(), QStringLiteral("Motor Test"));
    QCOMPARE(
        model.Instructions(),
        QStringLiteral(
            "DANGER: REMOVE ALL PROPELLERS. Verifies motor order and "
            "direction. Each test spins one motor at the set throttle % for "
            "the set duration. Test all in sequence steps through every "
            "motor (A, B, C…) one after another."));
    QCOMPARE(model.ThrottlePercent(), 8);
    QCOMPARE(model.DurationSec(), 2);
    QCOMPARE(model.Status(), QString());
    QCOMPARE(model.ComponentId(), 1);
    QVERIFY(!model.Connected());
    QVERIFY(!model.Armed());
    QVERIFY(!model.Busy());
    QVERIFY(!model.CanRun());
    QCOMPARE(model.Motors().size(), 8);
    verifyMotor(model.Motors(), 0, 1, 0,
                QStringLiteral("Test motor A"), QString());
    verifyMotor(model.Motors(), 7, 8, 0,
                QStringLiteral("Test motor H"), QString());

    model.setThrottlePercent(-1);
    QCOMPARE(model.ThrottlePercent(), 0);
    model.setThrottlePercent(101);
    QCOMPARE(model.ThrottlePercent(), 100);
    model.setDurationSec(-1);
    QCOMPARE(model.DurationSec(), 0);
    model.setDurationSec(61);
    QCOMPARE(model.DurationSec(), 60);

    model.setVehicleType(10); // MAV_TYPE_GROUND_ROVER
    QCOMPARE(model.Motors().size(), 4);
    model.setVehicleType(11); // MAV_TYPE_SURFACE_BOAT
    QCOMPARE(model.Motors().size(), 4);

    model.setVehicleType(2); // MAV_TYPE_QUADROTOR
    model.setParameterSnapshot({
        {1, QStringLiteral("Q_FRAME_CLASS"), 2},
        {1, QStringLiteral("Q_FRAME_TYPE"), 99}
    });
    QCOMPARE(model.Motors().size(), 6);
    model.setParameterSnapshot({
        {1, QStringLiteral("Q_FRAME_CLASS"), 3},
        {1, QStringLiteral("Q_FRAME_TYPE"), 99}
    });
    QCOMPARE(model.Motors().size(), 8);
    model.setParameterSnapshot({
        {1, QStringLiteral("Q_FRAME_CLASS"), 7},
        {1, QStringLiteral("Q_FRAME_TYPE"), 99}
    });
    QCOMPARE(model.Motors().size(), 4);
    model.setParameterSnapshot({
        {1, QStringLiteral("Q_FRAME_CLASS"), 6},
        {1, QStringLiteral("Q_FRAME_TYPE"), 99}
    });
    QCOMPARE(model.Motors().size(), 0);

    model.setVehicleType(29); // MAV_TYPE_DODECAROTOR
    model.setParameterSnapshot({
        {1, QStringLiteral("FRAME_TYPE"), 99}
    });
    QCOMPARE(model.Motors().size(), 12);
    verifyMotor(model.Motors(), 11, 12, 0,
                QStringLiteral("Test motor L"), QString());

    model.setVehicleType(4); // MAV_TYPE_HELICOPTER
    model.setParameterSnapshot({
        {1, QStringLiteral("FRAME_TYPE"), 99}
    });
    QCOMPARE(model.Motors().size(), 0);
}

void ConfigMotorTestViewTest::
    exactLayoutJsonOrderingAndMetadataHydrateWithoutWrites()
{
    ConfigMotorTestViewModel model;
    QSignalSpy writes(&model, &ConfigMotorTestViewModel::writeRequested);
    QSignalSpy commands(&model,
                       &ConfigMotorTestViewModel::motorTestRequested);

    model.setCatalog(catalogFixture());
    model.setMotorLayoutJson(quadLayoutsFixture());
    model.setParameterSnapshot(quadXParameters());

    QCOMPARE(writes.count(), 0);
    QCOMPARE(commands.count(), 0);
    QCOMPARE(model.ComponentId(), 1);
    QCOMPARE(model.FrameClass(), QStringLiteral("Class: QUAD"));
    QCOMPARE(model.FrameType(), QStringLiteral("Type: X"));
    const QList<MotorTestItem> motors = model.Motors();
    QCOMPARE(motors.size(), 4);
    verifyMotor(motors, 0, 1, 1,
                QStringLiteral("Test motor A  (Motor 1)"),
                QStringLiteral("CCW"));
    verifyMotor(motors, 1, 2, 4,
                QStringLiteral("Test motor B  (Motor 4)"),
                QStringLiteral("CW"));
    verifyMotor(motors, 2, 3, 2,
                QStringLiteral("Test motor C  (Motor 2)"),
                QStringLiteral("CCW"));
    verifyMotor(motors, 3, 4, 3,
                QStringLiteral("Test motor D  (Motor 3)"),
                QStringLiteral("CW"));

    model.setParameterSnapshot({
        {1, QStringLiteral("FRAME"), 1},
        {1, QStringLiteral("FRAME_CLASS"), 1},
        {1, QStringLiteral("FRAME_TYPE"), 0}
    });
    QCOMPARE(writes.count(), 0);
    QCOMPARE(model.FrameClass(), QStringLiteral("Class: QUAD"));
    QCOMPARE(model.FrameType(), QStringLiteral("Type: PLUS"));
    verifyMotor(model.Motors(), 0, 1, 3,
                QStringLiteral("Test motor A  (Motor 3)"),
                QStringLiteral("CW"));
    verifyMotor(model.Motors(), 1, 2, 1,
                QStringLiteral("Test motor B  (Motor 1)"),
                QStringLiteral("CCW"));
    verifyMotor(model.Motors(), 2, 3, 4,
                QStringLiteral("Test motor C  (Motor 4)"),
                QStringLiteral("CW"));
    verifyMotor(model.Motors(), 3, 4, 2,
                QStringLiteral("Test motor D  (Motor 2)"),
                QStringLiteral("CCW"));
}

void ConfigMotorTestViewTest::individualAllAndStopUseExactWireFields()
{
    ConfigMotorTestViewModel model;
    prepareRunnableModel(&model);
    model.setThrottlePercent(12);
    model.setDurationSec(7);
    QSignalSpy commands(&model,
                       &ConfigMotorTestViewModel::motorTestRequested);

    QVERIFY(model.TestMotor(3, true));
    QCOMPARE(commands.count(), 1);
    verifyCommand(commands.at(0), 3, 12, 7, 0);
    QVERIFY(model.Busy());
    QVERIFY(model.TestMayBeActive());
    QCOMPARE(model.Status(),
             QStringLiteral("Waiting for motor-test confirmation…"));
    model.commandAckReceived(kAutopilotComponent, kMotorTestCommand,
                             kAccepted);
    QVERIFY(!model.Busy());
    QVERIFY(model.TestMayBeActive());
    QCOMPARE(model.Status(), QString());

    QVERIFY(model.TestAllSequence(true));
    QCOMPARE(commands.count(), 2);
    // Test-all is one ArduPilot sequence command, not one command per motor.
    verifyCommand(commands.at(1), 1, 12, 7, 4);
    model.commandAckReceived(kAutopilotComponent, kMotorTestCommand,
                             kAccepted);
    QVERIFY(!model.Busy());

    QVERIFY(model.TestMotor(2, true));
    QCOMPARE(commands.count(), 3);
    QVERIFY(model.Busy());
    model.setArmed(true);
    QVERIFY(model.Busy());

    // Stop supersedes the pending non-zero test and emits exactly one global
    // zero-throttle/zero-timeout command immediately.
    QVERIFY(model.StopAll());
    QCOMPARE(commands.count(), 4);
    verifyCommand(commands.at(3), 1, 0, 0, 0);
    QVERIFY(model.Busy());
    QVERIFY(model.TestMayBeActive());
    QVERIFY(model.NeedsEmergencyStop());
    QCOMPARE(model.Status(), QStringLiteral("Stopping all motors…"));
    QVERIFY(!model.TestMotor(1, true));
    QCOMPARE(commands.count(), 4);

    // ACK cannot be correlated with either the canceled test or the stop.
    // It must not end the full ACK-timeout quarantine early.
    model.commandAckReceived(kAutopilotComponent, kMotorTestCommand,
                             kAccepted);
    QVERIFY(model.Busy());
    // A post-Stop disarm transition is independent evidence that the
    // autopilot has left motor-test state, unlike the ambiguous ACK above.
    model.setArmed(false);
    QVERIFY(!model.Busy());
    QVERIFY(!model.NeedsEmergencyStop());
    QCOMPARE(model.Status(),
             QStringLiteral("Motors stopped (vehicle disarmed)."));
}

void ConfigMotorTestViewTest::
    stopSendFailurePreservesEmergencyStateAndError()
{
    ConfigMotorTestViewModel model;
    prepareRunnableModel(&model);
    QSignalSpy commands(&model,
                       &ConfigMotorTestViewModel::motorTestRequested);

    QVERIFY(model.TestMotor(1, true));
    QVERIFY(model.TestMayBeActive());
    QCOMPARE(commands.count(), 1);
    QVERIFY(model.StopAll());
    QCOMPARE(commands.count(), 2);
    verifyCommand(commands.at(1), 1, 0, 0, 0);

    model.commandSendFailed(kAutopilotComponent,
                            QStringLiteral("stop link failure"));
    QVERIFY(model.NeedsEmergencyStop());
    const QString errorStatus = model.Status();
    QVERIFY2(errorStatus.contains(QStringLiteral("fail"),
                                  Qt::CaseInsensitive),
             qPrintable(errorStatus));
    QVERIFY2(errorStatus.contains(QStringLiteral("stop"),
                                  Qt::CaseInsensitive),
             qPrintable(errorStatus));
    QVERIFY(errorStatus != QStringLiteral("Stop commands sent."));

    // The quarantine timer from StopAll must not overwrite a transport error
    // with a false success after its normal deadline.
    QTest::qWait(4500);
    QVERIFY(model.NeedsEmergencyStop());
    QCOMPARE(model.Status(), errorStatus);
    QVERIFY(model.Status() != QStringLiteral("Stop commands sent."));
}

void ConfigMotorTestViewTest::
    roverAndBoatTestAllUseFourMotorSequence_data()
{
    QTest::addColumn<int>("vehicleType");
    QTest::newRow("ground rover") << 10; // MAV_TYPE_GROUND_ROVER
    QTest::newRow("surface boat") << 11; // MAV_TYPE_SURFACE_BOAT
}

void ConfigMotorTestViewTest::roverAndBoatTestAllUseFourMotorSequence()
{
    QFETCH(int, vehicleType);

    ConfigMotorTestViewModel model;
    model.setVehicleType(vehicleType);
    model.setConnected(true);
    model.setThrottlePercent(9);
    // A zero duration keeps this state-machine test fast; non-zero commands
    // use the same queue with a 50% quiet interval before the next output.
    model.setDurationSec(0);
    QCOMPARE(model.Motors().size(), 4);
    for (int index = 0; index < model.Motors().size(); ++index) {
        QCOMPARE(model.Motors().at(index).TestOrder, index + 1);
    }

    QSignalSpy commands(&model,
                       &ConfigMotorTestViewModel::motorTestRequested);
    QVERIFY(model.TestAllSequence(true));
    QCOMPARE(commands.count(), 1);
    // Rover's handler ignores param5, unlike Copter. The GCS therefore sends
    // one acknowledged single-output command at a time.
    for (int motor = 1; motor <= 4; ++motor) {
        QTRY_COMPARE_WITH_TIMEOUT(commands.count(), motor, 1000);
        verifyCommand(commands.at(motor - 1), motor, 9, 0, 0);
        QVERIFY(model.Busy());
        model.setArmed(true);
        model.commandAckReceived(kAutopilotComponent, kMotorTestCommand,
                                 kAccepted);
        model.setArmed(false);
    }
    QVERIFY(!model.Busy());
}

void ConfigMotorTestViewTest::safetyInterlocksBlockAccidentalCommands()
{
    ConfigMotorTestViewModel model;
    model.setParameterSnapshot(quadXParameters());
    QSignalSpy commands(&model,
                       &ConfigMotorTestViewModel::motorTestRequested);
    QSignalSpy writes(&model, &ConfigMotorTestViewModel::writeRequested);

    QVERIFY(!model.TestMotor(1, true));
    QCOMPARE(model.Status(), QStringLiteral("Connect to a vehicle first."));
    QVERIFY(!model.StopAll());
    QCOMPARE(commands.count(), 0);

    model.setConnected(true);
    QVERIFY(model.CanRun());
    QVERIFY(!model.TestMotor(0, true));
    QVERIFY(!model.TestMotor(9, true));
    QVERIFY(!model.TestMotor(1, false));
    QVERIFY(!model.TestAllSequence(false));
    QCOMPARE(commands.count(), 0);

    model.setArmed(true);
    QVERIFY(!model.CanRun());
    QVERIFY(!model.TestMotor(1, true));
    QCOMPARE(model.Status(),
             QStringLiteral("Disarm the vehicle before motor testing."));
    QVERIFY(!model.TestAllSequence(true));
    QVERIFY(!model.SetSpinArm());
    QVERIFY(!model.SetSpinMin());
    QCOMPARE(commands.count(), 0);
    QCOMPARE(writes.count(), 0);

    model.setArmed(false);
    QVERIFY(model.TestMotor(1, true));
    QVERIFY(model.Busy());
    QVERIFY(!model.TestMotor(2, true));
    QVERIFY(!model.TestAllSequence(true));
    QVERIFY(!model.SetSpinArm());
    QCOMPARE(commands.count(), 1);
    QCOMPARE(writes.count(), 0);

    model.setConnected(false);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QStringLiteral("Connect to a vehicle first."));
    model.commandAckReceived(kAutopilotComponent, kMotorTestCommand,
                             kAccepted);
    QCOMPARE(model.Status(), QStringLiteral("Connect to a vehicle first."));

    ConfigMotorTestViewModel noMotors;
    noMotors.setVehicleType(4); // MAV_TYPE_HELICOPTER
    noMotors.setParameterSnapshot({
        {1, QStringLiteral("FRAME_TYPE"), 99}
    });
    QCOMPARE(noMotors.Motors().size(), 0);
    noMotors.setConnected(true);
    QSignalSpy emptyCommands(
        &noMotors, &ConfigMotorTestViewModel::motorTestRequested);
    QVERIFY(!noMotors.CanRun());
    QVERIFY(!noMotors.TestAllSequence(true));
    QCOMPARE(emptyCommands.count(), 0);
    QVERIFY(!noMotors.StopAll());
}

void ConfigMotorTestViewTest::
    acknowledgementsFailuresAndTimeoutAreDeterministic()
{
    ConfigMotorTestViewModel model;
    prepareRunnableModel(&model);

    QVERIFY(model.TestMotor(1, true));
    model.commandAckReceived(2, kMotorTestCommand, kAccepted);
    QVERIFY(model.Busy());
    model.commandAckReceived(kAutopilotComponent, kMotorTestCommand + 1,
                             kAccepted);
    QVERIFY(model.Busy());
    model.commandAckReceived(kAutopilotComponent, kMotorTestCommand,
                             kInProgress);
    QVERIFY(model.Busy());
    QCOMPARE(model.Status(),
             QStringLiteral("Waiting for motor-test confirmation…"));
    model.commandAckReceived(kAutopilotComponent, kMotorTestCommand,
                             kAccepted);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QString());

    QVERIFY(model.TestAllSequence(true));
    model.commandAckReceived(kAutopilotComponent, kMotorTestCommand,
                             kDenied);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(),
             QStringLiteral("Command was denied by the autopilot."));

    QVERIFY(model.TestMotor(2, true));
    model.commandSendFailed(2, QStringLiteral("wrong vehicle"));
    QVERIFY(model.Busy());
    model.commandSendFailed(kAutopilotComponent,
                            QStringLiteral("link write failed"));
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(),
             QStringLiteral("Failed to test motor: link write failed"));

    ConfigMotorTestView timeoutView(catalogFixture());
    timeoutView.setMotorLayoutJson(quadLayoutsFixture());
    timeoutView.setParameterSnapshot(quadXParameters());
    timeoutView.setConnected(true);
    timeoutView.viewModel()->setDurationSec(0);
    QPushButton *spinArm = timeoutView.findChild<QPushButton *>(
        QStringLiteral("motorTestSetSpinArm"));
    QPushButton *spinMin = timeoutView.findChild<QPushButton *>(
        QStringLiteral("motorTestSetSpinMin"));
    QVERIFY(spinArm);
    QVERIFY(spinMin);
    QVERIFY(spinArm->isEnabled());
    QVERIFY(spinMin->isEnabled());

    QVERIFY(timeoutView.viewModel()->TestMotor(3, true));
    QTRY_VERIFY_WITH_TIMEOUT(!timeoutView.viewModel()->Busy(), 6000);
    QVERIFY(timeoutView.viewModel()->NeedsEmergencyStop());
    QVERIFY(!timeoutView.viewModel()->TestMayBeActive());
    QCOMPARE(timeoutView.viewModel()->Status(),
             QStringLiteral("Failed to test motor: command timeout."));
    QVERIFY(!spinArm->isEnabled());
    QVERIFY(!spinMin->isEnabled());
    timeoutView.commandAckReceived(
        kAutopilotComponent, kMotorTestCommand, kAccepted);
    QCOMPARE(timeoutView.viewModel()->Status(),
             QStringLiteral("Failed to test motor: command timeout."));
}

void ConfigMotorTestViewTest::
    spinFormulasEchoAndFailureMatchMissionPlanner()
{
    ConfigMotorTestViewModel model;
    model.setParameterSnapshot(quadXParameters(0.159));
    QSignalSpy writes(&model, &ConfigMotorTestViewModel::writeRequested);

    QVERIFY(!model.SetSpinArm());
    QCOMPARE(model.Status(), QStringLiteral("Connect to a vehicle first."));
    QCOMPARE(writes.count(), 0);

    model.setConnected(true);
    model.setThrottlePercent(8);
    QVERIFY(model.SetSpinArm());
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(0).toInt(), 1);
    QCOMPARE(writes.at(0).at(1).toString(),
             QStringLiteral("MOT_SPIN_ARM"));
    QCOMPARE(writes.at(0).at(2).toDouble(), 0.10);
    QVERIFY(model.Busy());
    QVERIFY(!model.SetSpinMin());
    QCOMPARE(writes.count(), 1);

    model.parameterChanged(2, QStringLiteral("MOT_SPIN_ARM"), 0.10);
    QVERIFY(model.Busy());
    model.parameterChanged(1, QStringLiteral("MOT_SPIN_ARM"), 0.11);
    QVERIFY(model.Busy());
    model.parameterChanged(1, QStringLiteral("MOT_SPIN_ARM"), 0.10);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QStringLiteral("MOT_SPIN_ARM set to 0.10."));

    // Mission Planner intentionally truncates the existing value to hundredths
    // before adding 0.03: 0.159 -> 0.18.
    QVERIFY(model.SetSpinMin());
    QCOMPARE(writes.count(), 2);
    QCOMPARE(writes.at(1).at(0).toInt(), 1);
    QCOMPARE(writes.at(1).at(1).toString(),
             QStringLiteral("MOT_SPIN_MIN"));
    QCOMPARE(writes.at(1).at(2).toDouble(), 0.18);
    model.parameterChanged(1, QStringLiteral("MOT_SPIN_MIN"), 0.18);
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QStringLiteral("MOT_SPIN_MIN set to 0.18."));

    QVERIFY(model.SetSpinMin());
    QCOMPARE(writes.count(), 3);
    QCOMPARE(writes.at(2).at(2).toDouble(), 0.21);
    model.parameterWriteFailed(2, QStringLiteral("MOT_SPIN_MIN"),
                               QStringLiteral("wrong component"));
    QVERIFY(model.Busy());
    model.parameterWriteFailed(1, QStringLiteral("mot_spin_min"),
                               QStringLiteral("link lost"));
    QVERIFY(!model.Busy());
    QCOMPARE(model.Status(), QStringLiteral("Failed to set MOT_SPIN_MIN."));

    model.setThrottlePercent(20);
    QVERIFY(!model.SetSpinArm());
    QCOMPARE(model.Status(),
             QStringLiteral("Throttle percent above 20, too high."));
    QVERIFY(!model.SetSpinMin());
    QCOMPARE(writes.count(), 3);

    ConfigMotorTestViewModel missing;
    missing.setConnected(true);
    QSignalSpy missingWrites(
        &missing, &ConfigMotorTestViewModel::writeRequested);
    QVERIFY(!missing.SetSpinArm());
    QCOMPARE(missing.Status(), QStringLiteral("param MOT_SPIN_ARM missing."));
    QVERIFY(!missing.SetSpinMin());
    QCOMPARE(missing.Status(), QStringLiteral("param MOT_SPIN_MIN missing."));
    QCOMPARE(missingWrites.count(), 0);

    ConfigMotorTestViewModel armed;
    armed.setParameterSnapshot(quadXParameters());
    armed.setConnected(true);
    armed.setArmed(true);
    QSignalSpy armedWrites(&armed,
                           &ConfigMotorTestViewModel::writeRequested);
    QVERIFY(!armed.SetSpinArm());
    QCOMPARE(armed.Status(),
             QStringLiteral("Disarm the vehicle before motor testing."));
    QCOMPARE(armedWrites.count(), 0);
}

void ConfigMotorTestViewTest::widgetMatchesMissionPlannerContract()
{
    ConfigMotorTestView view(catalogFixture());
    QSignalSpy writes(&view, &ConfigMotorTestView::writeRequested);
    QSignalSpy commands(&view, &ConfigMotorTestView::motorTestRequested);
    view.setMotorLayoutJson(quadLayoutsFixture());
    view.setParameterSnapshot(quadXParameters());
    view.setConnected(true);

    QCOMPARE(writes.count(), 0);
    QCOMPARE(commands.count(), 0);
    view.show();
    QTest::qWait(1);
    view.hide();
    QCOMPARE(commands.count(), 0);
    QCOMPARE(view.objectName(), QStringLiteral("ConfigMotorTestView"));
    QCOMPARE(view.sizeHint(), QSize(800, 640));

    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    view.layout()->getContentsMargins(&left, &top, &right, &bottom);
    QCOMPARE(left, 16);
    QCOMPARE(top, 16);
    QCOMPARE(right, 16);
    QCOMPARE(bottom, 16);

    QLabel *title = view.findChild<QLabel *>(
        QStringLiteral("motorTestTitle"));
    QFrame *danger = view.findChild<QFrame *>(
        QStringLiteral("motorTestDanger"));
    QLabel *instructions = view.findChild<QLabel *>(
        QStringLiteral("motorTestInstructions"));
    QVERIFY(title);
    QVERIFY(danger);
    QVERIFY(instructions);
    QCOMPARE(title->text(), QStringLiteral("Motor Test"));
    QCOMPARE(instructions->text(), view.viewModel()->Instructions());
    QVERIFY(instructions->wordWrap());
    danger->layout()->getContentsMargins(&left, &top, &right, &bottom);
    QCOMPARE(left, 12);
    QCOMPARE(top, 12);
    QCOMPARE(right, 12);
    QCOMPARE(bottom, 12);

    QWidget *frameRow = view.findChild<QWidget *>(
        QStringLiteral("motorTestFrameRow"));
    QLabel *frameClass = view.findChild<QLabel *>(
        QStringLiteral("motorTestFrameClass"));
    QLabel *frameType = view.findChild<QLabel *>(
        QStringLiteral("motorTestFrameType"));
    QVERIFY(frameRow);
    QVERIFY(frameClass);
    QVERIFY(frameType);
    QCOMPARE(frameClass->text(), QStringLiteral("Class: QUAD"));
    QCOMPARE(frameType->text(), QStringLiteral("Type: X"));

    QWidget *controls = view.findChild<QWidget *>(
        QStringLiteral("motorTestControls"));
    QLabel *throttleLabel = view.findChild<QLabel *>(
        QStringLiteral("motorTestThrottleLabel"));
    QLabel *durationLabel = view.findChild<QLabel *>(
        QStringLiteral("motorTestDurationLabel"));
    QSpinBox *throttle = view.findChild<QSpinBox *>(
        QStringLiteral("motorTestThrottleEditor"));
    QSpinBox *duration = view.findChild<QSpinBox *>(
        QStringLiteral("motorTestDurationEditor"));
    QPushButton *spinArm = view.findChild<QPushButton *>(
        QStringLiteral("motorTestSetSpinArm"));
    QPushButton *spinMin = view.findChild<QPushButton *>(
        QStringLiteral("motorTestSetSpinMin"));
    QVERIFY(controls);
    QVERIFY(throttleLabel);
    QVERIFY(durationLabel);
    QVERIFY(throttle);
    QVERIFY(duration);
    QVERIFY(spinArm);
    QVERIFY(spinMin);
    QCOMPARE(throttleLabel->text(), QStringLiteral("Throttle %"));
    QCOMPARE(durationLabel->text(), QStringLiteral("Duration (s)"));
    QCOMPARE(throttle->minimum(), 0);
    QCOMPARE(throttle->maximum(), 100);
    QCOMPARE(throttle->singleStep(), 1);
    QCOMPARE(throttle->minimumWidth(), 100);
    QCOMPARE(throttle->value(), 8);
    QCOMPARE(duration->minimum(), 0);
    QCOMPARE(duration->maximum(), 60);
    QCOMPARE(duration->singleStep(), 1);
    QCOMPARE(duration->minimumWidth(), 100);
    QCOMPARE(duration->value(), 2);
    QCOMPARE(spinArm->text(), QStringLiteral("Set MOT_SPIN_ARM"));
    QCOMPARE(spinMin->text(), QStringLiteral("Set MOT_SPIN_MIN"));
    QGridLayout *controlLayout = qobject_cast<QGridLayout *>(
        controls->layout());
    QVERIFY(controlLayout);
    QCOMPARE(controlLayout->horizontalSpacing(), 12);

    QScrollArea *scroll = view.findChild<QScrollArea *>(
        QStringLiteral("motorTestScroll"));
    QWidget *motorContent = view.findChild<QWidget *>(
        QStringLiteral("motorTestMotorContent"));
    QWidget *sequenceRow = view.findChild<QWidget *>(
        QStringLiteral("motorTestSequenceRow"));
    QPushButton *testAll = view.findChild<QPushButton *>(
        QStringLiteral("motorTestAllButton"));
    QPushButton *stopAll = view.findChild<QPushButton *>(
        QStringLiteral("motorStopAllButton"));
    QLabel *status = view.findChild<QLabel *>(
        QStringLiteral("motorTestStatus"));
    QVERIFY(scroll);
    QVERIFY(motorContent);
    QVERIFY(sequenceRow);
    QVERIFY(testAll);
    QVERIFY(stopAll);
    QVERIFY(status);
    QCOMPARE(scroll->widget(), motorContent);
    QCOMPARE(testAll->text(), QStringLiteral("Test all in sequence"));
    QCOMPARE(stopAll->text(), QStringLiteral("Stop all motors"));
    QCOMPARE(status->text(), QString());
    QVERIFY(status->wordWrap());

    const QStringList motorLabels = {
        QStringLiteral("Test motor A  (Motor 1)"),
        QStringLiteral("Test motor B  (Motor 4)"),
        QStringLiteral("Test motor C  (Motor 2)"),
        QStringLiteral("Test motor D  (Motor 3)")
    };
    const QStringList rotations = {
        QStringLiteral("CCW"), QStringLiteral("CW"),
        QStringLiteral("CCW"), QStringLiteral("CW")
    };
    for (int order = 1; order <= 4; ++order) {
        QWidget *row = view.findChild<QWidget *>(
            QStringLiteral("motorTestRow_%1").arg(order));
        QPushButton *button = view.findChild<QPushButton *>(
            QStringLiteral("motorTestButton_%1").arg(order));
        QLabel *rotation = view.findChild<QLabel *>(
            QStringLiteral("motorTestRotation_%1").arg(order));
        QVERIFY(row);
        QVERIFY(button);
        QVERIFY(rotation);
        QCOMPARE(button->text(), motorLabels.at(order - 1));
        QCOMPARE(button->minimumWidth(), 220);
        QCOMPARE(rotation->text(), rotations.at(order - 1));
        QVERIFY(button->isEnabled());
    }
    QVERIFY(!view.findChild<QWidget *>(QStringLiteral("motorTestRow_5")));
    QVERIFY(throttle->isEnabled());
    QVERIFY(duration->isEnabled());
    QVERIFY(spinArm->isEnabled());
    QVERIFY(spinMin->isEnabled());
    QVERIFY(testAll->isEnabled());
    QVERIFY(stopAll->isEnabled());

    view.setArmed(true);
    QVERIFY(!throttle->isEnabled());
    QVERIFY(!duration->isEnabled());
    QVERIFY(!spinArm->isEnabled());
    QVERIFY(!spinMin->isEnabled());
    QVERIFY(!testAll->isEnabled());
    QVERIFY(!view.findChild<QPushButton *>(
                 QStringLiteral("motorTestButton_1"))->isEnabled());
    QVERIFY(stopAll->isEnabled());
    view.setConnected(false);
    QVERIFY(!stopAll->isEnabled());

    QCOMPARE(ConfigMotorTestView::SafetyConfirmationTitle(),
             QStringLiteral("Motor Test Safety Warning"));
    const QString confirmation =
        ConfigMotorTestView::SafetyConfirmationText();
    QVERIFY2(confirmation.contains(QStringLiteral("REMOVE ALL PROPELLERS")),
             qPrintable(confirmation));
    QVERIFY2(confirmation.contains(QStringLiteral("spin immediately"),
                                   Qt::CaseInsensitive),
             qPrintable(confirmation));
}

QTEST_MAIN(ConfigMotorTestViewTest)
#include "test_configmotortestview.moc"
