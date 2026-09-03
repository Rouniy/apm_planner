#include "ui/configuration/PlannerStartupUdpOptions.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QSettings settingsFor(const QTemporaryDir &directory, const QString &name)
{
    return QSettings(directory.filePath(name), QSettings::IniFormat);
}
}

class PlannerStartupUdpOptionsTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsMatchMissionPlanner10();
    void detectsOnlyExplicitStartupConfiguration();
    void invalidPortsUseFieldSpecificFallbacks();
    void enabledValueIsParsedDefensively();
    void orderedPortsHonorEnabledStateAndDeduplicate();
    void savePersistsNormalizedValues();
    void statusAndRestartNoteAreDeterministic();
};

void PlannerStartupUdpOptionsTest::defaultsMatchMissionPlanner10()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("defaults.ini"));

    const PlannerStartupUdpOptions options =
        PlannerStartupUdpOptions::load(settings);
    QVERIFY(options.enabled);
    QCOMPARE(options.primaryPort, 14550);
    QCOMPARE(options.alternatePort, 14551);
    QCOMPARE(options.orderedPorts(), QList<int>({14550, 14551}));

    QCOMPARE(QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey),
             QStringLiteral("startup_udp_listeners_enabled"));
    QCOMPARE(QLatin1String(PlannerStartupUdpOptions::PrimaryPortSettingKey),
             QStringLiteral("startup_udp_primary_port"));
    QCOMPARE(QLatin1String(PlannerStartupUdpOptions::AlternatePortSettingKey),
             QStringLiteral("startup_udp_alternate_port"));
}

void PlannerStartupUdpOptionsTest::detectsOnlyExplicitStartupConfiguration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("explicit.ini"));
    QVERIFY(!PlannerStartupUdpOptions::hasExplicitConfiguration(settings));
    QVERIFY(!settings.fallbacksEnabled());

    settings.setValue(
        QLatin1String(PlannerStartupUdpOptions::PrimaryPortSettingKey), 16000);
    QVERIFY(PlannerStartupUdpOptions::hasExplicitConfiguration(settings));
}

void PlannerStartupUdpOptionsTest::invalidPortsUseFieldSpecificFallbacks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("ports.ini"));

    settings.setValue(
        QLatin1String(PlannerStartupUdpOptions::PrimaryPortSettingKey), 0);
    settings.setValue(
        QLatin1String(PlannerStartupUdpOptions::AlternatePortSettingKey),
        65536);
    PlannerStartupUdpOptions options = PlannerStartupUdpOptions::load(settings);
    QCOMPARE(options.primaryPort,
             PlannerStartupUdpOptions::DefaultPrimaryPort);
    QCOMPARE(options.alternatePort,
             PlannerStartupUdpOptions::DefaultAlternatePort);

    settings.setValue(
        QLatin1String(PlannerStartupUdpOptions::PrimaryPortSettingKey),
        QStringLiteral("not-a-port"));
    settings.setValue(
        QLatin1String(PlannerStartupUdpOptions::AlternatePortSettingKey), -1);
    options = PlannerStartupUdpOptions::load(settings);
    QCOMPARE(options.primaryPort,
             PlannerStartupUdpOptions::DefaultPrimaryPort);
    QCOMPARE(options.alternatePort,
             PlannerStartupUdpOptions::DefaultAlternatePort);

    QCOMPARE(PlannerStartupUdpOptions::normalizePort(1, 12), 1);
    QCOMPARE(PlannerStartupUdpOptions::normalizePort(65535, 12), 65535);
    QCOMPARE(PlannerStartupUdpOptions::normalizePort(0, 12), 12);
    QCOMPARE(PlannerStartupUdpOptions::normalizePort(65536, 12), 12);
}

void PlannerStartupUdpOptionsTest::enabledValueIsParsedDefensively()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = settingsFor(directory, QStringLiteral("enabled.ini"));
    const QString key =
        QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey);

    settings.setValue(key, false);
    QVERIFY(!PlannerStartupUdpOptions::load(settings).enabled);
    settings.setValue(key, QStringLiteral("TRUE"));
    QVERIFY(PlannerStartupUdpOptions::load(settings).enabled);
    settings.setValue(key, QStringLiteral("0"));
    QVERIFY(!PlannerStartupUdpOptions::load(settings).enabled);
    settings.setValue(key, QStringLiteral("corrupt"));
    QVERIFY(PlannerStartupUdpOptions::load(settings).enabled);
}

void PlannerStartupUdpOptionsTest::orderedPortsHonorEnabledStateAndDeduplicate()
{
    PlannerStartupUdpOptions options;
    options.primaryPort = 16000;
    options.alternatePort = 15000;
    QCOMPARE(options.orderedPorts(), QList<int>({16000, 15000}));

    options.alternatePort = 16000;
    QCOMPARE(options.orderedPorts(), QList<int>({16000}));

    options.enabled = false;
    QVERIFY(options.orderedPorts().isEmpty());

    options.enabled = true;
    options.primaryPort = 0;
    options.alternatePort = 0;
    QCOMPARE(options.orderedPorts(), QList<int>({14550, 14551}));
}

void PlannerStartupUdpOptionsTest::savePersistsNormalizedValues()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("saved.ini"));

    {
        QSettings settings(path, QSettings::IniFormat);
        PlannerStartupUdpOptions options;
        options.enabled = false;
        options.primaryPort = -1;
        options.alternatePort = 17000;
        options.save(settings);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }

    QSettings reloaded(path, QSettings::IniFormat);
    QVERIFY(reloaded.contains(
        QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey)));
    QVERIFY(reloaded.contains(
        QLatin1String(PlannerStartupUdpOptions::PrimaryPortSettingKey)));
    QVERIFY(reloaded.contains(
        QLatin1String(PlannerStartupUdpOptions::AlternatePortSettingKey)));

    PlannerStartupUdpOptions expected;
    expected.enabled = false;
    expected.primaryPort = PlannerStartupUdpOptions::DefaultPrimaryPort;
    expected.alternatePort = 17000;
    QVERIFY(PlannerStartupUdpOptions::load(reloaded) == expected);
}

void PlannerStartupUdpOptionsTest::statusAndRestartNoteAreDeterministic()
{
    PlannerStartupUdpOptions options;
    QCOMPARE(options.configurationStatus(), QStringLiteral(
        "Startup UDP listeners are configured for ports 14550 and 14551. "
        "Changes take effect after restart."));

    options.alternatePort = options.primaryPort;
    QCOMPARE(options.configurationStatus(), QStringLiteral(
        "Startup UDP listener is configured for port 14550. "
        "Changes take effect after restart."));

    options.enabled = false;
    QCOMPARE(options.configurationStatus(), QStringLiteral(
        "Automatic startup UDP listeners are disabled."));
    QCOMPARE(PlannerStartupUdpOptions::restartNote(), QStringLiteral(
        "Each port is a separate MAVLink connection. Changes take effect "
        "after restart; duplicate port values open one listener."));
}

QTEST_APPLESS_MAIN(PlannerStartupUdpOptionsTest)

#include "test_plannerstartupudpoptions.moc"
