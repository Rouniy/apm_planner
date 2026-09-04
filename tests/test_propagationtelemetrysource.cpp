#include <QtTest>

#include "comm/VehicleTargetManager.h"
#include "ui/tools/PropagationTelemetrySource.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace
{
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

mavlink_message_t homePosition(int systemId, int componentId,
                               qint32 latitudeE7 = 351800000,
                               qint32 longitudeE7 = 333800000,
                               qint32 altitudeMm = 120000)
{
    const float orientation[4]{1.0F, 0.0F, 0.0F, 0.0F};
    mavlink_message_t message{};
    mavlink_msg_home_position_pack(
        systemId, componentId, &message,
        latitudeE7, longitudeE7, altitudeMm,
        0.0F, 0.0F, 0.0F, orientation,
        0.0F, 0.0F, 0.0F, 0);
    return message;
}

mavlink_message_t globalPosition(int systemId, int componentId,
                                 qint32 longitudeE7,
                                 qint32 latitudeE7 = 351856000,
                                 qint32 altitudeMm = 125500)
{
    mavlink_message_t message{};
    mavlink_msg_global_position_int_pack(
        systemId, componentId, &message, 0,
        latitudeE7, longitudeE7, altitudeMm, 30500,
        0, 0, 0, 9000);
    return message;
}

mavlink_message_t gpsRaw(int systemId, int componentId, quint8 fixType)
{
    mavlink_message_t message{};
    mavlink_msg_gps_raw_int_pack(
        systemId, componentId, &message, 0, fixType,
        351856000, 333823000, 125500,
        100, 100, 0, 0, 12,
        0, 0, 0, 0, 0, 0);
    return message;
}

mavlink_message_t systemStatus(int systemId, int componentId,
                               qint16 currentCentiAmps,
                               qint8 remaining)
{
    mavlink_message_t message{};
    mavlink_msg_sys_status_pack(
        systemId, componentId, &message,
        0, 0, 0, 0, 12000, currentCentiAmps, remaining,
        0, 0, 0, 0, 0, 0);
    return message;
}

mavlink_message_t batteryStatus(int systemId, int componentId,
                                quint8 batteryId,
                                qint32 consumedMah,
                                qint16 currentCentiAmps,
                                qint8 remaining)
{
    quint16 voltages[MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_LEN];
    quint16 voltagesExt[MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_EXT_LEN];
    std::fill(std::begin(voltages), std::end(voltages),
              std::numeric_limits<quint16>::max());
    std::fill(std::begin(voltagesExt), std::end(voltagesExt),
              std::numeric_limits<quint16>::max());
    mavlink_message_t message{};
    mavlink_msg_battery_status_pack(
        systemId, componentId, &message,
        batteryId, MAV_BATTERY_FUNCTION_ALL, MAV_BATTERY_TYPE_LIPO,
        std::numeric_limits<qint16>::max(), voltages,
        currentCentiAmps, consumedMah, -1,
        remaining, 0, MAV_BATTERY_CHARGE_STATE_UNDEFINED,
        voltagesExt, 0, 0);
    return message;
}
}

class PropagationTelemetrySourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void filtersAndDecodesTheExactPhysicalTarget();
    void ignoresSecondaryBatteryAndIntegratesSysStatusCurrent();
    void targetAndDisconnectEpochsClearAllDerivedState();
    void reentrantTargetSwitchCannotContaminateTheNewEpoch();
};

void PropagationTelemetrySourceTest::
filtersAndDecodesTheExactPhysicalTarget()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 0;
    PropagationTelemetrySource source(&targets, [&now]() { return now; });
    QSignalSpy changed(&source, &PropagationTelemetrySource::snapshotChanged);

    source.observeMessage(8, heartbeat(42, 1, true));
    source.observeMessage(7, heartbeat(43, 1, true));
    source.observeMessage(7, heartbeat(42, 2, true));
    QCOMPARE(changed.count(), 0);

    source.observeMessage(7, heartbeat(42, 1, true));
    source.observeMessage(7, gpsRaw(42, 1, 3));
    source.observeMessage(7, homePosition(42, 1));
    source.observeMessage(7, batteryStatus(42, 1, 0, 120, 500, 80));
    source.observeMessage(7, globalPosition(42, 1, 333823000));
    now = 1000;
    source.observeMessage(7, globalPosition(42, 1, 333833000));

    const PropagationTelemetrySnapshot state = source.snapshot();
    QVERIFY(state.isValid());
    QCOMPARE(state.lease.endpoint.linkId, 7);
    QVERIFY(state.heartbeatValid);
    QVERIFY(state.armed);
    QVERIFY(state.homeValid);
    QCOMPARE(state.homeLatitude, 35.18);
    QCOMPARE(state.homeLongitude, 33.38);
    QCOMPARE(state.homeAltitudeAmslM, 120.0);
    QVERIFY(state.positionValid);
    QCOMPARE(state.latitude, 35.1856);
    QCOMPARE(state.longitude, 33.3833);
    QCOMPARE(state.altitudeAmsl, 125.5);
    QCOMPARE(state.gpsFixType, 3);
    QVERIFY(state.batteryRemainingValid);
    QCOMPARE(state.batteryRemainingPercent, 80.0);
    QVERIFY(state.usedMahValid);
    QCOMPARE(state.usedMah, 120.0);
    QVERIFY(state.batteryKilometresLeftValid);
    QVERIFY(state.batteryKilometresLeft > 0.0);
}

void PropagationTelemetrySourceTest::
ignoresSecondaryBatteryAndIntegratesSysStatusCurrent()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 0;
    PropagationTelemetrySource source(&targets, [&now]() { return now; });
    source.observeMessage(7, heartbeat(42, 1, true));
    source.observeMessage(7, gpsRaw(42, 1, 3));
    source.observeMessage(7, globalPosition(42, 1, 333823000));

    source.observeMessage(7, batteryStatus(42, 1, 1, 900, 1000, 75));
    QVERIFY(!source.snapshot().batteryRemainingValid);
    QVERIFY(!source.snapshot().usedMahValid);

    source.observeMessage(7, systemStatus(42, 1, 1000, 75));
    QVERIFY(source.snapshot().batteryRemainingValid);
    QVERIFY(!source.snapshot().usedMahValid);
    now = 3600;
    source.observeMessage(7, systemStatus(42, 1, 1000, 75));
    source.observeMessage(7, globalPosition(42, 1, 333833000));
    PropagationTelemetrySnapshot state = source.snapshot();
    QVERIFY(state.usedMahValid);
    QVERIFY(std::abs(state.usedMah - 10.0) < 1.0e-9);
    QVERIFY(state.batteryKilometresLeftValid);

    now = 4000;
    source.observeMessage(7, batteryStatus(42, 1, 0, 250, 500, 70));
    state = source.snapshot();
    QCOMPARE(state.usedMah, 250.0);
    QCOMPARE(state.batteryRemainingPercent, 70.0);
}

void PropagationTelemetrySourceTest::
targetAndDisconnectEpochsClearAllDerivedState()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    qint64 now = 0;
    PropagationTelemetrySource source(&targets, [&now]() { return now; });
    source.observeMessage(7, heartbeat(42, 1, true));
    source.observeMessage(7, homePosition(42, 1));
    source.observeMessage(7, batteryStatus(42, 1, 0, 100, 500, 80));
    source.observeMessage(7, gpsRaw(42, 1, 3));
    source.observeMessage(7, globalPosition(42, 1, 333823000));
    const quint64 firstEpoch = source.snapshot().telemetryEpoch;
    QVERIFY(source.snapshot().homeValid);
    QVERIFY(source.snapshot().usedMahValid);

    QVERIFY(targets.selectTarget(8, 42, 1));
    PropagationTelemetrySnapshot state = source.snapshot();
    QCOMPARE(state.lease.endpoint.linkId, 8);
    QVERIFY(state.telemetryEpoch > firstEpoch);
    QVERIFY(!state.homeValid);
    QVERIFY(!state.positionValid);
    QVERIFY(!state.usedMahValid);

    source.observeMessage(7, homePosition(42, 1));
    QVERIFY(!source.snapshot().homeValid);
    source.observeMessage(8, heartbeat(42, 1, false));
    source.observeMessage(8, homePosition(42, 1));
    QVERIFY(source.snapshot().homeValid);

    const quint64 secondEpoch = source.snapshot().telemetryEpoch;
    source.observeLinkDisconnected(7);
    QCOMPARE(source.snapshot().telemetryEpoch, secondEpoch);
    source.observeLinkDisconnected(8);
    state = source.snapshot();
    QCOMPARE(state.lease.endpoint.linkId, 8);
    QVERIFY(state.telemetryEpoch > secondEpoch);
    QVERIFY(!state.heartbeatValid);
    QVERIFY(!state.homeValid);
    QVERIFY(!state.batteryKilometresLeftValid);
    QVERIFY(qIsNaN(state.batteryKilometresLeft));

    QVERIFY(targets.removeLink(8));
    QVERIFY(!source.snapshot().isValid());
}

void PropagationTelemetrySourceTest::
reentrantTargetSwitchCannotContaminateTheNewEpoch()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    bool switchDuringClockRead = false;
    PropagationTelemetrySource source(&targets, [&]() {
        if (switchDuringClockRead) {
            switchDuringClockRead = false;
            targets.selectTarget(8, 42, 1);
        }
        return qint64(100);
    });

    switchDuringClockRead = true;
    source.observeMessage(7, globalPosition(42, 1, 333823000));
    const PropagationTelemetrySnapshot state = source.snapshot();
    QCOMPARE(state.lease.endpoint.linkId, 8);
    QVERIFY(!state.positionValid);
    QVERIFY(!state.usedMahValid);
}

QTEST_APPLESS_MAIN(PropagationTelemetrySourceTest)
#include "test_propagationtelemetrysource.moc"
