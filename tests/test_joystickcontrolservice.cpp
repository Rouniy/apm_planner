#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "input/JoystickDevice.h"
#include "services/JoystickControlService.h"

#include <QtTest>

#include <functional>
#include <limits>

namespace
{
constexpr int LinkId = 81;
constexpr int SystemId = 42;
constexpr quint8 LocalSystemId = 250;
constexpr quint8 LocalComponentId = MAV_COMP_ID_MISSIONPLANNER;

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned status = MAVLINK_FRAMING_INCOMPLETE;
    for (char byte : bytes)
        status = parser.parseByte(quint8(byte), &message);
    return status == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

mavlink_message_t heartbeat(const VehicleEndpoint &target, bool armed = false)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        quint8(target.systemId), quint8(target.componentId), &message,
        MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        3, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t acknowledgement(
    const SwarmVehicleInstanceLease &vehicle, MAV_CMD command)
{
    mavlink_command_ack_t ack{};
    ack.command = command;
    ack.result = MAV_RESULT_ACCEPTED;
    ack.target_system = LocalSystemId;
    ack.target_component = LocalComponentId;
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(
        quint8(vehicle.endpoint.systemId),
        quint8(vehicle.endpoint.componentId), &message, &ack);
    return message;
}

struct Fixture
{
    struct Sent {
        int linkId = -1;
        mavlink_message_t message{};
    };

    qint64 nowMs = 1000;
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry{[this] { return nowMs; }};
    QVector<Sent> sent;
    std::function<void()> writerHook;
    bool writerResult = true;
    ExactLinkTransmitter transmitter{
        [this](int linkId, const QByteArray &bytes) {
            sent.append({linkId, decodeFrame(bytes)});
            if (writerHook) writerHook();
            return writerResult;
        }};
    VehicleCommandService commands{&targets, &transmitter};
    JoystickDevice device;
    QPointer<JoystickControlService> service;
    VehicleEndpoint endpoint;
    quint64 linkEpoch = 0;
    bool routeAllowed = true;
    std::function<void()> routeHook;

    Fixture()
    {
        endpoint.linkId = LinkId;
        endpoint.systemId = SystemId;
        endpoint.componentId = MAV_COMP_ID_AUTOPILOT1;
        endpoint.linkName = QStringLiteral("Joystick test link");
        endpoint.componentName = QStringLiteral("Autopilot");
        linkEpoch = registry.beginLinkSession(LinkId, endpoint.linkName);
        transmitter.setLinkSessionEpoch(LinkId, linkEpoch);
        QVERIFY(registry.observeMessage(
            LinkId, linkEpoch, heartbeat(endpoint)));
        QVERIFY(targets.observeEndpoint(endpoint, true));
        targets.observeHeartbeat(endpoint, false,
            MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR);
        commands.setLocalIdentity(LocalSystemId, LocalComponentId);
        QVERIFY(commands.configureExactTransactions(
            [this](const SwarmVehicleInstanceLease &lease) {
                return registry.validateLease(lease);
            },
            [this](const SwarmVehicleInstanceLease &lease, QString *error) {
                return route(lease, error);
            }));
        QVERIFY(commands.configureSingleVehicleExactRoute(
            [this](const SwarmVehicleInstanceLease &lease, QString *error) {
                return route(lease, error);
            }));
        service = new JoystickControlService(
            &device, &targets, &registry, &transmitter, &commands,
            LocalSystemId, LocalComponentId,
            [](const SwarmVehicleInstanceLease &,
               QVector<JoystickControlService::ChannelLimits> *limits,
               QString *) {
                limits->fill(JoystickControlService::ChannelLimits(),
                             JoystickConfiguration::ChannelCount);
                (*limits)[0] = {1100, 1900, 1500};
                return true;
            },
            [this](const SwarmVehicleInstanceLease &lease, QString *error) {
                return route(lease, error);
            });
        service->setSendIntervalForTesting(2);
        setInput({0, 0, 0, 0}, QVector<bool>(8, false));
        auto profile = JoystickConfiguration::defaults();
        profile.deviceId = QStringLiteral("test-guid");
        profile.deviceName = QStringLiteral("Test joystick");
        profile.channels[0].axis = QStringLiteral("X");
        profile.channels[1].axis = QStringLiteral("Y");
        profile.channels[2].axis = QStringLiteral("Z");
        profile.channels[3].axis = QStringLiteral("Rz");
        QString error;
        QVERIFY2(service->setConfiguration(profile, &error), qPrintable(error));
    }

    ~Fixture()
    {
        if (service) {
            service->shutdown();
            delete service.data();
        }
    }

    bool route(const SwarmVehicleInstanceLease &lease, QString *error)
    {
        if (routeHook) routeHook();
        const bool allowed = routeAllowed && registry.validateLease(lease);
        if (!allowed && error) *error = QStringLiteral("Injected route refusal");
        return allowed;
    }

    void setInput(std::initializer_list<qint16> axes,
                  const QVector<bool> &buttons,
                  quint64 generation = 11, qint32 instanceId = 7)
    {
        JoystickDevice::Info info;
        info.instanceId = instanceId;
        info.id = QStringLiteral("test-guid");
        info.name = QStringLiteral("Test joystick");
        info.axes = int(axes.size());
        info.buttons = buttons.size();
        JoystickDevice::Snapshot snapshot;
        snapshot.connected = true;
        snapshot.instanceId = instanceId;
        snapshot.generation = generation;
        snapshot.monotonicMs = nowMs;
        for (qint16 axis : axes) {
            snapshot.rawAxes.append(axis);
            snapshot.axes.append(JoystickConfiguration::normalizeAxis(
                axis, JoystickConfiguration::Range()));
        }
        snapshot.buttons = buttons;
        if (service) service->setDeviceStateForTesting(info, snapshot);
    }

    JoystickControlService::EnablePlan prepare()
    {
        JoystickControlService::EnablePlan plan;
        QString error;
        if (!service->prepareEnable(&plan, &error)) qWarning() << error;
        return plan;
    }

    bool enable(const JoystickControlService::EnablePlan &plan)
    {
        QString error;
        const bool result = service->enable(plan, &error);
        if (!result) qWarning() << error;
        return result;
    }

    int count(quint32 messageId) const
    {
        int result = 0;
        for (const Sent &item : sent)
            if (item.message.msgid == messageId) ++result;
        return result;
    }

    QVector<mavlink_command_long_t> commandsSent() const
    {
        QVector<mavlink_command_long_t> result;
        for (const Sent &item : sent) {
            if (item.message.msgid != MAVLINK_MSG_ID_COMMAND_LONG) continue;
            mavlink_command_long_t command{};
            mavlink_msg_command_long_decode(&item.message, &command);
            result.append(command);
        }
        return result;
    }
};
}

class JoystickControlServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void rcOverrideUsesFrozenDeviceVehicleAndLimits()
    {
        Fixture fixture;
        const auto plan = fixture.prepare();
        QVERIFY(plan.isValid());
        QVERIFY(plan.description().contains(QString::number(LinkId)));
        QVERIFY(fixture.enable(plan));
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.count(MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE) >= 1, 250);
        mavlink_rc_channels_override_t payload{};
        for (const auto &sent : fixture.sent) {
            if (sent.message.msgid == MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE) {
                mavlink_msg_rc_channels_override_decode(&sent.message, &payload);
                break;
            }
        }
        QCOMPARE(payload.target_system, quint8(SystemId));
        QCOMPARE(payload.target_component, quint8(MAV_COMP_ID_AUTOPILOT1));
        QCOMPARE(payload.chan1_raw, quint16(1500));
        QCOMPARE(payload.chan5_raw, std::numeric_limits<quint16>::max());
        QCOMPARE(payload.chan9_raw, quint16(0));
        QVERIFY(fixture.service->framesSubmitted() >= 1);
    }

    void manualControlUsesFirstFourMappedChannels()
    {
        Fixture fixture;
        auto profile = fixture.service->configuration();
        profile.manualControl = true;
        QString error;
        QVERIFY(fixture.service->setConfiguration(profile, &error));
        QVERIFY(fixture.enable(fixture.prepare()));
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.count(MAVLINK_MSG_ID_MANUAL_CONTROL) >= 1, 250);
        mavlink_manual_control_t payload{};
        for (const auto &sent : fixture.sent) {
            if (sent.message.msgid == MAVLINK_MSG_ID_MANUAL_CONTROL) {
                mavlink_msg_manual_control_decode(&sent.message, &payload);
                break;
            }
        }
        QCOMPARE(payload.target, quint8(SystemId));
        QCOMPARE(payload.x, qint16(0));
        QCOMPARE(payload.y, qint16(0));
        QCOMPARE(payload.buttons, quint16(0));
    }

    void profileOrDeviceChangeInvalidatesConsent()
    {
        Fixture fixture;
        const auto oldPlan = fixture.prepare();
        QVERIFY(oldPlan.isValid());
        auto profile = fixture.service->configuration();
        profile.channels[0].reverse = true;
        QVERIFY(fixture.service->setConfiguration(profile));
        QVERIFY(!fixture.service->validate(oldPlan));
        fixture.setInput({0, 0, 0, 0}, QVector<bool>(8, false), 12);
        QVERIFY(!fixture.service->enable(oldPlan));
        QCOMPARE(fixture.sent.size(), 0);
    }

    void targetChangeStopsAndReleasesOriginalOnly()
    {
        Fixture fixture;
        auto profile = fixture.service->configuration();
        profile.channels[8].axis = QStringLiteral("X");
        QVERIFY(fixture.service->setConfiguration(profile));
        QVERIFY(fixture.enable(fixture.prepare()));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.service->framesSubmitted() > 0, 250);
        VehicleEndpoint other = fixture.endpoint;
        other.systemId = 43;
        QVERIFY(fixture.targets.observeEndpoint(other));
        QVERIFY(fixture.targets.selectTarget(
            other.linkId, other.systemId, other.componentId));
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.service->isEnabled(), 250);
        int releases = 0;
        for (const auto &sent : fixture.sent) {
            if (sent.message.msgid != MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE)
                continue;
            mavlink_rc_channels_override_t payload{};
            mavlink_msg_rc_channels_override_decode(&sent.message, &payload);
            if (payload.target_system == SystemId
                && payload.chan1_raw == 0
                && payload.chan5_raw == std::numeric_limits<quint16>::max()
                && payload.chan9_raw
                    == std::numeric_limits<quint16>::max() - 1
                && payload.chan18_raw == 0)
                ++releases;
        }
        QCOMPARE(releases, 1);
    }

    void routeRefusalStopsFurtherControl()
    {
        Fixture fixture;
        QVERIFY(fixture.enable(fixture.prepare()));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.service->framesSubmitted() > 0, 250);
        fixture.routeAllowed = false;
        const int before = fixture.sent.size();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.service->isEnabled(), 250);
        QTest::qWait(20);
        QCOMPARE(fixture.sent.size(), before);
    }

    void relayButtonUsesSharedExactCommandService()
    {
        Fixture fixture;
        auto profile = fixture.service->configuration();
        profile.buttons[0].buttonno = 0;
        profile.buttons[0].function = QStringLiteral("Do_Set_Relay");
        profile.buttons[0].p1 = 3;
        QVERIFY(fixture.service->setConfiguration(profile));
        QVERIFY(fixture.enable(fixture.prepare()));
        QVector<bool> buttons(8, false);
        buttons[0] = true;
        fixture.setInput({0, 0, 0, 0}, buttons);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(MAVLINK_MSG_ID_COMMAND_LONG), 1, 250);
        mavlink_command_long_t command{};
        for (const auto &sent : fixture.sent) {
            if (sent.message.msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
                mavlink_msg_command_long_decode(&sent.message, &command);
                break;
            }
        }
        QCOMPARE(command.command, quint16(MAV_CMD_DO_SET_RELAY));
        QCOMPARE(command.param1, 3.0F);
        QCOMPARE(command.param2, 1.0F);
        const auto vehicle = fixture.service->activePlan().vehicle();
        fixture.commands.observePhysicalMessage(
            LinkId, fixture.linkEpoch,
            acknowledgement(vehicle, MAV_CMD_DO_SET_RELAY));
        QTRY_COMPARE_WITH_TIMEOUT(
            fixture.service->buttonActionsSubmitted(), quint64(1), 250);
    }

    void takeoffChangesToGuidedThenSubmitsTakeoff()
    {
        Fixture fixture;
        auto profile = fixture.service->configuration();
        profile.buttons[0].buttonno = 0;
        profile.buttons[0].function = QStringLiteral("TakeOff");
        QVERIFY(fixture.service->setConfiguration(profile));
        QVERIFY(fixture.enable(fixture.prepare()));
        QVector<bool> buttons(8, false);
        buttons[0] = true;
        fixture.setInput({0, 0, 0, 0}, buttons);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.commandsSent().size(), 1, 250);
        auto commands = fixture.commandsSent();
        QCOMPARE(commands.at(0).command, quint16(MAV_CMD_DO_SET_MODE));
        QCOMPARE(commands.at(0).param1,
                 float(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED));
        QCOMPARE(commands.at(0).param2, 4.0F);

        const auto vehicle = fixture.service->activePlan().vehicle();
        fixture.commands.observePhysicalMessage(
            LinkId, fixture.linkEpoch,
            acknowledgement(vehicle, MAV_CMD_DO_SET_MODE));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.commandsSent().size(), 2, 250);
        commands = fixture.commandsSent();
        QCOMPARE(commands.at(1).command, quint16(MAV_CMD_NAV_TAKEOFF));
        QCOMPARE(commands.at(1).param7, 2.0F);
        fixture.commands.observePhysicalMessage(
            LinkId, fixture.linkEpoch,
            acknowledgement(vehicle, MAV_CMD_NAV_TAKEOFF));
    }

    void oldGuidedAckAfterDisableReenableCannotLaunchTakeoff()
    {
        Fixture fixture;
        auto profile = fixture.service->configuration();
        profile.buttons[0].buttonno = 0;
        profile.buttons[0].function = QStringLiteral("TakeOff");
        QVERIFY(fixture.service->setConfiguration(profile));
        QVERIFY(fixture.enable(fixture.prepare()));
        QVector<bool> buttons(8, false);
        buttons[0] = true;
        fixture.setInput({0, 0, 0, 0}, buttons);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.commandsSent().size(), 1, 250);
        const auto oldVehicle = fixture.service->activePlan().vehicle();

        QVERIFY(fixture.service->disable());
        QVERIFY(fixture.enable(fixture.prepare()));
        fixture.commands.observePhysicalMessage(
            LinkId, fixture.linkEpoch,
            acknowledgement(oldVehicle, MAV_CMD_DO_SET_MODE));
        QTest::qWait(30);
        QCOMPARE(fixture.commandsSent().size(), 1);
        QVERIFY(fixture.service->isEnabled());
    }

    void deviceLossStopsAndAttemptsOneRelease()
    {
        Fixture fixture;
        QVERIFY(fixture.enable(fixture.prepare()));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.service->framesSubmitted() > 0, 250);
        fixture.setInput({0, 0, 0, 0}, QVector<bool>(8, false), 12);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.service->isEnabled(), 250);
        int releases = 0;
        for (const auto &sent : fixture.sent) {
            if (sent.message.msgid != MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE)
                continue;
            mavlink_rc_channels_override_t payload{};
            mavlink_msg_rc_channels_override_decode(&sent.message, &payload);
            if (payload.chan1_raw == 0 && payload.chan18_raw == 0) ++releases;
        }
        QCOMPARE(releases, 1);
    }
};

QTEST_GUILESS_MAIN(JoystickControlServiceTest)
#include "test_joystickcontrolservice.moc"
