#include "ParameterMetaDataUpdater.h"

#include <QDateTime>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace {
constexpr qint64 kMaximumDownloadBytes = 16 * 1024 * 1024;
constexpr qint64 kLatestCacheMaximumAgeSeconds = 7 * 24 * 60 * 60;
constexpr int kRequestTimeoutMilliseconds = 30000;
constexpr int kExactFailureBackoffSeconds = 30 * 60;

QUrl latestUrl(const ParameterMetaDataSource &source)
{
    return QUrl(QStringLiteral(
        "https://autotest.ardupilot.org/Parameters/%1/apm.pdef.xml")
                    .arg(source.latestVehicleName));
}

QUrl versionedUrl(const ParameterMetaDataSource &source,
                  const QString &firmwareVersion)
{
    return QUrl(QStringLiteral(
        "https://autotest.ardupilot.org/Parameters/versioned/%1/"
        "stable-%2/apm.pdef.xml")
                    .arg(source.versionedVehicleName, firmwareVersion));
}
}

ParameterMetaDataUpdater::ParameterMetaDataUpdater(
    ParameterMetaDataRepository *repository, QObject *parent)
    : ParameterMetaDataUpdater(repository, QUrl(), parent)
{
}

ParameterMetaDataUpdater::ParameterMetaDataUpdater(
    ParameterMetaDataRepository *repository,
    const QUrl &loopbackEndpointOverride, QObject *parent)
    : QObject(parent),
      m_repository(repository),
      m_networkAccessManager(this)
{
    const QString host = loopbackEndpointOverride.host();
    if (loopbackEndpointOverride.isValid()
        && (host == QLatin1String("127.0.0.1")
            || host == QLatin1String("localhost")
            || host == QLatin1String("::1"))) {
        m_loopbackEndpointOverride = loopbackEndpointOverride;
    }
}

ParameterMetaDataUpdater::~ParameterMetaDataUpdater()
{
    QList<QPointer<QNetworkReply>> replies;
    for (const UpdateState &state : m_updates) {
        replies.append(state.reply);
    }
    m_updates.clear();
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply) {
            disconnect(reply, nullptr, this, nullptr);
            reply->abort();
        }
    }
}

QList<QUrl> ParameterMetaDataUpdater::candidateUrls(
    ParameterFirmwareFamily family, const QString &firmwareVersion,
    bool developmentFirmware)
{
    QList<QUrl> result;
    const ParameterMetaDataSource source =
        ParameterMetaDataRepository::sourceForFamily(family);
    const QString normalized =
        ParameterMetaDataRepository::normalizedVersion(firmwareVersion);
    if (!source.isValid() || source.latestVehicleName.isEmpty()) {
        return result;
    }
    if (!developmentFirmware && !firmwareVersion.trimmed().isEmpty()
        && normalized.isEmpty()) {
        return result;
    }
    if (!developmentFirmware && !normalized.isEmpty()
        && !source.versionedVehicleName.isEmpty()) {
        result.append(versionedUrl(source, normalized));
    }
    result.append(latestUrl(source));
    return result;
}

void ParameterMetaDataUpdater::requestUpdate(
    ParameterFirmwareFamily family, const QString &firmwareVersion,
    bool developmentFirmware)
{
    if (!m_repository) {
        return;
    }

    const QString normalized =
        ParameterMetaDataRepository::normalizedVersion(firmwareVersion);
    const QList<QUrl> urls = candidateUrls(
        family, firmwareVersion, developmentFirmware);
    if (urls.isEmpty()) {
        return;
    }

    QList<QPointer<QNetworkReply>> supersededReplies;
    for (auto iterator = m_updates.begin(); iterator != m_updates.end();) {
        if (iterator.key() == family) {
            ++iterator;
            continue;
        }
        supersededReplies.append(iterator->reply);
        iterator = m_updates.erase(iterator);
    }
    for (const QPointer<QNetworkReply> &reply : supersededReplies) {
        if (reply) {
            reply->abort();
        }
    }

    const QString requestKey = QStringLiteral("%1|%2")
        .arg(firmwareVersion.trimmed(),
             developmentFirmware ? QStringLiteral("nonofficial")
                                 : QStringLiteral("official"));
    auto existing = m_updates.find(family);
    if (existing != m_updates.end()) {
        if (existing->requestKey == requestKey) {
            return;
        }
        const QPointer<QNetworkReply> oldReply = existing->reply;
        m_updates.erase(existing);
        if (oldReply) {
            oldReply->abort();
        }
    }

    UpdateState state;
    state.requestKey = requestKey;
    int urlIndex = 0;
    if (!developmentFirmware && !normalized.isEmpty()) {
        if (m_repository->cachedCatalogIsFresh(
                family, normalized, kLatestCacheMaximumAgeSeconds)) {
            return;
        }
        const QString exactKey = QString::number(static_cast<int>(family))
            + QLatin1Char('|') + normalized;
        const QDateTime failedAt = m_failedExactAttempts.value(exactKey);
        if (!failedAt.isValid()
            || failedAt.secsTo(QDateTime::currentDateTimeUtc())
                >= kExactFailureBackoffSeconds) {
            state.candidates.append({requestUrl(urls.at(urlIndex)),
                                     urls.at(urlIndex), normalized});
        }
        ++urlIndex;
    }
    if (!m_repository->cachedCatalogIsFresh(
            family, QString(), kLatestCacheMaximumAgeSeconds)
        && urlIndex < urls.size()) {
        state.candidates.append({requestUrl(urls.at(urlIndex)),
                                 urls.at(urlIndex), QString()});
    }
    if (state.candidates.isEmpty()) {
        return;
    }

    m_updates.insert(family, state);
    startNext(family);
}

void ParameterMetaDataUpdater::cancel()
{
    QList<QPointer<QNetworkReply>> replies;
    for (const UpdateState &state : m_updates) {
        replies.append(state.reply);
    }
    m_updates.clear();
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply) {
            reply->abort();
        }
    }
}

void ParameterMetaDataUpdater::startNext(ParameterFirmwareFamily family)
{
    auto iterator = m_updates.find(family);
    if (iterator == m_updates.end()) {
        return;
    }
    ++iterator->candidateIndex;
    if (iterator->candidateIndex >= iterator->candidates.size()) {
        const QString reason = iterator->errors.join(QStringLiteral("; "));
        m_updates.erase(iterator);
        emit updateFailed(family, reason);
        return;
    }

    const QUrl url = iterator->candidates.at(
        iterator->candidateIndex).requestUrl;
    iterator->payload.clear();
    iterator->payloadTooLarge = false;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "APM-Planner-3.0");
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
#else
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
    QNetworkReply *reply = m_networkAccessManager.get(request);
    reply->setReadBufferSize(64 * 1024);
    iterator->reply = reply;
    connect(reply, &QIODevice::readyRead, this,
            [this, family, reply]() {
        auto state = m_updates.find(family);
        if (state == m_updates.end() || state->reply != reply) {
            return;
        }
        const qint64 remaining = kMaximumDownloadBytes
            - state->payload.size();
        if (remaining <= 0 || reply->bytesAvailable() > remaining) {
            state->payloadTooLarge = true;
            reply->abort();
            return;
        }
        state->payload.append(reply->read(remaining));
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, family, reply](qint64 received, qint64 total) {
        if (received > kMaximumDownloadBytes
            || total > kMaximumDownloadBytes) {
            auto state = m_updates.find(family);
            if (state != m_updates.end() && state->reply == reply) {
                state->payloadTooLarge = true;
            }
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, family, reply]() {
        requestFinished(family, reply);
    });
    QTimer::singleShot(kRequestTimeoutMilliseconds, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
}

void ParameterMetaDataUpdater::requestFinished(
    ParameterFirmwareFamily family, QNetworkReply *reply)
{
    auto iterator = m_updates.find(family);
    if (iterator == m_updates.end() || iterator->reply != reply) {
        reply->deleteLater();
        return;
    }

    const Candidate candidate = iterator->candidates.at(
        iterator->candidateIndex);
    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!iterator->payloadTooLarge && reply->bytesAvailable() > 0) {
        const qint64 remaining = kMaximumDownloadBytes
            - iterator->payload.size();
        if (remaining <= 0 || reply->bytesAvailable() > remaining) {
            iterator->payloadTooLarge = true;
        } else {
            iterator->payload.append(reply->read(remaining));
        }
    }
    const QByteArray xml = iterator->payload;
    QString failure;
    if (iterator->payloadTooLarge) {
        failure = QStringLiteral("%1: payload exceeds 16 MiB")
                      .arg(candidate.requestUrl.toString());
    } else if (reply->error() != QNetworkReply::NoError) {
        failure = QStringLiteral("%1: %2")
                      .arg(candidate.requestUrl.toString(),
                           reply->errorString());
    } else if (httpStatus >= 400) {
        failure = QStringLiteral("%1: HTTP %2")
                      .arg(candidate.requestUrl.toString())
                      .arg(httpStatus);
    } else if (xml.isEmpty() || xml.size() > kMaximumDownloadBytes) {
        failure = QStringLiteral("%1: invalid payload size")
                      .arg(candidate.requestUrl.toString());
    } else if (reply->url() != candidate.requestUrl) {
        failure = QStringLiteral("%1: redirected to unexpected URL %2")
                      .arg(candidate.requestUrl.toString(),
                           reply->url().toString());
    } else {
        ParameterMetaDataCacheInfo cacheInfo;
        cacheInfo.sourceUrl = candidate.sourceUrl.toString();
        cacheInfo.etag = QString::fromLatin1(reply->rawHeader("ETag"));
        cacheInfo.lastModified = QString::fromLatin1(
            reply->rawHeader("Last-Modified"));
        cacheInfo.fetchedAtUtc = QDateTime::currentDateTimeUtc();
        if (m_repository->installCatalog(
                family, candidate.cacheVersion, xml, cacheInfo, &failure)) {
            if (!candidate.cacheVersion.isEmpty()) {
                const QString exactKey =
                    QString::number(static_cast<int>(family))
                    + QLatin1Char('|') + candidate.cacheVersion;
                m_failedExactAttempts.remove(exactKey);
            }
            reply->deleteLater();
            m_updates.erase(iterator);
            emit catalogUpdated(family, candidate.cacheVersion);
            return;
        }
        failure = QStringLiteral("%1: %2")
                      .arg(candidate.requestUrl.toString(), failure);
    }

    if (!candidate.cacheVersion.isEmpty()) {
        const QString exactKey = QString::number(static_cast<int>(family))
            + QLatin1Char('|') + candidate.cacheVersion;
        m_failedExactAttempts.insert(exactKey,
                                     QDateTime::currentDateTimeUtc());
    }
    iterator->errors.append(failure);
    iterator->reply = nullptr;
    reply->deleteLater();
    startNext(family);
}

QUrl ParameterMetaDataUpdater::requestUrl(const QUrl &sourceUrl) const
{
    if (!m_loopbackEndpointOverride.isValid()) {
        return sourceUrl;
    }
    QUrl result = m_loopbackEndpointOverride;
    result.setPath(sourceUrl.path());
    result.setQuery(sourceUrl.query());
    return result;
}
