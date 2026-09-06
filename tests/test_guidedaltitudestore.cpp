#include "services/GuidedAltitudeStore.h"

#include <QPointer>
#include <QSignalSpy>
#include <QtTest>
#include <cmath>
#include <functional>
#include <limits>

namespace {
VehicleEndpoint endpoint(int link, int system = 1, int component = 1)
{
    VehicleEndpoint value;
    value.linkId = link; value.systemId = system; value.componentId = component;
    value.linkName = QStringLiteral("Physical %1").arg(link);
    return value;
}
mavlink_message_t heartbeat(const VehicleEndpoint &target, bool armed = false)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(quint8(target.systemId), quint8(target.componentId), &message,
        MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0, 0, MAV_STATE_ACTIVE);
    return message;
}
struct Fixture {
    qint64 now = 1000;
    std::function<void()> clockCallback;
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry{[this] {
        const auto callback = clockCallback;
        if (callback) callback();
        return now;
    }};
    GuidedAltitudeStore store{&targets, &registry};
    quint64 admit(const VehicleEndpoint &target, bool select = true)
    {
        auto epoch = registry.currentLinkSessionEpoch(target.linkId);
        if (!epoch) epoch = registry.beginLinkSession(target.linkId);
        registry.observeMessage(target.linkId, epoch, heartbeat(target));
        targets.observeEndpoint(target, false);
        if (select) targets.selectTarget(target.linkId, target.systemId, target.componentId);
        return epoch;
    }
};
}

class GuidedAltitudeStoreTest final : public QObject
{
    Q_OBJECT
private slots:
    void requiresSettledFreshExactInstance()
    {
        Fixture f; GuidedAltitudeStore::Context context; QString error;
        QVERIFY(!f.store.prepareCurrent(&context, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!context.target.isValid()); QVERIFY(!f.store.prepareCurrent(nullptr, &error));
        const auto a = endpoint(7); f.targets.observeEndpoint(a, true);
        QVERIFY(!f.store.prepareCurrent(&context)); // Discovery is not a heartbeat.
        f.admit(a); QVERIFY(f.store.prepareCurrent(&context));
        QVERIFY(!context.altitudeSet); QVERIFY(!context.pointSet); QCOMPARE(context.revision, quint64(0));
        QCOMPARE(context.altitudeM, 0.0f); QCOMPARE(context.frame, MAV_FRAME_GLOBAL_RELATIVE_ALT);
        f.now += f.registry.heartbeatMaximumAgeMs() + 1;
        QVERIFY(!f.store.validate(context)); QVERIFY(!f.store.current().target.isValid());
        f.admit(a); QVERIFY(f.store.prepareCurrent(&context));
        const auto camera = endpoint(7, 1, 100);
        f.targets.observeEndpoint(camera, false); f.targets.selectTarget(7, 1, 100);
        QVERIFY(!f.store.prepareCurrent(&context)); // Never substitutes component 1.

        bool sawUnsettled = false, refused = false;
        connect(&f.targets, &VehicleTargetManager::targetGenerationChanged, &f.store, [&] {
            sawUnsettled = !f.targets.isTargetGenerationSettled();
            refused = !f.store.prepareCurrent(&context);
        });
        f.targets.selectTarget(7, 1, 1);
        QVERIFY(sawUnsettled); QVERIFY(refused); QVERIFY(f.store.prepareCurrent(&context));
    }

    void explicitIntentOnlyAndUnitsIndependent()
    {
        Fixture f; const auto a = endpoint(7); const auto epoch = f.admit(a);
        mavlink_message_t position{};
        mavlink_msg_global_position_int_pack(1, 1, &position, 100, 350000000, 330000000,
            123000, 45000, 0, 0, 0, 0);
        QVERIFY(f.registry.observeMessage(7, epoch, position));
        auto context = f.store.current();
        QVERIFY(!context.altitudeSet); QVERIFY(!context.pointSet);
        QVERIFY(f.store.commitAltitude(context, -12.25, MAV_FRAME_GLOBAL, &context));
        QVERIFY(context.altitudeSet); QCOMPARE(context.altitudeM, -12.25f);
        QCOMPARE(context.frame, MAV_FRAME_GLOBAL); QVERIFY(!context.pointSet);
        QVERIFY(f.store.recordTarget(context, 0, 0, 0, MAV_FRAME_GLOBAL_TERRAIN_ALT, &context));
        QVERIFY(context.pointSet); QCOMPARE(context.latitude, 0.0); QCOMPARE(context.longitude, 0.0);
        QVERIFY(context.altitudeSet); QCOMPARE(context.altitudeM, 0.0f);
        // Altitude-only change retains the explicit point, never current GPS.
        QVERIFY(f.store.commitAltitude(context, 10.1, MAV_FRAME_GLOBAL_RELATIVE_ALT, &context));
        QCOMPARE(context.altitudeM, float(10.1)); QVERIFY(context.pointSet);
        QCOMPARE(context.latitude, 0.0); QCOMPARE(context.longitude, 0.0);
        // Armed state is not a live command gate for this memory-only store.
        QVERIFY(f.registry.observeMessage(7, epoch, heartbeat(a, true)));
        QVERIFY(f.store.commitAltitude(context, -0.0, MAV_FRAME_GLOBAL, &context));
        QVERIFY(std::signbit(context.altitudeM));
    }

    void guidedFreshnessIsThreeSecondsNotRegistryDefault()
    {
        Fixture f; f.admit(endpoint(7));
        const auto context = f.store.current(); QVERIFY(context.target.isValid());
        f.now += 3000;
        QVERIFY(f.store.validate(context));
        ++f.now;
        QVERIFY(f.registry.validateLease(context.vehicle)); // Registry's discovery limit is 5 seconds.
        QVERIFY(!f.store.validate(context));
        QVERIFY(!f.store.commitAltitude(context, 25, MAV_FRAME_GLOBAL));
        QVERIFY(!f.store.current().target.isValid());
    }

    void exactLinksAndSelectionAbaPreserveIntent()
    {
        Fixture f; const auto a = endpoint(7), b = endpoint(8);
        f.admit(a); auto original = f.store.current(); GuidedAltitudeStore::Context aSaved;
        QVERIFY(f.store.recordTarget(original, 35, 33, 50, MAV_FRAME_GLOBAL, &aSaved));
        f.admit(b); auto bContext = f.store.current(); QVERIFY(!bContext.altitudeSet);
        QVERIFY(!f.store.validate(aSaved));
        QVERIFY(f.store.commitAltitude(bContext, 70, MAV_FRAME_GLOBAL_TERRAIN_ALT, &bContext));
        QVERIFY(bContext.revision > aSaved.revision);
        f.targets.selectTarget(7, 1, 1); auto restored = f.store.current();
        QCOMPARE(restored.revision, aSaved.revision); QCOMPARE(restored.altitudeM, 50.0f);
        QCOMPARE(restored.latitude, 35.0); QCOMPARE(restored.longitude, 33.0);
        QVERIFY(restored.vehicle.sameInstance(aSaved.vehicle));
        QVERIFY(restored.target.generation != aSaved.target.generation);
        QVERIFY(!f.store.validate(aSaved)); QVERIFY(!f.store.validate(original));
        QVERIFY(f.store.validate(restored));
        f.targets.selectTarget(8, 1, 1);
        QCOMPARE(f.store.current().altitudeM, 70.0f);
    }

    void physicalEpochAndRediscoveryRetireOnlyMatchingInstance()
    {
        Fixture f; const auto a = endpoint(7), b = endpoint(8);
        const auto epoch = f.admit(a); auto old = f.store.current();
        QVERIFY(f.store.commitAltitude(old, 20, MAV_FRAME_GLOBAL, &old));
        f.admit(b); auto other = f.store.current();
        QVERIFY(f.store.commitAltitude(other, 60, MAV_FRAME_GLOBAL, &other));
        QVERIFY(f.registry.endLinkSession(7, epoch));
        QVERIFY(f.store.validate(other));
        f.admit(a); auto replacement = f.store.current();
        QVERIFY(!replacement.vehicle.sameInstance(old.vehicle)); QVERIFY(!replacement.altitudeSet);
        QCOMPARE(replacement.revision, quint64(0)); QVERIFY(!f.store.validate(old));
        QVERIFY(f.store.commitAltitude(replacement, 80, MAV_FRAME_GLOBAL, &replacement));
        const auto revision = replacement.revision;
        QSignalSpy invalidated(&f.store, &GuidedAltitudeStore::contextInvalidated);
        // Delayed old retirement must not erase a new instance with equal IDs.
        emit f.registry.endpointRetired(old.vehicle, SwarmTelemetryRegistry::RetirementReason::LinkSessionEnded);
        QVERIFY(f.store.validate(replacement)); QCOMPARE(f.store.current().revision, revision);
        QCOMPARE(invalidated.count(), 0);
        QCOMPARE(f.store.current().altitudeM, 80.0f);
        f.targets.selectTarget(8, 1, 1); QCOMPARE(f.store.current().altitudeM, 60.0f);

        f.targets.selectTarget(7, 1, 1); f.now += f.registry.heartbeatMaximumAgeMs() + 1;
        QVERIFY(f.registry.retireStaleEndpoints() > 0);
        f.admit(a); const auto rediscovered = f.store.current();
        QVERIFY(!rediscovered.vehicle.sameInstance(replacement.vehicle));
        QVERIFY(!rediscovered.altitudeSet); QCOMPARE(rediscovered.revision, quint64(0));
    }

    void everySnapshotFieldIsValidated_data()
    {
        QTest::addColumn<int>("field");
        for (int i = 0; i < 14; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void everySnapshotFieldIsValidated()
    {
        QFETCH(int, field);
        Fixture f; f.admit(endpoint(7)); auto original = f.store.current();
        QVERIFY(f.store.recordTarget(original, 10, 20, 30, MAV_FRAME_GLOBAL, &original));
        auto forged = original;
        switch (field) {
        case 0: ++forged.target.generation; break;
        case 1: ++forged.target.endpoint.linkId; break;
        case 2: ++forged.target.endpoint.systemId; break;
        case 3: ++forged.target.endpoint.componentId; break;
        case 4: ++forged.vehicle.instanceEpoch; break;
        case 5: ++forged.vehicle.linkSessionEpoch; break;
        case 6: ++forged.vehicle.endpoint.linkId; break;
        case 7: ++forged.revision; break;
        case 8: forged.altitudeSet = false; break;
        case 9: forged.altitudeM += 1; break;
        case 10: forged.frame = MAV_FRAME_GLOBAL_TERRAIN_ALT; break;
        case 11: forged.pointSet = false; break;
        case 12: forged.latitude += 1; break;
        case 13: forged.longitude += 1; break;
        }
        QString error; QVERIFY(!f.store.validate(forged, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!f.store.commitAltitude(forged, 99, MAV_FRAME_GLOBAL));
        QCOMPARE(f.store.current().revision, original.revision);
        QCOMPARE(f.store.current().altitudeM, 30.0f); QVERIFY(f.store.validate(original));
    }

    void revisionRejectsValueAbaAndUnseenForgery()
    {
        Fixture f; f.admit(endpoint(7)); auto empty = f.store.current(), forged = empty;
        forged.altitudeM = 15; QVERIFY(!f.store.validate(forged));
        forged = empty; forged.latitude = 35; QVERIFY(!f.store.validate(forged));
        auto value = empty; QVERIFY(f.store.commitAltitude(value, 10, MAV_FRAME_GLOBAL, &value));
        const auto first = value;
        QVERIFY(f.store.commitAltitude(value, 20, MAV_FRAME_GLOBAL, &value));
        QVERIFY(f.store.commitAltitude(value, 10, MAV_FRAME_GLOBAL, &value));
        QVERIFY(value.revision > first.revision); QVERIFY(!f.store.validate(first));
        QVERIFY(!f.store.validate(empty));
        const auto before = value.revision;
        QVERIFY(f.store.commitAltitude(value, 10, MAV_FRAME_GLOBAL, &value));
        QVERIFY(value.revision > before); // Even identical explicit commits revoke old snapshots.
        auto renamed = endpoint(7); renamed.linkName = QStringLiteral("Renamed link");
        f.targets.observeEndpoint(renamed, false);
        QVERIFY(f.store.validate(value)); // Presentation metadata is not a target transition.
    }

    void invalidNumbersAndFramesDoNotMutate()
    {
        Fixture f; f.admit(endpoint(7)); const auto context = f.store.current();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (double value : {nan, std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max(), 1e-300}) {
            QVERIFY(!f.store.commitAltitude(context, value, MAV_FRAME_GLOBAL));
            QVERIFY(f.store.validate(context));
        }
        for (MAV_FRAME frame : {MAV_FRAME_LOCAL_NED, MAV_FRAME_MISSION, MAV_FRAME_GLOBAL_INT})
            QVERIFY(!f.store.commitAltitude(context, 10, frame));
        for (const auto &point : {QPair<double, double>(91, 0), {0, 181}, {-91, 0}, {0, -181}, {nan, 0}, {0, nan}})
            QVERIFY(!f.store.recordTarget(context, point.first, point.second, 10, MAV_FRAME_GLOBAL));
        auto accepted = context;
        QVERIFY(f.store.recordTarget(accepted, -90, 180, -double(std::numeric_limits<float>::max()),
            MAV_FRAME_GLOBAL, &accepted));
        QCOMPARE(accepted.altitudeM, -std::numeric_limits<float>::max());
        QVERIFY(f.store.recordTarget(accepted, 90, -180, double(std::numeric_limits<float>::denorm_min()),
            MAV_FRAME_GLOBAL_TERRAIN_ALT, &accepted));
        QCOMPARE(accepted.altitudeM, std::numeric_limits<float>::denorm_min());
    }

    void acquisitionCallbackCannotRetargetOrReplaceIntent()
    {
        Fixture f; const auto a = endpoint(7), b = endpoint(8);
        f.admit(b, false); f.admit(a);
        const auto original = f.store.current(); bool invoked = false;
        f.clockCallback = [&] {
            if (invoked) return;
            invoked = true; f.targets.selectTarget(8, 1, 1); f.targets.selectTarget(7, 1, 1);
        };
        GuidedAltitudeStore::Context output; QVERIFY(!f.store.prepareCurrent(&output));
        QVERIFY(invoked); QVERIFY(!output.target.isValid());
        f.clockCallback = {}; QVERIFY(!f.store.validate(original));
        auto current = f.store.current(); invoked = false;
        f.clockCallback = [&] {
            if (invoked) return;
            invoked = true;
            QVERIFY(f.store.commitAltitude(current, 42, MAV_FRAME_GLOBAL));
        };
        QVERIFY(!f.store.commitAltitude(current, 99, MAV_FRAME_GLOBAL));
        f.clockCallback = {}; QCOMPARE(f.store.current().altitudeM, 42.0f);
        const auto oldInstance = f.store.current(); invoked = false;
        f.clockCallback = [&] {
            if (invoked) return;
            invoked = true;
            QVERIFY(f.registry.endLinkSession(7, oldInstance.vehicle.linkSessionEpoch));
            f.admit(a);
        };
        QVERIFY(!f.store.prepareCurrent(&output)); QVERIFY(invoked);
        f.clockCallback = {};
        QVERIFY(!f.store.validate(oldInstance)); QVERIFY(!f.store.current().altitudeSet);
    }

    void receiptPrecedesReentrantChangeAndDeletion()
    {
        Fixture f; f.admit(endpoint(7));
        auto original = f.store.current(); GuidedAltitudeStore::Context receipt;
        bool observed = false;
        const auto connection = connect(&f.store, &GuidedAltitudeStore::changed, &f.targets, [&] {
            if (observed) return;
            observed = true; QVERIFY(receipt.altitudeSet); QVERIFY(receipt.revision != 0);
            QVERIFY(f.store.commitAltitude(receipt, 80, MAV_FRAME_GLOBAL));
        });
        QVERIFY(f.store.commitAltitude(original, 40, MAV_FRAME_GLOBAL, &receipt));
        QVERIFY(observed); QCOMPARE(receipt.altitudeM, 40.0f);
        QVERIFY(!f.store.validate(receipt)); QCOMPARE(f.store.current().altitudeM, 80.0f);
        disconnect(connection);

        QPointer<GuidedAltitudeStore> store = new GuidedAltitudeStore(&f.targets, &f.registry);
        original = store->current();
        connect(store, &GuidedAltitudeStore::changed, &f.targets, [&] { delete store.data(); });
        QVERIFY(store->commitAltitude(original, 10, MAV_FRAME_GLOBAL, &receipt));
        QVERIFY(!store); QVERIFY(receipt.altitudeSet); QCOMPARE(receipt.altitudeM, 10.0f);
    }

    void dependencyAndAcquisitionDeletionAreSafe()
    {
        qint64 now = 1000;
        auto *targets = new VehicleTargetManager;
        auto *registry = new SwarmTelemetryRegistry([&] { return now; });
        QPointer<GuidedAltitudeStore> store = new GuidedAltitudeStore(targets, registry);
        QSignalSpy invalidated(store, &GuidedAltitudeStore::contextInvalidated);
        delete targets; QVERIFY(!store->current().target.isValid()); QCOMPARE(invalidated.count(), 1);
        delete registry; QCOMPARE(invalidated.count(), 2); delete store.data();

        Fixture f; f.admit(endpoint(7));
        store = new GuidedAltitudeStore(&f.targets, &f.registry);
        f.clockCallback = [&] { delete store.data(); };
        GuidedAltitudeStore::Context output;
        QVERIFY(!store->prepareCurrent(&output)); QVERIFY(!store); QVERIFY(!output.target.isValid());
        f.clockCallback = {};
        store = new GuidedAltitudeStore(&f.targets, &f.registry);
        connect(store, &GuidedAltitudeStore::contextInvalidated, &f.targets, [&] { delete store.data(); });
        f.targets.clearTarget(); QVERIFY(!store);
    }

    void boundedRegistrySizedIntentSet()
    {
        Fixture f;
        for (int system = 1; system <= SwarmTelemetryRegistry::MaximumVehicleEndpoints; ++system) {
            f.admit(endpoint(7, system)); auto context = f.store.current();
            QVERIFY(context.target.isValid());
            QVERIFY(f.store.commitAltitude(context, system, MAV_FRAME_GLOBAL));
        }
        for (int system = 1; system <= SwarmTelemetryRegistry::MaximumVehicleEndpoints; ++system) {
            f.targets.selectTarget(7, system, 1);
            QCOMPARE(f.store.current().altitudeM, float(system));
        }
        const auto epoch = f.registry.currentLinkSessionEpoch(7);
        QVERIFY(f.registry.endLinkSession(7, epoch));
        f.admit(endpoint(8)); QVERIFY(!f.store.current().altitudeSet);
    }
};

QTEST_GUILESS_MAIN(GuidedAltitudeStoreTest)
#include "test_guidedaltitudestore.moc"
