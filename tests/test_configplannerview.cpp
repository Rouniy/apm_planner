#include <QtTest>

#include "ui/configuration/ConfigPlannerView.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>

namespace {
QMap<QString, QVariant> settingsSnapshot(QSettings &settings)
{
    QMap<QString, QVariant> result;
    for (const QString &key : settings.allKeys()) {
        result.insert(key, settings.value(key));
    }
    return result;
}
}

class ConfigPlannerViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void exactNineSectionsArePresentAndNonEmpty();
    void constructionDoesNotPersistDefaults();
    void defaultViewsShareApplicationModel();
    void modelPersistsOwnedSettingsAndEmitsLiveUnitRequests();
    void hudAndSpeechControlsPersistAndSynchronize();
    void speechEventControlsPersistPromptAndSynchronize();
    void viewSynchronizesProfilesStartupAndRuntimeState();
    void mapAndLegacyRequestsStayOutsideTheView();

private:
    std::unique_ptr<QTemporaryDir> m_settingsDirectory;
};

void ConfigPlannerViewTest::initTestCase()
{
    m_settingsDirectory.reset(new QTemporaryDir);
    QVERIFY(m_settingsDirectory->isValid());
    const QString user = QDir(m_settingsDirectory->path()).filePath(
        QStringLiteral("user"));
    const QString system = QDir(m_settingsDirectory->path()).filePath(
        QStringLiteral("system"));
    QVERIFY(QDir().mkpath(user));
    QVERIFY(QDir().mkpath(system));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, user);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, system);
    QCoreApplication::setOrganizationName(
        QStringLiteral("APMPlannerNativePlannerTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("ConfigPlannerView"));
}

void ConfigPlannerViewTest::exactNineSectionsArePresentAndNonEmpty()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService profiles(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    ConfigPlannerViewModel model(&settings, &profiles);
    ConfigPlannerView view(&model);

    QCOMPARE(ConfigPlannerViewModel::sectionTitles().size(), 9);
    QList<QGroupBox *> sections;
    for (QGroupBox *group : view.findChildren<QGroupBox *>()) {
        if (group->property("plannerSection").toBool()) {
            sections.append(group);
        }
    }
    std::sort(sections.begin(), sections.end(),
              [](const QGroupBox *left, const QGroupBox *right) {
        return left->property("sectionIndex").toInt()
            < right->property("sectionIndex").toInt();
    });
    QCOMPARE(sections.size(), 9);
    QStringList titles;
    for (QGroupBox *section : sections) {
        titles.append(section->title());
        bool hasMeaningfulContent = false;
        for (QWidget *child : section->findChildren<QWidget *>()) {
            if (child->isHidden()) continue;
            if (qobject_cast<QAbstractButton *>(child)
                || qobject_cast<QComboBox *>(child)
                || qobject_cast<QSpinBox *>(child)) {
                hasMeaningfulContent = true;
                break;
            }
            auto *label = qobject_cast<QLabel *>(child);
            if (label && !label->text().trimmed().isEmpty()) {
                hasMeaningfulContent = true;
                break;
            }
        }
        QVERIFY2(hasMeaningfulContent,
                 qPrintable(section->objectName()));
    }
    QCOMPARE(titles, ConfigPlannerViewModel::sectionTitles());
    QCOMPARE(sections.first()->objectName(),
             QStringLiteral("PlannerDisplaySection"));
    QCOMPARE(sections.last()->objectName(),
             QStringLiteral("PlannerAdvancedSection"));
    QVERIFY(view.findChild<QLabel *>(
        QStringLiteral("FlightShortcutsPendingNote")));
    QVERIFY(view.findChild<QLabel *>(
        QStringLiteral("TelemetryRatesPendingNote")));
}

void ConfigPlannerViewTest::defaultViewsShareApplicationModel()
{
    ConfigPlannerView first;
    ConfigPlannerView second;
    QVERIFY(first.viewModel());
    QCOMPARE(first.viewModel(), second.viewModel());
}

void ConfigPlannerViewTest::constructionDoesNotPersistDefaults()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("sentinel"), 7);
    settings.sync();
    const QMap<QString, QVariant> before = settingsSnapshot(settings);

    DisplayViewProfileService profiles(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    ConfigPlannerViewModel model(&settings, &profiles);
    ConfigPlannerView view(&model);
    QCOMPARE(settingsSnapshot(settings), before);
    QCOMPARE(model.altitudeUnits(), QStringLiteral("Meters"));
    QCOMPARE(model.distanceUnits(), QStringLiteral("Meters"));
    QCOMPARE(model.displayProfile().preset(), DisplayViewPreset::Advanced);
    QCOMPARE(model.startupUdpOptions().orderedPorts(),
             QList<int>({14550, 14551}));
    QVERIFY(!model.betaUpdatesEnabled());
    QVERIFY(model.hudOverlayEnabled());
    QVERIFY(!model.speechEnabled());
}

void ConfigPlannerViewTest::modelPersistsOwnedSettingsAndEmitsLiveUnitRequests()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService profiles(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    ConfigPlannerViewModel model(&settings, &profiles);
    QSignalSpy altitude(&model,
                       &ConfigPlannerViewModel::altitudeUnitsChanged);
    QSignalSpy distance(&model,
                       &ConfigPlannerViewModel::distanceUnitsChanged);

    QVERIFY(model.setAltitudeUnits(QStringLiteral("feet")));
    QVERIFY(model.setDistanceUnits(QStringLiteral("FEET")));
    QCOMPARE(altitude.count(), 1);
    QCOMPARE(distance.count(), 1);
    QVERIFY(!settings.contains(QStringLiteral("altunits")));
    QVERIFY(!settings.contains(QStringLiteral("distunits")));
    QVERIFY(!model.setAltitudeUnits(QStringLiteral("yards")));
    QCOMPARE(altitude.count(), 1);

    QVERIFY(model.setDisplayPreset(DisplayViewPreset::Basic));
    QCOMPARE(profiles.current().preset(), DisplayViewPreset::Basic);
    PlannerStartupUdpOptions startup;
    startup.enabled = false;
    startup.primaryPort = 16000;
    startup.alternatePort = 16000;
    QVERIFY(model.setStartupUdpOptions(startup));
    QVERIFY(model.startupUdpOptions().orderedPorts().isEmpty());
    QVERIFY(model.setBetaUpdatesEnabled(true));
    settings.beginGroup(QStringLiteral("AUTO_UPDATE"));
    QCOMPARE(settings.value(QStringLiteral("RELEASE_TYPE")).toString(),
             QStringLiteral("beta"));
    settings.endGroup();
}

void ConfigPlannerViewTest::viewSynchronizesProfilesStartupAndRuntimeState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService profiles(
        &settings, directory.filePath(QStringLiteral("missing.displayview")));
    ConfigPlannerViewModel model(&settings, &profiles);
    ConfigPlannerView view(&model);

    auto *layout = view.findChild<QComboBox *>(QStringLiteral("CMB_displayview"));
    auto *enabled = view.findChild<QCheckBox *>(
        QStringLiteral("CHK_startup_udp_listeners"));
    auto *primary = view.findChild<QSpinBox *>(
        QStringLiteral("NUM_startup_udp_primary_port"));
    auto *alternate = view.findChild<QSpinBox *>(
        QStringLiteral("NUM_startup_udp_alternate_port"));
    QVERIFY(layout);
    QVERIFY(enabled);
    QVERIFY(primary);
    QVERIFY(alternate);
    QCOMPARE(layout->currentData().toInt(),
             static_cast<int>(DisplayViewPreset::Advanced));
    QVERIFY(profiles.applyPreset(DisplayViewPreset::Basic));
    QCOMPARE(layout->currentData().toInt(),
             static_cast<int>(DisplayViewPreset::Basic));

    enabled->setChecked(false);
    QVERIFY(!primary->isEnabled());
    QVERIFY(!alternate->isEnabled());
    QVERIFY(!model.startupUdpOptions().enabled);

    QSignalSpy audio(&view, &ConfigPlannerView::audioMuteChanged);
    QSignalSpy heartbeat(&view, &ConfigPlannerView::heartbeatChanged);
    QSignalSpy logging(&view, &ConfigPlannerView::mavlinkLoggingChanged);
    QSignalSpy proxy(&view, &ConfigPlannerView::autoProxyChanged);
    view.setAudioMuted(true);
    view.setHeartbeatEnabled(true);
    view.setMavlinkLoggingEnabled(true);
    view.setAutoProxyEnabled(true);
    QCOMPARE(audio.count(), 0);
    QCOMPARE(heartbeat.count(), 0);
    QCOMPARE(logging.count(), 0);
    QCOMPARE(proxy.count(), 0);
}

void ConfigPlannerViewTest::hudAndSpeechControlsPersistAndSynchronize()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService profiles(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    ConfigPlannerViewModel model(&settings, &profiles);
    ConfigPlannerView first(&model);
    ConfigPlannerView second(&model);

    auto *firstHud = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_hudshow"));
    auto *secondHud = second.findChild<QCheckBox *>(
        QStringLiteral("CHK_hudshow"));
    auto *firstSpeech = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechenable"));
    auto *secondSpeech = second.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechenable"));
    auto *testSpeech = first.findChild<QPushButton *>(
        QStringLiteral("SpeechTest"));
    auto *status = first.findChild<QLabel *>(
        QStringLiteral("SpeechBackendStatus"));
    QVERIFY(firstHud);
    QVERIFY(secondHud);
    QVERIFY(firstSpeech);
    QVERIFY(secondSpeech);
    QVERIFY(testSpeech);
    QVERIFY(status);
    QVERIFY(firstHud->isChecked());
    QVERIFY(!firstSpeech->isChecked());

    QSignalSpy hudChanged(&model,
                          &ConfigPlannerViewModel::hudOverlayEnabledChanged);
    QSignalSpy speechChanged(&model,
                             &ConfigPlannerViewModel::speechEnabledChanged);
    QSignalSpy testRequested(&first,
                             &ConfigPlannerView::speechTestRequested);
    firstHud->setChecked(false);
    firstSpeech->setChecked(true);
    QCOMPARE(hudChanged.count(), 1);
    QCOMPARE(speechChanged.count(), 1);
    QVERIFY(!secondHud->isChecked());
    QVERIFY(secondSpeech->isChecked());
    QCOMPARE(settings.value(QStringLiteral("CHK_hudshow")).toBool(), false);
    QCOMPARE(settings.value(QStringLiteral("speechenable")).toBool(), true);

    testSpeech->click();
    QCOMPARE(testRequested.count(), 1);
    first.setSpeechBackendStatus(QStringLiteral("ready"));
    QCOMPARE(status->text(), QStringLiteral("ready"));

    settings.setValue(QStringLiteral("CHK_hudshow"), true);
    settings.setValue(QStringLiteral("speechenable"), false);
    settings.sync();
    model.reload();
    QVERIFY(firstHud->isChecked());
    QVERIFY(!firstSpeech->isChecked());
    QVERIFY(secondHud->isChecked());
    QVERIFY(!secondSpeech->isChecked());
}

void ConfigPlannerViewTest::speechEventControlsPersistPromptAndSynchronize()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService profiles(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    ConfigPlannerViewModel model(&settings, &profiles);
    ConfigPlannerView first(&model);
    ConfigPlannerView second(&model);

    auto *master = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechenable"));
    auto *subOptions = first.findChild<QWidget *>(
        QStringLiteral("SpeechSubOptions"));
    auto *armedOnly = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speech_armed_only"));
    auto *waypoint = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechwaypoint"));
    auto *mode = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechmode"));
    auto *battery = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechbattery"));
    auto *arm = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speecharmdisarm"));
    QVERIFY(master);
    QVERIFY(subOptions);
    QVERIFY(armedOnly);
    QVERIFY(waypoint);
    QVERIFY(mode);
    QVERIFY(battery);
    QVERIFY(arm);
    QVERIFY(subOptions->isHidden());

    QSignalSpy waypointPrompt(
        &first, &ConfigPlannerView::speechWaypointConfigurationRequested);
    QSignalSpy modePrompt(
        &first, &ConfigPlannerView::speechModeConfigurationRequested);
    QSignalSpy batteryPrompt(
        &first, &ConfigPlannerView::speechBatteryConfigurationRequested);
    QSignalSpy armPrompt(
        &first, &ConfigPlannerView::speechArmConfigurationRequested);

    master->setChecked(true);
    QVERIFY(!subOptions->isHidden());
    armedOnly->setChecked(true);

    // Without a configuration handler, enabling is treated as cancelled:
    // no event policy or fallback template is persisted.
    waypoint->setChecked(true);
    QCOMPARE(waypointPrompt.count(), 1);
    QVERIFY(!waypoint->isChecked());
    QVERIFY(!settings.contains(QStringLiteral("speechwaypointenabled")));
    QVERIFY(!settings.contains(QStringLiteral("speechwaypoint")));

    connect(&first,
            &ConfigPlannerView::speechWaypointConfigurationRequested,
            &model, [&model]() {
        model.setSpeechWaypointTemplate(
            QStringLiteral("Heading to Waypoint {wpn}"));
        model.setSpeechWaypointEnabled(true);
    });
    connect(&first, &ConfigPlannerView::speechModeConfigurationRequested,
            &model, [&model]() {
        model.setSpeechModeTemplate(QStringLiteral("Mode changed to {mode}"));
        model.setSpeechModeEnabled(true);
    });
    connect(&first, &ConfigPlannerView::speechBatteryConfigurationRequested,
            &model, [&model]() {
        model.setSpeechBatteryTemplate(QStringLiteral(
            "WARNING, Battery at {batv} Volt, {batp} percent"));
        model.setSpeechBatteryWarningVoltage(9.6);
        model.setSpeechBatteryWarningPercent(20.0);
        model.setSpeechBatteryEnabled(true);
    });
    connect(&first, &ConfigPlannerView::speechArmConfigurationRequested,
            &model, [&model]() {
        model.setSpeechArmTemplate(QStringLiteral("Armed"));
        model.setSpeechDisarmTemplate(QStringLiteral("Disarmed"));
        model.setSpeechArmDisarmEnabled(true);
    });

    waypoint->setChecked(true);
    mode->setChecked(true);
    battery->setChecked(true);
    arm->setChecked(true);
    QCOMPARE(waypointPrompt.count(), 2);
    QCOMPARE(modePrompt.count(), 1);
    QCOMPARE(batteryPrompt.count(), 1);
    QCOMPARE(armPrompt.count(), 1);

    QCOMPARE(settings.value(QStringLiteral("speech_armed_only")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechwaypointenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechmodeenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechbatteryenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speecharmenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechwaypoint")).toString(),
             QStringLiteral("Heading to Waypoint {wpn}"));
    QCOMPARE(settings.value(QStringLiteral("speechmode")).toString(),
             QStringLiteral("Mode changed to {mode}"));
    QCOMPARE(settings.value(QStringLiteral("speecharm")).toString(),
             QStringLiteral("Armed"));
    QCOMPARE(settings.value(QStringLiteral("speechdisarm")).toString(),
             QStringLiteral("Disarmed"));
    QCOMPARE(settings.value(QStringLiteral("speechbatteryvolt")).toDouble(),
             9.6);
    QCOMPARE(settings.value(QStringLiteral("speechbatterypercent")).toDouble(),
             20.0);

    auto *secondMode = second.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechmode"));
    auto *secondArm = second.findChild<QCheckBox *>(
        QStringLiteral("CHK_speecharmdisarm"));
    auto *secondSubOptions = second.findChild<QWidget *>(
        QStringLiteral("SpeechSubOptions"));
    QVERIFY(secondMode);
    QVERIFY(secondArm);
    QVERIFY(secondSubOptions);
    QVERIFY(!secondSubOptions->isHidden());
    QVERIFY(secondMode->isChecked());
    QVERIFY(secondArm->isChecked());

    QVERIFY(model.setSpeechModeTemplate(QStringLiteral("Now {mode}")));
    QVERIFY(model.setSpeechWaypointTemplate(QStringLiteral("WP {wpn}")));
    QVERIFY(model.setSpeechArmTemplate(QStringLiteral("Vehicle armed")));
    QVERIFY(model.setSpeechDisarmTemplate(QStringLiteral("Vehicle safe")));
    QVERIFY(model.setSpeechBatteryTemplate(QStringLiteral("Battery {batp}")));
    QVERIFY(model.setSpeechBatteryWarningVoltage(10.5));
    QVERIFY(model.setSpeechBatteryWarningPercent(25.0));
    QCOMPARE(model.speechModeTemplate(), QStringLiteral("Now {mode}"));
    QCOMPARE(model.speechWaypointTemplate(), QStringLiteral("WP {wpn}"));
    QCOMPARE(model.speechArmTemplate(), QStringLiteral("Vehicle armed"));
    QCOMPARE(model.speechDisarmTemplate(), QStringLiteral("Vehicle safe"));
    QCOMPARE(model.speechBatteryTemplate(), QStringLiteral("Battery {batp}"));
    QCOMPARE(model.speechBatteryWarningVoltage(), 10.5);
    QCOMPARE(model.speechBatteryWarningPercent(), 25.0);

    master->setChecked(false);
    QVERIFY(subOptions->isHidden());
    QVERIFY(secondSubOptions->isHidden());
    QVERIFY(model.speechModeEnabled());
    QVERIFY(model.speechArmDisarmEnabled());
}

void ConfigPlannerViewTest::mapAndLegacyRequestsStayOutsideTheView()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService profiles(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    ConfigPlannerViewModel model(&settings, &profiles);
    ConfigPlannerView view(&model);
    QSignalSpy mapRequested(&view,
                           &ConfigPlannerView::mapBackendRequested);
    QSignalSpy legacyRequested(&view,
                              &ConfigPlannerView::legacyOptionsRequested);
    QSignalSpy telemetryRequested(
        &view, &ConfigPlannerView::legacyTelemetryOptionsRequested);

    view.setMapBackends({{QStringLiteral("one"), QStringLiteral("One")},
                         {QStringLiteral("two"), QStringLiteral("Two")}},
                        QStringLiteral("one"));
    auto *map = view.findChild<QComboBox *>(
        QStringLiteral("MapWidgetBackendComboBox"));
    QVERIFY(map);
    map->setCurrentIndex(1);
    QCOMPARE(mapRequested.count(), 1);
    QCOMPARE(mapRequested.takeFirst().first().toString(),
             QStringLiteral("two"));

    auto *legacy = view.findChild<QPushButton *>(
        QStringLiteral("OpenLegacyPlannerOptions"));
    QVERIFY(legacy);
    legacy->click();
    QCOMPARE(legacyRequested.count(), 1);
    QCOMPARE(telemetryRequested.count(), 0);

    auto *telemetry = view.findChild<QPushButton *>(
        QStringLiteral("OpenLegacyTelemetryOptions"));
    QVERIFY(telemetry);
    telemetry->click();
    QCOMPARE(legacyRequested.count(), 1);
    QCOMPARE(telemetryRequested.count(), 1);
}

QTEST_MAIN(ConfigPlannerViewTest)

#include "test_configplannerview.moc"
