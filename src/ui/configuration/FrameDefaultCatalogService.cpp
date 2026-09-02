#include "FrameDefaultCatalogService.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVariant>

#include <algorithm>

const QString FrameDefaultCatalogService::CatalogRoot =
    QStringLiteral("Tools/Frame_params");
const QString FrameDefaultCatalogService::UserAgent =
    QStringLiteral("APMPlanner3/frame-defaults");
const QUrl FrameDefaultCatalogService::DefaultApiRoot(
    QStringLiteral("https://api.github.com/repos/ArduPilot/ardupilot/contents/"));
const QUrl FrameDefaultCatalogService::DefaultRawRoot(
    QStringLiteral("https://raw.githubusercontent.com/ArduPilot/ardupilot/master/"));
const qint64 FrameDefaultCatalogService::DefaultMaxListingBytes = 4 * 1024 * 1024;
const qint64 FrameDefaultCatalogService::DefaultMaxDownloadBytes = 1024 * 1024;
const int FrameDefaultCatalogService::DefaultMaxDirectories = 64;
const int FrameDefaultCatalogService::DefaultMaxEntries = 4096;
const int FrameDefaultCatalogService::DefaultTransferTimeoutMs = 30000;

namespace {

QString withTrailingSlash(const QString &path)
{
    if (path.endsWith(QLatin1Char('/'))) {
        return path;
    }
    return path + QLatin1Char('/');
}

bool catalogPathLess(const FrameDefaultCatalogItem &left,
                     const FrameDefaultCatalogItem &right)
{
    return QString::compare(left.path, right.path, Qt::CaseInsensitive) < 0;
}

Qt::CaseSensitivity fileSystemPathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool pathIsWithin(const QString &path, const QString &directory)
{
    const Qt::CaseSensitivity sensitivity = fileSystemPathCaseSensitivity();
    return QString::compare(path, directory, sensitivity) == 0 ||
           path.startsWith(withTrailingSlash(directory), sensitivity);
}

bool isFileSystemLink(const QFileInfo &info)
{
    if (info.isSymLink()) {
        return true;
    }
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    // On Windows QFileInfo distinguishes junctions from symbolic links. Keep
    // this guarded so the project remains compatible with its Qt 5.10 floor.
    if (info.isJunction()) {
        return true;
    }
#endif
    return false;
}

// cacheRoot itself is the trusted filesystem anchor. Its existing final
// component and every existing component below it must be an ordinary entry.
// Canonical containment additionally catches link-like/reparse traversal that
// older Qt versions do not identify explicitly. This is a point-in-time check,
// not a defence against a concurrent filesystem replacement.
bool validateExistingCachePath(const QString &cacheRoot, const QString &target,
                               QString *unsafeComponent)
{
    const QString cleanRoot = QDir::cleanPath(QFileInfo(cacheRoot).absoluteFilePath());
    const QString cleanTarget = QDir::cleanPath(QFileInfo(target).absoluteFilePath());
    if (!pathIsWithin(cleanTarget, cleanRoot)) {
        if (unsafeComponent) {
            *unsafeComponent = cleanTarget;
        }
        return false;
    }

    const QFileInfo rootInfo(cleanRoot);
    if (isFileSystemLink(rootInfo)) {
        if (unsafeComponent) {
            *unsafeComponent = cleanRoot;
        }
        return false;
    }
    const QString canonicalRoot = rootInfo.exists() ? rootInfo.canonicalFilePath()
                                                     : QString();

    const QString relative = QDir(cleanRoot).relativeFilePath(cleanTarget);
    QString current = cleanRoot;
    const QStringList components = relative.split(QLatin1Char('/'));
    for (const QString &component : components) {
        if (component.isEmpty()) {
            continue;
        }
        current = QDir(current).filePath(component);
        const QFileInfo info(current);
        if (isFileSystemLink(info)) {
            if (unsafeComponent) {
                *unsafeComponent = current;
            }
            return false;
        }
        if (!canonicalRoot.isEmpty() && info.exists()) {
            const QString canonical = info.canonicalFilePath();
            if (canonical.isEmpty() || !pathIsWithin(canonical, canonicalRoot)) {
                if (unsafeComponent) {
                    *unsafeComponent = current;
                }
                return false;
            }
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// FrameDefaultCatalogItem

QString FrameDefaultCatalogItem::displayName() const
{
    return FrameDefaultCatalogService::DisplayName(name, path);
}

// ---------------------------------------------------------------------------
// construction

FrameDefaultCatalogService::FrameDefaultCatalogService(QObject *parent)
    : FrameDefaultCatalogService(nullptr, DefaultApiRoot, DefaultRawRoot, parent)
{
}

FrameDefaultCatalogService::FrameDefaultCatalogService(
    QNetworkAccessManager *manager, const QUrl &apiRoot, const QUrl &rawRoot,
    QObject *parent)
    : QObject(parent),
      m_injectedManager(manager),
      m_apiRoot(apiRoot.isValid() ? apiRoot : DefaultApiRoot),
      m_rawRoot(rawRoot.isValid() ? rawRoot : DefaultRawRoot),
      m_maxListingBytes(DefaultMaxListingBytes),
      m_maxDownloadBytes(DefaultMaxDownloadBytes),
      m_maxDirectories(DefaultMaxDirectories),
      m_maxEntries(DefaultMaxEntries),
      m_transferTimeoutMs(DefaultTransferTimeoutMs)
{
    qRegisterMetaType<FrameDefaultCatalogItem>();
    qRegisterMetaType<QVector<FrameDefaultCatalogItem>>();

    if (manager) {
        connect(manager, &QObject::destroyed, this,
                &FrameDefaultCatalogService::injectedManagerDestroyed);
    } else {
        m_ownedManager = new QNetworkAccessManager(this);
    }

    m_catalogWatchdog.setSingleShot(true);
    connect(&m_catalogWatchdog, &QTimer::timeout, this, [this]() {
        if (m_catalog && m_catalog->reply) {
            m_catalog->timedOut = true;
            m_catalog->reply->abort();
        }
    });
    m_downloadWatchdog.setSingleShot(true);
    connect(&m_downloadWatchdog, &QTimer::timeout, this, [this]() {
        if (m_download && m_download->reply) {
            m_download->timedOut = true;
            m_download->reply->abort();
        }
    });
}

FrameDefaultCatalogService::~FrameDefaultCatalogService()
{
    // Destruction never emits signals; just release the network replies.
    m_catalogWatchdog.stop();
    m_downloadWatchdog.stop();
    if (m_catalog) {
        std::unique_ptr<CatalogWalk> walk = std::move(m_catalog);
        discardReply(*walk, this, true);
    }
    if (m_download) {
        std::unique_ptr<Download> download = std::move(m_download);
        discardReply(*download, this, true);
    }
}

// ---------------------------------------------------------------------------
// pure helpers

QString FrameDefaultCatalogService::NormalizeEntryPath(const QString &path,
                                                       QString *error)
{
    if (path.trimmed().isEmpty() || path.startsWith(QLatin1Char('/')) ||
        path.contains(QLatin1Char('\\'))) {
        if (error) {
            *error = tr("GitHub returned an invalid frame-default path.");
        }
        return QString();
    }
    const QStringList parts = path.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part.trimmed().isEmpty() || part == QLatin1String(".") ||
            part == QLatin1String("..")) {
            if (error) {
                *error = tr("GitHub returned an unsafe frame-default path.");
            }
            return QString();
        }
    }
    const QString normalized = parts.join(QLatin1Char('/'));
    if (!normalized.startsWith(withTrailingSlash(CatalogRoot)) &&
        normalized != CatalogRoot) {
        if (error) {
            *error = tr("GitHub returned a path outside %1.").arg(CatalogRoot);
        }
        return QString();
    }
    return normalized;
}

QString FrameDefaultCatalogService::NormalizeParamPath(const QString &path,
                                                       QString *error)
{
    const QString normalized = NormalizeEntryPath(path, error);
    if (normalized.isEmpty()) {
        return QString();
    }
    if (!normalized.endsWith(QLatin1String(".param"), Qt::CaseInsensitive)) {
        if (error) {
            *error = tr("Frame-default files must use the .param extension.");
        }
        return QString();
    }
    return normalized;
}

QString FrameDefaultCatalogService::EscapePath(const QString &path)
{
    QStringList escaped;
    const QStringList segments = path.split(QLatin1Char('/'));
    escaped.reserve(segments.size());
    for (const QString &segment : segments) {
        escaped << QString::fromLatin1(QUrl::toPercentEncoding(segment));
    }
    return escaped.join(QLatin1Char('/'));
}

QString FrameDefaultCatalogService::GetCachePath(const QString &cacheRoot,
                                                 const QString &catalogPath,
                                                 QString *error)
{
    const QString normalized = NormalizeParamPath(catalogPath, error);
    if (normalized.isEmpty()) {
        return QString();
    }
    const QString root = QDir::cleanPath(
        QDir(cacheRoot).absoluteFilePath(QStringLiteral("frame-defaults")));
    const QString result = QDir::cleanPath(root + QLatin1Char('/') + normalized);
    if (!result.startsWith(withTrailingSlash(root))) {
        if (error) {
            *error = tr("Frame-default cache path escapes its cache directory.");
        }
        return QString();
    }
    QString unsafeComponent;
    if (!validateExistingCachePath(QDir(cacheRoot).absolutePath(), result,
                                   &unsafeComponent)) {
        if (error) {
            *error = tr("Frame-default cache path crosses an existing symbolic "
                        "link or reparse point at %1.")
                         .arg(QDir::toNativeSeparators(unsafeComponent));
        }
        return QString();
    }
    return result;
}

QString FrameDefaultCatalogService::WriteCacheFile(const QString &cacheRoot,
                                                   const QString &catalogPath,
                                                   const QByteArray &bytes,
                                                   QString *error)
{
    const QString path = GetCachePath(cacheRoot, catalogPath, error);
    if (path.isEmpty()) {
        return QString();
    }
    const QString directory = QFileInfo(path).path();
    if (!QDir().mkpath(directory)) {
        if (error) {
            *error = tr("Unable to create the frame-default cache directory %1.")
                         .arg(QDir::toNativeSeparators(directory));
        }
        return QString();
    }
    // Recheck after creating missing directories. There is deliberately no
    // promise against a concurrent process replacing an entry after this point;
    // ordinary pre-existing link traversal is rejected.
    QString unsafeComponent;
    if (!validateExistingCachePath(QDir(cacheRoot).absolutePath(), path,
                                   &unsafeComponent)) {
        if (error) {
            *error = tr("Frame-default cache path crosses an existing symbolic "
                        "link or reparse point at %1.")
                         .arg(QDir::toNativeSeparators(unsafeComponent));
        }
        return QString();
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = tr("Unable to write %1: %2")
                         .arg(QDir::toNativeSeparators(path), file.errorString());
        }
        return QString();
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) {
            *error = tr("Unable to write %1: %2")
                         .arg(QDir::toNativeSeparators(path), file.errorString());
        }
        return QString();
    }
    return path;
}

QString FrameDefaultCatalogService::DefaultCacheRoot()
{
    // Same derivation as SrtmElevationSource::DefaultCacheDirectory so the
    // frame-defaults directory lands beside the srtm cache.
    QString dataRoot = QString::fromLocal8Bit(qgetenv("APM_PLANNER_HOME")).trimmed();
    if (dataRoot.isEmpty()) {
        dataRoot = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
    }
    if (dataRoot.isEmpty()) {
        dataRoot = QDir(QStandardPaths::writableLocation(
                            QStandardPaths::HomeLocation))
                       .filePath(QStringLiteral("apmplanner3"));
    }
    return QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath());
}

QString FrameDefaultCatalogService::DisplayName(const QString &name,
                                                const QString &path)
{
    const QString root = withTrailingSlash(CatalogRoot);
    if (path.startsWith(root, Qt::CaseInsensitive)) {
        QString display = path.mid(root.size());
        display.replace(QLatin1String("/"), QLatin1String(" / "));
        return display;
    }
    return name;
}

// ---------------------------------------------------------------------------
// limits and configuration

qint64 FrameDefaultCatalogService::maxListingBytes() const
{
    return m_maxListingBytes;
}

void FrameDefaultCatalogService::setMaxListingBytes(qint64 bytes)
{
    m_maxListingBytes = qMax<qint64>(1, bytes);
}

qint64 FrameDefaultCatalogService::maxDownloadBytes() const
{
    return m_maxDownloadBytes;
}

void FrameDefaultCatalogService::setMaxDownloadBytes(qint64 bytes)
{
    m_maxDownloadBytes = qMax<qint64>(1, bytes);
}

int FrameDefaultCatalogService::maxDirectories() const
{
    return m_maxDirectories;
}

void FrameDefaultCatalogService::setMaxDirectories(int count)
{
    m_maxDirectories = qMax(1, count);
}

int FrameDefaultCatalogService::maxEntries() const
{
    return m_maxEntries;
}

void FrameDefaultCatalogService::setMaxEntries(int count)
{
    m_maxEntries = qMax(1, count);
}

int FrameDefaultCatalogService::transferTimeoutMs() const
{
    return m_transferTimeoutMs;
}

void FrameDefaultCatalogService::setTransferTimeoutMs(int milliseconds)
{
    m_transferTimeoutMs = qMax(0, milliseconds); // 0 disables the watchdog
}

QUrl FrameDefaultCatalogService::apiRoot() const
{
    return m_apiRoot;
}

QUrl FrameDefaultCatalogService::rawRoot() const
{
    return m_rawRoot;
}

// ---------------------------------------------------------------------------
// networking plumbing

QNetworkAccessManager *FrameDefaultCatalogService::manager() const
{
    if (m_ownedManager) {
        return m_ownedManager;
    }
    return m_injectedManager.data();
}

QNetworkRequest FrameDefaultCatalogService::makeRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, UserAgent);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/vnd.github+json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    return request;
}

FrameDefaultCatalogService::Transfer *
FrameDefaultCatalogService::activeTransfer(OperationId generation)
{
    if (m_catalog && m_catalog->generation == generation) {
        return m_catalog.get();
    }
    if (m_download && m_download->generation == generation) {
        return m_download.get();
    }
    return nullptr;
}

bool FrameDefaultCatalogService::startGet(
    const QUrl &url, OperationId generation, QTimer &watchdog, qint64 limit,
    void (FrameDefaultCatalogService::*finished)(OperationId), QString *error)
{
    Transfer *transfer = activeTransfer(generation);
    if (!transfer) {
        if (error) {
            *error = tr("The frame-default operation is no longer active.");
        }
        return false;
    }
    QNetworkAccessManager *networkManager = manager();
    if (!networkManager) {
        if (error) {
            *error = tr("The network access manager is no longer available.");
        }
        return false;
    }
    QNetworkReply *reply = networkManager->get(makeRequest(url));
    if (!reply) {
        if (error) {
            *error = tr("The request for %1 could not be started.")
                         .arg(url.toString());
        }
        return false;
    }

    transfer->reply = reply;
    transfer->body.clear();
    transfer->tooLarge = false;
    transfer->timedOut = false;

    QTimer *watchdogPointer = &watchdog;
    connect(reply, &QIODevice::readyRead, this,
            [this, generation, reply, limit, watchdogPointer]() {
                Transfer *active = activeTransfer(generation);
                if (!active || active->reply != reply) {
                    return; // late data from a cancelled or replaced request
                }
                if (watchdogPointer->isActive()) {
                    watchdogPointer->start(); // data is flowing: restart
                }
                // Bounded read; on overflow the reply is aborted and the
                // finished handler reports the limit.
                drainReply(*active, reply, limit);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, generation, finished]() { (this->*finished)(generation); });

    if (m_transferTimeoutMs > 0) {
        watchdog.start(m_transferTimeoutMs);
    } else {
        watchdog.stop();
    }
    return true;
}

QString FrameDefaultCatalogService::replyFailure(const QString &what,
                                                 QNetworkReply *reply) const
{
    const QVariant statusAttribute =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
    if (status != 0 && (status < 200 || status >= 300)) {
        QString reason =
            reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        if (!reason.isEmpty()) {
            reason.prepend(QLatin1Char(' '));
        }
        return tr("%1 failed: HTTP %2%3.").arg(what).arg(status).arg(reason);
    }
    if (reply->error() != QNetworkReply::NoError) {
        return tr("%1 failed: %2").arg(what, reply->errorString());
    }
    return QString();
}

bool FrameDefaultCatalogService::drainReply(Transfer &transfer,
                                            QNetworkReply *reply, qint64 limit)
{
    while (true) {
        const qint64 allowed = limit + 1 - transfer.body.size();
        if (allowed <= 0) {
            break;
        }
        const QByteArray chunk = reply->read(allowed);
        if (chunk.isEmpty()) {
            break;
        }
        transfer.body += chunk;
    }
    if (static_cast<qint64>(transfer.body.size()) > limit ||
        reply->bytesAvailable() > 0) {
        transfer.tooLarge = true;
        transfer.body.clear();
        reply->abort();
        return false;
    }
    return true;
}

void FrameDefaultCatalogService::discardReply(Transfer &transfer, QObject *owner,
                                              bool abortReply)
{
    QNetworkReply *reply = transfer.reply.data();
    transfer.reply.clear();
    if (!reply) {
        return;
    }
    QObject::disconnect(reply, nullptr, owner, nullptr);
    if (abortReply) {
        reply->abort();
        reply->deleteLater();
    }
    // Otherwise the reply belongs to a manager that is being destroyed and
    // will be deleted with it.
}

void FrameDefaultCatalogService::failAll(const QString &catalogError,
                                         const QString &downloadError,
                                         bool cancelled, bool abortReplies)
{
    // Detach everything before emitting anything: a slot may start new
    // operations and they must not be swept up by this failure.
    std::unique_ptr<CatalogWalk> walk = std::move(m_catalog);
    std::unique_ptr<Download> download = std::move(m_download);
    m_catalogWatchdog.stop();
    m_downloadWatchdog.stop();
    if (walk) {
        discardReply(*walk, this, abortReplies);
    }
    if (download) {
        discardReply(*download, this, abortReplies);
    }
    if (walk) {
        emit catalogFailed(catalogError, cancelled);
    }
    if (download) {
        emit downloadFailed(download->catalogPath, downloadError, cancelled);
    }
}

void FrameDefaultCatalogService::injectedManagerDestroyed()
{
    // QObject::destroyed is emitted before the manager deletes its child
    // replies, so detach without touching them.
    failAll(tr("The network access manager was destroyed during the "
               "frame-default list request."),
            tr("The network access manager was destroyed during the "
               "frame-default download."),
            false, false);
}

// ---------------------------------------------------------------------------
// catalog walk

bool FrameDefaultCatalogService::requestCatalog(bool forceRefresh, QString *error,
                                                bool *started,
                                                OperationId *operationId)
{
    if (started) {
        *started = false;
    }
    if (operationId) {
        *operationId = InvalidOperationId;
    }
    if (m_catalog) {
        if (operationId) {
            *operationId = m_catalog->generation;
        }
        return true; // join the walk that is already running
    }
    if (!forceRefresh && m_hasCachedCatalog) {
        emit catalogReady(m_cachedCatalog, true);
        return true;
    }

    auto walk = std::make_unique<CatalogWalk>();
    walk->generation = m_nextGeneration++;
    const OperationId newOperationId = walk->generation;
    walk->pending << CatalogRoot;
    walk->discovered.insert(CatalogRoot.toCaseFolded());
    m_catalog = std::move(walk);

    QString startError;
    if (!startNextDirectory(&startError)) {
        m_catalog.reset();
        if (error) {
            *error = startError;
        }
        return false;
    }
    if (started) {
        *started = true;
    }
    if (operationId) {
        *operationId = newOperationId;
    }
    return true;
}

bool FrameDefaultCatalogService::isCatalogLoading() const
{
    return m_catalog != nullptr;
}

bool FrameDefaultCatalogService::hasCachedCatalog() const
{
    return m_hasCachedCatalog;
}

QVector<FrameDefaultCatalogItem> FrameDefaultCatalogService::cachedCatalog() const
{
    return m_cachedCatalog;
}

bool FrameDefaultCatalogService::startNextDirectory(QString *startError)
{
    CatalogWalk &walk = *m_catalog;
    while (!walk.pending.isEmpty()) {
        const QString directory = walk.pending.takeFirst();
        const QString key = directory.toCaseFolded();
        if (walk.visited.contains(key)) {
            continue;
        }
        walk.visited.insert(key);
        if (++walk.directoriesRequested > m_maxDirectories) {
            failCatalog(tr("The frame-default catalog walk exceeded %1 directories.")
                            .arg(m_maxDirectories));
            return true;
        }
        walk.currentDirectory = directory;
        const QUrl url(m_apiRoot.toString(QUrl::FullyEncoded) + EscapePath(directory));
        return startGet(url, walk.generation, m_catalogWatchdog, m_maxListingBytes,
                        &FrameDefaultCatalogService::catalogReplyFinished,
                        startError);
    }
    finishCatalog(deduplicateAndSort(walk.files));
    return true;
}

void FrameDefaultCatalogService::catalogReplyFinished(OperationId generation)
{
    if (!m_catalog || m_catalog->generation != generation) {
        return; // late reply from a cancelled walk
    }
    CatalogWalk &walk = *m_catalog;
    m_catalogWatchdog.stop();
    QNetworkReply *reply = walk.reply.data();
    walk.reply.clear();
    if (!reply) {
        failCatalog(tr("The GitHub listing reply disappeared."));
        return;
    }
    reply->deleteLater();

    const QString what = tr("GitHub listing of %1").arg(walk.currentDirectory);
    if (walk.tooLarge) {
        failCatalog(tr("%1 exceeds the %2-byte listing limit.")
                        .arg(what)
                        .arg(m_maxListingBytes));
        return;
    }
    if (walk.timedOut) {
        failCatalog(tr("%1 timed out after %2 ms without data.")
                        .arg(what)
                        .arg(m_transferTimeoutMs));
        return;
    }
    const QString failure = replyFailure(what, reply);
    if (!failure.isEmpty()) {
        failCatalog(failure);
        return;
    }
    if (!drainReply(walk, reply, m_maxListingBytes)) { // final bounded drain
        failCatalog(tr("%1 exceeds the %2-byte listing limit.")
                        .arg(what)
                        .arg(m_maxListingBytes));
        return;
    }

    QStringList directories;
    QVector<FrameDefaultCatalogItem> files;
    QString parseError;
    if (!parseListing(walk.body, &walk.discovered, m_maxDirectories,
                      &directories, &files, &parseError)) {
        failCatalog(parseError);
        return;
    }
    walk.body.clear();
    walk.pending += directories;
    walk.files += files;
    if (walk.files.size() > m_maxEntries) {
        failCatalog(tr("The frame-default catalog exceeds %1 entries.")
                        .arg(m_maxEntries));
        return;
    }

    QString startError;
    if (!startNextDirectory(&startError)) {
        failCatalog(startError);
    }
}

bool FrameDefaultCatalogService::parseListing(
    const QByteArray &body, QSet<QString> *discoveredDirectories,
    int maxDirectories, QStringList *directories,
    QVector<FrameDefaultCatalogItem> *files, QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = tr("GitHub returned an invalid frame-default listing: %1")
                         .arg(parseError.errorString());
        }
        return false;
    }
    if (!document.isArray()) {
        if (error) {
            *error = tr("GitHub returned an invalid frame-default listing.");
        }
        return false;
    }

    const QJsonArray entries = document.array();
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        QString pathError;
        const QString path = NormalizeEntryPath(
            entry.value(QLatin1String("path")).toString(), &pathError);
        if (path.isEmpty()) {
            if (error) {
                *error = pathError;
            }
            return false;
        }
        const QString type = entry.value(QLatin1String("type")).toString();
        if (type.compare(QLatin1String("dir"), Qt::CaseInsensitive) == 0) {
            const QString key = path.toCaseFolded();
            if (discoveredDirectories->contains(key)) {
                continue;
            }
            if (discoveredDirectories->size() >= maxDirectories) {
                if (error) {
                    *error = tr("The frame-default catalog walk exceeded %1 directories.")
                                 .arg(maxDirectories);
                }
                return false;
            }
            discoveredDirectories->insert(key);
            directories->append(path);
        } else if (type.compare(QLatin1String("file"), Qt::CaseInsensitive) == 0 &&
                   path.endsWith(QLatin1String(".param"), Qt::CaseInsensitive)) {
            QString name = entry.value(QLatin1String("name")).toString();
            if (name.trimmed().isEmpty()) {
                name = path.section(QLatin1Char('/'), -1);
            }
            FrameDefaultCatalogItem item;
            item.name = name;
            item.path = path;
            files->append(item);
        }
    }
    return true;
}

QVector<FrameDefaultCatalogItem> FrameDefaultCatalogService::deduplicateAndSort(
    const QVector<FrameDefaultCatalogItem> &files)
{
    QVector<FrameDefaultCatalogItem> unique;
    QSet<QString> seen;
    unique.reserve(files.size());
    for (const FrameDefaultCatalogItem &file : files) {
        const QString key = file.path.toCaseFolded();
        if (seen.contains(key)) {
            continue; // first occurrence wins, like GroupBy().First()
        }
        seen.insert(key);
        unique.append(file);
    }
    std::stable_sort(unique.begin(), unique.end(), catalogPathLess);
    return unique;
}

void FrameDefaultCatalogService::finishCatalog(
    const QVector<FrameDefaultCatalogItem> &items)
{
    m_catalogWatchdog.stop();
    m_catalog.reset();
    m_cachedCatalog = items;
    m_hasCachedCatalog = true;
    emit catalogReady(items, false);
}

void FrameDefaultCatalogService::failCatalog(const QString &error, bool cancelled)
{
    m_catalogWatchdog.stop();
    if (m_catalog) {
        std::unique_ptr<CatalogWalk> walk = std::move(m_catalog);
        discardReply(*walk, this, true);
    }
    emit catalogFailed(error, cancelled);
}

// ---------------------------------------------------------------------------
// download

bool FrameDefaultCatalogService::download(const QString &catalogPath, QString *error,
                                          OperationId *operationId)
{
    if (operationId) {
        *operationId = InvalidOperationId;
    }
    QString normalizeError;
    const QString normalized = NormalizeParamPath(catalogPath, &normalizeError);
    if (normalized.isEmpty()) {
        if (error) {
            *error = normalizeError;
        }
        return false;
    }
    if (m_download) {
        if (error) {
            *error = tr("A frame-default download is already running.");
        }
        return false;
    }

    auto download = std::make_unique<Download>();
    download->generation = m_nextGeneration++;
    const OperationId newOperationId = download->generation;
    download->catalogPath = normalized;
    m_download = std::move(download);

    QString startError;
    const QUrl url(m_rawRoot.toString(QUrl::FullyEncoded) + EscapePath(normalized));
    if (!startGet(url, m_download->generation, m_downloadWatchdog,
                  m_maxDownloadBytes,
                  &FrameDefaultCatalogService::downloadReplyFinished,
                  &startError)) {
        m_download.reset();
        if (error) {
            *error = startError;
        }
        return false;
    }
    if (operationId) {
        *operationId = newOperationId;
    }
    return true;
}

bool FrameDefaultCatalogService::isDownloading() const
{
    return m_download != nullptr;
}

void FrameDefaultCatalogService::downloadReplyFinished(OperationId generation)
{
    if (!m_download || m_download->generation != generation) {
        return; // late reply from a cancelled download
    }
    Download &download = *m_download;
    m_downloadWatchdog.stop();
    const QString catalogPath = download.catalogPath;
    QNetworkReply *reply = download.reply.data();
    download.reply.clear();
    if (!reply) {
        failDownload(catalogPath, tr("The download reply disappeared."));
        return;
    }
    reply->deleteLater();

    const QString what = tr("Download of %1").arg(catalogPath);
    if (download.tooLarge) {
        failDownload(catalogPath, tr("%1 exceeds the %2-byte download limit.")
                                      .arg(what)
                                      .arg(m_maxDownloadBytes));
        return;
    }
    if (download.timedOut) {
        failDownload(catalogPath, tr("%1 timed out after %2 ms without data.")
                                      .arg(what)
                                      .arg(m_transferTimeoutMs));
        return;
    }
    const QString failure = replyFailure(what, reply);
    if (!failure.isEmpty()) {
        failDownload(catalogPath, failure);
        return;
    }
    if (!drainReply(download, reply, m_maxDownloadBytes)) { // final bounded drain
        failDownload(catalogPath, tr("%1 exceeds the %2-byte download limit.")
                                      .arg(what)
                                      .arg(m_maxDownloadBytes));
        return;
    }

    const QByteArray bytes = download.body;
    m_download.reset();
    emit downloadFinished(catalogPath, bytes);
}

void FrameDefaultCatalogService::failDownload(const QString &catalogPath,
                                              const QString &error, bool cancelled)
{
    m_downloadWatchdog.stop();
    if (m_download) {
        std::unique_ptr<Download> download = std::move(m_download);
        discardReply(*download, this, true);
    }
    emit downloadFailed(catalogPath, error, cancelled);
}

// ---------------------------------------------------------------------------
// cancellation

bool FrameDefaultCatalogService::cancelCatalog(OperationId operationId)
{
    if (operationId == InvalidOperationId || !m_catalog ||
        m_catalog->generation != operationId) {
        return false;
    }
    failCatalog(tr("Frame-default list request cancelled."), true);
    return true;
}

bool FrameDefaultCatalogService::cancelDownload(OperationId operationId)
{
    if (operationId == InvalidOperationId || !m_download ||
        m_download->generation != operationId) {
        return false;
    }
    const QString catalogPath = m_download->catalogPath;
    failDownload(catalogPath, tr("Frame-default download cancelled."), true);
    return true;
}

void FrameDefaultCatalogService::cancel()
{
    failAll(tr("Frame-default list request cancelled."),
            tr("Frame-default download cancelled."), true, true);
}
