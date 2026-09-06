#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"

#include <QtTest>
#include <memory>

namespace {
using Service = VehicleCommandService;
using Reservation = Service::ExactReservationToken;
using Token = Service::ExactCommandToken;
using Report = Service::ExactCommandReport;
using Submit = Service::ExactSubmitResult;
using Terminal = Service::ExactTerminalResult;

VehicleEndpoint endpoint(int link = 7, int system = 42, int component = 100)
{
    VehicleEndpoint value;
    value.linkId = link; value.systemId = system; value.componentId = component;
    return value;
}

MavlinkComponentInstanceLease component(int link = 7, quint64 epoch = 11,
                                      quint64 instance = 21)
{ return {endpoint(link), epoch, instance}; }

Service::ExactCommandRequest request(int retries = 0, int timeout = 25, int lifetime = 1000)
{
    Service::ExactCommandRequest value;
    value.command = MAV_CMD_REQUEST_CAMERA_INFORMATION;
    value.maximumRetries = retries;
    value.acknowledgementTimeoutMs = timeout;
    value.maximumLifetimeMs = lifetime;
    return value;
}

mavlink_message_t acknowledgement(int system = 42, int sourceComponent = 100,
    MAV_RESULT result = MAV_RESULT_ACCEPTED, int targetSystem = 250,
    int targetComponent = 190, MAV_CMD command = MAV_CMD_REQUEST_CAMERA_INFORMATION)
{
    mavlink_command_ack_t value{};
    value.command = quint16(command); value.result = quint8(result);
    value.target_system = quint8(targetSystem); value.target_component = quint8(targetComponent);
    value.progress = 50;
    mavlink_message_t message{};
    mavlink_msg_command_ack_encode(quint8(system), quint8(sourceComponent), &message, &value);
    return message;
}

mavlink_command_long_t command(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (char byte : bytes) state = parser.parseByte(quint8(byte), &message);
    if (state != MAVLINK_FRAMING_OK || message.msgid != MAVLINK_MSG_ID_COMMAND_LONG) return {};
    mavlink_command_long_t value{};
    mavlink_msg_command_long_decode(&message, &value);
    return value;
}

Report report(const QSignalSpy &spy, int index = 0)
{ return qvariant_cast<Report>(spy.at(index).at(0)); }

struct Fixture
{
    VehicleTargetManager targets;
    QObject owner;
    QList<MavlinkComponentInstanceLease> active{component()};
    QVector<QPair<int, QByteArray>> frames;
    std::function<void()> onWrite, onLease, onRoute;
    bool routeAvailable = true;
    bool writerAccepted = true;
    ExactLinkTransmitter transmitter;
    std::unique_ptr<Service> service;

    Fixture()
        : transmitter([this](int link, const QByteArray &bytes) {
            frames.append(qMakePair(link, bytes));
            const auto callback = onWrite;
            if (callback) callback();
            return writerAccepted;
        }), service(new Service(&targets, &transmitter))
    {
        service->setLocalIdentity(250, 190);
        service->configureComponentExactTransactions(
            [this](const MavlinkComponentInstanceLease &lease) {
                const auto callback = onLease;
                if (callback) callback();
                return active.contains(lease);
            },
            [this](const MavlinkComponentInstanceLease &, QString *error) {
                const auto callback = onRoute;
                if (callback) callback();
                if (!routeAvailable && error) *error = QStringLiteral("Fixture route unavailable");
                return routeAvailable;
            });
    }

    Reservation reserve(const MavlinkComponentInstanceLease &lease = component())
    {
        Reservation value;
        service->reserveComponentEndpoint(&owner, lease, &value);
        return value;
    }

    Submit start(const Reservation &reservation, const Service::ExactCommandRequest &value,
                 Token *token = nullptr, const MavlinkComponentInstanceLease &lease = component())
    { return service->submitComponentCommandLong(reservation, lease, value, token); }

    void ack(const mavlink_message_t &message = acknowledgement(), int link = 7, quint64 epoch = 11)
    { service->observeComponentMessage(link, epoch, message); }
};
}

class ComponentCommandServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    void peripheralDoesNotBecomeSelectedVehicle()
    {
        Fixture f;
        QVERIFY(f.targets.observeEndpoint(endpoint(7, 42, 1), true));
        const auto target = f.targets.acquireTarget();
        const auto reservation = f.reserve();
        QVERIFY(reservation.isValid());
        QVERIFY(reservation.leases.isEmpty());
        QCOMPARE(reservation.componentLeases.size(), 1);
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        Token token;
        auto value = request(); value.params = {{1, 2, 3, 4, 5, 6, 7}};
        QCOMPARE(f.start(reservation, value, &token), Submit::Started);
        QVERIFY(token.isValid()); QVERIFY(token.isComponentOperation());
        QVERIFY(!token.lease.isValid());
        QCOMPARE(f.frames.size(), 1); QCOMPARE(f.frames.first().first, 7);
        const auto payload = command(f.frames.first().second);
        QCOMPARE(payload.command, quint16(MAV_CMD_REQUEST_CAMERA_INFORMATION));
        QCOMPARE(payload.target_system, quint8(42)); QCOMPARE(payload.target_component, quint8(100));
        QCOMPARE(payload.param1, 1.0f); QCOMPARE(payload.param7, 7.0f);
        QCOMPARE(f.targets.acquireTarget().generation, target.generation);
        QVERIFY(f.targets.acquireTarget().endpoint.sameIdentity(target.endpoint));
        f.ack(); QCOMPARE(done.size(), 1);
        QCOMPARE(report(done).terminalResult, Terminal::AcknowledgedAccepted);
    }

    void invalidStaleMixedDomainsAndRetryBoundsFailClosed()
    {
        Fixture f;
        Reservation out;
        QCOMPARE(f.service->reserveComponentEndpoint(nullptr, component(), &out),
                 Service::ExactReservationResult::InvalidOwner);
        for (const auto &lease : {component(8), component(7, 12), component(7, 11, 22),
                                 MavlinkComponentInstanceLease{}}) {
            QVERIFY(f.service->reserveComponentEndpoint(&f.owner, lease, &out)
                    != Service::ExactReservationResult::Reserved);
            QVERIFY(!out.isValid());
        }
        const auto reservation = f.reserve(); QVERIFY(reservation.isValid());
        Token token;
        for (int retries : {-1, 4}) {
            QCOMPARE(f.start(reservation, request(retries), &token), Submit::InvalidCommand);
            QVERIFY(!token.isValid());
        }
        Reservation forged = reservation;
        forged.leases.append({endpoint(), 11, 21});
        QVERIFY(!forged.isValid());
        QVERIFY(f.start(forged, request(), &token) != Submit::Started);
        QVERIFY(f.service->submitExactCommandLong(reservation, {endpoint(), 11, 21}, request(), &token)
                != Submit::Started);
        QVERIFY(f.frames.isEmpty());
    }

    void acknowledgementRequiresPhysicalEpochAndCompleteEnvelope()
    {
        Fixture f; const auto reservation = f.reserve();
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        QCOMPARE(f.start(reservation, request(0, 1000)), Submit::Started);
        f.ack(acknowledgement(), 8, 11); f.ack(acknowledgement(), 7, 12);
        f.ack(acknowledgement(), 7, 0);
        f.ack(acknowledgement(43)); f.ack(acknowledgement(42, 101));
        f.ack(acknowledgement(42, 100, MAV_RESULT_ACCEPTED, 251));
        f.ack(acknowledgement(42, 100, MAV_RESULT_ACCEPTED, 250, 191));
        f.ack(acknowledgement(42, 100, MAV_RESULT_ACCEPTED, 250, 190, MAV_CMD_VIDEO_START_STREAMING));
        f.service->observeMessage(7, acknowledgement());
        QCOMPARE(done.size(), 0);
        f.ack(acknowledgement(42, 100, MAV_RESULT_DENIED, 0, 0));
        QCOMPARE(done.size(), 1); QCOMPARE(report(done).terminalResult, Terminal::AcknowledgedRejected);
    }

    void physicalThenLegacyIngressCannotCompleteReentrantSuccessor()
    {
        Fixture f; const auto reservation = f.reserve();
        Token first, second;
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        Submit replacement = Submit::ContextUnavailable;
        connect(f.service.get(), &Service::exactCommandFinished, this, [&](const Report &value) {
            if (value.token.transactionId == first.transactionId)
                replacement = f.start(reservation, request(0, 1000), &second);
        });
        QCOMPARE(f.start(reservation, request(0, 1000), &first), Submit::Started);
        f.ack(); QCOMPARE(replacement, Submit::Started);
        QVERIFY(second.transactionId != first.transactionId);
        f.service->observeMessage(7, acknowledgement());
        QCOMPARE(done.size(), 1);
        f.ack(); QCOMPARE(done.size(), 2);
        QCOMPARE(report(done, 1).token.transactionId, second.transactionId);
    }

    void endpointOwnershipIsSharedAcrossDomainsAndLinksAreIndependent()
    {
        Fixture f;
        QVERIFY(f.service->configureExactTransactions(
            [](const SwarmVehicleInstanceLease &) { return true; },
            [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
        const auto reservation = f.reserve(); QVERIFY(reservation.isValid());
        Reservation conflicting;
        QCOMPARE(f.service->reserveExactEndpoints(&f.owner, {{endpoint(), 11, 21}}, &conflicting),
                 Service::ExactReservationResult::Busy);
        f.active.append(component(8));
        const auto other = f.reserve(component(8)); QVERIFY(other.isValid());
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        QCOMPARE(f.start(reservation, request(0, 1000)), Submit::Started);
        QCOMPARE(f.start(other, request(0, 1000), nullptr, component(8)), Submit::Started);
        f.ack(acknowledgement(), 8); QCOMPARE(done.size(), 1);
        QCOMPARE(report(done).token.componentLease.endpoint.linkId, 8);
        f.ack(); QCOMPARE(done.size(), 2);
    }

    void synchronousWriterAcknowledgementPublishesTokenAndAttemptReceipt()
    {
        Fixture f; const auto reservation = f.reserve(); Token token;
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        bool tokenWasPublished = false;
        f.onWrite = [&] { tokenWasPublished = token.isValid(); f.ack(); };
        QCOMPARE(f.start(reservation, request(), &token), Submit::Started);
        QVERIFY(tokenWasPublished); QCOMPARE(done.size(), 1);
        QVERIFY(report(done).frameAttempted); QCOMPARE(report(done).transmissionAttempts, 1);
        QCOMPARE(report(done).token.transactionId, token.transactionId);
    }

    void synchronousRetirementHasUncertainAttemptButSigningRejectionDoesNot()
    {
        Fixture f; const auto reservation = f.reserve();
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        f.onWrite = [&] { f.service->retireComponent(component()); };
        f.start(reservation, request());
        QCOMPARE(done.size(), 1);
        QCOMPARE(report(done).terminalResult, Terminal::LeaseRetiredOutcomeUncertain);
        QVERIFY(report(done).frameAttempted); QCOMPARE(report(done).transmissionAttempts, 1);

        Fixture rejected; const auto r = rejected.reserve();
        QSignalSpy rejectedDone(rejected.service.get(), &Service::exactCommandFinished);
        rejected.transmitter.setSigningRequired(7, true);
        QCOMPARE(rejected.start(r, request()), Submit::ContextUnavailable);
        QCOMPARE(rejectedDone.size(), 1); QVERIFY(rejected.frames.isEmpty());
        QCOMPARE(report(rejectedDone).terminalResult, Terminal::RejectedBeforeTransmission);
        QVERIFY(!report(rejectedDone).frameAttempted);
        QCOMPARE(report(rejectedDone).transmissionAttempts, 0);
    }

    void retriesKeepTokenAndUseConfirmationsZeroThroughThree()
    {
        Fixture f; const auto reservation = f.reserve(); Token token;
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        QCOMPARE(f.start(reservation, request(3), &token), Submit::Started);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1500);
        QCOMPARE(f.frames.size(), 4);
        for (int index = 0; index < f.frames.size(); ++index) {
            const auto payload = command(f.frames[index].second);
            QCOMPARE(payload.confirmation, quint8(index));
            QCOMPARE(payload.target_component, quint8(100));
        }
        QCOMPARE(report(done).token.transactionId, token.transactionId);
        QCOMPARE(report(done).transmissionAttempts, 4);
        QCOMPARE(report(done).terminalResult, Terminal::TimedOutOutcomeUncertain);
    }

    void progressDisablesRetriesAndCannotExtendAbsoluteLifetime()
    {
        Fixture f; const auto reservation = f.reserve();
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        QCOMPARE(f.start(reservation, request(3, 30, 120)), Submit::Started);
        f.ack(acknowledgement(42, 100, MAV_RESULT_IN_PROGRESS));
        QTimer progress;
        connect(&progress, &QTimer::timeout, this, [&] {
            f.ack(acknowledgement(42, 100, MAV_RESULT_IN_PROGRESS));
        });
        progress.start(5);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
        QCOMPARE(f.frames.size(), 1);
        QCOMPARE(report(done).terminalResult, Terminal::TimedOutOutcomeUncertain);
    }

    void retryRevalidatesSafetyGateAndAbsoluteDeadline()
    {
        for (bool delayPastDeadline : {false, true}) {
            Fixture f; const auto reservation = f.reserve();
            QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
            int calls = 0;
            auto value = request(3, 20, delayPastDeadline ? 70 : 1000);
            value.validateBeforeWrite = [&](QString *) {
                ++calls;
                if (f.frames.isEmpty()) return true;
                if (delayPastDeadline) { QThread::msleep(80); return true; }
                return false;
            };
            QCOMPARE(f.start(reservation, value), Submit::Started);
            QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
            QCOMPARE(calls, 3); QCOMPARE(f.frames.size(), 1);
            QCOMPARE(report(done).transmissionAttempts, 1);
            QCOMPARE(report(done).terminalResult, delayPastDeadline
                ? Terminal::TimedOutOutcomeUncertain : Terminal::TransportOutcomeUncertain);
        }
    }

    void detachedOwnerPreventsRetriesButPreservesReceipt()
    {
        Fixture f; std::unique_ptr<QObject> owner(new QObject);
        Reservation reservation;
        QCOMPARE(f.service->reserveComponentEndpoint(owner.get(), component(), &reservation),
                 Service::ExactReservationResult::Reserved);
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        QCOMPARE(f.start(reservation, request(3)), Submit::Started);
        owner.reset();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
        QCOMPARE(f.frames.size(), 1); QVERIFY(report(done).ownerDetached);
        QVERIFY(report(done).frameAttempted);
    }

    void staleComponentRetirementCannotFinishNewInstance()
    {
        Fixture f; const auto old = f.reserve();
        QVERIFY(f.service->releaseExactReservation(old));
        f.active = {component(7, 11, 22)};
        const auto replacement = f.reserve(f.active.first());
        QVERIFY(replacement.isValid());
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        QCOMPARE(f.start(replacement, request(0, 1000), nullptr, f.active.first()), Submit::Started);
        f.service->retireComponent(component());
        QCOMPARE(done.size(), 0);
        f.service->forgetLink(8); QCOMPARE(done.size(), 0);
        f.service->forgetLink(7); QCOMPARE(done.size(), 1);
        QCOMPARE(report(done).terminalResult, Terminal::LinkForgottenOutcomeUncertain);
    }

    void acknowledgementValidationCannotCrossAbsoluteDeadline()
    {
        Fixture f; const auto reservation = f.reserve();
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        QCOMPARE(f.start(reservation, request(0, 1000, 30)), Submit::Started);
        f.onLease = [] { QThread::msleep(40); };
        f.ack();
        QCOMPARE(done.size(), 1);
        QCOMPARE(report(done).terminalResult, Terminal::TimedOutOutcomeUncertain);
    }

    void postSignerInvalidationPreventsInitialAndRetryWrites_data()
    {
        QTest::addColumn<QString>("mutation");
        QTest::addColumn<bool>("retry");
        for (const char *mutation : {"retire", "cancel", "owner", "stale", "route", "delete"}) {
            QTest::newRow(qPrintable(QString::fromLatin1(mutation) + QStringLiteral("-initial")))
                << QString::fromLatin1(mutation) << false;
            QTest::newRow(qPrintable(QString::fromLatin1(mutation) + QStringLiteral("-retry")))
                << QString::fromLatin1(mutation) << true;
        }
    }

    void postSignerInvalidationPreventsInitialAndRetryWrites()
    {
        QFETCH(QString, mutation); QFETCH(bool, retry);
        Fixture f;
        std::unique_ptr<QObject> owner(new QObject);
        Reservation reservation;
        QCOMPARE(f.service->reserveComponentEndpoint(owner.get(), component(), &reservation),
                 Service::ExactReservationResult::Reserved);
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        const QPointer<Service> guard(f.service.get());
        int signings = 0;
        f.transmitter.setFrameSigner([&](int, const QByteArray &frame, QByteArray *signedFrame) {
            *signedFrame = frame;
            ++signings;
            if (retry && signings == 1) return true;
            if (mutation == QLatin1String("retire")) f.service->retireComponent(component());
            else if (mutation == QLatin1String("cancel")) f.service->releaseExactReservation(reservation);
            else if (mutation == QLatin1String("owner")) owner.reset();
            else if (mutation == QLatin1String("stale")) f.active.clear();
            else if (mutation == QLatin1String("route")) f.routeAvailable = false;
            else f.service.reset();
            return true;
        });
        f.start(reservation, request(3));
        if (retry) QTRY_VERIFY_WITH_TIMEOUT(signings >= 2, 1000);
        QCOMPARE(signings, retry ? 2 : 1);
        QCOMPARE(f.frames.size(), retry ? 1 : 0);
        if (mutation == QLatin1String("delete")) {
            QVERIFY(!guard);
        } else {
            QCOMPARE(done.size(), 1);
            QCOMPARE(report(done).transmissionAttempts, retry ? 1 : 0);
            QCOMPARE(report(done).frameAttempted, retry);
            if (!retry) QCOMPARE(report(done).terminalResult, Terminal::RejectedBeforeTransmission);
            else QVERIFY(report(done).terminalResult != Terminal::RejectedBeforeTransmission);
        }
    }

    void postSignerValidationCallbacksAreGuarded()
    {
        for (const char *boundary : {"lease", "route", "safety"}) {
            Fixture f; const auto reservation = f.reserve();
            const QPointer<Service> guard(f.service.get());
            bool signedFrameReady = false;
            f.transmitter.setFrameSigner([&](int, const QByteArray &frame, QByteArray *out) {
                *out = frame; signedFrameReady = true; return true;
            });
            const auto destroy = [&] { if (signedFrameReady) f.service.reset(); };
            auto value = request();
            if (qstrcmp(boundary, "lease") == 0) f.onLease = destroy;
            else if (qstrcmp(boundary, "route") == 0) f.onRoute = destroy;
            else value.validateBeforeWrite = [&](QString *) { destroy(); return true; };
            f.start(reservation, value);
            QVERIFY(!guard); QVERIFY(f.frames.isEmpty());
        }
    }

    void synchronousRetryWriterFailurePreservesAttemptCount()
    {
        Fixture f; const auto reservation = f.reserve();
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        f.onWrite = [&] { if (f.frames.size() == 2) f.writerAccepted = false; };
        QCOMPARE(f.start(reservation, request(3)), Submit::Started);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
        QCOMPARE(f.frames.size(), 2); QCOMPARE(report(done).transmissionAttempts, 2);
        QVERIFY(report(done).frameAttempted);
        QCOMPARE(report(done).terminalResult, Terminal::TransportOutcomeUncertain);
    }

    void physicalDispatcherDoesNotReuseAckAcrossDomains()
    {
        Fixture f;
        QVERIFY(f.service->configureExactTransactions(
            [](const SwarmVehicleInstanceLease &) { return true; },
            [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
        const auto firstReservation = f.reserve(); Token first, successor;
        Reservation successorReservation;
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        Service::ExactReservationResult reserved = Service::ExactReservationResult::Busy;
        Submit submitted = Submit::Busy;
        connect(f.service.get(), &Service::exactCommandFinished, this, [&](const Report &value) {
            if (value.token.transactionId != first.transactionId) return;
            f.service->releaseExactReservation(firstReservation);
            reserved = f.service->reserveExactEndpoints(&f.owner, {{endpoint(), 11, 21}}, &successorReservation);
            submitted = f.service->submitExactCommandLong(successorReservation,
                {endpoint(), 11, 21}, request(0, 1000), &successor);
        });
        QCOMPARE(f.start(firstReservation, request(0, 1000), &first), Submit::Started);
        f.service->observePhysicalMessage(7, 11, acknowledgement());
        QCOMPARE(reserved, Service::ExactReservationResult::Reserved);
        QCOMPARE(submitted, Submit::Started);
        QCOMPARE(done.size(), 1); QVERIFY(!successor.isComponentOperation());
        f.service->observePhysicalMessage(7, 12, acknowledgement());
        QCOMPARE(done.size(), 1);
        f.service->observePhysicalMessage(7, 11, acknowledgement());
        QCOMPARE(done.size(), 2); QCOMPARE(report(done, 1).token.transactionId, successor.transactionId);
    }

    void signingCannotAcknowledgeAnUntransmittedFirstFrame()
    {
        Fixture f; const auto reservation = f.reserve();
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        f.transmitter.setFrameSigner([&](int, const QByteArray &frame, QByteArray *out) {
            *out = frame; f.ack(); return true;
        });
        QCOMPARE(f.start(reservation, request(0, 1000)), Submit::Started);
        QCOMPARE(f.frames.size(), 1); QCOMPARE(done.size(), 0);
        f.ack(); QCOMPARE(done.size(), 1);
    }

    void lateAcknowledgementQuarantineIsScopedToPhysicalEpoch()
    {
        Fixture f; const auto reservation = f.reserve();
        QSignalSpy done(f.service.get(), &Service::exactCommandFinished);
        QCOMPARE(f.start(reservation, request()), Submit::Started);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
        QCOMPARE(f.start(reservation, request()), Submit::Quarantined);
        f.ack(acknowledgement(), 7, 12);
        QCOMPARE(f.start(reservation, request()), Submit::Quarantined);
        f.service->observeMessage(7, acknowledgement());
        QCOMPARE(f.start(reservation, request()), Submit::Quarantined);
        f.ack();
        QCOMPARE(f.start(reservation, request(0, 1000)), Submit::Started);
        f.ack(); QCOMPARE(done.size(), 2);
    }

    void callbacksMayDeleteService_data()
    {
        QTest::addColumn<QString>("where");
        for (const char *where : {"reserve-lease", "submit-route", "writer", "retry-route", "ack-lease", "finished"})
            QTest::newRow(where) << QString::fromLatin1(where);
    }

    void callbacksMayDeleteService()
    {
        QFETCH(QString, where);
        Fixture f; const QPointer<Service> guard(f.service.get());
        const auto destroy = [&] { f.service.reset(); };
        if (where == QLatin1String("reserve-lease")) {
            f.onLease = destroy; f.reserve(); QVERIFY(!guard); QVERIFY(f.frames.isEmpty()); return;
        }
        const auto reservation = f.reserve(); QVERIFY(reservation.isValid());
        if (where == QLatin1String("submit-route")) f.onRoute = destroy;
        if (where == QLatin1String("writer")) f.onWrite = destroy;
        if (where == QLatin1String("finished"))
            connect(f.service.get(), &Service::exactCommandFinished, this, destroy);
        f.start(reservation, request(3));
        if (where == QLatin1String("retry-route")) {
            f.onRoute = destroy;
            QTRY_VERIFY_WITH_TIMEOUT(!guard, 1000);
        } else if (where == QLatin1String("ack-lease")) {
            f.onLease = destroy; f.ack();
        } else if (where == QLatin1String("finished")) f.ack();
        QVERIFY(!guard);
        QCOMPARE(f.frames.size(), where == QLatin1String("submit-route") ? 0 : 1);
    }
};

QTEST_GUILESS_MAIN(ComponentCommandServiceTest)
#include "test_componentcommandservice.moc"
