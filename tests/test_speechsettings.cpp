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
    void nonFiniteThresholdsAreRejected();
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
    QVERIFY(!speech.batteryEnabled());
    QVERIFY(!speech.armDisarmEnabled());
    QCOMPARE(speech.waypointTemplate(),
             QStringLiteral("Heading to Waypoint {wpn}"));
    QCOMPARE(speech.modeTemplate(),
             QStringLiteral("Mode changed to {mode}"));
    QCOMPARE(speech.batteryTemplate(), QStringLiteral(
        "WARNING, Battery at {batv} Volt, {batp} percent"));
    QCOMPARE(speech.batteryWarningVoltage(), 9.6);
    QCOMPARE(speech.batteryWarningPercent(), 20.0);
    QCOMPARE(speech.armTemplate(), QStringLiteral("Armed"));
    QCOMPARE(speech.disarmTemplate(), QStringLiteral("Disarmed"));
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
    settings.setValue(QStringLiteral("speechbatteryenabled"), true);
    settings.setValue(QStringLiteral("speechbattery"),
                      QStringLiteral("Battery custom"));
    settings.setValue(QStringLiteral("speechbatteryvolt"), 11.4);
    settings.setValue(QStringLiteral("speechbatterypercent"), 32.5);
    settings.setValue(QStringLiteral("speecharmenabled"), true);
    settings.setValue(QStringLiteral("speecharm"),
                      QStringLiteral("Vehicle armed"));
    settings.setValue(QStringLiteral("speechdisarm"),
                      QStringLiteral("Vehicle disarmed"));

    SpeechSettings speech(&settings);

    QCOMPARE(SpeechSettings::settingsKey(), QStringLiteral("speechenable"));
    QVERIFY(speech.isEnabled());
    QVERIFY(speech.armedOnly());
    QVERIFY(speech.waypointEnabled());
    QVERIFY(speech.modeEnabled());
    QVERIFY(speech.batteryEnabled());
    QVERIFY(speech.armDisarmEnabled());
    QCOMPARE(speech.waypointTemplate(),
             QStringLiteral("Waypoint {wpn} for {sysid}"));
    QCOMPARE(speech.modeTemplate(), QStringLiteral("Now {mode}"));
    QCOMPARE(speech.batteryTemplate(), QStringLiteral("Battery custom"));
    QCOMPARE(speech.batteryWarningVoltage(), 11.4);
    QCOMPARE(speech.batteryWarningPercent(), 32.5);
    QCOMPARE(speech.armTemplate(), QStringLiteral("Vehicle armed"));
    QCOMPARE(speech.disarmTemplate(), QStringLiteral("Vehicle disarmed"));
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
    speech.setBatteryTemplate(QStringLiteral("Low battery"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setBatteryWarningVoltage(10.7);
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setBatteryWarningPercent(27.5);
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setArmTemplate(QStringLiteral("Armed {sysid}"));
    QCOMPARE(policyChanged.count(), 1);

    policyChanged.clear();
    speech.setDisarmTemplate(QStringLiteral("Disarmed {sysid}"));
    QCOMPARE(policyChanged.count(), 1);

    QSettings persisted(path, QSettings::IniFormat);
    QVERIFY(persisted.value(QStringLiteral("speechenable")).toBool());
    QVERIFY(persisted.value(QStringLiteral("speech_armed_only")).toBool());
    QCOMPARE(persisted.value(QStringLiteral("speechwaypoint")).toString(),
             QStringLiteral("Next {wpn}"));
    QCOMPARE(persisted.value(QStringLiteral("speechmode")).toString(),
             QStringLiteral("Flight mode {mode}"));
    QCOMPARE(persisted.value(QStringLiteral("speechbattery")).toString(),
             QStringLiteral("Low battery"));
    QCOMPARE(persisted.value(QStringLiteral("speechbatteryvolt")).toDouble(),
             10.7);
    QCOMPARE(persisted.value(QStringLiteral("speechbatterypercent")).toDouble(),
             27.5);
    QCOMPARE(persisted.value(QStringLiteral("speecharm")).toString(),
             QStringLiteral("Armed {sysid}"));
    QCOMPARE(persisted.value(QStringLiteral("speechdisarm")).toString(),
             QStringLiteral("Disarmed {sysid}"));
}

void SpeechSettingsTest::nonFiniteThresholdsAreRejected()
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

    QCOMPARE(policyChanged.count(), 0);
    QCOMPARE(speech.batteryWarningVoltage(), 9.6);
    QCOMPARE(speech.batteryWarningPercent(), 20.0);
    QVERIFY(!settings.contains(QStringLiteral("speechbatteryvolt")));
    QVERIFY(!settings.contains(QStringLiteral("speechbatterypercent")));
}

void SpeechSettingsTest::enablingEventsSeedsOnlyMissingDefaults()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("seed.ini"));
    settings.setValue(QStringLiteral("speechmode"),
                      QStringLiteral("Existing {mode}"));
    settings.setValue(QStringLiteral("speechbattery"),
                      QStringLiteral("Existing battery"));
    settings.setValue(QStringLiteral("speechbatterypercent"), 14);
    settings.setValue(QStringLiteral("speechdisarm"),
                      QStringLiteral("Existing disarm"));
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
    speech.setArmDisarmEnabled(true);
    QCOMPARE(policyChanged.count(), 1);
    QVERIFY(settings.value(QStringLiteral("speecharmenabled")).toBool());
    QCOMPARE(settings.value(QStringLiteral("speecharm")).toString(),
             QStringLiteral("Armed"));
    QCOMPARE(settings.value(QStringLiteral("speechdisarm")).toString(),
             QStringLiteral("Existing disarm"));

    QVERIFY(speech.waypointEnabled());
    QVERIFY(speech.modeEnabled());
    QVERIFY(speech.batteryEnabled());
    QVERIFY(speech.armDisarmEnabled());
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
        external.setValue(QStringLiteral("speechbatteryvolt"), 12.1);
        external.sync();
    }
    speech.reload();
    QVERIFY(speech.isEnabled());
    QVERIFY(speech.modeEnabled());
    QCOMPARE(speech.modeTemplate(), QStringLiteral("External {mode}"));
    QCOMPARE(speech.batteryWarningVoltage(), 12.1);
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
    QCOMPARE(speech.modeTemplate(),
             QStringLiteral("Mode changed to {mode}"));
    QCOMPARE(speech.batteryWarningVoltage(), 9.6);
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
    QCOMPARE(speech.armStateAnnouncement(true, 23),
             QStringLiteral("Armed 23 {mode}"));
    QCOMPARE(speech.armStateAnnouncement(false, 23),
             QStringLiteral("Disarmed 23 {mode}"));
}

QTEST_GUILESS_MAIN(SpeechSettingsTest)

#include "test_speechsettings.moc"
