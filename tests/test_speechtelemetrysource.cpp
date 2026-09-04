#include <QtTest>

#include "comm/VehicleTargetManager.h"
#include "services/SpeechTelemetrySource.h"

#include <cmath>
#include <cstring>
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
                            int vehicleType = MAV_TYPE_QUADROTOR,
                            int autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, vehicleType,
        autopilot,
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

mavlink_message_t statusText(int systemId, int componentId, int severity,
                             const QByteArray &text, quint16 id = 0,
                             quint8 chunkSequence = 0)
{
    char field[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN]{};
    std::memcpy(field, text.constData(),
                size_t(qMin(text.size(), int(sizeof(field)))));
    mavlink_message_t message{};
    mavlink_msg_statustext_pack(
        systemId, componentId, &message, quint8(severity), field,
        id, chunkSequence);
    return message;
}
}

class SpeechTelemetrySourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void hasNoSnapshotWithoutCurrentTarget();
    void toleratesAMissingTargetManager();
    void filtersTheExactPhysicalEndpoint();
    void decodesCanonicalTelemetryAndAnyPacketTime();
    void emitsExactModeArmWaypointAndBatteryEvents();
    void invalidSamplesClearSpeedValidity();
    void nestedTargetSelectionPublishesOnlyTheSettledLease();
    void targetGenerationStartsAFreshEpoch();
    void emitsStandaloneStatusTextForTheExactEndpoint();
    void reassemblesChunkedUtf8StatusText();
    void rejectsBrokenChunksAndClearsThemOnTargetChange();
    void rechecksTheLeaseAfterSnapshotCallbacks();
    void rechecksTheLeaseBeforeDerivedEvents();
    void rechecksTheLeaseDuringDerivedSignalCallbacks();
    void normalizesLegacyArduPilotStatusSeverity();
};

void SpeechTelemetrySourceTest::initTestCase()
{
    qRegisterMetaType<ExactStatusText>();
}

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
    QSignalSpy exactSpy(&source,
                        &SpeechTelemetrySource::exactSpeechTelemetry);

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

    QCOMPARE(exactSpy.count(), 6);
    const QList<int> expectedKinds{
        ExactSpeechTelemetryEvent::Armed,
        ExactSpeechTelemetryEvent::FlightMode,
        ExactSpeechTelemetryEvent::Armed,
        ExactSpeechTelemetryEvent::FlightMode,
        ExactSpeechTelemetryEvent::Waypoint,
        ExactSpeechTelemetryEvent::Battery
    };
    for (int index = 0; index < exactSpy.count(); ++index) {
        const ExactSpeechTelemetryEvent event =
            qvariant_cast<ExactSpeechTelemetryEvent>(
                exactSpy.at(index).at(0));
        QCOMPARE(int(event.kind), expectedKinds.at(index));
        QCOMPARE(event.lease.endpoint.linkId, 7);
        QCOMPARE(event.lease.endpoint.systemId, 42);
        QCOMPARE(event.lease.endpoint.componentId, 1);
        QVERIFY(event.lease.generation != 0);
    }
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

void SpeechTelemetrySourceTest::emitsStandaloneStatusTextForTheExactEndpoint()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 100;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    QSignalSpy statusSpy(&source,
                         &SpeechTelemetrySource::statusTextCompleted);

    source.observeMessage(
        8, statusText(42, 1, MAV_SEVERITY_WARNING, "wrong link"));
    source.observeMessage(
        7, statusText(42, 2, MAV_SEVERITY_WARNING, "wrong component"));
    QCOMPARE(statusSpy.count(), 0);

    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_WARNING, "Battery low"));
    QCOMPARE(statusSpy.count(), 1);
    const ExactStatusText event = qvariant_cast<ExactStatusText>(
        statusSpy.takeFirst().at(0));
    QCOMPARE(event.lease.endpoint.linkId, 7);
    QCOMPARE(event.lease.endpoint.systemId, 42);
    QCOMPARE(event.lease.endpoint.componentId, 1);
    QCOMPARE(event.rawSeverity, quint8(MAV_SEVERITY_WARNING));
    QCOMPARE(event.severity, quint8(MAV_SEVERITY_WARNING));
    QCOMPARE(event.messageId, quint16(0));
    QCOMPARE(event.text, QStringLiteral("Battery low"));
    QCOMPARE(event.completedAtMs, qint64(100));
}

void SpeechTelemetrySourceTest::reassemblesChunkedUtf8StatusText()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 10;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    QSignalSpy statusSpy(&source,
                         &SpeechTelemetrySource::statusTextCompleted);

    QByteArray first(49, 'A');
    const QByteArray euro = QString::fromUtf8("\xE2\x82\xAC").toUtf8();
    first.append(euro.left(1));
    QByteArray second = euro.mid(1) + QByteArray(" complete");
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_ERROR, first, 91, 0));
    QCOMPARE(statusSpy.count(), 0);
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_ERROR, second, 91, 1));
    QCOMPARE(statusSpy.count(), 1);
    const ExactStatusText event = qvariant_cast<ExactStatusText>(
        statusSpy.takeFirst().at(0));
    QCOMPARE(event.messageId, quint16(91));
    QCOMPARE(event.text,
             QString(49, QLatin1Char('A'))
                 + QString::fromUtf8("\xE2\x82\xAC")
                 + QStringLiteral(" complete"));

    const QByteArray firstFull(50, 'B');
    const QByteArray secondFull(50, 'C');
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_WARNING, firstFull, 92, 0));
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_WARNING, secondFull, 92, 1));
    QCOMPARE(statusSpy.count(), 0);
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_WARNING, QByteArray(), 92, 2));
    QCOMPARE(statusSpy.count(), 1);
    const ExactStatusText exactMultiple = qvariant_cast<ExactStatusText>(
        statusSpy.takeFirst().at(0));
    QCOMPARE(exactMultiple.text,
             QString(50, QLatin1Char('B'))
                 + QString(50, QLatin1Char('C')));
}

void SpeechTelemetrySourceTest::rejectsBrokenChunksAndClearsThemOnTargetChange()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    qint64 now = 10;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    QSignalSpy statusSpy(&source,
                         &SpeechTelemetrySource::statusTextCompleted);
    const QByteArray full(50, 'x');

    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_WARNING, full, 12, 0));
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_WARNING, "tail", 12, 2));
    QCOMPARE(statusSpy.count(), 0);
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_WARNING, "tail", 12, 1));
    QCOMPARE(statusSpy.count(), 0);

    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_WARNING, full, 13, 0));
    QVERIFY(targets.selectTarget(8, 42, 1));
    source.observeMessage(
        8, statusText(42, 1, MAV_SEVERITY_WARNING, "tail", 13, 1));
    QCOMPARE(statusSpy.count(), 0);
}

void SpeechTelemetrySourceTest::rechecksTheLeaseAfterSnapshotCallbacks()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    qint64 now = 20;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    bool switched = false;
    connect(&source, &SpeechTelemetrySource::snapshotChanged,
            &source, [&]() {
        if (!switched && source.snapshot().lastPacketMs == now) {
            switched = true;
            targets.selectTarget(8, 42, 1);
        }
    });
    QSignalSpy statusSpy(&source,
                         &SpeechTelemetrySource::statusTextCompleted);

    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_ERROR, "stale"));
    QVERIFY(switched);
    QCOMPARE(statusSpy.count(), 0);
    QCOMPARE(source.snapshot().lease.endpoint.linkId, 8);
}

void SpeechTelemetrySourceTest::rechecksTheLeaseBeforeDerivedEvents()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    qint64 now = 30;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    bool switched = false;
    connect(&source, &SpeechTelemetrySource::snapshotChanged,
            &source, [&]() {
        if (!switched && source.snapshot().lastPacketMs == now) {
            switched = true;
            targets.selectTarget(8, 42, 1);
        }
    });
    QSignalSpy armedSpy(&source, &SpeechTelemetrySource::armedChanged);
    QSignalSpy modeSpy(&source, &SpeechTelemetrySource::flightModeChanged);

    source.observeMessage(7, heartbeat(42, 1, true));
    QVERIFY(switched);
    QCOMPARE(armedSpy.count(), 0);
    QCOMPARE(modeSpy.count(), 0);
    QCOMPARE(source.snapshot().lease.endpoint.linkId, 8);
}

void SpeechTelemetrySourceTest::rechecksTheLeaseDuringDerivedSignalCallbacks()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    targets.observeEndpoint(endpoint(8), false);
    qint64 now = 40;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    bool switched = false;
    connect(&source, &SpeechTelemetrySource::armedChanged,
            &source, [&](bool) {
        if (!switched) {
            switched = true;
            targets.selectTarget(8, 42, 1);
        }
    });
    QSignalSpy exactSpy(&source,
                        &SpeechTelemetrySource::exactSpeechTelemetry);

    source.observeMessage(7, heartbeat(42, 1, true));
    QVERIFY(switched);
    QCOMPARE(exactSpy.count(), 0);
    QCOMPARE(source.snapshot().lease.endpoint.linkId, 8);
}

void SpeechTelemetrySourceTest::normalizesLegacyArduPilotStatusSeverity()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(7), true);
    qint64 now = 10;
    SpeechTelemetrySource source(&targets, [&now]() { return now; });
    QSignalSpy statusSpy(&source,
                         &SpeechTelemetrySource::statusTextCompleted);

    source.observeMessage(7, heartbeat(42, 1, false));
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_INFO,
                      "ArduCopter V3.3.0"));
    statusSpy.clear();
    source.observeMessage(
        7, statusText(42, 1, 1, "Legacy low severity"));
    QCOMPARE(statusSpy.count(), 1);
    ExactStatusText event = qvariant_cast<ExactStatusText>(
        statusSpy.takeFirst().at(0));
    QCOMPARE(event.rawSeverity, quint8(1));
    QCOMPARE(event.severity, quint8(MAV_SEVERITY_WARNING));

    QVERIFY(targets.removeLink(7));
    targets.observeEndpoint(endpoint(7), true);
    source.observeMessage(7, heartbeat(42, 1, false));
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_INFO,
                      "ArduCopter V3.4.0"));
    statusSpy.clear();
    source.observeMessage(
        7, statusText(42, 1, 1, "Modern alert"));
    QCOMPARE(statusSpy.count(), 1);
    event = qvariant_cast<ExactStatusText>(statusSpy.takeFirst().at(0));
    QCOMPARE(event.rawSeverity, quint8(1));
    QCOMPARE(event.severity, quint8(1));

    QVERIFY(targets.removeLink(7));
    targets.observeEndpoint(endpoint(7), true);
    source.observeMessage(
        7, heartbeat(42, 1, false, 3, MAV_TYPE_QUADROTOR,
                     MAV_AUTOPILOT_GENERIC));
    source.observeMessage(
        7, statusText(42, 1, MAV_SEVERITY_INFO,
                      "ArduCopter V3.3.0"));
    statusSpy.clear();
    source.observeMessage(
        7, statusText(42, 1, 1, "Generic autopilot alert"));
    QCOMPARE(statusSpy.count(), 1);
    event = qvariant_cast<ExactStatusText>(statusSpy.takeFirst().at(0));
    QCOMPARE(event.rawSeverity, quint8(1));
    QCOMPARE(event.severity, quint8(1));
}

QTEST_APPLESS_MAIN(SpeechTelemetrySourceTest)
#include "test_speechtelemetrysource.moc"
