#ifndef NATIVEGDALLIBRARY_H
#define NATIVEGDALLIBRARY_H

#include <QStringList>

// One cross-platform discovery policy for raster, vector and CRS consumers.
inline QStringList nativeGdalLibraryCandidates()
{
    QStringList candidates;
    const QString configured = QString::fromLocal8Bit(
        qgetenv("MISSIONPLANNER_GDAL_LIBRARY")).trimmed();
    if (!configured.isEmpty()) candidates.append(configured);
#ifdef Q_OS_WIN
    candidates.append({QStringLiteral("gdal.dll"), QStringLiteral("gdal313.dll"),
        QStringLiteral("gdal312.dll"), QStringLiteral("gdal311.dll"),
        QStringLiteral("gdal310.dll"), QStringLiteral("gdal309.dll"),
        QStringLiteral("gdal308.dll"), QStringLiteral("gdal307.dll"),
        QStringLiteral("gdal306.dll"), QStringLiteral("gdal305.dll")});
#elif defined(Q_OS_MACOS)
    candidates.append({QStringLiteral("libgdal.dylib"),
        QStringLiteral("/opt/homebrew/lib/libgdal.dylib"),
        QStringLiteral("/usr/local/lib/libgdal.dylib")});
#else
    candidates.append(QStringLiteral("libgdal.so"));
    for (int abi = 40; abi >= 30; --abi)
        candidates.append(QStringLiteral("libgdal.so.%1").arg(abi));
#endif
    candidates.removeDuplicates();
    return candidates;
}

inline QStringList nativeProjLibraryCandidates()
{
    QStringList candidates;
    const QString configured = QString::fromLocal8Bit(
        qgetenv("MISSIONPLANNER_PROJ_LIBRARY")).trimmed();
    if (!configured.isEmpty()) candidates.append(configured);
#ifdef Q_OS_WIN
    candidates.append(QStringLiteral("proj.dll"));
    for (int major = 9; major >= 8; --major)
        for (int minor = 9; minor >= 0; --minor)
            candidates.append(QStringLiteral("proj_%1_%2.dll").arg(major).arg(minor));
#elif defined(Q_OS_MACOS)
    candidates.append({QStringLiteral("libproj.dylib"),
        QStringLiteral("/opt/homebrew/lib/libproj.dylib"),
        QStringLiteral("/usr/local/lib/libproj.dylib")});
#else
    candidates.append(QStringLiteral("libproj.so"));
    for (int abi = 30; abi >= 20; --abi)
        candidates.append(QStringLiteral("libproj.so.%1").arg(abi));
#endif
    candidates.removeDuplicates();
    return candidates;
}
#endif
