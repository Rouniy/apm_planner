#include <QtTest>

#include "services/StatusMessageSettings.h"
#include "services/StatusTextPolicy.h"
#include "uas/APMFirmwareVersion.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

class StatusMessageSettingsTest final : public QObject
{
    Q_OBJECT

private slots:
    void constructionUsesWarningWithoutWritingAKey();
    void persistsAndReloadsOnlyValidSeverity();
    void exposesTheExactMavlinkOrder();
    void appliesThresholdAndPrefixPolicy();
    void normalizesLegacyArduPilotSeverity();
};

void StatusMessageSettingsTest::constructionUsesWarningWithoutWritingAKey()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("status.ini")),
                    QSettings::IniFormat);
    store.setValue(QStringLiteral("sentinel"), 1);
    const QStringList before = store.allKeys();

    StatusMessageSettings settings(&store);

    QCOMPARE(settings.severity(), 4);
    QCOMPARE(store.allKeys(), before);
    QVERIFY(!store.contains(StatusMessageSettings::settingsKey()));
}

void StatusMessageSettingsTest::persistsAndReloadsOnlyValidSeverity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("status.ini")),
                    QSettings::IniFormat);
    StatusMessageSettings settings(&store);
    QSignalSpy changed(&settings,
                       &StatusMessageSettings::severityChanged);

    QVERIFY(settings.setSeverity(7));
    QCOMPARE(store.value(QStringLiteral("severity")).toInt(), 7);
    QCOMPARE(settings.severity(), 7);
    QCOMPARE(changed.count(), 1);
    QVERIFY(!settings.setSeverity(-1));
    QVERIFY(!settings.setSeverity(8));
    QCOMPARE(settings.severity(), 7);
    QCOMPARE(changed.count(), 1);

    store.setValue(QStringLiteral("severity"), 2);
    settings.reload();
    QCOMPARE(settings.severity(), 2);
    QCOMPARE(changed.count(), 2);
    store.setValue(QStringLiteral("severity"), 99);
    settings.reload();
    QCOMPARE(settings.severity(), 4);
    QCOMPARE(changed.count(), 3);
}

void StatusMessageSettingsTest::exposesTheExactMavlinkOrder()
{
    QCOMPARE(StatusMessageSettings::severityNames(),
             QStringList({QStringLiteral("Emergency"),
                          QStringLiteral("Alert"),
                          QStringLiteral("Critical"),
                          QStringLiteral("Error"),
                          QStringLiteral("Warning"),
                          QStringLiteral("Notice"),
                          QStringLiteral("Info"),
                          QStringLiteral("Debug")}));
}

void StatusMessageSettingsTest::appliesThresholdAndPrefixPolicy()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("status.ini")),
                    QSettings::IniFormat);
    StatusMessageSettings settings(&store);

    for (int severity = 0; severity <= 7; ++severity) {
        QCOMPARE(settings.shouldPromote(QStringLiteral("message"), severity),
                 severity <= 4);
    }
    QVERIFY(settings.shouldPromote(QStringLiteral("Tuning: pitch"), 7));
    QVERIFY(settings.shouldPromote(QStringLiteral("PreArm: compass"), 7));
    QVERIFY(settings.shouldPromote(QStringLiteral("Arm: denied"), 7));
    QVERIFY(!settings.shouldPromote(QString(), 0));
    QVERIFY(!settings.shouldPromote(QStringLiteral("message"), 255));

    QCOMPARE(StatusTextPolicy::speechText(
                 QStringLiteral("Tuning: pitch"), true),
             QStringLiteral("Tuning: pitch"));
    QCOMPARE(StatusTextPolicy::speechText(
                 QStringLiteral("PreArm: compass"), true),
             QStringLiteral("Pre-arm check: compass"));
    QCOMPARE(StatusTextPolicy::speechText(
                 QStringLiteral("Arm: denied"), true),
             QStringLiteral("Arm check: denied"));
    QCOMPARE(StatusTextPolicy::speechText(
                 QStringLiteral("#audio: hello"), false),
             QStringLiteral("hello"));
    QVERIFY(StatusTextPolicy::speechText(
                QStringLiteral("PX4v2 board"), true).isEmpty());
    QVERIFY(StatusTextPolicy::speechText(
                QStringLiteral("ordinary"), false).isEmpty());
}

void StatusMessageSettingsTest::normalizesLegacyArduPilotSeverity()
{
    QCOMPARE(StatusTextPolicy::normalizeLegacySeverity(1), 4);
    QCOMPARE(StatusTextPolicy::normalizeLegacySeverity(2), 1);
    QCOMPARE(StatusTextPolicy::normalizeLegacySeverity(3), 2);
    QCOMPARE(StatusTextPolicy::normalizeLegacySeverity(5), 2);
    QCOMPARE(StatusTextPolicy::normalizeLegacySeverity(0), 6);

    QVERIFY(StatusTextPolicy::requiresLegacySeverityCompatibility(
        APMFirmwareVersion(QStringLiteral("ArduCopter V3.3.9"))));
    QVERIFY(!StatusTextPolicy::requiresLegacySeverityCompatibility(
        APMFirmwareVersion(QStringLiteral("ArduCopter V3.4.0"))));
    QVERIFY(StatusTextPolicy::requiresLegacySeverityCompatibility(
        APMFirmwareVersion(QStringLiteral("APM:Rover V2.5.9"))));
    QVERIFY(!StatusTextPolicy::requiresLegacySeverityCompatibility(
        APMFirmwareVersion(QStringLiteral("APM:Rover V2.6.0"))));
}

QTEST_APPLESS_MAIN(StatusMessageSettingsTest)
#include "test_statusmessagesettings.moc"
