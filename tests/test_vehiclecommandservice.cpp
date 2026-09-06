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
    MAV_CMD command, int timeoutMs = 0, int maximumLifetimeMs = 0)
{
    VehicleCommandService::ExactCommandRequest request;
    request.command = command;
    request.acknowledgementTimeoutMs = timeoutMs;
    request.maximumLifetimeMs = maximumLifetimeMs;
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
    void exactProgressExtendsInactivityWithinAbsoluteLifetime();
    void exactContinuousProgressCannotExtendAbsoluteLifetime();
    void exactWaiterExistsBeforeSynchronousWriterAcknowledgement();
    void exactEndpointReservationAndPendingCommandAreExclusive();
    void exactRouteCallbackRetirementFailsBeforeWriter();
    void exactTimeoutQuarantinesAndConsumesLateAcknowledgement();
    void exactOwnerDetachAndLeaseRetirementDrainSafely();
    void exactWriterFailureIsTerminalOutcomeUncertain();
    void exactSigningRejectionBeforeWriterIsDefinite();
    void singleVehicleReservationUsesDedicatedRouteAndExactAck();
    void singleVehicleSelectionAbaAndSwarmIsolation();
    void singleVehicleCallbacksFailClosedBeforeWriter();
    void singleVehicleOwnerDetachAndTargetRetirementDrainSafely();
    void terminalCallbacksMayDeleteCommandService_data();
    void terminalCallbacksMayDeleteCommandService();
    void idleReservationReleaseMayDeleteCommandService_data();
    void idleReservationReleaseMayDeleteCommandService();
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
exactProgressExtendsInactivityWithinAbsoluteLifetime()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(250, 190);
    const SwarmVehicleInstanceLease lease = swarmLease(27, 71, 100);
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
                 exactRequest(MAV_CMD_DO_CHANGE_SPEED, 80, 300)),
             VehicleCommandService::ExactSubmitResult::Started);
    QTest::qWait(50);
    service.observeMessage(
        27, commandAck(71, 100, MAV_CMD_DO_CHANGE_SPEED,
                       MAV_RESULT_IN_PROGRESS, 250, 190, 42));
    QCOMPARE(progress.count(), 1);
    QCOMPARE(finished.count(), 0);

    // This is beyond the original 80 ms inactivity deadline but remains
    // inside both the renewed inactivity window and immutable 300 ms bound.
    QTest::qWait(50);
    QCOMPARE(finished.count(), 0);
    service.observeMessage(
        27, commandAck(71, 100, MAV_CMD_DO_CHANGE_SPEED,
                       MAV_RESULT_ACCEPTED));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedAccepted);
}

void VehicleCommandServiceTest::
exactContinuousProgressCannotExtendAbsoluteLifetime()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(250, 190);
    service.setExactQuarantineForTesting(500);
    const SwarmVehicleInstanceLease lease = swarmLease(28, 72, 101);
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
    QSignalSpy released(
        &service, &VehicleCommandService::exactReservationReleased);

    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_NAV_GUIDED_ENABLE, 50, 160)),
             VehicleCommandService::ExactSubmitResult::Started);
    QVERIFY(service.releaseExactReservation(reservation));
    QCOMPARE(released.count(), 0);
    QElapsedTimer elapsed;
    elapsed.start();
    while (finished.isEmpty() && elapsed.elapsed() < 400) {
        QTest::qWait(15);
        if (finished.isEmpty()) {
            service.observeMessage(
                28, commandAck(72, 101, MAV_CMD_NAV_GUIDED_ENABLE,
                               MAV_RESULT_IN_PROGRESS, 250, 190, 50));
        }
    }

    QCOMPARE(finished.count(), 1);
    QVERIFY(progress.count() >= 2);
    const VehicleCommandService::ExactCommandReport report =
        reportAt(finished, 0);
    QCOMPARE(report.terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 TimedOutOutcomeUncertain);
    QVERIFY(report.frameAttempted);
    QVERIFY(report.description.contains(
        QStringLiteral("maximum lifetime"), Qt::CaseInsensitive));
    QVERIFY(service.isExactCommandQuarantined(
        lease, MAV_CMD_NAV_GUIDED_ENABLE));
    QCOMPARE(released.count(), 1);
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

    // Reservation ownership alone, before an exact command becomes pending,
    // excludes every legacy command on the same endpoint.
    QCOMPARE(service.sendCommandLong(
                 targets.acquireTarget(), 250, 190,
                 MAV_CMD_NAV_RETURN_TO_LAUNCH, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::TransportUnavailable);
    QCOMPARE(frames.size(), 0);

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

void VehicleCommandServiceTest::
exactSigningRejectionBeforeWriterIsDefinite()
{
    VehicleTargetManager targets;
    int writes = 0;
    ExactLinkTransmitter transmitter(
        [&writes](int, const QByteArray &) {
            ++writes;
            return true;
        });
    transmitter.setFrameSigner(
        [](int, const QByteArray &, QByteArray *) { return false; });
    VehicleCommandService service(&targets, &transmitter);
    service.setLocalIdentity(250, 190);
    const SwarmVehicleInstanceLease lease = swarmLease(17, 42);
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
    const auto request = exactRequest(MAV_CMD_NAV_RETURN_TO_LAUNCH);
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease, request),
             VehicleCommandService::ExactSubmitResult::ContextUnavailable);
    QCOMPARE(writes, 0);
    QCOMPARE(finished.count(), 1);
    const auto rejected = reportAt(finished, 0);
    QCOMPARE(rejected.terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 RejectedBeforeTransmission);
    QVERIFY(!rejected.frameAttempted);
    QVERIFY(!service.isExactCommandQuarantined(
        lease, MAV_CMD_NAV_RETURN_TO_LAUNCH));

    transmitter.setFrameSigner(ExactLinkTransmitter::FrameSigner());
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease, request),
             VehicleCommandService::ExactSubmitResult::Started);
    QCOMPARE(writes, 1);
    service.observeMessage(
        lease.endpoint.linkId,
        commandAck(lease.endpoint.systemId, lease.endpoint.componentId,
                   MAV_CMD_NAV_RETURN_TO_LAUNCH, MAV_RESULT_ACCEPTED));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(reportAt(finished, 1).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedAccepted);
}

void VehicleCommandServiceTest::
singleVehicleReservationUsesDedicatedRouteAndExactAck()
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
    const SwarmVehicleInstanceLease lease =
        swarmLease(51, 73, 1, 9, 17);
    QList<SwarmVehicleInstanceLease> active{lease};
    int swarmRouteCalls = 0;
    int singleRouteCalls = 0;
    bool attemptReconfigure = false;
    bool reconfigureResult = true;
    bool safeToWrite = true;
    bool becomeUnsafeInRoute = false;
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [&swarmRouteCalls](const SwarmVehicleInstanceLease &, QString *) {
            ++swarmRouteCalls;
            return false;
        }));
    QVERIFY(service.configureSingleVehicleExactRoute(
        [&service, &singleRouteCalls, &attemptReconfigure,
         &reconfigureResult, &safeToWrite, &becomeUnsafeInRoute](
            const SwarmVehicleInstanceLease &, QString *) {
            ++singleRouteCalls;
            if (attemptReconfigure) {
                attemptReconfigure = false;
                reconfigureResult =
                    service.configureSingleVehicleExactRoute(
                        [](const SwarmVehicleInstanceLease &, QString *) {
                            return false;
                        });
            }
            if (becomeUnsafeInRoute) {
                becomeUnsafeInRoute = false;
                safeToWrite = false;
            }
            return true;
        }));
    QVERIFY(targets.observeEndpoint(lease.endpoint, true));
    const VehicleTargetLease target = targets.acquireTarget();
    QVERIFY(target.isValid());
    QVERIFY(targets.isTargetGenerationSettled());

    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    attemptReconfigure = true;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, target, lease, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QVERIFY(!reconfigureResult);
    QCOMPARE(swarmRouteCalls, 0);
    QVERIFY(singleRouteCalls >= 1);

    auto routeChangedSafety =
        exactRequest(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN);
    routeChangedSafety.validateBeforeWrite =
        [&safeToWrite](QString *) { return safeToWrite; };
    becomeUnsafeInRoute = true;
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease, routeChangedSafety),
             VehicleCommandService::ExactSubmitResult::RouteUnavailable);
    QCOMPARE(frames.size(), 0);
    safeToWrite = true;

    int safetyCalls = 0;
    auto request = exactRequest(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN);
    request.params = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    request.validateBeforeWrite = [&safetyCalls](QString *) {
        ++safetyCalls;
        return true;
    };
    QSignalSpy finished(
        &service, &VehicleCommandService::exactCommandFinished);
    VehicleCommandService::ExactCommandToken commandToken;
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease, request, &commandToken),
             VehicleCommandService::ExactSubmitResult::Started);
    QVERIFY(commandToken.isValid());
    QCOMPARE(safetyCalls, 2); // Admission and final post-signing/pre-writer gate.
    QCOMPARE(swarmRouteCalls, 0);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().linkId, lease.endpoint.linkId);

    // Link, source component, and acknowledgement target all belong to the
    // immutable command envelope; near matches cannot complete it.
    service.observeMessage(
        lease.endpoint.linkId + 1,
        commandAck(lease.endpoint.systemId, lease.endpoint.componentId,
                   request.command, MAV_RESULT_ACCEPTED));
    service.observeMessage(
        lease.endpoint.linkId,
        commandAck(lease.endpoint.systemId,
                   lease.endpoint.componentId + 1,
                   request.command, MAV_RESULT_ACCEPTED));
    service.observeMessage(
        lease.endpoint.linkId,
        commandAck(lease.endpoint.systemId, lease.endpoint.componentId,
                   request.command, MAV_RESULT_ACCEPTED, 249, 190));
    QCOMPARE(finished.count(), 0);

    service.observeMessage(
        lease.endpoint.linkId,
        commandAck(lease.endpoint.systemId, lease.endpoint.componentId,
                   request.command, MAV_RESULT_ACCEPTED));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).token.transactionId,
             commandToken.transactionId);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedAccepted);
    QVERIFY(service.releaseExactReservation(reservation));
}

void VehicleCommandServiceTest::
singleVehicleSelectionAbaAndSwarmIsolation()
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
    const SwarmVehicleInstanceLease first =
        swarmLease(52, 74, 1, 4, 20);
    const VehicleEndpoint alternate = endpoint(53, 75, 1);
    const QList<SwarmVehicleInstanceLease> active{first};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QVERIFY(service.configureSingleVehicleExactRoute(
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QVERIFY(targets.observeEndpoint(first.endpoint, true));
    QVERIFY(targets.observeEndpoint(alternate));

    const VehicleTargetLease staleA = targets.acquireTarget();
    QVERIFY(targets.selectTarget(
        alternate.linkId, alternate.systemId, alternate.componentId));
    QVERIFY(targets.selectTarget(
        first.endpoint.linkId, first.endpoint.systemId,
        first.endpoint.componentId));
    QObject owner;
    VehicleCommandService::ExactReservationToken singleReservation;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, staleA, first, &singleReservation),
             VehicleCommandService::ExactReservationResult::StaleLease);

    const VehicleTargetLease freshA = targets.acquireTarget();
    QSignalSpy released(
        &service, &VehicleCommandService::exactReservationReleased);
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, freshA, first, &singleReservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QVERIFY(targets.selectTarget(
        alternate.linkId, alternate.systemId, alternate.componentId));
    QCOMPARE(released.count(), 1);
    QCOMPARE(service.submitExactCommandLong(
                 singleReservation, first,
                 exactRequest(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             VehicleCommandService::ExactSubmitResult::InvalidReservation);
    QCOMPARE(frames.size(), 0);

    // The original Swarm reservation policy is deliberately independent of
    // global target selection and therefore survives another selection ABA.
    QObject swarmOwner;
    VehicleCommandService::ExactReservationToken swarmReservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &swarmOwner, active, &swarmReservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QVERIFY(targets.selectTarget(
        first.endpoint.linkId, first.endpoint.systemId,
        first.endpoint.componentId));
    QVERIFY(targets.selectTarget(
        alternate.linkId, alternate.systemId, alternate.componentId));
    QCOMPARE(released.count(), 1);
    QCOMPARE(service.submitExactCommandLong(
                 swarmReservation, first,
                 exactRequest(MAV_CMD_NAV_RETURN_TO_LAUNCH)),
             VehicleCommandService::ExactSubmitResult::Started);
    QCOMPARE(frames.size(), 1);
    service.observeMessage(
        first.endpoint.linkId,
        commandAck(first.endpoint.systemId, first.endpoint.componentId,
                   MAV_CMD_NAV_RETURN_TO_LAUNCH, MAV_RESULT_ACCEPTED));
    QVERIFY(service.releaseExactReservation(swarmReservation));
}

void VehicleCommandServiceTest::
singleVehicleCallbacksFailClosedBeforeWriter()
{
    VehicleTargetManager targets;
    int writes = 0;
    ExactLinkTransmitter transmitter(
        [&writes](int, const QByteArray &) {
            ++writes;
            return true;
        });
    VehicleCommandService service(&targets, &transmitter);
    const SwarmVehicleInstanceLease lease =
        swarmLease(54, 76, 1, 6, 21);
    const VehicleEndpoint alternate = endpoint(55, 77, 1);
    const QList<SwarmVehicleInstanceLease> active{lease};
    bool changeTargetInRoute = false;
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QVERIFY(service.configureSingleVehicleExactRoute(
        [&targets, &alternate, &lease, &changeTargetInRoute](
            const SwarmVehicleInstanceLease &, QString *) {
            if (changeTargetInRoute) {
                changeTargetInRoute = false;
                targets.selectTarget(
                    alternate.linkId, alternate.systemId,
                    alternate.componentId);
                targets.selectTarget(
                    lease.endpoint.linkId, lease.endpoint.systemId,
                    lease.endpoint.componentId);
            }
            return true;
        }));
    QVERIFY(targets.observeEndpoint(lease.endpoint, true));
    QVERIFY(targets.observeEndpoint(alternate));

    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    const VehicleTargetLease beforeReserveAba = targets.acquireTarget();
    changeTargetInRoute = true;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, beforeReserveAba, lease, &reservation),
             VehicleCommandService::ExactReservationResult::StaleLease);
    QVERIFY(!reservation.isValid());

    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, targets.acquireTarget(), lease, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    changeTargetInRoute = true;
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_PREFLIGHT_CALIBRATION)),
             VehicleCommandService::ExactSubmitResult::InvalidReservation);
    QCOMPARE(writes, 0);

    // The operation-specific safety callback runs after the normal route and
    // lease checks and can still veto without allocating a token or writing.
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, targets.acquireTarget(), lease, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    auto rejected = exactRequest(MAV_CMD_PREFLIGHT_CALIBRATION);
    rejected.validateBeforeWrite = [](QString *error) {
        if (error) {
            *error = QStringLiteral("Vehicle became armed.");
        }
        return false;
    };
    VehicleCommandService::ExactCommandToken rejectedToken;
    QString error;
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease, rejected, &rejectedToken, &error),
             VehicleCommandService::ExactSubmitResult::RouteUnavailable);
    QVERIFY(!rejectedToken.isValid());
    QVERIFY(error.contains(QStringLiteral("armed"), Qt::CaseInsensitive));
    QCOMPARE(writes, 0);

    auto changedDuringSafety =
        exactRequest(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN);
    changedDuringSafety.validateBeforeWrite =
        [&targets, &alternate, &lease](QString *) {
            targets.selectTarget(
                alternate.linkId, alternate.systemId,
                alternate.componentId);
            targets.selectTarget(
                lease.endpoint.linkId, lease.endpoint.systemId,
                lease.endpoint.componentId);
            return true;
        };
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease, changedDuringSafety),
             VehicleCommandService::ExactSubmitResult::StaleLease);
    QCOMPARE(writes, 0);
    QVERIFY(!service.releaseExactReservation(reservation));

    // A copied policy callable may delete the service. The outer call must
    // fail without dereferencing its former QObject state.
    ExactLinkTransmitter deletingTransmitter(
        [](int, const QByteArray &) { return true; });
    auto *deletingService =
        new VehicleCommandService(&targets, &deletingTransmitter);
    QPointer<VehicleCommandService> guarded(deletingService);
    QVERIFY(deletingService->configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QVERIFY(deletingService->configureSingleVehicleExactRoute(
        [&deletingService](
            const SwarmVehicleInstanceLease &, QString *) {
            VehicleCommandService *victim = deletingService;
            deletingService = nullptr;
            delete victim;
            return true;
        }));
    VehicleCommandService::ExactReservationToken deletedReservation;
    QCOMPARE(deletingService->reserveSingleVehicleEndpoint(
                 &owner, targets.acquireTarget(), lease,
                 &deletedReservation),
             VehicleCommandService::ExactReservationResult::
                 ContextUnavailable);
    QVERIFY(guarded.isNull());
}

void VehicleCommandServiceTest::
singleVehicleOwnerDetachAndTargetRetirementDrainSafely()
{
    VehicleTargetManager targets;
    const SwarmVehicleInstanceLease lease =
        swarmLease(56, 78, 1, 7, 22);
    const VehicleEndpoint alternate = endpoint(57, 79, 1);
    VehicleCommandService *servicePointer = nullptr;
    bool injectAckBeforeServiceTargetHandler = false;
    connect(&targets, &VehicleTargetManager::targetGenerationChanged,
            &targets,
            [&servicePointer, &injectAckBeforeServiceTargetHandler,
             &lease](qulonglong) {
                if (injectAckBeforeServiceTargetHandler
                    && servicePointer) {
                    injectAckBeforeServiceTargetHandler = false;
                    servicePointer->observeMessage(
                        lease.endpoint.linkId,
                        commandAck(
                            lease.endpoint.systemId,
                            lease.endpoint.componentId,
                            MAV_CMD_PREFLIGHT_CALIBRATION,
                            MAV_RESULT_ACCEPTED));
                }
            });
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    VehicleCommandService service(&targets, &transmitter);
    servicePointer = &service;
    service.setLocalIdentity(250, 190);
    service.setExactQuarantineForTesting(500);
    const QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QVERIFY(service.configureSingleVehicleExactRoute(
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QVERIFY(targets.observeEndpoint(lease.endpoint, true));
    QVERIFY(targets.observeEndpoint(alternate));

    QSignalSpy finished(
        &service, &VehicleCommandService::exactCommandFinished);
    QSignalSpy released(
        &service, &VehicleCommandService::exactReservationReleased);
    auto *owner = new QObject;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 owner, targets.acquireTarget(), lease, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN)),
             VehicleCommandService::ExactSubmitResult::Started);
    delete owner;
    QCOMPARE(released.count(), 0);
    service.observeMessage(
        lease.endpoint.linkId,
        commandAck(lease.endpoint.systemId, lease.endpoint.componentId,
                   MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN,
                   MAV_RESULT_ACCEPTED));
    QCOMPARE(finished.count(), 1);
    QVERIFY(reportAt(finished, 0).ownerDetached);
    QCOMPARE(reportAt(finished, 0).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 AcknowledgedAccepted);
    QCOMPARE(released.count(), 1);

    QObject secondOwner;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &secondOwner, targets.acquireTarget(), lease,
                 &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    QCOMPARE(service.submitExactCommandLong(
                 reservation, lease,
                 exactRequest(MAV_CMD_PREFLIGHT_CALIBRATION)),
             VehicleCommandService::ExactSubmitResult::Started);
    // This observer was connected before the service's invalidation handler.
    // An ACK injected from targetGenerationChanged must not be accepted under
    // the stale selected-target generation.
    injectAckBeforeServiceTargetHandler = true;
    QVERIFY(targets.selectTarget(
        alternate.linkId, alternate.systemId, alternate.componentId));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(reportAt(finished, 1).terminalResult,
             VehicleCommandService::ExactTerminalResult::
                 LeaseRetiredOutcomeUncertain);
    QVERIFY(reportAt(finished, 1).frameAttempted);
    QCOMPARE(released.count(), 2);
    QVERIFY(service.isExactCommandQuarantined(
        lease, MAV_CMD_PREFLIGHT_CALIBRATION));
}

void VehicleCommandServiceTest::terminalCallbacksMayDeleteCommandService_data()
{
    QTest::addColumn<int>("trigger");
    QTest::addColumn<bool>("deleteOnRelease");
    for (int trigger = 0; trigger < 3; ++trigger) {
        const QByteArray name = trigger == 0 ? "retire"
            : trigger == 1 ? "forget-link" : "timeout";
        QTest::newRow((name + "-finished").constData()) << trigger << false;
        QTest::newRow((name + "-released").constData()) << trigger << true;
    }
}

void VehicleCommandServiceTest::terminalCallbacksMayDeleteCommandService()
{
    QFETCH(int, trigger);
    QFETCH(bool, deleteOnRelease);
    VehicleTargetManager targets;
    int writes = 0;
    ExactLinkTransmitter transmitter(
        [&writes](int, const QByteArray &) { ++writes; return true; });
    QPointer<VehicleCommandService> service =
        new VehicleCommandService(&targets, &transmitter, &targets);
    const QList<SwarmVehicleInstanceLease> active{
        swarmLease(53, 81), swarmLease(53, 82)};
    QVERIFY(service->configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &lease) {
            return containsLease(active, lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QObject owner;
    VehicleCommandService::ExactReservationToken reservation;
    QCOMPARE(service->reserveExactEndpoints(&owner, active, &reservation),
             VehicleCommandService::ExactReservationResult::Reserved);
    const int timeoutMs = trigger == 2 ? 1 : 1000;
    for (const auto &lease : active) {
        QCOMPARE(service->submitExactCommandLong(
                     reservation, lease,
                     exactRequest(MAV_CMD_FLASH_BOOTLOADER, timeoutMs, timeoutMs)),
                 VehicleCommandService::ExactSubmitResult::Started);
    }
    // Closing reservations must drain their pending commands, then notify.
    QVERIFY(service->releaseExactReservation(reservation));
    QList<VehicleCommandService::ExactCommandReport> reports;
    int releases = 0;
    int destructions = 0;
    connect(service.data(), &QObject::destroyed, this,
            [&destructions]() { ++destructions; });
    connect(service.data(), &VehicleCommandService::exactCommandFinished,
            this, [&](const VehicleCommandService::ExactCommandReport &report) {
        reports.append(report);
        if (!deleteOnRelease) delete service.data();
    });
    connect(service.data(), &VehicleCommandService::exactReservationReleased,
            this, [&](qulonglong id) {
        QCOMPARE(id, qulonglong(reservation.reservationId));
        ++releases;
        if (deleteOnRelease) delete service.data();
    });

    if (trigger == 0) {
        service->retireExactVehicle(active.at(0));
        if (service) service->retireExactVehicle(active.at(1));
    } else if (trigger == 1) {
        service->forgetLink(53);
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(service.isNull(), 1000);
    }
    QVERIFY(service.isNull());
    QCOMPARE(destructions, 1);
    QCOMPARE(writes, 2);
    QCOMPARE(reports.size(), deleteOnRelease ? 2 : 1);
    QCOMPARE(releases, deleteOnRelease ? 1 : 0);
    for (const auto &report : reports) {
        QVERIFY(report.frameAttempted);
        QVERIFY(report.token.isValid());
        QCOMPARE(report.token.command, MAV_CMD_FLASH_BOOTLOADER);
        const auto expected = trigger == 0
            ? VehicleCommandService::ExactTerminalResult::LeaseRetiredOutcomeUncertain
            : trigger == 1
                ? VehicleCommandService::ExactTerminalResult::LinkForgottenOutcomeUncertain
                : VehicleCommandService::ExactTerminalResult::TimedOutOutcomeUncertain;
        QCOMPARE(report.terminalResult, expected);
    }
    // Destruction must suppress the remaining waiter, timer and release paths.
    QCoreApplication::processEvents();
    QCOMPARE(reports.size(), deleteOnRelease ? 2 : 1);
    QCOMPARE(releases, deleteOnRelease ? 1 : 0);
}

void VehicleCommandServiceTest::idleReservationReleaseMayDeleteCommandService_data()
{
    QTest::addColumn<bool>("forgetLink");
    QTest::newRow("retire") << false;
    QTest::newRow("forget-link") << true;
}

void VehicleCommandServiceTest::idleReservationReleaseMayDeleteCommandService()
{
    QFETCH(bool, forgetLink);
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    QPointer<VehicleCommandService> service =
        new VehicleCommandService(&targets, &transmitter, &targets);
    const QList<SwarmVehicleInstanceLease> active{
        swarmLease(54, 83), swarmLease(54, 84)};
    QVERIFY(service->configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &lease) {
            return containsLease(active, lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QObject owner;
    for (const auto &lease : active) {
        VehicleCommandService::ExactReservationToken reservation;
        QCOMPARE(service->reserveExactEndpoints(&owner, {lease}, &reservation),
                 VehicleCommandService::ExactReservationResult::Reserved);
    }
    int releases = 0;
    connect(service.data(), &VehicleCommandService::exactReservationReleased,
            this, [&](qulonglong) {
        ++releases;
        delete service.data();
    });
    if (forgetLink) service->forgetLink(54);
    else service->retireExactVehicle(active.constFirst());
    QVERIFY(service.isNull());
    QCOMPARE(releases, 1);
    QCoreApplication::processEvents();
    QCOMPARE(releases, 1);
}

QTEST_GUILESS_MAIN(VehicleCommandServiceTest)

#include "test_vehiclecommandservice.moc"
