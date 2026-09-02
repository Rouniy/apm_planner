/*===================================================================
APM_PLANNER Open Source Ground Control Station

This file is part of the APM_PLANNER project and is distributed under
the terms of the GNU General Public License version 3 or later.
======================================================================*/

#include "AutoUpdateCheck.h"

#include "configuration.h"
#include "logging.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVersionNumber>

#define APM_STRINGIFY_DETAIL(value) #value
#define APM_STRINGIFY(value) APM_STRINGIFY_DETAIL(value)

namespace {
struct ParsedVersion
{
    QVersionNumber number;
    bool releaseCandidate = false;
    int releaseCandidateNumber = 0;
    bool valid = false;
};

ParsedVersion parseVersion(const QString &value)
{
    static const QRegularExpression expression(QStringLiteral(
        "^\\s*(\\d+)\\.(\\d+)(?:\\.(\\d+))?(?:-?rc(\\d+))?\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(value);
    if (!match.hasMatch()) {
        return {};
    }

    ParsedVersion result;
    result.number = QVersionNumber(match.captured(1).toInt(),
                                    match.captured(2).toInt(),
                                    match.captured(3).isEmpty()
                                        ? 0 : match.captured(3).toInt());
    result.releaseCandidate = !match.captured(4).isEmpty();
    result.releaseCandidateNumber = match.captured(4).toInt();
    result.valid = true;
    return result;
}

bool isValidReleaseObject(const QJsonObject &release)
{
    const QString urlText = release.value(QStringLiteral("url")).toString().trimmed();
    const QString name = release.value(QStringLiteral("name")).toString().trimmed();
    const QUrl url(urlText);
    return !release.value(QStringLiteral("platform")).toString().trimmed().isEmpty()
        && !release.value(QStringLiteral("type")).toString().trimmed().isEmpty()
        && !release.value(QStringLiteral("version")).toString().trimmed().isEmpty()
        && url.isValid()
        && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && !url.host().isEmpty()
        && !name.isEmpty()
        && name != QStringLiteral(".")
        && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/'))
        && !name.contains(QLatin1Char('\\'))
        && QFileInfo(name).fileName() == name;
}
}

AutoUpdateCheck::AutoUpdateCheck(QObject *parent)
    : QObject(parent),
      m_networkAccessManager(new QNetworkAccessManager(this)),
      m_timeoutTimer(new QTimer(this))
{
    qRegisterMetaType<AutoUpdateCheck::ReleaseChannel>();
    qRegisterMetaType<AutoUpdateCheck::Presentation>();
    m_timeoutTimer->setSingleShot(true);
    m_timeoutTimer->setInterval(60000);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        if (!m_networkReplyPtr) {
            return;
        }
        QNetworkReply *reply = m_networkReplyPtr.take();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
        finishFailure(tr("The update service timed out."));
    });
    loadSettings();
}

AutoUpdateCheck::~AutoUpdateCheck() = default;

bool AutoUpdateCheck::checkForUpdates(ReleaseChannel channel,
                                      Presentation presentation,
                                      const QUrl &manifestUrl)
{
    if (isChecking()) {
        return false;
    }

    QUrl url = manifestUrl;
    if (url.isEmpty()) {
        url = QUrl(QStringLiteral(APM_UPDATE_MANIFEST_URL));
    }
    const bool supportedScheme = url.scheme().compare(
        QStringLiteral("https"), Qt::CaseInsensitive) == 0
        || url.isLocalFile();
    if (!url.isValid() || !supportedScheme) {
        return false;
    }

    m_activeChannel = channel;
    m_activePresentation = presentation;
    m_activeSuppressNoUpdateSignal = m_suppressNextNoUpdateSignal;
    m_suppressNextNoUpdateSignal = false;
    m_redirectCount = 0;
    m_checking = true;

    emit checkingChanged(true);
    emit checkStarted(channel, presentation);
    m_timeoutTimer->start();
    startRequest(url);
    return true;
}

bool AutoUpdateCheck::isChecking() const
{
    return m_checking;
}

AutoUpdateCheck::ReleaseChannel AutoUpdateCheck::configuredReleaseChannel() const
{
    return m_releaseChannel;
}

void AutoUpdateCheck::setConfiguredReleaseChannel(ReleaseChannel channel)
{
    if (m_releaseChannel == channel) {
        return;
    }
    m_releaseChannel = channel;
    writeSettings();
}

QString AutoUpdateCheck::skippedVersion(ReleaseChannel channel) const
{
    return channel == Beta ? m_betaSkipVersion : m_stableSkipVersion;
}

void AutoUpdateCheck::setSkippedVersion(ReleaseChannel channel,
                                        const QString &version)
{
    if (channel == Beta) {
        m_betaSkipVersion = version;
    } else {
        m_stableSkipVersion = version;
    }
    writeSettings();
}

QString AutoUpdateCheck::releaseChannelName(ReleaseChannel channel)
{
    return channel == Beta ? QStringLiteral("beta") : QStringLiteral("stable");
}

AutoUpdateCheck::ReleaseChannel AutoUpdateCheck::releaseChannelFromString(
    const QString &value)
{
    return value.trimmed().compare(QStringLiteral("beta"), Qt::CaseInsensitive) == 0
        ? Beta : Stable;
}

bool AutoUpdateCheck::isVersionNewer(const QString &candidate,
                                     const QString &current)
{
    const ParsedVersion candidateVersion = parseVersion(candidate);
    const ParsedVersion currentVersion = parseVersion(current);
    if (!candidateVersion.valid || !currentVersion.valid) {
        return false;
    }

    const int numericComparison = QVersionNumber::compare(
        candidateVersion.number, currentVersion.number);
    if (numericComparison != 0) {
        return numericComparison > 0;
    }

    if (candidateVersion.releaseCandidate != currentVersion.releaseCandidate) {
        // A final release is newer than an RC with the same numeric version.
        return !candidateVersion.releaseCandidate;
    }
    return candidateVersion.releaseCandidate
        && candidateVersion.releaseCandidateNumber
               > currentVersion.releaseCandidateNumber;
}

AutoUpdateCheck::UpdateSelection AutoUpdateCheck::selectUpdate(
    const QByteArray &manifest,
    const QString &platform,
    ReleaseChannel channel,
    const QString &currentVersion,
    const QString &skippedVersion)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifest, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        UpdateSelection invalid;
        invalid.status = UpdateSelection::InvalidManifest;
        invalid.error = parseError.error == QJsonParseError::NoError
            ? tr("The update manifest root is not an object.")
            : tr("Invalid update manifest: %1").arg(parseError.errorString());
        return invalid;
    }

    const QJsonValue releasesValue = document.object().value(
        QStringLiteral("releases"));
    if (!releasesValue.isArray()) {
        UpdateSelection invalid;
        invalid.status = UpdateSelection::InvalidManifest;
        invalid.error = tr("The update manifest has no releases array.");
        return invalid;
    }

    UpdateSelection best;
    const QString expectedType = releaseChannelName(channel);
    for (const QJsonValue &value : releasesValue.toArray()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject release = value.toObject();
        if (!isValidReleaseObject(release)) {
            continue;
        }

        const QString releasePlatform = release.value(
            QStringLiteral("platform")).toString().trimmed();
        const QString releaseType = release.value(
            QStringLiteral("type")).toString().trimmed().toLower();
        const QString version = release.value(
            QStringLiteral("version")).toString().trimmed();
        if (releasePlatform.compare(platform, Qt::CaseInsensitive) != 0
            || releaseType != expectedType
            || !isVersionNewer(version, currentVersion)) {
            continue;
        }
        if (best.status == UpdateSelection::Available
            && !isVersionNewer(version, best.version)) {
            continue;
        }

        best.status = UpdateSelection::Available;
        best.version = version;
        best.releaseType = releaseType;
        best.url = release.value(QStringLiteral("url")).toString().trimmed();
        best.name = release.value(QStringLiteral("name")).toString().trimmed();
    }

    if (best.status == UpdateSelection::Available
        && !skippedVersion.isEmpty()
        && best.version.compare(skippedVersion, Qt::CaseInsensitive) == 0) {
        return {};
    }
    return best;
}

void AutoUpdateCheck::forcedAutoUpdateCheck()
{
    loadSettings();
    checkForUpdates(m_releaseChannel, Modal);
}

void AutoUpdateCheck::autoUpdateCheck()
{
    loadSettings();
    checkForUpdates(m_releaseChannel, Background);
}

void AutoUpdateCheck::autoUpdateCheck(const QString &url)
{
    loadSettings();
    checkForUpdates(m_releaseChannel, Background, QUrl(url));
}

void AutoUpdateCheck::startRequest(const QUrl &url)
{
    QLOG_DEBUG() << "Retrieve version object from server:" << url;
    QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
#endif
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("%1/%2 (Qt updater)")
            .arg(QStringLiteral(QGC_APPLICATION_NAME),
                 QStringLiteral(QGC_APPLICATION_VERSION)));
    m_networkReplyPtr.reset(m_networkAccessManager->get(request));
    connect(m_networkReplyPtr.data(), &QNetworkReply::finished,
            this, &AutoUpdateCheck::httpFinished);
    connect(m_networkReplyPtr.data(), &QNetworkReply::downloadProgress,
            this, &AutoUpdateCheck::updateDataReadProgress);
}

void AutoUpdateCheck::cancelDownload()
{
    if (!m_networkReplyPtr) {
        return;
    }
    QNetworkReply *reply = m_networkReplyPtr.take();
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    finishFailure(tr("Update check canceled."));
}

void AutoUpdateCheck::httpFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_networkReplyPtr.data()) {
        return;
    }

    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorString = reply->errorString();
    const QVariant redirect = reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute);

    if (networkError != QNetworkReply::NoError) {
        m_networkReplyPtr.reset();
        finishFailure(networkErrorString);
        return;
    }

    if (!redirect.isNull()) {
        if (++m_redirectCount > 5) {
            m_networkReplyPtr.reset();
            finishFailure(tr("Update check failed: too many redirects."));
            return;
        }
        const QUrl redirectUrl = reply->url().resolved(redirect.toUrl());
        if (!redirectUrl.isValid()
            || redirectUrl.scheme().compare(QStringLiteral("https"),
                                            Qt::CaseInsensitive) != 0
            || redirectUrl.host().isEmpty()) {
            m_networkReplyPtr.reset();
            finishFailure(tr("An update redirect attempted to leave HTTPS."));
            return;
        }
        m_networkReplyPtr.reset();
        startRequest(redirectUrl);
        return;
    }

    const QByteArray manifest = reply->readAll();
    m_networkReplyPtr.reset();
    const QString platform = QString::fromLatin1(APM_STRINGIFY(APP_PLATFORM));
    const QString skipped = m_activePresentation == Background
        ? skippedVersion(m_activeChannel) : QString();
    const UpdateSelection selection = selectUpdate(
        manifest, platform, m_activeChannel,
        QStringLiteral(QGC_APPLICATION_VERSION), skipped);
    if (selection.status == UpdateSelection::InvalidManifest) {
        finishFailure(selection.error);
    } else if (selection.status == UpdateSelection::Available) {
        finishAvailable(selection);
    } else {
        finishNoUpdate();
    }
}

void AutoUpdateCheck::finishAvailable(const UpdateSelection &selection)
{
    const ReleaseChannel channel = m_activeChannel;
    const Presentation presentation = m_activePresentation;
    finishChecking();
    emit checkAvailable(channel, presentation, selection.version,
                        selection.releaseType, selection.url, selection.name);
    if (presentation != Inline) {
        emit updateAvailable(selection.version, selection.releaseType,
                             selection.url, selection.name);
    }
}

void AutoUpdateCheck::finishNoUpdate()
{
    const ReleaseChannel channel = m_activeChannel;
    const Presentation presentation = m_activePresentation;
    const bool suppressLegacy = m_activeSuppressNoUpdateSignal;
    finishChecking();
    emit checkNoUpdate(channel, presentation);
    if (presentation == Modal && !suppressLegacy) {
        emit noUpdateAvailable();
    }
}

void AutoUpdateCheck::finishFailure(const QString &reason)
{
    const ReleaseChannel channel = m_activeChannel;
    const Presentation presentation = m_activePresentation;
    finishChecking();
    emit checkFailed(channel, presentation, reason);
}

void AutoUpdateCheck::finishChecking()
{
    m_timeoutTimer->stop();
    m_checking = false;
    m_activeSuppressNoUpdateSignal = false;
    emit checkingChanged(false);
}

void AutoUpdateCheck::httpReadyRead()
{
}

void AutoUpdateCheck::updateDataReadProgress(qint64 bytesRead, qint64 totalBytes)
{
    QLOG_DEBUG() << "Downloading update manifest:" << bytesRead << "/" << totalBytes;
}

void AutoUpdateCheck::setSkipVersion(const QString &version)
{
    setSkippedVersion(m_activeChannel, version);
}

void AutoUpdateCheck::setAutoUpdateEnabled(bool enabled)
{
    m_isAutoUpdateEnabled = enabled;
    writeSettings();
}

bool AutoUpdateCheck::isUpdateEnabled()
{
    return m_isAutoUpdateEnabled;
}

void AutoUpdateCheck::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("AUTO_UPDATE"));
    m_isAutoUpdateEnabled = settings.value(
        QStringLiteral("ENABLED"), true).toBool();
    m_stableSkipVersion = settings.value(
        QStringLiteral("SKIP_VERSION_STABLE"),
        QStringLiteral("0.0.0")).toString();
    m_betaSkipVersion = settings.value(
        QStringLiteral("SKIP_VERSION_BETA"),
        QStringLiteral("0.0.0")).toString();
    m_releaseChannel = releaseChannelFromString(settings.value(
        QStringLiteral("RELEASE_TYPE"), QStringLiteral(APM_STRINGIFY(APP_TYPE)))
                                                    .toString());
    settings.endGroup();
}

void AutoUpdateCheck::writeSettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("AUTO_UPDATE"));
    settings.setValue(QStringLiteral("ENABLED"), m_isAutoUpdateEnabled);
    settings.setValue(QStringLiteral("SKIP_VERSION_STABLE"),
                      m_stableSkipVersion);
    settings.setValue(QStringLiteral("SKIP_VERSION_BETA"),
                      m_betaSkipVersion);
    settings.setValue(QStringLiteral("RELEASE_TYPE"),
                      releaseChannelName(m_releaseChannel));
    settings.endGroup();
    settings.sync();
}

void AutoUpdateCheck::suppressNoUpdateSignal()
{
    m_suppressNextNoUpdateSignal = true;
}
