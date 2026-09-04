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
    void pendingControlDisclosuresAreExact();
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

void ConfigPlannerViewTest::pendingControlDisclosuresAreExact()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    DisplayViewProfileService profiles(
        &settings, directory.filePath(QStringLiteral("custom.displayview")));
    ConfigPlannerViewModel model(&settings, &profiles);
    ConfigPlannerView view(&model);

    auto *layoutLabel = view.findChild<QLabel *>(
        QStringLiteral("DisplayLayoutLabel"));
    auto *layout = view.findChild<QComboBox *>(
        QStringLiteral("CMB_displayview"));
    auto *display = view.findChild<QLabel *>(
        QStringLiteral("DisplayPendingNote"));
    auto *waypoints = view.findChild<QLabel *>(
        QStringLiteral("WaypointsConnectPendingNote"));
    auto *telemetry = view.findChild<QLabel *>(
        QStringLiteral("TelemetryRatesPendingNote"));
    QVERIFY(layoutLabel);
    QVERIFY(layout);
    QVERIFY(display);
    QVERIFY(waypoints);
    QVERIFY(telemetry);

    QCOMPARE(layoutLabel->text(), QStringLiteral("Layout"));
    QCOMPARE(layout->toolTip(), QStringLiteral(
        "Controls the shared Basic/Advanced/Custom CONFIG and SETUP "
        "visibility profile; it does not change the color theme."));
    QCOMPARE(display->text(), QStringLiteral(
        "MP10 UI language, color Theme, Edit Custom theme editor, speed "
        "units and OSD color do not yet have complete Qt consumers. The "
        "Layout selector above changes only the shared CONFIG/SETUP "
        "visibility profile. Useful legacy Qt appearance choices remain "
        "available in Legacy options."));
    QCOMPARE(waypoints->text(), QStringLiteral(
        "MP10 waypoint-on-connect, distance-to-home Flight Data display, "
        "map rotation, USB reset, ESP32 RTS reset and no-RC policies do "
        "not yet have Qt consumers."));
    QCOMPARE(telemetry->text(), QStringLiteral(
        "The five MP10 grouped stream rates, Track Length and target-safe "
        "parameter refresh are not ported. GCS sysid has a persisted "
        "startup consumer, but no safe native editor until every outbound "
        "service can be updated atomically. The different seven-rate APM "
        "Planner editor remains available in Legacy options."));
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
    QCOMPARE(model.messageSeverity(), 4);
    QVERIFY(!settings.contains(QStringLiteral("severity")));
    QVERIFY(!model.speechEnabled());
    QVERIFY(!model.speechCustomEnabled());
    QVERIFY(!model.speechAltWarningEnabled());
    QVERIFY(!model.speechLowSpeedEnabled());
    QCOMPARE(model.speechCustomTemplate(), QStringLiteral(
        "Heading to Waypoint {wpn}, altitude is {alt}, Ground speed is {gsp} "));
    QCOMPARE(model.speechAltWarningTemplate(),
             QStringLiteral("WARNING, low altitude {alt}"));
    QCOMPARE(model.speechAltWarningHeightMeters(), 2.0);
    QVERIFY(!model.speechAltWarningHeightConfigured());
    QCOMPARE(model.speechLowGroundSpeedTemplate(),
             QStringLiteral("Low Ground Speed {gsp}"));
    QCOMPARE(model.speechLowAirSpeedTemplate(),
             QStringLiteral("Low Air Speed {asp}"));
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
    QCOMPARE(model.altitudeUnitLabel(), QStringLiteral("ft"));
    QVERIFY(qAbs(model.altitudeFromMeters(0.3048) - 1.0) < 1.0e-12);
    QVERIFY(qAbs(model.altitudeToMeters(1.0) - 0.3048) < 1.0e-12);
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
    auto *firstSeverity = first.findChild<QComboBox *>(
        QStringLiteral("CMB_severity"));
    auto *secondSeverity = second.findChild<QComboBox *>(
        QStringLiteral("CMB_severity"));
    auto *testSpeech = first.findChild<QPushButton *>(
        QStringLiteral("SpeechTest"));
    auto *status = first.findChild<QLabel *>(
        QStringLiteral("SpeechBackendStatus"));
    QVERIFY(firstHud);
    QVERIFY(secondHud);
    QVERIFY(firstSpeech);
    QVERIFY(secondSpeech);
    QVERIFY(firstSeverity);
    QVERIFY(secondSeverity);
    QVERIFY(testSpeech);
    QVERIFY(status);
    QVERIFY(firstHud->isChecked());
    QVERIFY(!firstSpeech->isChecked());
    QCOMPARE(firstSeverity->count(), 8);
    QCOMPARE(firstSeverity->itemText(0), QStringLiteral("Emergency"));
    QCOMPARE(firstSeverity->itemText(7), QStringLiteral("Debug"));
    QCOMPARE(firstSeverity->currentData().toInt(), 4);

    QSignalSpy hudChanged(&model,
                          &ConfigPlannerViewModel::hudOverlayEnabledChanged);
    QSignalSpy speechChanged(&model,
                             &ConfigPlannerViewModel::speechEnabledChanged);
    QSignalSpy testRequested(&first,
                             &ConfigPlannerView::speechTestRequested);
    firstHud->setChecked(false);
    firstSpeech->setChecked(true);
    firstSeverity->setCurrentIndex(firstSeverity->findData(6));
    QCOMPARE(hudChanged.count(), 1);
    QCOMPARE(speechChanged.count(), 1);
    QVERIFY(!secondHud->isChecked());
    QVERIFY(secondSpeech->isChecked());
    QCOMPARE(settings.value(QStringLiteral("CHK_hudshow")).toBool(), false);
    QCOMPARE(settings.value(QStringLiteral("speechenable")).toBool(), true);
    QCOMPARE(settings.value(QStringLiteral("severity")).toInt(), 6);
    QCOMPARE(secondSeverity->currentData().toInt(), 6);

    testSpeech->click();
    QCOMPARE(testRequested.count(), 1);
    first.setSpeechBackendStatus(QStringLiteral("ready"));
    QCOMPARE(status->text(), QStringLiteral("ready"));

    settings.setValue(QStringLiteral("CHK_hudshow"), true);
    settings.setValue(QStringLiteral("speechenable"), false);
    settings.setValue(QStringLiteral("severity"), 2);
    settings.sync();
    model.reload();
    QVERIFY(firstHud->isChecked());
    QVERIFY(!firstSpeech->isChecked());
    QVERIFY(secondHud->isChecked());
    QVERIFY(!secondSpeech->isChecked());
    QCOMPARE(firstSeverity->currentData().toInt(), 2);
    QCOMPARE(secondSeverity->currentData().toInt(), 2);
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
    auto *custom = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechcustom"));
    auto *battery = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechbattery"));
    auto *altWarning = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechaltwarning"));
    auto *arm = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speecharmdisarm"));
    auto *lowSpeed = first.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechlowspeed"));
    QVERIFY(master);
    QVERIFY(subOptions);
    QVERIFY(armedOnly);
    QVERIFY(waypoint);
    QVERIFY(mode);
    QVERIFY(custom);
    QVERIFY(battery);
    QVERIFY(altWarning);
    QVERIFY(arm);
    QVERIFY(lowSpeed);
    QVERIFY(subOptions->isHidden());

    QStringList speechOrder;
    for (QCheckBox *option : subOptions->findChildren<QCheckBox *>(
             QString(), Qt::FindDirectChildrenOnly)) {
        speechOrder.append(option->objectName());
    }
    QCOMPARE(speechOrder,
             QStringList({QStringLiteral("CHK_speech_armed_only"),
                          QStringLiteral("CHK_speechwaypoint"),
                          QStringLiteral("CHK_speechmode"),
                          QStringLiteral("CHK_speechcustom"),
                          QStringLiteral("CHK_speechbattery"),
                          QStringLiteral("CHK_speechaltwarning"),
                          QStringLiteral("CHK_speecharmdisarm"),
                          QStringLiteral("CHK_speechlowspeed")}));

    QSignalSpy waypointPrompt(
        &first, &ConfigPlannerView::speechWaypointConfigurationRequested);
    QSignalSpy modePrompt(
        &first, &ConfigPlannerView::speechModeConfigurationRequested);
    QSignalSpy customPrompt(
        &first, &ConfigPlannerView::speechCustomConfigurationRequested);
    QSignalSpy batteryPrompt(
        &first, &ConfigPlannerView::speechBatteryConfigurationRequested);
    QSignalSpy altWarningPrompt(
        &first, &ConfigPlannerView::speechAltWarningConfigurationRequested);
    QSignalSpy armPrompt(
        &first, &ConfigPlannerView::speechArmConfigurationRequested);
    QSignalSpy lowSpeedPrompt(
        &first, &ConfigPlannerView::speechLowSpeedConfigurationRequested);

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
    custom->setChecked(true);
    QCOMPARE(customPrompt.count(), 1);
    QVERIFY(!custom->isChecked());
    QVERIFY(!settings.contains(QStringLiteral("speechcustomenabled")));
    QVERIFY(!settings.contains(QStringLiteral("speechcustom")));

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
    connect(&first, &ConfigPlannerView::speechCustomConfigurationRequested,
            &model, [&model]() {
        model.setSpeechCustomTemplate(QStringLiteral(
            "Heading to Waypoint {wpn}, altitude is {alt}, Ground speed is {gsp} "));
        model.setSpeechCustomEnabled(true);
    });
    connect(&first, &ConfigPlannerView::speechBatteryConfigurationRequested,
            &model, [&model]() {
        model.setSpeechBatteryTemplate(QStringLiteral(
            "WARNING, Battery at {batv} Volt, {batp} percent"));
        model.setSpeechBatteryWarningVoltage(9.6);
        model.setSpeechBatteryWarningPercent(20.0);
        model.setSpeechBatteryEnabled(true);
    });
    connect(&first,
            &ConfigPlannerView::speechAltWarningConfigurationRequested,
            &model, [&model]() {
        model.setSpeechAltWarningTemplate(
            QStringLiteral("WARNING, low altitude {alt}"));
        model.setSpeechAltWarningHeightMeters(15.0);
        model.setSpeechAltWarningEnabled(true);
    });
    connect(&first, &ConfigPlannerView::speechArmConfigurationRequested,
            &model, [&model]() {
        model.setSpeechArmTemplate(QStringLiteral("Armed"));
        model.setSpeechDisarmTemplate(QStringLiteral("Disarmed"));
        model.setSpeechArmDisarmEnabled(true);
    });
    connect(&first, &ConfigPlannerView::speechLowSpeedConfigurationRequested,
            &model, [&model]() {
        model.setSpeechLowGroundSpeedTemplate(
            QStringLiteral("Low Ground Speed {gsp}"));
        model.setSpeechLowGroundSpeedTriggerMps(3.0);
        model.setSpeechLowAirSpeedTemplate(
            QStringLiteral("Low Air Speed {asp}"));
        model.setSpeechLowAirSpeedTriggerMps(5.0);
        model.setSpeechLowSpeedEnabled(true);
    });

    waypoint->setChecked(true);
    mode->setChecked(true);
    custom->setChecked(true);
    battery->setChecked(true);
    altWarning->setChecked(true);
    arm->setChecked(true);
    lowSpeed->setChecked(true);
    QCOMPARE(waypointPrompt.count(), 2);
    QCOMPARE(modePrompt.count(), 1);
    QCOMPARE(customPrompt.count(), 2);
    QCOMPARE(batteryPrompt.count(), 1);
    QCOMPARE(altWarningPrompt.count(), 1);
    QCOMPARE(armPrompt.count(), 1);
    QCOMPARE(lowSpeedPrompt.count(), 1);

    QCOMPARE(settings.value(QStringLiteral("speech_armed_only")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechwaypointenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechmodeenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechcustomenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechbatteryenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechaltenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speecharmenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechlowspeedenabled")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("speechwaypoint")).toString(),
             QStringLiteral("Heading to Waypoint {wpn}"));
    QCOMPARE(settings.value(QStringLiteral("speechmode")).toString(),
             QStringLiteral("Mode changed to {mode}"));
    QCOMPARE(settings.value(QStringLiteral("speechcustom")).toString(),
             QStringLiteral(
                 "Heading to Waypoint {wpn}, altitude is {alt}, Ground speed is {gsp} "));
    QCOMPARE(settings.value(QStringLiteral("speecharm")).toString(),
             QStringLiteral("Armed"));
    QCOMPARE(settings.value(QStringLiteral("speechdisarm")).toString(),
             QStringLiteral("Disarmed"));
    QCOMPARE(settings.value(QStringLiteral("speechbatteryvolt")).toDouble(),
             9.6);
    QCOMPARE(settings.value(QStringLiteral("speechbatterypercent")).toDouble(),
             20.0);
    QCOMPARE(settings.value(QStringLiteral("speechalt")).toString(),
             QStringLiteral("WARNING, low altitude {alt}"));
    QCOMPARE(settings.value(QStringLiteral("speechaltheight")).toDouble(),
             15.0);
    QCOMPARE(settings.value(QStringLiteral("speechlowgroundspeed")).toString(),
             QStringLiteral("Low Ground Speed {gsp}"));
    QCOMPARE(settings.value(
                 QStringLiteral("speechlowgroundspeedtrigger")).toDouble(),
             3.0);
    QCOMPARE(settings.value(QStringLiteral("speechlowairspeed")).toString(),
             QStringLiteral("Low Air Speed {asp}"));
    QCOMPARE(settings.value(
                 QStringLiteral("speechlowairspeedtrigger")).toDouble(),
             5.0);

    auto *secondMode = second.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechmode"));
    auto *secondArm = second.findChild<QCheckBox *>(
        QStringLiteral("CHK_speecharmdisarm"));
    auto *secondCustom = second.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechcustom"));
    auto *secondAltWarning = second.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechaltwarning"));
    auto *secondLowSpeed = second.findChild<QCheckBox *>(
        QStringLiteral("CHK_speechlowspeed"));
    auto *secondSubOptions = second.findChild<QWidget *>(
        QStringLiteral("SpeechSubOptions"));
    QVERIFY(secondMode);
    QVERIFY(secondArm);
    QVERIFY(secondCustom);
    QVERIFY(secondAltWarning);
    QVERIFY(secondLowSpeed);
    QVERIFY(secondSubOptions);
    QVERIFY(!secondSubOptions->isHidden());
    QVERIFY(secondMode->isChecked());
    QVERIFY(secondArm->isChecked());
    QVERIFY(secondCustom->isChecked());
    QVERIFY(secondAltWarning->isChecked());
    QVERIFY(secondLowSpeed->isChecked());

    QVERIFY(model.setSpeechModeTemplate(QStringLiteral("Now {mode}")));
    QVERIFY(model.setSpeechWaypointTemplate(QStringLiteral("WP {wpn}")));
    QVERIFY(model.setSpeechCustomTemplate(QStringLiteral("Status {alt}")));
    QVERIFY(model.setSpeechArmTemplate(QStringLiteral("Vehicle armed")));
    QVERIFY(model.setSpeechDisarmTemplate(QStringLiteral("Vehicle safe")));
    QVERIFY(model.setSpeechBatteryTemplate(QStringLiteral("Battery {batp}")));
    QVERIFY(model.setSpeechBatteryWarningVoltage(10.5));
    QVERIFY(model.setSpeechBatteryWarningPercent(25.0));
    QVERIFY(model.setSpeechAltWarningTemplate(QStringLiteral("Alt {alt}")));
    QVERIFY(model.setSpeechAltWarningHeightMeters(25.0));
    QVERIFY(model.setSpeechLowGroundSpeedTemplate(QStringLiteral("GS {gsp}")));
    QVERIFY(model.setSpeechLowGroundSpeedTriggerMps(4.0));
    QVERIFY(model.setSpeechLowAirSpeedTemplate(QStringLiteral("AS {asp}")));
    QVERIFY(model.setSpeechLowAirSpeedTriggerMps(6.0));
    QCOMPARE(model.speechModeTemplate(), QStringLiteral("Now {mode}"));
    QCOMPARE(model.speechWaypointTemplate(), QStringLiteral("WP {wpn}"));
    QCOMPARE(model.speechCustomTemplate(), QStringLiteral("Status {alt}"));
    QCOMPARE(model.speechArmTemplate(), QStringLiteral("Vehicle armed"));
    QCOMPARE(model.speechDisarmTemplate(), QStringLiteral("Vehicle safe"));
    QCOMPARE(model.speechBatteryTemplate(), QStringLiteral("Battery {batp}"));
    QCOMPARE(model.speechBatteryWarningVoltage(), 10.5);
    QCOMPARE(model.speechBatteryWarningPercent(), 25.0);
    QCOMPARE(model.speechAltWarningTemplate(), QStringLiteral("Alt {alt}"));
    QCOMPARE(model.speechAltWarningHeightMeters(), 25.0);
    QCOMPARE(model.speechLowGroundSpeedTemplate(), QStringLiteral("GS {gsp}"));
    QCOMPARE(model.speechLowGroundSpeedTriggerMps(), 4.0);
    QCOMPARE(model.speechLowAirSpeedTemplate(), QStringLiteral("AS {asp}"));
    QCOMPARE(model.speechLowAirSpeedTriggerMps(), 6.0);

    master->setChecked(false);
    QVERIFY(subOptions->isHidden());
    QVERIFY(secondSubOptions->isHidden());
    QVERIFY(model.speechModeEnabled());
    QVERIFY(model.speechArmDisarmEnabled());
    QVERIFY(model.speechCustomEnabled());
    QVERIFY(model.speechAltWarningEnabled());
    QVERIFY(model.speechLowSpeedEnabled());
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
