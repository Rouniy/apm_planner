#include "SrtmElevationSource.h"

#include <quazip.h>
#include <quazipfile.h>
#include <quazipfileinfo.h>

#include <QBuffer>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 kSrtm3ByteCount = 1201LL * 1201LL * 2LL;
constexpr qint64 kSrtm1ByteCount = 3601LL * 3601LL * 2LL;
constexpr int kMinimumTerrainAltitude = -1000;
constexpr int kMaximumArchiveBytes = 64 * 1024 * 1024;
constexpr int kRequestSpacingMilliseconds = 1000;
constexpr int kRequestTimeoutMilliseconds = 30000;
constexpr int kOceanMarkerLifetimeDays = 30;
const QByteArray kOceanMarkerContents("APM-SRTM-CACHE-v2\n");

QDateTime cacheEpoch()
{
    return QDateTime(QDate(2026, 3, 1), QTime(0, 0), Qt::UTC);
}

bool validHgtFile(const QString &path)
{
    const QFileInfo info(path);
    return info.isFile() && info.lastModified().toUTC() >= cacheEpoch()
        && (info.size() == kSrtm3ByteCount
            || info.size() == kSrtm1ByteCount);
}

bool validOceanMarker(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.lastModified().toUTC() < cacheEpoch()
        || info.lastModified().toUTC().daysTo(
               QDateTime::currentDateTimeUtc()) > kOceanMarkerLifetimeDays) {
        return false;
    }
    QFile marker(path);
    return marker.open(QIODevice::ReadOnly)
        && marker.readAll() == kOceanMarkerContents;
}

bool validCoordinate(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude < 90.0
        && longitude >= -180.0 && longitude < 180.0;
}

qint16 readBigEndianSample(QFile *file, int size, int row, int column,
                           bool *ok)
{
    if (!file || !ok || row < 0 || column < 0
        || row >= size || column >= size) {
        if (ok) {
            *ok = false;
        }
        return 0;
    }
    const qint64 offset = (static_cast<qint64>(row) * size + column) * 2;
    char bytes[2] = {};
    if (!file->seek(offset) || file->read(bytes, 2) != 2) {
        *ok = false;
        return 0;
    }
    const quint16 value = (static_cast<quint16>(
                               static_cast<unsigned char>(bytes[0])) << 8)
        | static_cast<quint16>(static_cast<unsigned char>(bytes[1]));
    return static_cast<qint16>(value);
}

double interpolate(double first, double second, double fraction)
{
    return first + (second - first) * fraction;
}
}

SrtmElevationSource::SrtmElevationSource(
    QObject *parent, const QString &cacheDirectory,
    const QStringList &downloadRoots)
    : QObject(parent),
      m_cacheDirectory(QDir::cleanPath(cacheDirectory.trimmed().isEmpty()
          ? DefaultCacheDirectory() : cacheDirectory))
{
    QSettings settings;
    settings.setFallbacksEnabled(false);
    m_autoDownload = settings.value(
        QString::fromLatin1(AutoDownloadSettingsKey), true).toBool();
    const QStringList officialRoots{
        QStringLiteral("https://terrain.ardupilot.org/SRTM1/"),
        QStringLiteral("https://terrain.ardupilot.org/SRTM3/Africa/"),
        QStringLiteral("https://terrain.ardupilot.org/SRTM3/Antarctic/"),
        QStringLiteral("https://terrain.ardupilot.org/SRTM3/Arctic/"),
        QStringLiteral("https://terrain.ardupilot.org/SRTM3/Australia/"),
        QStringLiteral("https://terrain.ardupilot.org/SRTM3/Eurasia/"),
        QStringLiteral("https://terrain.ardupilot.org/SRTM3/Islands/"),
        QStringLiteral("https://terrain.ardupilot.org/SRTM3/Misc/"),
        QStringLiteral("https://terrain.ardupilot.org/SRTM3/North_America/"),
        QStringLiteral("https://terrain.ardupilot.org/SRTM3/South_America/")};
    m_downloadRoots = downloadRoots.isEmpty() ? officialRoots : downloadRoots;
}

SrtmElevationSource::~SrtmElevationSource()
{
    Shutdown();
}

QString SrtmElevationSource::TileName(double latitude, double longitude)
{
    if (!validCoordinate(latitude, longitude)) {
        return {};
    }
    const int south = static_cast<int>(std::floor(latitude));
    const int west = static_cast<int>(std::floor(longitude));
    return QStringLiteral("%1%2%3%4.hgt")
        .arg(south >= 0 ? QLatin1String("N") : QLatin1String("S"))
        .arg(std::abs(south), 2, 10, QLatin1Char('0'))
        .arg(west >= 0 ? QLatin1String("E") : QLatin1String("W"))
        .arg(std::abs(west), 3, 10, QLatin1Char('0'));
}

QString SrtmElevationSource::DefaultCacheDirectory()
{
    const QString explicitDirectory = QString::fromLocal8Bit(
        qgetenv("APM_SRTM_CACHE_DIR")).trimmed();
    if (!explicitDirectory.isEmpty()) {
        return QDir::cleanPath(QFileInfo(explicitDirectory).absoluteFilePath());
    }
    QString dataRoot = QString::fromLocal8Bit(
        qgetenv("APM_PLANNER_HOME")).trimmed();
    if (dataRoot.isEmpty()) {
        dataRoot = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
    }
    if (dataRoot.isEmpty()) {
        dataRoot = QDir(QStandardPaths::writableLocation(
            QStandardPaths::HomeLocation)).filePath(
                QStringLiteral("apmplanner3"));
    }
    return QDir::cleanPath(QDir(dataRoot).filePath(QStringLiteral("srtm")));
}

bool SrtmElevationSource::SampleHgtFile(
    const QString &path, double latitude, double longitude, double *altitude)
{
    if (!altitude || !validCoordinate(latitude, longitude)) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const qint64 byteCount = file.size();
    const int size = byteCount == kSrtm3ByteCount ? 1201
        : byteCount == kSrtm1ByteCount ? 3601 : 0;
    if (size == 0) {
        return false;
    }

    const double latitudeFraction = latitude - std::floor(latitude);
    const double longitudeFraction = longitude - std::floor(longitude);
    const double columnPosition = std::clamp(
        longitudeFraction * (size - 1), 0.0,
        static_cast<double>(size - 1));
    const double rowPosition = std::clamp(
        (1.0 - latitudeFraction) * (size - 1), 0.0,
        static_cast<double>(size - 1));
    const int column0 = std::min(static_cast<int>(std::floor(columnPosition)),
                                 size - 2);
    const int row0 = std::min(static_cast<int>(std::floor(rowPosition)),
                              size - 2);
    const double columnFraction = columnPosition - column0;
    const double rowFraction = rowPosition - row0;

    bool ok = true;
    const double northWest = readBigEndianSample(
        &file, size, row0, column0, &ok);
    const double northEast = readBigEndianSample(
        &file, size, row0, column0 + 1, &ok);
    const double southWest = readBigEndianSample(
        &file, size, row0 + 1, column0, &ok);
    const double southEast = readBigEndianSample(
        &file, size, row0 + 1, column0 + 1, &ok);
    if (!ok || northWest < kMinimumTerrainAltitude
        || northEast < kMinimumTerrainAltitude
        || southWest < kMinimumTerrainAltitude
        || southEast < kMinimumTerrainAltitude) {
        return false;
    }
    const double north = interpolate(northWest, northEast, columnFraction);
    const double south = interpolate(southWest, southEast, columnFraction);
    const double result = interpolate(north, south, rowFraction);
    if (!std::isfinite(result) || result < kMinimumTerrainAltitude) {
        return false;
    }
    *altitude = result;
    return true;
}

QString SrtmElevationSource::CacheDirectory() const
{
    return m_cacheDirectory;
}

bool SrtmElevationSource::AutoDownloadEnabled() const
{
    QMutexLocker locker(&m_requestGate);
    return m_autoDownload;
}

void SrtmElevationSource::SetAutoDownloadEnabled(bool enabled)
{
    {
        QMutexLocker locker(&m_requestGate);
        if (m_autoDownload == enabled) {
            return;
        }
        m_autoDownload = enabled;
    }
    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.setValue(QString::fromLatin1(AutoDownloadSettingsKey), enabled);
    settings.sync();
}

bool SrtmElevationSource::SampleAltitudeCached(
    double latitude, double longitude, double *altitude) const
{
    // WGS84 permits +180 while degree tiles use the equivalent -180 edge.
    const double normalizedLongitude = longitude == 180.0
        ? -180.0 : longitude;
    const QString tileName = TileName(latitude, normalizedLongitude);
    if (tileName.isEmpty() || !altitude) {
        return false;
    }
    const QString tilePath = TilePath(tileName);
    if (QFileInfo::exists(tilePath)) {
        if (validHgtFile(tilePath)) {
            return SampleHgtFile(
                tilePath, latitude, normalizedLongitude, altitude);
        }
        QFile::remove(tilePath);
    }
    const QString markerPath = OceanMarkerPath(tileName);
    if (validOceanMarker(markerPath)) {
        *altitude = 0.0;
        return true;
    }
    QFile::remove(markerPath);
    return false;
}

bool SrtmElevationSource::RequestTileForCoordinate(
    double latitude, double longitude)
{
    const double normalizedLongitude = longitude == 180.0
        ? -180.0 : longitude;
    const QString tileName = TileName(latitude, normalizedLongitude);
    if (tileName.isEmpty()) {
        return false;
    }
    {
        // Admission and de-duplication happen before a worker can enqueue a
        // GUI-thread meta-call. A full-world raster can therefore create no
        // more than MaximumPendingTiles events or network jobs.
        QMutexLocker locker(&m_requestGate);
        if (!m_autoDownload || m_shuttingDown
            || m_reservedTiles.contains(tileName)
            || m_reservedTiles.size() >= MaximumPendingTiles) {
            return false;
        }
        m_reservedTiles.insert(tileName);
    }

    if (QThread::currentThread() == thread()) {
        RequestTile(tileName);
        return true;
    }
    const bool queued = QMetaObject::invokeMethod(
        this, [this, tileName]() { RequestTile(tileName); },
        Qt::QueuedConnection);
    if (!queued) {
        ReleaseTileReservation(tileName);
    }
    return queued;
}

bool SrtmElevationSource::SampleAltitude(
    double latitude, double longitude, double *altitude)
{
    if (SampleAltitudeCached(latitude, longitude, altitude)) {
        return true;
    }
    RequestTileForCoordinate(latitude, longitude);
    return false;
}

void SrtmElevationSource::RequestTile(const QString &tileName)
{
    {
        QMutexLocker locker(&m_requestGate);
        if (m_shuttingDown) {
            m_reservedTiles.remove(tileName);
            return;
        }
    }
    if (m_pendingTiles.contains(tileName)
        || validHgtFile(TilePath(tileName))
        || validOceanMarker(OceanMarkerPath(tileName))) {
        ReleaseTileReservation(tileName);
        return;
    }
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    m_pendingTiles.insert(tileName);
    m_downloadQueue.enqueue(tileName);
    StartNextTile();
}

void SrtmElevationSource::ReleaseTileReservation(const QString &tileName)
{
    QMutexLocker locker(&m_requestGate);
    m_reservedTiles.remove(tileName);
}

void SrtmElevationSource::StartNextTile()
{
    if (m_shuttingDown || !m_activeTile.isEmpty()) {
        return;
    }
    while (!m_downloadQueue.isEmpty()) {
        const QString tileName = m_downloadQueue.dequeue();
        if (m_pendingTiles.contains(tileName)) {
            m_activeTile = tileName;
            StartCandidate(tileName, 0);
            return;
        }
    }
}

void SrtmElevationSource::ScheduleNextTile()
{
    m_activeTile.clear();
    QTimer::singleShot(kRequestSpacingMilliseconds, this,
                       [this]() { StartNextTile(); });
}

void SrtmElevationSource::StartCandidate(
    const QString &tileName, int candidateIndex)
{
    if (m_shuttingDown || !m_pendingTiles.contains(tileName)) {
        return;
    }
    if (candidateIndex >= m_downloadRoots.size()) {
        bool markerStored = false;
        if (QDir().mkpath(m_cacheDirectory)) {
            QSaveFile marker(OceanMarkerPath(tileName));
            if (marker.open(QIODevice::WriteOnly)) {
                markerStored = marker.write(kOceanMarkerContents)
                        == kOceanMarkerContents.size()
                    && marker.commit();
            }
        }
        if (!markerStored) {
            FinishFailure(
                tileName,
                tr("Unable to store the ocean marker for %1.")
                    .arg(tileName));
            return;
        }
        m_pendingTiles.remove(tileName);
        ReleaseTileReservation(tileName);
        ScheduleNextTile();
        emit TileAvailable(tileName);
        return;
    }
    const QUrl url(m_downloadRoots.at(candidateIndex)
                   + tileName + QStringLiteral(".zip"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("APM Planner 3.0.0"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_network->get(request);
    reply->setReadBufferSize(kMaximumArchiveBytes);
    m_replies.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
            [this, tileName, candidateIndex, reply]() {
        FinishCandidate(tileName, candidateIndex, reply);
    });
    QTimer::singleShot(kRequestTimeoutMilliseconds, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
}

void SrtmElevationSource::FinishCandidate(
    const QString &tileName, int candidateIndex, QNetworkReply *reply)
{
    if (!reply) {
        FinishFailure(tileName, tr("Terrain download ended without a reply."));
        return;
    }
    m_replies.remove(reply);
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QByteArray payload = networkError == QNetworkReply::NoError
        ? reply->readAll() : QByteArray();
    const QString errorText = reply->errorString();
    reply->deleteLater();
    if (m_shuttingDown || !m_pendingTiles.contains(tileName)) {
        return;
    }
    if (networkError == QNetworkReply::NoError && status >= 200
        && status < 300) {
        QString error;
        if (InstallArchive(tileName, payload, &error)) {
            m_pendingTiles.remove(tileName);
            ReleaseTileReservation(tileName);
            ScheduleNextTile();
            emit TileAvailable(tileName);
        } else {
            FinishFailure(tileName, error);
        }
        return;
    }
    if (status == 404
        || networkError == QNetworkReply::ContentNotFoundError) {
        QTimer::singleShot(
            kRequestSpacingMilliseconds, this,
            [this, tileName, candidateIndex]() {
                StartCandidate(tileName, candidateIndex + 1);
            });
        return;
    }
    FinishFailure(tileName, tr("Terrain download failed: %1").arg(errorText));
}

bool SrtmElevationSource::InstallArchive(
    const QString &tileName, const QByteArray &archive, QString *error)
{
    if (archive.isEmpty() || archive.size() > kMaximumArchiveBytes) {
        if (error) {
            *error = tr("Downloaded terrain archive %1 has an invalid size.")
                .arg(tileName);
        }
        return false;
    }
    QByteArray mutableArchive = archive;
    QBuffer buffer(&mutableArchive);
    if (!buffer.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = tr("Unable to open the downloaded terrain archive.");
        }
        return false;
    }
    QuaZip zip(&buffer);
    if (!zip.open(QuaZip::mdUnzip)
        || !zip.setCurrentFile(tileName, QuaZip::csInsensitive)) {
        if (error) {
            *error = tr("The terrain archive does not contain %1.")
                .arg(tileName);
        }
        return false;
    }
    QuaZipFileInfo fileInfo;
    if (!zip.getCurrentFileInfo(&fileInfo)
        || (fileInfo.uncompressedSize != kSrtm3ByteCount
            && fileInfo.uncompressedSize != kSrtm1ByteCount)) {
        if (error) {
            *error = tr("Downloaded terrain tile %1 has an invalid size.")
                .arg(tileName);
        }
        return false;
    }
    QuaZipFile zippedFile(&zip);
    if (!zippedFile.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = tr("Unable to read %1 from the terrain archive.")
                .arg(tileName);
        }
        return false;
    }
    const QByteArray terrain = zippedFile.readAll();
    zippedFile.close();
    zip.close();
    if (terrain.size() != kSrtm3ByteCount
        && terrain.size() != kSrtm1ByteCount) {
        if (error) {
            *error = tr("Downloaded terrain tile %1 has an invalid size.")
                .arg(tileName);
        }
        return false;
    }
    if (!QDir().mkpath(m_cacheDirectory)) {
        if (error) {
            *error = tr("Unable to create the terrain cache directory: %1")
                .arg(m_cacheDirectory);
        }
        return false;
    }
    QSaveFile output(TilePath(tileName));
    if (!output.open(QIODevice::WriteOnly)
        || output.write(terrain) != terrain.size()
        || !output.commit()) {
        if (error) {
            *error = tr("Unable to store terrain tile %1.").arg(tileName);
        }
        return false;
    }
    QFile::remove(OceanMarkerPath(tileName));
    if (error) {
        error->clear();
    }
    return true;
}

QString SrtmElevationSource::TilePath(const QString &tileName) const
{
    return QDir(m_cacheDirectory).filePath(tileName);
}

QString SrtmElevationSource::OceanMarkerPath(const QString &tileName) const
{
    return TilePath(tileName) + QStringLiteral(".ocean");
}

void SrtmElevationSource::FinishFailure(
    const QString &tileName, const QString &error)
{
    m_pendingTiles.remove(tileName);
    ReleaseTileReservation(tileName);
    if (m_activeTile == tileName) {
        ScheduleNextTile();
    }
    emit DownloadFailed(tileName, error);
}

void SrtmElevationSource::Shutdown()
{
    {
        QMutexLocker locker(&m_requestGate);
        if (m_shuttingDown) {
            return;
        }
        m_shuttingDown = true;
        m_reservedTiles.clear();
    }
    m_pendingTiles.clear();
    m_downloadQueue.clear();
    m_activeTile.clear();
    const QSet<QNetworkReply *> replies = m_replies;
    m_replies.clear();
    for (QNetworkReply *reply : replies) {
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
    }
}
