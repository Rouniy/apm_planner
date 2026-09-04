#ifndef SRTMELEVATIONSOURCE_H
#define SRTMELEVATIONSOURCE_H

#include <QObject>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

class SrtmElevationSource final : public QObject
{
    Q_OBJECT

public:
    static constexpr const char *AutoDownloadSettingsKey =
        "FlightPlanner/SrtmAutoDownload";
    static constexpr int MaximumPendingTiles = 8;

    explicit SrtmElevationSource(QObject *parent = nullptr,
                                 const QString &cacheDirectory = QString(),
                                 const QStringList &downloadRoots = {});
    ~SrtmElevationSource() override;

    static QString TileName(double latitude, double longitude);
    static QString DefaultCacheDirectory();
    static bool SampleHgtFile(const QString &path, double latitude,
                              double longitude, double *altitude);

    QString CacheDirectory() const;
    bool AutoDownloadEnabled() const;
    void SetAutoDownloadEnabled(bool enabled);

    /** Reads only an installed HGT tile or ocean marker; never uses network. */
    bool SampleAltitudeCached(double latitude, double longitude,
                              double *altitude) const;
    /** Admits at most one bounded background request for this degree tile. */
    bool RequestTileForCoordinate(double latitude, double longitude);
    bool SampleAltitude(double latitude, double longitude,
                        double *altitude);
    void Shutdown();

signals:
    void TileAvailable(const QString &tileName);
    void DownloadFailed(const QString &tileName, const QString &error);

private:
    void RequestTile(const QString &tileName);
    void ReleaseTileReservation(const QString &tileName);
    void StartNextTile();
    void ScheduleNextTile();
    void StartCandidate(const QString &tileName, int candidateIndex);
    void FinishCandidate(const QString &tileName, int candidateIndex,
                         QNetworkReply *reply);
    bool InstallArchive(const QString &tileName, const QByteArray &archive,
                        QString *error);
    QString TilePath(const QString &tileName) const;
    QString OceanMarkerPath(const QString &tileName) const;
    void FinishFailure(const QString &tileName, const QString &error);

    QString m_cacheDirectory;
    QStringList m_downloadRoots;
    QNetworkAccessManager *m_network = nullptr;
    QSet<QString> m_pendingTiles;
    QSet<QString> m_reservedTiles;
    QSet<QNetworkReply *> m_replies;
    QQueue<QString> m_downloadQueue;
    mutable QMutex m_requestGate;
    QString m_activeTile;
    bool m_autoDownload = true;
    bool m_shuttingDown = false;
};

#endif
