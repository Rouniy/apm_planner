#include "comm/MAVLinkFrameParser.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"

#include <QtTest>

#include <algorithm>

namespace
{

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("Link %1").arg(linkId);
    result.componentName = QStringLiteral("Component %1").arg(componentId);
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
    if (state != MAVLINK_FRAMING_OK) {
        return {};
    }
    return message;
}

mavlink_message_t commandAck(
    int sourceSystem, int sourceComponent, MAV_CMD command,
    MAV_RESULT result, int targetSystem = 250, int targetComponent = 190,
    int progress = 255)
{
    mavlink_command_ack_t payload{};
    payload.command = static_cast<quint16>(command);
    payload.result = static_cast<quint8>(result);
    payload.progress = static_cast<quint8>(progress);
    payload.target_system = static_cast<quint8>(targetSystem);
    payload.target_component = static_cast<quint8>(targetComponent);
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(
        static_cast<quint8>(sourceSystem),
        static_cast<quint8>(sourceComponent), &message, &payload);
    return message;
}

SwarmVehicleInstanceLease swarmLease(
    int linkId, int systemId, int componentId = 1,
    quint64 linkSessionEpoch = 1, quint64 instanceEpoch = 1)
{
    SwarmVehicleInstanceLease lease;
    lease.endpoint = endpoint(linkId, systemId, componentId);
    lease.linkSessionEpoch = linkSessionEpoch;
    lease.instanceEpoch = instanceEpoch;
    return lease;
}

bool containsLease(
    const QList<SwarmVehicleInstanceLease> &active,
    const SwarmVehicleInstanceLease &lease)
{
    return std::any_of(
        active.cbegin(), active.cend(),
        [&lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        });
}

VehicleCommandService::ExactCommandRequest exactRequest(
    MAV_CMD command, int timeoutMs = 0)
{
    VehicleCommandService::ExactCommandRequest request;
    request.command = command;
    request.acknowledgementTimeoutMs = timeoutMs;
    return request;
}

VehicleCommandService::ExactCommandReport reportAt(
    const QSignalSpy &spy, int index)
{
    return qvariant_cast<VehicleCommandService::ExactCommandReport>(
        spy.at(index).at(0));
}

} // namespace

class VehicleCommandServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void commandLongUsesOnlyTheExactSelectedEndpoint();
    void currentCommandUsesConfiguredIdentityAndExactTarget();
    void commandIntPreservesCoordinatePayload();
    void invalidStaleAndUnavailableTargetsFailClosed();
    void transmitVersionAndSequenceAreIndependentPerLink();
    void acknowledgementRequiresExactLeaseEnvelopeAndCommand();
    void targetChangeInvalidatesPendingAcknowledgement();
    void exactReservationsAllowParallelDuplicateIdsAndOutOfOrderAcks();
    void exactAcknowledgementsAcceptZeroTargetsAndExtendInProgress();
    void exactWaiterExistsBeforeSynchronousWriterAcknowledgement();
    void exactEndpointReservationAndPendingCommandAreExclusive();
    void exactRouteCallbackRetirementFailsBeforeWriter();
    void exactTimeoutQuarantinesAndConsumesLateAcknowledgement();
    void exactOwnerDetachAndLeaseRetirementDrainSafely();
    void exactWriterFailureIsTerminalOutcomeUncertain();
};

void VehicleCommandServiceTest::commandLongUsesOnlyTheExactSelectedEndpoint()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);

    QVERIFY(targets.observeEndpoint(endpoint(4, 42, 1), true));
    QVERIFY(targets.observeEndpoint(endpoint(9, 42, 100)));
    QVERIFY(targets.selectTarget(9, 42, 100));
    const VehicleTargetLease lease = targets.acquireTarget();

    QCOMPARE(service.sendCommandLong(
                 lease, 250, 190, MAV_CMD_DO_CHANGE_SPEED, 3,
                 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F),
             VehicleCommandService::SendResult::Sent);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().linkId, 9);

    const mavlink_message_t message = decodeFrame(frames.first().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_COMMAND_LONG));
    QCOMPARE(message.sysid, quint8(250));
    QCOMPARE(message.compid, quint8(190));
    mavlink_command_long_t payload{};
    mavlink_msg_command_long_decode(&message, &payload);
    QCOMPARE(payload.target_system, quint8(42));
    QCOMPARE(payload.target_component, quint8(100));
    QCOMPARE(payload.command, quint16(MAV_CMD_DO_CHANGE_SPEED));
    QCOMPARE(payload.confirmation, quint8(3));
    QCOMPARE(payload.param1, 1.0F);
    QCOMPARE(payload.param7, 7.0F);
}

void VehicleCommandServiceTest::currentCommandUsesConfiguredIdentityAndExactTarget()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(251, 190);
    QVERIFY(targets.observeEndpoint(endpoint(17, 77, 42), true));

    QCOMPARE(service.sendCurrentCommandLong(
                 MAV_CMD_PREFLIGHT_STORAGE, 0,
                 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F),
             int(VehicleCommandService::SendResult::Sent));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.constFirst().linkId, 17);
    const mavlink_message_t message = decodeFrame(frames.constFirst().bytes);
    QCOMPARE(message.sysid, quint8(251));
    QCOMPARE(message.compid, quint8(190));
    mavlink_command_long_t payload{};
    mavlink_msg_command_long_decode(&message, &payload);
    QCOMPARE(payload.target_system, quint8(77));
    QCOMPARE(payload.target_component, quint8(42));
    QCOMPARE(payload.command, quint16(MAV_CMD_PREFLIGHT_STORAGE));
    QCOMPARE(payload.param1, 1.0F);
}

void VehicleCommandServiceTest::commandIntPreservesCoordinatePayload()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(7, 71, 154), true));

    QCOMPARE(service.sendCommandInt(
                 targets.acquireTarget(), 251, 190,
                 MAV_CMD_DO_SET_ROI_LOCATION,
                 MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
                 1.0F, 2.0F, 3.0F, 4.0F,
                 -353632610, 1491652300, 85.5F, 1, 1),
             VehicleCommandService::SendResult::Sent);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().linkId, 7);

    const mavlink_message_t message = decodeFrame(frames.first().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_COMMAND_INT));
    mavlink_command_int_t payload{};
    mavlink_msg_command_int_decode(&message, &payload);
    QCOMPARE(payload.target_system, quint8(71));
    QCOMPARE(payload.target_component, quint8(154));
    QCOMPARE(payload.frame,
             quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT_INT));
    QCOMPARE(payload.x, qint32(-353632610));
    QCOMPARE(payload.y, qint32(1491652300));
    QCOMPARE(payload.z, 85.5F);
    QCOMPARE(payload.current, quint8(1));
    QCOMPARE(payload.autocontinue, quint8(1));
}

void VehicleCommandServiceTest::invalidStaleAndUnavailableTargetsFailClosed()
{
    VehicleTargetManager targets;
    int writes = 0;
    bool writerSucceeds = true;
    ExactLinkTransmitter transmitter(
        [&writes, &writerSucceeds](int, const QByteArray &) {
            ++writes;
            return writerSucceeds;
        });
    VehicleCommandService service(&targets, &transmitter);

    QCOMPARE(service.sendCommandLong(
                 {}, 250, 190, MAV_CMD_NAV_RETURN_TO_LAUNCH, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::InvalidTarget);
    QCOMPARE(writes, 0);

    QVERIFY(targets.observeEndpoint(endpoint(1), true));
    QVERIFY(targets.observeEndpoint(endpoint(2)));
    const VehicleTargetLease stale = targets.acquireTarget();
    QVERIFY(targets.selectTarget(2, 42, 1));
    QCOMPARE(service.sendCommandLong(
                 stale, 250, 190, MAV_CMD_NAV_RETURN_TO_LAUNCH, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::StaleTarget);
    QCOMPARE(writes, 0);

    writerSucceeds = false;
    QCOMPARE(service.sendCommandLong(
                 targets.acquireTarget(), 250, 190,
                 MAV_CMD_NAV_RETURN_TO_LAUNCH, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::TransportUnavailable);
    QCOMPARE(writes, 1);

    QSignalSpy acknowledgements(
        &service, &VehicleCommandService::commandAckReceived);
    service.observeMessage(
        2, commandAck(42, 1, MAV_CMD_NAV_RETURN_TO_LAUNCH,
                      MAV_RESULT_ACCEPTED));
    QCOMPARE(acknowledgements.count(), 0);
}

void VehicleCommandServiceTest::transmitVersionAndSequenceAreIndependentPerLink()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(11), true));
    QVERIFY(targets.observeEndpoint(endpoint(22)));
    transmitter.setOutboundVersion(11, 1);
    transmitter.setOutboundVersion(22, 2);

    const auto send = [&service, &targets](MAV_CMD command) {
        return service.sendCommandLong(
            targets.acquireTarget(), 250, 190, command, 0,
            0, 0, 0, 0, 0, 0, 0);
    };
    QCOMPARE(send(MAV_CMD_NAV_RETURN_TO_LAUNCH),
             VehicleCommandService::SendResult::Sent);
    QVERIFY(targets.selectTarget(22, 42, 1));
    QCOMPARE(send(MAV_CMD_MISSION_START),
             VehicleCommandService::SendResult::Sent);
    QVERIFY(targets.selectTarget(11, 42, 1));
    QCOMPARE(send(MAV_CMD_DO_CHANGE_SPEED),
             VehicleCommandService::SendResult::Sent);

    QCOMPARE(frames.size(), 3);
    const mavlink_message_t first = decodeFrame(frames.at(0).bytes);
    const mavlink_message_t second = decodeFrame(frames.at(1).bytes);
    const mavlink_message_t third = decodeFrame(frames.at(2).bytes);
    QCOMPARE(first.magic, quint8(MAVLINK_STX_MAVLINK1));
    QCOMPARE(second.magic, quint8(MAVLINK_STX));
    QCOMPARE(third.magic, quint8(MAVLINK_STX_MAVLINK1));
    QCOMPARE(first.seq, quint8(0));
    QCOMPARE(second.seq, quint8(0));
    QCOMPARE(third.seq, quint8(1));

    service.forgetLink(11);
    transmitter.forgetLink(11);
    transmitter.setOutboundVersion(11, 2);
    QCOMPARE(send(MAV_CMD_DO_GO_AROUND),
             VehicleCommandService::SendResult::Sent);
    const mavlink_message_t reset = decodeFrame(frames.last().bytes);
    QCOMPARE(reset.magic, quint8(MAVLINK_STX));
    QCOMPARE(reset.seq, quint8(0));
}

void VehicleCommandServiceTest::acknowledgementRequiresExactLeaseEnvelopeAndCommand()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    VehicleCommandService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(9, 42, 100), true));
    const VehicleTargetLease lease = targets.acquireTarget();
    QCOMPARE(service.sendCommandLong(
                 lease, 250, 190, MAV_CMD_DO_CHANGE_SPEED, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::Sent);

    QSignalSpy acknowledgements(
        &service, &VehicleCommandService::commandAckReceived);
    service.observeMessage(
        8, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED));
    service.observeMessage(
        9, commandAck(41, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED));
    service.observeMessage(
        9, commandAck(42, 1, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED));
    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_MISSION_START,
                      MAV_RESULT_ACCEPTED));
    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED, 249, 190));
    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED, 250, 191));
    QCOMPARE(acknowledgements.count(), 0);

    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_IN_PROGRESS, 250, 190, 37));
    QCOMPARE(acknowledgements.count(), 1);
    QCOMPARE(acknowledgements.first().at(0).toULongLong(),
             lease.generation);
    QCOMPARE(acknowledgements.first().at(1).toInt(), 9);
    QCOMPARE(acknowledgements.first().at(2).toInt(), 42);
    QCOMPARE(acknowledgements.first().at(3).toInt(), 100);
    QCOMPARE(acknowledgements.first().at(4).toInt(),
             int(MAV_CMD_DO_CHANGE_SPEED));
    QCOMPARE(acknowledgements.first().at(6).toInt(), 37);

    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED, 0, 0));
    QCOMPARE(acknowledgements.count(), 2);
    service.observeMessage(
        9, commandAck(42, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED, 0, 0));
    QCOMPARE(acknowledgements.count(), 2);
}

void VehicleCommandServiceTest::targetChangeInvalidatesPendingAcknowledgement()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    VehicleCommandService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(3), true));
    QVERIFY(targets.observeEndpoint(endpoint(4)));
    const VehicleTargetLease oldTarget = targets.acquireTarget();
    QCOMPARE(service.sendCommandLong(
                 oldTarget, 250, 190, MAV_CMD_MISSION_START, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::Sent);

    QVERIFY(targets.selectTarget(4, 42, 1));
    QVERIFY(targets.selectTarget(3, 42, 1));
    QSignalSpy acknowledgements(
        &service, &VehicleCommandService::commandAckReceived);
    service.observeMessage(
        3, commandAck(42, 1, MAV_CMD_MISSION_START,
                      MAV_RESULT_ACCEPTED));
    QCOMPARE(acknowledgements.count(), 0);
}

void VehicleCommandServiceTest::
exactReservationsAllowParallelDuplicateIdsAndOutOfOrderAcks()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(250, 190);

    const SwarmVehicleInstanceLease first = swarmLease(10, 42);
    const SwarmVehicleInstanceLease second = swarmLease(20, 42);
    QList<SwarmVehicleInstanceLease> active{first, second};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &lease) {
            return containsLease(active, lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));

    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);

    QSignalSpy finished(
        &service, &VehicleCommandService::exactCommandFinished);
    VehicleCommandService::ExactCommandToken firstToken;
    VehicleCommandService::ExactCommandToken secondToken;
    const auto request = exactRequest(MAV_CMD_COMPONENT_ARM_DISARM);
    QCOMPARE(service.submitExactCommandLong(
                 reservation, first, request, &firstToken),
             VehicleCommandService::ExactSubmitResult::Started);
    QCOMPARE(service.submitExactCommandLong(
                 reservation, second, request, &secondToken),
             VehicleCommandService::ExactSubmitResult::Started);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0).linkId, 10);
    QCOMPARE(frames.at(1).linkId, 20);

    // Same sysid on a foreign link, a foreign component, and another command
    // must not complete either exact transaction.
    service.observeMessage(
        30, commandAck(42, 1, MAV_CMD_COMPONENT_ARM_DISARM,
                       MAV_RESULT_ACCEPTED));
    service.observeMessage(
        10, commandAck(42, 2, MAV_CMD_COMPONENT_ARM_DISARM,
                       MAV_RESULT_ACCEPTED));
    service.observeMessage(
        10, commandAck(42, 1, MAV_CMD_NAV_TAKEOFF,
                       MAV_RESULT_ACCEPTED));
    QCOMPARE(finished.count(), 0);

    // Parallel endpoints are correlated independently, so terminal ACK order
    // is not required to match submission order.
    service.observeMessage(
        20, commandAck(42, 1, MAV_CMD_COMPONENT_ARM_DISARM,
                       MAV_RESULT_ACCEPTED));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).token.transactionId,
             secondToken.transactionId);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedAccepted);

    service.observeMessage(
        10, commandAck(42, 1, MAV_CMD_COMPONENT_ARM_DISARM,
                       MAV_RESULT_DENIED));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(reportAt(finished, 1).token.transactionId,
             firstToken.transactionId);
    QCOMPARE(reportAt(finished, 1).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedRejected);
    QCOMPARE(reportAt(finished, 1).mavResult, int(MAV_RESULT_DENIED));
}

void VehicleCommandServiceTest::
exactAcknowledgementsAcceptZeroTargetsAndExtendInProgress()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(250, 190);
    const SwarmVehicleInstanceLease lease = swarmLease(7, 71, 100);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));

    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QSignalSpy progress(
        &service, &VehicleCommandService::exactCommandProgress);
    QSignalSpy finished(
        &service, &VehicleCommandService::exactCommandFinished);

    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_DO_CHANGE_SPEED, 100)),
             VehicleCommandService::ExactSubmitResult::Started);
    service.observeMessage(
        7, commandAck(71, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_IN_PROGRESS, 249, 190, 12));
    QCOMPARE(progress.count(), 0);
    QCOMPARE(finished.count(), 0);

    service.observeMessage(
        7, commandAck(71, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_IN_PROGRESS, 250, 190, 37));
    QCOMPARE(progress.count(), 1);
    QCOMPARE(progress.first().at(2).toInt(), 37);
    QCOMPARE(finished.count(), 0);

    // Zero target extensions are valid for MAVLink 1/older implementations.
    service.observeMessage(
        7, commandAck(71, 100, MAV_CMD_DO_CHANGE_SPEED,
                      MAV_RESULT_ACCEPTED, 0, 0));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedAccepted);
    QCOMPARE(reportAt(finished, 0).acknowledgementTargetSystem, 0);

    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             VehicleCommandService::ExactSubmitResult::Started);
    service.observeMessage(
        7, commandAck(71, 100, MAV_CMD_NAV_RETURN_TO_LAUNCH,
                      MAV_RESULT_ACCEPTED, 250, 190));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(reportAt(finished, 1).acknowledgementTargetSystem, 250);
    QCOMPARE(reportAt(finished, 1).acknowledgementTargetComponent, 190);
}

void VehicleCommandServiceTest::
exactWaiterExistsBeforeSynchronousWriterAcknowledgement()
{
    VehicleTargetManager targets;
    VehicleCommandService *servicePointer = nullptr;
    ExactLinkTransmitter transmitter(
        [&servicePointer](int linkId, const QByteArray &bytes) {
            const mavlink_message_t message = decodeFrame(bytes);
            mavlink_command_long_t command{};
            mavlink_msg_command_long_decode(&message, &command);
            servicePointer->observeMessage(
                linkId,
                commandAck(command.target_system,
                           command.target_component,
                           static_cast<MAV_CMD>(command.command),
                           MAV_RESULT_ACCEPTED));
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    servicePointer = &service;
    service.setLocalIdentity(250, 190);
    const SwarmVehicleInstanceLease lease = swarmLease(8, 81, 42);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));

    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &VehicleCommandService::exactCommandFinished);
    VehicleCommandService::ExactCommandToken token;
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_NAV_GUIDED_ENABLE), &token),
             VehicleCommandService::ExactSubmitResult::Started);
    QVERIFY(token.isValid());
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).token.transactionId,
             token.transactionId);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedAccepted);
}

void VehicleCommandServiceTest::
exactEndpointReservationAndPendingCommandAreExclusive()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(250, 190);
    const SwarmVehicleInstanceLease lease = swarmLease(9, 42);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QVERIFY(targets.observeEndpoint(lease.endpoint, true));

    QObject firstOwner;
    QObject secondOwner;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &firstOwner, active, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    VehicleCommandService::ExactReservationToken rejectedReservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &secondOwner, active, &rejectedReservation),
             VehicleCommandService::ExactReservationResult::Busy);

    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_COMPONENT_ARM_DISARM)),
             VehicleCommandService::ExactSubmitResult::Started);
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_NAV_TAKEOFF)),
             VehicleCommandService::ExactSubmitResult::Busy);
    QCOMPARE(frames.size(), 1);

    // The compatible selected-target API remains available but fails closed
    // while the endpoint is owned by an exact reservation.
    QCOMPARE(service.sendCommandLong(
                 targets.acquireTarget(), 250, 190,
                 MAV_CMD_NAV_RETURN_TO_LAUNCH, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::TransportUnavailable);
    QCOMPARE(frames.size(), 1);

    service.observeMessage(
        9, commandAck(42, 1, MAV_CMD_COMPONENT_ARM_DISARM,
                      MAV_RESULT_ACCEPTED));
    QSignalSpy released(
        &service, &VehicleCommandService::exactReservationReleased);
    QVERIFY(service.releaseExactReservation(reservation));
    QCOMPARE(released.count(), 1);
    QCOMPARE(service.reserveExactEndpoints(
                 &secondOwner, active, &rejectedReservation),
             VehicleCommandService::ExactReservationResult::Reserved);
}

void VehicleCommandServiceTest::
exactRouteCallbackRetirementFailsBeforeWriter()
{
    VehicleTargetManager targets;
    int writes = 0;
    ExactLinkTransmitter transmitter(
        [&writes](int, const QByteArray &) {
            ++writes;
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    const SwarmVehicleInstanceLease lease = swarmLease(11, 42);
    QList<SwarmVehicleInstanceLease> active{lease};
    bool retireDuringRoute = false;
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [&service, &active, &retireDuringRoute](
            const SwarmVehicleInstanceLease &candidate, QString *) {
            if (retireDuringRoute) {
                active.clear();
                service.retireExactVehicle(candidate);
            }
            return true;
        }));

    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &VehicleCommandService::exactCommandFinished);
    retireDuringRoute = true;
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             VehicleCommandService::ExactSubmitResult::InvalidReservation);
    QCOMPARE(writes, 0);
    QCOMPARE(finished.count(), 0);
}

void VehicleCommandServiceTest::
exactTimeoutQuarantinesAndConsumesLateAcknowledgement()
{
    VehicleTargetManager targets;
    int writes = 0;
    ExactLinkTransmitter transmitter(
        [&writes](int, const QByteArray &) {
            ++writes;
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(250, 190);
    service.setExactCommandTimeoutForTesting(20);
    service.setExactQuarantineForTesting(500);
    const SwarmVehicleInstanceLease lease = swarmLease(12, 42);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));

    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QSignalSpy progress(
        &service, &VehicleCommandService::exactCommandProgress);
    QSignalSpy finished(
        &service, &VehicleCommandService::exactCommandFinished);
    const auto request = exactRequest(MAV_CMD_NAV_GUIDED_ENABLE);
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease, request),
             VehicleCommandService::ExactSubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 250);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 TimedOutOutcomeUncertain);
    QVERIFY(reportAt(finished, 0).frameAttempted);
    QVERIFY(service.isExactCommandQuarantined(
        lease, MAV_CMD_NAV_GUIDED_ENABLE));

    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease, request),
             VehicleCommandService::ExactSubmitResult::Quarantined);
    QCOMPARE(writes, 1);

    // Late progress and terminal ACKs are consumed by quarantine and never
    // forwarded to a later transaction. Terminal evidence drains it early.
    service.observeMessage(
        12, commandAck(42, 1, MAV_CMD_NAV_GUIDED_ENABLE,
                       MAV_RESULT_IN_PROGRESS));
    QCOMPARE(progress.count(), 0);
    service.observeMessage(
        12, commandAck(42, 1, MAV_CMD_NAV_GUIDED_ENABLE,
                       MAV_RESULT_ACCEPTED));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!service.isExactCommandQuarantined(
        lease, MAV_CMD_NAV_GUIDED_ENABLE));

    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease, request),
             VehicleCommandService::ExactSubmitResult::Started);
    QCOMPARE(writes, 2);
    service.observeMessage(
        12, commandAck(42, 1, MAV_CMD_NAV_GUIDED_ENABLE,
                       MAV_RESULT_ACCEPTED));
    QCOMPARE(finished.count(), 2);
}

void VehicleCommandServiceTest::
exactOwnerDetachAndLeaseRetirementDrainSafely()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(250, 190);
    service.setExactQuarantineForTesting(500);
    const SwarmVehicleInstanceLease first = swarmLease(14, 42, 1, 1, 1);
    QList<SwarmVehicleInstanceLease> active{first};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));

    QSignalSpy finished(
        &service, &VehicleCommandService::exactCommandFinished);
    QSignalSpy released(
        &service, &VehicleCommandService::exactReservationReleased);
    auto *firstOwner = new QObject;
    VehicleCommandService::ExactReservationToken firstReservation;
    QCOMPARE(service.reserveExactEndpoints(
                 firstOwner, active, &firstReservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QCOMPARE(service.submitExactCommandLong(
                 firstReservation, first,
                 exactRequest(MAV_CMD_COMPONENT_ARM_DISARM)),
             VehicleCommandService::ExactSubmitResult::Started);
    delete firstOwner;
    QCOMPARE(released.count(), 0);

    service.observeMessage(
        14, commandAck(42, 1, MAV_CMD_COMPONENT_ARM_DISARM,
                       MAV_RESULT_ACCEPTED));
    QCOMPARE(finished.count(), 1);
    QVERIFY(reportAt(finished, 0).ownerDetached);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedAccepted);
    QCOMPARE(released.count(), 1);

    const SwarmVehicleInstanceLease replacement =
        swarmLease(14, 42, 1, 1, 2);
    active = {replacement};
    QObject secondOwner;
    VehicleCommandService::ExactReservationToken secondReservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &secondOwner, active, &secondReservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QCOMPARE(service.submitExactCommandLong(
                 secondReservation, replacement,
                 exactRequest(MAV_CMD_NAV_TAKEOFF)),
             VehicleCommandService::ExactSubmitResult::Started);
    active.clear();
    service.retireExactVehicle(replacement);
    QCOMPARE(finished.count(), 2);
    QCOMPARE(reportAt(finished, 1).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 LeaseRetiredOutcomeUncertain);
    QCOMPARE(released.count(), 2);

    // A rediscovered instance may reserve the endpoint, but the same wire
    // command remains quarantined because COMMAND_ACK carries no epoch.
    const SwarmVehicleInstanceLease reconnected =
        swarmLease(14, 42, 1, 2, 3);
    active = {reconnected};
    QObject thirdOwner;
    VehicleCommandService::ExactReservationToken thirdReservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &thirdOwner, active, &thirdReservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QCOMPARE(service.submitExactCommandLong(
                 thirdReservation, reconnected,
                 exactRequest(MAV_CMD_NAV_TAKEOFF)),
             VehicleCommandService::ExactSubmitResult::Quarantined);
    QCOMPARE(service.submitExactCommandLong(
                 thirdReservation, reconnected,
                 exactRequest(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             VehicleCommandService::ExactSubmitResult::Started);
    service.forgetLink(14);
    QCOMPARE(finished.count(), 3);
    QCOMPARE(reportAt(finished, 2).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 LinkForgottenOutcomeUncertain);
    QCOMPARE(released.count(), 3);
}

void VehicleCommandServiceTest::
exactWriterFailureIsTerminalOutcomeUncertain()
{
    VehicleTargetManager targets;
    int writeAttempts = 0;
    ExactLinkTransmitter transmitter(
        [&writeAttempts](int, const QByteArray &) {
            ++writeAttempts;
            return false;
        });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(250, 190);
    const SwarmVehicleInstanceLease lease = swarmLease(16, 42);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));

    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &VehicleCommandService::exactCommandFinished);
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             VehicleCommandService::ExactSubmitResult::
                 TransportOutcomeUncertain);
    QCOMPARE(writeAttempts, 1);
    QCOMPARE(finished.count(), 1);
    const auto report = reportAt(finished, 0);
    QCOMPARE(report.terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 TransportOutcomeUncertain);
    QVERIFY(report.frameAttempted);
    QVERIFY(service.isExactCommandQuarantined(
        lease, MAV_CMD_NAV_RETURN_TO_LAUNCH));
}

QTEST_GUILESS_MAIN(VehicleCommandServiceTest)

#include "test_vehiclecommandservice.moc"
