#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"

#include <QtTest>

#include <cmath>
#include <functional>
#include <limits>

namespace
{

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId = 7, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("Exact INT link");
    result.componentName = QStringLiteral("Autopilot");
    return result;
}

SwarmVehicleInstanceLease instanceLease()
{
    SwarmVehicleInstanceLease result;
    result.endpoint = endpoint();
    result.linkSessionEpoch = 9;
    result.instanceEpoch = 17;
    return result;
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int status = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        status = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    return status == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

mavlink_message_t commandAck(const SwarmVehicleInstanceLease &lease,
                             MAV_CMD command,
                             MAV_RESULT result = MAV_RESULT_ACCEPTED)
{
    mavlink_command_ack_t payload{};
    payload.command = quint16(command);
    payload.result = quint8(result);
    payload.progress = 255;
    payload.target_system = 250;
    payload.target_component = 190;
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(
        quint8(lease.endpoint.systemId),
        quint8(lease.endpoint.componentId), &message, &payload);
    return message;
}

VehicleCommandService::ExactCommandIntRequest commandIntRequest(
    int timeoutMs = 1000, int maximumLifetimeMs = 2000)
{
    VehicleCommandService::ExactCommandIntRequest request;
    request.command = MAV_CMD_DO_REPOSITION;
    request.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    request.params = {-1.0F, 2.5F, -3.25F,
                      std::numeric_limits<float>::quiet_NaN()};
    request.x = -353629381;
    request.y = 1491652379;
    request.z = 85.5F;
    request.current = 1;
    request.autocontinue = 7;
    request.acknowledgementTimeoutMs = timeoutMs;
    request.maximumLifetimeMs = maximumLifetimeMs;
    return request;
}

QByteArray payloadBytes(const mavlink_message_t &message)
{
    return QByteArray(_MAV_PAYLOAD(&message), message.len);
}

class Fixture
{
public:
    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
              frames.append({linkId, bytes});
              return writeHook ? writeHook(linkId, bytes) : true;
          })
        , service(&targets, &transmitter)
    {
        service.setLocalIdentity(250, 190);
    }

    bool initialize()
    {
        if (!service.configureExactTransactions(
                [this](const SwarmVehicleInstanceLease &candidate) {
                    return active && candidate.sameInstance(lease);
                },
                [this](const SwarmVehicleInstanceLease &candidate,
                       QString *error) {
                    if (!candidate.sameInstance(lease)) {
                        if (error) *error = QStringLiteral("Wrong route");
                        return false;
                    }
                    return routeHook ? routeHook(candidate, error) : true;
                })) {
            return false;
        }
        if (!targets.observeEndpoint(lease.endpoint, true)) {
            return false;
        }
        return service.reserveExactEndpoints(
                   &owner, {lease}, &reservation)
            == VehicleCommandService::ExactReservationResult::Reserved;
    }

    VehicleTargetManager targets;
    const SwarmVehicleInstanceLease lease = instanceLease();
    bool active = true;
    QVector<CapturedFrame> frames;
    std::function<bool(int, const QByteArray &)> writeHook;
    std::function<bool(const SwarmVehicleInstanceLease &, QString *)>
        routeHook;
    ExactLinkTransmitter transmitter;
    VehicleCommandService service;
    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
};

VehicleCommandService::ExactCommandReport reportAt(
    const QSignalSpy &spy, int index)
{
    return qvariant_cast<VehicleCommandService::ExactCommandReport>(
        spy.at(index).at(0));
}

} // namespace

class ExactCommandIntTest final : public QObject
{
    Q_OBJECT

private slots:
    void wirePayloadIsExact_data();
    void wirePayloadIsExact();
    void retryKeepsIdenticalCommandPayload();
    void terminalAckAfterRetryQuarantinesBothWireForms_data();
    void terminalAckAfterRetryQuarantinesBothWireForms();
    void longAndIntShareQuarantine_data();
    void longAndIntShareQuarantine();
    void postSigningCallbacksFailClosed();
    void synchronousAckUsesInstalledWaiter();
    void endpointReservationAndPendingLaneAreShared();
};

void ExactCommandIntTest::wirePayloadIsExact_data()
{
    QTest::addColumn<int>("wireVersion");
    QTest::newRow("mavlink-1") << 1;
    QTest::newRow("mavlink-2") << 2;
}

void ExactCommandIntTest::wirePayloadIsExact()
{
    QFETCH(int, wireVersion);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.transmitter.setOutboundVersion(
        fixture.lease.endpoint.linkId, unsigned(wireVersion));

    const auto request = commandIntRequest();
    VehicleCommandService::ExactCommandToken token;
    QCOMPARE(fixture.service.submitExactCommandInt(
                 fixture.reservation, fixture.lease, request, &token),
             VehicleCommandService::ExactSubmitResult::Started);
    QVERIFY(token.isValid());
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(quint8(fixture.frames.constFirst().bytes.at(0)),
             wireVersion == 1 ? quint8(MAVLINK_STX_MAVLINK1)
                              : quint8(MAVLINK_STX));

    const mavlink_message_t message =
        decodeFrame(fixture.frames.constFirst().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_COMMAND_INT));
    QCOMPARE(message.sysid, quint8(250));
    QCOMPARE(message.compid, quint8(190));
    mavlink_command_int_t payload{};
    mavlink_msg_command_int_decode(&message, &payload);
    QCOMPARE(payload.target_system,
             quint8(fixture.lease.endpoint.systemId));
    QCOMPARE(payload.target_component,
             quint8(fixture.lease.endpoint.componentId));
    QCOMPARE(payload.command, quint16(request.command));
    QCOMPARE(payload.frame, quint8(request.frame));
    QCOMPARE(payload.param1, request.params[0]);
    QCOMPARE(payload.param2, request.params[1]);
    QCOMPARE(payload.param3, request.params[2]);
    QVERIFY(std::isnan(payload.param4));
    QCOMPARE(payload.x, request.x);
    QCOMPARE(payload.y, request.y);
    QCOMPARE(payload.z, request.z);
    QCOMPARE(payload.current, request.current);
    QCOMPARE(payload.autocontinue, request.autocontinue);

    fixture.service.observePhysicalMessage(
        fixture.lease.endpoint.linkId, fixture.lease.linkSessionEpoch,
        commandAck(fixture.lease, request.command));
}

void ExactCommandIntTest::retryKeepsIdenticalCommandPayload()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    auto request = commandIntRequest(10, 1000);
    request.maximumRetries = 1;
    QSignalSpy finished(
        &fixture.service, &VehicleCommandService::exactCommandFinished);

    QCOMPARE(fixture.service.submitExactCommandInt(
                 fixture.reservation, fixture.lease, request),
             VehicleCommandService::ExactSubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 2, 500);
    const mavlink_message_t first = decodeFrame(fixture.frames.at(0).bytes);
    const mavlink_message_t second = decodeFrame(fixture.frames.at(1).bytes);
    QCOMPARE(first.msgid, quint32(MAVLINK_MSG_ID_COMMAND_INT));
    QCOMPARE(second.msgid, quint32(MAVLINK_MSG_ID_COMMAND_INT));
    QCOMPARE(payloadBytes(second), payloadBytes(first));

    fixture.service.observePhysicalMessage(
        fixture.lease.endpoint.linkId, fixture.lease.linkSessionEpoch,
        commandAck(fixture.lease, request.command));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).transmissionAttempts, 2);
}

void ExactCommandIntTest::terminalAckAfterRetryQuarantinesBothWireForms_data()
{
    QTest::addColumn<bool>("intFirst");
    QTest::addColumn<int>("ackResult");
    QTest::newRow("INT-accepted") << true << int(MAV_RESULT_ACCEPTED);
    QTest::newRow("INT-rejected") << true << int(MAV_RESULT_DENIED);
    QTest::newRow("LONG-accepted") << false << int(MAV_RESULT_ACCEPTED);
    QTest::newRow("LONG-rejected") << false << int(MAV_RESULT_DENIED);
}

void ExactCommandIntTest::terminalAckAfterRetryQuarantinesBothWireForms()
{
    QFETCH(bool, intFirst);
    QFETCH(int, ackResult);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.service.setExactQuarantineForTesting(150);
    auto intRequest = commandIntRequest(20, 2000);
    intRequest.maximumRetries = 3;
    VehicleCommandService::ExactCommandRequest longRequest;
    longRequest.command = intRequest.command;
    longRequest.acknowledgementTimeoutMs = 20;
    longRequest.maximumLifetimeMs = 2000;
    longRequest.maximumRetries = 3;
    QSignalSpy finished(&fixture.service, &VehicleCommandService::exactCommandFinished);
    QCOMPARE(intFirst ? fixture.service.submitExactCommandInt(
        fixture.reservation, fixture.lease, intRequest)
        : fixture.service.submitExactCommandLong(fixture.reservation, fixture.lease, longRequest),
        VehicleCommandService::ExactSubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(fixture.frames.size() >= 2, 500);
    fixture.service.observePhysicalMessage(fixture.lease.endpoint.linkId,
        fixture.lease.linkSessionEpoch,
        commandAck(fixture.lease, intRequest.command, MAV_RESULT(ackResult)));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).terminalResult, ackResult == MAV_RESULT_ACCEPTED
        ? VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted
        : VehicleCommandService::ExactTerminalResult::AcknowledgedRejected);
    QVERIFY(reportAt(finished, 0).transmissionAttempts > 1);
    QVERIFY(fixture.service.isExactCommandQuarantined(fixture.lease, intRequest.command));
    QVERIFY(fixture.service.isExactEndpointBusy(fixture.lease.endpoint, intRequest.command));
    QCOMPARE(intFirst ? fixture.service.submitExactCommandLong(
        fixture.reservation, fixture.lease, longRequest)
        : fixture.service.submitExactCommandInt(fixture.reservation, fixture.lease, intRequest),
        VehicleCommandService::ExactSubmitResult::Quarantined);
    for (int index = 0; index < 2; ++index)
        fixture.service.observePhysicalMessage(fixture.lease.endpoint.linkId,
            fixture.lease.linkSessionEpoch, commandAck(fixture.lease, intRequest.command));
    QCOMPARE(finished.count(), 1); // Late duplicates cannot complete another command.
    QVERIFY(fixture.service.isExactCommandQuarantined(fixture.lease, intRequest.command));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.service.isExactCommandQuarantined(
        fixture.lease, intRequest.command), 1000);
}

void ExactCommandIntTest::longAndIntShareQuarantine_data()
{
    QTest::addColumn<bool>("intFirst");
    QTest::newRow("int-blocks-long") << true;
    QTest::newRow("long-blocks-int") << false;
}

void ExactCommandIntTest::longAndIntShareQuarantine()
{
    QFETCH(bool, intFirst);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.service.setExactQuarantineForTesting(1000);
    auto intRequest = commandIntRequest(5, 5);
    VehicleCommandService::ExactCommandRequest longRequest;
    longRequest.command = intRequest.command;
    longRequest.acknowledgementTimeoutMs = 5;
    longRequest.maximumLifetimeMs = 5;
    QSignalSpy finished(
        &fixture.service, &VehicleCommandService::exactCommandFinished);

    const auto first = intFirst
        ? fixture.service.submitExactCommandInt(
              fixture.reservation, fixture.lease, intRequest)
        : fixture.service.submitExactCommandLong(
              fixture.reservation, fixture.lease, longRequest);
    QCOMPARE(first, VehicleCommandService::ExactSubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 500);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 TimedOutOutcomeUncertain);
    QVERIFY(fixture.service.isExactCommandQuarantined(
        fixture.lease, intRequest.command));

    const auto second = intFirst
        ? fixture.service.submitExactCommandLong(
              fixture.reservation, fixture.lease, longRequest)
        : fixture.service.submitExactCommandInt(
              fixture.reservation, fixture.lease, intRequest);
    QCOMPARE(second, VehicleCommandService::ExactSubmitResult::Quarantined);
    QCOMPARE(fixture.frames.size(), 1);
}

void ExactCommandIntTest::postSigningCallbacksFailClosed()
{
    // The operation-specific gate runs once for admission and again only
    // after the transport has completed signing.
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        bool signedFrame = false;
        int safetyCalls = 0;
        fixture.transmitter.setFrameSigner(
            [&signedFrame](int, const QByteArray &frame, QByteArray *out) {
                signedFrame = true;
                *out = frame;
                return true;
            });
        auto request = commandIntRequest();
        request.validateBeforeWrite =
            [&signedFrame, &safetyCalls](QString *error) {
                ++safetyCalls;
                if (signedFrame) {
                    if (error) *error = QStringLiteral("Cancelled");
                    return false;
                }
                return true;
            };
        QSignalSpy finished(
            &fixture.service, &VehicleCommandService::exactCommandFinished);
        QCOMPARE(fixture.service.submitExactCommandInt(
                     fixture.reservation, fixture.lease, request),
                 VehicleCommandService::ExactSubmitResult::
                     ContextUnavailable);
        QCOMPARE(safetyCalls, 2);
        QCOMPARE(fixture.frames.size(), 0);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(reportAt(finished, 0).terminalResult,
                 VehicleCommandService::ExactTerminalResult::
                     RejectedBeforeTransmission);
        QVERIFY(!reportAt(finished, 0).frameAttempted);
    }

    // A signing callback may replace the physical epoch. The transmitter
    // rejects that signed context before the writer or ACK waiter can escape.
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.transmitter.setLinkSessionEpoch(
            fixture.lease.endpoint.linkId,
            fixture.lease.linkSessionEpoch);
        fixture.transmitter.setFrameSigner(
            [&fixture](int, const QByteArray &frame, QByteArray *out) {
                *out = frame;
                fixture.transmitter.setLinkSessionEpoch(
                    fixture.lease.endpoint.linkId,
                    fixture.lease.linkSessionEpoch + 1);
                return true;
            });
        QSignalSpy finished(
            &fixture.service, &VehicleCommandService::exactCommandFinished);
        QCOMPARE(fixture.service.submitExactCommandInt(
                     fixture.reservation, fixture.lease,
                     commandIntRequest()),
                 VehicleCommandService::ExactSubmitResult::
                     ContextUnavailable);
        QCOMPARE(fixture.frames.size(), 0);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(reportAt(finished, 0).terminalResult,
                 VehicleCommandService::ExactTerminalResult::
                     RejectedBeforeTransmission);
    }

    // The post-signing route callback is reentrant. Closing the reservation
    // from it must invalidate this frame without touching a successor route.
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        bool signedFrame = false;
        fixture.transmitter.setFrameSigner(
            [&signedFrame](int, const QByteArray &frame, QByteArray *out) {
                signedFrame = true;
                *out = frame;
                return true;
            });
        fixture.routeHook =
            [&fixture, &signedFrame](const SwarmVehicleInstanceLease &,
                                     QString *) {
                if (signedFrame) {
                    signedFrame = false;
                    fixture.service.releaseExactReservation(
                        fixture.reservation);
                }
                return true;
            };
        QSignalSpy finished(
            &fixture.service, &VehicleCommandService::exactCommandFinished);
        QCOMPARE(fixture.service.submitExactCommandInt(
                     fixture.reservation, fixture.lease,
                     commandIntRequest()),
                 VehicleCommandService::ExactSubmitResult::
                     ContextUnavailable);
        QCOMPARE(fixture.frames.size(), 0);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(reportAt(finished, 0).terminalResult,
                 VehicleCommandService::ExactTerminalResult::
                     RejectedBeforeTransmission);
    }
}

void ExactCommandIntTest::synchronousAckUsesInstalledWaiter()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.writeHook = [&fixture](int linkId, const QByteArray &bytes) {
        const mavlink_message_t outgoing = decodeFrame(bytes);
        if (outgoing.msgid == MAVLINK_MSG_ID_COMMAND_INT) {
            mavlink_command_int_t payload{};
            mavlink_msg_command_int_decode(&outgoing, &payload);
            fixture.service.observePhysicalMessage(
                linkId, fixture.lease.linkSessionEpoch,
                commandAck(fixture.lease, MAV_CMD(payload.command)));
        }
        return true;
    };
    QSignalSpy finished(
        &fixture.service, &VehicleCommandService::exactCommandFinished);
    VehicleCommandService::ExactCommandToken token;
    QCOMPARE(fixture.service.submitExactCommandInt(
                 fixture.reservation, fixture.lease,
                 commandIntRequest(), &token),
             VehicleCommandService::ExactSubmitResult::Started);
    QVERIFY(token.isValid());
    QCOMPARE(finished.count(), 1);
    const auto report = reportAt(finished, 0);
    QCOMPARE(report.token.transactionId, token.transactionId);
    QCOMPARE(report.terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedAccepted);
    QVERIFY(report.frameAttempted);
    QCOMPARE(report.transmissionAttempts, 1);
}

void ExactCommandIntTest::endpointReservationAndPendingLaneAreShared()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const VehicleTargetLease target = fixture.targets.acquireTarget();
    QVERIFY(target.isValid());

    // Merely holding the exact endpoint reservation excludes legacy LONG and
    // INT sends, even before an exact transaction is pending.
    QCOMPARE(fixture.service.sendCommandLong(
                 target, 250, 190, MAV_CMD_DO_REPOSITION, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::TransportUnavailable);
    QCOMPARE(fixture.service.sendCommandInt(
                 target, 250, 190, MAV_CMD_DO_REPOSITION,
                 MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::TransportUnavailable);
    QCOMPARE(fixture.frames.size(), 0);

    const auto intRequest = commandIntRequest();
    QCOMPARE(fixture.service.submitExactCommandInt(
                 fixture.reservation, fixture.lease, intRequest),
             VehicleCommandService::ExactSubmitResult::Started);
    VehicleCommandService::ExactCommandRequest longRequest;
    longRequest.command = MAV_CMD_NAV_GUIDED_ENABLE;
    QCOMPARE(fixture.service.submitExactCommandLong(
                 fixture.reservation, fixture.lease, longRequest),
             VehicleCommandService::ExactSubmitResult::Busy);
    QCOMPARE(fixture.frames.size(), 1);

    fixture.service.observePhysicalMessage(
        fixture.lease.endpoint.linkId, fixture.lease.linkSessionEpoch,
        commandAck(fixture.lease, intRequest.command));
    QCOMPARE(fixture.service.submitExactCommandLong(
                 fixture.reservation, fixture.lease, longRequest),
             VehicleCommandService::ExactSubmitResult::Started);
    QCOMPARE(fixture.frames.size(), 2);
    fixture.service.observePhysicalMessage(
        fixture.lease.endpoint.linkId, fixture.lease.linkSessionEpoch,
        commandAck(fixture.lease, longRequest.command));
}

QTEST_GUILESS_MAIN(ExactCommandIntTest)

#include "test_exactcommandint.moc"
