#include <QtTest>

#include "comm/CompassCalibrationService.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"

#include <QPointer>
#include <QVector>

#include <limits>

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

mavlink_message_t commandAck(
    int sourceSystem, int sourceComponent, MAV_CMD command,
    MAV_RESULT result, int targetSystem = 250,
    int targetComponent = MAV_COMP_ID_MISSIONPLANNER)
{
    mavlink_command_ack_t payload{};
    payload.command = static_cast<quint16>(command);
    payload.result = static_cast<quint8>(result);
    payload.progress = 255;
    payload.target_system = static_cast<quint8>(targetSystem);
    payload.target_component = static_cast<quint8>(targetComponent);
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(
        static_cast<quint8>(sourceSystem),
        static_cast<quint8>(sourceComponent), &message, &payload);
    return message;
}

mavlink_message_t progressMessage(
    int sourceSystem, int sourceComponent, quint8 compassId,
    quint8 mask, quint8 percent, quint8 status = 2)
{
    quint8 completionMask[10]{};
    mavlink_message_t message{};
    mavlink_msg_mag_cal_progress_pack(
        static_cast<quint8>(sourceSystem),
        static_cast<quint8>(sourceComponent), &message,
        compassId, mask, status, 1, percent, completionMask,
        0.0F, 0.0F, 0.0F);
    return message;
}

mavlink_message_t reportMessage(
    int sourceSystem, int sourceComponent, quint8 compassId,
    quint8 mask, quint8 status, bool autosaved,
    float offsetX = 1.0F)
{
    mavlink_message_t message{};
    mavlink_msg_mag_cal_report_pack(
        static_cast<quint8>(sourceSystem),
        static_cast<quint8>(sourceComponent), &message,
        compassId, mask, status, autosaved ? 1 : 0,
        5.5F, offsetX, 2.0F, 3.0F,
        1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 0, 0, 1.0F);
    return message;
}

struct Fixture
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    bool writerAccepts = true;
    ExactLinkTransmitter transmitter;
    VehicleCommandService commands;
    CompassCalibrationService service;

    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
              if (!writerAccepts) {
                  return false;
              }
              frames.append({linkId, bytes});
              return true;
          })
        , commands(&targets, &transmitter)
        , service(&targets, &commands)
    {
        service.setLocalIdentity(250, MAV_COMP_ID_MISSIONPLANNER);
        service.setTimeoutsForTesting(500, 500, 1000);
    }

    VehicleTargetLease select(
        int linkId = 9, int systemId = 42, int componentId = 1)
    {
        targets.observeEndpoint(endpoint(linkId, systemId, componentId));
        targets.selectTarget(linkId, systemId, componentId);
        return targets.acquireTarget();
    }

    mavlink_command_long_t sentCommand(int index = -1) const
    {
        const int actualIndex = index < 0 ? frames.size() - 1 : index;
        const mavlink_message_t message =
            decodeFrame(frames.at(actualIndex).bytes);
        mavlink_command_long_t command{};
        mavlink_msg_command_long_decode(&message, &command);
        return command;
    }

    void ack(MAV_CMD command, MAV_RESULT result,
             int linkId = 9, int systemId = 42, int componentId = 1)
    {
        commands.observeMessage(
            linkId, commandAck(systemId, componentId, command, result));
    }
};

} // namespace

class CompassCalibrationServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void startUsesExactWireAndRequiresAckBoundary();
    void reportsWaitForWholeMaskAndRetryFailures();
    void autosavedReportsCompleteOnlyAsASet();
    void telemetryRequiresExactEnvelopeAndStableMask();
    void placeholderAndUnaddressedTelemetryAreIgnored();
    void acceptAndCancelUseSafeMasksAndAckResults();
    void cancelRacesAreSingleShot();
    void commandTimeoutPoisonsWhileActivityAndTotalWarn();
    void targetAndLinkLossFailClosedButNewGenerationCanStart();
    void rebootLatchIsScopedToItsVehicle();
    void fixedYawValidatesAndCorrelatesCommand();
    void shutdownAndReentrantDeletionAreSafe();
};

void CompassCalibrationServiceTest::startUsesExactWireAndRequiresAckBoundary()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select();

    QCOMPARE(fixture.service.start(lease, true),
             CompassCalibrationService::RequestResult::Armed);
    QCOMPARE(fixture.frames.size(), 0);
    QCOMPARE(fixture.service.start(lease, false),
             CompassCalibrationService::RequestResult::Started);
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::StartPending);
    QCOMPARE(fixture.frames.size(), 1);

    const mavlink_command_long_t sent = fixture.sentCommand();
    QCOMPARE(sent.target_system, quint8(42));
    QCOMPARE(sent.target_component, quint8(1));
    QCOMPARE(sent.command, quint16(MAV_CMD_DO_START_MAG_CAL));
    QCOMPARE(sent.param1, 0.0F);
    QCOMPARE(sent.param2, 1.0F);
    QCOMPARE(sent.param3, 1.0F);
    QCOMPARE(sent.param4, 0.0F);
    QCOMPARE(sent.param5, 0.0F);
    QCOMPARE(sent.param6, 0.0F);
    QCOMPARE(sent.param7, 0.0F);

    // A plausible packet before a correlated ACK cannot rescue the start.
    fixture.service.observeMessage(
        9, progressMessage(42, 1, 0, 1, 73));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::StartPending);
    QCOMPARE(fixture.service.progress(0), 0);
    QCOMPARE(fixture.service.calibrationMask(), quint8(0));

    fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_IN_PROGRESS);
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::Running);
    fixture.service.observeMessage(
        9, progressMessage(42, 1, 0, 1, 255));
    QCOMPARE(fixture.service.progress(0), 100);
    QCOMPARE(fixture.service.calibrationMask(), quint8(1));
    fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::Running);
}

void CompassCalibrationServiceTest::reportsWaitForWholeMaskAndRetryFailures()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select();
    QCOMPARE(fixture.service.start(lease, false),
             CompassCalibrationService::RequestResult::Started);
    fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);

    // Raw status 8 is absent from the old bundled enum. Retry-on-failure means
    // this one attempt is not the terminal state of the session.
    fixture.service.observeMessage(
        9, reportMessage(42, 1, 0, 3, 8, true, 4.0F));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::Running);
    QCOMPARE(fixture.service.progress(0), 0);
    QVERIFY(!fixture.service.rebootRequired());
    QVERIFY(fixture.service.resultText().contains(
        QStringLiteral("MAG_CAL_FAILED_OFFSETS")));
    const QString oneFailure = fixture.service.resultText();
    fixture.service.observeMessage(
        9, reportMessage(42, 1, 0, 3, 8, true, 4.0F));
    QCOMPARE(fixture.service.resultText(), oneFailure);

    fixture.service.observeMessage(
        9, reportMessage(42, 1, 1, 3, 4, true));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::Running);
    QVERIFY(fixture.service.rebootRequired());

    // A later successful retry supersedes the failure. Only the successful,
    // unsaved compass becomes eligible for manual acceptance.
    fixture.service.observeMessage(
        9, reportMessage(42, 1, 0, 3, 4, false));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::AwaitingAccept);
    QCOMPARE(fixture.service.manualAcceptMask(), quint8(1));
    QCOMPARE(fixture.service.progress(0), 100);
    QCOMPARE(fixture.service.progress(1), 100);
}

void CompassCalibrationServiceTest::autosavedReportsCompleteOnlyAsASet()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select();
    fixture.service.start(lease, false);
    fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);

    fixture.service.observeMessage(
        9, reportMessage(42, 1, 0, 7, 4, true));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::Running);
    fixture.service.observeMessage(
        9, reportMessage(42, 1, 1, 7, 4, true));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::Running);
    fixture.service.observeMessage(
        9, reportMessage(42, 1, 2, 7, 4, true));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::CompletedNeedsReboot);
    QVERIFY(fixture.service.rebootRequired());

    fixture.service.clearRebootRequired(lease);
    QCOMPARE(fixture.service.state(), CompassCalibrationService::State::Idle);
    QVERIFY(!fixture.service.rebootRequired());
}

void CompassCalibrationServiceTest::telemetryRequiresExactEnvelopeAndStableMask()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select();
    fixture.service.start(lease, false);
    fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);

    fixture.service.observeMessage(
        8, progressMessage(42, 1, 0, 3, 10));
    fixture.service.observeMessage(
        9, progressMessage(43, 1, 0, 3, 20));
    fixture.service.observeMessage(
        9, progressMessage(42, 2, 0, 3, 30));
    QCOMPARE(fixture.service.calibrationMask(), quint8(0));
    QCOMPARE(fixture.service.progress(0), 0);

    fixture.service.observeMessage(
        9, progressMessage(42, 1, 0, 3, 40));
    QCOMPARE(fixture.service.calibrationMask(), quint8(3));
    QCOMPARE(fixture.service.progress(0), 40);

    fixture.service.observeMessage(
        9, progressMessage(42, 1, 0, 1, 50));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::OutcomeUncertain);
    QVERIFY(fixture.service.resultText().contains(
        QStringLiteral("mask changed"), Qt::CaseInsensitive));

    Fixture malformed;
    const VehicleTargetLease malformedLease = malformed.select();
    malformed.service.start(malformedLease, false);
    malformed.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
    malformed.service.observeMessage(
        9, progressMessage(42, 1, 8, 1, 30));
    QCOMPARE(malformed.service.state(),
             CompassCalibrationService::State::Running);
}

void CompassCalibrationServiceTest::placeholderAndUnaddressedTelemetryAreIgnored()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select();
    fixture.service.start(lease, false);
    fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);

    fixture.service.observeMessage(
        9, reportMessage(42, 1, 0, 0, 0, false, 0.0F));
    fixture.service.observeMessage(
        9, progressMessage(42, 1, 0, 0, 25, 1));
    fixture.service.observeMessage(
        9, reportMessage(42, 1, 2, 1, 4, true));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::Running);
    QCOMPARE(fixture.service.calibrationMask(), quint8(0));
    QVERIFY(fixture.service.resultText().contains(
        QStringLiteral("started"), Qt::CaseInsensitive));

    fixture.service.observeMessage(
        9, progressMessage(42, 1, 0, 1, 25));
    QCOMPARE(fixture.service.calibrationMask(), quint8(1));
    QCOMPARE(fixture.service.progress(0), 25);
}

void CompassCalibrationServiceTest::acceptAndCancelUseSafeMasksAndAckResults()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select();
    fixture.service.start(lease, false);
    fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
    fixture.service.observeMessage(
        9, reportMessage(42, 1, 0, 3, 4, false));
    fixture.service.observeMessage(
        9, reportMessage(42, 1, 1, 3, 4, true));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::AwaitingAccept);

    QCOMPARE(fixture.service.accept(false),
             CompassCalibrationService::RequestResult::Started);
    const mavlink_command_long_t accept = fixture.sentCommand();
    QCOMPARE(accept.command, quint16(MAV_CMD_DO_ACCEPT_MAG_CAL));
    QCOMPARE(accept.param1, 1.0F); // successful and unsaved mask only
    QCOMPARE(accept.param2, 0.0F);
    QCOMPARE(accept.param3, 1.0F); // retained MP10 wire value
    QCOMPARE(accept.param7, 0.0F);
    fixture.ack(MAV_CMD_DO_ACCEPT_MAG_CAL, MAV_RESULT_DENIED);
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::Failed);

    QCOMPARE(fixture.service.cancel(),
             CompassCalibrationService::RequestResult::Started);
    const mavlink_command_long_t cancel = fixture.sentCommand();
    QCOMPARE(cancel.command, quint16(MAV_CMD_DO_CANCEL_MAG_CAL));
    QCOMPARE(cancel.param1, 0.0F);
    QCOMPARE(cancel.param2, 0.0F);
    QCOMPARE(cancel.param3, 1.0F);
    QCOMPARE(cancel.param7, 0.0F);
    fixture.ack(MAV_CMD_DO_CANCEL_MAG_CAL, MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.state(), CompassCalibrationService::State::Idle);
}

void CompassCalibrationServiceTest::cancelRacesAreSingleShot()
{
    {
        Fixture fixture;
        const VehicleTargetLease lease = fixture.select();
        fixture.service.start(lease, false);
        QCOMPARE(fixture.service.cancel(),
                 CompassCalibrationService::RequestResult::InvalidState);
        QCOMPARE(fixture.frames.size(), 1);

        fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
        QCOMPARE(fixture.service.cancel(),
                 CompassCalibrationService::RequestResult::Started);
        QCOMPARE(fixture.frames.size(), 2);
        QCOMPARE(fixture.service.cancel(),
                 CompassCalibrationService::RequestResult::InvalidState);
        QCOMPARE(fixture.frames.size(), 2);

        fixture.service.setTimeoutsForTesting(20, 500, 500);
        // Re-arm the already-running command timer with the short test value.
        fixture.ack(MAV_CMD_DO_CANCEL_MAG_CAL, MAV_RESULT_IN_PROGRESS);
        QTRY_COMPARE_WITH_TIMEOUT(
            fixture.service.state(),
            CompassCalibrationService::State::OutcomeUncertain, 200);
        QCOMPARE(fixture.service.cancel(),
                 CompassCalibrationService::RequestResult::OutcomeUncertain);
        QCOMPARE(fixture.frames.size(), 2);
    }
    {
        Fixture fixture;
        const VehicleTargetLease lease = fixture.select();
        fixture.service.start(lease, false);
        fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
        fixture.service.observeMessage(
            9, reportMessage(42, 1, 0, 1, 4, false));
        QCOMPARE(fixture.service.accept(true),
                 CompassCalibrationService::RequestResult::Armed);
        QCOMPARE(fixture.service.state(),
                 CompassCalibrationService::State::AwaitingAccept);
        fixture.service.accept(false);
        fixture.ack(MAV_CMD_DO_ACCEPT_MAG_CAL, MAV_RESULT_DENIED);
        QCOMPARE(fixture.service.state(),
                 CompassCalibrationService::State::Failed);
        QVERIFY(fixture.service.canCancel());
        const int frameCount = fixture.frames.size();
        QCOMPARE(fixture.service.start(lease, false),
                 CompassCalibrationService::RequestResult::Busy);
        QCOMPARE(fixture.service.fixedYaw(lease, 45.0, false),
                 CompassCalibrationService::RequestResult::Busy);
        QCOMPARE(fixture.frames.size(), frameCount);
    }
    {
        Fixture fixture;
        const VehicleTargetLease lease = fixture.select();
        fixture.service.setTimeoutsForTesting(500, 20, 40);
        fixture.service.start(lease, false);
        fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
        fixture.writerAccepts = false;
        QCOMPARE(fixture.service.cancel(),
                 CompassCalibrationService::RequestResult::TransportUnavailable);
        QCOMPARE(fixture.service.state(),
                 CompassCalibrationService::State::Running);
        QVERIFY(fixture.service.canCancel());
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.service.resultText().contains(
                QStringLiteral("remains active")), 200);
        fixture.writerAccepts = true;
        QCOMPARE(fixture.service.cancel(),
                 CompassCalibrationService::RequestResult::Started);
    }
}

void CompassCalibrationServiceTest::commandTimeoutPoisonsWhileActivityAndTotalWarn()
{
    {
        Fixture fixture;
        const VehicleTargetLease lease = fixture.select();
        fixture.service.setTimeoutsForTesting(20, 500, 500);
        fixture.service.start(lease, false);
        QTRY_COMPARE_WITH_TIMEOUT(
            fixture.service.state(),
            CompassCalibrationService::State::OutcomeUncertain, 200);
        const int count = fixture.frames.size();
        QCOMPARE(fixture.service.start(lease, false),
                 CompassCalibrationService::RequestResult::OutcomeUncertain);
        QCOMPARE(fixture.frames.size(), count);
        // Cancel remains available to put the onboard side into a known stop.
        QCOMPARE(fixture.service.cancel(),
                 CompassCalibrationService::RequestResult::Started);
        fixture.ack(MAV_CMD_DO_CANCEL_MAG_CAL, MAV_RESULT_ACCEPTED);
        QCOMPARE(fixture.service.state(),
                 CompassCalibrationService::State::Idle);
        // The old START ACK is still un-tokenized on the wire; same-generation
        // retry therefore remains poisoned even after a confirmed cancel.
        QCOMPARE(fixture.service.start(lease, false),
                 CompassCalibrationService::RequestResult::OutcomeUncertain);
    }
    {
        Fixture fixture;
        const VehicleTargetLease lease = fixture.select();
        fixture.service.setTimeoutsForTesting(500, 20, 500);
        fixture.service.start(lease, false);
        fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.service.resultText().contains(
                QStringLiteral("remains active")), 200);
        QCOMPARE(fixture.service.state(),
                 CompassCalibrationService::State::Running);
        QVERIFY(!fixture.service.resultText().contains(
            QStringLiteral("outcome is unknown")));
    }
    {
        Fixture fixture;
        const VehicleTargetLease lease = fixture.select();
        fixture.service.setTimeoutsForTesting(500, 500, 20);
        fixture.service.start(lease, false);
        fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.service.resultText().contains(
                QStringLiteral("longer than expected")), 200);
        QCOMPARE(fixture.service.state(),
                 CompassCalibrationService::State::Running);
    }
}

void CompassCalibrationServiceTest::targetAndLinkLossFailClosedButNewGenerationCanStart()
{
    Fixture fixture;
    const VehicleTargetLease first = fixture.select(9, 42, 1);
    fixture.targets.observeEndpoint(endpoint(10, 42, 1));
    fixture.service.start(first, false);
    fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
    QVERIFY(fixture.targets.selectTarget(10, 42, 1));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::Idle);
    QVERIFY(!fixture.service.hasActiveTarget());

    const VehicleTargetLease second = fixture.targets.acquireTarget();
    QCOMPARE(fixture.service.start(second, false),
             CompassCalibrationService::RequestResult::Started);
    QCOMPARE(fixture.frames.constLast().linkId, 10);

    fixture.service.forgetLink(10);
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::OutcomeUncertain);
}

void CompassCalibrationServiceTest::rebootLatchIsScopedToItsVehicle()
{
    Fixture fixture;
    const VehicleTargetLease first = fixture.select(9, 42, 1);
    fixture.service.start(first, false);
    fixture.ack(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED);
    fixture.service.observeMessage(
        9, reportMessage(42, 1, 0, 1, 4, true));
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::CompletedNeedsReboot);
    QVERIFY(fixture.service.rebootRequired());
    QVERIFY(fixture.service.rebootRequiredFor(first));
    QCOMPARE(fixture.service.start(first, false),
             CompassCalibrationService::RequestResult::RebootRequired);
    QCOMPARE(fixture.service.fixedYaw(first, 15.0, false),
             CompassCalibrationService::RequestResult::RebootRequired);

    fixture.targets.observeEndpoint(endpoint(10, 43, 1));
    QVERIFY(fixture.targets.selectTarget(10, 43, 1));
    const VehicleTargetLease second = fixture.targets.acquireTarget();
    QCOMPARE(fixture.service.fixedYaw(second, 15.0, false),
             CompassCalibrationService::RequestResult::Started);
    QVERIFY(fixture.service.rebootRequired());
    QVERIFY(!fixture.service.rebootRequiredFor(second));
    QCOMPARE(fixture.frames.constLast().linkId, 10);
    fixture.ack(MAV_CMD_FIXED_MAG_CAL_YAW, MAV_RESULT_ACCEPTED,
                10, 43, 1);

    QVERIFY(fixture.targets.selectTarget(9, 42, 1));
    const VehicleTargetLease firstAgain = fixture.targets.acquireTarget();
    QVERIFY(fixture.service.rebootRequiredFor(firstAgain));
    QCOMPARE(fixture.service.start(firstAgain, false),
             CompassCalibrationService::RequestResult::RebootRequired);
}

void CompassCalibrationServiceTest::fixedYawValidatesAndCorrelatesCommand()
{
    Fixture fixture;
    const VehicleTargetLease lease = fixture.select();
    QCOMPARE(fixture.service.fixedYaw(
                 lease, std::numeric_limits<double>::quiet_NaN(), false),
             CompassCalibrationService::RequestResult::InvalidHeading);
    QCOMPARE(fixture.service.fixedYaw(lease, 90.0, true),
             CompassCalibrationService::RequestResult::Armed);
    QCOMPARE(fixture.frames.size(), 0);

    QCOMPARE(fixture.service.fixedYaw(lease, 123.5, false),
             CompassCalibrationService::RequestResult::Started);
    const mavlink_command_long_t sent = fixture.sentCommand();
    QCOMPARE(sent.command, quint16(MAV_CMD_FIXED_MAG_CAL_YAW));
    QCOMPARE(sent.param1, 123.5F);
    QCOMPARE(sent.param2, 0.0F);
    QCOMPARE(sent.param7, 0.0F);

    fixture.ack(MAV_CMD_FIXED_MAG_CAL_YAW, MAV_RESULT_ACCEPTED,
                8, 42, 1);
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::FixedYawPending);
    fixture.ack(MAV_CMD_FIXED_MAG_CAL_YAW, MAV_RESULT_ACCEPTED);
    QCOMPARE(fixture.service.state(),
             CompassCalibrationService::State::FixedYawCompleted);
}

void CompassCalibrationServiceTest::shutdownAndReentrantDeletionAreSafe()
{
    {
        Fixture fixture;
        const VehicleTargetLease lease = fixture.select();
        fixture.service.shutdown();
        QCOMPARE(fixture.service.state(),
                 CompassCalibrationService::State::Idle);
        QCOMPARE(fixture.service.start(lease, false),
                 CompassCalibrationService::RequestResult::ShuttingDown);
    }

    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService commands(&targets, &transmitter);
    targets.observeEndpoint(endpoint(9, 42, 1), true);
    auto *service = new CompassCalibrationService(&targets, &commands);
    service->setLocalIdentity(250, MAV_COMP_ID_MISSIONPLANNER);
    QPointer<CompassCalibrationService> guarded(service);
    connect(service, &CompassCalibrationService::changed,
            service, [service]() {
        if (service->state() == CompassCalibrationService::State::Running) {
            delete service;
        }
    });
    QCOMPARE(service->start(targets.acquireTarget(), false),
             CompassCalibrationService::RequestResult::Started);
    commands.observeMessage(
        9, commandAck(42, 1, MAV_CMD_DO_START_MAG_CAL,
                      MAV_RESULT_ACCEPTED));
    QVERIFY(guarded.isNull());
}

QTEST_GUILESS_MAIN(CompassCalibrationServiceTest)
#include "test_compasscalibrationservice.moc"
