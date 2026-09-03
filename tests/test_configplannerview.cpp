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
