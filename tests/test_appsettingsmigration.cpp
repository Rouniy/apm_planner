#include <QtTest>

#include "AppSettingsMigration.h"

#include <QSettings>
#include <QTemporaryDir>

class AppSettingsMigrationTest final : public QObject
{
    Q_OBJECT

private slots:
    void copiesOnlyMissingKeysOnce();
};

void AppSettingsMigrationTest::copiesOnlyMissingKeysOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QSettings legacy(directory.filePath(QStringLiteral("legacy.ini")), QSettings::IniFormat);
    QSettings current(directory.filePath(QStringLiteral("current.ini")), QSettings::IniFormat);
    legacy.setValue(QStringLiteral("GLOBAL_SETTINGS/LOG_DIRECTORY"), QStringLiteral("/legacy/logs"));
    legacy.setValue(QStringLiteral("THEME"), QStringLiteral("legacy-theme"));
    current.setValue(QStringLiteral("THEME"), QStringLiteral("3.0-theme"));

    QCOMPARE(AppSettingsMigration::migrateOnce(legacy, current), 1);
    QCOMPARE(current.value(QStringLiteral("GLOBAL_SETTINGS/LOG_DIRECTORY")).toString(),
             QStringLiteral("/legacy/logs"));
    QCOMPARE(current.value(QStringLiteral("THEME")).toString(),
             QStringLiteral("3.0-theme"));
    QVERIFY(current.value(AppSettingsMigration::migrationMarkerKey()).toBool());

    legacy.setValue(QStringLiteral("LATE_LEGACY_KEY"), 42);
    QCOMPARE(AppSettingsMigration::migrateOnce(legacy, current), 0);
    QVERIFY(!current.contains(QStringLiteral("LATE_LEGACY_KEY")));
}

QTEST_APPLESS_MAIN(AppSettingsMigrationTest)

#include "test_appsettingsmigration.moc"
