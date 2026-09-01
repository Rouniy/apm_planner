#ifndef PARAMETERMETADATAREPOSITORY_H
#define PARAMETERMETADATAREPOSITORY_H

#include "core/parameters/ParameterMetaData.h"

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

    bool isValid() const
    {
        return !fileName.isEmpty() && !vehicleName.isEmpty();
    }
};

/**
 * Mission Planner-compatible metadata facade.
 *
 * The initial implementation deliberately guarantees a packaged, offline
 * catalog. A network/cache provider can later update the same per-family
 * snapshots without coupling configuration widgets to download state.
 */
class ParameterMetaDataRepository
{
public:
    explicit ParameterMetaDataRepository(const QString &packagedDirectory);

    static ParameterMetaDataSource sourceForFamily(
        ParameterFirmwareFamily family);

    ParameterMetaDataCatalog catalog(ParameterFirmwareFamily family);
    QString errorString(ParameterFirmwareFamily family) const;
    void clear();

private:
    QString m_packagedDirectory;
    QMap<ParameterFirmwareFamily, ParameterMetaDataCatalog> m_catalogs;
    QMap<ParameterFirmwareFamily, QString> m_errors;
    QSet<ParameterFirmwareFamily> m_attempted;
};

#endif
