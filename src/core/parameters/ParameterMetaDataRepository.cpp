#include "ParameterMetaDataRepository.h"

#include <QDir>
#include <QFile>
#include <QStringList>

ParameterMetaDataRepository::ParameterMetaDataRepository(
    const QString &packagedDirectory)
    : m_packagedDirectory(QDir::cleanPath(packagedDirectory))
{
}

ParameterMetaDataSource ParameterMetaDataRepository::sourceForFamily(
    ParameterFirmwareFamily family)
{
    switch (family) {
    case ParameterFirmwareFamily::ArduCopter:
        return {QStringLiteral("arducopter.pdef.xml"),
                QStringLiteral("ArduCopter")};
    case ParameterFirmwareFamily::ArduPlane:
        return {QStringLiteral("arduplane.pdef.xml"),
                QStringLiteral("ArduPlane")};
    case ParameterFirmwareFamily::Rover:
        // The remote directory is historically APMrover2, but the XML block
        // in current and packaged PDEF files is named Rover.
        return {QStringLiteral("ardurover.pdef.xml"),
                QStringLiteral("Rover")};
    case ParameterFirmwareFamily::ArduSub:
        return {QStringLiteral("ardusub.pdef.xml"),
                QStringLiteral("ArduSub")};
    case ParameterFirmwareFamily::AntennaTracker:
        return {QStringLiteral("antennatracker.pdef.xml"),
                QStringLiteral("AntennaTracker")};
    case ParameterFirmwareFamily::Unknown:
        break;
    }
    return {};
}

ParameterMetaDataCatalog ParameterMetaDataRepository::catalog(
    ParameterFirmwareFamily family)
{
    if (m_attempted.contains(family)) {
        return m_catalogs.value(family);
    }
    m_attempted.insert(family);

    const ParameterMetaDataSource source = sourceForFamily(family);
    if (!source.isValid()) {
        m_errors.insert(family, QStringLiteral("Unsupported firmware family"));
        return {};
    }

    const QStringList candidates = {
        QDir(m_packagedDirectory).filePath(source.fileName),
        QStringLiteral(":/metadata/ardupilotmega/") + source.fileName
    };
    QStringList errors;
    for (const QString &candidate : candidates) {
        QFile file(candidate);
        if (!file.open(QIODevice::ReadOnly)) {
            errors.append(QStringLiteral("%1: %2")
                              .arg(candidate, file.errorString()));
            continue;
        }
        const ParameterMetaDataCatalog loaded =
            ParameterMetaDataCatalog::fromPdef(&file, source.vehicleName);
        if (loaded.isValid()) {
            m_catalogs.insert(family, loaded);
            m_errors.remove(family);
            return loaded;
        }
        errors.append(QStringLiteral("%1: %2")
                          .arg(candidate, loaded.errorString()));
    }
    m_errors.insert(family, errors.join(QStringLiteral("; ")));
    return {};
}

QString ParameterMetaDataRepository::errorString(
    ParameterFirmwareFamily family) const
{
    return m_errors.value(family);
}

void ParameterMetaDataRepository::clear()
{
    m_catalogs.clear();
    m_errors.clear();
    m_attempted.clear();
}
