#include <QtTest>

#include "GAudioOutput.h"
#include "services/SpeechAnnouncer.h"
#include "services/SpeechSettings.h"
#include "services/StatusMessageSettings.h"
#include "ui/flightdata/FlightDataViewModel.h"

#include <QSettings>
#include <QTemporaryDir>

namespace {
ExactStatusText statusEvent(const SpeechAnnouncer::VehicleState &vehicle,
                            int severity, const QString &text, qint64 now)
{
    ExactStatusText event;
    event.lease.endpoint.linkId = vehicle.linkId;
    event.lease.endpoint.systemId = vehicle.systemId;
    event.lease.endpoint.componentId = vehicle.componentId;
    event.lease.generation = vehicle.generation;
    event.rawSeverity = quint8(severity);
    event.severity = quint8(severity);
    event.text = text;
    event.completedAtMs = now;
    return event;
}
}

// The injectable constructor does not use these production dependencies.
// Small definitions keep this unit test isolated from the full vehicle/audio
// object graphs while still compiling the production constructor.
GAudioOutput *GAudioOutput::instance()
{
    return nullptr;
}

bool GAudioOutput::say(QString, int)
{
    return false;
}

bool GAudioOutput::isSpeechIdle() const
{
    return false;
}

void GAudioOutput::stopSpeech()
{
}

UASInterface *FlightDataViewModel::activeUAS() const
{
    return nullptr;
}

class SpeechAnnouncerTest final : public QObject
{
    Q_OBJECT

private slots:
    void eventsUsePolicyTemplatesAndCurrentVehicle();
    void inactiveVehicleSuppressesAllEvents();
    void batteryCadenceUsesSuccessfulSpeech();
    void formatsExactTelemetryInSelectedDisplayUnits();
    void periodicCustomHonoursCadenceReadinessAndGeneration();
    void noDataUsesStrictGraceAgeAndRepeatBoundaries();
    void noDataBannerDoesNotDependOnSpeechMaster();
    void altitudeWarningRequiresAThresholdCrossing();
    void altitudeStateTracksWhileSpeechCannotRun();
    void lowSpeedUsesCanonicalSiAndAirspeedPriority();
    void highStatusUsesThresholdExactLeaseAndTenSecondLifetime();
    void highStatusRetriesAndRoutesSpecialSpeechOnce();
    void customWarningsRepeatOnceAndRespectSpeechGates();
};

void SpeechAnnouncerTest::eventsUsePolicyTemplatesAndCurrentVehicle()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("speech.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    settings.setEnabled(true);
    settings.setArmedOnly(true);
    settings.setModeEnabled(true);
    settings.setWaypointEnabled(true);
    settings.setArmDisarmEnabled(true);
    settings.setModeTemplate(QStringLiteral("UAS {sysid}: {mode}"));
    settings.setWaypointTemplate(QStringLiteral("UAS {sysid}: WP {wpn}"));
    settings.setDisarmTemplate(QStringLiteral("UAS {sysid}: disarmed"));

    QStringList spoken;
    qint64 now = 0;
    SpeechAnnouncer::VehicleState vehicle{42, true, true};
    SpeechAnnouncer announcer(
        &settings,
        [&spoken](const QString &message) {
            spoken.append(message);
            return true;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; });

    announcer.announceFlightMode(QStringLiteral("AUTO"));
    vehicle.systemId = 77;
    announcer.announceWaypoint(4);
    vehicle.armed = false;
    announcer.announceFlightMode(QStringLiteral("LOITER"));
    announcer.announceWaypoint(5);
    announcer.announceArmState(false);

    QCOMPARE(spoken, QStringList({QStringLiteral("UAS 42: AUTO"),
                                  QStringLiteral("UAS 77: WP 4"),
                                  QStringLiteral("UAS 77: disarmed")}));
}

void SpeechAnnouncerTest::inactiveVehicleSuppressesAllEvents()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("speech.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    settings.setEnabled(true);
    settings.setModeEnabled(true);
    settings.setWaypointEnabled(true);
    settings.setArmDisarmEnabled(true);
    settings.setBatteryEnabled(true);

    QStringList spoken;
    qint64 now = 30001;
    SpeechAnnouncer announcer(
        &settings,
        [&spoken](const QString &message) {
            spoken.append(message);
            return true;
        },
        []() { return SpeechAnnouncer::VehicleState{}; },
        [&now]() { return now; });

    now = 60002;
    announcer.announceFlightMode(QStringLiteral("AUTO"));
    announcer.announceWaypoint(3);
    announcer.announceArmState(true);
    announcer.handleBatteryTelemetry(8.5, 10.0);

    QVERIFY(spoken.isEmpty());
}

void SpeechAnnouncerTest::batteryCadenceUsesSuccessfulSpeech()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("speech.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    settings.setEnabled(true);
    settings.setArmedOnly(true);
    settings.setBatteryEnabled(true);
    settings.setBatteryWarningVoltage(10.0);
    settings.setBatteryWarningPercent(20.0);
    settings.setBatteryTemplate(
        QStringLiteral("Battery {batv}/{batp} on {sysid}"));

    QStringList attempted;
    bool speechSucceeds = false;
    qint64 now = 0;
    SpeechAnnouncer::VehicleState vehicle{23, true, true};
    SpeechAnnouncer announcer(
        &settings,
        [&attempted, &speechSucceeds](const QString &message) {
            attempted.append(message);
            return speechSucceeds;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; });

    now = 30000;
    announcer.handleBatteryTelemetry(9.0, 10.0);
    QVERIFY(attempted.isEmpty());

    now = 30001;
    announcer.handleBatteryTelemetry(9.0, 10.0);
    QCOMPARE(attempted, QStringList{
        QStringLiteral("Battery 9.00/10 on 23")});

    speechSucceeds = true;
    now = 30002;
    announcer.handleBatteryTelemetry(9.0, 10.0);
    QCOMPARE(attempted.size(), 2);

    now = 60002;
    announcer.handleBatteryTelemetry(9.0, 10.0);
    QCOMPARE(attempted.size(), 2);

    now = 60003;
    announcer.handleBatteryTelemetry(9.0, 10.0);
    QCOMPARE(attempted.size(), 3);

    vehicle.armed = false;
    now = 90004;
    announcer.handleBatteryTelemetry(9.0, 10.0);
    QCOMPARE(attempted.size(), 3);
}

void SpeechAnnouncerTest::formatsExactTelemetryInSelectedDisplayUnits()
{
    SpeechAnnouncer::VehicleState state;
    state.systemId = 42;
    state.valid = true;
    state.waypointValid = true;
    state.waypointNumber = 0;
    state.altitudeValid = true;
    state.altitudeMeters = 10.0;
    state.airspeedValid = true;
    state.airspeedMps = 5.0;
    state.groundSpeedValid = true;
    state.groundSpeedMps = 10.0;
    state.componentId = 1;
    state.modeValid = true;
    state.mode = QStringLiteral("Auto");
    state.batteryVoltageValid = true;
    state.batteryVoltage = 12.345;
    state.batteryRemainingValid = true;
    state.batteryRemainingPercent = 67.4;

    QCOMPARE(SpeechAnnouncer::formatTelemetryTemplate(
                 QStringLiteral(
                     "WP {wpn} id {sysid}/{compid} {mode} "
                     "battery {batv}/{batp} alt {alt}{altunit} "
                     "air {asp}{speedunit} ground {gsp} {unknown}"),
                 state, QStringLiteral("Feet"), QStringLiteral("knots")),
             QStringLiteral(
                 "WP Home id 42/1 Auto battery 12.35/67 "
                 "alt 33ft air 10kts ground 19 {unknown}"));
}

void SpeechAnnouncerTest::periodicCustomHonoursCadenceReadinessAndGeneration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("custom.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    settings.setEnabled(true);
    settings.setCustomEnabled(true);
    settings.setCustomTemplate(QStringLiteral("UAS {sysid} WP {wpn}"));

    QStringList spoken;
    qint64 now = 0;
    bool ready = true;
    SpeechAnnouncer::VehicleState vehicle;
    vehicle.systemId = 23;
    vehicle.valid = true;
    vehicle.generation = 1;
    vehicle.connectedSinceMs = 0;
    vehicle.lastPacketMs = 0;
    vehicle.waypointValid = true;
    vehicle.waypointNumber = 7;
    SpeechAnnouncer announcer(
        &settings,
        [&spoken](const QString &message) {
            spoken.append(message);
            return true;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; }, nullptr,
        [&ready]() { return ready; });

    announcer.tick(); // Establish generation and restart every countdown.
    now = 30000;
    announcer.tick();
    QVERIFY(spoken.isEmpty());
    ready = false;
    now = 30001;
    announcer.tick();
    QVERIFY(spoken.isEmpty());
    ready = true;
    announcer.tick();
    QCOMPARE(spoken, QStringList{QStringLiteral("UAS 23 WP 7")});

    vehicle.generation = 2;
    now = 40000;
    announcer.tick();
    now = 70000;
    announcer.tick();
    QCOMPARE(spoken.size(), 1);
    now = 70001;
    announcer.tick();
    QCOMPARE(spoken.size(), 2);
}

void SpeechAnnouncerTest::noDataUsesStrictGraceAgeAndRepeatBoundaries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("nodata.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    settings.setEnabled(true);

    QStringList spoken;
    qint64 now = 0;
    SpeechAnnouncer::VehicleState vehicle;
    vehicle.systemId = 5;
    vehicle.armed = true;
    vehicle.valid = true;
    vehicle.generation = 1;
    vehicle.connectedSinceMs = 0;
    vehicle.lastPacketMs = 0;
    SpeechAnnouncer announcer(
        &settings,
        [&spoken](const QString &message) {
            spoken.append(message);
            return true;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; });

    announcer.tick();
    now = 30000;
    announcer.tick();
    QVERIFY(spoken.isEmpty());
    now = 30001;
    announcer.tick();
    QCOMPARE(spoken, QStringList{
        QStringLiteral("WARNING No Data for 30 Seconds")});
    now = 35001;
    announcer.tick();
    QCOMPARE(spoken.size(), 1);
    now = 35002;
    announcer.tick();
    QCOMPARE(spoken.last(),
             QStringLiteral("WARNING No Data for 35 Seconds"));

    vehicle.lastPacketMs = now;
    now += 3001;
    announcer.tick();
    QCOMPARE(spoken.size(), 2);
}

void SpeechAnnouncerTest::noDataBannerDoesNotDependOnSpeechMaster()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("nodata-muted.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);

    QStringList spoken;
    qint64 now = 0;
    SpeechAnnouncer::VehicleState vehicle;
    vehicle.systemId = 5;
    vehicle.armed = true;
    vehicle.valid = true;
    vehicle.generation = 1;
    vehicle.connectedSinceMs = 0;
    vehicle.lastPacketMs = 0;
    SpeechAnnouncer announcer(
        &settings,
        [&spoken](const QString &message) {
            spoken.append(message);
            return true;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; });

    announcer.tick();
    now = 30001;
    announcer.tick();
    QCOMPARE(announcer.highMessage(),
             QStringLiteral("WARNING No Data for 30 Seconds"));
    QCOMPARE(announcer.highMessageSeverity(),
             int(MAV_SEVERITY_EMERGENCY));
    QVERIFY(spoken.isEmpty());
}

void SpeechAnnouncerTest::altitudeWarningRequiresAThresholdCrossing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("alt.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    settings.setEnabled(true);
    settings.setAltWarningTemplate(QStringLiteral("Low {alt}"));
    settings.setAltWarningHeightMeters(10.0);
    settings.setAltWarningEnabled(true);

    QStringList spoken;
    qint64 now = 0;
    SpeechAnnouncer::VehicleState vehicle;
    vehicle.systemId = 8;
    vehicle.armed = true;
    vehicle.valid = true;
    vehicle.generation = 1;
    vehicle.altitudeValid = true;
    vehicle.altitudeMeters = 20.0;
    SpeechAnnouncer announcer(
        &settings,
        [&spoken](const QString &message) {
            spoken.append(message);
            return true;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; });

    announcer.tick();
    now = 1;
    announcer.tick();
    vehicle.altitudeMeters = 5.0;
    now = 10000;
    announcer.tick();
    QVERIFY(spoken.isEmpty());
    now = 10001;
    announcer.tick();
    QCOMPARE(spoken, QStringList{QStringLiteral("Low 5")});

    vehicle.armed = false;
    now = 20002;
    announcer.tick();
    vehicle.armed = true;
    vehicle.altitudeMeters = 5.0;
    now = 30003;
    announcer.tick();
    QCOMPARE(spoken.size(), 1);
    vehicle.altitudeMeters = 11.0;
    now = 30004;
    announcer.tick();
    vehicle.altitudeMeters = 4.0;
    now = 40005;
    announcer.tick();
    QCOMPARE(spoken.size(), 2);
}

void SpeechAnnouncerTest::altitudeStateTracksWhileSpeechCannotRun()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("alt-state.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    settings.setAltWarningTemplate(QStringLiteral("Low {alt}"));
    settings.setAltWarningHeightMeters(10.0);
    settings.setAltWarningEnabled(true);

    QStringList spoken;
    qint64 now = 0;
    bool ready = false;
    SpeechAnnouncer::VehicleState vehicle;
    vehicle.systemId = 8;
    vehicle.armed = true;
    vehicle.valid = true;
    vehicle.generation = 1;
    vehicle.altitudeValid = true;
    vehicle.altitudeMeters = 20.0;
    SpeechAnnouncer announcer(
        &settings,
        [&spoken](const QString &message) {
            spoken.append(message);
            return true;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; }, nullptr,
        [&ready]() { return ready; });

    announcer.tick(); // Establish the target generation.
    now = 1;
    announcer.tick(); // Capture the maximum while speech is disabled/busy.
    vehicle.altitudeMeters = 5.0;
    settings.setEnabled(true);
    ready = true;
    now = 10001;
    announcer.tick();
    QCOMPARE(spoken, QStringList{QStringLiteral("Low 5")});

    settings.setEnabled(false);
    vehicle.armed = false;
    now = 10002;
    announcer.tick();
    vehicle.armed = true;
    now = 10003;
    announcer.tick();
    settings.setEnabled(true);
    now = 20002;
    announcer.tick();
    QCOMPARE(spoken.size(), 1);
}

void SpeechAnnouncerTest::lowSpeedUsesCanonicalSiAndAirspeedPriority()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("speed.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    settings.setEnabled(true);
    settings.setLowAirSpeedTemplate(QStringLiteral("Air {asp}"));
    settings.setLowGroundSpeedTemplate(QStringLiteral("Ground {gsp}"));
    settings.setLowAirSpeedTriggerMps(10.0);
    settings.setLowGroundSpeedTriggerMps(5.0);
    settings.setLowSpeedEnabled(true);

    QStringList spoken;
    qint64 now = 0;
    SpeechAnnouncer::VehicleState vehicle;
    vehicle.systemId = 9;
    vehicle.armed = true;
    vehicle.valid = true;
    vehicle.generation = 1;
    vehicle.airspeedValid = true;
    vehicle.airspeedMps = 8.0;
    vehicle.groundSpeedValid = true;
    vehicle.groundSpeedMps = 2.0;
    SpeechAnnouncer announcer(
        &settings,
        [&spoken](const QString &message) {
            spoken.append(message);
            return true;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; }, nullptr, {}, []() {
            SpeechAnnouncer::DisplayUnitState units;
            units.speedUnits = QStringLiteral("mph");
            return units;
        });

    announcer.tick();
    now = 10001;
    announcer.tick();
    QCOMPARE(spoken, QStringList{QStringLiteral("Air 18")});

    vehicle.airspeedMps = 12.0;
    now = 20002;
    announcer.tick();
    QCOMPARE(spoken.last(), QStringLiteral("Ground 4"));
}

void SpeechAnnouncerTest::highStatusUsesThresholdExactLeaseAndTenSecondLifetime()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("high.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    StatusMessageSettings statusSettings(&store);
    settings.setEnabled(true);

    QStringList spoken;
    qint64 now = 0;
    SpeechAnnouncer::VehicleState vehicle;
    vehicle.systemId = 42;
    vehicle.componentId = 1;
    vehicle.linkId = 7;
    vehicle.generation = 3;
    vehicle.armed = true;
    vehicle.valid = true;
    SpeechAnnouncer announcer(
        &settings,
        [&spoken](const QString &message) {
            spoken.append(message);
            return true;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; }, nullptr, []() { return true; }, {},
        &statusSettings);
    QSignalSpy high(&announcer, &SpeechAnnouncer::highMessageChanged);

    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_NOTICE, QStringLiteral("ordinary notice"), now));
    QVERIFY(announcer.highMessage().isEmpty());
    QVERIFY(spoken.isEmpty());

    ExactStatusText stale = statusEvent(
        vehicle, MAV_SEVERITY_WARNING, QStringLiteral("wrong link"), now);
    stale.lease.endpoint.linkId = 8;
    announcer.enqueueStatusText(stale);
    stale = statusEvent(
        vehicle, MAV_SEVERITY_WARNING, QStringLiteral("wrong generation"), now);
    ++stale.lease.generation;
    announcer.enqueueStatusText(stale);
    QVERIFY(announcer.highMessage().isEmpty());

    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_WARNING, QStringLiteral("Battery low"), now));
    QCOMPARE(announcer.highMessage(), QStringLiteral("Battery low"));
    QCOMPARE(announcer.highMessageSeverity(), int(MAV_SEVERITY_WARNING));
    QVERIFY(spoken.isEmpty());
    QCOMPARE(high.count(), 1);
    announcer.tick();
    QCOMPARE(spoken, QStringList{QStringLiteral("Battery low")});

    now = 5000;
    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_WARNING, QStringLiteral("Battery low"), now));
    QCOMPARE(spoken.size(), 1);
    QCOMPARE(high.count(), 1);
    now = 15000;
    announcer.tick();
    QCOMPARE(announcer.highMessage(), QStringLiteral("Battery low"));
    now = 15001;
    announcer.tick();
    QVERIFY(announcer.highMessage().isEmpty());
    QCOMPARE(high.count(), 2);

    ExactStatusText expired = statusEvent(
        vehicle, MAV_SEVERITY_ERROR, QStringLiteral("too old"), 0);
    announcer.enqueueStatusText(expired);
    QVERIFY(announcer.highMessage().isEmpty());
}

void SpeechAnnouncerTest::highStatusRetriesAndRoutesSpecialSpeechOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("high-retry.ini")),
                    QSettings::IniFormat);
    SpeechSettings settings(&store);
    StatusMessageSettings statusSettings(&store);
    settings.setEnabled(true);
    settings.setArmedOnly(true);

    QStringList attempted;
    bool succeeds = false;
    qint64 now = 10;
    SpeechAnnouncer::VehicleState vehicle;
    vehicle.systemId = 42;
    vehicle.componentId = 1;
    vehicle.linkId = 7;
    vehicle.generation = 1;
    vehicle.valid = true;
    vehicle.armed = false;
    SpeechAnnouncer announcer(
        &settings,
        [&attempted, &succeeds](const QString &message) {
            attempted.append(message);
            return succeeds;
        },
        [&vehicle]() { return vehicle; },
        [&now]() { return now; }, nullptr, []() { return true; }, {},
        &statusSettings);

    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_ERROR, QStringLiteral("Motor error"), now));
    QVERIFY(attempted.isEmpty());
    vehicle.armed = true;
    announcer.tick();
    QCOMPARE(attempted, QStringList{QStringLiteral("Motor error")});
    succeeds = true;
    announcer.tick();
    QCOMPARE(attempted.size(), 2);
    announcer.tick();
    QCOMPARE(attempted.size(), 2);

    ++now;
    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_DEBUG, QStringLiteral("PreArm: compass"), now));
    announcer.tick();
    QCOMPARE(attempted.last(), QStringLiteral("Pre-arm check: compass"));
    const int afterPreArm = attempted.size();
    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_DEBUG, QStringLiteral("PreArm: compass"), now));
    announcer.tick();
    QCOMPARE(attempted.size(), afterPreArm);

    ++now;
    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_DEBUG, QStringLiteral("Arm: denied"), now));
    announcer.tick();
    QCOMPARE(attempted.last(), QStringLiteral("Arm check: denied"));

    ++now;
    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_DEBUG, QStringLiteral("#audio: alert"), now));
    QCOMPARE(announcer.highMessage(), QStringLiteral("Audio message: alert"));
    announcer.tick();
    QCOMPARE(attempted.last(), QStringLiteral("alert"));

    ++now;
    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_ERROR, QStringLiteral("PX4v2 board"), now));
    const int beforePx4Tick = attempted.size();
    announcer.tick();
    QCOMPARE(attempted.size(), beforePx4Tick);
    QCOMPARE(announcer.highMessage(), QStringLiteral("PX4v2 board"));

    ++now;
    announcer.enqueueStatusText(statusEvent(
        vehicle, MAV_SEVERITY_DEBUG, QStringLiteral("Tuning: pitch"), now));
    const int beforeTuningTick = attempted.size();
    announcer.tick();
    QCOMPARE(attempted.size(), beforeTuningTick + 1);
    QCOMPARE(attempted.last(), QStringLiteral("Tuning: pitch"));

    vehicle.generation = 2;
    now += 1;
    announcer.tick();
    QVERIFY(announcer.highMessage().isEmpty());
}

void SpeechAnnouncerTest::customWarningsRepeatOnceAndRespectSpeechGates()
{
    QTemporaryDir dir;
    QSettings store(dir.filePath("speech.ini"), QSettings::IniFormat);
    SpeechSettings settings(&store);
    settings.setEnabled(true);
    settings.setArmedOnly(true);
    qint64 now = 0;
    SpeechAnnouncer::VehicleState vehicle;
    vehicle.valid = true; vehicle.armed = true;
    vehicle.linkId = 7; vehicle.systemId = 42; vehicle.componentId = 1;
    vehicle.generation = 1;
    QStringList spoken;
    SpeechAnnouncer announcer(&settings, [&spoken](const QString &text) {
        spoken << text; return true;
    }, [&vehicle]() { return vehicle; }, [&now]() { return now; });
    const auto lease = statusEvent(vehicle, MAV_SEVERITY_WARNING, "", now).lease;
    announcer.enqueueCustomWarning(lease, "Battery low");
    QCOMPARE(spoken.size(), 1);
    QCOMPARE(announcer.highMessage(), QString("Battery low"));
    announcer.tick();
    QCOMPARE(spoken.size(), 1); // No second high-message speech path.
    now = 1000;
    announcer.enqueueCustomWarning(lease, "Battery low");
    QCOMPARE(spoken.size(), 2); // Identical due warning is not swallowed.
    settings.setEnabled(false);
    announcer.enqueueCustomWarning(lease, "Visible without audio");
    QCOMPARE(announcer.highMessage(), QString("Visible without audio"));
    QCOMPARE(spoken.size(), 2);
    settings.setEnabled(true);
    vehicle.armed = false;
    announcer.enqueueCustomWarning(lease, "Disarmed warning");
    QCOMPARE(announcer.highMessage(), QString("Disarmed warning"));
    QCOMPARE(spoken.size(), 2);
    vehicle.armed = true;
    auto wrong = lease; wrong.endpoint.linkId = 99;
    announcer.enqueueCustomWarning(wrong, "Wrong link");
    QCOMPARE(announcer.highMessage(), QString("Disarmed warning"));
    QCOMPARE(spoken.size(), 2);
    vehicle.generation = 2;
    announcer.enqueueCustomWarning(lease, "Stale generation");
    QCOMPARE(spoken.size(), 2);
    announcer.tick();
    QVERIFY(announcer.highMessage().isEmpty());
}

QTEST_APPLESS_MAIN(SpeechAnnouncerTest)
#include "test_speechannouncer.moc"
