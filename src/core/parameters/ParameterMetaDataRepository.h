#ifndef PARAMETERMETADATAREPOSITORY_H
#define PARAMETERMETADATAREPOSITORY_H

#include "core/parameters/ParameterMetaData.h"

#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QString>

enum class ParameterFirmwareFamily
{
    Unknown,
    ArduCopter,
    ArduPlane,
    Rover,
    ArduSub,
    AntennaTracker
};

inline uint qHash(ParameterFirmwareFamily family, uint seed = 0)
{
    return ::qHash(static_cast<int>(family), seed);
}

struct ParameterMetaDataSource
{
    QString fileName;
    QString vehicleName;
    QString latestVehicleName;
    QString versionedVehicleName;

    bool isValid() const
    {
        return !fileName.isEmpty() && !vehicleName.isEmpty();
    }
};

struct ParameterMetaDataCacheInfo
{
    QString sourceUrl;
    QString etag;
    QString lastModified;
    QDateTime fetchedAtUtc;
};

/**
 * Mission Planner-compatible metadata facade.
 *
 * Provides an always-available packaged catalog and validated per-family
 * network caches. Cache XML remains usable when advisory provenance is lost;
 * only a fresh, hash-matched versioned cache is considered exact enough for
 * strict range enforcement.
 */
class ParameterMetaDataRepository
{
public:
    explicit ParameterMetaDataRepository(
        const QString &packagedDirectory,
        const QString &cacheDirectory = QString());

    static ParameterMetaDataSource sourceForFamily(
        ParameterFirmwareFamily family);

    ParameterMetaDataCatalog catalog(
        ParameterFirmwareFamily family,
        const QString &firmwareVersion = QString());
    QString errorString(
        ParameterFirmwareFamily family,
        const QString &firmwareVersion = QString()) const;
    bool installCatalog(ParameterFirmwareFamily family,
                        const QString &firmwareVersion,
                        const QByteArray &xml,
                        const ParameterMetaDataCacheInfo &cacheInfo,
                        QString *error = nullptr);
    bool cachedCatalogIsFresh(ParameterFirmwareFamily family,
                              const QString &firmwareVersion,
                              qint64 maximumAgeSeconds) const;
    bool catalogMatchesFirmwareVersion(
        ParameterFirmwareFamily family,
        const QString &firmwareVersion) const;
    QString cacheFilePath(ParameterFirmwareFamily family,
                          const QString &firmwareVersion) const;
    static QString normalizedVersion(const QString &firmwareVersion);
    void clear(ParameterFirmwareFamily family);
    void clear();
    // GUI-thread publication notice. All repositories using this directory
    // reload lazily; previously returned page snapshots remain untouched.
    static void invalidateSharedCache(const QString &cacheDirectory);

private:
    ParameterMetaDataCatalog loadCatalogFile(
        const QString &path, const ParameterMetaDataSource &source,
        bool requireSubstantialCatalog, QString *error,
        QByteArray *catalogBytes = nullptr) const;
    QString catalogKey(ParameterFirmwareFamily family,
                       const QString &firmwareVersion) const;
    QString cacheInfoPath(const QString &catalogPath) const;
    bool cacheProvenanceIsValid(ParameterFirmwareFamily family,
                                const QString &firmwareVersion,
                                QDateTime *fetchedAtUtc = nullptr,
                                const QByteArray *catalogBytes = nullptr) const;

    QString m_packagedDirectory;
    QString m_cacheDirectory;
    QMap<QString, ParameterMetaDataCatalog> m_catalogs;
    QMap<QString, QString> m_errors;
    QSet<QString> m_attempted;
    QSet<QString> m_versionMatchedCatalogs;
    quint64 m_cacheRevision = 0;
};

#endif
