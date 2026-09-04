#include <QtTest>

#include "comm/VehicleTargetManager.h"
#include "services/SpeechTelemetrySource.h"

#include <cmath>
#include <limits>

namespace {
VehicleEndpoint endpoint(int linkId, int systemId = 42, int componentId = 1)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = componentId;
    return value;
}

mavlink_message_t heartbeat(int systemId, int componentId, bool armed,
                            quint32 customMode = 3,
                            int vehicleType = MAV_TYPE_QUADROTOR)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, vehicleType,
        MAV_AUTOPILOT_ARDUPILOTMEGA,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        customMode, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t systemStatus(int systemId, int componentId,
                               quint16 millivolts, qint8 remaining)
{
    mavlink_message_t message{};
    mavlink_msg_sys_status_pack(
        systemId, componentId, &message,
        0, 0, 0, 0, millivolts, -1, remaining,
        0, 0, 0, 0, 0, 0);
    return message;
}

mavlink_message_t globalPosition(int systemId, int componentId,
                                 qint32 relativeAltitudeMm,
                                 qint16 northCms, qint16 eastCms)
{
    mavlink_message_t message{};
    mavlink_msg_global_position_int_pack(
        systemId, componentId, &message, 0, 0, 0, 0,
        relativeAltitudeMm, northCms, eastCms, 0, 0);
    return message;
}

mavlink_message_t vfrHud(int systemId, int componentId,
                         float airspeed, float groundSpeed)
{
    mavlink_message_t message{};
    mavlink_msg_vfr_hud_pack(systemId, componentId, &message,
                             airspeed, groundSpeed, 0, 0, 0.0F, 0.0F);
    return message;
}

mavlink_message_t missionCurrent(int systemId, int componentId, quint16 seq)
{
    mavlink_message_t message{};
    mavlink_msg_mission_current_pack(systemId, componentId, &message, seq);
    return message;
}
}

class SpeechTelemetrySourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void hasNoSnapshotWithoutCurrentTarget();
    void toleratesAMissingTargetManager();
    void filtersTheExactPhysicalEndpoint();
    void decodesCanonicalTelemetryAndAnyPacketTime();
    void emitsExactModeArmWaypointAndBatteryEvents();
    void invalidSamplesClearSpeedValidity();
    void nestedTargetSelectionPublishesOnlyTheSettledLease();
    void targetGenerationStartsAFreshEpoch();
};

void SpeechTelemetrySourceTest::hasNoSnapshotWithoutCurrentTarget()
{
    VehicleTargetManager targets;
    qint64 now = 100;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });

    QVERIFY(!source.snapshot().isValid());
    source.observeMessage(7, heartbeat(42, 1, true));
    QVERIFY(!source.snapshot().isValid());
    QCOMPARE(source.snapshot().lastPacketMs, qint64(-1));
}

void SpeechTelemetrySourceTest::emitsExactModeArmWaypointAndBatteryEvents()
{
    QCOMPARE(SpeechTelemetrySource::modeText(
                 MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR,
                 3, MAV_MODE_FLAG_CUSTOM_MODE_ENABLED),
             QStringLiteral("Auto"));
    QCOMPARE(SpeechTelemetrySource::modeText(
                 MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_FIXED_WING,
                 11, MAV_MODE_FLAG_CUSTOM_MODE_ENABLED),
             QStringLiteral("RTL"));

    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 10;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    QSignalSpy armSpy(&source, &SpeechTelemetrySource::armedChanged);
    QSignalSpy modeSpy(&source, &SpeechTelemetrySource::flightModeChanged);
    QSignalSpy waypointSpy(&source, &SpeechTelemetrySource::waypointChanged);
    QSignalSpy batterySpy(
        &source, &SpeechTelemetrySource::batteryTelemetryChanged);

    source.observeMessage(8, heartbeat(42, 1, true));
    QCOMPARE(armSpy.count(), 0);
    QCOMPARE(modeSpy.count(), 0);

    source.observeMessage(7, heartbeat(42, 1, true));
    QCOMPARE(armSpy.count(), 1);
    QCOMPARE(armSpy.takeFirst().at(0).toBool(), true);
    QCOMPARE(modeSpy.count(), 1);
    QCOMPARE(modeSpy.takeFirst().at(0).toString(), QStringLiteral("Auto"));

    source.observeMessage(7, heartbeat(42, 1, true));
    QCOMPARE(armSpy.count(), 0);
    QCOMPARE(modeSpy.count(), 0);
    source.observeMessage(7, heartbeat(42, 1, false, 4));
    QCOMPARE(armSpy.count(), 1);
    QCOMPARE(armSpy.takeFirst().at(0).toBool(), false);
    QCOMPARE(modeSpy.count(), 1);
    QCOMPARE(modeSpy.takeFirst().at(0).toString(), QStringLiteral("Guided"));

    source.observeMessage(7, missionCurrent(42, 1, 6));
    QCOMPARE(waypointSpy.count(), 1);
    QCOMPARE(waypointSpy.takeFirst().at(0).toInt(), 6);
    source.observeMessage(7, systemStatus(42, 1, 11400, 32));
    QCOMPARE(batterySpy.count(), 1);
    const QList<QVariant> battery = batterySpy.takeFirst();
    QCOMPARE(battery.at(0).toDouble(), 11.4);
    QCOMPARE(battery.at(1).toDouble(), 32.0);
}

void SpeechTelemetrySourceTest::toleratesAMissingTargetManager()
{
    qint64 now = 100;
    SpeechTelemetrySource source(nullptr, [&now]() { return now; });
    QVERIFY(!source.snapshot().isValid());
    source.observeMessage(7, heartbeat(42, 1, true));
    QVERIFY(!source.snapshot().isValid());
}

void SpeechTelemetrySourceTest::filtersTheExactPhysicalEndpoint()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 100;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });

    source.observeMessage(8, heartbeat(42, 1, true));
    source.observeMessage(7, heartbeat(43, 1, true));
    source.observeMessage(7, heartbeat(42, 2, true));
    QVERIFY(!source.snapshot().heartbeatValid);
    QCOMPARE(source.snapshot().lastPacketMs, qint64(-1));

    now = 120;
    source.observeMessage(7, heartbeat(42, 1, true));
    QVERIFY(source.snapshot().heartbeatValid);
    QVERIFY(source.snapshot().armed);
    QCOMPARE(source.snapshot().lastPacketMs, qint64(120));
}

void SpeechTelemetrySourceTest::invalidSamplesClearSpeedValidity()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 10;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });

    source.observeMessage(7, vfrHud(42, 1, 12.0F, 4.0F));
    QVERIFY(source.snapshot().airspeedValid);
    QVERIFY(source.snapshot().groundSpeedValid);

    now = 20;
    source.observeMessage(
        7, vfrHud(42, 1,
                  std::numeric_limits<float>::quiet_NaN(),
                  std::numeric_limits<float>::quiet_NaN()));
    QVERIFY(!source.snapshot().airspeedValid);
    QVERIFY(!source.snapshot().groundSpeedValid);
    QCOMPARE(source.snapshot().lastPacketMs, qint64(20));

    now = 30;
    source.observeMessage(7, globalPosition(42, 1, 1000, 300, 400));
    QVERIFY(source.snapshot().groundSpeedValid);
    source.observeMessage(
        7, globalPosition(42, 1, 1000,
                          std::numeric_limits<qint16>::max(),
                          std::numeric_limits<qint16>::max()));
    QVERIFY(!source.snapshot().groundSpeedValid);
}

void SpeechTelemetrySourceTest::nestedTargetSelectionPublishesOnlyTheSettledLease()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    targets.observeEndpoint(endpoint(9), false);
    QObject::connect(
        &targets, &VehicleTargetManager::targetGenerationChanged,
        &targets, [&targets](qulonglong) {
        const VehicleTargetLease lease = targets.acquireTarget();
        if (lease.endpoint.linkId == 8) {
            targets.selectTarget(9, 42, 1);
        }
    });

    qint64 now = 100;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    QList<int> publishedValidLinks;
    QObject::connect(&source, &SpeechTelemetrySource::snapshotChanged,
                     &source, [&source, &publishedValidLinks]() {
        const auto state = source.snapshot();
        if (state.isValid()) {
            publishedValidLinks.append(state.lease.endpoint.linkId);
        }
    });

    now = 200;
    QVERIFY(targets.selectTarget(8, 42, 1));
    QCOMPARE(source.snapshot().lease.endpoint.linkId, 9);
    QCOMPARE(publishedValidLinks, QList<int>{9});
}

void SpeechTelemetrySourceTest::decodesCanonicalTelemetryAndAnyPacketTime()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 1000;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    QCOMPARE(source.snapshot().connectedSinceMs, qint64(1000));

    now = 1010;
    source.observeMessage(7, globalPosition(42, 1, 123450, 300, 400));
    auto state = source.snapshot();
    QVERIFY(state.altitudeValid);
    QCOMPARE(state.altitudeMeters, 123.45);
    QVERIFY(state.groundSpeedValid);
    QCOMPARE(state.groundSpeedMps, 5.0);
    QCOMPARE(state.lastPacketMs, qint64(1010));

    now = 1020;
    source.observeMessage(7, vfrHud(42, 1, 17.5F, 6.25F));
    state = source.snapshot();
    QVERIFY(state.airspeedValid);
    QCOMPARE(state.airspeedMps, 17.5);
    QCOMPARE(state.groundSpeedMps, 6.25);

    now = 1030;
    source.observeMessage(7, missionCurrent(42, 1, 9));
    state = source.snapshot();
    QVERIFY(state.waypointValid);
    QCOMPARE(state.waypointNumber, 9);
    QCOMPARE(state.lastPacketMs, qint64(1030));
}

void SpeechTelemetrySourceTest::targetGenerationStartsAFreshEpoch()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    qint64 now = 10;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    source.observeMessage(7, heartbeat(42, 1, true));
    source.observeMessage(7, vfrHud(42, 1, 12.0F, 4.0F));

    const quint64 firstGeneration = source.snapshot().lease.generation;
    now = 50;
    QVERIFY(targets.selectTarget(8, 42, 1));
    auto state = source.snapshot();
    QVERIFY(state.isValid());
    QCOMPARE(state.lease.endpoint.linkId, 8);
    QVERIFY(state.lease.generation != firstGeneration);
    QCOMPARE(state.connectedSinceMs, qint64(50));
    QCOMPARE(state.lastPacketMs, qint64(-1));
    QVERIFY(!state.heartbeatValid);
    QVERIFY(!state.airspeedValid);

    now = 60;
    source.observeMessage(7, heartbeat(42, 1, true));
    QVERIFY(!source.snapshot().heartbeatValid);
    source.observeMessage(8, heartbeat(42, 1, false));
    QVERIFY(source.snapshot().heartbeatValid);
    QVERIFY(!source.snapshot().armed);

    now = 80;
    QVERIFY(targets.removeLink(8));
    QVERIFY(!source.snapshot().isValid());
    QCOMPARE(source.snapshot().connectedSinceMs, qint64(-1));
    QCOMPARE(source.snapshot().lastPacketMs, qint64(-1));
}

QTEST_APPLESS_MAIN(SpeechTelemetrySourceTest)
#include "test_speechtelemetrysource.moc"
