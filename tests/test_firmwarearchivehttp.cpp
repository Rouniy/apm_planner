#include "services/FirmwareArchiveHttp.h"

#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtConcurrentRun>
#include <atomic>
#include <stdexcept>

namespace {
class Server final : public QTcpServer
{
public:
    QHash<QByteArray, QByteArray> replies;
    QList<QByteArray> requests;
    Server() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true);
                    const QByteArray path = request.split(' ').value(1);
                    requests.append(path);
                    if (path == "/hang") return;
                    socket->write(replies.value(path, "HTTP/1.1 404 Missing\r\nContent-Length: 0\r\n\r\n"));
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url(const QString &path) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path));
    }
};
struct Outcome { FirmwareArchive::FetchResult result; QByteArray bytes; int chunks = 0; };
QFuture<Outcome> start(QUrl url, qint64 cap = 1024, int timeout = 1000,
                       FirmwareArchive::Cancel cancel = {}, bool throwSink = false,
                       bool httpsOnly = false)
{
    return QtConcurrent::run([=] {
        Outcome outcome;
        outcome.result = FirmwareArchiveHttp::transport(timeout)(url, cap, httpsOnly, cancel,
            [&](const QByteArray &bytes) {
                if (throwSink) throw std::runtime_error("test sink");
                ++outcome.chunks;
                outcome.bytes += bytes;
                return true;
            });
        return outcome;
    });
}
}

class FirmwareArchiveHttpTest : public QObject
{
    Q_OBJECT
private slots:
    void streamsBinaryAndFollowsRedirect() {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray binary("\0\1\2\xff\0", 5);
        server.replies["/start"] = "HTTP/1.1 302 Found\r\nLocation: /firmware\r\nContent-Length: 999999\r\n\r\n";
        server.replies["/firmware"] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n" + binary;
        auto job = start(server.url("/start"));
        QTRY_VERIFY_WITH_TIMEOUT(job.isFinished(), 3000);
        const auto result = job.result();
        QVERIFY2(result.result.success(), qPrintable(result.result.error));
        QCOMPARE(result.bytes, binary);
        QCOMPARE(result.result.bytes, qint64(5));
        QCOMPARE(server.requests, QList<QByteArray>({"/start", "/firmware"}));
    }
    void rejectsOversizeDeclaredAndChunked_data() {
        QTest::addColumn<QByteArray>("reply");
        QTest::newRow("declared") << QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 30\r\n\r\n") + QByteArray(30, 'x');
        QTest::newRow("chunked") << QByteArray("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1e\r\n") + QByteArray(30, 'x') + "\r\n0\r\n\r\n";
    }
    void errorBodiesNeverReachSinkAndFourRequestsRemainIndependent() {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.replies["/error"] = "HTTP/1.1 404 Missing\r\nContent-Length: 999999999\r\n\r\n";
        auto rejected = start(server.url("/error"), 10);
        QTRY_VERIFY_WITH_TIMEOUT(rejected.isFinished(), 3000);
        QCOMPARE(rejected.result().result.failure, FirmwareArchive::Failure::Network);
        QCOMPARE(rejected.result().result.error, QStringLiteral("HTTP 404: Missing"));
        QVERIFY(rejected.result().bytes.isEmpty());
        QCOMPARE(rejected.result().chunks, 0);
        QVector<QFuture<Outcome>> jobs;
        for (int i = 0; i < 4; ++i) {
            const QByteArray path = "/parallel" + QByteArray::number(i);
            server.replies[path] = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n" + QByteArray(1, char('0' + i));
            jobs.append(start(server.url(QString::fromLatin1(path))));
        }
        for (int i = 0; i < jobs.size(); ++i) {
            QTRY_VERIFY_WITH_TIMEOUT(jobs[i].isFinished(), 3000);
            QVERIFY2(jobs[i].result().result.success(), qPrintable(jobs[i].result().result.error));
            QCOMPARE(jobs[i].result().bytes, QByteArray(1, char('0' + i)));
        }
    }
    void rejectsOversizeDeclaredAndChunked() {
        QFETCH(QByteArray, reply);
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.replies["/firmware"] = reply;
        auto job = start(server.url("/firmware"), 10);
        QTRY_VERIFY_WITH_TIMEOUT(job.isFinished(), 3000);
        QCOMPARE(job.result().result.failure, FirmwareArchive::Failure::Limit);
        QVERIFY(job.result().bytes.size() <= 10);
    }
    void urlPolicyRedirectLoopAndIncompleteBodies() {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto refused = start(server.url("/firmware"), 1024, 1000, {}, false, true);
        QTRY_VERIFY_WITH_TIMEOUT(refused.isFinished(), 3000);
        QCOMPARE(refused.result().result.failure, FirmwareArchive::Failure::Policy);
        QVERIFY(server.requests.isEmpty());
        server.replies["/loop"] = "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n";
        auto loop = start(server.url("/loop"));
        QTRY_VERIFY_WITH_TIMEOUT(loop.isFinished(), 3000);
        QCOMPARE(loop.result().result.failure, FirmwareArchive::Failure::Policy);
        QCOMPARE(server.requests.size(), 9);
        server.replies["/short"] = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nabc";
        auto incomplete = start(server.url("/short"));
        QTRY_VERIFY_WITH_TIMEOUT(incomplete.isFinished(), 3000);
        QCOMPARE(incomplete.result().result.failure, FirmwareArchive::Failure::Network);
        QCOMPARE(incomplete.result().bytes, QByteArray("abc"));
    }
    void timeoutCancellationAndThrowingSinkAreBounded() {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto timeout = start(server.url("/hang"), 1024, 80);
        QTRY_VERIFY_WITH_TIMEOUT(timeout.isFinished(), 3000);
        QCOMPARE(timeout.result().result.failure, FirmwareArchive::Failure::Network);
        std::atomic_bool stop{false};
        auto cancelled = start(server.url("/hang"), 1024, 2000, [&] { return stop.load(); });
        QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 2, 1000);
        stop.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(cancelled.isFinished(), 1000);
        QCOMPARE(cancelled.result().result.failure, FirmwareArchive::Failure::Cancelled);
        server.replies["/data"] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
        auto throwing = start(server.url("/data"), 1024, 1000, {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(throwing.isFinished(), 3000);
        QCOMPARE(throwing.result().result.failure, FirmwareArchive::Failure::LocalIo);
    }
};
QTEST_GUILESS_MAIN(FirmwareArchiveHttpTest)
#include "test_firmwarearchivehttp.moc"
