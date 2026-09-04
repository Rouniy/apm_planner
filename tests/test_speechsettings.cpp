#include <QtTest>

#include "services/SpeechSettings.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <limits>

namespace {
QSettings settingsFor(const QTemporaryDir &directory, const QString &name)
{
    return QSettings(directory.filePath(name), QSettings::IniFormat);
}
}

class SpeechSettingsTest final : public QObject
{
    Q_OBJECT

private slots:
    void missingSettingsUseMp10DefaultsWithoutWriting();
    void initialValuesUseCanonicalKeys();
    void settersPersistAndSignal();
    void invalidThresholdsAreRejected();
    void enablingEventsSeedsOnlyMissingDefaults();
    void reloadObservesExternalChangesWithoutWriting();
    void announcementGatesMatchSpeechPolicy();
    void announcementFormattingReplacesKnownTokensOnly();
};

void SpeechSettingsTest::missingSettingsUseMp10DefaultsWithoutWriting()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("missing.ini"));

    SpeechSettings speech(&settings);

    QVERIFY(!speech.isEnabled());
    QVERIFY(!speech.armedOnly());
    QVERIFY(!speech.waypointEnabled());
    QVERIFY(!speech.modeEnabled());
    QVERIFY(!speech.customEnabled());
    QVERIFY(!speech.batteryEnabled());
    QVERIFY(!speech.altWarningEnabled());
    QVERIFY(!speech.armDisarmEnabled());
    QVERIFY(!speech.lowSpeedEnabled());
    QCOMPARE(speech.waypointTemplate(),
             QStringLiteral("Heading to Waypoint {wpn}"));
    QCOMPARE(speech.modeTemplate(),
             QStringLiteral("Mode changed to {mode}"));
    QCOMPARE(speech.customTemplate(), QStringLiteral(
        "Heading to Waypoint {wpn}, altitude is {alt}, Ground speed is {gsp} "));
    QCOMPARE(speech.batteryTemplate(), QStringLiteral(
        "WARNING, Battery at {batv} Volt, {batp} percent"));
    QCOMPARE(speech.batteryWarningVoltage(), 9.6);
    QCOMPARE(speech.batteryWarningPercent(), 20.0);
    QCOMPARE(speech.altWarningTemplate(),
             QStringLiteral("WARNING, low altitude {alt}"));
    QCOMPARE(speech.altWarningHeightMeters(), 2.0);
    QVERIFY(!speech.altWarningHeightConfigured());
    QCOMPARE(speech.armTemplate(), QStringLiteral("Armed"));
    QCOMPARE(speech.disarmTemplate(), QStringLiteral("Disarmed"));
    QCOMPARE(speech.lowGroundSpeedTemplate(),
             QStringLiteral("Low Ground Speed {gsp}"));
    QCOMPARE(speech.lowGroundSpeedTriggerMps(), 0.0);
    QCOMPARE(speech.lowAirSpeedTemplate(),
             QStringLiteral("Low Air Speed {asp}"));
    QCOMPARE(speech.lowAirSpeedTriggerMps(), 0.0);
    QVERIFY(settings.allKeys().isEmpty());
}

void SpeechSettingsTest::initialValuesUseCanonicalKeys()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("initial.ini"));
    settings.setValue(QStringLiteral("speechenable"), true);
    settings.setValue(QStringLiteral("speech_armed_only"), true);
    settings.setValue(QStringLiteral("speechwaypointenabled"), true);
    settings.setValue(QStringLiteral("speechwaypoint"),
                      QStringLiteral("Waypoint {wpn} for {sysid}"));
    settings.setValue(QStringLiteral("speechmodeenabled"), true);
    settings.setValue(QStringLiteral("speechmode"),
                      QStringLiteral("Now {mode}"));
    settings.setValue(QStringLiteral("speechcustomenabled"), true);
    settings.setValue(QStringLiteral("speechcustom"),
                      QStringLiteral("Custom {alt} {gsp}"));
    settings.setValue(QStringLiteral("speechbatteryenabled"), true);
    settings.setValue(QStringLiteral("speechbattery"),
                      QStringLiteral("Battery custom"));
    settings.setValue(QStringLiteral("speechbatteryvolt"), 11.4);
    settings.setValue(QStringLiteral("speechbatterypercent"), 32.5);
    settings.setValue(QStringLiteral("speechaltenabled"), true);
    settings.setValue(QStringLiteral("speechalt"),
                      QStringLiteral("Altitude {alt}"));
    settings.setValue(QStringLiteral("speechaltheight"), 18.25);
    settings.setValue(QStringLiteral("speecharmenabled"), true);
    settings.setValue(QStringLiteral("speecharm"),
                      QStringLiteral("Vehicle armed"));
    settings.setValue(QStringLiteral("speechdisarm"),
                      QStringLiteral("Vehicle disarmed"));
    settings.setValue(QStringLiteral("speechlowspeedenabled"), true);
    settings.setValue(QStringLiteral("speechlowgroundspeed"),
                      QStringLiteral("Ground {gsp}"));
    settings.setValue(QStringLiteral("speechlowgroundspeedtrigger"), 4.5);
    settings.setValue(QStringLiteral("speechlowairspeed"),
                      QStringLiteral("Air {asp}"));
    settings.setValue(QStringLiteral("speechlowairspeedtrigger"), 6.75);

    SpeechSettings speech(&settings);

    QCOMPARE(SpeechSettings::settingsKey(), QStringLiteral("speechenable"));
    QVERIFY(speech.isEnabled());
    QVERIFY(speech.armedOnly());
    QVERIFY(speech.waypointEnabled());
    QVERIFY(speech.modeEnabled());
    QVERIFY(speech.customEnabled());
    QVERIFY(speech.batteryEnabled());
    QVERIFY(speech.altWarningEnabled());
    QVERIFY(speech.armDisarmEnabled());
    QVERIFY(speech.lowSpeedEnabled());
    QCOMPARE(speech.waypointTemplate(),
             QStringLiteral("Waypoint {wpn} for {sysid}"));
    QCOMPARE(speech.modeTemplate(), QStringLiteral("Now {mode}"));
    QCOMPARE(speech.customTemplate(), QStringLiteral("Custom {alt} {gsp}"));
    QCOMPARE(speech.batteryTemplate(), QStringLiteral("Battery custom"));
    QCOMPARE(speech.batteryWarningVoltage(), 11.4);
    QCOMPARE(speech.batteryWarningPercent(), 32.5);
    QCOMPARE(speech.altWarningTemplate(), QStringLiteral("Altitude {alt}"));
    QCOMPARE(speech.altWarningHeightMeters(), 18.25);
    QVERIFY(speech.altWarningHeightConfigured());
    QCOMPARE(speech.armTemplate(), QStringLiteral("Vehicle armed"));
    QCOMPARE(speech.disarmTemplate(), QStringLiteral("Vehicle disarmed"));
    QCOMPARE(speech.lowGroundSpeedTemplate(), QStringLiteral("Ground {gsp}"));
    QCOMPARE(speech.lowGroundSpeedTriggerMps(), 4.5);
    QCOMPARE(speech.lowAirSpeedTemplate(), QStringLiteral("Air {asp}"));
    QCOMPARE(speech.lowAirSpeedTriggerMps(), 6.75);
}

void SpeechSettingsTest::settersPersistAndSignal()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("persist.ini"));
    QSettings settings(path, QSettings::IniFormat);
    SpeechSettings speech(&settings);
    QSignalSpy enabledChanged(&speech, &SpeechSettings::enabledChanged);
    QSignalSpy policyChanged(&speech, &SpeechSettings::policyChanged);

    speech.setEnabled(false);
    QCOMPARE(enabledChanged.count(), 0);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(settings.contains(QStringLiteral("speechenable")));
    QVERIFY(!settings.value(QStringLiteral("speechenable")).toBool());

    policyChanged.clear();
    speech.setEnabled(true);
    QCOMPARE(enabledChanged.count(), 1);
    QCOMPARE(enabledChanged.takeFirst().at(0).toBool(), true);
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setEnabled(true);
    QCOMPARE(enabledChanged.count(), 0);
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setArmedOnly(true);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(speech.armedOnly());

    policyChanged.clear();
    speech.setWaypointTemplate(QStringLiteral("Next {wpn}"));
    QCOMPARE(policyChanged.count(), 1);
    QCOMPARE(speech.waypointTemplate(), QStringLiteral("Next {wpn}"));

    policyChanged.clear();
    speech.setModeTemplate(QStringLiteral("Flight mode {mode}"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setCustomTemplate(QStringLiteral("Status {alt} {gsp}"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setBatteryTemplate(QStringLiteral("Low battery"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setBatteryWarningVoltage(10.7);
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setBatteryWarningPercent(27.5);
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setAltWarningTemplate(QStringLiteral("Low altitude {alt}"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setAltWarningHeightMeters(12.5);
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setArmTemplate(QStringLiteral("Armed {sysid}"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setDisarmTemplate(QStringLiteral("Disarmed {sysid}"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setLowGroundSpeedTemplate(QStringLiteral("Ground {gsp}"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setLowGroundSpeedTriggerMps(3.25);
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setLowAirSpeedTemplate(QStringLiteral("Air {asp}"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setLowAirSpeedTriggerMps(5.5);
    QCOMPARE(policyChanged.count(), 1);

    QSettings persisted(path, QSettings::IniFormat);
    QVERIFY(persisted.value(QStringLiteral("speechenable")).toBool());
    QVERIFY(persisted.value(QStringLiteral("speech_armed_only")).toBool());
    QCOMPARE(persisted.value(QStringLiteral("speechwaypoint")).toString(),
             QStringLiteral("Next {wpn}"));
    QCOMPARE(persisted.value(QStringLiteral("speechmode")).toString(),
             QStringLiteral("Flight mode {mode}"));
    QCOMPARE(persisted.value(QStringLiteral("speechcustom")).toString(),
             QStringLiteral("Status {alt} {gsp}"));
    QCOMPARE(persisted.value(QStringLiteral("speechbattery")).toString(),
             QStringLiteral("Low battery"));
    QCOMPARE(persisted.value(QStringLiteral("speechbatteryvolt")).toDouble(),
             10.7);
    QCOMPARE(persisted.value(QStringLiteral("speechbatterypercent")).toDouble(),
             27.5);
    QCOMPARE(persisted.value(QStringLiteral("speechalt")).toString(),
             QStringLiteral("Low altitude {alt}"));
    QCOMPARE(persisted.value(QStringLiteral("speechaltheight")).toDouble(),
             12.5);
    QCOMPARE(persisted.value(QStringLiteral("speecharm")).toString(),
             QStringLiteral("Armed {sysid}"));
    QCOMPARE(persisted.value(QStringLiteral("speechdisarm")).toString(),
             QStringLiteral("Disarmed {sysid}"));
    QCOMPARE(persisted.value(QStringLiteral("speechlowgroundspeed")).toString(),
             QStringLiteral("Ground {gsp}"));
    QCOMPARE(persisted.value(
                 QStringLiteral("speechlowgroundspeedtrigger")).toDouble(),
             3.25);
    QCOMPARE(persisted.value(QStringLiteral("speechlowairspeed")).toString(),
             QStringLiteral("Air {asp}"));
    QCOMPARE(persisted.value(
                 QStringLiteral("speechlowairspeedtrigger")).toDouble(),
             5.5);
}

void SpeechSettingsTest::invalidThresholdsAreRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("finite.ini"));
    SpeechSettings speech(&settings);
    QSignalSpy policyChanged(&speech, &SpeechSettings::policyChanged);

    speech.setBatteryWarningVoltage(
        std::numeric_limits<double>::quiet_NaN());
    speech.setBatteryWarningPercent(
        std::numeric_limits<double>::infinity());
    speech.setAltWarningHeightMeters(-1.0);
    speech.setLowGroundSpeedTriggerMps(
        std::numeric_limits<double>::quiet_NaN());
    speech.setLowAirSpeedTriggerMps(-0.1);

    QCOMPARE(policyChanged.count(), 0);
    QCOMPARE(speech.batteryWarningVoltage(), 9.6);
    QCOMPARE(speech.batteryWarningPercent(), 20.0);
    QVERIFY(!settings.contains(QStringLiteral("speechbatteryvolt")));
    QVERIFY(!settings.contains(QStringLiteral("speechbatterypercent")));
    QVERIFY(!settings.contains(QStringLiteral("speechaltheight")));
    QVERIFY(!settings.contains(
        QStringLiteral("speechlowgroundspeedtrigger")));
    QVERIFY(!settings.contains(QStringLiteral("speechlowairspeedtrigger")));
}

void SpeechSettingsTest::enablingEventsSeedsOnlyMissingDefaults()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("seed.ini"));
    settings.setValue(QStringLiteral("speechmode"),
                      QStringLiteral("Existing {mode}"));
    settings.setValue(QStringLiteral("speechcustom"),
                      QStringLiteral("Existing custom"));
    settings.setValue(QStringLiteral("speechbattery"),
                      QStringLiteral("Existing battery"));
    settings.setValue(QStringLiteral("speechbatterypercent"), 14);
    settings.setValue(QStringLiteral("speechaltheight"), 7.5);
    settings.setValue(QStringLiteral("speechdisarm"),
                      QStringLiteral("Existing disarm"));
    settings.setValue(QStringLiteral("speechlowgroundspeed"),
                      QStringLiteral("Existing ground"));
    settings.setValue(QStringLiteral("speechlowairspeedtrigger"), 8.0);
    SpeechSettings speech(&settings);
    QSignalSpy policyChanged(&speech, &SpeechSettings::policyChanged);

    speech.setWaypointEnabled(true);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(settings.value(QStringLiteral("speechwaypointenabled")).toBool());
    QCOMPARE(settings.value(QStringLiteral("speechwaypoint")).toString(),
             QStringLiteral("Heading to Waypoint {wpn}"));

    policyChanged.clear();
    speech.setModeEnabled(true);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(settings.value(QStringLiteral("speechmodeenabled")).toBool());
    QCOMPARE(settings.value(QStringLiteral("speechmode")).toString(),
             QStringLiteral("Existing {mode}"));

    policyChanged.clear();
    speech.setCustomEnabled(true);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(settings.value(QStringLiteral("speechcustomenabled")).toBool());
    QCOMPARE(settings.value(QStringLiteral("speechcustom")).toString(),
             QStringLiteral("Existing custom"));

    policyChanged.clear();
    speech.setBatteryEnabled(true);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(settings.value(QStringLiteral("speechbatteryenabled")).toBool());
    QCOMPARE(settings.value(QStringLiteral("speechbattery")).toString(),
             QStringLiteral("Existing battery"));
    QCOMPARE(settings.value(QStringLiteral("speechbatteryvolt")).toDouble(),
             9.6);
    QCOMPARE(settings.value(QStringLiteral("speechbatterypercent")).toInt(),
             14);

    policyChanged.clear();
    speech.setAltWarningEnabled(true);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(settings.value(QStringLiteral("speechaltenabled")).toBool());
    QCOMPARE(settings.value(QStringLiteral("speechalt")).toString(),
             QStringLiteral("WARNING, low altitude {alt}"));
    QCOMPARE(settings.value(QStringLiteral("speechaltheight")).toDouble(),
             7.5);

    policyChanged.clear();
    speech.setArmDisarmEnabled(true);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(settings.value(QStringLiteral("speecharmenabled")).toBool());
    QCOMPARE(settings.value(QStringLiteral("speecharm")).toString(),
             QStringLiteral("Armed"));
    QCOMPARE(settings.value(QStringLiteral("speechdisarm")).toString(),
             QStringLiteral("Existing disarm"));

    policyChanged.clear();
    speech.setLowSpeedEnabled(true);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(settings.value(QStringLiteral("speechlowspeedenabled")).toBool());
    QCOMPARE(settings.value(QStringLiteral("speechlowgroundspeed")).toString(),
             QStringLiteral("Existing ground"));
    QCOMPARE(settings.value(
                 QStringLiteral("speechlowgroundspeedtrigger")).toDouble(),
             0.0);
    QCOMPARE(settings.value(QStringLiteral("speechlowairspeed")).toString(),
             QStringLiteral("Low Air Speed {asp}"));
    QCOMPARE(settings.value(
                 QStringLiteral("speechlowairspeedtrigger")).toDouble(),
             8.0);

    QVERIFY(speech.waypointEnabled());
    QVERIFY(speech.modeEnabled());
    QVERIFY(speech.customEnabled());
    QVERIFY(speech.batteryEnabled());
    QVERIFY(speech.altWarningEnabled());
    QVERIFY(speech.armDisarmEnabled());
    QVERIFY(speech.lowSpeedEnabled());
}

void SpeechSettingsTest::reloadObservesExternalChangesWithoutWriting()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("reload.ini"));
    QSettings settings(path, QSettings::IniFormat);
    SpeechSettings speech(&settings);
    QSignalSpy enabledChanged(&speech, &SpeechSettings::enabledChanged);
    QSignalSpy policyChanged(&speech, &SpeechSettings::policyChanged);

    {
        QSettings external(path, QSettings::IniFormat);
        external.setValue(QStringLiteral("speechenable"), true);
        external.setValue(QStringLiteral("speechmodeenabled"), true);
        external.setValue(QStringLiteral("speechmode"),
                          QStringLiteral("External {mode}"));
        external.setValue(QStringLiteral("speechcustomenabled"), true);
        external.setValue(QStringLiteral("speechcustom"),
                          QStringLiteral("External custom"));
        external.setValue(QStringLiteral("speechbatteryvolt"), 12.1);
        external.setValue(QStringLiteral("speechaltenabled"), true);
        external.setValue(QStringLiteral("speechaltheight"), 21.0);
        external.setValue(QStringLiteral("speechlowspeedenabled"), true);
        external.setValue(QStringLiteral("speechlowgroundspeedtrigger"), 2.0);
        external.sync();
    }
    speech.reload();
    QVERIFY(speech.isEnabled());
    QVERIFY(speech.modeEnabled());
    QCOMPARE(speech.modeTemplate(), QStringLiteral("External {mode}"));
    QVERIFY(speech.customEnabled());
    QCOMPARE(speech.customTemplate(), QStringLiteral("External custom"));
    QCOMPARE(speech.batteryWarningVoltage(), 12.1);
    QVERIFY(speech.altWarningEnabled());
    QCOMPARE(speech.altWarningHeightMeters(), 21.0);
    QVERIFY(speech.altWarningHeightConfigured());
    QVERIFY(speech.lowSpeedEnabled());
    QCOMPARE(speech.lowGroundSpeedTriggerMps(), 2.0);
    QCOMPARE(enabledChanged.count(), 1);
    QCOMPARE(policyChanged.count(), 1);

    speech.reload();
    QCOMPARE(enabledChanged.count(), 1);
    QCOMPARE(policyChanged.count(), 1);

    {
        QSettings external(path, QSettings::IniFormat);
        external.clear();
        external.sync();
    }
    speech.reload();
    QVERIFY(!speech.isEnabled());
    QVERIFY(!speech.modeEnabled());
    QVERIFY(!speech.customEnabled());
    QVERIFY(!speech.altWarningEnabled());
    QVERIFY(!speech.lowSpeedEnabled());
    QCOMPARE(speech.modeTemplate(),
             QStringLiteral("Mode changed to {mode}"));
    QCOMPARE(speech.batteryWarningVoltage(), 9.6);
    QVERIFY(!speech.altWarningHeightConfigured());
    QCOMPARE(enabledChanged.count(), 2);
    QCOMPARE(policyChanged.count(), 2);

    settings.sync();
    QVERIFY(settings.allKeys().isEmpty());
}

void SpeechSettingsTest::announcementGatesMatchSpeechPolicy()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("gates.ini"));
    settings.setValue(QStringLiteral("speechenable"), true);
    settings.setValue(QStringLiteral("speech_armed_only"), true);
    settings.setValue(QStringLiteral("speechmodeenabled"), true);
    settings.setValue(QStringLiteral("speechwaypointenabled"), true);
    settings.setValue(QStringLiteral("speecharmenabled"), true);
    SpeechSettings speech(&settings);

    QVERIFY(speech.modeAnnouncement(QStringLiteral("AUTO"), 1, false).isEmpty());
    QVERIFY(speech.waypointAnnouncement(4, 1, false).isEmpty());
    QCOMPARE(speech.modeAnnouncement(QStringLiteral("AUTO"), 1, true),
             QStringLiteral("Mode changed to AUTO"));
    QCOMPARE(speech.waypointAnnouncement(4, 1, true),
             QStringLiteral("Heading to Waypoint 4"));

    // Arm/disarm announcements deliberately ignore armed-only mode.
    QCOMPARE(speech.armStateAnnouncement(true, 1), QStringLiteral("Armed"));
    QCOMPARE(speech.armStateAnnouncement(false, 1),
             QStringLiteral("Disarmed"));

    speech.setModeEnabled(false);
    speech.setWaypointEnabled(false);
    speech.setArmDisarmEnabled(false);
    QVERIFY(speech.modeAnnouncement(QStringLiteral("RTL"), 1, true).isEmpty());
    QVERIFY(speech.waypointAnnouncement(5, 1, true).isEmpty());
    QVERIFY(speech.armStateAnnouncement(true, 1).isEmpty());

    speech.setModeEnabled(true);
    speech.setWaypointEnabled(true);
    speech.setArmDisarmEnabled(true);
    speech.setEnabled(false);
    QVERIFY(speech.modeAnnouncement(QStringLiteral("RTL"), 1, true).isEmpty());
    QVERIFY(speech.waypointAnnouncement(5, 1, true).isEmpty());
    QVERIFY(speech.armStateAnnouncement(false, 1).isEmpty());
}

void SpeechSettingsTest::announcementFormattingReplacesKnownTokensOnly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("format.ini"));
    settings.setValue(QStringLiteral("speechenable"), true);
    settings.setValue(QStringLiteral("speechmodeenabled"), true);
    settings.setValue(QStringLiteral("speechwaypointenabled"), true);
    settings.setValue(QStringLiteral("speecharmenabled"), true);
    settings.setValue(QStringLiteral("speechmode"),
                      QStringLiteral("Sys {sysid}: {mode} {unknown}"));
    settings.setValue(QStringLiteral("speechwaypoint"),
                      QStringLiteral("WP {wpn}, sys {sysid}, {alt}"));
    settings.setValue(QStringLiteral("speecharm"),
                      QStringLiteral("Armed {sysid} {mode}"));
    settings.setValue(QStringLiteral("speechdisarm"),
                      QStringLiteral("Disarmed {sysid} {mode}"));
    SpeechSettings speech(&settings);

    QCOMPARE(speech.modeAnnouncement(QStringLiteral("LOITER"), 23, false),
             QStringLiteral("Sys 23: LOITER {unknown}"));
    QCOMPARE(speech.waypointAnnouncement(17, 23, false),
             QStringLiteral("WP 17, sys 23, {alt}"));
    QCOMPARE(speech.waypointAnnouncement(0, 23, false),
             QStringLiteral("WP Home, sys 23, {alt}"));
    QCOMPARE(speech.armStateAnnouncement(true, 23),
             QStringLiteral("Armed 23 {mode}"));
    QCOMPARE(speech.armStateAnnouncement(false, 23),
             QStringLiteral("Disarmed 23 {mode}"));
    QCOMPARE(SpeechSettings::formatTemplate(
                 QStringLiteral("{wpn} {alt} {gsp} {asp} {unknown}"),
                 {{QStringLiteral("{wpn}"), QStringLiteral("8")},
                  {QStringLiteral("{alt}"), QStringLiteral("120")},
                  {QStringLiteral("{gsp}"), QStringLiteral("12")},
                  {QStringLiteral("{asp}"), QStringLiteral("10")}}),
             QStringLiteral("8 120 12 10 {unknown}"));
}

QTEST_GUILESS_MAIN(SpeechSettingsTest)

#include "test_speechsettings.moc"
