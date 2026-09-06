#include "comm/ExactLinkTransmitter.h"
#include "comm/GuidedTargetService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "services/GuidedAltitudeStore.h"
#include "services/GuidedNavigationService.h"

#include <QSignalSpy>
#include <QtTest>

namespace {
VehicleEndpoint endpoint(int link)
{
    VehicleEndpoint value;
    value.linkId = link; value.systemId = 42; value.componentId = 1;
    return value;
}
struct Fixture {
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    GuidedAltitudeStore store{&targets, &registry};
    QList<QByteArray> frames;
    ExactLinkTransmitter transmitter{[this](int, const QByteArray &bytes) {
        frames.append(bytes); return true;
    }};
    VehicleCommandService commands{&targets, &transmitter};
    GuidedTargetService guided{&targets, &commands};
    GuidedNavigationService navigation{&store, &guided, &commands};
    bool initialize()
    {
        commands.setLocalIdentity(250, 190); guided.setLocalIdentity(250, 190);
        commands.setExactQuarantineForTesting(1000);
        if (!commands.configureExactTransactions(
                [this](const SwarmVehicleInstanceLease &lease) { return registry.validateLease(lease, 3000); },
                [](const SwarmVehicleInstanceLease &, QString *) { return true; })
            || !commands.configureSingleVehicleExactRoute(
                [](const SwarmVehicleInstanceLease &, QString *) { return true; })) return false;
        return select(7);
    }
    bool select(int link)
    {
        const auto target = endpoint(link);
        auto epoch = registry.currentLinkSessionEpoch(link);
        if (!epoch) epoch = registry.beginLinkSession(link);
        transmitter.setLinkSessionEpoch(link, epoch);
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(42, 1, &message, MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_ACTIVE);
        if (!registry.observeMessage(link, epoch, message)) return false;
        targets.observeEndpoint(target, false);
        targets.selectTarget(link, 42, 1);
        targets.observeHeartbeat(target, false, MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR);
        return targets.acquireTarget().endpoint.sameIdentity(target);
    }
};
}

class GuidedNavigationAdmissionTest final : public QObject
{
    Q_OBJECT
private slots:
    void exactReservationStopsReservedGuidedSubmitAsDefiniteBusy()
    {
        Fixture f; QVERIFY(f.initialize());
        QObject exactOwner, guidedOwner;
        const auto target = f.targets.acquireTarget();
        GuidedTargetService::SessionToken session;
        QCOMPARE(f.guided.reserve(&guidedOwner, target, &session),
            GuidedTargetService::RequestResult::Started);
        VehicleCommandService::ExactReservationToken reservation;
        QCOMPARE(f.commands.reserveExactEndpoints(&exactOwner,
            {f.registry.acquireVehicle(target.endpoint)}, &reservation),
            VehicleCommandService::ExactReservationResult::Reserved);
        QSignalSpy ended(&f.guided, &GuidedTargetService::sessionEnded);
        QCOMPARE(f.guided.submit(session, {35, 33, 25}), GuidedTargetService::RequestResult::Busy);
        QCOMPARE(ended.count(), 1);
        QVERIFY(f.frames.isEmpty());
        QVERIFY(!f.guided.hasActiveSession());
        QVERIFY(!f.guided.isOutcomeUncertain(target.endpoint));
        QVERIFY(GuidedTargetService::resultDescription(GuidedTargetService::RequestResult::Busy)
            .contains(QStringLiteral("exact command channel")));
        QVERIFY(f.commands.releaseExactReservation(reservation));
    }

    void reservedExactEndpointBlocksLegacyGuidedAdmission()
    {
        Fixture f; QVERIFY(f.initialize());
        QObject exactOwner, followMeOwner;
        const auto target = f.targets.acquireTarget();
        const auto vehicle = f.registry.acquireVehicle(target.endpoint);
        VehicleCommandService::ExactReservationToken reservation;
        QCOMPARE(f.commands.reserveExactEndpoints(&exactOwner, {vehicle}, &reservation),
            VehicleCommandService::ExactReservationResult::Reserved);
        QVERIFY(f.commands.isExactEndpointBusy(target.endpoint, MAV_CMD_DO_REPOSITION));
        GuidedTargetService::SessionToken followMe;
        QCOMPARE(f.guided.reserve(&followMeOwner, target, &followMe), GuidedTargetService::RequestResult::Busy);
        QVERIFY(!followMe.isValid()); QVERIFY(!f.guided.hasActiveSession());
        QVERIFY(!f.guided.isOutcomeUncertain(target.endpoint)); QVERIFY(f.frames.isEmpty());
        QVERIFY(f.commands.releaseExactReservation(reservation));
        QVERIFY(!f.commands.isExactEndpointBusy(target.endpoint, MAV_CMD_DO_REPOSITION));
        QCOMPARE(f.guided.reserve(&followMeOwner, target, &followMe), GuidedTargetService::RequestResult::Started);
        QVERIFY(followMe.isValid()); QVERIFY(f.frames.isEmpty());
        QCOMPARE(f.guided.stop(followMe), GuidedTargetService::RequestResult::Stopped);
    }

    void navigationTargetAbaCannotBypassLateAckQuarantine()
    {
        Fixture f; QVERIFY(f.initialize());
        QObject navigationOwner, followMeOwner;
        auto context = f.store.current();
        QVERIFY(f.store.commitAltitude(context, 25, MAV_FRAME_GLOBAL_RELATIVE_ALT, &context));
        GuidedNavigationService::Plan plan;
        QVERIFY(f.navigation.prepare(context, 35, 33, 25, MAV_FRAME_GLOBAL_RELATIVE_ALT,
            true, GuidedNavigationService::Purpose::FlyToHere, &plan));
        quint64 operation = 0;
        QCOMPARE(f.navigation.execute(&navigationOwner, plan, &operation), GuidedNavigationService::SubmitResult::Started);
        QVERIFY(operation != 0); QCOMPARE(f.frames.size(), 1); QVERIFY(f.navigation.busy());
        GuidedTargetService::SessionToken followMe;
        QCOMPARE(f.guided.reserve(&followMeOwner, context.target, &followMe), GuidedTargetService::RequestResult::Busy);
        QVERIFY(!followMe.isValid()); QCOMPARE(f.frames.size(), 1);

        QVERIFY(f.select(8)); QVERIFY(f.select(7));
        const auto returnedTarget = f.targets.acquireTarget();
        QVERIFY(returnedTarget.generation != context.target.generation);
        QVERIFY(f.registry.acquireVehicle(endpoint(7)).sameInstance(context.vehicle));
        // The selected-target transition ends the old Reserved guided session;
        // its independent exact ACK quarantine must still block a new owner.
        QVERIFY(!f.guided.hasActiveSession());
        QVERIFY(f.commands.isExactCommandQuarantined(context.vehicle, MAV_CMD_DO_REPOSITION));
        QVERIFY(f.commands.isExactEndpointBusy(endpoint(7), MAV_CMD_DO_REPOSITION));
        QCOMPARE(f.guided.reserve(&followMeOwner, returnedTarget, &followMe), GuidedTargetService::RequestResult::Busy);
        QVERIFY(!followMe.isValid()); QVERIFY(!f.guided.isOutcomeUncertain(endpoint(7)));
        QCOMPARE(f.frames.size(), 1);

        QTRY_VERIFY(!f.commands.isExactEndpointBusy(endpoint(7), MAV_CMD_DO_REPOSITION));
        QTRY_VERIFY(!f.navigation.busy());
        QVERIFY(f.select(7));
        QCOMPARE(f.guided.reserve(&followMeOwner, f.targets.acquireTarget(), &followMe), GuidedTargetService::RequestResult::Started);
        QVERIFY(followMe.isValid()); QCOMPARE(f.frames.size(), 1);
        QCOMPARE(f.guided.stop(followMe), GuidedTargetService::RequestResult::Stopped);
        QCOMPARE(f.navigation.lastReport().operationId, operation);
        QVERIFY(f.navigation.lastReport().frameAttempted);
        QVERIFY(!f.navigation.lastReport().targetRecorded);
    }

    void selectionIndependentExactPendingAlsoBlocksReturnedGuidedOwner()
    {
        Fixture f; QVERIFY(f.initialize());
        QObject oldGuidedOwner, exactOwner, nextGuidedOwner;
        GuidedTargetService::SessionToken oldGuided, replacement;
        const auto target = f.targets.acquireTarget();
        const auto vehicle = f.registry.acquireVehicle(target.endpoint);
        QCOMPARE(f.guided.reserve(&oldGuidedOwner, target, &oldGuided), GuidedTargetService::RequestResult::Started);
        VehicleCommandService::ExactReservationToken reservation;
        QCOMPARE(f.commands.reserveExactEndpoints(&exactOwner, {vehicle}, &reservation),
            VehicleCommandService::ExactReservationResult::Reserved);
        VehicleCommandService::ExactCommandIntRequest request;
        request.command = MAV_CMD_DO_REPOSITION; request.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        request.x = 350000000; request.y = 330000000; request.z = 25;
        request.acknowledgementTimeoutMs = 1000; request.maximumLifetimeMs = 5000;
        VehicleCommandService::ExactCommandToken token;
        QCOMPARE(f.commands.submitExactCommandInt(reservation, vehicle, request, &token),
            VehicleCommandService::ExactSubmitResult::Started);
        QVERIFY(token.isValid()); QCOMPARE(f.frames.size(), 1);
        QSignalSpy reports(&f.commands, &VehicleCommandService::exactCommandFinished);
        QVERIFY(f.select(8)); QVERIFY(f.select(7));
        QCOMPARE(reports.count(), 0); // Group-domain operation remains pending across selection changes.
        QVERIFY(!f.guided.hasActiveSession());
        QVERIFY(f.commands.isExactEndpointBusy(target.endpoint, MAV_CMD_DO_REPOSITION));
        QCOMPARE(f.guided.reserve(&nextGuidedOwner, f.targets.acquireTarget(), &replacement), GuidedTargetService::RequestResult::Busy);
        QVERIFY(!replacement.isValid()); QVERIFY(!f.guided.isOutcomeUncertain(target.endpoint));
        QCOMPARE(f.frames.size(), 1);
        QVERIFY(f.commands.releaseExactReservation(reservation));
        QTRY_COMPARE(reports.count(), 1);
        QVERIFY(f.commands.isExactEndpointBusy(target.endpoint, MAV_CMD_DO_REPOSITION));
        QCOMPARE(f.guided.reserve(&nextGuidedOwner, f.targets.acquireTarget(), &replacement), GuidedTargetService::RequestResult::Busy);
        QTRY_VERIFY(!f.commands.isExactEndpointBusy(target.endpoint, MAV_CMD_DO_REPOSITION));
        QVERIFY(f.select(7));
        QCOMPARE(f.guided.reserve(&nextGuidedOwner, f.targets.acquireTarget(), &replacement), GuidedTargetService::RequestResult::Started);
        QCOMPARE(f.frames.size(), 1);
        QCOMPARE(f.guided.stop(replacement), GuidedTargetService::RequestResult::Stopped);
    }
};

QTEST_GUILESS_MAIN(GuidedNavigationAdmissionTest)
#include "test_guidednavigation_admission.moc"
