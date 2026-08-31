#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

namespace
{

QString cleanedAbsolutePath(const QString &path)
{
    if (path.trimmed().isEmpty()) {
        return QString();
    }

    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isResourceRoot(const QString &path)
{
    const QDir root(path);
    return root.exists(QStringLiteral("data"))
            && root.exists(QStringLiteral("files"))
            && root.exists(QStringLiteral("qml"));
}

void appendCandidate(QStringList &candidates, const QString &path)
{
    const QString candidate = cleanedAbsolutePath(path);
    if (!candidate.isEmpty() && !candidates.contains(candidate)) {
        candidates.append(candidate);
    }
}

QString locateResourceRoot()
{
    QStringList candidates;

    // This override is useful for portable deployments and automated tests.
    appendCandidate(candidates, qEnvironmentVariable("APM_PLANNER_DATA_DIR"));

    const QString applicationDir = QCoreApplication::applicationDirPath();
    appendCandidate(candidates, applicationDir);

#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    // A conventional macOS bundle keeps non-executable assets in Resources.
    appendCandidate(candidates,
                    QDir(applicationDir).absoluteFilePath(
                        QStringLiteral("../Resources/APMPlanner3")));
    appendCandidate(candidates,
                    QDir(applicationDir).absoluteFilePath(
                        QStringLiteral("../Resources")));
#endif

    // Prefer a location relative to the executable so a staged installation
    // remains relocatable even when CMAKE_INSTALL_PREFIX changes afterwards.
    appendCandidate(candidates,
                    QDir(applicationDir).absoluteFilePath(
                        QStringLiteral("../share/APMPlanner3")));
    appendCandidate(candidates,
                    QDir(applicationDir).absoluteFilePath(
                        QStringLiteral("../share/APMPlanner2")));

#ifdef APM_INSTALL_DATA_DIR
    appendCandidate(candidates, QString::fromUtf8(APM_INSTALL_DATA_DIR));
#endif
#ifdef APM_LEGACY_INSTALL_DATA_DIR
    appendCandidate(candidates, QString::fromUtf8(APM_LEGACY_INSTALL_DATA_DIR));
#endif
#ifdef APM_DEVELOPMENT_DATA_DIR
    appendCandidate(candidates, QString::fromUtf8(APM_DEVELOPMENT_DATA_DIR));
#endif

    for (const QString &candidate : candidates) {
        if (isResourceRoot(candidate)) {
            return candidate;
        }
    }

    // Keep the return value deterministic. Callers can report a missing file,
    // but never accidentally load resources from an arbitrary working folder.
    return cleanedAbsolutePath(applicationDir);
}

} // namespace

namespace AppPaths
{

QString resourceRoot()
{
    static const QString root = locateResourceRoot();
    return root;
}

QString resourcePath(const QString &relativePath)
{
    QString cleanRelativePath = QDir::cleanPath(relativePath);
    while (cleanRelativePath.startsWith(QLatin1Char('/'))
           || cleanRelativePath.startsWith(QLatin1Char('\\'))) {
        cleanRelativePath.remove(0, 1);
    }
    return QDir(resourceRoot()).absoluteFilePath(cleanRelativePath);
}

QString legacyUserDataDirectory()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return QDir(home).absoluteFilePath(QStringLiteral("apmplanner2"));
}

QString writableDataDirectory()
{
    const QString overridePath = qEnvironmentVariable("APM_PLANNER_HOME");
    if (!overridePath.trimmed().isEmpty()) {
        return cleanedAbsolutePath(overridePath);
    }

    // Do not strand logs, missions and parameters from an older installation.
    const QString legacyPath = legacyUserDataDirectory();
    if (QDir(legacyPath).exists()) {
        return legacyPath;
    }

    // Logs and downloaded vehicle data are machine-local; on Windows this
    // intentionally selects LocalAppData instead of the roaming profile.
    QString standardPath = QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation);
    if (standardPath.isEmpty()) {
        standardPath = legacyPath;
    }
    return cleanedAbsolutePath(standardPath);
}

bool ensureDirectory(const QString &path)
{
    if (path.trimmed().isEmpty()) {
        return false;
    }

    QDir directory;
    return directory.mkpath(QDir::cleanPath(path));
}

} // namespace AppPaths
