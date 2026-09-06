#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "services/GuidedAltitudeStore.h"
#include "services/GuidedNavigationService.h"

#include <QtTest>

#include <cmath>
#include <functional>

namespace
{

static_assert(GuidedNavigationService::DefaultMaximumRetries == 3,
              "MP10 sends the original frame plus three retries");

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId = 7, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = componentId;
    value.linkName = QStringLiteral("Guided physical link");
    value.componentName = QStringLiteral("Autopilot");
    return value;
}

mavlink_message_t heartbeat(const VehicleEndpoint &target,
                            bool armed = false)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        quint8(target.systemId), quint8(target.componentId), &message,
        MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        0, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t commandAck(const SwarmVehicleInstanceLease &lease,
                             MAV_RESULT result = MAV_RESULT_ACCEPTED)
{
    mavlink_command_ack_t payload{};
    payload.command = MAV_CMD_DO_REPOSITION;
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

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int status = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        status = parser.parseByte(quint8(byte), &message);
    }
    return status == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

QByteArray payloadBytes(const QByteArray &bytes)
{
    const mavlink_message_t message = decodeFrame(bytes);
    return QByteArray(_MAV_PAYLOAD(&message), message.len);
}

GuidedNavigationService::Report reportAt(
    const QSignalSpy &spy, int index)
{
    return qvariant_cast<GuidedNavigationService::Report>(
        spy.at(index).at(0));
}

struct Fixture
{
    qint64 now = 1000;
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry{[this] { return now; }};
    GuidedAltitudeStore altitudeStore{&targets, &registry};
    QVector<CapturedFrame> frames;
    std::function<bool(int, const QByteArray &)> writeHook;
    std::function<bool(const SwarmVehicleInstanceLease &, QString *)>
        routeHook;
    ExactLinkTransmitter transmitter{
        [this](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return writeHook ? writeHook(linkId, bytes) : true;
        }};
    VehicleCommandService commands{&targets, &transmitter};
    GuidedTargetService guided{&targets, &commands};
    GuidedNavigationService navigation{
        &altitudeStore, &guided, &commands};
    VehicleEndpoint target = endpoint();
    quint64 linkEpoch = 0;
    GuidedAltitudeStore::Context context;

    bool initialize(bool armed = false)
    {
        commands.setLocalIdentity(250, 190);
        commands.setExactQuarantineForTesting(50);
        if (!commands.configureExactTransactions(
                [this](const SwarmVehicleInstanceLease &lease) {
                    return registry.validateLease(lease);
                },
                [this](const SwarmVehicleInstanceLease &lease,
                       QString *error) {
                    if (routeHook) return routeHook(lease, error);
                    return registry.validateLease(lease);
                })) {
            return false;
        }
        if (!commands.configureSingleVehicleExactRoute(
                [this](const SwarmVehicleInstanceLease &lease,
                       QString *error) {
                    if (routeHook) return routeHook(lease, error);
                    if (!registry.validateLease(lease)) {
                        if (error) *error = QStringLiteral("Stale route");
                        return false;
                    }
                    return true;
                })) {
            return false;
        }
        linkEpoch = registry.beginLinkSession(
            target.linkId, target.linkName);
        if (!registry.observeMessage(
                target.linkId, linkEpoch, heartbeat(target, armed))) {
            return false;
        }
        if (!targets.observeEndpoint(target, true)) return false;
        targets.observeHeartbeat(
            target, armed, MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_TYPE_QUADROTOR);
        transmitter.setLinkSessionEpoch(target.linkId, linkEpoch);
        return altitudeStore.prepareCurrent(&context);
    }

    bool setAltitude(double altitude, MAV_FRAME frame)
    {
        GuidedAltitudeStore::Context updated;
        if (!altitudeStore.commitAltitude(
                context, altitude, frame, &updated)) {
            return false;
        }
        context = updated;
        return true;
    }

    GuidedNavigationService::Plan prepare(
        GuidedNavigationService::Purpose purpose,
        double latitude = 35.123456789,
        double longitude = -33.987654321,
        double altitude = 42.5,
        MAV_FRAME frame = MAV_FRAME_GLOBAL_RELATIVE_ALT,
        bool changeMode = false)
    {
        GuidedNavigationService::Plan plan;
        navigation.prepare(context, latitude, longitude, altitude,
                           frame, changeMode, purpose, &plan);
        return plan;
    }
};

} // namespace

class GuidedNavigationServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void terrainRequiresExplicitStoredAltitudeButReinterpretsFrame();
    void exactWireAndAcceptedReceiptAreTruthful();
    void retriesKeepOneImmutablePayload();
    void acceptedAfterRetryCancelFromTargetReceiptKeepsBarrier();
    void cancellationBeforeAdmissionHasNoEffects();
    void cancellationDuringWriterDrainsSharedQuarantine();
    void targetAbaAndPostSigningContextChangeFailClosed();
    void guidedLaneExcludesExistingSession();
    void preparedPlansAreSingleUseAndOwnerScoped();
};

void GuidedNavigationServiceTest::
terrainRequiresExplicitStoredAltitudeButReinterpretsFrame()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    GuidedNavigationService::Plan plan;
    QString error;
    QVERIFY(!fixture.navigation.prepare(
        fixture.context, 35, 33, 0,
        MAV_FRAME_GLOBAL_RELATIVE_ALT, false,
        GuidedNavigationService::Purpose::TerrainClick,
        &plan, &error));
    QVERIFY(!plan.isValid());
    QVERIFY(!error.isEmpty());
    QVERIFY(!fixture.navigation.prepare(
        fixture.context, 35, 33, 0,
        MAV_FRAME_GLOBAL_RELATIVE_ALT, true,
        GuidedNavigationService::Purpose::FlyToHere,
        &plan, &error));
    QVERIFY(!fixture.navigation.prepare(
        fixture.context, 0, 0, 10,
        MAV_FRAME_GLOBAL_RELATIVE_ALT, false,
        GuidedNavigationService::Purpose::Coordinates,
        &plan, &error));

    QVERIFY(fixture.setAltitude(-42.25, MAV_FRAME_GLOBAL));
    QVERIFY(fixture.navigation.prepare(
        fixture.context, 35, 33, -42.25,
        MAV_FRAME_GLOBAL_RELATIVE_ALT, false,
        GuidedNavigationService::Purpose::TerrainClick,
        &plan, &error));
    QVERIFY(plan.isValid());
    QCOMPARE(plan.context().frame, MAV_FRAME_GLOBAL);
    QCOMPARE(plan.frame(), MAV_FRAME_GLOBAL_RELATIVE_ALT);
    QCOMPARE(plan.altitudeM(), -42.25);
    QVERIFY(!plan.changeMode());
    QVERIFY(fixture.navigation.validate(plan));

    GuidedNavigationService::Plan rejected;
    QVERIFY(!fixture.navigation.prepare(
        fixture.context, 35, 33, -42.25,
        MAV_FRAME_GLOBAL_RELATIVE_ALT, true,
        GuidedNavigationService::Purpose::TerrainClick,
        &rejected, &error));
    QVERIFY(!rejected.isValid());
    QVERIFY(fixture.navigation.validate(plan));

    QVERIFY(fixture.navigation.prepare(
        fixture.context, 35, 33, -42.25,
        MAV_FRAME_GLOBAL_RELATIVE_ALT, false,
        GuidedNavigationService::Purpose::TerrainClick,
        &plan));
    QVERIFY(plan.description().contains(
        QStringLiteral("link 7"), Qt::CaseInsensitive));
    QVERIFY(plan.description().contains(QStringLiteral("42/1")));
}

void GuidedNavigationServiceTest::exactWireAndAcceptedReceiptAreTruthful()
{
    Fixture fixture;
    QVERIFY(fixture.initialize(true)); // One-shot reposition permits armed flight.
    QVERIFY(fixture.setAltitude(-12.25, MAV_FRAME_GLOBAL));
    const auto plan = fixture.prepare(
        GuidedNavigationService::Purpose::FlyToHere,
        35.123456789, -33.987654321, -12.25,
        MAV_FRAME_GLOBAL, true);
    QVERIFY(plan.isValid());
    fixture.writeHook = [&fixture](int linkId, const QByteArray &) {
        fixture.commands.observePhysicalMessage(
            linkId, fixture.linkEpoch,
            commandAck(fixture.context.vehicle));
        return true;
    };
    QSignalSpy finished(
        &fixture.navigation,
        &GuidedNavigationService::operationFinished);
    QObject uiOwner;
    quint64 operationId = 0;
    QCOMPARE(fixture.navigation.execute(
                 &uiOwner, plan, &operationId),
             GuidedNavigationService::SubmitResult::Started);
    QVERIFY(operationId != 0);
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(finished.count(), 1);

    const mavlink_message_t message =
        decodeFrame(fixture.frames.constFirst().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_COMMAND_INT));
    mavlink_command_int_t command{};
    mavlink_msg_command_int_decode(&message, &command);
    QCOMPARE(command.command, quint16(MAV_CMD_DO_REPOSITION));
    QCOMPARE(command.target_system, quint8(42));
    QCOMPARE(command.target_component, quint8(1));
    QCOMPARE(command.frame, quint8(MAV_FRAME_GLOBAL));
    QCOMPARE(command.param1, -1.0F);
    QCOMPARE(command.param2, 1.0F);
    QCOMPARE(command.param3, 0.0F);
    QVERIFY(std::isnan(command.param4));
    QCOMPARE(command.x, qint32(351234567));
    QCOMPARE(command.y, qint32(-339876543));
    QCOMPARE(command.z, -12.25F);
    QCOMPARE(command.current, quint8(0));
    QCOMPARE(command.autocontinue, quint8(0));

    const auto report = reportAt(finished, 0);
    QCOMPARE(report.operationId, operationId);
    QCOMPARE(report.outcome, GuidedNavigationService::Outcome::Accepted);
    QVERIFY(report.frameAttempted);
    QCOMPARE(report.transmissionAttempts, 1);
    QVERIFY(report.targetRecorded);
    QVERIFY(!report.cancellationRequested);
    const auto stored = fixture.altitudeStore.current();
    QVERIFY(stored.pointSet);
    QCOMPARE(stored.latitude, plan.latitude());
    QCOMPARE(stored.longitude, plan.longitude());
    QCOMPARE(stored.altitudeM, float(plan.altitudeM()));
    QCOMPARE(stored.frame, plan.frame());
    QVERIFY(!fixture.guided.hasActiveSession());
}

void GuidedNavigationServiceTest::retriesKeepOneImmutablePayload()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.setAltitude(30, MAV_FRAME_GLOBAL_RELATIVE_ALT));
    fixture.navigation.setTimeoutsForTesting(5, 250, 2);
    const auto plan = fixture.prepare(
        GuidedNavigationService::Purpose::Coordinates,
        10.123456789, 20.987654321, 30,
        MAV_FRAME_GLOBAL_RELATIVE_ALT, false);
    QSignalSpy finished(
        &fixture.navigation,
        &GuidedNavigationService::operationFinished);
    fixture.writeHook = [&fixture](int linkId, const QByteArray &) {
        if (fixture.frames.size() == 3) {
            fixture.commands.observePhysicalMessage(
                linkId, fixture.linkEpoch,
                commandAck(fixture.context.vehicle));
        }
        return true;
    };
    QObject owner;
    quint64 operationId = 0;
    QCOMPARE(fixture.navigation.execute(
                 &owner, plan, &operationId),
             GuidedNavigationService::SubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    QCOMPARE(fixture.frames.size(), 3);
    QCOMPARE(payloadBytes(fixture.frames.at(1).bytes),
             payloadBytes(fixture.frames.at(0).bytes));
    QCOMPARE(payloadBytes(fixture.frames.at(2).bytes),
             payloadBytes(fixture.frames.at(0).bytes));
    const auto report = reportAt(finished, 0);
    QCOMPARE(report.outcome, GuidedNavigationService::Outcome::Accepted);
    QCOMPARE(report.transmissionAttempts, 3);
}

void GuidedNavigationServiceTest::
acceptedAfterRetryCancelFromTargetReceiptKeepsBarrier()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.commands.setExactQuarantineForTesting(200);
    fixture.navigation.setTimeoutsForTesting(5, 250, 1);
    const auto plan = fixture.prepare(
        GuidedNavigationService::Purpose::FlyToHere,
        10.25, 20.5, 31,
        MAV_FRAME_GLOBAL_RELATIVE_ALT, false);
    QVERIFY(plan.isValid());

    QSignalSpy finished(
        &fixture.navigation,
        &GuidedNavigationService::operationFinished);
    quint64 operationId = 0;
    bool receiptObserved = false;
    bool cancellationCalled = false;
    bool busyDuringReceipt = false;
    bool guidedReservedDuringReceipt = false;
    bool terminalEmittedDuringReceipt = false;
    bool centralQuarantinedDuringReceipt = true;
    connect(&fixture.altitudeStore, &GuidedAltitudeStore::changed,
            &fixture.navigation, [&] {
                receiptObserved = true;
                cancellationCalled = fixture.navigation.cancel(operationId);
                busyDuringReceipt = fixture.navigation.busy();
                guidedReservedDuringReceipt =
                    fixture.guided.hasActiveSession();
                terminalEmittedDuringReceipt = finished.count() != 0;
                centralQuarantinedDuringReceipt =
                    fixture.commands.isExactCommandQuarantined(
                        plan.vehicle(), MAV_CMD_DO_REPOSITION);
            });
    fixture.writeHook = [&fixture](int linkId, const QByteArray &) {
        if (fixture.frames.size() == 2) {
            fixture.commands.observePhysicalMessage(
                linkId, fixture.linkEpoch,
                commandAck(fixture.context.vehicle));
        }
        return true;
    };

    QObject owner;
    QCOMPARE(fixture.navigation.execute(
                 &owner, plan, &operationId),
             GuidedNavigationService::SubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(receiptObserved, 1000);
    QCOMPARE(fixture.frames.size(), 2);
    QVERIFY(receiptObserved);
    QVERIFY(cancellationCalled);
    QVERIFY(busyDuringReceipt);
    QVERIFY(guidedReservedDuringReceipt);
    QVERIFY(!terminalEmittedDuringReceipt);
    QVERIFY(centralQuarantinedDuringReceipt);
    QVERIFY(fixture.navigation.busy());
    QVERIFY(fixture.guided.hasActiveSession());
    QCOMPARE(finished.count(), 0);

    QObject followMeOwner;
    GuidedTargetService::SessionToken followMeSession;
    QCOMPARE(fixture.guided.reserve(
                 &followMeOwner, plan.target(), &followMeSession),
             GuidedTargetService::RequestResult::Busy);
    // The indistinguishable duplicate retry ACK is consumed by the central
    // quarantine before a new Follow Me owner can acquire the guided lane.
    fixture.commands.observePhysicalMessage(
        plan.target().endpoint.linkId,
        plan.vehicle().linkSessionEpoch,
        commandAck(plan.vehicle()));
    fixture.commands.observePhysicalMessage(
        plan.target().endpoint.linkId,
        plan.vehicle().linkSessionEpoch,
        commandAck(plan.vehicle()));
    QVERIFY(fixture.commands.isExactCommandQuarantined(
        plan.vehicle(), MAV_CMD_DO_REPOSITION));
    QCOMPARE(fixture.guided.reserve(
                 &followMeOwner, plan.target(), &followMeSession),
             GuidedTargetService::RequestResult::Busy);

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    const auto report = reportAt(finished, 0);
    QCOMPARE(report.operationId, operationId);
    QCOMPARE(report.outcome, GuidedNavigationService::Outcome::Accepted);
    QCOMPARE(report.transmissionAttempts, 2);
    QVERIFY(report.frameAttempted);
    QVERIFY(report.targetRecorded);
    QVERIFY(report.cancellationRequested);
    QVERIFY(!fixture.navigation.busy());
    QVERIFY(!fixture.guided.hasActiveSession());
    QCOMPARE(fixture.guided.reserve(
                 &followMeOwner, fixture.targets.acquireTarget(),
                 &followMeSession),
             GuidedTargetService::RequestResult::Started);
    QCOMPARE(fixture.guided.stop(followMeSession),
             GuidedTargetService::RequestResult::Stopped);
}

void GuidedNavigationServiceTest::cancellationBeforeAdmissionHasNoEffects()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const auto plan = fixture.prepare(
        GuidedNavigationService::Purpose::Coordinates);
    QSignalSpy finished(
        &fixture.navigation,
        &GuidedNavigationService::operationFinished);
    quint64 operationId = 0;
    bool cancelled = false;
    connect(&fixture.navigation, &GuidedNavigationService::stateChanged,
            &fixture.navigation, [&] {
                if (!cancelled && operationId != 0
                    && fixture.navigation.busy()) {
                    cancelled = fixture.navigation.cancel(operationId);
                }
            });
    QObject owner;
    QCOMPARE(fixture.navigation.execute(
                 &owner, plan, &operationId),
             GuidedNavigationService::SubmitResult::Started);
    QVERIFY(cancelled);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(fixture.frames.size(), 0);
    QCOMPARE(reportAt(finished, 0).outcome,
             GuidedNavigationService::Outcome::Cancelled);
    QVERIFY(!fixture.guided.hasActiveSession());
}

void GuidedNavigationServiceTest::
cancellationDuringWriterDrainsSharedQuarantine()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.commands.setExactQuarantineForTesting(25);
    const auto plan = fixture.prepare(
        GuidedNavigationService::Purpose::FlyToHere);
    QSignalSpy finished(
        &fixture.navigation,
        &GuidedNavigationService::operationFinished);
    quint64 operationId = 0;
    bool cancellationCalled = false;
    fixture.writeHook = [&fixture, &operationId, &cancellationCalled](
                            int, const QByteArray &) {
        cancellationCalled = fixture.navigation.cancel(operationId);
        return false;
    };
    QObject owner;
    QCOMPARE(fixture.navigation.execute(
                 &owner, plan, &operationId),
             GuidedNavigationService::SubmitResult::Started);
    QVERIFY(cancellationCalled);
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(fixture.navigation.busy());
    QVERIFY(fixture.guided.hasActiveSession());
    QVERIFY(!fixture.navigation.cancel(operationId + 1));

    QObject otherOwner;
    GuidedTargetService::SessionToken otherSession;
    QCOMPARE(fixture.guided.reserve(
                 &otherOwner, plan.target(), &otherSession),
             GuidedTargetService::RequestResult::Busy);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    const auto report = reportAt(finished, 0);
    QCOMPARE(report.outcome,
             GuidedNavigationService::Outcome::OutcomeUncertain);
    QVERIFY(report.cancellationRequested);
    QVERIFY(report.frameAttempted);
    QVERIFY(!fixture.navigation.busy());
    QVERIFY(!fixture.guided.hasActiveSession());
    QCOMPARE(fixture.guided.reserve(
                 &otherOwner, fixture.targets.acquireTarget(),
                 &otherSession),
             GuidedTargetService::RequestResult::Started);
    QCOMPARE(fixture.guided.stop(otherSession),
             GuidedTargetService::RequestResult::Stopped);
}

void GuidedNavigationServiceTest::
targetAbaAndPostSigningContextChangeFailClosed()
{
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        const auto plan = fixture.prepare(
            GuidedNavigationService::Purpose::Coordinates);
        const VehicleEndpoint alternate = endpoint(8, 43, 1);
        QVERIFY(fixture.targets.observeEndpoint(alternate));
        QVERIFY(fixture.targets.selectTarget(
            alternate.linkId, alternate.systemId,
            alternate.componentId));
        QVERIFY(fixture.targets.selectTarget(
            fixture.target.linkId, fixture.target.systemId,
            fixture.target.componentId));
        QSignalSpy finished(
            &fixture.navigation,
            &GuidedNavigationService::operationFinished);
        QObject owner;
        quint64 operationId = 0;
        QCOMPARE(fixture.navigation.execute(
                     &owner, plan, &operationId),
                 GuidedNavigationService::SubmitResult::Started);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(reportAt(finished, 0).outcome,
                 GuidedNavigationService::Outcome::Rejected);
        QCOMPARE(fixture.frames.size(), 0);
    }

    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        QVERIFY(fixture.setAltitude(42.5,
                                    MAV_FRAME_GLOBAL_RELATIVE_ALT));
        const auto originalContext = fixture.context;
        const auto plan = fixture.prepare(
            GuidedNavigationService::Purpose::Coordinates);
        fixture.transmitter.setFrameSigner(
            [&fixture, originalContext](int, const QByteArray &frame,
                                        QByteArray *signedFrame) {
                GuidedAltitudeStore::Context changed;
                fixture.altitudeStore.commitAltitude(
                    originalContext, 99,
                    MAV_FRAME_GLOBAL_RELATIVE_ALT, &changed);
                *signedFrame = frame;
                return true;
            });
        QSignalSpy finished(
            &fixture.navigation,
            &GuidedNavigationService::operationFinished);
        QObject owner;
        quint64 operationId = 0;
        QCOMPARE(fixture.navigation.execute(
                     &owner, plan, &operationId),
                 GuidedNavigationService::SubmitResult::Started);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(reportAt(finished, 0).outcome,
                 GuidedNavigationService::Outcome::Rejected);
        QVERIFY(!reportAt(finished, 0).frameAttempted);
        QCOMPARE(fixture.frames.size(), 0);
        QVERIFY(!fixture.guided.hasActiveSession());
    }
}

void GuidedNavigationServiceTest::guidedLaneExcludesExistingSession()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    QObject existingOwner;
    GuidedTargetService::SessionToken existingSession;
    QCOMPARE(fixture.guided.reserve(
                 &existingOwner, fixture.targets.acquireTarget(),
                 &existingSession),
             GuidedTargetService::RequestResult::Started);
    const auto plan = fixture.prepare(
        GuidedNavigationService::Purpose::Coordinates);
    QSignalSpy finished(
        &fixture.navigation,
        &GuidedNavigationService::operationFinished);
    QObject uiOwner;
    quint64 operationId = 0;
    QCOMPARE(fixture.navigation.execute(
                 &uiOwner, plan, &operationId),
             GuidedNavigationService::SubmitResult::Started);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(reportAt(finished, 0).outcome,
             GuidedNavigationService::Outcome::Rejected);
    QCOMPARE(fixture.frames.size(), 0);
    QCOMPARE(fixture.guided.activeSession().generation,
             existingSession.generation);
    QCOMPARE(fixture.guided.stop(existingSession),
             GuidedTargetService::RequestResult::Stopped);
}

void GuidedNavigationServiceTest::preparedPlansAreSingleUseAndOwnerScoped()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const auto first = fixture.prepare(
        GuidedNavigationService::Purpose::Coordinates,
        10, 20, 30);
    const auto second = fixture.prepare(
        GuidedNavigationService::Purpose::Coordinates,
        11, 21, 31);
    QVERIFY(fixture.navigation.validate(first));
    QVERIFY(fixture.navigation.validate(second));
    QVERIFY(fixture.navigation.discardPlan(first));
    QVERIFY(!fixture.navigation.validate(first));
    QVERIFY(!fixture.navigation.discardPlan(first));
    quint64 operationId = 123;
    QCOMPARE(fixture.navigation.execute(
                 nullptr, second, &operationId),
             GuidedNavigationService::SubmitResult::InvalidOwner);
    QCOMPARE(operationId, quint64(0));
    QVERIFY(fixture.navigation.validate(second));

    QSignalSpy finished(
        &fixture.navigation,
        &GuidedNavigationService::operationFinished);
    QObject *owner = new QObject;
    fixture.writeHook = [&fixture](int linkId, const QByteArray &) {
        fixture.commands.observePhysicalMessage(
            linkId, fixture.linkEpoch,
            commandAck(fixture.context.vehicle));
        return true;
    };
    QCOMPARE(fixture.navigation.execute(
                 owner, second, &operationId),
             GuidedNavigationService::SubmitResult::Started);
    delete owner;
    QCOMPARE(finished.count(), 1);
    QCOMPARE(fixture.navigation.execute(
                 &fixture.targets, second, nullptr),
             GuidedNavigationService::SubmitResult::InvalidPlan);

    fixture.navigation.setTimeoutsForTesting(5, 10, 0);
    fixture.commands.setExactQuarantineForTesting(10);
    fixture.context = fixture.altitudeStore.current();
    const auto third = fixture.prepare(
        GuidedNavigationService::Purpose::AltitudeUpdate,
        12, 22, -5, MAV_FRAME_GLOBAL, false);
    QVERIFY(third.isValid());
    fixture.writeHook = [](int, const QByteArray &) { return true; };
    owner = new QObject;
    QCOMPARE(fixture.navigation.execute(
                 owner, third, &operationId),
             GuidedNavigationService::SubmitResult::Started);
    delete owner;
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 1000);
    const auto cancelled = reportAt(finished, 1);
    QVERIFY(cancelled.cancellationRequested);
    QCOMPARE(cancelled.outcome,
             GuidedNavigationService::Outcome::OutcomeUncertain);
}

QTEST_GUILESS_MAIN(GuidedNavigationServiceTest)

#include "test_guidednavigationservice.moc"
