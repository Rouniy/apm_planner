#include <QtTest>

#include "ui/configuration/FrameDefaultCatalogService.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cstring>
#include <memory>

namespace {

// Observations that outlive the reply object (the service releases replies
// with deleteLater, which QTRY_* helpers process).
struct ReplyStats
{
    qint64 bytesRead = 0;
    int bodySize = 0;
    bool abortRequested = false; // abort() was called (even after finishing)
    bool aborted = false;        // abort() actually cut the transfer short
    bool finished = false;
};

const QString kApiRoot = QStringLiteral("http://catalog.test/contents/");
const QString kRawRoot = QStringLiteral("http://raw.test/master/");
const QString kUserAgent = QStringLiteral("APMPlanner3/frame-defaults");

// In-memory QNetworkReply: delivers a canned body immediately, in chunks, or
// never (Hang) so limits, cancellation and watchdogs are deterministic.
class FakeReply final : public QNetworkReply
{
public:
    enum Mode { Immediate, Chunked, Hang };

    FakeReply(const QNetworkRequest &request, int status, const QByteArray &body,
              Mode mode, int chunkSize, int chunkDelayMs,
              std::shared_ptr<ReplyStats> stats, QObject *parent)
        : QNetworkReply(parent), m_body(body), m_mode(mode),
          m_chunkSize(qMax(1, chunkSize)),
          m_chunkDelayMs(qMax(0, chunkDelayMs)), m_status(status),
          m_stats(std::move(stats))
    {
        m_stats->bodySize = body.size();
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setAttribute(QNetworkRequest::HttpReasonPhraseAttribute, reasonFor(status));
        setHeader(QNetworkRequest::ContentLengthHeader, body.size());
        if (status >= 400) {
            setError(status == 404 ? QNetworkReply::ContentNotFoundError
                                   : QNetworkReply::InternalServerError,
                     reasonFor(status));
        }
        if (mode != Hang) {
            QTimer::singleShot(mode == Chunked ? m_chunkDelayMs : 0,
                               this, &FakeReply::deliver);
        }
    }

    void abort() override
    {
        m_stats->abortRequested = true;
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_stats->aborted = true;
        m_stats->finished = true;
        setError(QNetworkReply::OperationCanceledError,
                 QStringLiteral("Operation canceled"));
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return static_cast<qint64>(m_delivered - m_offset) + QIODevice::bytesAvailable();
    }

    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 available = m_delivered - m_offset;
        if (available <= 0) {
            return m_finished ? -1 : 0;
        }
        const qint64 count = qMin(maxSize, available);
        std::memcpy(data, m_body.constData() + m_offset, static_cast<size_t>(count));
        m_offset += static_cast<int>(count);
        m_stats->bytesRead = m_offset;
        return count;
    }

private:
    static QString reasonFor(int status)
    {
        switch (status) {
        case 200:
            return QStringLiteral("OK");
        case 301:
            return QStringLiteral("Moved Permanently");
        case 404:
            return QStringLiteral("Not Found");
        case 500:
            return QStringLiteral("Internal Server Error");
        default:
            return QString();
        }
    }

    void deliver()
    {
        if (m_finished) {
            return;
        }
        if (m_mode == Chunked && m_delivered < m_body.size()) {
            m_delivered = qMin(m_body.size(), m_delivered + m_chunkSize);
            emit readyRead();
            if (!m_finished) {
                QTimer::singleShot(m_chunkDelayMs, this, &FakeReply::deliver);
            }
            return;
        }
        m_delivered = m_body.size();
        m_finished = true;
        m_stats->finished = true;
        emit readyRead();
        emit finished();
    }

    QByteArray m_body;
    Mode m_mode;
    int m_chunkSize;
    int m_chunkDelayMs;
    int m_status;
    std::shared_ptr<ReplyStats> m_stats;
    int m_delivered = 0;
    int m_offset = 0;
    bool m_finished = false;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager
{
public:
    struct Response
    {
        int status = 404;
        QByteArray body;
        FakeReply::Mode mode = FakeReply::Immediate;
        int chunkSize = 64 * 1024;
        int chunkDelayMs = 0;
    };

    using QNetworkAccessManager::QNetworkAccessManager;

    QHash<QString, Response> responses;
    QVector<QNetworkRequest> requests;
    QVector<QPointer<FakeReply>> replies;
    QVector<std::shared_ptr<ReplyStats>> stats;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoingData) override
    {
        Q_UNUSED(operation);
        Q_UNUSED(outgoingData);
        requests.append(request);
        const Response response =
            responses.value(request.url().toString(QUrl::FullyEncoded));
        auto replyStats = std::make_shared<ReplyStats>();
        stats.append(replyStats);
        auto *reply = new FakeReply(request, response.status, response.body,
                                    response.mode, response.chunkSize,
                                    response.chunkDelayMs,
                                    replyStats, this);
        replies.append(reply);
        return reply;
    }
};

FakeNetworkAccessManager::Response ok(const QByteArray &body,
                                      FakeReply::Mode mode = FakeReply::Immediate,
                                      int chunkSize = 64 * 1024,
                                      int chunkDelayMs = 0)
{
    FakeNetworkAccessManager::Response response;
    response.status = 200;
    response.body = body;
    response.mode = mode;
    response.chunkSize = chunkSize;
    response.chunkDelayMs = chunkDelayMs;
    return response;
}

FakeNetworkAccessManager::Response status(int code)
{
    FakeNetworkAccessManager::Response response;
    response.status = code;
    return response;
}

void installCatalog(FakeNetworkAccessManager &fake)
{
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"), ok(R"([
        {"name":"Copter.param","path":"Tools/Frame_params/Copter.param","type":"file"},
        {"name":"README.md","path":"Tools/Frame_params/README.md","type":"file"},
        {"name":"QuadPlanes","path":"Tools/Frame_params/QuadPlanes","type":"dir"},
        {"name":"Zeta.param","path":"Tools/Frame_params/Zeta.param","type":"file"},
        {"name":"Duplicate.param","path":"Tools/Frame_params/COPTER.param","type":"file"},
        {"name":"","path":"Tools/Frame_params/Unnamed.param","type":"file"},
        {"name":"quadplanes","path":"Tools/Frame_params/QUADPLANES","type":"DIR"},
        {"name":"symlink","path":"Tools/Frame_params/link","type":"symlink"}
    ])"));
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params/QuadPlanes"), ok(R"([
        {"name":"Tailsitter.param","path":"Tools/Frame_params/QuadPlanes/Tailsitter.param","type":"file"},
        {"name":"Nested","path":"Tools/Frame_params/QuadPlanes/Nested","type":"dir"}
    ])"));
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params/QuadPlanes/Nested"), ok(R"([
        {"name":"Deep.param","path":"Tools/Frame_params/QuadPlanes/Nested/Deep.param","type":"file"}
    ])"));
    fake.responses.insert(
        kRawRoot + QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param"),
        ok(QByteArrayLiteral("Q_FRAME_CLASS,10\n")));
}

QStringList paths(const QVector<FrameDefaultCatalogItem> &items)
{
    QStringList result;
    for (const FrameDefaultCatalogItem &item : items) {
        result << item.path;
    }
    return result;
}

QVector<FrameDefaultCatalogItem> itemsOf(const QSignalSpy &spy, int index)
{
    return spy.at(index).at(0).value<QVector<FrameDefaultCatalogItem>>();
}

} // namespace

class FrameDefaultCatalogServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void normalizesPathsLikeMissionPlanner_data();
    void normalizesPathsLikeMissionPlanner();
    void cachePathPreservesSubdirectoriesAndStaysBelowRoot();
    void cachePathRejectsExistingSymbolicLinkTraversal();
    void displayNamesAndEscaping();
    void listsRecursivelyFiltersSortsAndCaches();
    void joinsRunningWalkAndReloadsAfterCancel();
    void failsOnMalformedCatalog_data();
    void failsOnMalformedCatalog();
    void reportsHttpErrors();
    void enforcesListingLimitWhileStreaming();
    void boundsReadsForImmediateLargeBodies();
    void enforcesDirectoryAndEntryLimits();
    void rejectsExcessUniqueDirectoriesBeforeEnqueue();
    void downloadsValidatedParamFiles();
    void enforcesDownloadLimitWhileStreaming();
    void cancelsExactlyOnceAndIgnoresLateReplies();
    void cancelIsReentrancySafe();
    void cancelsOperationsIndependentlyByGeneration();
    void failsActiveOperationsWhenInjectedManagerIsDestroyed();
    void timesOutSilentReplies();
    void writesCacheFileAtomically();
    void failsWhenInjectedManagerIsGone();
};

void FrameDefaultCatalogServiceTest::normalizesPathsLikeMissionPlanner_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("normalized");
    QTest::addColumn<QString>("errorFragment");

    QTest::newRow("plain") << QStringLiteral("Tools/Frame_params/Copter.param")
                           << QStringLiteral("Tools/Frame_params/Copter.param") << QString();
    QTest::newRow("nested upper-case extension")
        << QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.PARAM")
        << QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.PARAM") << QString();
    QTest::newRow("parent traversal") << QStringLiteral("../secret.param") << QString()
                                      << QStringLiteral("unsafe frame-default path");
    QTest::newRow("inner traversal") << QStringLiteral("Tools/Frame_params/../secret.param")
                                     << QString() << QStringLiteral("unsafe frame-default path");
    QTest::newRow("dot segment") << QStringLiteral("Tools/Frame_params/./x.param")
                                 << QString() << QStringLiteral("unsafe frame-default path");
    QTest::newRow("empty segment") << QStringLiteral("Tools/Frame_params//x.param")
                                   << QString() << QStringLiteral("unsafe frame-default path");
    QTest::newRow("outside root") << QStringLiteral("Tools/Other/secret.param")
                                  << QString() << QStringLiteral("outside Tools/Frame_params");
    QTest::newRow("root case differs") << QStringLiteral("tools/frame_params/x.param")
                                       << QString() << QStringLiteral("outside Tools/Frame_params");
    QTest::newRow("root prefix extended") << QStringLiteral("Tools/Frame_paramsX/x.param")
                                          << QString() << QStringLiteral("outside Tools/Frame_params");
    QTest::newRow("leading slash") << QStringLiteral("/Tools/Frame_params/secret.param")
                                   << QString() << QStringLiteral("invalid frame-default path");
    QTest::newRow("backslashes") << QStringLiteral("Tools\\Frame_params\\secret.param")
                                 << QString() << QStringLiteral("invalid frame-default path");
    QTest::newRow("empty") << QString() << QString() << QStringLiteral("invalid frame-default path");
    QTest::newRow("wrong extension") << QStringLiteral("Tools/Frame_params/secret.txt")
                                     << QString() << QStringLiteral(".param extension");
    QTest::newRow("root itself") << QStringLiteral("Tools/Frame_params") << QString()
                                 << QStringLiteral(".param extension");
}

void FrameDefaultCatalogServiceTest::normalizesPathsLikeMissionPlanner()
{
    QFETCH(QString, input);
    QFETCH(QString, normalized);
    QFETCH(QString, errorFragment);

    QString error;
    const QString result = FrameDefaultCatalogService::NormalizeParamPath(input, &error);
    QCOMPARE(result, normalized);
    if (errorFragment.isEmpty()) {
        QVERIFY2(error.isEmpty(), qPrintable(error));
    } else {
        QVERIFY2(error.contains(errorFragment), qPrintable(error));
    }
}

void FrameDefaultCatalogServiceTest::cachePathPreservesSubdirectoriesAndStaysBelowRoot()
{
    const QString root = QDir::tempPath() + QStringLiteral("/apm-frame-default-tests");

    QString error;
    const QString path = FrameDefaultCatalogService::GetCachePath(
        root, QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(path, QDir::cleanPath(root + QStringLiteral(
        "/frame-defaults/Tools/Frame_params/QuadPlanes/Tailsitter.param")));

    // A relative cache root is anchored like Path.GetFullPath.
    const QString relative = FrameDefaultCatalogService::GetCachePath(
        QStringLiteral("cache"), QStringLiteral("Tools/Frame_params/Copter.param"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(QDir::isAbsolutePath(relative));
    QVERIFY(relative.endsWith(QStringLiteral("/cache/frame-defaults/Tools/Frame_params/Copter.param")));

    const QString escaped = FrameDefaultCatalogService::GetCachePath(
        root, QStringLiteral("Tools/Frame_params/../../secret.param"), &error);
    QVERIFY(escaped.isEmpty());
    QVERIFY2(error.contains(QStringLiteral("unsafe")), qPrintable(error));
}

void FrameDefaultCatalogServiceTest::cachePathRejectsExistingSymbolicLinkTraversal()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString cacheRoot = directory.filePath(QStringLiteral("cache"));
    const QString toolsDirectory =
        QDir(cacheRoot).filePath(QStringLiteral("frame-defaults/Tools"));
    const QString outsideDirectory = directory.filePath(QStringLiteral("outside"));
    QVERIFY(QDir().mkpath(toolsDirectory));
    QVERIFY(QDir().mkpath(outsideDirectory));

    const QString linkPath = QDir(toolsDirectory).filePath(QStringLiteral("Frame_params"));
    if (!QFile::link(outsideDirectory, linkPath)) {
        QSKIP("This platform/test environment cannot create a directory symbolic link.");
    }
    if (!QFileInfo(linkPath).isSymLink()) {
        QSKIP("Qt does not expose the created link as a filesystem symbolic link.");
    }

    QString error;
    const QString catalogPath = QStringLiteral("Tools/Frame_params/Copter.param");
    QVERIFY(FrameDefaultCatalogService::GetCachePath(cacheRoot, catalogPath, &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("symbolic link")), qPrintable(error));
    error.clear();
    QVERIFY(FrameDefaultCatalogService::WriteCacheFile(
                cacheRoot, catalogPath, QByteArrayLiteral("FRAME_CLASS,1\n"), &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("symbolic link")), qPrintable(error));
    QVERIFY(!QFileInfo(QDir(outsideDirectory).filePath(QStringLiteral("Copter.param"))).exists());
}

void FrameDefaultCatalogServiceTest::displayNamesAndEscaping()
{
    FrameDefaultCatalogItem item;
    item.name = QStringLiteral("Tailsitter.param");
    item.path = QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param");
    QCOMPARE(item.displayName(), QStringLiteral("QuadPlanes / Tailsitter.param"));
    QCOMPARE(FrameDefaultCatalogService::DisplayName(
                 QStringLiteral("Copter.param"), QStringLiteral("Tools/Frame_params/Copter.param")),
             QStringLiteral("Copter.param"));
    QCOMPARE(FrameDefaultCatalogService::DisplayName(
                 QStringLiteral("fallback"), QStringLiteral("Other/x.param")),
             QStringLiteral("fallback"));

    QCOMPARE(FrameDefaultCatalogService::EscapePath(
                 QStringLiteral("Tools/Frame_params/My Dir/a#b+c.param")),
             QStringLiteral("Tools/Frame_params/My%20Dir/a%23b%2Bc.param"));
    QCOMPARE(FrameDefaultCatalogService::CatalogRoot, QStringLiteral("Tools/Frame_params"));
}

void FrameDefaultCatalogServiceTest::listsRecursivelyFiltersSortsAndCaches()
{
    FakeNetworkAccessManager fake;
    installCatalog(fake);
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);
    QSignalSpy failed(&service, &FrameDefaultCatalogService::catalogFailed);

    QString error;
    QVERIFY2(service.requestCatalog(false, &error), qPrintable(error));
    QVERIFY(service.isCatalogLoading());
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(failed.count(), 0);
    QVERIFY(!service.isCatalogLoading());

    const QVector<FrameDefaultCatalogItem> items = itemsOf(ready, 0);
    QCOMPARE(ready.at(0).at(1).toBool(), false);
    QCOMPARE(paths(items), QStringList()
             << QStringLiteral("Tools/Frame_params/Copter.param")
             << QStringLiteral("Tools/Frame_params/QuadPlanes/Nested/Deep.param")
             << QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param")
             << QStringLiteral("Tools/Frame_params/Unnamed.param")
             << QStringLiteral("Tools/Frame_params/Zeta.param"));
    QCOMPARE(items.at(0).name, QStringLiteral("Copter.param")); // first duplicate wins
    QCOMPARE(items.at(3).name, QStringLiteral("Unnamed.param")); // name from path
    QCOMPARE(items.at(2).displayName(), QStringLiteral("QuadPlanes / Tailsitter.param"));

    // Root, QuadPlanes and Nested were listed once each; the case-variant
    // QUADPLANES directory was skipped as already visited.
    QCOMPARE(fake.requests.size(), 3);
    QCOMPARE(fake.requests.at(0).url().toString(QUrl::FullyEncoded),
             kApiRoot + QStringLiteral("Tools/Frame_params"));
    QCOMPARE(fake.requests.at(1).url().toString(QUrl::FullyEncoded),
             kApiRoot + QStringLiteral("Tools/Frame_params/QuadPlanes"));
    QCOMPARE(fake.requests.at(2).url().toString(QUrl::FullyEncoded),
             kApiRoot + QStringLiteral("Tools/Frame_params/QuadPlanes/Nested"));
    for (const QNetworkRequest &request : fake.requests) {
        QCOMPARE(request.header(QNetworkRequest::UserAgentHeader).toString(), kUserAgent);
        QCOMPARE(request.rawHeader(QByteArrayLiteral("Accept")),
                 QByteArrayLiteral("application/vnd.github+json"));
    }

    // Memoised: a second request is answered synchronously without network.
    QVERIFY(service.hasCachedCatalog());
    QCOMPARE(service.cachedCatalog(), items);
    bool cachedStarted = true;
    FrameDefaultCatalogService::OperationId cachedId = 123;
    QVERIFY(service.requestCatalog(false, &error, &cachedStarted, &cachedId));
    QVERIFY(!cachedStarted);
    QCOMPARE(cachedId, FrameDefaultCatalogService::InvalidOperationId);
    QCOMPARE(ready.count(), 2);
    QCOMPARE(ready.at(1).at(1).toBool(), true);
    QCOMPARE(itemsOf(ready, 1), items);
    QCOMPARE(fake.requests.size(), 3);

    // forceRefresh walks the catalog again.
    QVERIFY(service.requestCatalog(true, &error));
    QTRY_COMPARE(ready.count(), 3);
    QCOMPARE(ready.at(2).at(1).toBool(), false);
    QCOMPARE(fake.requests.size(), 6);
    QCOMPARE(failed.count(), 0);
}

void FrameDefaultCatalogServiceTest::joinsRunningWalkAndReloadsAfterCancel()
{
    FakeNetworkAccessManager fake;
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                          ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);
    QSignalSpy failed(&service, &FrameDefaultCatalogService::catalogFailed);

    bool started = false;
    FrameDefaultCatalogService::OperationId ownerId =
        FrameDefaultCatalogService::InvalidOperationId;
    QVERIFY(service.requestCatalog(false, nullptr, &started, &ownerId));
    QVERIFY(started);
    QVERIFY(ownerId != FrameDefaultCatalogService::InvalidOperationId);

    bool joinedStarted = true;
    FrameDefaultCatalogService::OperationId joinedId =
        FrameDefaultCatalogService::InvalidOperationId;
    QVERIFY(service.requestCatalog(false, nullptr, &joinedStarted, &joinedId));
    QVERIFY(!joinedStarted);
    QCOMPARE(joinedId, ownerId);
    QCOMPARE(fake.requests.size(), 1);

    QVERIFY(!service.cancelCatalog(ownerId + 100));
    QCOMPARE(failed.count(), 0);
    QVERIFY(service.isCatalogLoading());
    QVERIFY(service.cancelCatalog(ownerId));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(1).toBool(), true);
    QVERIFY2(failed.at(0).at(0).toString().contains(QStringLiteral("cancelled")),
             qPrintable(failed.at(0).at(0).toString()));
    QVERIFY(!service.isCatalogLoading());
    QVERIFY(!service.hasCachedCatalog());

    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                          ok(QByteArrayLiteral("[]")));
    FrameDefaultCatalogService::OperationId replacementId =
        FrameDefaultCatalogService::InvalidOperationId;
    QVERIFY(service.requestCatalog(false, nullptr, &started, &replacementId));
    QVERIFY(started);
    QVERIFY(replacementId != ownerId);
    QCOMPARE(fake.requests.size(), 2);
    QTRY_COMPARE(ready.count(), 1);
    QVERIFY(itemsOf(ready, 0).isEmpty());
    QCOMPARE(failed.count(), 1);
}

void FrameDefaultCatalogServiceTest::failsOnMalformedCatalog_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<QString>("errorFragment");

    QTest::newRow("object instead of array")
        << QByteArrayLiteral("{\"message\":\"Not Found\"}")
        << QStringLiteral("invalid frame-default listing");
    QTest::newRow("truncated json") << QByteArrayLiteral("[{\"name\":")
                                    << QStringLiteral("invalid frame-default listing");
    QTest::newRow("empty body") << QByteArray()
                                << QStringLiteral("invalid frame-default listing");
    QTest::newRow("non-object entry") << QByteArrayLiteral("[42]")
                                      << QStringLiteral("invalid frame-default path");
    QTest::newRow("entry without path")
        << QByteArrayLiteral("[{\"name\":\"x\",\"type\":\"file\"}]")
        << QStringLiteral("invalid frame-default path");
    QTest::newRow("traversal entry")
        << QByteArrayLiteral("[{\"name\":\"x\",\"path\":\"Tools/Frame_params/../x.param\",\"type\":\"file\"}]")
        << QStringLiteral("unsafe frame-default path");
    QTest::newRow("outside entry even when ignored type")
        << QByteArrayLiteral("[{\"name\":\"x\",\"path\":\"Tools/Other/x.md\",\"type\":\"file\"}]")
        << QStringLiteral("outside Tools/Frame_params");
    QTest::newRow("backslash entry")
        << QByteArrayLiteral("[{\"name\":\"x\",\"path\":\"Tools\\\\Frame_params\\\\x.param\",\"type\":\"dir\"}]")
        << QStringLiteral("invalid frame-default path");
}

void FrameDefaultCatalogServiceTest::failsOnMalformedCatalog()
{
    QFETCH(QByteArray, body);
    QFETCH(QString, errorFragment);

    FakeNetworkAccessManager fake;
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"), ok(body));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);
    QSignalSpy failed(&service, &FrameDefaultCatalogService::catalogFailed);

    QVERIFY(service.requestCatalog());
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(failed.at(0).at(1).toBool(), false);
    QVERIFY2(failed.at(0).at(0).toString().contains(errorFragment),
             qPrintable(failed.at(0).at(0).toString()));
    QVERIFY(!service.isCatalogLoading());
    QVERIFY(!service.hasCachedCatalog());
}

void FrameDefaultCatalogServiceTest::reportsHttpErrors()
{
    const int codes[] = {404, 500, 301};
    for (int code : codes) {
        FakeNetworkAccessManager fake;
        fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"), status(code));
        FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
        QSignalSpy failed(&service, &FrameDefaultCatalogService::catalogFailed);

        QVERIFY(service.requestCatalog());
        QTRY_COMPARE(failed.count(), 1);
        const QString error = failed.at(0).at(0).toString();
        QVERIFY2(error.contains(QStringLiteral("HTTP %1").arg(code)), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("Tools/Frame_params")), qPrintable(error));
        QCOMPARE(failed.at(0).at(1).toBool(), false);
    }

    // A nested directory failure reports that directory and aborts the walk.
    FakeNetworkAccessManager fake;
    installCatalog(fake);
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params/QuadPlanes/Nested"), status(404));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);
    QSignalSpy failed(&service, &FrameDefaultCatalogService::catalogFailed);
    QVERIFY(service.requestCatalog());
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QVERIFY2(failed.at(0).at(0).toString().contains(QStringLiteral("QuadPlanes/Nested")),
             qPrintable(failed.at(0).at(0).toString()));
    QVERIFY(!service.hasCachedCatalog());
}

void FrameDefaultCatalogServiceTest::enforcesListingLimitWhileStreaming()
{
    const QByteArray body = QByteArrayLiteral("[") + QByteArray(3 * 1024 * 1024, ' ')
                            + QByteArrayLiteral("]");
    FakeNetworkAccessManager fake;
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                          ok(body, FakeReply::Chunked, 16 * 1024));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    service.setMaxListingBytes(64 * 1024);
    QSignalSpy failed(&service, &FrameDefaultCatalogService::catalogFailed);

    QVERIFY(service.requestCatalog());
    QTRY_COMPARE(failed.count(), 1);
    const QString error = failed.at(0).at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("exceeds the 65536-byte listing limit")),
             qPrintable(error));
    QCOMPARE(failed.at(0).at(1).toBool(), false);

    QCOMPARE(fake.stats.size(), 1);
    QVERIFY(fake.stats.at(0)->aborted);
    QVERIFY(fake.stats.at(0)->bytesRead < body.size());
    QVERIFY(fake.stats.at(0)->bytesRead <= 64 * 1024 + 16 * 1024);
    QTest::qWait(10); // late chunks after the abort must not resurrect the walk
    QCOMPARE(failed.count(), 1);
    QVERIFY(!service.isCatalogLoading());
}

void FrameDefaultCatalogServiceTest::boundsReadsForImmediateLargeBodies()
{
    // A reply that offers its whole body at once must not be read beyond
    // limit + 1 bytes, for listings and for downloads alike.
    const QByteArray listing = QByteArrayLiteral("[") + QByteArray(3 * 1024 * 1024, ' ')
                               + QByteArrayLiteral("]");
    const QByteArray file(2 * 1024 * 1024, 'x');
    FakeNetworkAccessManager fake;
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"), ok(listing));
    fake.responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Huge.param"), ok(file));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    service.setMaxListingBytes(64 * 1024);
    service.setMaxDownloadBytes(1024);
    QSignalSpy catalogFailed(&service, &FrameDefaultCatalogService::catalogFailed);
    QSignalSpy downloadFailed(&service, &FrameDefaultCatalogService::downloadFailed);
    QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);
    QSignalSpy finished(&service, &FrameDefaultCatalogService::downloadFinished);

    QVERIFY(service.requestCatalog());
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Huge.param")));
    QTRY_COMPARE(catalogFailed.count(), 1);
    QTRY_COMPARE(downloadFailed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(finished.count(), 0);
    QVERIFY2(catalogFailed.at(0).at(0).toString().contains(QStringLiteral("65536-byte listing limit")),
             qPrintable(catalogFailed.at(0).at(0).toString()));
    QVERIFY2(downloadFailed.at(0).at(1).toString().contains(QStringLiteral("1024-byte download limit")),
             qPrintable(downloadFailed.at(0).at(1).toString()));

    QCOMPARE(fake.stats.size(), 2);
    QVERIFY(fake.stats.at(0)->abortRequested);
    QVERIFY(fake.stats.at(0)->bytesRead <= 64 * 1024 + 1);
    QVERIFY(fake.stats.at(1)->abortRequested);
    QVERIFY(fake.stats.at(1)->bytesRead <= 1024 + 1);

    // Large chunks are bounded the same way.
    fake.responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Huge.param"),
                          ok(file, FakeReply::Chunked, 512 * 1024));
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Huge.param")));
    QTRY_COMPARE(downloadFailed.count(), 2);
    QCOMPARE(fake.stats.size(), 3);
    QVERIFY(fake.stats.at(2)->aborted);
    QVERIFY(fake.stats.at(2)->bytesRead <= 1024 + 1);
    QVERIFY(!service.isCatalogLoading());
    QVERIFY(!service.isDownloading());
}

void FrameDefaultCatalogServiceTest::enforcesDirectoryAndEntryLimits()
{
    FakeNetworkAccessManager fake;
    installCatalog(fake);
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy failed(&service, &FrameDefaultCatalogService::catalogFailed);
    QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);

    service.setMaxDirectories(2);
    QVERIFY(service.requestCatalog());
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY2(failed.at(0).at(0).toString().contains(QStringLiteral("exceeded 2 directories")),
             qPrintable(failed.at(0).at(0).toString()));
    QCOMPARE(fake.requests.size(), 2);

    service.setMaxDirectories(FrameDefaultCatalogService::DefaultMaxDirectories);
    service.setMaxEntries(3);
    QVERIFY(service.requestCatalog());
    QTRY_COMPARE(failed.count(), 2);
    QVERIFY2(failed.at(1).at(0).toString().contains(QStringLiteral("exceeds 3 entries")),
             qPrintable(failed.at(1).at(0).toString()));
    QCOMPARE(ready.count(), 0);
}

void FrameDefaultCatalogServiceTest::rejectsExcessUniqueDirectoriesBeforeEnqueue()
{
    // CatalogRoot plus one unique child exactly meets the limit. A case-only
    // duplicate must neither consume capacity nor trigger another request.
    {
        FakeNetworkAccessManager fake;
        fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"), ok(R"([
            {"path":"Tools/Frame_params/One","type":"dir"},
            {"path":"Tools/Frame_params/ONE","type":"DIR"}
        ])"));
        fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params/One"),
                              ok(QByteArrayLiteral("[]")));
        FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
        service.setMaxDirectories(2);
        QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);
        QSignalSpy failed(&service, &FrameDefaultCatalogService::catalogFailed);

        QVERIFY(service.requestCatalog());
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(fake.requests.size(), 2);
    }

    // A second unique child is rejected while parsing the root response. It is
    // never retained for later processing and no child request is issued.
    {
        FakeNetworkAccessManager fake;
        fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"), ok(R"([
            {"path":"Tools/Frame_params/One","type":"dir"},
            {"path":"Tools/Frame_params/ONE","type":"DIR"},
            {"path":"Tools/Frame_params/Two","type":"dir"}
        ])"));
        FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
        service.setMaxDirectories(2);
        QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);
        QSignalSpy failed(&service, &FrameDefaultCatalogService::catalogFailed);

        QVERIFY(service.requestCatalog());
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(fake.requests.size(), 1);
        QVERIFY2(failed.at(0).at(0).toString().contains(
                     QStringLiteral("exceeded 2 directories")),
                 qPrintable(failed.at(0).at(0).toString()));
    }
}

void FrameDefaultCatalogServiceTest::downloadsValidatedParamFiles()
{
    FakeNetworkAccessManager fake;
    installCatalog(fake);
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy finished(&service, &FrameDefaultCatalogService::downloadFinished);
    QSignalSpy failed(&service, &FrameDefaultCatalogService::downloadFailed);

    QString error;
    QVERIFY2(service.download(QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param"), &error),
             qPrintable(error));
    QVERIFY(service.isDownloading());
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(0).toString(),
             QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param"));
    QCOMPARE(finished.at(0).at(1).toByteArray(), QByteArrayLiteral("Q_FRAME_CLASS,10\n"));
    QCOMPARE(failed.count(), 0);
    QVERIFY(!service.isDownloading());
    QCOMPARE(fake.requests.size(), 1);
    QCOMPARE(fake.requests.at(0).url().toString(QUrl::FullyEncoded),
             kRawRoot + QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param"));
    QCOMPARE(fake.requests.at(0).header(QNetworkRequest::UserAgentHeader).toString(), kUserAgent);

    // Invalid paths are refused synchronously without a network request.
    QVERIFY(!service.download(QStringLiteral("Tools/Other/x.param"), &error));
    QVERIFY2(error.contains(QStringLiteral("outside")), qPrintable(error));
    QVERIFY(!service.download(QStringLiteral("Tools/Frame_params/x.txt"), &error));
    QVERIFY2(error.contains(QStringLiteral(".param")), qPrintable(error));
    QVERIFY(!service.download(QStringLiteral("Tools/Frame_params/../x.param"), &error));
    QCOMPARE(fake.requests.size(), 1);
    QCOMPARE(failed.count(), 0);

    // A missing file is an HTTP failure for that exact path.
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Missing.param"), &error));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("Tools/Frame_params/Missing.param"));
    QVERIFY2(failed.at(0).at(1).toString().contains(QStringLiteral("HTTP 404")),
             qPrintable(failed.at(0).at(1).toString()));
    QCOMPARE(failed.at(0).at(2).toBool(), false);

    // Only one download at a time.
    fake.responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Slow.param"),
                          ok(QByteArrayLiteral("x"), FakeReply::Hang));
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Slow.param"), &error));
    QVERIFY(!service.download(QStringLiteral("Tools/Frame_params/Copter.param"), &error));
    QVERIFY2(error.contains(QStringLiteral("already running")), qPrintable(error));
    service.cancel();
    QCOMPARE(failed.count(), 2);
    QCOMPARE(failed.at(1).at(2).toBool(), true);
}

void FrameDefaultCatalogServiceTest::enforcesDownloadLimitWhileStreaming()
{
    const QByteArray body(2 * 1024 * 1024, 'x');
    FakeNetworkAccessManager fake;
    fake.responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Huge.param"),
                          ok(body, FakeReply::Chunked, 8 * 1024));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    service.setMaxDownloadBytes(1024);
    QSignalSpy finished(&service, &FrameDefaultCatalogService::downloadFinished);
    QSignalSpy failed(&service, &FrameDefaultCatalogService::downloadFailed);

    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Huge.param")));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(finished.count(), 0);
    QVERIFY2(failed.at(0).at(1).toString().contains(QStringLiteral("exceeds the 1024-byte download limit")),
             qPrintable(failed.at(0).at(1).toString()));
    QCOMPARE(fake.stats.size(), 1);
    QVERIFY(fake.stats.at(0)->aborted);
    QVERIFY(fake.stats.at(0)->bytesRead < body.size());
    QVERIFY(!service.isDownloading());
}

void FrameDefaultCatalogServiceTest::cancelsExactlyOnceAndIgnoresLateReplies()
{
    FakeNetworkAccessManager fake;
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                          ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    fake.responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Copter.param"),
                          ok(QByteArrayLiteral("x"), FakeReply::Hang));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);
    QSignalSpy catalogFailed(&service, &FrameDefaultCatalogService::catalogFailed);
    QSignalSpy finished(&service, &FrameDefaultCatalogService::downloadFinished);
    QSignalSpy downloadFailed(&service, &FrameDefaultCatalogService::downloadFailed);

    service.cancel(); // nothing active: no signals
    QCOMPARE(catalogFailed.count(), 0);
    QCOMPARE(downloadFailed.count(), 0);

    QVERIFY(service.requestCatalog());
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Copter.param")));
    QCOMPARE(fake.replies.size(), 2);

    service.cancel();
    QCOMPARE(catalogFailed.count(), 1);
    QCOMPARE(catalogFailed.at(0).at(1).toBool(), true);
    QCOMPARE(downloadFailed.count(), 1);
    QCOMPARE(downloadFailed.at(0).at(0).toString(), QStringLiteral("Tools/Frame_params/Copter.param"));
    QCOMPARE(downloadFailed.at(0).at(2).toBool(), true);
    QVERIFY(!service.isCatalogLoading());
    QVERIFY(!service.isDownloading());
    QVERIFY(fake.stats.at(0)->aborted);
    QVERIFY(fake.stats.at(1)->aborted);

    service.cancel(); // idempotent
    QTest::qWait(20); // deleteLater and any late reply activity
    QCOMPARE(catalogFailed.count(), 1);
    QCOMPARE(downloadFailed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(finished.count(), 0);
    QVERIFY(!fake.replies.at(0)); // replies were released
    QVERIFY(!fake.replies.at(1));
}

void FrameDefaultCatalogServiceTest::cancelIsReentrancySafe()
{
    FakeNetworkAccessManager fake;
    installCatalog(fake);
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                          ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    fake.responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Slow.param"),
                          ok(QByteArrayLiteral("x"), FakeReply::Hang));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy catalogFailed(&service, &FrameDefaultCatalogService::catalogFailed);
    QSignalSpy downloadFailed(&service, &FrameDefaultCatalogService::downloadFailed);
    QSignalSpy finished(&service, &FrameDefaultCatalogService::downloadFinished);

    // A slot reacting to the cancelled catalog immediately starts another
    // download; the cancel in progress must not sweep it up.
    bool restarted = false;
    QString restartError;
    connect(&service, &FrameDefaultCatalogService::catalogFailed, &service,
            [&service, &restarted, &restartError]() {
                restarted = service.download(
                    QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param"),
                    &restartError);
            });

    QVERIFY(service.requestCatalog());
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Slow.param")));
    service.cancel();

    QVERIFY2(restarted, qPrintable(restartError));
    QCOMPARE(catalogFailed.count(), 1);
    QCOMPARE(catalogFailed.at(0).at(1).toBool(), true);
    QCOMPARE(downloadFailed.count(), 1); // only the old download was cancelled
    QCOMPARE(downloadFailed.at(0).at(0).toString(), QStringLiteral("Tools/Frame_params/Slow.param"));
    QCOMPARE(downloadFailed.at(0).at(2).toBool(), true);
    QVERIFY(service.isDownloading()); // the restarted download is alive
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(0).toString(),
             QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param"));
    QCOMPARE(finished.at(0).at(1).toByteArray(), QByteArrayLiteral("Q_FRAME_CLASS,10\n"));
    QCOMPARE(downloadFailed.count(), 1);
    QCOMPARE(catalogFailed.count(), 1);
}

void FrameDefaultCatalogServiceTest::cancelsOperationsIndependentlyByGeneration()
{
    FakeNetworkAccessManager fake;
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                          ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    fake.responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Copter.param"),
                          ok(QByteArrayLiteral("x"), FakeReply::Hang));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy catalogFailed(&service, &FrameDefaultCatalogService::catalogFailed);
    QSignalSpy downloadFailed(&service, &FrameDefaultCatalogService::downloadFailed);

    bool replacementStarted = false;
    FrameDefaultCatalogService::OperationId replacementId =
        FrameDefaultCatalogService::InvalidOperationId;
    connect(&service, &FrameDefaultCatalogService::catalogFailed, &service,
            [&service, &replacementStarted, &replacementId](const QString &,
                                                             bool cancelled) {
                if (cancelled &&
                    replacementId == FrameDefaultCatalogService::InvalidOperationId) {
                    replacementStarted = service.requestCatalog(
                        false, nullptr, &replacementStarted, &replacementId);
                }
            });

    bool catalogStarted = false;
    FrameDefaultCatalogService::OperationId catalogId =
        FrameDefaultCatalogService::InvalidOperationId;
    QVERIFY(service.requestCatalog(false, nullptr, &catalogStarted, &catalogId));
    QVERIFY(catalogStarted);

    FrameDefaultCatalogService::OperationId downloadId =
        FrameDefaultCatalogService::InvalidOperationId;
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Copter.param"),
                             nullptr, &downloadId));
    QVERIFY(downloadId != FrameDefaultCatalogService::InvalidOperationId);
    QVERIFY(downloadId != catalogId);
    QVERIFY(!service.cancelCatalog(downloadId));
    QVERIFY(!service.cancelDownload(catalogId));
    QCOMPARE(catalogFailed.count(), 0);
    QCOMPARE(downloadFailed.count(), 0);

    QVERIFY(service.cancelCatalog(catalogId));
    QCOMPARE(catalogFailed.count(), 1);
    QCOMPARE(catalogFailed.at(0).at(1).toBool(), true);
    QCOMPARE(downloadFailed.count(), 0);
    QVERIFY(service.isCatalogLoading()); // replacement started from the failure slot
    QVERIFY(service.isDownloading());
    QVERIFY(fake.stats.at(0)->aborted);
    QVERIFY(!fake.stats.at(1)->abortRequested);

    // The cancelled catalog's slot started this replacement re-entrantly.
    QVERIFY(replacementStarted);
    QVERIFY(replacementId != catalogId);
    QVERIFY(!service.cancelCatalog(catalogId)); // stale owner cannot cancel replacement
    QVERIFY(service.isCatalogLoading());
    QCOMPARE(catalogFailed.count(), 1);

    QVERIFY(!service.cancelDownload(downloadId + 100));
    QCOMPARE(downloadFailed.count(), 0);
    QVERIFY(service.isDownloading());
    QVERIFY(service.cancelDownload(downloadId));
    QCOMPARE(downloadFailed.count(), 1);
    QCOMPARE(downloadFailed.at(0).at(0).toString(),
             QStringLiteral("Tools/Frame_params/Copter.param"));
    QCOMPARE(downloadFailed.at(0).at(2).toBool(), true);
    QVERIFY(!service.isDownloading());
    QVERIFY(service.isCatalogLoading()); // download cancellation left it alone

    QVERIFY(service.cancelCatalog(replacementId));
    QCOMPARE(catalogFailed.count(), 2);
    QCOMPARE(catalogFailed.at(1).at(1).toBool(), true);
    QVERIFY(!service.isCatalogLoading());
    QCOMPARE(downloadFailed.count(), 1);
}

void FrameDefaultCatalogServiceTest::failsActiveOperationsWhenInjectedManagerIsDestroyed()
{
    auto *fake = new FakeNetworkAccessManager;
    fake->responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                           ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    fake->responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Copter.param"),
                           ok(QByteArrayLiteral("x"), FakeReply::Hang));
    FrameDefaultCatalogService service(fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy catalogFailed(&service, &FrameDefaultCatalogService::catalogFailed);
    QSignalSpy downloadFailed(&service, &FrameDefaultCatalogService::downloadFailed);
    QSignalSpy ready(&service, &FrameDefaultCatalogService::catalogReady);
    QSignalSpy finished(&service, &FrameDefaultCatalogService::downloadFinished);

    QVERIFY(service.requestCatalog());
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Copter.param")));
    QVERIFY(service.isCatalogLoading());
    QVERIFY(service.isDownloading());

    delete fake; // takes its child replies with it

    QCOMPARE(catalogFailed.count(), 1);
    QCOMPARE(catalogFailed.at(0).at(1).toBool(), false);
    QVERIFY2(catalogFailed.at(0).at(0).toString().contains(QStringLiteral("destroyed")),
             qPrintable(catalogFailed.at(0).at(0).toString()));
    QCOMPARE(downloadFailed.count(), 1);
    QCOMPARE(downloadFailed.at(0).at(0).toString(), QStringLiteral("Tools/Frame_params/Copter.param"));
    QCOMPARE(downloadFailed.at(0).at(2).toBool(), false);
    QVERIFY2(downloadFailed.at(0).at(1).toString().contains(QStringLiteral("destroyed")),
             qPrintable(downloadFailed.at(0).at(1).toString()));
    QVERIFY(!service.isCatalogLoading());
    QVERIFY(!service.isDownloading());

    QTest::qWait(20);
    QCOMPARE(catalogFailed.count(), 1);
    QCOMPARE(downloadFailed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(finished.count(), 0);

    QString error;
    QVERIFY(!service.requestCatalog(false, &error));
    QVERIFY2(error.contains(QStringLiteral("no longer available")), qPrintable(error));
    service.cancel(); // nothing active any more
    QCOMPARE(catalogFailed.count(), 1);
    QCOMPARE(downloadFailed.count(), 1);
}

void FrameDefaultCatalogServiceTest::timesOutSilentReplies()
{
    FakeNetworkAccessManager fake;
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                          ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    fake.responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Copter.param"),
                          ok(QByteArrayLiteral("x"), FakeReply::Hang));
    FrameDefaultCatalogService service(&fake, QUrl(kApiRoot), QUrl(kRawRoot));
    service.setTransferTimeoutMs(50);
    QSignalSpy catalogFailed(&service, &FrameDefaultCatalogService::catalogFailed);
    QSignalSpy downloadFailed(&service, &FrameDefaultCatalogService::downloadFailed);

    QVERIFY(service.requestCatalog());
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Copter.param")));
    QTRY_COMPARE(catalogFailed.count(), 1);
    QTRY_COMPARE(downloadFailed.count(), 1);
    QVERIFY2(catalogFailed.at(0).at(0).toString().contains(QStringLiteral("timed out after 50 ms")),
             qPrintable(catalogFailed.at(0).at(0).toString()));
    QCOMPARE(catalogFailed.at(0).at(1).toBool(), false);
    QVERIFY2(downloadFailed.at(0).at(1).toString().contains(QStringLiteral("timed out after 50 ms")),
             qPrintable(downloadFailed.at(0).at(1).toString()));
    QCOMPARE(downloadFailed.at(0).at(2).toBool(), false);
    QVERIFY(!service.isCatalogLoading());
    QVERIFY(!service.isDownloading());

    // Data that keeps flowing restarts the watchdog. Every gap is shorter than
    // the timeout, but the total transfer is longer than it, so this fails if
    // readyRead does not restart the timer.
    service.setTransferTimeoutMs(100);
    fake.responses.insert(kRawRoot + QStringLiteral("Tools/Frame_params/Copter.param"),
                          ok(QByteArray(10 * 1024, 'y'), FakeReply::Chunked,
                             1024, 20));
    QSignalSpy finished(&service, &FrameDefaultCatalogService::downloadFinished);
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(service.download(QStringLiteral("Tools/Frame_params/Copter.param")));
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(elapsed.elapsed() > service.transferTimeoutMs());
    QCOMPARE(finished.at(0).at(1).toByteArray().size(), 10 * 1024);
    QCOMPARE(downloadFailed.count(), 1);
}

void FrameDefaultCatalogServiceTest::writesCacheFileAtomically()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QString error;
    const QString path = FrameDefaultCatalogService::WriteCacheFile(
        directory.path(), QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param"),
        QByteArrayLiteral("Q_FRAME_CLASS,10\n"), &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    QCOMPARE(path, FrameDefaultCatalogService::GetCachePath(
                       directory.path(), QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param")));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("Q_FRAME_CLASS,10\n"));
    file.close();

    // Overwrites in place and keeps the directory layout.
    QVERIFY(!FrameDefaultCatalogService::WriteCacheFile(
                 directory.path(), QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param"),
                 QByteArrayLiteral("Q_FRAME_CLASS,11\n"), &error).isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("Q_FRAME_CLASS,11\n"));
    QVERIFY(QDir(directory.path()).exists(QStringLiteral("frame-defaults/Tools/Frame_params/QuadPlanes")));

    QVERIFY(FrameDefaultCatalogService::WriteCacheFile(
                directory.path(), QStringLiteral("Tools/Other/x.param"),
                QByteArrayLiteral("x"), &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("outside")), qPrintable(error));
    QVERIFY(!QDir(directory.path()).exists(QStringLiteral("frame-defaults/Tools/Other")));

    QVERIFY(!FrameDefaultCatalogService::DefaultCacheRoot().isEmpty());
    QVERIFY(QDir::isAbsolutePath(FrameDefaultCatalogService::DefaultCacheRoot()));
}

void FrameDefaultCatalogServiceTest::failsWhenInjectedManagerIsGone()
{
    auto *fake = new FakeNetworkAccessManager;
    FrameDefaultCatalogService service(fake, QUrl(kApiRoot), QUrl(kRawRoot));
    QSignalSpy catalogFailed(&service, &FrameDefaultCatalogService::catalogFailed);
    QSignalSpy downloadFailed(&service, &FrameDefaultCatalogService::downloadFailed);
    QCOMPARE(service.apiRoot(), QUrl(kApiRoot));
    QCOMPARE(service.rawRoot(), QUrl(kRawRoot));

    delete fake;

    QString error;
    QVERIFY(!service.requestCatalog(false, &error));
    QVERIFY2(error.contains(QStringLiteral("no longer available")), qPrintable(error));
    QVERIFY(!service.isCatalogLoading());
    QVERIFY(!service.download(QStringLiteral("Tools/Frame_params/Copter.param"), &error));
    QVERIFY2(error.contains(QStringLiteral("no longer available")), qPrintable(error));
    QVERIFY(!service.isDownloading());
    QCOMPARE(catalogFailed.count(), 0);
    QCOMPARE(downloadFailed.count(), 0);

    // The production constructor owns its manager and uses the official roots.
    FrameDefaultCatalogService production;
    QCOMPARE(production.apiRoot(), FrameDefaultCatalogService::DefaultApiRoot);
    QCOMPARE(production.rawRoot(), FrameDefaultCatalogService::DefaultRawRoot);
    QCOMPARE(production.maxDownloadBytes(), FrameDefaultCatalogService::DefaultMaxDownloadBytes);
    QCOMPARE(production.transferTimeoutMs(), FrameDefaultCatalogService::DefaultTransferTimeoutMs);
}

QTEST_GUILESS_MAIN(FrameDefaultCatalogServiceTest)
#include "test_framedefaultcatalogservice.moc"
