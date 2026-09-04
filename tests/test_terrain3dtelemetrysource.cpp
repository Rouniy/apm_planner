#include <QtTest>

#include "comm/VehicleTargetManager.h"
#include "ui/terrain/Terrain3DTelemetrySource.h"

#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;

VehicleEndpoint endpoint(int linkId, int systemId = 42, int componentId = 1)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = componentId;
    return value;
}

mavlink_message_t heartbeat(int systemId, int componentId, bool armed)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, MAV_TYPE_QUADROTOR,
        MAV_AUTOPILOT_ARDUPILOTMEGA,
        quint8(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED
               | (armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0)),
        4, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t globalPosition(int systemId, int componentId)
{
    mavlink_message_t message{};
    mavlink_msg_global_position_int_pack(
        systemId, componentId, &message, 0,
        351856000, 333823000, 125500, 30500,
        600, -400, 125, 9000);
    return message;
}

mavlink_message_t attitude(int systemId, int componentId)
{
    mavlink_message_t message{};
    mavlink_msg_attitude_pack(
        systemId, componentId, &message, 0,
        float(10.0 * kPi / 180.0),
        float(-12.0 * kPi / 180.0),
        float(-45.0 * kPi / 180.0), 0, 0, 0);
    return message;
}
}

class Terrain3DTelemetrySourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void filtersAndDecodesExactTarget();
    void targetSwitchClearsTheSnapshot();
    void reentrantTargetSwitchCannotPublishStaleState();
    void formatterCannotContaminateANewTargetEpoch();
};

void Terrain3DTelemetrySourceTest::filtersAndDecodesExactTarget()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 100;
    Terrain3DTelemetrySource source(
        &targets, [&now]() { return now; },
        [](int, int, quint32, quint8) {
            return QStringLiteral("Guided");
        });
    QSignalSpy changed(&source, &Terrain3DTelemetrySource::snapshotChanged);
    changed.clear();

    source.observeMessage(8, globalPosition(42, 1));
    source.observeMessage(7, globalPosition(43, 1));
    source.observeMessage(7, globalPosition(42, 2));
    QCOMPARE(changed.count(), 0);

    source.observeMessage(7, heartbeat(42, 1, true));
    now = 250;
    source.observeMessage(7, globalPosition(42, 1));
    source.observeMessage(7, attitude(42, 1));
    const Terrain3DTelemetrySnapshot state = source.snapshot();
    QCOMPARE(changed.count(), 3);
    QVERIFY(state.heartbeatValid);
    QVERIFY(state.armed);
    QCOMPARE(state.mode, QStringLiteral("Guided"));
    QVERIFY(state.positionValid);
    QCOMPARE(state.latitude, 35.1856);
    QCOMPARE(state.longitude, 33.3823);
    QCOMPARE(state.altitudeAmslM, 125.5);
    QCOMPARE(state.altitudeRelativeM, 30.5);
    QCOMPARE(state.velocityNorthMps, 6.0);
    QCOMPARE(state.velocityEastMps, -4.0);
    QCOMPARE(state.velocityUpMps, -1.25);
    QCOMPARE(state.positionObservedMs, qint64(250));
    QVERIFY(state.attitudeValid);
    QVERIFY(std::abs(state.rollDeg - 10.0) < 0.001);
    QVERIFY(std::abs(state.pitchDeg + 12.0) < 0.001);
    QVERIFY(std::abs(state.yawDeg - 315.0) < 0.001);

    now = 400;
    source.observeMessage(7, globalPosition(42, 1));
    QCOMPARE(source.snapshot().positionObservedMs, qint64(400));
}

void Terrain3DTelemetrySourceTest::targetSwitchClearsTheSnapshot()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    qint64 now = 10;
    Terrain3DTelemetrySource source(&targets, [&now]() { return now; });
    source.observeMessage(7, globalPosition(42, 1));
    QVERIFY(source.snapshot().positionValid);

    QVERIFY(targets.selectTarget(8, 42, 1));
    QVERIFY(source.snapshot().isValid());
    QCOMPARE(source.snapshot().lease.endpoint.linkId, 8);
    QVERIFY(!source.snapshot().positionValid);
    QVERIFY(!source.snapshot().attitudeValid);

    source.observeMessage(7, globalPosition(42, 1));
    QVERIFY(!source.snapshot().positionValid);
    source.observeMessage(8, globalPosition(42, 1));
    QVERIFY(source.snapshot().positionValid);

    QVERIFY(targets.removeLink(8));
    QVERIFY(!source.snapshot().isValid());
}

void Terrain3DTelemetrySourceTest::reentrantTargetSwitchCannotPublishStaleState()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    qint64 now = 10;
    Terrain3DTelemetrySource source(&targets, [&now]() { return now; });
    int callbacks = 0;
    connect(&source, &Terrain3DTelemetrySource::snapshotChanged,
            &source, [&]() {
        ++callbacks;
        if (source.snapshot().lease.endpoint.linkId == 7
            && source.snapshot().positionValid) {
            targets.selectTarget(8, 42, 1);
        }
    });

    source.observeMessage(7, globalPosition(42, 1));
    QVERIFY(callbacks >= 2);
    QCOMPARE(source.snapshot().lease.endpoint.linkId, 8);
    QVERIFY(!source.snapshot().positionValid);
}

void Terrain3DTelemetrySourceTest::formatterCannotContaminateANewTargetEpoch()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    Terrain3DTelemetrySource source(
        &targets, {},
        [&targets](int, int, quint32, quint8) {
            targets.selectTarget(8, 42, 1);
            return QStringLiteral("Stale mode");
        });

    source.observeMessage(7, heartbeat(42, 1, true));
    const Terrain3DTelemetrySnapshot state = source.snapshot();
    QCOMPARE(state.lease.endpoint.linkId, 8);
    QVERIFY(!state.heartbeatValid);
    QVERIFY(!state.armed);
    QVERIFY(state.mode.isEmpty());
}

QTEST_APPLESS_MAIN(Terrain3DTelemetrySourceTest)
#include "test_terrain3dtelemetrysource.moc"
