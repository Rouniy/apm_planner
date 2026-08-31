#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>

/**
 * Cross-platform locations used by the desktop application.
 *
 * Resource lookup is deliberately independent of the process working
 * directory. User data uses the platform conventions exposed by Qt, while an
 * existing APM Planner 2 home directory remains the preferred upgrade path.
 */
namespace AppPaths
{

QString resourceRoot();
QString resourcePath(const QString &relativePath);

QString writableDataDirectory();
QString legacyUserDataDirectory();

bool ensureDirectory(const QString &path);

} // namespace AppPaths

#endif // APPPATHS_H
