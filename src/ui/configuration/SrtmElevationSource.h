#ifndef SRTMELEVATIONSOURCE_H
#define SRTMELEVATIONSOURCE_H

#include <QObject>
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

    bool SampleAltitude(double latitude, double longitude,
                        double *altitude);
    void Shutdown();

signals:
    void TileAvailable(const QString &tileName);
    void DownloadFailed(const QString &tileName, const QString &error);

private:
    void RequestTile(const QString &tileName);
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
    QSet<QNetworkReply *> m_replies;
    QQueue<QString> m_downloadQueue;
    QString m_activeTile;
    bool m_autoDownload = true;
    bool m_shuttingDown = false;
};

#endif
