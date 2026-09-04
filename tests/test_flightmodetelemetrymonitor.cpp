#include "comm/VehicleTargetManager.h"
#include "ui/configuration/FlightModeTelemetryMonitor.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <limits>

class FlightModeTelemetryMonitorTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactEnvelopeAndHeartbeatBoundary();
    void channelsAndRawPortOffsets();
    void targetGenerationInvalidatesMonitor();
};

namespace {
VehicleEndpoint endpoint(int linkId = 7, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    return result;
}

mavlink_message_t heartbeat(int systemId = 42, int componentId = 1,
                            quint32 customMode = 7, bool armed = false)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, MAV_TYPE_QUADROTOR,
        MAV_AUTOPILOT_ARDUPILOTMEGA,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        customMode, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t channels(int systemId = 42, int componentId = 1,
                           quint16 channel5 = 1500)
{
    mavlink_message_t message{};
    mavlink_msg_rc_channels_pack(
        systemId, componentId, &message, 10, 18,
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(), channel5,
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(),
        std::numeric_limits<quint16>::max(), 90);
    return message;
}
} // namespace

void FlightModeTelemetryMonitorTest::exactEnvelopeAndHeartbeatBoundary()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(), true));
    FlightModeTelemetryMonitor monitor(&targets, targets.acquireTarget());
    QSignalSpy heartbeatSpy(&monitor,
        &FlightModeTelemetryMonitor::heartbeatObserved);
    QSignalSpy rcSpy(&monitor,
        &FlightModeTelemetryMonitor::rcInputObserved);

    monitor.observeMessage(7, channels());
    QCOMPARE(rcSpy.count(), 0);
    monitor.observeMessage(8, heartbeat());
    monitor.observeMessage(7, heartbeat(41, 1));
    monitor.observeMessage(7, heartbeat(42, 2));
    QCOMPARE(heartbeatSpy.count(), 0);

    monitor.observeMessage(7, heartbeat(42, 1, 11, true));
    QCOMPARE(heartbeatSpy.count(), 1);
    QCOMPARE(heartbeatSpy.takeFirst(),
             QList<QVariant>({11U, true,
                 static_cast<int>(MAV_AUTOPILOT_ARDUPILOTMEGA),
                 static_cast<int>(MAV_TYPE_QUADROTOR)}));

    monitor.observeMessage(7, channels(42, 1, 1491));
    QCOMPARE(rcSpy.count(), 1);
    QCOMPARE(rcSpy.takeFirst(), QList<QVariant>({5, 1491}));
}

void FlightModeTelemetryMonitorTest::channelsAndRawPortOffsets()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(), true));
    FlightModeTelemetryMonitor monitor(&targets, targets.acquireTarget());
    QSignalSpy rcSpy(&monitor,
        &FlightModeTelemetryMonitor::rcInputObserved);
    monitor.observeMessage(7, heartbeat());
    monitor.observeMessage(7, channels(42, 1, 1230));
    QCOMPARE(rcSpy.takeFirst(), QList<QVariant>({5, 1230}));

    mavlink_message_t rawMessage{};
    const quint16 unavailable = std::numeric_limits<quint16>::max();
    mavlink_msg_rc_channels_raw_pack(
        42, 1, &rawMessage, 20, 1,
        1750, unavailable, unavailable, unavailable,
        unavailable, unavailable, unavailable, unavailable, 50);
    monitor.observeMessage(7, rawMessage);
    QCOMPARE(rcSpy.count(), 1);
    QCOMPARE(rcSpy.takeFirst(), QList<QVariant>({9, 1750}));
}

void FlightModeTelemetryMonitorTest::targetGenerationInvalidatesMonitor()
{
    VehicleTargetManager targets;
    QVERIFY(targets.observeEndpoint(endpoint(), true));
    const VehicleTargetLease lease = targets.acquireTarget();
    FlightModeTelemetryMonitor monitor(&targets, lease);
    QSignalSpy invalidatedSpy(&monitor,
        &FlightModeTelemetryMonitor::targetInvalidated);
    QSignalSpy heartbeatSpy(&monitor,
        &FlightModeTelemetryMonitor::heartbeatObserved);

    monitor.observeMessage(7, heartbeat());
    QCOMPARE(heartbeatSpy.count(), 1);
    QVERIFY(targets.observeEndpoint(endpoint(8, 42, 1)));
    QVERIFY(targets.selectTarget(8, 42, 1));
    QCOMPARE(invalidatedSpy.count(), 1);
    QVERIFY(monitor.invalidated());

    monitor.observeMessage(7, heartbeat());
    QCOMPARE(heartbeatSpy.count(), 1);
}

QTEST_APPLESS_MAIN(FlightModeTelemetryMonitorTest)
#include "test_flightmodetelemetrymonitor.moc"
