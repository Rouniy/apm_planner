#include "DeveloperVehicleToolRuntimeAudit.h"
#include "comm/LinkManager.h"
#include "comm/LinkManagerFactory.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/TCPLink.h"
#include "comm/VehicleTargetManager.h"
#include "services/DeveloperVehicleToolService.h"
#include "ui/MainWindow.h"
#include "ui/configuration/ConfigDeveloperToolsView.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <functional>
#include <cmath>
#include <cstring>

namespace {
constexpr int FixtureLinkId = 910110;
constexpr quint8 FixtureSystem = 234;
bool waitFor(const std::function<bool()> &condition, int timeout = 2500)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return condition();
}

// No sockets are opened: this exercises the production physical-link path,
// command/parameter services and actual Tools navigation against one simulator.
class DeveloperAuditLink final : public TCPLink {
public:
    DeveloperAuditLink() : TCPLink(QHostAddress::LocalHost,
        QStringLiteral("Developer action audit"), 61981, false) {}
    int getId() const override { return FixtureLinkId; }
    bool isConnected() const override { return m_connected; }
    bool connect() override {
        if (!m_connected) {
            m_connected = true;
            emit connected(); emit connected(this); emit connected(true);
        }
        return true;
    }
    bool disconnect() override {
        if (m_connected) {
            m_connected = false;
            emit disconnected(); emit disconnected(this); emit connected(false);
        }
        return true;
    }
    void inject(mavlink_message_t message) {
        uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
        const int size = mavlink_msg_to_send_buffer(buffer, &message);
        emit bytesReceived(this, QByteArray(reinterpret_cast<const char *>(buffer), size));
    }
    void heartbeat(bool armed = false) {
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(FixtureSystem, 1, &message, MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA, armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
            0, MAV_STATE_STANDBY);
        inject(message);
    }
    void pressureReply() {
        mavlink_param_value_t value{};
        std::memcpy(value.param_id, "GND_ABS_PRESS", 13);
        value.param_value = pressure;
        value.param_type = MAV_PARAM_TYPE_REAL32;
        value.param_count = 1;
        value.param_index = 0;
        mavlink_message_t message{};
        mavlink_msg_param_value_encode(FixtureSystem, 1, &message, &value);
        inject(message);
    }
    void writeBytes(const char *bytes, qint64 size) override {
        mavlink_message_t message{};
        for (qint64 i = 0; i < size; ++i) {
            if (parser.parseByte(quint8(bytes[i]), &message) != MAVLINK_FRAMING_OK) continue;
            if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_LIST
                || message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) pressureReply();
            if (message.msgid == MAVLINK_MSG_ID_PARAM_SET) {
                mavlink_param_set_t value{};
                mavlink_msg_param_set_decode(&message, &value);
                if (value.target_system != FixtureSystem || value.target_component != 1) continue;
                if (std::memcmp(value.param_id, "GND_ABS_PRESS", 13) != 0) continue;
                ++parameterWrites;
                pressure = value.param_value;
                pressureReply();
            }
            if (message.msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
                mavlink_command_long_t command{};
                mavlink_msg_command_long_decode(&message, &command);
                if (command.target_system != FixtureSystem || command.target_component != 1) continue;
                if (command.command == MAV_CMD_PREFLIGHT_CALIBRATION
                    || command.command == MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN)
                    commands.append(command);
                mavlink_command_ack_t ack{};
                ack.command = command.command;
                ack.result = MAV_RESULT_ACCEPTED;
                ack.target_system = message.sysid;
                ack.target_component = message.compid;
                mavlink_message_t reply{};
                mavlink_msg_command_ack_encode(FixtureSystem, 1, &reply, &ack);
                inject(reply);
            }
        }
    }
    int parameterWrites = 0;
    float pressure = 101325.0f;
    QVector<mavlink_command_long_t> commands;
private:
    MAVLinkFrameParser parser;
    bool m_connected = false;
};
}

int RunDeveloperVehicleToolRuntimeAudit()
{
    int failures = 0;
    const auto expect = [&](bool condition, const char *description) {
        if (!condition) { ++failures; qCritical() << "Developer runtime:" << description; }
    };
    auto *links = LinkManager::instance();
    auto *window = MainWindow::instance();
    auto *service = links->developerVehicleToolService();
    auto *action = window->findChild<QAction *>(QStringLiteral("actionDeveloperTools"));
    expect(service && action, "application service or Tools action missing");
    if (!service || !action) return 1;
    action->trigger();
    QCoreApplication::processEvents();
    QPointer<ConfigDeveloperToolsView> page(window->findChild<ConfigDeveloperToolsView *>());
    expect(page && page->ImplementedActionCount() == 11 && page->ActionCount() == 32,
           "production Developer route did not bind six vehicle tools");
    if (!page) return 1;
    auto *reboot = page->findChild<QPushButton *>(QStringLiteral("RebootVehicleButton"));
    expect(reboot && !reboot->isEnabled(), "offline reboot was enabled");

    QPointer<DeveloperAuditLink> fixture(new DeveloperAuditLink);
    LinkManager::ConnectionProfile profile;
    profile.id = QStringLiteral("f38d827e-5849-42b4-83b0-49c1d158ab37");
    LinkManagerFactory::connectLinkSignals(fixture, links);
    links->addLink(fixture, profile);
    expect(links->connectLink(FixtureLinkId), "fixture connect failed");
    fixture->heartbeat();
    expect(waitFor([&] { return links->vehicleTargetManager()->contains(FixtureLinkId, FixtureSystem, 1); }),
           "heartbeat did not discover exact fixture");
    links->vehicleTargetManager()->selectTarget(FixtureLinkId, FixtureSystem, 1);
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, fixture, [fixture] { if (fixture) fixture->heartbeat(); });
    heartbeat.start(250);
    fixture->pressureReply();
    expect(waitFor([&] { return service->canPrepare(DeveloperVehicleToolService::Action::SetQnh); }),
           "exact pressure snapshot did not become ready");
    // A complete list is readable before the classic PARAM traffic isolation
    // window expires. Wait for actual lane admission without weakening that
    // production fence or transmitting a parameter operation.
    QObject admissionProbe;
    expect(waitFor([&] {
        DeveloperVehicleToolService::Plan plan;
        if (!service->prepare(DeveloperVehicleToolService::Action::SetQnh, &plan)) return false;
        ParameterService::ExactReservationToken reservation;
        auto *parameters = links->parameterService();
        if (parameters->reserveSingleVehicleEndpoint(&admissionProbe, plan.target, plan.vehicle,
                &reservation) != ParameterService::ExactReservationResult::Reserved) return false;
        return parameters->releaseExactReservation(reservation);
    }, ParameterService::DefaultExactWriteQuarantineMs + 2500),
           "initial parameter traffic did not drain");
    action->trigger();
    QCoreApplication::processEvents();
    page = window->findChild<ConfigDeveloperToolsView *>();
    const QStringList names = {QStringLiteral("SetQnhButton"), QStringLiteral("AdjustBarometerAltitudeButton"),
        QStringLiteral("ForceAccelCalibratedButton"), QStringLiteral("ForceCompassCalibratedButton"),
        QStringLiteral("RebootVehicleButton"), QStringLiteral("RebootToDfuButton")};
    for (int i = 0; i < names.size() && page && fixture; ++i) {
        auto *button = page->findChild<QPushButton *>(names[i]);
        expect(waitFor([&] { return button && button->isEnabled(); }), "connected action disabled");
        if (!button || !button->isEnabled()) continue;
        const int before = fixture->commands.size() + fixture->parameterWrites;
        const auto openConsent = [&]() -> QMessageBox * {
            button->click();
            QCoreApplication::processEvents();
            if (i < 2) {
                auto *input = page->findChild<QInputDialog *>(QStringLiteral("DeveloperVehicleValueDialog"));
                if (!input) return nullptr;
                input->setDoubleValue(i == 0 ? 101500.0 : 2.0);
                input->accept();
                QCoreApplication::processEvents();
            }
            return page->findChild<QMessageBox *>(QStringLiteral("DeveloperVehicleConfirmation"));
        };
        auto *confirmation = openConsent();
        expect(confirmation && confirmation->defaultButton() == confirmation->button(QMessageBox::Cancel),
               "named default-Cancel confirmation missing");
        if (!confirmation) continue;
        confirmation->button(QMessageBox::Cancel)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(fixture->commands.size() + fixture->parameterWrites == before, "Cancel sent a vehicle change");
        confirmation = openConsent();
        expect(confirmation != nullptr, "second confirmation missing");
        if (!confirmation) continue;
        const auto priorOperation = service->lastReport().operationId;
        confirmation->button(QMessageBox::Yes)->click();
        expect(waitFor([&] { return !service->busy() && service->lastReport().operationId != priorOperation; }),
               "exact terminal result did not reach application service");
        qInfo() << "Developer runtime action:" << names[i] << service->lastReport().description;
        expect(service->lastReport().outcome == DeveloperVehicleToolService::Outcome::Succeeded,
               "matching exact acknowledgement did not succeed");
        expect(fixture->commands.size() + fixture->parameterWrites == before + 1, "action did not send exactly one expected change");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    if (fixture) {
        expect(fixture->parameterWrites == 2 && std::abs(fixture->pressure - 101522.2f) < 0.02f,
               "pressure/11.1 Pa per metre mapping changed");
        expect(fixture->commands.size() == 4, "command action inventory mismatch");
        if (fixture->commands.size() == 4) {
            expect(fixture->commands[0].param5 == 76 && fixture->commands[1].param2 == 76,
                   "force calibration sentinel mismatch");
            expect(fixture->commands[2].param1 == 1, "ordinary reboot wire mismatch");
            const auto dfu = fixture->commands[3];
            expect(dfu.param1 == 42 && dfu.param2 == 24 && dfu.param3 == 71 && dfu.param4 == 99,
                   "DFU was confused with hold-in-bootloader");
        }
    }
    heartbeat.stop();
    links->removeLink(FixtureLinkId);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(!fixture && !service->busy(), "fixture cleanup left active transport/operation");
    qInfo() << "Developer vehicle runtime audit failures:" << failures;
    return failures ? 1 : 0;
}
