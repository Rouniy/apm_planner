#include <QtTest>

#include "comm/DeviceOperationService.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleTargetManager.h"

#include <QPointer>
#include <QSignalSpy>
#include <QVector>

#include <cstring>

namespace {

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(
    int linkId, int systemId = 42, int componentId = 1)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = componentId;
    value.linkName = QStringLiteral("link%1").arg(linkId);
    return value;
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

mavlink_message_t heartbeat(
    quint8 systemId, quint8 componentId, bool armed)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, MAV_TYPE_QUADROTOR,
        MAV_AUTOPILOT_ARDUPILOTMEGA,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        0, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t readReply(
    quint8 systemId, quint8 componentId, quint32 requestId,
    quint8 result, quint8 registerStart, quint8 count,
    const QByteArray &bytes = QByteArray())
{
    quint8 data[DeviceOperationService::MaximumDataLength]{};
    const int copyLength = qMin(
        bytes.size(), DeviceOperationService::MaximumDataLength);
    if (copyLength > 0) {
        std::memcpy(data, bytes.constData(),
                    static_cast<size_t>(copyLength));
    }
    mavlink_message_t message{};
    mavlink_msg_device_op_read_reply_pack(
        systemId, componentId, &message, requestId, result,
        registerStart, count, data, 0);
    return message;
}

mavlink_message_t writeReply(
    quint8 systemId, quint8 componentId, quint32 requestId,
    quint8 result)
{
    mavlink_message_t message{};
    mavlink_msg_device_op_write_reply_pack(
        systemId, componentId, &message, requestId, result);
    return message;
}

DeviceOperationService::OperationResult resultAt(
    const QSignalSpy &spy, int index = 0)
{
    return spy.at(index).at(0)
        .value<DeviceOperationService::OperationResult>();
}

struct Fixture
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    int writerCalls = 0;
    bool writerAccepts = true;
    ExactLinkTransmitter transmitter;
    DeviceOperationService service;

    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
              ++writerCalls;
              if (!writerAccepts) {
                  return false;
              }
              frames.append({linkId, bytes});
              return true;
          })
        , service(&targets, &transmitter)
    {
        service.setLocalIdentity(250, 190);
        service.setTimeoutMs(25);
    }

    VehicleTargetLease selectAndBind(
        int linkId = 9, int systemId = 42, int componentId = 1)
    {
        targets.observeEndpoint(endpoint(linkId, systemId, componentId));
        targets.selectTarget(linkId, systemId, componentId);
        const VehicleTargetLease lease = targets.acquireTarget();
        service.bind(lease);
        return lease;
    }

    mavlink_message_t frame(int index) const
    {
        return decodeFrame(frames.at(index).bytes);
    }
};

} // namespace

class DeviceOperationServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void validatesProtocolBoundsAndUtf8BusNames();
    void bindRequiresCurrentExactLease();
    void encodesFullLengthUtf8SpiName();
    void encodesReadAndReturnsAddressedData();
    void correlatesRepliesByFullEnvelopeTypeAndRequestId();
    void timesOutCancelsAndAllowsReentrantRestart();
    void requestIdsAreUniqueAcrossIndependentServices();
    void targetChangeAwayAndBackRequiresExplicitRebind();
    void linkRemovalAndShutdownCancelSafely();
    void reportsMavlink1AndTransportFailures();
    void icmTestRequiresDisarmedSpiAndRunsFixedSequence();
    void icmWriteTimeoutStillContinuesToRead();
    void armingDuringIcmTestCancelsBeforeTheRead();
    void malformedReplyIsBoundedAndDiagnosed();
    void synchronousReplyCanFinishAndDeleteDuringStart();
    void destructionAndReentrantDeletionAreSafeAndSilent();
};

void DeviceOperationServiceTest::validatesProtocolBoundsAndUtf8BusNames()
{
    DeviceOperationService::Request request;
    QVERIFY(DeviceOperationService::ValidateRequest(request).isEmpty());

    request.destinationSystemId = 0;
    QVERIFY(!DeviceOperationService::ValidateRequest(request).isEmpty());
    request.destinationSystemId = 1;
    request.destinationComponentId = 256;
    QVERIFY(!DeviceOperationService::ValidateRequest(request).isEmpty());
    request.destinationComponentId = 1;
    request.count = 129;
    QVERIFY(!DeviceOperationService::ValidateRequest(request).isEmpty());
    request.count = 1;
    request.busType = DeviceOperationService::BusType::SPI;
    request.busName = QString(20, QChar(0x00e9)); // exactly 40 UTF-8 bytes
    QVERIFY(DeviceOperationService::ValidateRequest(request).isEmpty());
    request.busName.append(QChar(0x00e9));
    QVERIFY(!DeviceOperationService::ValidateRequest(request).isEmpty());
    request.busName.clear();
    QVERIFY(!DeviceOperationService::ValidateRequest(request).isEmpty());

    request.busType = DeviceOperationService::BusType::I2C;
    QVERIFY(DeviceOperationService::ValidateRequest(request).isEmpty());
    request.busType = static_cast<DeviceOperationService::BusType>(99);
    QVERIFY(!DeviceOperationService::ValidateRequest(request).isEmpty());
}

void DeviceOperationServiceTest::bindRequiresCurrentExactLease()
{
    Fixture fixture;
    DeviceOperationService::Request request;
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::NotBound);
    QVERIFY(!fixture.service.bind(VehicleTargetLease()));

    QVERIFY(fixture.targets.observeEndpoint(endpoint(3), true));
    QVERIFY(fixture.targets.observeEndpoint(endpoint(4)));
    const VehicleTargetLease stale = fixture.targets.acquireTarget();
    QVERIFY(fixture.targets.selectTarget(4, 42, 1));
    QVERIFY(!fixture.service.bind(stale));
    QVERIFY(!fixture.service.isBound());
    QVERIFY(fixture.service.bind(fixture.targets.acquireTarget()));
    QVERIFY(fixture.service.isBound());
}

void DeviceOperationServiceTest::encodesFullLengthUtf8SpiName()
{
    Fixture fixture;
    fixture.selectAndBind();
    fixture.service.setTimeoutMs(500);
    DeviceOperationService::Request request;
    request.busType = DeviceOperationService::BusType::SPI;
    request.busName = QString(20, QChar(0x00e9));
    QCOMPARE(request.busName.toUtf8().size(), 40);
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Started);
    mavlink_device_op_read_t payload{};
    const mavlink_message_t sent = fixture.frame(0);
    mavlink_msg_device_op_read_decode(&sent, &payload);
    QCOMPARE(QByteArray(payload.busname, 40), request.busName.toUtf8());
    fixture.service.cancel();
}

void DeviceOperationServiceTest::encodesReadAndReturnsAddressedData()
{
    Fixture fixture;
    fixture.selectAndBind();
    QSignalSpy finished(
        &fixture.service, &DeviceOperationService::operationFinished);

    DeviceOperationService::Request request;
    request.destinationSystemId = 77;
    request.destinationComponentId = 55;
    request.busType = DeviceOperationService::BusType::I2C;
    request.busName = QStringLiteral("ignored-for-i2c");
    request.bus = 4;
    request.address = 104;
    request.registerStart = 0xf8;
    request.count = 18;
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Started);
    QVERIFY(fixture.service.isBusy());
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(fixture.frames.first().linkId, 9);

    const mavlink_message_t sent = fixture.frame(0);
    QCOMPARE(sent.magic, quint8(MAVLINK_STX));
    QCOMPARE(sent.msgid, quint32(MAVLINK_MSG_ID_DEVICE_OP_READ));
    QCOMPARE(sent.sysid, quint8(250));
    QCOMPARE(sent.compid, quint8(190));
    mavlink_device_op_read_t payload{};
    mavlink_msg_device_op_read_decode(&sent, &payload);
    QCOMPARE(payload.target_system, quint8(77));
    QCOMPARE(payload.target_component, quint8(55));
    QCOMPARE(payload.bustype, quint8(DEVICE_OP_BUSTYPE_I2C));
    QCOMPARE(payload.bus, quint8(4));
    QCOMPARE(payload.address, quint8(104));
    QCOMPARE(payload.regstart, quint8(0xf8));
    QCOMPARE(payload.count, quint8(18));
    QCOMPARE(payload.bank, quint8(0));
    QCOMPARE(QByteArray(payload.busname, 40), QByteArray(40, '\0'));

    QByteArray data;
    for (int value = 0; value < 18; ++value) {
        data.append(static_cast<char>(value));
    }
    // MP10 formats from the requested register, even if the peer echoes a
    // different regstart.
    fixture.service.observeMessage(
        9, readReply(77, 55, payload.request_id, 4, 0x12, 18, data));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!fixture.service.isBusy());
    const auto result = resultAt(finished);
    QVERIFY(result.readReplyReceived);
    QVERIFY(!result.readTimedOut);
    QCOMPARE(result.readResult, quint8(4));
    QCOMPARE(result.readRegister, quint8(0xf8));
    QCOMPARE(result.readData, data);
}

void DeviceOperationServiceTest::correlatesRepliesByFullEnvelopeTypeAndRequestId()
{
    Fixture fixture;
    fixture.selectAndBind();
    fixture.service.setTimeoutMs(500);
    QSignalSpy finished(
        &fixture.service, &DeviceOperationService::operationFinished);

    DeviceOperationService::Request request;
    request.destinationSystemId = 77;
    request.destinationComponentId = 55;
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Started);
    mavlink_device_op_read_t sent{};
    const mavlink_message_t frame = fixture.frame(0);
    mavlink_msg_device_op_read_decode(&frame, &sent);

    const QByteArray data = QByteArray::fromHex("aabb");
    fixture.service.observeMessage(
        8, readReply(77, 55, sent.request_id, 0, 255, 2, data));
    fixture.service.observeMessage(
        9, readReply(76, 55, sent.request_id, 0, 255, 2, data));
    fixture.service.observeMessage(
        9, readReply(77, 54, sent.request_id, 0, 255, 2, data));
    fixture.service.observeMessage(
        9, readReply(77, 55, sent.request_id + 1, 0, 255, 2, data));
    fixture.service.observeMessage(
        9, writeReply(77, 55, sent.request_id, 0));
    QCOMPARE(finished.count(), 0);
    QVERIFY(fixture.service.isBusy());

    fixture.service.observeMessage(
        9, readReply(77, 55, sent.request_id, 0, 255, 2, data));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(resultAt(finished).readData, data);

    fixture.service.observeMessage(
        9, readReply(77, 55, sent.request_id, 0, 255, 2, data));
    QCOMPARE(finished.count(), 1); // late duplicate ignored
}

void DeviceOperationServiceTest::timesOutCancelsAndAllowsReentrantRestart()
{
    Fixture fixture;
    fixture.selectAndBind();
    fixture.service.setTimeoutMs(5);
    QSignalSpy finished(
        &fixture.service, &DeviceOperationService::operationFinished);
    DeviceOperationService::Request request;

    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Started);
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Busy);
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(resultAt(finished).readTimedOut);
    QVERIFY(!resultAt(finished).cancelled);
    QVERIFY(!fixture.service.isBusy());

    fixture.service.setTimeoutMs(500);
    DeviceOperationService::StartResult reentrantStart =
        DeviceOperationService::StartResult::Busy;
    bool restarted = false;
    const QMetaObject::Connection connection = connect(
        &fixture.service, &DeviceOperationService::operationFinished,
        &fixture.service,
        [&](const DeviceOperationService::OperationResult &) {
            if (!restarted) {
                restarted = true;
                // finish() removes the old operation before this signal.
                fixture.service.cancel();
                reentrantStart = fixture.service.startRead(request);
            }
        });
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Started);
    fixture.service.cancel();
    QVERIFY(restarted);
    QCOMPARE(reentrantStart, DeviceOperationService::StartResult::Started);
    QVERIFY(fixture.service.isBusy());
    disconnect(connection);
    fixture.service.cancel();
    QCOMPARE(finished.count(), 3);
    QVERIFY(resultAt(finished, 1).cancelled);
    QVERIFY(resultAt(finished, 2).cancelled);
}

void DeviceOperationServiceTest::requestIdsAreUniqueAcrossIndependentServices()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    QVERIFY(targets.observeEndpoint(endpoint(9), true));
    DeviceOperationService first(&targets, &transmitter);
    DeviceOperationService second(&targets, &transmitter);
    QVERIFY(first.bind(targets.acquireTarget()));
    QVERIFY(second.bind(targets.acquireTarget()));
    first.setTimeoutMs(500);
    second.setTimeoutMs(500);
    QSignalSpy firstFinished(&first, &DeviceOperationService::operationFinished);
    QSignalSpy secondFinished(&second, &DeviceOperationService::operationFinished);

    DeviceOperationService::Request request;
    QCOMPARE(first.startRead(request),
             DeviceOperationService::StartResult::Started);
    QCOMPARE(second.startRead(request),
             DeviceOperationService::StartResult::Started);
    QCOMPARE(frames.size(), 2);
    mavlink_device_op_read_t firstRequest{};
    mavlink_device_op_read_t secondRequest{};
    const mavlink_message_t firstFrame = decodeFrame(frames.at(0).bytes);
    const mavlink_message_t secondFrame = decodeFrame(frames.at(1).bytes);
    mavlink_msg_device_op_read_decode(&firstFrame, &firstRequest);
    mavlink_msg_device_op_read_decode(&secondFrame, &secondRequest);
    QVERIFY(firstRequest.request_id != secondRequest.request_id);

    const mavlink_message_t secondReply = readReply(
        1, 1, secondRequest.request_id, 0, 255, 1,
        QByteArray(1, '\x22'));
    first.observeMessage(9, secondReply);
    second.observeMessage(9, secondReply);
    QCOMPARE(firstFinished.count(), 0);
    QCOMPARE(secondFinished.count(), 1);

    const mavlink_message_t firstReply = readReply(
        1, 1, firstRequest.request_id, 0, 255, 1,
        QByteArray(1, '\x11'));
    first.observeMessage(9, firstReply);
    second.observeMessage(9, firstReply);
    QCOMPARE(firstFinished.count(), 1);
    QCOMPARE(secondFinished.count(), 1);
}

void DeviceOperationServiceTest::targetChangeAwayAndBackRequiresExplicitRebind()
{
    Fixture fixture;
    const VehicleTargetLease original = fixture.selectAndBind(9, 42, 1);
    QVERIFY(fixture.targets.observeEndpoint(endpoint(10, 42, 1)));
    fixture.service.setTimeoutMs(500);
    QSignalSpy finished(
        &fixture.service, &DeviceOperationService::operationFinished);
    QSignalSpy invalidated(
        &fixture.service, &DeviceOperationService::bindingInvalidated);
    DeviceOperationService::Request request;

    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Started);
    QVERIFY(fixture.targets.selectTarget(10, 42, 1));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(invalidated.count(), 1);
    QVERIFY(resultAt(finished).cancelled);
    QVERIFY(fixture.service.requiresRebind());
    QVERIFY(!fixture.service.isBound());

    QVERIFY(fixture.targets.selectTarget(
        original.endpoint.linkId, original.endpoint.systemId,
        original.endpoint.componentId));
    QCOMPARE(invalidated.count(), 1);
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::StaleTarget);
    QVERIFY(fixture.service.bind(fixture.targets.acquireTarget()));
    QVERIFY(fixture.service.isBound());
    QVERIFY(!fixture.service.requiresRebind());
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Started);
    fixture.service.cancel();
}

void DeviceOperationServiceTest::linkRemovalAndShutdownCancelSafely()
{
    Fixture fixture;
    fixture.selectAndBind();
    fixture.service.setTimeoutMs(500);
    QSignalSpy finished(
        &fixture.service, &DeviceOperationService::operationFinished);
    DeviceOperationService::Request request;
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Started);

    fixture.service.forgetLink(9);
    QCOMPARE(finished.count(), 1);
    QVERIFY(resultAt(finished).cancelled);
    QVERIFY(fixture.service.requiresRebind());
    QVERIFY(!fixture.service.isBusy());

    Fixture shuttingDown;
    shuttingDown.selectAndBind();
    shuttingDown.service.setTimeoutMs(500);
    QSignalSpy noShutdownCallback(
        &shuttingDown.service, &DeviceOperationService::operationFinished);
    QCOMPARE(shuttingDown.service.startRead(request),
             DeviceOperationService::StartResult::Started);
    shuttingDown.service.shutdown();
    QCOMPARE(noShutdownCallback.count(), 0);
    QVERIFY(!shuttingDown.service.isBusy());
    QVERIFY(!shuttingDown.service.isBound());
    QCOMPARE(shuttingDown.service.startRead(request),
             DeviceOperationService::StartResult::ShuttingDown);
}

void DeviceOperationServiceTest::reportsMavlink1AndTransportFailures()
{
    Fixture fixture;
    fixture.selectAndBind();
    DeviceOperationService::Request request;
    fixture.transmitter.setOutboundVersion(9, 1);
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Mavlink1Unsupported);
    QVERIFY(fixture.service.lastError().contains(QStringLiteral("MAVLink 2")));
    QCOMPARE(fixture.writerCalls, 0);
    QVERIFY(!fixture.service.isBusy());

    fixture.transmitter.setOutboundVersion(9, 2);
    fixture.writerAccepts = false;
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::TransportUnavailable);
    QCOMPARE(fixture.writerCalls, 1);
    QVERIFY(!fixture.service.isBusy());
}

void DeviceOperationServiceTest::icmTestRequiresDisarmedSpiAndRunsFixedSequence()
{
    Fixture fixture;
    fixture.selectAndBind();
    fixture.service.setTimeoutMs(500);
    DeviceOperationService::Request request;
    request.destinationSystemId = 77;
    request.destinationComponentId = 55;
    request.busName = QStringLiteral("  icm20948_ext  ");

    QCOMPARE(fixture.service.startIcm20948Test(request),
             DeviceOperationService::StartResult::InvalidRequest);
    QVERIFY(fixture.service.lastError().contains(
        QStringLiteral("bound target 42:1")));
    QCOMPARE(fixture.frames.size(), 0);
    request.destinationSystemId = 42;
    request.destinationComponentId = 1;

    QCOMPARE(fixture.service.armState(),
             DeviceOperationService::ArmState::Unknown);
    QCOMPARE(fixture.service.startIcm20948Test(request),
             DeviceOperationService::StartResult::UnsafeArmState);
    QCOMPARE(fixture.frames.size(), 0);
    fixture.service.observeMessage(8, heartbeat(42, 1, false));
    fixture.service.observeMessage(9, heartbeat(41, 1, false));
    QCOMPARE(fixture.service.armState(),
             DeviceOperationService::ArmState::Unknown);
    fixture.service.observeMessage(9, heartbeat(42, 1, false));
    QCOMPARE(fixture.service.armState(),
             DeviceOperationService::ArmState::Disarmed);

    request.busType = DeviceOperationService::BusType::I2C;
    QCOMPARE(fixture.service.startIcm20948Test(request),
             DeviceOperationService::StartResult::InvalidRequest);
    request.busType = DeviceOperationService::BusType::SPI;
    QSignalSpy finished(
        &fixture.service, &DeviceOperationService::operationFinished);
    QCOMPARE(fixture.service.startIcm20948Test(request),
             DeviceOperationService::StartResult::Started);
    QCOMPARE(fixture.frames.size(), 1);

    const mavlink_message_t writeFrame = fixture.frame(0);
    QCOMPARE(writeFrame.msgid, quint32(MAVLINK_MSG_ID_DEVICE_OP_WRITE));
    mavlink_device_op_write_t write{};
    mavlink_msg_device_op_write_decode(&writeFrame, &write);
    QCOMPARE(write.target_system, quint8(42));
    QCOMPARE(write.target_component, quint8(1));
    QCOMPARE(write.bustype, quint8(DEVICE_OP_BUSTYPE_SPI));
    QCOMPARE(write.bus, quint8(0));
    QCOMPARE(write.address, quint8(0));
    QCOMPARE(write.regstart, quint8(0xff));
    QCOMPARE(write.count, quint8(2));
    QCOMPARE(write.bank, quint8(0));
    QCOMPARE(write.data[0], quint8(0x72));
    QCOMPARE(write.data[1], quint8(0x00));
    QVERIFY(QByteArray(write.busname, 40).startsWith(
        QByteArrayLiteral("icm20948_ext")));

    // A nonzero write result is retained but, like MP10, does not suppress the
    // diagnostic read.
    fixture.service.observeMessage(
        9, writeReply(42, 1, write.request_id, 4));
    QCOMPARE(fixture.frames.size(), 2);
    const mavlink_message_t readFrame = fixture.frame(1);
    QCOMPARE(readFrame.msgid, quint32(MAVLINK_MSG_ID_DEVICE_OP_READ));
    mavlink_device_op_read_t read{};
    mavlink_msg_device_op_read_decode(&readFrame, &read);
    QVERIFY(read.request_id != write.request_id);
    QCOMPARE(read.target_system, quint8(42));
    QCOMPARE(read.target_component, quint8(1));
    QCOMPARE(read.bustype, quint8(DEVICE_OP_BUSTYPE_SPI));
    QCOMPARE(read.bus, quint8(0));
    QCOMPARE(read.address, quint8(0));
    QCOMPARE(read.regstart, quint8(0xff));
    QCOMPARE(read.count, quint8(2));

    fixture.service.observeMessage(
        9, readReply(42, 1, read.request_id, 0, 0xff, 2,
                     QByteArray::fromHex("ea01")));
    QCOMPARE(finished.count(), 1);
    const auto result = resultAt(finished);
    QVERIFY(result.icm20948Test);
    QVERIFY(result.writeReplyReceived);
    QVERIFY(!result.writeTimedOut);
    QCOMPARE(result.writeResult, quint8(4));
    QVERIFY(result.readReplyReceived);
    QCOMPARE(result.readResult, quint8(0));
    QCOMPARE(result.readData, QByteArray::fromHex("ea01"));
}

void DeviceOperationServiceTest::icmWriteTimeoutStillContinuesToRead()
{
    Fixture fixture;
    fixture.selectAndBind();
    fixture.service.observeMessage(9, heartbeat(42, 1, false));
    fixture.service.setTimeoutMs(5);
    QSignalSpy finished(
        &fixture.service, &DeviceOperationService::operationFinished);
    DeviceOperationService::Request request;
    request.destinationSystemId = 42;
    request.destinationComponentId = 1;

    QCOMPARE(fixture.service.startIcm20948Test(request),
             DeviceOperationService::StartResult::Started);
    QCOMPARE(fixture.frames.size(), 1);
    QTRY_COMPARE(fixture.frames.size(), 2);
    const mavlink_message_t readFrame = fixture.frame(1);
    QCOMPARE(readFrame.msgid, quint32(MAVLINK_MSG_ID_DEVICE_OP_READ));
    QTRY_COMPARE(finished.count(), 1);
    const auto result = resultAt(finished);
    QVERIFY(result.writeTimedOut);
    QVERIFY(!result.writeReplyReceived);
    QVERIFY(result.readTimedOut);
}

void DeviceOperationServiceTest::armingDuringIcmTestCancelsBeforeTheRead()
{
    Fixture fixture;
    fixture.selectAndBind();
    fixture.service.observeMessage(9, heartbeat(42, 1, false));
    fixture.service.setTimeoutMs(500);
    QSignalSpy finished(
        &fixture.service, &DeviceOperationService::operationFinished);
    DeviceOperationService::Request request;
    request.destinationSystemId = 42;
    request.destinationComponentId = 1;

    QCOMPARE(fixture.service.startIcm20948Test(request),
             DeviceOperationService::StartResult::Started);
    QCOMPARE(fixture.frames.size(), 1);
    fixture.service.observeMessage(9, heartbeat(42, 1, true));
    QCOMPARE(finished.count(), 1);
    QVERIFY(resultAt(finished).cancelled);
    QVERIFY(resultAt(finished).error.contains(
        QStringLiteral("armed"), Qt::CaseInsensitive));
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(!fixture.service.isBusy());
    QCOMPARE(fixture.service.armState(),
             DeviceOperationService::ArmState::Armed);
    QCOMPARE(fixture.service.startIcm20948Test(request),
             DeviceOperationService::StartResult::UnsafeArmState);
}

void DeviceOperationServiceTest::malformedReplyIsBoundedAndDiagnosed()
{
    Fixture fixture;
    fixture.selectAndBind();
    fixture.service.setTimeoutMs(500);
    QSignalSpy finished(
        &fixture.service, &DeviceOperationService::operationFinished);
    DeviceOperationService::Request request;
    QCOMPARE(fixture.service.startRead(request),
             DeviceOperationService::StartResult::Started);
    mavlink_device_op_read_t sent{};
    const mavlink_message_t frame = fixture.frame(0);
    mavlink_msg_device_op_read_decode(&frame, &sent);

    fixture.service.observeMessage(
        9, readReply(1, 1, sent.request_id, 0, 255, 200,
                     QByteArray(DeviceOperationService::MaximumDataLength,
                                '\x5a')));
    QCOMPARE(finished.count(), 1);
    const auto result = resultAt(finished);
    QVERIFY(result.readReplyReceived);
    QCOMPARE(result.readData.size(),
             DeviceOperationService::MaximumDataLength);
    QVERIFY(result.error.contains(QStringLiteral("at most 128")));
}

void DeviceOperationServiceTest::synchronousReplyCanFinishAndDeleteDuringStart()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(9), true));
    QPointer<DeviceOperationService> service;
    int completions = 0;
    ExactLinkTransmitter transmitter(
        [&](int linkId, const QByteArray &bytes) {
            const mavlink_message_t sent = decodeFrame(bytes);
            mavlink_device_op_read_t request{};
            mavlink_msg_device_op_read_decode(&sent, &request);
            if (service) {
                service->observeMessage(
                    linkId,
                    readReply(1, 1, request.request_id, 0, 255, 1,
                              QByteArray(1, '\x42')));
            }
            return true;
        });
    service = new DeviceOperationService(&targets, &transmitter);
    QVERIFY(service->bind(targets.acquireTarget()));
    connect(service.data(), &DeviceOperationService::operationFinished,
            &targets,
            [&](const DeviceOperationService::OperationResult &result) {
        ++completions;
        QCOMPARE(result.readData, QByteArray(1, '\x42'));
        delete service.data();
    });

    DeviceOperationService *const raw = service.data();
    const DeviceOperationService::StartResult start =
        raw->startRead(DeviceOperationService::Request());
    QCOMPARE(start, DeviceOperationService::StartResult::Started);
    QCOMPARE(completions, 1);
    QVERIFY(service.isNull());
}

void DeviceOperationServiceTest::destructionAndReentrantDeletionAreSafeAndSilent()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    QVERIFY(targets.observeEndpoint(endpoint(9), true));
    int completionCount = 0;

    auto *silent = new DeviceOperationService(&targets, &transmitter);
    QVERIFY(silent->bind(targets.acquireTarget()));
    silent->setTimeoutMs(500);
    connect(silent, &DeviceOperationService::operationFinished,
            &targets, [&](const DeviceOperationService::OperationResult &) {
        ++completionCount;
    });
    QCOMPARE(silent->startRead(DeviceOperationService::Request()),
             DeviceOperationService::StartResult::Started);
    delete silent;
    QCOMPARE(completionCount, 0); // destructor is silent

    frames.clear();
    auto *doomed = new DeviceOperationService(&targets, &transmitter);
    QPointer<DeviceOperationService> guard(doomed);
    QVERIFY(doomed->bind(targets.acquireTarget()));
    doomed->setTimeoutMs(500);
    connect(doomed, &DeviceOperationService::operationFinished,
            doomed, [doomed](const DeviceOperationService::OperationResult &) {
        delete doomed;
    });
    QCOMPARE(doomed->startRead(DeviceOperationService::Request()),
             DeviceOperationService::StartResult::Started);
    mavlink_device_op_read_t request{};
    const mavlink_message_t sent = decodeFrame(frames.last().bytes);
    mavlink_msg_device_op_read_decode(&sent, &request);
    doomed->observeMessage(
        9, readReply(1, 1, request.request_id, 0, 255, 1,
                     QByteArray(1, '\x01')));
    QVERIFY(guard.isNull());
}

QTEST_GUILESS_MAIN(DeviceOperationServiceTest)

#include "test_deviceoperationservice.moc"
