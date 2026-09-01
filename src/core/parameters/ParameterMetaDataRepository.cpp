#include "ParameterMetaDataRepository.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QUrl>

namespace {
constexpr qint64 kMaximumCatalogBytes = 16 * 1024 * 1024;
constexpr qint64 kMaximumProvenanceBytes = 64 * 1024;
constexpr qint64 kTrustedCatalogMaximumAgeSeconds = 7 * 24 * 60 * 60;

struct CatalogCandidate
{
    QString path;
    bool requireSubstantialCatalog = false;
    QString exactFirmwareVersion;
};

bool isFresh(const QDateTime &fetchedAtUtc, qint64 maximumAgeSeconds)
{
    if (!fetchedAtUtc.isValid() || maximumAgeSeconds < 0) {
        return false;
    }
    const qint64 age = fetchedAtUtc.secsTo(QDateTime::currentDateTimeUtc());
    return age >= 0 && age <= maximumAgeSeconds;
}

QJsonObject readJsonObject(const QString &path, bool *valid)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *valid = false;
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        file.read(kMaximumProvenanceBytes + 1), &parseError);
    *valid = parseError.error == QJsonParseError::NoError
        && document.isObject() && file.atEnd();
    return *valid ? document.object() : QJsonObject();
}
}

ParameterMetaDataRepository::ParameterMetaDataRepository(
    const QString &packagedDirectory, const QString &cacheDirectory)
    : m_packagedDirectory(QDir::cleanPath(packagedDirectory)),
      m_cacheDirectory(cacheDirectory.trimmed().isEmpty()
          ? QString() : QDir::cleanPath(cacheDirectory))
{
}

ParameterMetaDataSource ParameterMetaDataRepository::sourceForFamily(
    ParameterFirmwareFamily family)
{
    switch (family) {
    case ParameterFirmwareFamily::ArduCopter:
        return {QStringLiteral("arducopter.pdef.xml"),
                QStringLiteral("ArduCopter"),
                QStringLiteral("ArduCopter"),
                QStringLiteral("Copter")};
    case ParameterFirmwareFamily::ArduPlane:
        return {QStringLiteral("arduplane.pdef.xml"),
                QStringLiteral("ArduPlane"),
                QStringLiteral("ArduPlane"),
                QStringLiteral("Plane")};
    case ParameterFirmwareFamily::Rover:
        return {QStringLiteral("ardurover.pdef.xml"),
                QStringLiteral("Rover"),
                QStringLiteral("Rover"),
                QStringLiteral("Rover")};
    case ParameterFirmwareFamily::ArduSub:
        return {QStringLiteral("ardusub.pdef.xml"),
                QStringLiteral("ArduSub"),
                QStringLiteral("ArduSub"),
                QStringLiteral("Sub")};
    case ParameterFirmwareFamily::AntennaTracker:
        return {QStringLiteral("antennatracker.pdef.xml"),
                QStringLiteral("AntennaTracker"),
                QStringLiteral("AntennaTracker"),
                QStringLiteral("Tracker")};
    case ParameterFirmwareFamily::Unknown:
        break;
    }
    return {};
}

ParameterMetaDataCatalog ParameterMetaDataRepository::catalog(
    ParameterFirmwareFamily family, const QString &firmwareVersion)
{
    const QString key = catalogKey(family, firmwareVersion);
    if (m_attempted.contains(key)) {
        return m_catalogs.value(key);
    }
    m_attempted.insert(key);

    const ParameterMetaDataSource source = sourceForFamily(family);
    if (!source.isValid()) {
        m_errors.insert(key, QStringLiteral("Unsupported firmware family"));
        return {};
    }

    QList<CatalogCandidate> candidates;
    const QString normalized = normalizedVersion(firmwareVersion);
    if (!m_cacheDirectory.isEmpty()) {
        if (!normalized.isEmpty()) {
            candidates.append({cacheFilePath(family, normalized), true,
                               normalized});
        }
        const QString latestPath = cacheFilePath(family, QString());
        if (candidates.isEmpty() || candidates.last().path != latestPath) {
            candidates.append({latestPath, true, QString()});
        }
    }
    candidates.append({QDir(m_packagedDirectory).filePath(source.fileName),
                       false, QString()});
    candidates.append({QStringLiteral(":/metadata/ardupilotmega/")
                           + source.fileName,
                       false, QString()});

    QStringList errors;
    for (const auto &candidate : candidates) {
        QString loadError;
        QByteArray catalogBytes;
        const ParameterMetaDataCatalog loaded = loadCatalogFile(
            candidate.path, source, candidate.requireSubstantialCatalog,
            &loadError, &catalogBytes);
        if (loaded.isValid()) {
            m_catalogs.insert(key, loaded);
            QDateTime fetchedAtUtc;
            const bool matchesFirmwareVersion =
                !candidate.exactFirmwareVersion.isEmpty()
                && cacheProvenanceIsValid(
                    family, candidate.exactFirmwareVersion,
                    &fetchedAtUtc, &catalogBytes)
                && isFresh(fetchedAtUtc,
                           kTrustedCatalogMaximumAgeSeconds);
            if (matchesFirmwareVersion) {
                m_versionMatchedCatalogs.insert(key);
            } else {
                m_versionMatchedCatalogs.remove(key);
            }
            m_errors.remove(key);
            return loaded;
        }
        errors.append(QStringLiteral("%1: %2")
                          .arg(candidate.path, loadError));
    }
    m_errors.insert(key, errors.join(QStringLiteral("; ")));
    return {};
}

QString ParameterMetaDataRepository::errorString(
    ParameterFirmwareFamily family, const QString &firmwareVersion) const
{
    return m_errors.value(catalogKey(family, firmwareVersion));
}

bool ParameterMetaDataRepository::installCatalog(
    ParameterFirmwareFamily family, const QString &firmwareVersion,
    const QByteArray &xml, const ParameterMetaDataCacheInfo &cacheInfo,
    QString *error)
{
    const ParameterMetaDataSource source = sourceForFamily(family);
    const QString normalized = normalizedVersion(firmwareVersion);
    if (!source.isValid() || m_cacheDirectory.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Metadata cache is unavailable");
        }
        return false;
    }
    if (!firmwareVersion.trimmed().isEmpty() && normalized.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Invalid firmware version");
        }
        return false;
    }
    if (xml.isEmpty() || xml.size() > kMaximumCatalogBytes) {
        if (error) {
            *error = QStringLiteral("Metadata payload size is invalid");
        }
        return false;
    }

    QBuffer buffer;
    buffer.setData(xml);
    buffer.open(QIODevice::ReadOnly);
    const ParameterMetaDataCatalog parsed =
        ParameterMetaDataCatalog::fromPdef(&buffer, source.vehicleName);
    if (!parsed.isValid() || parsed.entries().size() <= 100) {
        if (error) {
            *error = parsed.isValid()
                ? QStringLiteral("Metadata catalog is unexpectedly small")
                : parsed.errorString();
        }
        return false;
    }
    const QString path = cacheFilePath(family, normalized);
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("Cannot create metadata cache directory");
        }
        return false;
    }

    const QByteArray digest = QCryptographicHash::hash(
        xml, QCryptographicHash::Sha256).toHex();
    QJsonObject provenance;
    provenance.insert(QStringLiteral("format"), 1);
    provenance.insert(QStringLiteral("family"), source.vehicleName);
    provenance.insert(QStringLiteral("firmwareVersion"),
                      normalized.isEmpty() ? QStringLiteral("latest")
                                           : normalized);
    provenance.insert(QStringLiteral("sourceUrl"), cacheInfo.sourceUrl);
    provenance.insert(QStringLiteral("etag"), cacheInfo.etag);
    provenance.insert(QStringLiteral("lastModified"),
                      cacheInfo.lastModified);
    provenance.insert(QStringLiteral("fetchedAtUtc"),
                      (cacheInfo.fetchedAtUtc.isValid()
                           ? cacheInfo.fetchedAtUtc.toUTC()
                           : QDateTime::currentDateTimeUtc())
                          .toString(Qt::ISODate));
    provenance.insert(QStringLiteral("sha256"),
                      QString::fromLatin1(digest));

    QSaveFile catalogFile(path);
    if (!catalogFile.open(QIODevice::WriteOnly)
        || catalogFile.write(xml) != xml.size()
        || !catalogFile.commit()) {
        if (error) {
            *error = catalogFile.errorString();
        }
        return false;
    }

    // The XML is authoritative and is already atomically published. The
    // provenance sidecar is deliberately best-effort: two independent files
    // cannot form one atomic transaction on all supported platforms.
    QSaveFile provenanceFile(cacheInfoPath(path));
    const QByteArray provenanceBytes =
        QJsonDocument(provenance).toJson(QJsonDocument::Indented);
    if (provenanceFile.open(QIODevice::WriteOnly)
        && provenanceFile.write(provenanceBytes) == provenanceBytes.size()) {
        provenanceFile.commit();
    }

    clear(family);
    return true;
}

bool ParameterMetaDataRepository::cachedCatalogIsFresh(
    ParameterFirmwareFamily family, const QString &firmwareVersion,
    qint64 maximumAgeSeconds) const
{
    const ParameterMetaDataSource source = sourceForFamily(family);
    const QString normalized = normalizedVersion(firmwareVersion);
    if (!source.isValid()
        || (!firmwareVersion.trimmed().isEmpty() && normalized.isEmpty())) {
        return false;
    }
    if (maximumAgeSeconds < 0) {
        return false;
    }
    QString ignored;
    QByteArray catalogBytes;
    const ParameterMetaDataCatalog catalog = loadCatalogFile(
        cacheFilePath(family, normalized), source, true, &ignored,
        &catalogBytes);
    if (!catalog.isValid()) {
        return false;
    }
    QDateTime fetchedAt;
    if (!cacheProvenanceIsValid(
            family, normalized, &fetchedAt, &catalogBytes)) {
        return false;
    }
    return isFresh(fetchedAt, maximumAgeSeconds);
}

bool ParameterMetaDataRepository::catalogMatchesFirmwareVersion(
    ParameterFirmwareFamily family, const QString &firmwareVersion) const
{
    return !normalizedVersion(firmwareVersion).isEmpty()
        && m_versionMatchedCatalogs.contains(
            catalogKey(family, firmwareVersion));
}

QString ParameterMetaDataRepository::cacheFilePath(
    ParameterFirmwareFamily family, const QString &firmwareVersion) const
{
    const ParameterMetaDataSource source = sourceForFamily(family);
    if (m_cacheDirectory.isEmpty() || source.latestVehicleName.isEmpty()) {
        return {};
    }
    const QString normalized = normalizedVersion(firmwareVersion);
    if (!firmwareVersion.trimmed().isEmpty() && normalized.isEmpty()) {
        return {};
    }
    const QString suffix = normalized.isEmpty()
        ? QStringLiteral("latest") : normalized;
    return QDir(m_cacheDirectory).filePath(
        source.latestVehicleName + QLatin1Char('-') + suffix
        + QStringLiteral(".apm.pdef.xml"));
}

QString ParameterMetaDataRepository::normalizedVersion(
    const QString &firmwareVersion)
{
    static const QRegularExpression expression(
        QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)$"));
    const QRegularExpressionMatch match = expression.match(
        firmwareVersion.trimmed());
    if (!match.hasMatch()) {
        return {};
    }
    bool majorOk = false;
    bool minorOk = false;
    bool patchOk = false;
    const uint major = match.captured(1).toUInt(&majorOk);
    const uint minor = match.captured(2).toUInt(&minorOk);
    const uint patch = match.captured(3).toUInt(&patchOk);
    if (!majorOk || !minorOk || !patchOk
        || major > 255 || minor > 255 || patch > 255) {
        return {};
    }
    return QStringLiteral("%1.%2.%3")
        .arg(major).arg(minor).arg(patch);
}

ParameterMetaDataCatalog ParameterMetaDataRepository::loadCatalogFile(
    const QString &path, const ParameterMetaDataSource &source,
    bool requireSubstantialCatalog, QString *error,
    QByteArray *catalogBytes) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return {};
    }
    const QByteArray xml = file.read(kMaximumCatalogBytes + 1);
    if (xml.isEmpty() || xml.size() > kMaximumCatalogBytes) {
        if (error) {
            *error = QStringLiteral("Metadata payload size is invalid");
        }
        return {};
    }
    if (catalogBytes) {
        *catalogBytes = xml;
    }

    QBuffer buffer;
    buffer.setData(xml);
    buffer.open(QIODevice::ReadOnly);
    const ParameterMetaDataCatalog loaded =
        ParameterMetaDataCatalog::fromPdef(&buffer, source.vehicleName);
    if (error) {
        if (!loaded.isValid()) {
            *error = loaded.errorString();
        } else if (requireSubstantialCatalog
                   && loaded.entries().size() <= 100) {
            *error = QStringLiteral("Metadata catalog is unexpectedly small");
        }
    }
    if (requireSubstantialCatalog && loaded.entries().size() <= 100) {
        return {};
    }
    return loaded;
}

QString ParameterMetaDataRepository::catalogKey(
    ParameterFirmwareFamily family, const QString &firmwareVersion) const
{
    const QString requested = firmwareVersion.trimmed();
    const QString normalized = normalizedVersion(requested);
    if (!requested.isEmpty() && normalized.isEmpty()) {
        return QString::number(static_cast<int>(family))
            + QStringLiteral("|invalid:") + requested;
    }
    return QString::number(static_cast<int>(family)) + QLatin1Char('|')
        + (normalized.isEmpty() ? QStringLiteral("latest") : normalized);
}

QString ParameterMetaDataRepository::cacheInfoPath(
    const QString &catalogPath) const
{
    return catalogPath + QStringLiteral(".json");
}

bool ParameterMetaDataRepository::cacheProvenanceIsValid(
    ParameterFirmwareFamily family, const QString &firmwareVersion,
    QDateTime *fetchedAtUtc, const QByteArray *catalogBytes) const
{
    const ParameterMetaDataSource source = sourceForFamily(family);
    const QString normalized = normalizedVersion(firmwareVersion);
    if (!source.isValid() || m_cacheDirectory.isEmpty()
        || (!firmwareVersion.trimmed().isEmpty() && normalized.isEmpty())) {
        return false;
    }
    const QString path = cacheFilePath(family, normalized);
    QByteArray loadedCatalogBytes;
    if (!catalogBytes) {
        QFile catalogFile(path);
        if (!catalogFile.open(QIODevice::ReadOnly)) {
            return false;
        }
        loadedCatalogBytes = catalogFile.read(kMaximumCatalogBytes + 1);
        catalogBytes = &loadedCatalogBytes;
    }
    if (catalogBytes->isEmpty()
        || catalogBytes->size() > kMaximumCatalogBytes) {
        return false;
    }

    bool provenanceValid = false;
    const QJsonObject provenance = readJsonObject(
        cacheInfoPath(path), &provenanceValid);
    if (!provenanceValid) {
        return false;
    }
    const QString expectedVersion = normalized.isEmpty()
        ? QStringLiteral("latest") : normalized;
    const QDateTime recorded = QDateTime::fromString(
        provenance.value(QStringLiteral("fetchedAtUtc")).toString(),
        Qt::ISODate);
    const QUrl sourceUrl(provenance.value(
        QStringLiteral("sourceUrl")).toString());
    const QString expectedPath = normalized.isEmpty()
        ? QStringLiteral("/Parameters/%1/apm.pdef.xml")
              .arg(source.latestVehicleName)
        : QStringLiteral("/Parameters/versioned/%1/stable-%2/apm.pdef.xml")
              .arg(source.versionedVehicleName, normalized);
    const QByteArray actualDigest = QCryptographicHash::hash(
        *catalogBytes, QCryptographicHash::Sha256).toHex();
    provenanceValid = provenance.value(QStringLiteral("format")).toInt() == 1
        && provenance.value(QStringLiteral("family")).toString()
            == source.vehicleName
        && provenance.value(QStringLiteral("firmwareVersion")).toString()
            == expectedVersion
        && provenance.value(QStringLiteral("sha256")).toString().toLatin1()
            == actualDigest
        && sourceUrl.scheme() == QLatin1String("https")
        && sourceUrl.host().compare(QStringLiteral("autotest.ardupilot.org"),
                                    Qt::CaseInsensitive) == 0
        && sourceUrl.path() == expectedPath
        && recorded.isValid();
    if (provenanceValid && fetchedAtUtc) {
        *fetchedAtUtc = recorded.toUTC();
    }
    return provenanceValid;
}

void ParameterMetaDataRepository::clear(ParameterFirmwareFamily family)
{
    const QString prefix = QString::number(static_cast<int>(family))
        + QLatin1Char('|');
    for (const QString &key : m_catalogs.keys()) {
        if (key.startsWith(prefix)) {
            m_catalogs.remove(key);
        }
    }
    for (const QString &key : m_errors.keys()) {
        if (key.startsWith(prefix)) {
            m_errors.remove(key);
        }
    }
    for (const QString &key : m_attempted.values()) {
        if (key.startsWith(prefix)) {
            m_attempted.remove(key);
        }
    }
    for (const QString &key : m_versionMatchedCatalogs.values()) {
        if (key.startsWith(prefix)) {
            m_versionMatchedCatalogs.remove(key);
        }
    }
}

void ParameterMetaDataRepository::clear()
{
    m_catalogs.clear();
    m_errors.clear();
    m_attempted.clear();
    m_versionMatchedCatalogs.clear();
}
