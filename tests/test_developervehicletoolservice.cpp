#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/ParameterService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterStore.h"
#include "services/DeveloperVehicleToolService.h"

#include <QtTest>

#include <algorithm>
#include <cstring>
#include <functional>

namespace
{
struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    return state == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

mavlink_message_t heartbeat(int systemId, bool armed)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        static_cast<quint8>(systemId), MAV_COMP_ID_AUTOPILOT1, &message,
        MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        0, MAV_STATE_STANDBY);
    return message;
}

void copyParameterName(const QString &name, char id[16])
{
    const QByteArray encoded = name.toLatin1();
    const int count = std::min(16, encoded.size());
    std::memset(id, 0, 16);
    std::memcpy(id, encoded.constData(), static_cast<size_t>(count));
}

mavlink_message_t parameterValue(const VehicleEndpoint &source,
                                 const QString &name,
                                 double value,
                                 ParameterType type,
                                 int count = 1,
                                 int index = 0)
{
    bool encoded = false;
    mavlink_param_value_t payload{};
    payload.param_value = ParameterCodec::encodeClassic(
        value, type, ParameterEncoding::Bytewise, &encoded);
    Q_ASSERT(encoded);
    copyParameterName(name, payload.param_id);
    payload.param_type = static_cast<quint8>(type);
    payload.param_count = static_cast<quint16>(count);
    payload.param_index = static_cast<quint16>(index);
    mavlink_message_t message{};
    mavlink_msg_param_value_encode(
        static_cast<quint8>(source.systemId),
        static_cast<quint8>(source.componentId), &message, &payload);
    return message;
}

mavlink_message_t commandAck(const VehicleEndpoint &source,
                             MAV_CMD command,
                             MAV_RESULT result)
{
    mavlink_command_ack_t payload{};
    payload.command = static_cast<quint16>(command);
    payload.result = static_cast<quint8>(result);
    payload.progress = 255;
    // Zero target fields are the MAVLink 1/extension-compatible ACK form.
    payload.target_system = 0;
    payload.target_component = 0;
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(
        static_cast<quint8>(source.systemId),
        static_cast<quint8>(source.componentId), &message, &payload);
    return message;
}

struct ParameterSeed
{
    QString name;
    QVariant value;
    ParameterType type = ParameterType::Unknown;
};

class Fixture
{
public:
    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
              frames.append(CapturedFrame{linkId, bytes});
              if (onWrite) {
                  onWrite(decodeFrame(bytes));
              }
              return true;
          })
        , parameters(&targets, &transmitter)
        , commands(&targets, &transmitter)
        , tools(&targets, &registry, &parameters, &commands,
                [this](const SwarmVehicleInstanceLease &candidate,
                       QString *error) {
                    if (routeHook) {
                        routeHook();
                    }
                    if (!routeAllowed) {
                        if (error) {
                            *error = QStringLiteral("Test route is disabled.");
                        }
                        return false;
                    }
                    return registry.validateLease(
                        candidate,
                        DeveloperVehicleToolService::MaximumHeartbeatAgeMs);
                })
    {
        linkEpoch = registry.beginLinkSession(linkId,
                                              QStringLiteral("Test link"));
        setArmed(false);
        QVERIFY(parameters.configureExactTransactions(
            [this](const SwarmVehicleInstanceLease &candidate) {
                return registry.validateLease(candidate, 3000);
            },
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
        QVERIFY(parameters.configureSingleVehicleExactRoute(
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
        QVERIFY(commands.configureExactTransactions(
            [this](const SwarmVehicleInstanceLease &candidate) {
                return registry.validateLease(candidate, 3000);
            },
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
        QVERIFY(commands.configureSingleVehicleExactRoute(
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
    }

    void setArmed(bool armed)
    {
        const mavlink_message_t update = heartbeat(systemId, armed);
        QVERIFY(registry.observeMessage(linkId, linkEpoch, update));
        const QList<VehicleEndpoint> endpoints = registry.endpoints();
        QVERIFY(!endpoints.isEmpty());
        vehicleEndpoint = endpoints.constFirst();
        if (!targets.contains(
                vehicleEndpoint.linkId,
                vehicleEndpoint.systemId,
                vehicleEndpoint.componentId)) {
            QVERIFY(targets.observeEndpoint(vehicleEndpoint, true));
        }
        targets.observeHeartbeat(
            vehicleEndpoint, armed, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
    }

    bool loadParameters(const QList<ParameterSeed> &seeds)
    {
        if (seeds.isEmpty()) {
            return false;
        }
        ParameterStore *store = parameters.store();
        store->selectEndpoint(vehicleEndpoint);
        store->beginLoad(vehicleEndpoint);
        for (int index = 0; index < seeds.size(); ++index) {
            const ParameterSeed &seed = seeds.at(index);
            if (!store->ingest(
                    vehicleEndpoint, seeds.size(), index,
                    seed.name, seed.value, seed.type)) {
                return false;
            }
        }
        store->finishLoad(vehicleEndpoint);
        return store->snapshot(vehicleEndpoint).isComplete();
    }

    SwarmVehicleInstanceLease lease() const
    {
        return registry.acquireVehicle(vehicleEndpoint, 3000);
    }

    static constexpr int linkId = 17;
    static constexpr int systemId = 42;

    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    QVector<CapturedFrame> frames;
    std::function<void(const mavlink_message_t &)> onWrite;
    std::function<void()> routeHook;
    bool routeAllowed = true;
    ExactLinkTransmitter transmitter;
    ParameterService parameters;
    VehicleCommandService commands;
    DeveloperVehicleToolService tools;
    quint64 linkEpoch = 0;
    VehicleEndpoint vehicleEndpoint;
};

mavlink_command_long_t commandPayload(const CapturedFrame &frame)
{
    const mavlink_message_t message = decodeFrame(frame.bytes);
    mavlink_command_long_t payload{};
    if (message.msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
        mavlink_msg_command_long_decode(&message, &payload);
    }
    return payload;
}
}

class DeveloperVehicleToolServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void eligibilityAndSnapshotFence();
    void qnhAndAltitudeAreAckGated();
    void fallbackRequiresInternalWriteCapability();
    void pressureRequiresReal32();
    void commandsUseMissionPlannerPayloadsAndTerminalReports();
    void levelCalibrationRejectedAndGated();
    void bootloaderRejectsWrongEvidenceAndTimesOutUncertain();
    void bootloaderWriteGateRejectsArmedAndSelectionAba();
    void deletionAtFinalCommandValidationIsSafe();
    void retryRejectsChangedPressureSnapshot();
    void commandWriteGateRejectsArmedTransition();
    void reentrantPlanAndReleaseCallbacksCannotOverlap();
    void synchronousCompletionAndShutdownAreSafe();
};

void DeveloperVehicleToolServiceTest::eligibilityAndSnapshotFence()
{
    Fixture fixture;
    QVERIFY(fixture.loadParameters({
        {QStringLiteral("GND_ABS_PRESS"), 101325.0,
         ParameterType::Real32}}));

    QString error;
    QVERIFY(fixture.tools.canPrepare(
        DeveloperVehicleToolService::Action::SetQnh, &error));
    DeveloperVehicleToolService::Plan plan;
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::SetQnh, &plan, &error));
    QVERIFY(plan.isValid());
    QCOMPARE(plan.parameterName, QStringLiteral("GND_ABS_PRESS"));
    QCOMPARE(plan.vehicle.endpoint.componentId,
             int(MAV_COMP_ID_AUTOPILOT1));
    QVERIFY(fixture.tools.validate(plan, &error));

    QVERIFY(fixture.loadParameters({
        {QStringLiteral("GND_ABS_PRESS"), 101400.0,
         ParameterType::Real32}}));
    QVERIFY(!fixture.tools.validate(plan, &error));
    QVERIFY(error.contains(QStringLiteral("snapshot"),
                           Qt::CaseInsensitive));
    QCOMPARE(fixture.tools.execute(plan, 102000.0, &error),
             DeveloperVehicleToolService::SubmitResult::Unavailable);
    QVERIFY(fixture.frames.isEmpty());

    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::RebootVehicle,
        &plan, &error));
    fixture.setArmed(true);
    QVERIFY(!fixture.tools.validate(plan, &error));
    QVERIFY(error.contains(QStringLiteral("armed"),
                           Qt::CaseInsensitive));
}

void DeveloperVehicleToolServiceTest::qnhAndAltitudeAreAckGated()
{
    Fixture fixture;
    QVERIFY(fixture.loadParameters({
        {QStringLiteral("GND_ABS_PRESS"), 101325.0,
         ParameterType::Real32}}));
    QSignalSpy finished(
        &fixture.tools,
        &DeveloperVehicleToolService::operationFinished);

    DeveloperVehicleToolService::Plan qnh;
    QString error;
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::SetQnh, &qnh, &error));
    QCOMPARE(fixture.tools.execute(qnh, 102000.0, &error),
             DeveloperVehicleToolService::SubmitResult::Started);
    QVERIFY(fixture.tools.busy());
    QCOMPARE(finished.count(), 0);
    QCOMPARE(fixture.frames.size(), 1);
    const mavlink_message_t sent = decodeFrame(fixture.frames.constLast().bytes);
    QCOMPARE(sent.msgid, quint32(MAVLINK_MSG_ID_PARAM_SET));
    mavlink_param_set_t parameterSet{};
    mavlink_msg_param_set_decode(&sent, &parameterSet);
    QCOMPARE(parameterSet.target_system, quint8(Fixture::systemId));
    QCOMPARE(parameterSet.target_component,
             quint8(MAV_COMP_ID_AUTOPILOT1));

    fixture.parameters.observePhysicalMessage(
        Fixture::linkId, fixture.linkEpoch,
        parameterValue(fixture.vehicleEndpoint, qnh.parameterName,
                       102000.0, qnh.parameterType));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!fixture.tools.busy());
    QCOMPARE(fixture.tools.lastReport().outcome,
             DeveloperVehicleToolService::Outcome::Succeeded);

    DeveloperVehicleToolService::Plan altitude;
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::AdjustBarometerAltitude,
        &altitude, &error));
    fixture.frames.clear();
    QCOMPARE(fixture.tools.execute(altitude, 10.0, &error),
             DeveloperVehicleToolService::SubmitResult::Started);
    QCOMPARE(fixture.frames.size(), 1);
    const mavlink_message_t adjustedFrame =
        decodeFrame(fixture.frames.constFirst().bytes);
    mavlink_param_set_t adjusted{};
    mavlink_msg_param_set_decode(&adjustedFrame, &adjusted);
    bool decoded = false;
    const QVariant adjustedValue = ParameterCodec::decodeClassic(
        adjusted.param_value, altitude.parameterType,
        ParameterEncoding::Bytewise, &decoded);
    QVERIFY(decoded);
    QCOMPARE(adjustedValue.toFloat(), 102111.0F);

    fixture.parameters.observePhysicalMessage(
        Fixture::linkId, fixture.linkEpoch,
        parameterValue(fixture.vehicleEndpoint, altitude.parameterName,
                       102111.0, altitude.parameterType));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(fixture.tools.execute(altitude, 0.0, &error),
             DeveloperVehicleToolService::SubmitResult::InvalidPlan);
}

void DeveloperVehicleToolServiceTest::
fallbackRequiresInternalWriteCapability()
{
    Fixture fixture;
    QVERIFY(fixture.loadParameters({
        {QStringLiteral("BARO1_GND_PRESS"), 101325.0,
         ParameterType::Real32}}));
    DeveloperVehicleToolService::Plan plan;
    QString error;
    QVERIFY(!fixture.tools.prepare(
        DeveloperVehicleToolService::Action::SetQnh, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("BRD_OPTIONS")));

    QVERIFY(fixture.loadParameters({
        {QStringLiteral("BARO1_GND_PRESS"), 101325.0,
         ParameterType::Real32},
        {QStringLiteral("BRD_OPTIONS"), 4,
         ParameterType::Int32}}));
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::SetQnh, &plan, &error));
    QCOMPARE(plan.parameterName, QStringLiteral("BARO1_GND_PRESS"));
    QCOMPARE(plan.capabilityParameterName, QStringLiteral("BRD_OPTIONS"));
    QVERIFY(fixture.tools.validate(plan, &error));

    QVERIFY(fixture.loadParameters({
        {QStringLiteral("BARO1_GND_PRESS"), 101325.0,
         ParameterType::Real32},
        {QStringLiteral("BRD_OPTIONS"), 0,
         ParameterType::Int32}}));
    QVERIFY(!fixture.tools.validate(plan, &error));
    QVERIFY(error.contains(QStringLiteral("snapshot changed")));
    QVERIFY(!fixture.tools.prepare(
        DeveloperVehicleToolService::Action::SetQnh, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("BRD_OPTIONS")));
}

void DeveloperVehicleToolServiceTest::pressureRequiresReal32()
{
    Fixture fixture;
    QVERIFY(fixture.loadParameters({
        {QStringLiteral("GND_ABS_PRESS"), 101325, ParameterType::Int32}}));
    fixture.frames.clear();
    QString error;
    DeveloperVehicleToolService::Plan plan;
    QVERIFY(!fixture.tools.canPrepare(DeveloperVehicleToolService::Action::SetQnh, &error));
    QVERIFY(error.contains(QStringLiteral("REAL32")));
    QVERIFY(!fixture.tools.prepare(
        DeveloperVehicleToolService::Action::AdjustBarometerAltitude, &plan, &error));
    QVERIFY(!plan.isValid());
    QVERIFY(fixture.frames.isEmpty());
}

void DeveloperVehicleToolServiceTest::
commandsUseMissionPlannerPayloadsAndTerminalReports()
{
    Fixture fixture;
    struct CommandCase
    {
        DeveloperVehicleToolService::Action action;
        MAV_CMD command;
        std::array<float, 7> params;
    };
    const QList<CommandCase> cases{
        {DeveloperVehicleToolService::Action::CalibrateLevel,
         MAV_CMD_PREFLIGHT_CALIBRATION, {0, 0, 0, 0, 2, 0, 0}},
        {DeveloperVehicleToolService::Action::SimpleAccelCalibration,
         MAV_CMD_PREFLIGHT_CALIBRATION, {0, 0, 0, 0, 4, 0, 0}},
        {DeveloperVehicleToolService::Action::ForceAccelCalibrated,
         MAV_CMD_PREFLIGHT_CALIBRATION, {0, 0, 0, 0, 76, 0, 0}},
        {DeveloperVehicleToolService::Action::ForceCompassCalibrated,
         MAV_CMD_PREFLIGHT_CALIBRATION, {0, 76, 0, 0, 0, 0, 0}},
        {DeveloperVehicleToolService::Action::RebootVehicle,
         MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, {1, 0, 0, 0, 0, 0, 0}},
        {DeveloperVehicleToolService::Action::RebootToDfu,
         MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, {42, 24, 71, 99, 0, 0, 0}},
        {DeveloperVehicleToolService::Action::UpgradeBootloader,
         static_cast<MAV_CMD>(MAV_CMD_FLASH_BOOTLOADER),
         {0, 0, 0, 0,
          DeveloperVehicleToolService::BootloaderMagic, 0, 0}}
    };

    QSignalSpy finished(
        &fixture.tools,
        &DeveloperVehicleToolService::operationFinished);
    for (int index = 0; index < cases.size(); ++index) {
        const CommandCase &candidate = cases.at(index);
        DeveloperVehicleToolService::Plan plan;
        QString error;
        QVERIFY(fixture.tools.prepare(candidate.action, &plan, &error));
        fixture.frames.clear();
        QCOMPARE(fixture.tools.execute(plan, 0.0, &error),
                 DeveloperVehicleToolService::SubmitResult::Started);
        QVERIFY(fixture.tools.busy());
        QCOMPARE(fixture.frames.size(), 1);
        const mavlink_command_long_t payload =
            commandPayload(fixture.frames.constFirst());
        QCOMPARE(payload.command, quint16(candidate.command));
        QCOMPARE(payload.param1, candidate.params[0]);
        QCOMPARE(payload.param2, candidate.params[1]);
        QCOMPARE(payload.param3, candidate.params[2]);
        QCOMPARE(payload.param4, candidate.params[3]);
        QCOMPARE(payload.param5, candidate.params[4]);
        QCOMPARE(payload.param6, candidate.params[5]);
        QCOMPARE(payload.param7, candidate.params[6]);
        QCOMPARE(payload.confirmation, quint8(0));
        QCOMPARE(payload.target_system, quint8(Fixture::systemId));
        QCOMPARE(payload.target_component,
                 quint8(MAV_COMP_ID_AUTOPILOT1));

        if (candidate.action
            == DeveloperVehicleToolService::Action::RebootToDfu) {
            fixture.commands.retireExactVehicle(plan.vehicle);
            QCOMPARE(fixture.tools.lastReport().outcome,
                     DeveloperVehicleToolService::Outcome::OutcomeUncertain);
            QVERIFY(fixture.tools.lastReport().description.contains(
                QStringLiteral("does not confirm DFU entry")));
        } else {
            fixture.commands.observeMessage(
                Fixture::linkId,
                commandAck(fixture.vehicleEndpoint,
                           candidate.command, MAV_RESULT_ACCEPTED));
            QCOMPARE(fixture.tools.lastReport().outcome,
                     DeveloperVehicleToolService::Outcome::Succeeded);
            if (candidate.action
                == DeveloperVehicleToolService::Action::UpgradeBootloader) {
                QVERIFY(fixture.tools.lastReport().description.contains(
                    QStringLiteral("flashed or was already current")));
            }
        }
        QVERIFY(!fixture.tools.busy());
        QCOMPARE(finished.count(), index + 1);
    }
}

void DeveloperVehicleToolServiceTest::levelCalibrationRejectedAndGated()
{
    QCOMPARE(
        DeveloperVehicleToolService::CalibrationAcknowledgementTimeoutMs,
        25000);
    QVERIFY(VehicleCommandService::DefaultExactCommandMaximumLifetimeMs
            >= 30000);
    Fixture fixture;
    DeveloperVehicleToolService::Plan plan;
    QString error;
    QVERIFY(fixture.tools.canPrepare(
        DeveloperVehicleToolService::Action::CalibrateLevel, &error));
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::CalibrateLevel,
        &plan, &error));
    QCOMPARE(fixture.tools.execute(plan, 0.0, &error),
             DeveloperVehicleToolService::SubmitResult::Started);
    QCOMPARE(fixture.frames.size(), 1);
    const mavlink_command_long_t payload =
        commandPayload(fixture.frames.constFirst());
    QCOMPARE(payload.command, quint16(MAV_CMD_PREFLIGHT_CALIBRATION));
    QCOMPARE(payload.param1, 0.0F);
    QCOMPARE(payload.param2, 0.0F);
    QCOMPARE(payload.param3, 0.0F);
    QCOMPARE(payload.param4, 0.0F);
    QCOMPARE(payload.param5, 2.0F);
    QCOMPARE(payload.param6, 0.0F);
    QCOMPARE(payload.param7, 0.0F);

    fixture.commands.observeMessage(
        Fixture::linkId,
        commandAck(fixture.vehicleEndpoint,
                   MAV_CMD_PREFLIGHT_CALIBRATION,
                   MAV_RESULT_DENIED));
    QVERIFY(!fixture.tools.busy());
    QCOMPARE(fixture.tools.lastReport().action,
             DeveloperVehicleToolService::Action::CalibrateLevel);
    QCOMPARE(fixture.tools.lastReport().outcome,
             DeveloperVehicleToolService::Outcome::Rejected);

    fixture.setArmed(true);
    fixture.frames.clear();
    plan = {};
    QVERIFY(!fixture.tools.canPrepare(
        DeveloperVehicleToolService::Action::CalibrateLevel, &error));
    QVERIFY(!fixture.tools.prepare(
        DeveloperVehicleToolService::Action::CalibrateLevel,
        &plan, &error));
    QVERIFY(!plan.isValid());
    QVERIFY(fixture.frames.isEmpty());
}

void DeveloperVehicleToolServiceTest::
bootloaderRejectsWrongEvidenceAndTimesOutUncertain()
{
    QCOMPARE(DeveloperVehicleToolService::BootloaderAcknowledgementTimeoutMs,
             5 * 60 * 1000);
    QCOMPARE(DeveloperVehicleToolService::BootloaderMaximumLifetimeMs,
             5 * 60 * 1000);
    QCOMPARE(quint16(MAV_CMD_FLASH_BOOTLOADER), quint16(42650));

    {
        Fixture fixture;
        // The bootloader's explicit timeout must override the short generic
        // command default, not accidentally inherit the normal ACK window.
        fixture.commands.setExactCommandTimeoutForTesting(5);
        DeveloperVehicleToolService::Plan plan;
        QString error;
        QVERIFY(fixture.tools.canPrepare(
            DeveloperVehicleToolService::Action::UpgradeBootloader,
            &error));
        QVERIFY(fixture.tools.prepare(
            DeveloperVehicleToolService::Action::UpgradeBootloader,
            &plan, &error));
        QCOMPARE(fixture.tools.execute(plan, 123.0, &error),
                 DeveloperVehicleToolService::SubmitResult::Started);
        QCOMPARE(fixture.frames.size(), 1);
        QTest::qWait(30);
        QVERIFY(fixture.tools.busy());
        QCOMPARE(fixture.frames.size(), 1);

        fixture.commands.observeMessage(
            Fixture::linkId,
            commandAck(fixture.vehicleEndpoint,
                       MAV_CMD_PREFLIGHT_CALIBRATION,
                       MAV_RESULT_ACCEPTED));
        QVERIFY(fixture.tools.busy());
        QCOMPARE(fixture.frames.size(), 1);

        VehicleEndpoint wrongSource = fixture.vehicleEndpoint;
        wrongSource.systemId += 1;
        fixture.commands.observeMessage(
            Fixture::linkId,
            commandAck(wrongSource,
                       static_cast<MAV_CMD>(MAV_CMD_FLASH_BOOTLOADER),
                       MAV_RESULT_ACCEPTED));
        QVERIFY(fixture.tools.busy());
        QCOMPARE(fixture.frames.size(), 1);

        fixture.commands.observeMessage(
            Fixture::linkId,
            commandAck(fixture.vehicleEndpoint,
                       static_cast<MAV_CMD>(MAV_CMD_FLASH_BOOTLOADER),
                       MAV_RESULT_DENIED));
        QVERIFY(!fixture.tools.busy());
        QCOMPARE(fixture.tools.lastReport().outcome,
                 DeveloperVehicleToolService::Outcome::Rejected);
        QVERIFY(fixture.tools.lastReport().description.contains(
            QStringLiteral("may not support bootloader flashing")));
        QVERIFY(fixture.tools.lastReport().description.contains(
            QStringLiteral("Keep power connected")));
        QCOMPARE(fixture.frames.size(), 1);
    }

    {
        Fixture fixture;
        fixture.tools.setBootloaderTimeoutForTesting(20);
        DeveloperVehicleToolService::Plan plan;
        QString error;
        QVERIFY(fixture.tools.prepare(
            DeveloperVehicleToolService::Action::UpgradeBootloader,
            &plan, &error));
        QCOMPARE(fixture.tools.execute(plan, 0.0, &error),
                 DeveloperVehicleToolService::SubmitResult::Started);
        QCOMPARE(fixture.frames.size(), 1);
        QTest::qWait(5);
        QVERIFY(fixture.tools.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.tools.busy(), 250);
        QCOMPARE(fixture.frames.size(), 1);
        QCOMPARE(fixture.tools.lastReport().outcome,
                 DeveloperVehicleToolService::Outcome::OutcomeUncertain);
        QVERIFY(fixture.tools.lastReport().description.contains(
            QStringLiteral("Keep power connected")));
        QVERIFY(fixture.tools.lastReport().description.contains(
            QStringLiteral("do not retry or power-cycle automatically")));
    }

    {
        Fixture fixture;
        DeveloperVehicleToolService::Plan plan;
        QString error;
        QVERIFY(fixture.tools.prepare(
            DeveloperVehicleToolService::Action::UpgradeBootloader,
            &plan, &error));
        QCOMPARE(fixture.tools.execute(plan, 0.0, &error),
                 DeveloperVehicleToolService::SubmitResult::Started);
        QCOMPARE(fixture.frames.size(), 1);
        fixture.commands.retireExactVehicle(plan.vehicle);
        QVERIFY(!fixture.tools.busy());
        QCOMPARE(fixture.frames.size(), 1);
        QCOMPARE(fixture.tools.lastReport().outcome,
                 DeveloperVehicleToolService::Outcome::OutcomeUncertain);
        QVERIFY(fixture.tools.lastReport().description.contains(
            QStringLiteral("Keep power connected")));
    }
}

void DeveloperVehicleToolServiceTest::
bootloaderWriteGateRejectsArmedAndSelectionAba()
{
    {
        Fixture fixture;
        int busyRouteValidations = 0;
        fixture.routeHook = [&fixture, &busyRouteValidations]() {
            if (fixture.tools.busy()
                && ++busyRouteValidations == 2) {
                fixture.setArmed(true);
            }
        };
        DeveloperVehicleToolService::Plan plan;
        QString error;
        QVERIFY(fixture.tools.prepare(
            DeveloperVehicleToolService::Action::UpgradeBootloader,
            &plan, &error));
        QCOMPARE(fixture.tools.execute(plan, 0.0, &error),
                 DeveloperVehicleToolService::SubmitResult::Unavailable);
        QVERIFY(fixture.frames.isEmpty());
        QVERIFY(!fixture.tools.busy());
        QCOMPARE(fixture.tools.lastReport().outcome,
                 DeveloperVehicleToolService::Outcome::Rejected);
    }

    {
        Fixture fixture;
        int busyRouteValidations = 0;
        fixture.routeHook = [&fixture, &busyRouteValidations]() {
            if (!fixture.tools.busy()
                || ++busyRouteValidations != 2) {
                return;
            }
            VehicleEndpoint alternate;
            alternate.linkId = Fixture::linkId + 1;
            alternate.systemId = Fixture::systemId + 1;
            alternate.componentId = MAV_COMP_ID_AUTOPILOT1;
            QVERIFY(fixture.targets.observeEndpoint(alternate));
            QVERIFY(fixture.targets.selectTarget(
                alternate.linkId, alternate.systemId,
                alternate.componentId));
            QVERIFY(fixture.targets.selectTarget(
                fixture.vehicleEndpoint.linkId,
                fixture.vehicleEndpoint.systemId,
                fixture.vehicleEndpoint.componentId));
            fixture.targets.observeHeartbeat(
                fixture.vehicleEndpoint, false,
                MAV_AUTOPILOT_ARDUPILOTMEGA,
                MAV_TYPE_QUADROTOR);
        };
        DeveloperVehicleToolService::Plan plan;
        QString error;
        QVERIFY(fixture.tools.prepare(
            DeveloperVehicleToolService::Action::UpgradeBootloader,
            &plan, &error));
        QCOMPARE(fixture.tools.execute(plan, 0.0, &error),
                 DeveloperVehicleToolService::SubmitResult::Unavailable);
        QVERIFY(fixture.frames.isEmpty());
        QVERIFY(!fixture.tools.busy());
        QCOMPARE(fixture.tools.lastReport().outcome,
                 DeveloperVehicleToolService::Outcome::Rejected);
    }
}

void DeveloperVehicleToolServiceTest::
deletionAtFinalCommandValidationIsSafe()
{
    Fixture fixture;
    QPointer<DeveloperVehicleToolService> tools;
    int busyValidations = 0;
    tools = new DeveloperVehicleToolService(
        &fixture.targets, &fixture.registry,
        &fixture.parameters, &fixture.commands,
        [&fixture, &tools, &busyValidations](
            const SwarmVehicleInstanceLease &lease, QString *) {
            if (tools && tools->busy() && ++busyValidations == 1) {
                delete tools.data();
                return false;
            }
            return fixture.registry.validateLease(
                lease,
                DeveloperVehicleToolService::MaximumHeartbeatAgeMs);
        });

    DeveloperVehicleToolService::Plan plan;
    QString error;
    QVERIFY(tools->prepare(
        DeveloperVehicleToolService::Action::UpgradeBootloader,
        &plan, &error));
    DeveloperVehicleToolService *rawTools = tools.data();
    QCOMPARE(rawTools->execute(plan, 0.0, &error),
             DeveloperVehicleToolService::SubmitResult::Unavailable);
    QVERIFY(tools.isNull());
    QVERIFY(fixture.frames.isEmpty());

    // Destruction released the reservation made immediately before the
    // callback; the exact command lane remains usable by another owner.
    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(fixture.commands.reserveSingleVehicleEndpoint(
                 &owner, fixture.targets.acquireTarget(), fixture.lease(),
                 &reservation, &error),
             VehicleCommandService::ExactReservationResult::Reserved);
    QVERIFY(reservation.isValid());
    fixture.commands.releaseExactReservation(reservation);
}

void DeveloperVehicleToolServiceTest::retryRejectsChangedPressureSnapshot()
{
    Fixture fixture;
    fixture.parameters.setExactRetryPolicyForTesting(
        10, 0, 5, 2, 100, 50);
    QVERIFY(fixture.loadParameters({
        {QStringLiteral("GND_ABS_PRESS"), 101325.0,
         ParameterType::Real32}}));
    DeveloperVehicleToolService::Plan plan;
    QString error;
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::SetQnh, &plan, &error));
    QCOMPARE(fixture.tools.execute(plan, 102000.0, &error),
             DeveloperVehicleToolService::SubmitResult::Started);
    QCOMPARE(fixture.frames.size(), 1);

    // A complete replacement snapshot arriving before the retry invalidates
    // the consent plan.  The first frame makes the outcome uncertain, but no
    // second frame may be emitted.
    QVERIFY(fixture.loadParameters({
        {QStringLiteral("GND_ABS_PRESS"), 101400.0,
         ParameterType::Real32}}));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.tools.busy(), 200);
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(fixture.tools.lastReport().outcome,
             DeveloperVehicleToolService::Outcome::OutcomeUncertain);
}

void DeveloperVehicleToolServiceTest::
commandWriteGateRejectsArmedTransition()
{
    Fixture fixture;
    int busyRouteValidations = 0;
    fixture.routeHook = [&fixture, &busyRouteValidations]() {
        if (fixture.tools.busy()
            && ++busyRouteValidations == 2) {
            fixture.setArmed(true);
        }
    };
    DeveloperVehicleToolService::Plan plan;
    QString error;
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::ForceAccelCalibrated,
        &plan, &error));
    QCOMPARE(fixture.tools.execute(plan, 0.0, &error),
             DeveloperVehicleToolService::SubmitResult::Unavailable);
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(!fixture.tools.busy());
    QCOMPARE(fixture.tools.lastReport().outcome,
             DeveloperVehicleToolService::Outcome::Rejected);
}

void DeveloperVehicleToolServiceTest::
reentrantPlanAndReleaseCallbacksCannotOverlap()
{
    Fixture fixture;
    DeveloperVehicleToolService::Plan plan;
    QString error;
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::RebootVehicle,
        &plan, &error));

    bool attemptedDuringRoute = false;
    bool replacementAdmitted = true;
    fixture.routeHook = [&]() {
        if (attemptedDuringRoute) {
            return;
        }
        attemptedDuringRoute = true;
        DeveloperVehicleToolService::Plan replacement;
        replacementAdmitted = fixture.tools.prepare(
            DeveloperVehicleToolService::Action::RebootToDfu,
            &replacement);
    };
    bool attemptedDuringRelease = false;
    bool releaseAdmission = true;
    connect(&fixture.commands,
            &VehicleCommandService::exactReservationReleased,
            &fixture.tools,
            [&](qulonglong) {
                attemptedDuringRelease = true;
                DeveloperVehicleToolService::Plan replacement;
                releaseAdmission = fixture.tools.prepare(
                    DeveloperVehicleToolService::Action::RebootToDfu,
                    &replacement);
            });

    QCOMPARE(fixture.tools.execute(plan, 0.0, &error),
             DeveloperVehicleToolService::SubmitResult::Started);
    QVERIFY(attemptedDuringRoute);
    QVERIFY(!replacementAdmitted);
    QCOMPARE(fixture.frames.size(), 1);
    fixture.commands.observeMessage(
        Fixture::linkId,
        commandAck(fixture.vehicleEndpoint,
                   MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN,
                   MAV_RESULT_ACCEPTED));
    QVERIFY(attemptedDuringRelease);
    QVERIFY(!releaseAdmission);
    QVERIFY(!fixture.tools.busy());

    DeveloperVehicleToolService::Plan successor;
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::ForceAccelCalibrated,
        &successor, &error));
    QVERIFY(successor.planId != plan.planId);
}

void DeveloperVehicleToolServiceTest::
synchronousCompletionAndShutdownAreSafe()
{
    Fixture fixture;
    QVERIFY(fixture.loadParameters({
        {QStringLiteral("GND_ABS_PRESS"), 101325.0,
         ParameterType::Real32}}));
    fixture.onWrite = [&fixture](const mavlink_message_t &message) {
        if (message.msgid != MAVLINK_MSG_ID_PARAM_SET) {
            return;
        }
        fixture.parameters.observePhysicalMessage(
            Fixture::linkId, fixture.linkEpoch,
            parameterValue(fixture.vehicleEndpoint,
                           QStringLiteral("GND_ABS_PRESS"),
                           102500.0, ParameterType::Real32));
    };

    QSignalSpy finished(
        &fixture.tools,
        &DeveloperVehicleToolService::operationFinished);
    DeveloperVehicleToolService::Plan plan;
    QString error;
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::SetQnh, &plan, &error));
    QCOMPARE(fixture.tools.execute(plan, 102500.0, &error),
             DeveloperVehicleToolService::SubmitResult::Started);
    QVERIFY(!fixture.tools.busy());
    QCOMPARE(finished.count(), 1);
    QCOMPARE(fixture.tools.lastReport().outcome,
             DeveloperVehicleToolService::Outcome::Succeeded);

    fixture.onWrite = {};
    QVERIFY(fixture.tools.prepare(
        DeveloperVehicleToolService::Action::RebootVehicle,
        &plan, &error));
    QCOMPARE(fixture.tools.execute(plan, 0.0, &error),
             DeveloperVehicleToolService::SubmitResult::Started);
    QVERIFY(fixture.tools.busy());
    fixture.tools.shutdown();
    QVERIFY(!fixture.tools.busy());
    QVERIFY(!fixture.tools.canPrepare(
        DeveloperVehicleToolService::Action::RebootVehicle, &error));
}

QTEST_GUILESS_MAIN(DeveloperVehicleToolServiceTest)

#include "test_developervehicletoolservice.moc"
