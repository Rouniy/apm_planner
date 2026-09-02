#include "ConfigMotorTestViewModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMotorCommand = 209; // MAV_CMD_DO_MOTOR_TEST
constexpr int kAutopilotComponent = 1; // MAV_COMP_ID_AUTOPILOT1
constexpr int kThrottlePercent = 0; // MOTOR_TEST_THROTTLE_PERCENT
constexpr int kTestOrderDefault = 0; // MOTOR_TEST_ORDER_DEFAULT
constexpr int kAccepted = 0; // MAV_RESULT_ACCEPTED
constexpr int kInProgress = 5; // MAV_RESULT_IN_PROGRESS
constexpr int kCommandTimeoutMs = 4000;
constexpr int kParameterTimeoutMs = 5000;

// MAV_TYPE values from the common MAVLink dialect. Keeping the model free of
// generated headers makes its deterministic tests independent of a dialect.
constexpr int kFixedWing = 1;
constexpr int kQuadrotor = 2;
constexpr int kHelicopter = 4;
constexpr int kGroundRover = 10;
constexpr int kSurfaceBoat = 11;
constexpr int kHexarotor = 13;
constexpr int kOctorotor = 14;
constexpr int kTricopter = 15;
constexpr int kDodecarotor = 29;

bool variantsEqual(const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    if (leftOk && rightOk) {
        return leftValue == rightValue
            || std::abs(leftValue - rightValue) <= 1.0e-6;
    }
    return left == right;
}
} // namespace

ConfigMotorTestViewModel::ConfigMotorTestViewModel(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<MotorTestItem>();
    rebuildMotors();
}

QString ConfigMotorTestViewModel::Title() const
{
    return tr("Motor Test");
}

QString ConfigMotorTestViewModel::Instructions() const
{
    return tr(
        "DANGER: REMOVE ALL PROPELLERS. Verifies motor order and direction. "
        "Each test spins one motor at the set throttle % for the set "
        "duration. Test all in sequence steps through every motor "
        "(A, B, C…) one after another.");
}

bool ConfigMotorTestViewModel::Busy() const
{
    return m_expectedCommandAcks > 0
        || !m_pendingCommands.isEmpty()
        || !m_pendingParameter.name.isEmpty() || m_stopBatch;
}

bool ConfigMotorTestViewModel::CanRun() const
{
    return m_connected && !m_armed && !Busy()
        && !m_commandAckUncertain && !m_motors.isEmpty();
}

void ConfigMotorTestViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_catalog = catalog;
    rebuildMotors();
}

void ConfigMotorTestViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    const QSet<QString> relevant = {
        QStringLiteral("FRAME"),
        QStringLiteral("FRAME_CLASS"),
        QStringLiteral("FRAME_TYPE"),
        QStringLiteral("Q_FRAME_CLASS"),
        QStringLiteral("Q_FRAME_TYPE"),
        QStringLiteral("MOT_SPIN_ARM"),
        QStringLiteral("MOT_SPIN_MIN")
    };
    QSet<int> components;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (relevant.contains(normalizedName(parameter.name))) {
            components.insert(parameter.componentId);
        }
    }
    if (components.contains(preferredComponent)) {
        m_componentId = preferredComponent;
    } else if (components.contains(1)) {
        m_componentId = 1;
    } else if (!components.isEmpty()) {
        QList<int> ordered = components.values();
        std::sort(ordered.begin(), ordered.end());
        m_componentId = ordered.first();
    } else {
        m_componentId = preferredComponent;
    }

    ++m_parameterGeneration;
    ++m_commandGeneration;
    m_pendingParameter = {};
    m_pendingCommands.clear();
    m_expectedCommandAcks = 0;
    m_receivedCommandAcks = 0;
    m_stopBatch = false;
    m_stopAckAmbiguous = false;
    m_stopSawArmed = false;
    m_waitingForSequenceDisarm = false;
    m_sequenceDelayElapsed = false;
    m_values.clear();
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (parameter.componentId == m_componentId) {
            m_values.insert(normalizedName(parameter.name), parameter.value);
        }
    }
    rebuildMotors();
    emit stateChanged();
}

void ConfigMotorTestViewModel::setMotorLayoutJson(const QByteArray &json)
{
    if (m_layoutJson == json) {
        return;
    }
    m_layoutJson = json;
    rebuildMotors();
}

void ConfigMotorTestViewModel::setVehicleType(int mavType)
{
    if (m_vehicleType == mavType) {
        return;
    }
    m_vehicleType = mavType;
    rebuildMotors();
}

void ConfigMotorTestViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!m_connected) {
        ++m_testActivityGeneration;
        m_testMayBeActive = false;
        m_testActivityDurationMs = 0;
        m_commandAckUncertain = false;
        m_stopAttempted = false;
        m_motorTestArmedObserved = false;
        m_motorTestDisarmedAfterArmObserved = false;
        cancelPending(tr("Connect to a vehicle first."));
    }
    emit stateChanged();
}

void ConfigMotorTestViewModel::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    if (m_armed) {
        if (m_stopBatch) {
            m_stopSawArmed = true;
        } else if (m_testMayBeActive || Busy()) {
            m_motorTestArmedObserved = true;
        }
    } else if (m_motorTestArmedObserved) {
        m_motorTestDisarmedAfterArmObserved = true;
    }
    if (!m_armed && m_stopBatch && m_stopSawArmed) {
        finishStop(tr("Motors stopped (vehicle disarmed)."));
    } else if (!m_armed && m_waitingForSequenceDisarm) {
        maybeDispatchNextMotorCommand();
    }
    // ArduPilot temporarily reports motors armed while a DO_MOTOR_TEST is
    // active. Do not mistake that firmware-controlled transition for a user
    // arm and cancel a test which the autopilot already accepted.
    if (m_armed && !m_pendingParameter.name.isEmpty()) {
        cancelPending(tr("Disarm the vehicle before motor testing."));
    }
    emit stateChanged();
}

void ConfigMotorTestViewModel::setThrottlePercent(int percent)
{
    const int bounded = qBound(0, percent, 100);
    if (m_throttlePercent == bounded) {
        return;
    }
    m_throttlePercent = bounded;
    emit settingsChanged();
}

void ConfigMotorTestViewModel::setDurationSec(int seconds)
{
    const int bounded = qBound(0, seconds, 60);
    if (m_durationSec == bounded) {
        return;
    }
    m_durationSec = bounded;
    emit settingsChanged();
}

bool ConfigMotorTestViewModel::TestMotor(int testOrder, bool confirmed)
{
    const auto found = std::find_if(
        m_motors.constBegin(), m_motors.constEnd(),
        [testOrder](const MotorTestItem &item) {
            return item.TestOrder == testOrder;
        });
    if (found == m_motors.constEnd()) {
        return false;
    }
    MotorCommand command;
    command.motor = testOrder;
    command.throttle = m_throttlePercent;
    command.durationSec = m_durationSec;
    return beginMotorCommands({command}, confirmed, false);
}

bool ConfigMotorTestViewModel::TestAllSequence(bool confirmed)
{
    if (m_motors.isEmpty()) {
        return false;
    }
    QList<MotorCommand> commands;
    if (m_vehicleType == kGroundRover
        || m_vehicleType == kSurfaceBoat) {
        // Rover/Boat implement only the single-output form of
        // MAV_CMD_DO_MOTOR_TEST and ignore param5 (motor count). Run their
        // four outputs from the GCS, leaving the same 50% quiet interval
        // between tests that Copter's firmware-managed sequence uses.
        for (const MotorTestItem &motor : m_motors) {
            MotorCommand command;
            command.motor = motor.TestOrder;
            command.throttle = m_throttlePercent;
            command.durationSec = m_durationSec;
            commands.append(command);
        }
    } else {
        MotorCommand command;
        command.motor = 1;
        command.throttle = m_throttlePercent;
        command.durationSec = m_durationSec;
        command.motorCount = m_motors.size();
        commands.append(command);
    }
    return beginMotorCommands(commands, confirmed, false);
}

bool ConfigMotorTestViewModel::StopAll()
{
    if (!m_connected) {
        setStatus(tr("Connect to a vehicle first."));
        return false;
    }
    if (m_motors.isEmpty()) {
        return false;
    }

    if (m_stopBatch) {
        return true;
    }

    // A stop request supersedes any outstanding test and is sent immediately;
    // waiting for the previous ACK would delay a safety action.
    const bool hadOutstandingMotorAck = m_expectedCommandAcks > 0;
    const bool testCouldBeActive = NeedsEmergencyStop()
        || !m_pendingCommands.isEmpty();
    const int safetyDelayMs = qMax(
        kCommandTimeoutMs,
        m_testActivityDurationMs + kCommandTimeoutMs);
    ++m_commandGeneration;
    m_expectedCommandAcks = 0;
    m_receivedCommandAcks = 0;
    m_pendingCommands.clear();
    m_pendingParameter = {};
    ++m_testActivityGeneration;
    m_testMayBeActive = testCouldBeActive;
    m_commandAckUncertain = true;
    m_stopBatch = true;
    m_stopAckAmbiguous = hadOutstandingMotorAck;
    m_stopSawArmed = m_armed || m_motorTestArmedObserved;
    m_stopAttempted = true;
    m_waitingForSequenceDisarm = false;
    m_sequenceDelayElapsed = false;
    setStatus(tr("Stopping all motors…"));
    emit stateChanged();
    // ArduPilot's zero-throttle/zero-timeout command stops the global motor
    // test state. Sending one command avoids re-entering that state N times.
    emit motorTestRequested(
        kAutopilotComponent, 1, kThrottlePercent,
        0, 0, 0, kTestOrderDefault);

    // COMMAND_ACK does not carry request parameters. If a previous command
    // had no ACK yet, its ACK and the Stop ACK are indistinguishable. Keep
    // the interlock until independent disarm telemetry arrives or until the
    // original test's conservative maximum run time has elapsed.
    const quint64 generation = m_commandGeneration;
    QTimer::singleShot(safetyDelayMs, this, [this, generation]() {
        if (generation == m_commandGeneration && m_stopBatch) {
            finishStop(tr(
                "Stop command dispatched; motor-test safety timeout "
                "elapsed."));
        }
    });
    return true;
}

bool ConfigMotorTestViewModel::SetSpinArm()
{
    if (!requireParameter(QStringLiteral("MOT_SPIN_ARM"))) {
        return false;
    }
    if (m_throttlePercent >= 20) {
        setStatus(tr("Throttle percent above 20, too high."));
        return false;
    }
    queueParameterWrite(
        QStringLiteral("MOT_SPIN_ARM"),
        (m_throttlePercent + 2) / 100.0);
    return true;
}

bool ConfigMotorTestViewModel::SetSpinMin()
{
    const QString name = QStringLiteral("MOT_SPIN_MIN");
    if (!requireParameter(name)) {
        return false;
    }
    if (m_throttlePercent >= 20) {
        setStatus(tr("Throttle percent above 20, too high."));
        return false;
    }
    const double value =
        (static_cast<int>(m_values.value(name).toDouble() * 100.0) + 3)
        / 100.0;
    queueParameterWrite(name, value);
    return true;
}

void ConfigMotorTestViewModel::commandAckReceived(
    int componentId, int command, int result)
{
    if (componentId != kAutopilotComponent || command != kMotorCommand) {
        return;
    }
    if (m_stopBatch) {
        if (m_stopAckAmbiguous || result == kInProgress) {
            return;
        }
        if (result != kAccepted) {
            ++m_commandGeneration;
            m_stopBatch = false;
            m_commandAckUncertain = true;
            setStatus(tr("Stop command was denied by the autopilot."));
            emit stateChanged();
        } else {
            setStatus(tr(
                "Stop command accepted; waiting for vehicle disarm…"));
        }
        return;
    }
    if (m_expectedCommandAcks <= 0) {
        return;
    }
    if (result == kInProgress) {
        ++m_commandGeneration;
        scheduleCommandTimeout();
        return;
    }
    if (result != kAccepted) {
        ++m_testActivityGeneration;
        m_testMayBeActive = false;
        m_testActivityDurationMs = 0;
        m_commandAckUncertain = false;
        finishMotorCommands(tr("Command was denied by the autopilot."));
        return;
    }
    ++m_receivedCommandAcks;
    if (m_receivedCommandAcks >= m_expectedCommandAcks) {
        m_commandAckUncertain = false;
        const MotorCommand completed = m_pendingCommands.isEmpty()
            ? MotorCommand{} : m_pendingCommands.first();
        if (m_pendingCommands.size() > 1) {
            m_pendingCommands.removeFirst();
            m_expectedCommandAcks = 0;
            m_receivedCommandAcks = 0;
            const quint64 generation = ++m_commandGeneration;
            m_waitingForSequenceDisarm = true;
            m_sequenceDelayElapsed = false;
            emit stateChanged();
            QTimer::singleShot(
                qMax(0, completed.durationSec * 1500), this,
                [this, generation]() {
                if (generation == m_commandGeneration
                    && m_waitingForSequenceDisarm) {
                    m_sequenceDelayElapsed = true;
                    maybeDispatchNextMotorCommand();
                }
            });
            QTimer::singleShot(
                qMax(0, completed.durationSec * 1500)
                    + kCommandTimeoutMs,
                this, [this, generation]() {
                if (generation == m_commandGeneration
                    && m_waitingForSequenceDisarm) {
                    finishMotorCommands(tr(
                        "Failed to continue motor sequence: vehicle "
                        "disarm was not observed."));
                    m_commandAckUncertain = true;
                    emit stateChanged();
                }
            });
            return;
        }
        const int activityMs = motorCommandDurationMs(completed);
        if (m_testMayBeActive && activityMs > 0) {
            ++m_testActivityGeneration;
            m_testActivityDurationMs = activityMs;
            scheduleActivityExpiry(activityMs + 500);
        }
        finishMotorCommands(QString());
    }
}

void ConfigMotorTestViewModel::commandSendFailed(
    int componentId, const QString &reason)
{
    if (componentId != kAutopilotComponent) {
        return;
    }
    if (m_stopBatch) {
        // The stop signal is handled synchronously by SetupView. Preserve an
        // explicit unsafe/unknown state when no link accepted the command;
        // otherwise the quarantine timer could falsely report success.
        ++m_commandGeneration;
        m_stopBatch = false;
        m_commandAckUncertain = true;
        setStatus(
            reason.isEmpty()
                ? tr("Failed to stop motors: command send failed.")
                : tr("Failed to stop motors: %1").arg(reason));
        emit stateChanged();
        return;
    }
    if (m_expectedCommandAcks <= 0) {
        return;
    }
    ++m_testActivityGeneration;
    m_testMayBeActive = false;
    m_testActivityDurationMs = 0;
    m_commandAckUncertain = false;
    finishMotorCommands(
        reason.isEmpty()
            ? tr("Failed to test motor: command send failed.")
            : tr("Failed to test motor: %1").arg(reason));
}

void ConfigMotorTestViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    const bool frameParameter = normalized == QLatin1String("FRAME")
        || normalized == QLatin1String("FRAME_CLASS")
        || normalized == QLatin1String("FRAME_TYPE")
        || normalized == QLatin1String("Q_FRAME_CLASS")
        || normalized == QLatin1String("Q_FRAME_TYPE");
    const bool spinParameter = normalized == QLatin1String("MOT_SPIN_ARM")
        || normalized == QLatin1String("MOT_SPIN_MIN");
    if (!frameParameter && !spinParameter) {
        return;
    }
    m_values.insert(normalized, value);
    if (frameParameter) {
        rebuildMotors();
    }
    if (m_pendingParameter.name == normalized) {
        if (variantsEqual(m_pendingParameter.expectedValue, value)) {
            finishParameterWrite(true);
        }
    }
}

void ConfigMotorTestViewModel::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    if (componentId == m_componentId
        && m_pendingParameter.name == normalizedName(name)) {
        finishParameterWrite(false, reason);
    }
}

void ConfigMotorTestViewModel::rebuildMotors()
{
    m_motors.clear();
    m_frameClass.clear();
    m_frameType.clear();

    QList<LayoutMotor> layout;
    int motorMax = 8;
    if (m_vehicleType == kGroundRover || m_vehicleType == kSurfaceBoat) {
        motorMax = 4;
    } else {
        const bool frameEnabled = m_values.contains(QStringLiteral("FRAME"))
            || m_values.contains(QStringLiteral("Q_FRAME_TYPE"))
            || m_values.contains(QStringLiteral("FRAME_TYPE"));
        if (frameEnabled
            && (tryFrameLayout(QStringLiteral("FRAME_CLASS"),
                               QStringLiteral("FRAME_TYPE"), &layout)
                || tryFrameLayout(QStringLiteral("Q_FRAME_CLASS"),
                                  QStringLiteral("Q_FRAME_TYPE"), &layout))) {
            if (!layout.isEmpty()) {
                motorMax = layout.size();
            } else {
                motorMax = fallbackMotorCount();
            }
        } else if (frameEnabled) {
            motorMax = fallbackMotorCount();
        }
    }

    for (int order = 1; order <= motorMax; ++order) {
        MotorTestItem item;
        item.TestOrder = order;
        item.Label = tr("Test motor %1")
            .arg(QChar(static_cast<ushort>('A' + order - 1)));
        const auto found = std::find_if(
            layout.constBegin(), layout.constEnd(),
            [order](const LayoutMotor &motor) {
                return motor.testOrder == order;
            });
        if (found != layout.constEnd()) {
            item.MotorNumber = found->number;
            item.Label += tr("  (Motor %1)").arg(found->number);
            if (!found->rotation.isEmpty()
                && found->rotation != QLatin1String("?")) {
                item.Rotation = found->rotation;
            }
        }
        m_motors.append(item);
    }
    emit motorsChanged();
    emit stateChanged();
}

bool ConfigMotorTestViewModel::tryFrameLayout(
    const QString &classParameter, const QString &typeParameter,
    QList<LayoutMotor> *layout)
{
    if (!layout || !m_values.contains(classParameter)
        || !m_values.contains(typeParameter)) {
        return false;
    }
    const int frameClass = m_values.value(classParameter).toInt();
    const int frameType = m_values.value(typeParameter).toInt();
    const QString classLabel = optionLabel(classParameter, frameClass);
    const QString typeLabel = optionLabel(typeParameter, frameType);
    if (!classLabel.isEmpty()) {
        m_frameClass = tr("Class: %1").arg(classLabel);
    }
    if (!typeLabel.isEmpty()) {
        m_frameType = tr("Type: %1").arg(typeLabel);
    }
    *layout = lookupLayout(frameClass, frameType);
    return true;
}

QList<ConfigMotorTestViewModel::LayoutMotor>
ConfigMotorTestViewModel::lookupLayout(int frameClass, int frameType) const
{
    QList<LayoutMotor> result;
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(m_layoutJson, &error);
    if (error.error != QJsonParseError::NoError
        || !document.isObject()) {
        return result;
    }
    const QJsonArray layouts =
        document.object().value(QStringLiteral("layouts")).toArray();
    for (const QJsonValue &value : layouts) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("Class")).toInt() != frameClass
            || object.value(QStringLiteral("Type")).toInt() != frameType) {
            continue;
        }
        const QJsonArray motors =
            object.value(QStringLiteral("motors")).toArray();
        for (const QJsonValue &motorValue : motors) {
            const QJsonObject motorObject = motorValue.toObject();
            LayoutMotor motor;
            motor.number =
                motorObject.value(QStringLiteral("Number")).toInt();
            motor.testOrder =
                motorObject.value(QStringLiteral("TestOrder")).toInt();
            motor.rotation =
                motorObject.value(QStringLiteral("Rotation")).toString(
                    QStringLiteral("?"));
            if (motor.number > 0 && motor.testOrder > 0) {
                result.append(motor);
            }
        }
        break;
    }
    return result;
}

QString ConfigMotorTestViewModel::optionLabel(
    const QString &name, const QVariant &value) const
{
    const ParameterMetaData metadata = m_catalog.value(name);
    for (const ParameterMetaDataOption &option : metadata.values) {
        if (variantsEqual(option.value, value)) {
            return option.label;
        }
    }
    return QString();
}

int ConfigMotorTestViewModel::fallbackMotorCount() const
{
    int type = kQuadrotor;
    if (m_values.contains(QStringLiteral("Q_FRAME_CLASS"))) {
        switch (m_values.value(QStringLiteral("Q_FRAME_CLASS")).toInt()) {
        case 2:
        case 5:
            type = kHexarotor;
            break;
        case 3:
        case 4:
            type = kOctorotor;
            break;
        case 6:
            type = kHelicopter;
            break;
        case 7:
            type = kTricopter;
            break;
        default:
            type = kQuadrotor;
            break;
        }
    } else if (m_values.contains(QStringLiteral("FRAME"))
               || m_values.contains(QStringLiteral("FRAME_TYPE"))) {
        type = m_vehicleType;
    }

    switch (type) {
    case kTricopter:
    case kQuadrotor:
        return 4;
    case kHexarotor:
        return 6;
    case kOctorotor:
        return 8;
    case kHelicopter:
        return 0;
    case kDodecarotor:
        return 12;
    default:
        return 8;
    }
}

bool ConfigMotorTestViewModel::beginMotorCommands(
    const QList<MotorCommand> &commands, bool confirmed, bool stopBatch)
{
    if (!m_connected) {
        setStatus(tr("Connect to a vehicle first."));
        return false;
    }
    if (!stopBatch && m_armed) {
        setStatus(tr("Disarm the vehicle before motor testing."));
        return false;
    }
    if (!confirmed || commands.isEmpty() || m_commandAckUncertain
        || (!stopBatch && Busy())) {
        return false;
    }

    ++m_commandGeneration;
    m_pendingCommands = commands;
    m_expectedCommandAcks = 0;
    m_receivedCommandAcks = 0;
    m_stopBatch = stopBatch;
    m_stopAckAmbiguous = false;
    m_stopSawArmed = false;
    m_stopAttempted = false;
    m_waitingForSequenceDisarm = false;
    m_sequenceDelayElapsed = false;
    m_motorTestArmedObserved = false;
    m_motorTestDisarmedAfterArmObserved = false;
    int activityMs = 0;
    for (int index = 0; index < commands.size(); ++index) {
        const MotorCommand &command = commands.at(index);
        activityMs += motorCommandDurationMs(command);
        if (index + 1 < commands.size()) {
            activityMs += command.durationSec * 500;
        }
    }
    if (activityMs > 0) {
        m_testMayBeActive = true;
        m_testActivityDurationMs = activityMs;
        ++m_testActivityGeneration;
        scheduleActivityExpiry(activityMs + kCommandTimeoutMs);
    } else {
        ++m_testActivityGeneration;
        m_testMayBeActive = false;
        m_testActivityDurationMs = 0;
    }
    setStatus(stopBatch
                  ? tr("Stopping all motors…")
                  : tr("Waiting for motor-test confirmation…"));
    emit stateChanged();
    dispatchNextMotorCommand();
    return true;
}

void ConfigMotorTestViewModel::dispatchNextMotorCommand()
{
    if (m_pendingCommands.isEmpty() || m_stopBatch) {
        return;
    }
    m_motorTestArmedObserved = false;
    m_motorTestDisarmedAfterArmObserved = false;
    m_expectedCommandAcks = 1;
    m_receivedCommandAcks = 0;
    const MotorCommand &command = m_pendingCommands.first();
    emit motorTestRequested(
        kAutopilotComponent, command.motor, kThrottlePercent,
        command.throttle, command.durationSec,
        command.motorCount, kTestOrderDefault);
    scheduleCommandTimeout();
}

void ConfigMotorTestViewModel::maybeDispatchNextMotorCommand()
{
    if (!m_waitingForSequenceDisarm || !m_sequenceDelayElapsed
        || !m_motorTestDisarmedAfterArmObserved || m_armed
        || m_pendingCommands.isEmpty()) {
        return;
    }
    m_waitingForSequenceDisarm = false;
    m_sequenceDelayElapsed = false;
    ++m_commandGeneration;
    dispatchNextMotorCommand();
    emit stateChanged();
}

int ConfigMotorTestViewModel::motorCommandDurationMs(
    const MotorCommand &command) const
{
    if (command.durationSec <= 0) {
        return 0;
    }
    const int count = qMax(1, command.motorCount);
    return command.durationSec * 1000 * (3 * count - 1) / 2;
}

void ConfigMotorTestViewModel::finishStop(const QString &status)
{
    ++m_commandGeneration;
    ++m_testActivityGeneration;
    m_pendingCommands.clear();
    m_expectedCommandAcks = 0;
    m_receivedCommandAcks = 0;
    m_stopBatch = false;
    m_stopAckAmbiguous = false;
    m_stopSawArmed = false;
    m_waitingForSequenceDisarm = false;
    m_sequenceDelayElapsed = false;
    m_testMayBeActive = false;
    m_testActivityDurationMs = 0;
    m_commandAckUncertain = false;
    m_motorTestArmedObserved = false;
    m_motorTestDisarmedAfterArmObserved = false;
    setStatus(status);
    emit stateChanged();
}

void ConfigMotorTestViewModel::scheduleCommandTimeout()
{
    const quint64 generation = m_commandGeneration;
    QTimer::singleShot(kCommandTimeoutMs, this, [this, generation]() {
        if (generation == m_commandGeneration
            && m_expectedCommandAcks > 0) {
            finishMotorCommands(
                tr("Failed to test motor: command timeout."));
            m_commandAckUncertain = true;
            emit stateChanged();
        }
    });
}

void ConfigMotorTestViewModel::scheduleActivityExpiry(int delayMs)
{
    const quint64 activityGeneration = m_testActivityGeneration;
    QTimer::singleShot(qMax(0, delayMs), this,
                       [this, activityGeneration]() {
        if (activityGeneration == m_testActivityGeneration
            && m_testMayBeActive) {
            m_testMayBeActive = false;
            m_testActivityDurationMs = 0;
            emit stateChanged();
        }
    });
}

bool ConfigMotorTestViewModel::requireParameter(const QString &name)
{
    if (!m_connected) {
        setStatus(tr("Connect to a vehicle first."));
        return false;
    }
    if (m_armed) {
        setStatus(tr("Disarm the vehicle before motor testing."));
        return false;
    }
    if (Busy()) {
        return false;
    }
    if (m_testMayBeActive || m_commandAckUncertain) {
        return false;
    }
    if (!m_values.contains(name)) {
        setStatus(tr("param %1 missing.").arg(name));
        return false;
    }
    return true;
}

void ConfigMotorTestViewModel::queueParameterWrite(
    const QString &name, double value)
{
    m_pendingParameter.name = name;
    m_pendingParameter.expectedValue = value;
    m_pendingParameter.generation = ++m_parameterGeneration;
    emit stateChanged();
    emit writeRequested(m_componentId, name, value);

    const quint64 generation = m_pendingParameter.generation;
    QTimer::singleShot(kParameterTimeoutMs, this, [this, generation]() {
        if (m_pendingParameter.generation == generation
            && !m_pendingParameter.name.isEmpty()) {
            finishParameterWrite(false, tr("write timeout"));
        }
    });
}

void ConfigMotorTestViewModel::finishParameterWrite(
    bool matched, const QString &reason)
{
    if (m_pendingParameter.name.isEmpty()) {
        return;
    }
    const QString name = m_pendingParameter.name;
    const double value = m_pendingParameter.expectedValue.toDouble();
    m_pendingParameter = {};
    if (matched) {
        setStatus(tr("%1 set to %2.")
                      .arg(name, QString::number(value, 'f', 2)));
    } else {
        Q_UNUSED(reason)
        setStatus(tr("Failed to set %1.").arg(name));
    }
    emit stateChanged();
}

void ConfigMotorTestViewModel::finishMotorCommands(const QString &status)
{
    ++m_commandGeneration;
    m_pendingCommands.clear();
    m_expectedCommandAcks = 0;
    m_receivedCommandAcks = 0;
    m_stopBatch = false;
    m_stopAckAmbiguous = false;
    m_stopSawArmed = false;
    m_waitingForSequenceDisarm = false;
    m_sequenceDelayElapsed = false;
    setStatus(status);
    emit stateChanged();
}

void ConfigMotorTestViewModel::cancelPending(const QString &status)
{
    ++m_commandGeneration;
    ++m_parameterGeneration;
    m_pendingCommands.clear();
    m_expectedCommandAcks = 0;
    m_receivedCommandAcks = 0;
    m_pendingParameter = {};
    m_stopBatch = false;
    m_stopAckAmbiguous = false;
    m_stopSawArmed = false;
    m_waitingForSequenceDisarm = false;
    m_sequenceDelayElapsed = false;
    m_stopAttempted = false;
    setStatus(status);
}

void ConfigMotorTestViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged();
}

QString ConfigMotorTestViewModel::normalizedName(const QString &name) const
{
    return name.trimmed().toUpper();
}
