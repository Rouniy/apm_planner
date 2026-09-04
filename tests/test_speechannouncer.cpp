#include <QtTest>

#include "GAudioOutput.h"
#include "services/SpeechAnnouncer.h"
#include "services/SpeechSettings.h"
#include "ui/flightdata/FlightDataViewModel.h"

#include <QSettings>
#include <QTemporaryDir>

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

QTEST_APPLESS_MAIN(SpeechAnnouncerTest)
#include "test_speechannouncer.moc"
