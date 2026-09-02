#ifndef FRAMEDEFAULTCATALOGSERVICE_H
#define FRAMEDEFAULTCATALOGSERVICE_H

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

/*
 * Port of Mission Planner 10 `Services/FrameDefaultCatalogService.cs`: lists
 * the official ArduPilot `Tools/Frame_params` catalog through the GitHub
 * contents API and downloads one `.param` file from raw.githubusercontent.
 *
 * The service is an isolated Qt object with no UI, settings or parameter
 * parsing. Networking is asynchronous through QNetworkAccessManager; the
 * manager and both URL roots are injectable so tests run fully offline.
 * Every request has a data-transfer watchdog, every body is bounded while it
 * streams, catalog results are memoised like MP10, and cancellation reports
 * exactly one failure signal while late replies are ignored.
 */

struct FrameDefaultCatalogItem
{
    QString name; // GitHub entry name, e.g. "Tailsitter.param"
    QString path; // normalized catalog path, e.g. "Tools/Frame_params/QuadPlanes/Tailsitter.param"

    // MP10 FrameDefaultFile.ToString(): the path below Tools/Frame_params with
    // " / " separators, or the entry name when the path is not below the root.
    QString displayName() const;

    bool operator==(const FrameDefaultCatalogItem &other) const
    {
        return name == other.name && path == other.path;
    }
    bool operator!=(const FrameDefaultCatalogItem &other) const
    {
        return !(*this == other);
    }
};

Q_DECLARE_METATYPE(FrameDefaultCatalogItem)

class FrameDefaultCatalogService final : public QObject
{
    Q_OBJECT

public:
    using OperationId = quint64;
    static constexpr OperationId InvalidOperationId = 0;

    static const QString CatalogRoot;      // "Tools/Frame_params"
    static const QString UserAgent;        // "APMPlanner3/frame-defaults"
    static const QUrl DefaultApiRoot;      // GitHub contents API root (trailing slash)
    static const QUrl DefaultRawRoot;      // raw.githubusercontent master root (trailing slash)
    static const qint64 DefaultMaxListingBytes;   // 4 MiB per directory listing
    static const qint64 DefaultMaxDownloadBytes;  // 1 MiB per .param file
    static const int DefaultMaxDirectories;       // 64 directories per catalog walk
    static const int DefaultMaxEntries;           // 4096 .param files per catalog
    static const int DefaultTransferTimeoutMs;    // 30 s without data aborts a request

    // Production: owns a QNetworkAccessManager and uses the official roots.
    explicit FrameDefaultCatalogService(QObject *parent = nullptr);
    // Injected manager (caller-owned, may be destroyed at any time) and roots.
    FrameDefaultCatalogService(QNetworkAccessManager *manager,
                               const QUrl &apiRoot, const QUrl &rawRoot,
                               QObject *parent = nullptr);
    ~FrameDefaultCatalogService() override;

    // --- pure helpers (MP10 semantics, errors returned instead of thrown) ---

    // Validates a GitHub entry path: non-empty, no leading '/', no '\\', no
    // empty/"."/".." segments, exactly CatalogRoot or below it (case-sensitive).
    static QString NormalizeEntryPath(const QString &path, QString *error = nullptr);
    // NormalizeEntryPath plus the ".param" extension requirement.
    static QString NormalizeParamPath(const QString &path, QString *error = nullptr);
    // Percent-encodes every segment (Uri.EscapeDataString) while keeping '/'.
    static QString EscapePath(const QString &path);
    // <cacheRoot>/frame-defaults/<catalog segments>. Rejects lexical escapes,
    // existing symbolic-link components exposed by Qt, and existing canonical
    // escapes (including reparse-point traversal). This intentionally does not
    // claim protection from a concurrent process replacing filesystem entries
    // after the check; empty is returned with an error when validation fails.
    static QString GetCachePath(const QString &cacheRoot, const QString &catalogPath,
                                QString *error = nullptr);
    // Writes bytes atomically (QSaveFile) to GetCachePath and returns the path.
    static QString WriteCacheFile(const QString &cacheRoot, const QString &catalogPath,
                                  const QByteArray &bytes, QString *error = nullptr);
    // Application data root (same derivation as the srtm cache); files land
    // in <root>/frame-defaults.
    static QString DefaultCacheRoot();
    static QString DisplayName(const QString &name, const QString &path);

    // --- limits ---
    qint64 maxListingBytes() const;
    void setMaxListingBytes(qint64 bytes);
    qint64 maxDownloadBytes() const;
    void setMaxDownloadBytes(qint64 bytes);
    int maxDirectories() const;
    void setMaxDirectories(int count);
    int maxEntries() const;
    void setMaxEntries(int count);
    int transferTimeoutMs() const;
    void setTransferTimeoutMs(int milliseconds);

    QUrl apiRoot() const;
    QUrl rawRoot() const;

    // --- catalog ---

    // Starts (or joins) a catalog walk. `started`, when supplied, is true only
    // when this call created the active walk; a joining caller must not cancel
    // work owned by its starter. `operationId` receives the active walk id for
    // both Started and Joined, or InvalidOperationId for a cached/failed call.
    // With a memoised catalog and no forceRefresh, catalogReady(items, true) is
    // emitted synchronously before returning. Returns false only when no request
    // could be started.
    bool requestCatalog(bool forceRefresh = false, QString *error = nullptr,
                        bool *started = nullptr,
                        OperationId *operationId = nullptr);
    bool isCatalogLoading() const;
    bool hasCachedCatalog() const;
    QVector<FrameDefaultCatalogItem> cachedCatalog() const;

    // --- download ---

    // Validates the catalog path and starts one bounded download. Returns
    // false with an error when the path is invalid, a download is already
    // running, or no request could be started; no signal is emitted then.
    bool download(const QString &catalogPath, QString *error = nullptr,
                  OperationId *operationId = nullptr);
    bool isDownloading() const;

    // Cancels exactly the matching active operation. Returns false and emits
    // nothing for InvalidOperationId, stale ids, or an operation of the other
    // kind. On success the matching failure signal is emitted synchronously,
    // exactly once, with cancelled == true; the other operation is untouched.
    bool cancelCatalog(OperationId operationId);
    bool cancelDownload(OperationId operationId);

    // Aborts the active catalog walk and download. Each active operation
    // reports exactly one failure signal with cancelled == true; both are
    // detached before any signal is emitted, so slots may start new work.
    // Destroying an injected manager likewise fails active operations once
    // (cancelled == false).
    void cancel();

signals:
    void catalogReady(const QVector<FrameDefaultCatalogItem> &items, bool fromCache);
    void catalogFailed(const QString &error, bool cancelled);
    void downloadFinished(const QString &catalogPath, const QByteArray &bytes);
    void downloadFailed(const QString &catalogPath, const QString &error, bool cancelled);

private:
    struct Transfer
    {
        OperationId generation = InvalidOperationId;
        QPointer<QNetworkReply> reply;
        QByteArray body;
        bool tooLarge = false;
        bool timedOut = false;
    };

    struct CatalogWalk : Transfer
    {
        QStringList pending;
        QSet<QString> visited; // case-folded directories
        QSet<QString> discovered; // case-folded pending + visited directories
        int directoriesRequested = 0;
        QString currentDirectory;
        QVector<FrameDefaultCatalogItem> files;
    };

    struct Download : Transfer
    {
        QString catalogPath;
    };

    QNetworkAccessManager *manager() const;
    QNetworkRequest makeRequest(const QUrl &url) const;
    Transfer *activeTransfer(OperationId generation);
    bool startGet(const QUrl &url, OperationId generation, QTimer &watchdog,
                  qint64 limit,
                  void (FrameDefaultCatalogService::*finished)(OperationId),
                  QString *error);
    QString replyFailure(const QString &what, QNetworkReply *reply) const;
    // Reads at most limit + 1 - body.size() bytes; marks tooLarge, clears the
    // body and aborts the reply when the limit is exceeded (returns false).
    static bool drainReply(Transfer &transfer, QNetworkReply *reply, qint64 limit);
    // Disconnects the reply from owner and, unless the reply's manager is
    // being destroyed, aborts and releases it.
    static void discardReply(Transfer &transfer, QObject *owner, bool abortReply);
    // Detaches every active operation first, then emits the saved failure
    // signals, so a re-entrant slot may start new operations safely.
    void failAll(const QString &catalogError, const QString &downloadError,
                 bool cancelled, bool abortReplies);
    void injectedManagerDestroyed();

    static bool parseListing(const QByteArray &body, QSet<QString> *discoveredDirectories,
                             int maxDirectories, QStringList *directories,
                             QVector<FrameDefaultCatalogItem> *files, QString *error);
    static QVector<FrameDefaultCatalogItem> deduplicateAndSort(
        const QVector<FrameDefaultCatalogItem> &files);

    bool startNextDirectory(QString *startError);
    void catalogReplyFinished(OperationId generation);
    void finishCatalog(const QVector<FrameDefaultCatalogItem> &items);
    void failCatalog(const QString &error, bool cancelled = false);

    void downloadReplyFinished(OperationId generation);
    void failDownload(const QString &catalogPath, const QString &error,
                      bool cancelled = false);

    QPointer<QNetworkAccessManager> m_injectedManager;
    QNetworkAccessManager *m_ownedManager = nullptr;
    QUrl m_apiRoot;
    QUrl m_rawRoot;
    qint64 m_maxListingBytes;
    qint64 m_maxDownloadBytes;
    int m_maxDirectories;
    int m_maxEntries;
    int m_transferTimeoutMs;
    OperationId m_nextGeneration = 1;

    std::unique_ptr<CatalogWalk> m_catalog;
    std::unique_ptr<Download> m_download;
    QTimer m_catalogWatchdog;
    QTimer m_downloadWatchdog;

    bool m_hasCachedCatalog = false;
    QVector<FrameDefaultCatalogItem> m_cachedCatalog;
};

#endif
