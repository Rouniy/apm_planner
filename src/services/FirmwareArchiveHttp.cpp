#include "FirmwareArchiveHttp.h"
#include "FirmwareArchiveManifest.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <exception>

namespace {
FirmwareArchive::FetchResult download(QUrl url, qint64 maximumBytes,
    bool httpsOnly, FirmwareArchive::Cancel cancel,
    FirmwareArchive::ChunkSink sink, int timeoutMs)
{
    using namespace FirmwareArchive;
    FetchResult result;
    const auto fail = [&](Failure failure, const QString &error) {
        if (result.success()) { result.failure = failure; result.error = error; }
    };
    const auto interrupted = [&] {
        if (!result.success()) return true;
        try {
            if (cancel && cancel())
                fail(Failure::Cancelled, QStringLiteral("Download cancelled."));
        } catch (...) {
            fail(Failure::LocalIo, QStringLiteral("Download cancellation callback failed."));
        }
        return !result.success();
    };
    if (!sink || maximumBytes < 0 || timeoutMs <= 0) {
        fail(Failure::Policy, QStringLiteral("Invalid firmware download contract."));
        return result;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    QNetworkAccessManager manager;
    for (int redirect = 0; redirect <= 8; ++redirect) {
        if (!FirmwareArchiveManifest::allowedUrl(url, httpsOnly)) {
            fail(Failure::Policy, QStringLiteral("Refused download URL or insecure redirect."));
            return result;
        }
        if (interrupted()) return result;
        if (elapsed.elapsed() >= timeoutMs) {
            fail(Failure::Network, QStringLiteral("Firmware HTTP request timed out."));
            return result;
        }
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        request.setRawHeader("User-Agent", "APMPlanner/FirmwareArchive");
        request.setRawHeader("Accept-Encoding", "identity");
        QNetworkReply *reply = manager.get(request);
        reply->setReadBufferSize(128 * 1024);
        QEventLoop loop;
        QTimer timer;
        timer.setInterval(20);
        const auto abort = [&] {
            if (reply->isRunning()) reply->abort();
            loop.quit();
        };
        const auto checkHeaders = [&] {
            const QVariant header = reply->header(QNetworkRequest::ContentLengthHeader);
            bool ok = false;
            const qint64 declared = header.toLongLong(&ok);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300) {
                // Do not wait for an unbounded error/redirect body. Headers
                // suffice to follow or report it after this signal returns.
                abort();
                return;
            }
            if (status >= 200 && status < 300 && ok && declared > maximumBytes) {
                fail(Failure::Limit, QStringLiteral("HTTP response exceeds the %1-byte limit.")
                     .arg(maximumBytes));
                abort();
            }
        };
        const auto consume = [&] {
            if (!result.success()) return;
            checkHeaders();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            // Redirect/error bodies never reach the archive or consume its byte budget.
            if (!result.success() || status < 200 || status >= 300) return;
            while (reply->bytesAvailable() > 0 && result.success()) {
                if (interrupted()) {
                    abort();
                    return;
                }
                const QByteArray bytes = reply->read(64 * 1024);
                if (bytes.isEmpty()) break;
                if (bytes.size() > maximumBytes - result.bytes) {
                    fail(Failure::Limit, QStringLiteral("HTTP response exceeds the %1-byte limit.")
                         .arg(maximumBytes));
                    abort();
                    return;
                }
                bool written = false;
                try { written = sink(bytes); } catch (...) {}
                if (!written) {
                    fail(Failure::LocalIo, QStringLiteral("Writing downloaded bytes failed."));
                    abort();
                    return;
                }
                result.bytes += bytes.size();
            }
        };
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, checkHeaders);
        QObject::connect(reply, &QIODevice::readyRead, &loop, consume);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
            if (interrupted()) {
                abort();
            } else if (elapsed.elapsed() >= timeoutMs) {
                fail(Failure::Network, QStringLiteral("Firmware HTTP request timed out."));
                abort();
            }
        });
        timer.start();
        if (!reply->isFinished()) loop.exec();
        timer.stop();
        consume();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QUrl redirectTarget = reply->attribute(
            QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const auto networkError = reply->error();
        const QString networkErrorText = reply->errorString();
        const QString statusReason = reply->attribute(
            QNetworkRequest::HttpReasonPhraseAttribute).toString();
        const QVariant lengthHeader = reply->header(QNetworkRequest::ContentLengthHeader);
        bool lengthKnown = false;
        const qint64 declared = lengthHeader.toLongLong(&lengthKnown);
        delete reply; // no outstanding event callback; manager outlives the reply
        if (!result.success()) return result;
        if (interrupted()) return result;
        if (status >= 300 && status < 400 && !redirectTarget.isEmpty()) {
            if (redirect == 8) {
                fail(Failure::Policy, QStringLiteral("Too many firmware HTTP redirects."));
                return result;
            }
            url = url.resolved(redirectTarget);
            continue;
        }
        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            fail(Failure::Network, QStringLiteral("HTTP %1: %2").arg(status).arg(
                status >= 300 ? (statusReason.isEmpty()
                    ? QStringLiteral("Server rejected the request.") : statusReason)
                              : networkErrorText));
        } else if (lengthKnown && declared >= 0 && result.bytes != declared) {
            fail(Failure::Network, QStringLiteral("Incomplete HTTP response: expected %1, received %2 bytes.")
                 .arg(declared).arg(result.bytes));
        }
        return result;
    }
    fail(Failure::Policy, QStringLiteral("Too many firmware HTTP redirects."));
    return result;
}
}

FirmwareArchive::Fetch FirmwareArchiveHttp::transport(int timeoutMs)
{
    return [timeoutMs](const QUrl &url, qint64 maximumBytes, bool httpsOnly,
                       const FirmwareArchive::Cancel &cancel,
                       const FirmwareArchive::ChunkSink &sink) {
        try {
            return download(url, maximumBytes, httpsOnly, cancel, sink, timeoutMs);
        } catch (const std::exception &error) {
            return FirmwareArchive::FetchResult{FirmwareArchive::Failure::LocalIo,
                QStringLiteral("Download callback failed: %1").arg(QString::fromUtf8(error.what())), 0};
        } catch (...) {
            return FirmwareArchive::FetchResult{FirmwareArchive::Failure::LocalIo,
                QStringLiteral("Download callback failed unexpectedly."), 0};
        }
    };
}
