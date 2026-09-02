#include <QtTest>

#include "comm/Esp8266ParameterClient.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleTargetManager.h"
#include "ui/configuration/ConfigHWESP8266View.h"
#include "ui/configuration/ConfigHWESP8266ViewModel.h"
#include "ui/configuration/Esp8266Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <cstring>

namespace {

VehicleEndpoint endpoint(int linkId, int systemId = 42, int componentId = MAV_COMP_ID_AUTOPILOT1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    return result;
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    if (state != MAVLINK_FRAMING_OK) {
        std::memset(&message, 0, sizeof(message));
    }
    return message;
}

QString parameterId(const char id[16])
{
    int length = 0;
    while (length < 16 && id[length] != '\0') {
        ++length;
    }
    return QString::fromLatin1(id, length);
}

mavlink_message_t parameterValue(const QString &name, const QByteArray &raw4)
{
    mavlink_param_value_t payload;
    std::memset(&payload, 0, sizeof(payload));
    const QByteArray id = name.toLatin1().left(16);
    std::memcpy(payload.param_id, id.constData(), static_cast<size_t>(id.size()));
    QByteArray raw = raw4.left(4);
    if (raw.size() < 4) {
        raw.append(QByteArray(4 - raw.size(), '\0'));
    }
    const quint32 bits = Esp8266SettingsCodec::UInt32FromRaw(raw);
    std::memcpy(&payload.param_value, &bits, sizeof(payload.param_value));
    payload.param_type = MAV_PARAM_TYPE_UINT32;
    payload.param_count = 64;
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_param_value_encode(42, MAV_COMP_ID_UDP_BRIDGE, &message, &payload);
    return message;
}

mavlink_message_t commandAck(MAV_CMD command, MAV_RESULT result = MAV_RESULT_ACCEPTED)
{
    mavlink_command_ack_t payload;
    std::memset(&payload, 0, sizeof(payload));
    payload.command = static_cast<quint16>(command);
    payload.result = static_cast<quint8>(result);
    payload.target_system = 250; // the client's local identity
    payload.target_component = 190;
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_command_ack_encode(42, MAV_COMP_ID_UDP_BRIDGE, &message, &payload);
    return message;
}

// The complete MP10 fixture as the bridge would report it.
Esp8266SettingsCodec::RawValues completeResponse()
{
    Esp8266SettingsCodec::RawValues values;
    auto packed = [&values](const QString &prefix, QByteArray text) {
        text = text.left(16);
        text.append(QByteArray(16 - text.size(), '\0'));
        for (int index = 0; index < 4; ++index) {
            values.insert(prefix + QString::number(index + 1), text.mid(index * 4, 4));
        }
    };
    auto number = [&values](const QString &name, quint32 value) {
        values.insert(name, Esp8266SettingsCodec::RawFromUInt32(value));
    };
    auto ip = [&values](const QString &name, const char *text) {
        QByteArray raw;
        Esp8266SettingsCodec::ParseIPv4(QString::fromLatin1(text), &raw);
        values.insert(name, raw);
    };
    packed(QStringLiteral("WIFI_SSID"), QByteArrayLiteral("FieldNetwork"));
    packed(QStringLiteral("WIFI_PASSWORD"), QByteArrayLiteral("secret-password"));
    number(QStringLiteral("UART_BAUDRATE"), 115200);
    number(QStringLiteral("WIFI_CHANNEL"), 11);
    number(QStringLiteral("DEBUG_ENABLED"), 0);
    number(QStringLiteral("WIFI_MODE"), 1);
    ip(QStringLiteral("WIFI_IPADDRESS"), "192.168.4.1");
    number(QStringLiteral("WIFI_UDP_HPORT"), 14550);
    number(QStringLiteral("WIFI_UDP_CPORT"), 14555);
    ip(QStringLiteral("WIFI_IPSTA"), "10.42.0.20");
    ip(QStringLiteral("WIFI_GATEWAYSTA"), "10.42.0.1");
    ip(QStringLiteral("WIFI_SUBNET_STA"), "255.255.255.0");
    return values;
}

struct Fixture
{
    VehicleTargetManager targets;
    QVector<QByteArray> frames;
    bool writerAccepts = true;
    ExactLinkTransmitter transmitter;
    Esp8266ParameterClient client;
    ConfigHWESP8266View view;

    Fixture()
        : transmitter([this](int, const QByteArray &bytes) {
              if (!writerAccepts) {
                  return false;
              }
              frames.append(bytes);
              return true;
          }),
          client(&targets, &transmitter),
          view(&client)
    {
        client.setLocalIdentity(250, 190);
        client.setTimingForTesting(30, 20, 1);
        view.resize(800, 640);
    }

    void connectVehicle(int linkId = 9)
    {
        targets.observeEndpoint(endpoint(linkId));
        targets.selectTarget(linkId, 42, MAV_COMP_ID_AUTOPILOT1);
        client.bind(targets.acquireTarget());
        view.setConnected(true);
    }

    void deliverAll(int linkId = 9)
    {
        const Esp8266SettingsCodec::RawValues values = completeResponse();
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            client.observeMessage(linkId, parameterValue(it.key(), it.value()));
        }
    }

    // Echo the PARAM_SET frame at `index` back as the bridge would.
    void echoWrite(int index, int linkId = 9)
    {
        const mavlink_message_t message = decodeFrame(frames.at(index));
        mavlink_param_set_t set;
        std::memset(&set, 0, sizeof(set));
        mavlink_msg_param_set_decode(&message, &set);
        quint32 bits = 0;
        std::memcpy(&bits, &set.param_value, sizeof(set.param_value));
        const QByteArray raw = Esp8266SettingsCodec::RawFromUInt32(bits);
        client.observeMessage(linkId, parameterValue(parameterId(set.param_id), raw));
    }

    mavlink_message_t message(int index) const { return decodeFrame(frames.at(index)); }
    QString status() const { return view.viewModel()->status(); }
    QLabel *statusLabel() const { return view.findChild<QLabel *>(QStringLiteral("hwEspStatus")); }
    QLineEdit *edit(const QString &name) const { return view.findChild<QLineEdit *>(name); }
    QComboBox *combo(const QString &name) const { return view.findChild<QComboBox *>(name); }
    QPushButton *button(const QString &name) const { return view.findChild<QPushButton *>(name); }
    QCheckBox *staMode() const { return view.findChild<QCheckBox *>(QStringLiteral("hwEspStaMode")); }
    QWidget *stationGrid() const { return view.findChild<QWidget *>(QStringLiteral("hwEspStationGrid")); }
    bool controlsEnabled() const
    {
        return edit(QStringLiteral("hwEspSsid"))->isEnabled()
               && combo(QStringLiteral("hwEspChannel"))->isEnabled()
               && staMode()->isEnabled()
               && button(QStringLiteral("hwEspSave"))->isEnabled()
               && button(QStringLiteral("hwEspResetDefaults"))->isEnabled();
    }
};

} // namespace

class ConfigHWESP8266ViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsMatchMissionPlanner();
    void activateLoadsAndAppliesTheCurrentGenerationOnly();
    void loadFailuresReportMissingParameters();
    void saveValidatesThenWritesTheExactList();
    void saveFailuresAndOfflineStatusesAreTruthful();
    void resetInvokesEraseSaveThenRefreshes();
    void busyGatingDeactivationAndDisconnect();
    void destructionDuringAnOperationIsSafe();
};

void ConfigHWESP8266ViewTest::defaultsMatchMissionPlanner()
{
    Fixture fixture;
    ConfigHWESP8266View &view = fixture.view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigHWESP8266View"));
    QVERIFY(view.client() == &fixture.client);

    auto *title = view.findChild<QLabel *>(QStringLiteral("hwEspTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("ESP8266"));
    QCOMPARE(fixture.statusLabel()->text(), QStringLiteral("Not connected."));
    QCOMPARE(fixture.status(), QStringLiteral("Not connected."));

    const QStringList labelNames{QStringLiteral("hwEspSsidLabel"), QStringLiteral("hwEspPasswordLabel"),
                                 QStringLiteral("hwEspChannelLabel"), QStringLiteral("hwEspBaudLabel"),
                                 QStringLiteral("hwEspIpLabel"), QStringLiteral("hwEspGatewayLabel"),
                                 QStringLiteral("hwEspSubnetLabel")};
    const QStringList labelTexts{QStringLiteral("SSID"), QStringLiteral("Password"),
                                 QStringLiteral("WiFi Channel"), QStringLiteral("UART Baud"),
                                 QStringLiteral("IP"), QStringLiteral("Gateway"), QStringLiteral("Subnet")};
    for (int index = 0; index < labelNames.size(); ++index) {
        auto *label = view.findChild<QLabel *>(labelNames.at(index));
        QVERIFY2(label, qPrintable(labelNames.at(index)));
        QCOMPARE(label->text(), labelTexts.at(index));
        QCOMPARE(label->minimumWidth(), 140);
    }

    QVERIFY(fixture.edit(QStringLiteral("hwEspSsid"))->text().isEmpty());
    QVERIFY(fixture.edit(QStringLiteral("hwEspPassword"))->text().isEmpty());
    QCOMPARE(fixture.edit(QStringLiteral("hwEspPassword"))->echoMode(), QLineEdit::Normal);
    QCOMPARE(fixture.combo(QStringLiteral("hwEspChannel"))->count(), 13);
    QCOMPARE(fixture.combo(QStringLiteral("hwEspChannel"))->currentText(), QStringLiteral("11"));
    QCOMPARE(fixture.combo(QStringLiteral("hwEspChannel"))->minimumWidth(), 120);
    QCOMPARE(fixture.combo(QStringLiteral("hwEspBaud"))->count(), 8);
    QCOMPARE(fixture.combo(QStringLiteral("hwEspBaud"))->currentText(), QStringLiteral("115200"));
    QCOMPARE(fixture.combo(QStringLiteral("hwEspBaud"))->minimumWidth(), 120);
    QCOMPARE(fixture.staMode()->text(), QStringLiteral("Station (STA) mode"));
    QVERIFY(!fixture.staMode()->isChecked());
    QVERIFY(!fixture.stationGrid()->isEnabled()); // follows the checkbox
    QCOMPARE(fixture.edit(QStringLiteral("hwEspIpSta"))->text(), QStringLiteral("192.168.4.1"));
    QCOMPARE(fixture.edit(QStringLiteral("hwEspGatewaySta"))->text(), QStringLiteral("192.168.4.1"));
    QCOMPARE(fixture.edit(QStringLiteral("hwEspSubnetSta"))->text(), QStringLiteral("255.255.255.0"));
    QCOMPARE(fixture.button(QStringLiteral("hwEspSave"))->text(), QStringLiteral("Save"));
    QCOMPARE(fixture.button(QStringLiteral("hwEspResetDefaults"))->text(), QStringLiteral("Reset to defaults"));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("hwEspDetails"))->text().isEmpty());
    QVERIFY(fixture.controlsEnabled());

    fixture.staMode()->setChecked(true);
    QVERIFY(fixture.stationGrid()->isEnabled());
    QVERIFY(view.viewModel()->staMode());
    fixture.staMode()->setChecked(false);
    QVERIFY(!fixture.stationGrid()->isEnabled());

    // Typing flows into the model; the model's save request mirrors the page.
    fixture.edit(QStringLiteral("hwEspSsid"))->setText(QStringLiteral("Net"));
    fixture.edit(QStringLiteral("hwEspPassword"))->setText(QStringLiteral("pw"));
    fixture.combo(QStringLiteral("hwEspChannel"))->setCurrentText(QStringLiteral("6"));
    const Esp8266SaveRequest request = view.viewModel()->saveRequest();
    QCOMPARE(request.ssid, QStringLiteral("Net"));
    QCOMPARE(request.password, QStringLiteral("pw"));
    QCOMPARE(request.channel, QStringLiteral("6"));
    QCOMPARE(request.baud, QStringLiteral("115200"));
    QCOMPARE(fixture.frames.size(), 0); // nothing is sent before activation
}

void ConfigHWESP8266ViewTest::activateLoadsAndAppliesTheCurrentGenerationOnly()
{
    Fixture fixture;
    ConfigHWESP8266View &view = fixture.view;

    // Not connected: MP10 Activate() only reports "Not connected.".
    view.activate();
    QCOMPARE(fixture.status(), QStringLiteral("Not connected."));
    QCOMPARE(fixture.frames.size(), 0);

    fixture.connectVehicle();
    view.activate();
    QCOMPARE(fixture.status(), QStringLiteral("Requesting ESP8266 parameters…"));
    QVERIFY(view.viewModel()->isBusy());
    QVERIFY(!fixture.controlsEnabled());
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(fixture.message(0).msgid, quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));

    // Values from another link are not ours.
    fixture.deliverAll(4);
    QVERIFY(view.viewModel()->isBusy());
    QVERIFY(!view.viewModel()->isLoaded());

    fixture.deliverAll(9);
    QVERIFY(!view.viewModel()->isBusy());
    QVERIFY(view.viewModel()->isLoaded());
    QVERIFY(fixture.status().isEmpty());
    QVERIFY(fixture.controlsEnabled());
    QCOMPARE(fixture.edit(QStringLiteral("hwEspSsid"))->text(), QStringLiteral("FieldNetwork"));
    QCOMPARE(fixture.edit(QStringLiteral("hwEspPassword"))->text(), QStringLiteral("secret-password"));
    QCOMPARE(fixture.combo(QStringLiteral("hwEspChannel"))->currentText(), QStringLiteral("11"));
    QCOMPARE(fixture.combo(QStringLiteral("hwEspBaud"))->currentText(), QStringLiteral("115200"));
    QVERIFY(fixture.staMode()->isChecked());
    QVERIFY(fixture.stationGrid()->isEnabled());
    QCOMPARE(fixture.edit(QStringLiteral("hwEspIpSta"))->text(), QStringLiteral("10.42.0.20"));
    QCOMPARE(fixture.edit(QStringLiteral("hwEspGatewaySta"))->text(), QStringLiteral("10.42.0.1"));
    QCOMPARE(fixture.edit(QStringLiteral("hwEspSubnetSta"))->text(), QStringLiteral("255.255.255.0"));
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("hwEspDetails"))->text(),
             QStringLiteral("DEBUG_ENABLED 0,\nWIFI_MODE 1,\nWIFI_IPADDRESS 192.168.4.1,\n"
                            "WIFI_UDP_HPORT 14550,\nWIFI_UDP_CPORT 14555,\nWIFI_IPSTA 10.42.0.20,\n"
                            "WIFI_GATEWAYSTA 10.42.0.1,\nWIFI_SUBNET_STA 255.255.255.0\n"));

    // A second activation re-requests (MP10 Activate always reloads).
    view.activate();
    QCOMPARE(fixture.frames.size(), 2);
    QVERIFY(view.viewModel()->isBusy());
    // Activation while loading replaces the load with a fresh request.
    view.activate();
    QCOMPARE(fixture.frames.size(), 3);
    QVERIFY(view.viewModel()->isBusy());

    // The selected vehicle changes: the load is discarded with MP10's text
    // and late values for the old lease are ignored.
    fixture.targets.observeEndpoint(endpoint(4, 43));
    QVERIFY(fixture.targets.selectTarget(4, 43, MAV_COMP_ID_AUTOPILOT1));
    QVERIFY(!view.viewModel()->isBusy());
    QCOMPARE(fixture.status(),
             QStringLiteral("The selected device changed before ESP8266 parameters were loaded."));
    QVERIFY(!view.viewModel()->isLoaded());
    fixture.edit(QStringLiteral("hwEspSsid"))->setText(QStringLiteral("typed"));
    fixture.deliverAll(9);
    QCOMPARE(fixture.edit(QStringLiteral("hwEspSsid"))->text(), QStringLiteral("typed"));
    QVERIFY(fixture.controlsEnabled());
    // Unbound now: activation reports "Not connected." and sends nothing.
    view.activate();
    QCOMPARE(fixture.status(), QStringLiteral("Not connected."));
    QCOMPARE(fixture.frames.size(), 3);
}

void ConfigHWESP8266ViewTest::loadFailuresReportMissingParameters()
{
    Fixture fixture;
    ConfigHWESP8266View &view = fixture.view;
    fixture.connectVehicle();

    view.activate();
    QTRY_VERIFY_WITH_TIMEOUT(!view.viewModel()->isBusy(), 500);
    QCOMPARE(fixture.status(), QStringLiteral("No ESP8266 / UDP-bridge component responded."));
    QVERIFY(!view.viewModel()->isLoaded());
    QVERIFY(fixture.controlsEnabled());

    view.activate();
    fixture.client.observeMessage(
        9, parameterValue(QStringLiteral("WIFI_SSID1"), QByteArrayLiteral("Fiel")));
    QTRY_VERIFY_WITH_TIMEOUT(!view.viewModel()->isBusy(), 500);
    QVERIFY2(fixture.status().startsWith(QStringLiteral("Incomplete ESP8266 response; missing: ")),
             qPrintable(fixture.status()));
    QVERIFY(fixture.status().contains(QStringLiteral("WIFI_SUBNET_STA")));
    QVERIFY(!fixture.status().contains(QStringLiteral("WIFI_SSID1,")));
    QVERIFY(fixture.status().endsWith(QStringLiteral(".")));
}

void ConfigHWESP8266ViewTest::saveValidatesThenWritesTheExactList()
{
    Fixture fixture;
    ConfigHWESP8266View &view = fixture.view;
    fixture.connectVehicle();
    view.activate();
    fixture.deliverAll();
    QVERIFY(view.viewModel()->isLoaded());
    const int framesBefore = fixture.frames.size();

    // Invalid station address: MP10 text, nothing sent.
    fixture.edit(QStringLiteral("hwEspIpSta"))->setText(QStringLiteral("300.1.1.1"));
    fixture.button(QStringLiteral("hwEspSave"))->click();
    QCOMPARE(fixture.status(), QStringLiteral("Invalid channel, baud rate, or IPv4 station settings."));
    QCOMPARE(fixture.frames.size(), framesBefore);
    QVERIFY(fixture.controlsEnabled());

    fixture.edit(QStringLiteral("hwEspIpSta"))->setText(QStringLiteral("10.42.0.21"));
    fixture.edit(QStringLiteral("hwEspSsid"))->setText(QStringLiteral("NewNet"));
    fixture.button(QStringLiteral("hwEspSave"))->click();
    QCOMPARE(fixture.status(), QStringLiteral("Saving…"));
    QVERIFY(view.viewModel()->isBusy());
    QVERIFY(!fixture.controlsEnabled());
    QCOMPARE(fixture.frames.size(), framesBefore + 1);

    // Echo every PARAM_SET in MP10 order; the client only advances on the echo.
    const QStringList order = Esp8266SettingsCodec::WriteOrder();
    for (int index = 0; index < order.size(); ++index) {
        const int frameIndex = framesBefore + index;
        QCOMPARE(fixture.frames.size(), frameIndex + 1);
        const mavlink_message_t message = fixture.message(frameIndex);
        QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_PARAM_SET));
        mavlink_param_set_t set;
        std::memset(&set, 0, sizeof(set));
        mavlink_msg_param_set_decode(&message, &set);
        QCOMPARE(parameterId(set.param_id), order.at(index));
        QCOMPARE(int(set.target_component), int(MAV_COMP_ID_UDP_BRIDGE));
        QCOMPARE(int(set.param_type), int(MAV_PARAM_TYPE_UINT32));
        if (order.at(index) == QStringLiteral("WIFI_SSID1")) {
            quint32 bits = 0;
            std::memcpy(&bits, &set.param_value, sizeof(set.param_value));
            const QByteArray raw = Esp8266SettingsCodec::RawFromUInt32(bits);
            QCOMPARE(raw, QByteArrayLiteral("NewN"));
        }
        if (order.at(index) == QStringLiteral("WIFI_IPSTA")) {
            quint32 bits = 0;
            std::memcpy(&bits, &set.param_value, sizeof(set.param_value));
            const QByteArray raw = Esp8266SettingsCodec::RawFromUInt32(bits);
            QCOMPARE(raw, QByteArray("\x0A\x2A\x00\x15", 4));
        }
        fixture.echoWrite(frameIndex);
    }
    // Then storage, then reboot, then "Programmed OK.".
    const int storageIndex = framesBefore + order.size();
    QCOMPARE(fixture.frames.size(), storageIndex + 1);
    mavlink_command_long_t storage;
    std::memset(&storage, 0, sizeof(storage));
    const mavlink_message_t storageMessage = fixture.message(storageIndex);
    QCOMPARE(storageMessage.msgid, quint32(MAVLINK_MSG_ID_COMMAND_LONG));
    mavlink_msg_command_long_decode(&storageMessage, &storage);
    QCOMPARE(int(storage.command), int(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(storage.param1, 1.0F);
    fixture.client.observeMessage(9, commandAck(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(fixture.frames.size(), storageIndex + 2);
    mavlink_command_long_t reboot;
    std::memset(&reboot, 0, sizeof(reboot));
    const mavlink_message_t rebootMessage = fixture.message(storageIndex + 1);
    mavlink_msg_command_long_decode(&rebootMessage, &reboot);
    QCOMPARE(int(reboot.command), int(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN));
    QCOMPARE(reboot.param2, 1.0F);
    QCOMPARE(fixture.status(), QStringLiteral("Saving…"));
    fixture.client.observeMessage(9, commandAck(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN));
    QCOMPARE(fixture.status(), QStringLiteral("Programmed OK."));
    QVERIFY(!view.viewModel()->isBusy());
    QVERIFY(fixture.controlsEnabled());
}

void ConfigHWESP8266ViewTest::saveFailuresAndOfflineStatusesAreTruthful()
{
    Fixture fixture;
    ConfigHWESP8266View &view = fixture.view;

    // Offline: MP10 "Not connected." and nothing sent.
    fixture.button(QStringLiteral("hwEspSave"))->click();
    QCOMPARE(fixture.status(), QStringLiteral("Not connected."));
    fixture.button(QStringLiteral("hwEspResetDefaults"))->click();
    QCOMPARE(fixture.status(), QStringLiteral("Not connected."));
    QCOMPARE(fixture.frames.size(), 0);

    fixture.connectVehicle();
    // Transport refuses the first frame: "Error setting parameter.".
    fixture.writerAccepts = false;
    fixture.button(QStringLiteral("hwEspSave"))->click();
    QCOMPARE(fixture.status(), QStringLiteral("Error setting parameter."));
    QVERIFY(!view.viewModel()->isBusy());
    QVERIFY(fixture.controlsEnabled());

    // A rejected storage command fails the save.
    fixture.writerAccepts = true;
    fixture.button(QStringLiteral("hwEspSave"))->click();
    QCOMPARE(fixture.status(), QStringLiteral("Saving…"));
    const QStringList order = Esp8266SettingsCodec::WriteOrder();
    for (int index = 0; index < order.size(); ++index) {
        fixture.echoWrite(index);
    }
    QCOMPARE(fixture.frames.size(), order.size() + 1);
    fixture.client.observeMessage(9, commandAck(MAV_CMD_PREFLIGHT_STORAGE, MAV_RESULT_FAILED));
    QCOMPARE(fixture.status(), QStringLiteral("Error setting parameter."));
    QVERIFY(!view.viewModel()->isBusy());
    QVERIFY(fixture.controlsEnabled());

    // An unacknowledged parameter times out into the same MP10 text.
    const int before = fixture.frames.size();
    fixture.button(QStringLiteral("hwEspSave"))->click();
    QCOMPARE(fixture.frames.size(), before + 1);
    QTRY_VERIFY_WITH_TIMEOUT(!view.viewModel()->isBusy(), 1000);
    QCOMPARE(fixture.status(), QStringLiteral("Error setting parameter."));
    QVERIFY(fixture.frames.size() > before + 1); // the client retried first
}

void ConfigHWESP8266ViewTest::resetInvokesEraseSaveThenRefreshes()
{
    Fixture fixture;
    ConfigHWESP8266View &view = fixture.view;
    fixture.connectVehicle();

    fixture.button(QStringLiteral("hwEspResetDefaults"))->click();
    QCOMPARE(fixture.status(), QStringLiteral("Resetting to defaults…"));
    QVERIFY(!fixture.controlsEnabled());
    QCOMPARE(fixture.frames.size(), 1);
    mavlink_command_long_t erase;
    std::memset(&erase, 0, sizeof(erase));
    const mavlink_message_t eraseMessage = fixture.message(0);
    mavlink_msg_command_long_decode(&eraseMessage, &erase);
    QCOMPARE(int(erase.command), int(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(erase.param1, 2.0F);
    fixture.client.observeMessage(9, commandAck(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(fixture.frames.size(), 2);
    mavlink_command_long_t store;
    std::memset(&store, 0, sizeof(store));
    const mavlink_message_t storeMessage = fixture.message(1);
    mavlink_msg_command_long_decode(&storeMessage, &store);
    QCOMPARE(int(store.command), int(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(store.param1, 1.0F);
    QCOMPARE(fixture.status(), QStringLiteral("Resetting to defaults…"));

    // Success: MP10 text, and the parameters are requested again.
    fixture.client.observeMessage(9, commandAck(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(fixture.status(), QStringLiteral("Programmed OK. Refreshing parameters…"));
    QCOMPARE(fixture.frames.size(), 3);
    QCOMPARE(fixture.message(2).msgid, quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));
    QVERIFY(view.viewModel()->isBusy());
    fixture.deliverAll();
    QVERIFY(view.viewModel()->isLoaded());
    QVERIFY(fixture.status().isEmpty());
    QCOMPARE(fixture.edit(QStringLiteral("hwEspSsid"))->text(), QStringLiteral("FieldNetwork"));

    // A rejected erase never sends the save command.
    fixture.button(QStringLiteral("hwEspResetDefaults"))->click();
    QCOMPARE(fixture.frames.size(), 4);
    fixture.client.observeMessage(9, commandAck(MAV_CMD_PREFLIGHT_STORAGE, MAV_RESULT_DENIED));
    QCOMPARE(fixture.status(), QStringLiteral("Error setting parameter."));
    QCOMPARE(fixture.frames.size(), 4);
    QVERIFY(fixture.controlsEnabled());
}

void ConfigHWESP8266ViewTest::busyGatingDeactivationAndDisconnect()
{
    Fixture fixture;
    ConfigHWESP8266View &view = fixture.view;
    fixture.connectVehicle();

    // Clicks while a load runs are ignored (buttons are disabled).
    view.activate();
    QVERIFY(!fixture.controlsEnabled());
    fixture.button(QStringLiteral("hwEspSave"))->click();
    fixture.button(QStringLiteral("hwEspResetDefaults"))->click();
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(fixture.status(), QStringLiteral("Requesting ESP8266 parameters…"));

    // Deactivation cancels the load quietly and re-enables the page.
    view.deactivate();
    QVERIFY(!view.viewModel()->isBusy());
    QVERIFY(!fixture.client.isBusy());
    QCOMPARE(fixture.status(), QStringLiteral("Requesting ESP8266 parameters…"));
    QVERIFY(fixture.controlsEnabled());
    fixture.deliverAll();
    QVERIFY(!view.viewModel()->isLoaded()); // late values after the cancel are ignored
    QTest::qWait(60);
    QCOMPARE(fixture.frames.size(), 1);

    // Deactivation does not interrupt a running save; a disconnect does.
    view.activate();
    fixture.deliverAll();
    fixture.button(QStringLiteral("hwEspSave"))->click();
    QVERIFY(fixture.client.isBusy());
    view.deactivate();
    QVERIFY(fixture.client.isBusy());
    QCOMPARE(fixture.status(), QStringLiteral("Saving…"));
    view.setConnected(false);
    QVERIFY(!fixture.client.isBusy());
    QCOMPARE(fixture.status(), QStringLiteral("Not connected."));
    QVERIFY(!view.viewModel()->isLoaded());
    QVERIFY(fixture.controlsEnabled());
    const int frames = fixture.frames.size();
    QTest::qWait(60);
    QCOMPARE(fixture.frames.size(), frames);

    // parameterTargetChanged cancels a running reset.
    view.setConnected(true);
    fixture.button(QStringLiteral("hwEspResetDefaults"))->click();
    QVERIFY(fixture.client.isBusy());
    view.parameterTargetChanged();
    QVERIFY(!fixture.client.isBusy());
    QVERIFY(!view.viewModel()->isBusy());
    QCOMPARE(fixture.status(), QStringLiteral("Reset cancelled."));
}

void ConfigHWESP8266ViewTest::destructionDuringAnOperationIsSafe()
{
    VehicleTargetManager targets;
    QVector<QByteArray> frames;
    ExactLinkTransmitter transmitter([&frames](int, const QByteArray &bytes) {
        frames.append(bytes);
        return true;
    });
    Esp8266ParameterClient client(&targets, &transmitter);
    client.setTimingForTesting(30, 20, 1);
    targets.observeEndpoint(endpoint(9));
    QVERIFY(targets.selectTarget(9, 42, MAV_COMP_ID_AUTOPILOT1));
    QVERIFY(client.bind(targets.acquireTarget()));
    int failures = 0;
    QObject::connect(&client, &Esp8266ParameterClient::operationFailed, &client,
                     [&failures]() { ++failures; });

    {
        QPointer<ConfigHWESP8266View> view(new ConfigHWESP8266View(&client));
        view->setConnected(true);
        view->findChild<QPushButton *>(QStringLiteral("hwEspSave"))->click();
        QVERIFY(client.isBusy());
        delete view.data();
        QVERIFY(view.isNull());
    }
    // The page cancelled its save on the way out and the client is idle.
    QVERIFY(!client.isBusy());
    QCOMPARE(failures, 1);
    const int frameCount = frames.size();
    QTest::qWait(60);
    QCOMPARE(frames.size(), frameCount);
    QVERIFY(client.isBound());
}

QTEST_MAIN(ConfigHWESP8266ViewTest)
#include "test_confighwesp8266view.moc"
