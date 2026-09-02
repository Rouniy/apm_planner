#include <QtTest>

#include "comm/Esp8266ParameterClient.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleTargetManager.h"

#include <QSignalSpy>
#include <QVector>

#include <algorithm>
#include <cstring>

namespace {

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId, int systemId = 42,
                         int componentId = MAV_COMP_ID_AUTOPILOT1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("link%1").arg(linkId);
    return result;
}

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

void copyParameterId(const QString &name, char id[16])
{
    const QByteArray bytes = name.toLatin1();
    const int count = std::min(16, static_cast<int>(bytes.size()));
    std::memset(id, 0, 16);
    if (count > 0) {
        std::memcpy(id, bytes.constData(), static_cast<size_t>(count));
    }
}

QString parameterId(const char id[16])
{
    int length = 0;
    while (length < 16 && id[length] != '\0') {
        ++length;
    }
    return QString::fromLatin1(id, length);
}

mavlink_message_t parameterValue(int systemId, int componentId,
                                 const QString &name, quint32 value,
                                 quint8 type = MAV_PARAM_TYPE_UINT32)
{
    mavlink_param_value_t payload{};
    copyParameterId(name, payload.param_id);
    std::memcpy(&payload.param_value, &value, sizeof(payload.param_value));
    payload.param_type = type;
    payload.param_count = 64;
    payload.param_index = 0;
    mavlink_message_t message{};
    mavlink_msg_param_value_encode(
        static_cast<quint8>(systemId), static_cast<quint8>(componentId),
        &message, &payload);
    return message;
}

mavlink_message_t commandAck(int systemId, int componentId,
                             MAV_CMD command,
                             MAV_RESULT result = MAV_RESULT_ACCEPTED,
                             int targetSystem = 250,
                             int targetComponent = 190)
{
    mavlink_command_ack_t payload{};
    payload.command = static_cast<quint16>(command);
    payload.result = static_cast<quint8>(result);
    payload.target_system = static_cast<quint8>(targetSystem);
    payload.target_component = static_cast<quint8>(targetComponent);
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(
        static_cast<quint8>(systemId), static_cast<quint8>(componentId),
        &message, &payload);
    return message;
}

quint32 writeValue(const mavlink_param_set_t &payload)
{
    quint32 bits = 0;
    std::memcpy(&bits, &payload.param_value, sizeof(payload.param_value));
    return bits;
}

struct Fixture
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    bool writerAccepts = true;
    ExactLinkTransmitter transmitter;
    Esp8266ParameterClient client;

    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
              if (!writerAccepts) {
                  return false;
              }
              frames.append({linkId, bytes});
              return true;
          }),
          client(&targets, &transmitter)
    {
        client.setLocalIdentity(250, 190);
        client.setTimingForTesting(30, 20, 1);
    }

    VehicleTargetLease selectAndBind(int linkId = 9, int systemId = 42,
                                     int componentId = MAV_COMP_ID_AUTOPILOT1)
    {
        const VehicleEndpoint selected = endpoint(linkId, systemId, componentId);
        targets.observeEndpoint(selected);
        targets.selectTarget(linkId, systemId, componentId);
        const VehicleTargetLease lease = targets.acquireTarget();
        client.bind(lease);
        return lease;
    }

    mavlink_message_t message(int index) const
    {
        return decodeFrame(frames.at(index).bytes);
    }
};

} // namespace

class Esp8266ParameterClientTest final : public QObject
{
    Q_OBJECT

private slots:
    void loadUsesSelectedLinkAndSecondaryComponent();
    void loadAcceptsOnlyCompleteExactUint32Replies();
    void loadTimeoutReportsMissingParameters();
    void saveSerializesWritesStorageAndReboot();
    void resetUsesEraseThenSaveWithoutReboot();
    void reentrantCancelFromProgressStopsWorkflow();
    void startedSignalCanCancelWithoutLateTimeout();
    void retriesAreBoundedAndCommandAcksAreCorrelated();
    void targetChangeCancelsAndPreventsLateTraffic();
    void invalidAndTransportRejectedStartsDoNotBecomeBusy();
};

void Esp8266ParameterClientTest::loadUsesSelectedLinkAndSecondaryComponent()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectAndBind();
    QSignalSpy started(&fixture.client,
                       &Esp8266ParameterClient::loadStarted);

    QCOMPARE(fixture.client.requestParameters(),
             Esp8266ParameterClient::SendResult::Sent);
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.first().at(0).toULongLong(), lease.generation);
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(fixture.frames.first().linkId, 9);

    const mavlink_message_t message = fixture.message(0);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));
    QCOMPARE(message.sysid, quint8(250));
    QCOMPARE(message.compid, quint8(190));
    mavlink_param_request_list_t request{};
    mavlink_msg_param_request_list_decode(&message, &request);
    QCOMPARE(request.target_system, quint8(42));
    QCOMPARE(request.target_component,
             quint8(MAV_COMP_ID_UDP_BRIDGE));
}

void Esp8266ParameterClientTest::loadAcceptsOnlyCompleteExactUint32Replies()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectAndBind();
    QSignalSpy loaded(&fixture.client,
                      &Esp8266ParameterClient::parametersLoaded);
    fixture.client.requestParameters();

    const QStringList required = Esp8266SettingsCodec::RequiredParameterNames();
    fixture.client.observeMessage(
        8, parameterValue(42, MAV_COMP_ID_UDP_BRIDGE, required.first(), 1));
    fixture.client.observeMessage(
        9, parameterValue(41, MAV_COMP_ID_UDP_BRIDGE, required.first(), 1));
    fixture.client.observeMessage(
        9, parameterValue(42, MAV_COMP_ID_AUTOPILOT1, required.first(), 1));
    fixture.client.observeMessage(
        9, parameterValue(42, MAV_COMP_ID_UDP_BRIDGE, required.first(), 1,
                          MAV_PARAM_TYPE_REAL32));
    QCOMPARE(loaded.count(), 0);

    for (int index = 0; index < required.size(); ++index) {
        fixture.client.observeMessage(
            9, parameterValue(42, MAV_COMP_ID_UDP_BRIDGE,
                              required.at(index),
                              static_cast<quint32>(index + 10),
                              index == required.size() - 1
                                  ? MAV_PARAM_TYPE_REAL32
                                  : MAV_PARAM_TYPE_UINT32));
    }
    QCOMPARE(loaded.count(), 1);
    QCOMPARE(loaded.first().at(0).toULongLong(), lease.generation);
    const Esp8266RawParameters values =
        qvariant_cast<Esp8266RawParameters>(loaded.first().at(1));
    QCOMPARE(values.size(), required.size());
    QCOMPARE(Esp8266SettingsCodec::UInt32FromRaw(
                 values.value(required.last())),
             quint32(required.size() - 1 + 10));
    QVERIFY(!fixture.client.isBusy());
}

void Esp8266ParameterClientTest::loadTimeoutReportsMissingParameters()
{
    Fixture fixture;
    fixture.selectAndBind();
    QSignalSpy failed(&fixture.client,
                      &Esp8266ParameterClient::loadFailed);
    fixture.client.requestParameters();
    const QString first =
        Esp8266SettingsCodec::RequiredParameterNames().first();
    fixture.client.observeMessage(
        9, parameterValue(42, MAV_COMP_ID_UDP_BRIDGE, first, 7));

    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 200);
    const QStringList missing = failed.first().at(2).toStringList();
    QVERIFY(!missing.contains(first));
    QVERIFY(missing.contains(QStringLiteral("WIFI_SUBNET_STA")));
    QVERIFY(failed.first().at(1).toString().contains(
        QStringLiteral("WIFI_SUBNET_STA")));
    QVERIFY(!fixture.client.isBusy());
}

void Esp8266ParameterClientTest::saveSerializesWritesStorageAndReboot()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.selectAndBind();
    const QList<Esp8266ParameterWrite> writes{
        {QStringLiteral("WIFI_CHANNEL"), 11U},
        {QStringLiteral("UART_BAUDRATE"), 115200U}
    };
    QSignalSpy progress(&fixture.client,
                        &Esp8266ParameterClient::saveProgress);
    QSignalSpy completed(&fixture.client,
                         &Esp8266ParameterClient::saveCompleted);

    QCOMPARE(fixture.client.save(writes),
             Esp8266ParameterClient::SendResult::Sent);
    QCOMPARE(fixture.frames.size(), 1);
    mavlink_param_set_t first{};
    const mavlink_message_t firstMessage = fixture.message(0);
    mavlink_msg_param_set_decode(&firstMessage, &first);
    QCOMPARE(first.target_system, quint8(42));
    QCOMPARE(first.target_component, quint8(MAV_COMP_ID_UDP_BRIDGE));
    QCOMPARE(first.param_type, quint8(MAV_PARAM_TYPE_UINT32));
    QCOMPARE(parameterId(first.param_id), QStringLiteral("WIFI_CHANNEL"));
    QCOMPARE(writeValue(first), quint32(11));

    // A different value is an update, not the acknowledgement of our write.
    fixture.client.observeMessage(
        9, parameterValue(42, MAV_COMP_ID_UDP_BRIDGE,
                          QStringLiteral("WIFI_CHANNEL"), 10));
    QCOMPARE(fixture.frames.size(), 1);
    fixture.client.observeMessage(
        9, parameterValue(42, MAV_COMP_ID_UDP_BRIDGE,
                          QStringLiteral("WIFI_CHANNEL"), 11));
    QCOMPARE(progress.count(), 1);
    QCOMPARE(fixture.frames.size(), 2);
    mavlink_param_set_t second{};
    const mavlink_message_t secondMessage = fixture.message(1);
    mavlink_msg_param_set_decode(&secondMessage, &second);
    QCOMPARE(parameterId(second.param_id), QStringLiteral("UART_BAUDRATE"));
    QCOMPARE(writeValue(second), quint32(115200));

    fixture.client.observeMessage(
        9, parameterValue(42, MAV_COMP_ID_UDP_BRIDGE,
                          QStringLiteral("UART_BAUDRATE"), 115200));
    QCOMPARE(fixture.frames.size(), 3);
    mavlink_command_long_t storage{};
    const mavlink_message_t storageMessage = fixture.message(2);
    mavlink_msg_command_long_decode(&storageMessage, &storage);
    QCOMPARE(storage.command, quint16(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(storage.target_component, quint8(MAV_COMP_ID_UDP_BRIDGE));
    QCOMPARE(storage.param1, 1.0F);

    fixture.client.observeMessage(
        8, commandAck(42, MAV_COMP_ID_UDP_BRIDGE,
                      MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(fixture.frames.size(), 3);
    fixture.client.observeMessage(
        9, commandAck(42, MAV_COMP_ID_UDP_BRIDGE,
                      MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(fixture.frames.size(), 4);
    mavlink_command_long_t reboot{};
    const mavlink_message_t rebootMessage = fixture.message(3);
    mavlink_msg_command_long_decode(&rebootMessage, &reboot);
    QCOMPARE(reboot.command,
             quint16(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN));
    QCOMPARE(reboot.param2, 1.0F);

    fixture.client.observeMessage(
        9, commandAck(42, MAV_COMP_ID_UDP_BRIDGE,
                      MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN));
    QCOMPARE(completed.count(), 1);
    QCOMPARE(completed.first().at(0).toULongLong(), lease.generation);
    QCOMPARE(progress.count(), 2);
    QVERIFY(!fixture.client.isBusy());
}

void Esp8266ParameterClientTest::resetUsesEraseThenSaveWithoutReboot()
{
    Fixture fixture;
    fixture.selectAndBind();
    QSignalSpy completed(&fixture.client,
                         &Esp8266ParameterClient::resetCompleted);

    QCOMPARE(fixture.client.resetDefaults(),
             Esp8266ParameterClient::SendResult::Sent);
    mavlink_command_long_t erase{};
    const mavlink_message_t eraseMessage = fixture.message(0);
    mavlink_msg_command_long_decode(&eraseMessage, &erase);
    QCOMPARE(erase.command, quint16(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(erase.param1, 2.0F);
    fixture.client.observeMessage(
        9, commandAck(42, MAV_COMP_ID_UDP_BRIDGE,
                      MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(fixture.frames.size(), 2);
    mavlink_command_long_t save{};
    const mavlink_message_t saveMessage = fixture.message(1);
    mavlink_msg_command_long_decode(&saveMessage, &save);
    QCOMPARE(save.command, quint16(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(save.param1, 1.0F);
    fixture.client.observeMessage(
        9, commandAck(42, MAV_COMP_ID_UDP_BRIDGE,
                      MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(completed.count(), 1);
    QCOMPARE(fixture.frames.size(), 2);
}

void Esp8266ParameterClientTest::reentrantCancelFromProgressStopsWorkflow()
{
    Fixture fixture;
    fixture.selectAndBind();
    const QList<Esp8266ParameterWrite> writes{
        {QStringLiteral("WIFI_CHANNEL"), 11U},
        {QStringLiteral("UART_BAUDRATE"), 115200U}
    };
    QSignalSpy failures(&fixture.client,
                        &Esp8266ParameterClient::operationFailed);
    connect(&fixture.client, &Esp8266ParameterClient::saveProgress,
            &fixture.client, [&fixture](qulonglong, int, int, const QString &) {
        fixture.client.cancel();
    });

    QCOMPARE(fixture.client.save(writes),
             Esp8266ParameterClient::SendResult::Sent);
    fixture.client.observeMessage(
        9, parameterValue(42, MAV_COMP_ID_UDP_BRIDGE,
                          QStringLiteral("WIFI_CHANNEL"), 11));
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.first().at(0)
                 .value<Esp8266ParameterClient::Operation>(),
             Esp8266ParameterClient::Operation::Saving);
    QVERIFY(failures.first().at(3).toBool());
    QCOMPARE(fixture.frames.size(), 1); // no second PARAM_SET or storage command
    QVERIFY(!fixture.client.isBusy());

    disconnect(&fixture.client, nullptr, &fixture.client, nullptr);
    QCOMPARE(fixture.client.save(writes),
             Esp8266ParameterClient::SendResult::Sent);
    QCOMPARE(fixture.frames.size(), 2); // starts cleanly with WIFI_CHANNEL
    const mavlink_message_t restarted = fixture.message(1);
    QCOMPARE(restarted.msgid, quint32(MAVLINK_MSG_ID_PARAM_SET));
}

void Esp8266ParameterClientTest::startedSignalCanCancelWithoutLateTimeout()
{
    Fixture fixture;
    fixture.selectAndBind();
    QSignalSpy failures(&fixture.client,
                        &Esp8266ParameterClient::operationFailed);
    QSignalSpy loadFailures(&fixture.client,
                            &Esp8266ParameterClient::loadFailed);
    connect(&fixture.client, &Esp8266ParameterClient::loadStarted,
            &fixture.client, [&fixture](qulonglong) {
        fixture.client.cancel();
    });

    QCOMPARE(fixture.client.requestParameters(),
             Esp8266ParameterClient::SendResult::Sent);
    QCOMPARE(failures.count(), 1);
    QVERIFY(!fixture.client.isBusy());
    QTest::qWait(80);
    QCOMPARE(loadFailures.count(), 0);
    QCOMPARE(failures.count(), 1);
}

void Esp8266ParameterClientTest::retriesAreBoundedAndCommandAcksAreCorrelated()
{
    Fixture fixture;
    fixture.selectAndBind();
    fixture.client.setTimingForTesting(30, 10, 1);
    QSignalSpy failures(&fixture.client,
                        &Esp8266ParameterClient::operationFailed);
    const QList<Esp8266ParameterWrite> write{
        {QStringLiteral("WIFI_CHANNEL"), 11U}
    };

    fixture.client.save(write);
    QTRY_COMPARE_WITH_TIMEOUT(failures.count(), 1, 200);
    QCOMPARE(fixture.frames.size(), 2); // initial PARAM_SET plus one retry
    for (const CapturedFrame &frame : fixture.frames) {
        QCOMPARE(decodeFrame(frame.bytes).msgid,
                 quint32(MAVLINK_MSG_ID_PARAM_SET));
    }
    QVERIFY(failures.first().at(2).toString().contains(
        QStringLiteral("WIFI_CHANNEL")));
    QVERIFY(!failures.first().at(3).toBool());

    fixture.frames.clear();
    failures.clear();
    fixture.client.setTimingForTesting(30, 30, 1);
    fixture.client.resetDefaults();
    fixture.client.observeMessage(
        9, commandAck(42, MAV_COMP_ID_UDP_BRIDGE,
                      MAV_CMD_PREFLIGHT_STORAGE,
                      MAV_RESULT_IN_PROGRESS));
    QCOMPARE(fixture.frames.size(), 1);
    // Foreign ACK target must not advance the reset workflow.
    fixture.client.observeMessage(
        9, commandAck(42, MAV_COMP_ID_UDP_BRIDGE,
                      MAV_CMD_PREFLIGHT_STORAGE,
                      MAV_RESULT_ACCEPTED, 7, 190));
    QCOMPARE(fixture.frames.size(), 1);
    fixture.client.observeMessage(
        9, commandAck(42, MAV_COMP_ID_UDP_BRIDGE,
                      MAV_CMD_PREFLIGHT_STORAGE,
                      MAV_RESULT_DENIED));
    QCOMPARE(failures.count(), 1);
    QCOMPARE(fixture.frames.size(), 1); // rejected erase never sends storage(1)
    QVERIFY(failures.first().at(2).toString().contains(
        QStringLiteral("MAV_RESULT 2")));
}

void Esp8266ParameterClientTest::targetChangeCancelsAndPreventsLateTraffic()
{
    Fixture fixture;
    const VehicleTargetLease first = fixture.selectAndBind();
    QSignalSpy failures(&fixture.client,
                        &Esp8266ParameterClient::operationFailed);
    QSignalSpy invalidated(&fixture.client,
                           &Esp8266ParameterClient::leaseInvalidated);
    fixture.client.requestParameters();

    const VehicleEndpoint second = endpoint(10, 43, MAV_COMP_ID_AUTOPILOT1);
    fixture.targets.observeEndpoint(second);
    fixture.targets.selectTarget(10, 43, MAV_COMP_ID_AUTOPILOT1);
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.first().at(0).toInt(),
             int(Esp8266ParameterClient::Operation::Loading));
    QVERIFY(failures.first().at(3).toBool());
    QCOMPARE(invalidated.count(), 1);
    QCOMPARE(invalidated.first().at(0).toULongLong(), first.generation);
    QVERIFY(!fixture.client.isBound());

    const int framesAfterSwitch = fixture.frames.size();
    QTest::qWait(80);
    QCOMPARE(fixture.frames.size(), framesAfterSwitch);
}

void Esp8266ParameterClientTest::invalidAndTransportRejectedStartsDoNotBecomeBusy()
{
    Fixture fixture;
    QCOMPARE(fixture.client.requestParameters(),
             Esp8266ParameterClient::SendResult::NotBound);
    fixture.selectAndBind();
    QCOMPARE(fixture.client.save({}),
             Esp8266ParameterClient::SendResult::InvalidRequest);
    fixture.writerAccepts = false;
    QCOMPARE(fixture.client.resetDefaults(),
             Esp8266ParameterClient::SendResult::TransportUnavailable);
    QVERIFY(!fixture.client.isBusy());
}

QTEST_GUILESS_MAIN(Esp8266ParameterClientTest)
#include "test_esp8266parameterclient.moc"
