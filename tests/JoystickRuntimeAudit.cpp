#include "JoystickRuntimeAudit.h"
#include "comm/LinkManager.h"
#include "comm/LinkManagerFactory.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/TCPLink.h"
#include "comm/VehicleTargetManager.h"
#include "input/JoystickDevice.h"
#include "services/JoystickControlService.h"
#include "ui/MainWindow.h"
#include "ui/configuration/SetupView.h"
#include "ui/configuration/ConfigJoystickView.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QDebug>
#include <SDL.h>
#include <functional>

namespace {
constexpr int LinkId = 910126;
constexpr quint8 SystemId = 234;
bool waitFor(const std::function<bool()> &ready, int limit = 5000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < limit) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}
template<class T> T *find(QObject *root, const char *name) {
    return root ? root->findChild<T *>(QString::fromLatin1(name)) : nullptr;
}
template<class T> T *visible(QObject *root, const char *name) {
    if (root) for (T *child : root->findChildren<T *>(QString::fromLatin1(name)))
        if (child->isVisible()) return child;
    return nullptr;
}
class AuditLink final : public TCPLink {
public:
    AuditLink() : TCPLink(QHostAddress::LocalHost, "Joystick local fixture", 61986, false) {}
    int getId() const override { return LinkId; }
    bool isConnected() const override { return online; }
    bool connect() override { online = true; emit connected(); emit connected(this); emit connected(true); return true; }
    bool disconnect() override { online = false; emit disconnected(); emit disconnected(this); emit connected(false); return true; }
    void inject(const mavlink_message_t &message) {
        uint8_t wire[MAVLINK_MAX_PACKET_LEN]{};
        const int size = mavlink_msg_to_send_buffer(wire, &message);
        emit bytesReceived(this, QByteArray(reinterpret_cast<const char *>(wire), size));
    }
    void heartbeat() {
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(SystemId, 1, &message, MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_STANDBY);
        inject(message);
    }
    void writeBytes(const char *bytes, qint64 size) override {
        mavlink_message_t message{};
        for (qint64 i = 0; i < size; ++i) {
            if (parser.parseByte(quint8(bytes[i]), &message) != MAVLINK_FRAMING_OK) continue;
            if (message.msgid == MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE) {
                mavlink_rc_channels_override_t value{};
                mavlink_msg_rc_channels_override_decode(&message, &value);
                rc.append(value);
            } else if (message.msgid == MAVLINK_MSG_ID_MANUAL_CONTROL) {
                mavlink_manual_control_t value{};
                mavlink_msg_manual_control_decode(&message, &value);
                manual.append(value);
            } else if (message.msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
                mavlink_command_long_t command{};
                mavlink_msg_command_long_decode(&message, &command);
                mavlink_command_ack_t ack{}; ack.command = command.command;
                ack.result = MAV_RESULT_UNSUPPORTED;
                ack.target_system = message.sysid; ack.target_component = message.compid;
                mavlink_message_t response{};
                mavlink_msg_command_ack_encode(SystemId, 1, &response, &ack);
                inject(response);
            }
        }
    }
    QVector<mavlink_rc_channels_override_t> rc;
    QVector<mavlink_manual_control_t> manual;
private:
    bool online = false;
    MAVLinkFrameParser parser;
};
}

int RunJoystickRuntimeAudit()
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    int failures = 0;
    const auto check = [&](bool good, const char *message) {
        if (!good) { ++failures; qCritical() << "Joystick runtime:" << message; }
    };
    auto *main = MainWindow::instance();
    auto *device = find<JoystickDevice>(main, "JoystickDevice");
    auto *service = find<JoystickControlService>(main, "JoystickControlService");
    auto *setup = main->findChild<SetupView *>();
    check(device && service && setup, "application-owned joystick services missing");
    if (!device || !service || !setup) return 1;
    main->loadHardwareConfigView();
    check(setup->showJoystick(), "SETUP Joystick is not selectable offline");
    main->show();
    QPointer<ConfigJoystickView> page = find<ConfigJoystickView>(setup, "ConfigJoystickView");
    check(page && waitFor([&] { return page->isVisible(); }), "real Joystick page not visible");
    if (!page) return 1;
    const int virtualIndex = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 8, 1);
    check(virtualIndex >= 0, "cannot attach SDL virtual joystick");
    if (virtualIndex < 0) return 1;
    SDL_Joystick *virtualDevice = SDL_JoystickOpen(virtualIndex);
    check(virtualDevice, "cannot open SDL virtual joystick");
    if (!virtualDevice) { SDL_JoystickDetachVirtual(virtualIndex); return 1; }
    const auto instance = SDL_JoystickGetDeviceInstanceID(virtualIndex);
    device->refresh();
    QString error;
    auto *deviceCombo = find<QComboBox>(page, "JoystickDevice");
    const int choice = deviceCombo ? deviceCombo->findData(instance) : -1;
    check(choice >= 0, "virtual joystick missing from device selector");
    if (choice >= 0) {
        deviceCombo->setCurrentIndex(-1);
        deviceCombo->setCurrentIndex(choice);
    }
    check(device->selectedDevice().instanceId == instance, "UI device selection did not open controller");
    SDL_JoystickSetVirtualAxis(virtualDevice, 0, 32767);
    SDL_JoystickSetVirtualAxis(virtualDevice, 1, -32768);
    device->poll();
    check(device->snapshot().connected && device->snapshot().rawAxes.value(0) == 32767,
          "SDL axes do not reach live input preview");
    SDL_JoystickSetVirtualAxis(virtualDevice, 0, 0);
    device->poll();
    auto *detect = find<QPushButton>(page, "JoystickDetectAxis_1");
    check(detect && detect->isEnabled(), "Axis Auto Detect unavailable");
    if (detect) detect->click();
    SDL_JoystickSetVirtualAxis(virtualDevice, 0, 32767);
    device->poll();
    check(page->profile().channels.value(0).axis == QStringLiteral("X")
          && !page->detectionActive(), "Auto Detect did not select the moved SDL X axis");
    auto *links = LinkManager::instance();
    auto *link = new AuditLink;
    LinkManager::ConnectionProfile connection;
    connection.id = QStringLiteral("7ad7e7a0-c1c8-4b20-aa9d-73912a34ad78");
    LinkManagerFactory::connectLinkSignals(link, links);
    links->addLink(link, connection);
    check(links->connectLink(LinkId), "private fixture link did not connect");
    link->heartbeat();
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, link, [link] { link->heartbeat(); });
    heartbeat.start(200);
    check(waitFor([&] { return links->vehicleTargetManager()->selectTarget(LinkId, SystemId, 1); }),
          "fixture vehicle was not discovered");
    setup->showJoystick();
    page = find<ConfigJoystickView>(setup, "ConfigJoystickView");
    QApplication::processEvents();
    for (const char *name : {"JoystickAxisTable", "JoystickButtonTable"}) {
        auto *table = find<QTableWidget>(page, name);
        check(table && table->rowCount() >= 16
              && !table->visualRect(table->model()->index(0, 0)).isEmpty(),
              "Joystick table rows exist in model but are not displayed");
    }
    const QStringList axes{"X", "Y", "Z", "Rx"};
    for (int i = 0; i < 4; ++i) {
        auto *axis = page ? page->findChild<QComboBox *>(QStringLiteral("JoystickAxis_%1").arg(i + 1)) : nullptr;
        check(axis && axis->findText(axes[i]) >= 0, "RC axis editor missing");
        if (axis) axis->setCurrentText(axes[i]);
    }
    auto *save = find<QPushButton>(page, "JoystickSaveButton");
    check(save && save->isEnabled(), "Save profile is unavailable");
    if (save) save->click();
    QSettings settings;
    JoystickConfiguration::Profile saved;
    check(JoystickConfiguration::load(&settings, &saved, &error)
          && saved.deviceId == device->selectedDevice().id
          && saved.channels.value(0).axis == QStringLiteral("X"),
          "UI Save did not persist selected device and mappings");
    auto *enable = find<QPushButton>(page, "JoystickEnableButton");
    check(enable && enable->isEnabled(), "Joystick Enable button unavailable");
    if (enable) enable->click();
    auto *consent = visible<QMessageBox>(page, "JoystickEnableConfirmation");
    check(consent && consent->isVisible(), "Joystick exact-target consent did not open");
    if (consent) {
        check(consent->defaultButton() == consent->button(QMessageBox::Cancel), "Enable consent not default Cancel");
        consent->button(QMessageBox::Cancel)->click();
    }
    check(!service->isEnabled() && link->rc.isEmpty(), "Cancelled Enable transmitted RC");
    if (enable) enable->click();
    consent = visible<QMessageBox>(page, "JoystickEnableConfirmation");
    if (consent) consent->button(QMessageBox::Yes)->click();
    check(service->isEnabled(), qPrintable(service->statusText()));
    check(waitFor([&] { return link->rc.size() >= 3; }), "20Hz RC override output missing");
    if (!link->rc.isEmpty()) {
        const auto packet = link->rc.last();
        check(packet.target_system == SystemId && packet.target_component == 1,
              "RC output addressed a different vehicle");
        check(packet.chan1_raw == 2000 && packet.chan2_raw == 1000,
              "axis endpoints do not map to RC1000..2000");
    }
    // Navigating away must not stop the app-owned controller.
    main->loadPilotView();
    check(service->isEnabled(), "leaving SETUP stopped joystick control");
    main->loadHardwareConfigView(); setup->showJoystick();
    page = find<ConfigJoystickView>(setup, "ConfigJoystickView");
    enable = find<QPushButton>(page, "JoystickEnableButton");
    const QString screenshot = qEnvironmentVariable("APM_JOYSTICK_AUDIT_SCREENSHOT");
    if (!screenshot.isEmpty()) check(page && page->grab().save(screenshot), "Joystick screenshot failed");
    if (enable) enable->click();
    check(!service->isEnabled(), "Disable did not stop the controller");
    check(!link->rc.isEmpty() && link->rc.last().chan1_raw == 0,
          "Disable did not release owned RC override");
    auto *manual = find<QCheckBox>(page, "JoystickManualControl");
    check(manual && manual->isEnabled(), "Manual Control selector unavailable after Disable");
    if (manual) manual->setChecked(true);
    if (enable) enable->click();
    consent = visible<QMessageBox>(page, "JoystickEnableConfirmation");
    if (consent) consent->button(QMessageBox::Yes)->click();
    check(waitFor([&] { return link->manual.size() >= 3; }), "Manual Control output missing");
    if (!link->manual.isEmpty()) {
        const auto packet = link->manual.last();
        check(packet.target == SystemId && packet.x == 1000 && packet.y == -1000,
              "Manual Control target/axis endpoints incorrect");
    }
    if (enable) enable->click();
    check(!service->isEnabled(), "Manual Control Disable did not stop output");
    heartbeat.stop();
    links->disconnectLink(LinkId);
    device->clearSelection();
    SDL_JoystickClose(virtualDevice);
    SDL_JoystickDetachVirtual(virtualIndex);
    main->close();
    check(!main->isVisible(), "MainWindow did not close");
    qInfo() << "Joystick runtime audit failures:" << failures
            << "(SDL virtual joystick and in-process vehicle only)";
    return failures ? 1 : 0;
#else
    qCritical() << "Joystick virtual-device audit requires SDL 2.0.14 or later.";
    return 2;
#endif
}
