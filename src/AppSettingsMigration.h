#ifndef APPSETTINGSMIGRATION_H
#define APPSETTINGSMIGRATION_H

#include <QSettings>
#include <QString>
#include <QStringList>

namespace AppSettingsMigration
{

inline QString migrationMarkerKey()
{
    return QStringLiteral("MIGRATION/APM_PLANNER_2_SETTINGS_IMPORTED");
}

/**
 * Copy keys which do not yet exist in the destination settings.
 *
 * This deliberately preserves destination values.  It is kept independent of
 * the platform-specific QSettings storage so it can be exercised with temporary
 * INI files in a unit test.
 */
inline int copyMissingKeys(const QSettings &source, QSettings &destination)
{
    int copiedKeyCount = 0;
    const QStringList keys = source.allKeys();

    for (const QString &key : keys) {
        if (key == migrationMarkerKey() || destination.contains(key)) {
            continue;
        }

        destination.setValue(key, source.value(key));
        ++copiedKeyCount;
    }

    return copiedKeyCount;
}

/**
 * Perform the non-destructive, one-time copy between two settings stores.
 * Returns the number of legacy keys copied during this invocation.
 */
inline int migrateOnce(QSettings &legacySettings, QSettings &currentSettings)
{
    if (currentSettings.value(migrationMarkerKey(), false).toBool()) {
        return 0;
    }

    const int copiedKeyCount = copyMissingKeys(legacySettings, currentSettings);
    currentSettings.setValue(migrationMarkerKey(), true);
    currentSettings.sync();
    return copiedKeyCount;
}

/**
 * Migrate the legacy user profile into the current application namespace.
 * Explicit formats and scopes make this equivalent to the old application's
 * default QSettings store on every supported desktop platform.
 */
inline int migrateLegacyUserSettings(const QString &organizationName,
                                     const QString &legacyApplicationName,
                                     const QString &currentApplicationName)
{
    QSettings legacySettings(QSettings::IniFormat,
                             QSettings::UserScope,
                             organizationName,
                             legacyApplicationName);
    QSettings currentSettings(QSettings::IniFormat,
                              QSettings::UserScope,
                              organizationName,
                              currentApplicationName);
    // Import only the two exact per-user application profiles. QSettings
    // fallbacks may otherwise expose organization-wide or system-wide keys as
    // if they belonged to the legacy application.
    legacySettings.setFallbacksEnabled(false);
    currentSettings.setFallbacksEnabled(false);
    return migrateOnce(legacySettings, currentSettings);
}

} // namespace AppSettingsMigration

#endif // APPSETTINGSMIGRATION_H
