#include <QtTest>

#include "ui/configuration/DisplayViewProfile.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace {
QJsonObject parsed(const DisplayViewProfile &profile)
{
    return QJsonDocument::fromJson(profile.toJson()).object();
}

void writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable(file.errorString()));
    QCOMPARE(file.write(contents), qint64(contents.size()));
    file.close();
}
}

class DisplayViewProfileTest final : public QObject
{
    Q_OBJECT

private slots:
    void presetsMatchMissionPlannerRouteDefaults();
    void configAndSetupOsdFlagsRemainIndependent();
    void jsonIsDeterministicAndPreservesUnknownCustomFields();
    void missingSettingsStartAdvancedWithoutPersistingImplicitly();
    void servicePersistsProfilesAndEmitsOnlyForChanges();
    void storedProfilesReceiveStartupParameterVisibilityOverrides();
    void invalidStoredProfileFallsBackToConstructorTemplate();
    void legacyMainWindowAdvancedModeIsMigratedOnce();
    void customPresetLoadsFileAndPreservesExtensions();
    void invalidCustomProfileDoesNotReplaceCurrentProfile();
    void legacySettingsAreNotMigrated();
};

void DisplayViewProfileTest::presetsMatchMissionPlannerRouteDefaults()
{
    const DisplayViewProfile basic = DisplayViewProfile::basic();
    const DisplayViewProfile advanced = DisplayViewProfile::advanced();
    QCOMPARE(static_cast<int>(basic.preset()),
             static_cast<int>(DisplayViewPreset::Basic));
    QCOMPARE(basic.presetName(), QStringLiteral("Basic"));
    QVERIFY(!basic.isAdvancedMode());
    QVERIFY(basic.displayPlannerLayout());
    QVERIFY(!basic.configFlags().displayStandardParams);
    QVERIFY(!basic.configFlags().displayAdvancedParams);
    QVERIFY(basic.configFlags().displayFullParamList);
    QVERIFY(!basic.setupFlags().displayTerminal);
    QVERIFY(basic.setupFlags().displayREPL);

    QCOMPARE(static_cast<int>(advanced.preset()),
             static_cast<int>(DisplayViewPreset::Advanced));
    QCOMPARE(advanced.presetName(), QStringLiteral("Advanced"));
    QVERIFY(advanced.isAdvancedMode());
    QVERIFY(!advanced.configFlags().displayStandardParams);
    QVERIFY(!advanced.configFlags().displayAdvancedParams);
    QVERIFY(advanced.configFlags().displayFullParamList);
    QVERIFY(advanced.setupFlags().displayTerminal);

    QJsonObject basicJson = parsed(basic);
    QJsonObject advancedJson = parsed(advanced);
    QCOMPARE(basicJson.size(), 47);
    QCOMPARE(advancedJson.size(), 47);
    QCOMPARE(basicJson.keys(), QStringList({
        QStringLiteral("displayADSB"),
        QStringLiteral("displayAccelCalibration"),
        QStringLiteral("displayAdvancedParams"),
        QStringLiteral("displayAirSpeed"),
        QStringLiteral("displayAntennaTracker"),
        QStringLiteral("displayBasicTuning"),
        QStringLiteral("displayBattMonitor"),
        QStringLiteral("displayBluetooth"),
        QStringLiteral("displayCAN"),
        QStringLiteral("displayCameraGimbal"),
        QStringLiteral("displayCompassConfiguration"),
        QStringLiteral("displayCompassMotorCalib"),
        QStringLiteral("displayEscCalibration"),
        QStringLiteral("displayEsp"),
        QStringLiteral("displayExtendedTuning"),
        QStringLiteral("displayFFTSetup"),
        QStringLiteral("displayFailSafe"),
        QStringLiteral("displayFlightModes"),
        QStringLiteral("displayFrameType"),
        QStringLiteral("displayFullParamList"),
        QStringLiteral("displayGPSOrder"),
        QStringLiteral("displayGeoFence"),
        QStringLiteral("displayHWIDs"),
        QStringLiteral("displayInitialParams"),
        QStringLiteral("displayInstallFirmware"),
        QStringLiteral("displayJoystick"),
        QStringLiteral("displayMavFTP"),
        QStringLiteral("displayMotorTest"),
        QStringLiteral("displayName"),
        QStringLiteral("displayOSD"),
        QStringLiteral("displayOpticalFlow"),
        QStringLiteral("displayOsd"),
        QStringLiteral("displayParachute"),
        QStringLiteral("displayPlannerLayout"),
        QStringLiteral("displayPlannerSettings"),
        QStringLiteral("displayPx4Flow"),
        QStringLiteral("displayREPL"),
        QStringLiteral("displayRTKInject"),
        QStringLiteral("displayRadioCalibration"),
        QStringLiteral("displayRangeFinder"),
        QStringLiteral("displaySerialPorts"),
        QStringLiteral("displayServoOutput"),
        QStringLiteral("displaySikRadio"),
        QStringLiteral("displayStandardParams"),
        QStringLiteral("displayTerminal"),
        QStringLiteral("displayUserParam"),
        QStringLiteral("isAdvancedMode")
    }));
    QCOMPARE(basicJson.value(QStringLiteral("displayName")).toInt(), 0);
    QCOMPARE(advancedJson.value(QStringLiteral("displayName")).toInt(), 1);
    for (auto it = basicJson.constBegin(); it != basicJson.constEnd(); ++it) {
        if (it.key() == QStringLiteral("displayName")) {
            continue;
        }
        const bool falseInBoth = it.key() == QStringLiteral("displayStandardParams")
            || it.key() == QStringLiteral("displayAdvancedParams");
        const bool falseInBasicOnly = it.key() == QStringLiteral("isAdvancedMode")
            || it.key() == QStringLiteral("displayTerminal");
        QCOMPARE(it.value().toBool(), !falseInBoth && !falseInBasicOnly);
        QCOMPARE(advancedJson.value(it.key()).toBool(), !falseInBoth);
    }
}

void DisplayViewProfileTest::configAndSetupOsdFlagsRemainIndependent()
{
    QJsonObject custom = parsed(DisplayViewProfile::advanced());
    custom.insert(QStringLiteral("displayName"), 2);
    custom.insert(QStringLiteral("displayOSD"), false);
    custom.insert(QStringLiteral("displayOsd"), true);
    DisplayViewProfile profile;
    QVERIFY(DisplayViewProfile::fromJson(
        QJsonDocument(custom).toJson(QJsonDocument::Compact), &profile));
    QVERIFY(!profile.configFlags().displayOSD);
    QVERIFY(profile.setupFlags().displayOsd);
}

void DisplayViewProfileTest::jsonIsDeterministicAndPreservesUnknownCustomFields()
{
    const QByteArray source = QByteArrayLiteral(
        "{\"zExtension\":{\"beta\":2,\"alpha\":1},"
        "\"displayName\":2,\"displayTerminal\":false,"
        "\"aExtension\":[3,{\"z\":false,\"a\":true}]}");
    DisplayViewProfile first;
    QString error;
    QVERIFY2(DisplayViewProfile::fromJson(source, &first, &error),
             qPrintable(error));
    const QByteArray canonical = first.toJson();
    DisplayViewProfile second;
    QVERIFY(DisplayViewProfile::fromJson(canonical, &second, &error));
    QCOMPARE(second.toJson(), canonical);
    QCOMPARE(first, second);

    const QJsonObject result = parsed(second);
    QCOMPARE(result.value(QStringLiteral("zExtension")).toObject()
                 .value(QStringLiteral("alpha")).toInt(), 1);
    QCOMPARE(result.value(QStringLiteral("aExtension")).toArray().size(), 2);
    QCOMPARE(result.value(QStringLiteral("displayName")).toInt(), 2);
    QVERIFY(!second.setupFlags().displayTerminal);
}

void DisplayViewProfileTest::missingSettingsStartAdvancedWithoutPersistingImplicitly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService service(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    QCOMPARE(static_cast<int>(service.current().preset()),
             static_cast<int>(DisplayViewPreset::Advanced));
    QVERIFY(service.current().isAdvancedMode());
    QVERIFY(!settings.contains(DisplayViewProfileService::settingsKey()));
    QVERIFY(!settings.fallbacksEnabled());
}

void DisplayViewProfileTest::servicePersistsProfilesAndEmitsOnlyForChanges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService service(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    QSignalSpy changed(&service, &DisplayViewProfileService::changed);

    QString error;
    QVERIFY2(service.applyPreset(DisplayViewPreset::Basic, &error),
             qPrintable(error));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(static_cast<int>(service.current().preset()),
             static_cast<int>(DisplayViewPreset::Basic));
    QVERIFY(settings.contains(QStringLiteral("displayview")));
    DisplayViewProfile stored;
    QVERIFY(DisplayViewProfile::fromJson(
        settings.value(QStringLiteral("displayview")).toString().toUtf8(),
        &stored, &error));
    QCOMPARE(stored, DisplayViewProfile::basic());

    QVERIFY(service.applyPreset(DisplayViewPreset::Basic, &error));
    QCOMPARE(changed.count(), 1);
    QVERIFY(service.setProfile(
        service.current().withAdvancedMode(true), &error));
    QCOMPARE(changed.count(), 2);
    QCOMPARE(static_cast<int>(service.current().preset()),
             static_cast<int>(DisplayViewPreset::Custom));
    QVERIFY(service.current().isAdvancedMode());
}

void DisplayViewProfileTest::storedProfilesReceiveStartupParameterVisibilityOverrides()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    QJsonObject stored = parsed(DisplayViewProfile::advanced());
    stored.insert(QStringLiteral("displayName"), 2);
    stored.insert(QStringLiteral("displayStandardParams"), true);
    stored.insert(QStringLiteral("displayAdvancedParams"), true);
    stored.insert(QStringLiteral("displayFullParamList"), false);
    stored.insert(QStringLiteral("extension"), QStringLiteral("kept"));
    settings.setValue(QStringLiteral("displayview"), QString::fromUtf8(
        QJsonDocument(stored).toJson(QJsonDocument::Compact)));

    DisplayViewProfileService service(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    const DisplayViewProfile loaded = service.current();
    QVERIFY(!loaded.configFlags().displayStandardParams);
    QVERIFY(!loaded.configFlags().displayAdvancedParams);
    QVERIFY(loaded.configFlags().displayFullParamList);
    QCOMPARE(parsed(loaded).value(QStringLiteral("extension")).toString(),
             QStringLiteral("kept"));
}

void DisplayViewProfileTest::invalidStoredProfileFallsBackToConstructorTemplate()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("displayview"), QStringLiteral("bad"));

    DisplayViewProfileService service(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    QCOMPARE(service.current().preset(), DisplayViewPreset::Basic);
    QVERIFY(!service.current().isAdvancedMode());

    QString error;
    QVERIFY(service.reload(&error));
    QVERIFY(error.isEmpty());
    DisplayViewProfile repaired;
    QVERIFY(DisplayViewProfile::fromJson(
        settings.value(QStringLiteral("displayview")).toString().toUtf8(),
        &repaired));
    QCOMPARE(repaired, DisplayViewProfile());
}

void DisplayViewProfileTest::legacyMainWindowAdvancedModeIsMigratedOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("QGC_MAINWINDOW/ADVANCED_MODE"), false);

    DisplayViewProfileService service(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    QCOMPARE(service.current(), DisplayViewProfile::basic());
    QVERIFY(settings.contains(QStringLiteral("displayview")));

    settings.setValue(QStringLiteral("QGC_MAINWINDOW/ADVANCED_MODE"), true);
    QVERIFY(service.reload());
    QCOMPARE(service.current(), DisplayViewProfile::basic());
}

void DisplayViewProfileTest::customPresetLoadsFileAndPreservesExtensions()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString customPath = directory.filePath(
        QStringLiteral("custom.displayview"));
    writeFile(customPath, QByteArrayLiteral(
        "{\"displayName\":0,\"displayPlannerLayout\":false,"
        "\"displayEsp\":false,\"pluginFlag\":{\"enabled\":true}}"));
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService service(&settings, customPath);
    QSignalSpy changed(&service, &DisplayViewProfileService::changed);

    QString error;
    QVERIFY2(service.applyPreset(DisplayViewPreset::Custom, &error),
             qPrintable(error));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(static_cast<int>(service.current().preset()),
             static_cast<int>(DisplayViewPreset::Custom));
    QVERIFY(!service.current().displayPlannerLayout());
    QVERIFY(!service.current().setupFlags().displayEsp);
    QVERIFY(parsed(service.current()).value(QStringLiteral("pluginFlag"))
                .toObject().value(QStringLiteral("enabled")).toBool());

    DisplayViewProfile restored;
    QVERIFY(DisplayViewProfile::fromJson(
        settings.value(QStringLiteral("displayview")).toString().toUtf8(),
        &restored, &error));
    QCOMPARE(restored, service.current());
}

void DisplayViewProfileTest::invalidCustomProfileDoesNotReplaceCurrentProfile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString customPath = directory.filePath(
        QStringLiteral("custom.displayview"));
    writeFile(customPath, QByteArrayLiteral("not json"));
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService service(&settings, customPath);
    QSignalSpy changed(&service, &DisplayViewProfileService::changed);

    QString error;
    QVERIFY(!service.applyPreset(DisplayViewPreset::Custom, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(changed.count(), 0);
    QCOMPARE(service.current(), DisplayViewProfile::advanced());
    QVERIFY(!settings.contains(QStringLiteral("displayview")));
}

void DisplayViewProfileTest::legacySettingsAreNotMigrated()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("advancedview"), false);
    DisplayViewProfileService service(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    QCOMPARE(service.current(), DisplayViewProfile::advanced());
    QVERIFY(settings.contains(QStringLiteral("advancedview")));
    QVERIFY(!settings.contains(QStringLiteral("displayview")));
}

QTEST_GUILESS_MAIN(DisplayViewProfileTest)

#include "test_displayviewprofile.moc"
