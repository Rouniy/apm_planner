#include "services/SftpLogSession.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <memory>

namespace {
class CancelledHandshake final : public QThread {
public:
    quint16 port = 0;
    int action = 0; // cancellation / explicit stop / owner destruction
    bool result = true, connected = true, reopenedInvalid = true;
    qint64 elapsed = 0;
    QString error;
    void run() override {
        auto session = createSftpLogSession();
        QElapsedTimer clock; clock.start();
        SshHostKeyChallenge challenge;
        const SftpLogConnection connection{QStringLiteral("127.0.0.1"), port,
            QStringLiteral("fixture-user"), QStringLiteral("NEVER-SEND-THIS-BEFORE-TRUST-6d4502f1")};
        SftpLogSession *raw = session.get();
        result = raw->connect(connection, {}, &challenge, &error, [&] {
            if (clock.elapsed() < 150) return false;
            if (action == 1 && session) session->stop();
            if (action == 2) session.reset();
            return action == 0;
        });
        elapsed = clock.elapsed(); connected = session && session->isConnected();
        if (session) {
            session->stop(); session->stop();
            QString ignored;
            reopenedInvalid = session->connect({}, {}, nullptr, &ignored);
        } else reopenedInvalid = false;
    }
};
}

class SftpLogSessionTest final : public QObject
{
    Q_OBJECT
private slots:
    void parseEndpoint_data()
    {
        QTest::addColumn<QString>("input"); QTest::addColumn<int>("defaultPort");
        QTest::addColumn<bool>("valid"); QTest::addColumn<QString>("host"); QTest::addColumn<int>("port");
        QTest::newRow("hostname") << QString("companion.local") << 22 << true << QString("companion.local") << 22;
        QTest::newRow("hostname-port") << QString(" companion.local:2202 ") << 22 << true << QString("companion.local") << 2202;
        QTest::newRow("ipv4") << QString("127.0.0.1:2222") << 22 << true << QString("127.0.0.1") << 2222;
        QTest::newRow("ipv6") << QString("[fe80::1234]:2222") << 22 << true << QString("fe80::1234") << 2222;
        QTest::newRow("bare-ipv6") << QString("fe80::1234") << 2200 << true << QString("fe80::1234") << 2200;
        QTest::newRow("bracket-default") << QString("[::1]") << 22 << true << QString("::1") << 22;
        QTest::newRow("scope") << QString("[fe80::1%eth0]:2222") << 22 << true << QString("fe80::1%eth0") << 2222;
        QTest::newRow("empty") << QString() << 22 << false << QString() << 0;
        QTest::newRow("bad-default") << QString("host") << 65536 << false << QString() << 0;
        QTest::newRow("zero-port") << QString("host:0") << 22 << false << QString() << 0;
        QTest::newRow("large-port") << QString("host:65536") << 22 << false << QString() << 0;
        QTest::newRow("missing-port") << QString("host:") << 22 << false << QString() << 0;
        QTest::newRow("negative-port") << QString("host:-1") << 22 << false << QString() << 0;
        QTest::newRow("missing-host") << QString(":22") << 22 << false << QString() << 0;
        QTest::newRow("bracket-garbage") << QString("[::1]garbage") << 22 << false << QString() << 0;
        QTest::newRow("unclosed") << QString("[::1") << 22 << false << QString() << 0;
        QTest::newRow("nul") << QString("host") + QChar(0) << 22 << false << QString() << 0;
    }
    void parseEndpoint()
    {
        QFETCH(QString, input); QFETCH(int, defaultPort); QFETCH(bool, valid);
        QFETCH(QString, host); QFETCH(int, port);
        QString actualHost = QStringLiteral("old"), error; quint16 actualPort = 999;
        QCOMPARE(SftpLogSession::parseEndpoint(input, defaultPort, &actualHost, &actualPort, &error), valid);
        QCOMPARE(actualHost, host); QCOMPARE(int(actualPort), port);
        QCOMPARE(error.isEmpty(), valid);
    }
    void remotePathMatchesPosixReference()
    {
        QCOMPARE(SftpLogSession::normalizeDirectory(" /var/log/dataflash/ "), QString("/var/log/dataflash"));
        QCOMPARE(SftpLogSession::normalizeDirectory("//var///log"), QString("/var/log"));
        QCOMPARE(SftpLogSession::normalizeDirectory("////"), QString("/"));
        for (const auto &path : {"relative/path", "/var/../etc", "/var/./log", ""}) {
            QString error; QVERIFY(SftpLogSession::normalizeDirectory(path, &error).isEmpty()); QVERIFY(!error.isEmpty());
        }
        QVERIFY(SftpLogSession::normalizeDirectory(QString("/var/log") + QChar(0)).isEmpty());
        QVERIFY(SftpLogSession::normalizeDirectory('/' + QString(4096, 'a')).isEmpty());
        for (const auto &name : {"00000001.BIN", ".hidden.bin", "--flag.bin", "back\\slash.bin", "a b.bin"})
            QVERIFY(SftpLogSession::isSafeName(name));
        for (const auto &name : {"", ".", "..", "../a.bin", "nested/file.bin"}) QVERIFY(!SftpLogSession::isSafeName(name));
        QVERIFY(!SftpLogSession::isSafeName(QString("bad") + QChar(0) + ".bin"));
        QVERIFY(!SftpLogSession::isSafeName(QString(4097, 'a')));
        SftpLogEntry root{"/", "00000001.BIN", 1, {}};
        QCOMPARE(root.remotePath(), QString("/00000001.BIN"));
        root.remoteDirectory = "//var//log/"; QCOMPARE(root.remotePath(), QString("/var/log/00000001.BIN"));
        root.name = "../bad.bin"; QVERIFY(root.remotePath().isEmpty());
    }
    void fingerprintsAndSettingsIdentityAreExact()
    {
        QCOMPARE(SftpLogSession::fingerprint("hello"), QString("SHA256:LPJNul+wow4m6DsqxbninhsWHlwfp0JecwQzYpOLmCQ"));
        const auto pin = SftpLogSession::fingerprint(QByteArray(32, char(0xa5)));
        QVERIFY(!pin.endsWith('=')); QVERIFY(SftpLogSession::fingerprintsEqual(' ' + pin + '\n', pin));
        QVERIFY(!SftpLogSession::fingerprintsEqual({}, pin));
        QVERIFY(!SftpLogSession::fingerprintsEqual({}, {}));
        QVERIFY(!SftpLogSession::fingerprintsEqual(pin.toLower(), pin));
        QVERIFY(!SftpLogSession::fingerprintsEqual(pin + '=', pin));
        QString changed = pin; changed[changed.size() - 1] = changed.endsWith('A') ? 'B' : 'A';
        QVERIFY(!SftpLogSession::fingerprintsEqual(changed, pin));
        QCOMPARE(SftpLogSession::trustedKeySettingName(" Companion.Local ", 22),
                 QString("SSHHostKey_2BA69E46DA82DF0A82EA176402045E507D69053DCDDB622DFE85B43D23926EA0"));
        QVERIFY(SftpLogSession::trustedKeySettingName("companion.local", 2202)
                != SftpLogSession::trustedKeySettingName("companion.local", 22));
        SshHostKeyChallenge challenge; QVERIFY(!challenge.isChanged());
        challenge.expectedFingerprint = pin; QVERIFY(challenge.isChanged());
    }
    void disconnectedAndInvalidInputsNeverWrite()
    {
        auto session = createSftpLogSession(); QVERIFY(session); QVERIFY(!session->isConnected());
        QString error; QVector<SftpLogEntry> entries{{"/logs", "old.bin", 1, {}}};
        QVERIFY(!session->listLogs("/logs", &entries, &error)); QVERIFY(entries.isEmpty()); QVERIFY(!error.isEmpty());
        QBuffer buffer; QVERIFY(buffer.open(QIODevice::ReadWrite)); qint64 copied = 123;
        const SftpLogEntry entry{"/logs", "one.bin", 10, QDateTime::currentDateTimeUtc()};
        QVERIFY(!session->download(entry, &buffer, &copied, &error)); QCOMPARE(copied, qint64(0)); QVERIFY(buffer.data().isEmpty());
        QVERIFY(!session->remove(entry, &error));
        for (const auto &connection : QVector<SftpLogConnection>{{{}, 22, "user", "secret"},
                {"127.0.0.1", 0, "user", "secret"}, {"127.0.0.1", 22, {}, "secret"},
                {"127.0.0.1", 22, "user", QString(65537, 'x')}}) {
            SshHostKeyChallenge challenge; challenge.presentedFingerprint = "stale";
            QVERIFY(!session->connect(connection, {}, &challenge, &error));
            QVERIFY(challenge.presentedFingerprint.isEmpty()); QVERIFY(!session->isConnected());
            QVERIFY(!error.contains("secret"));
        }
        session->stop(); session->stop(); QVERIFY(!session->isConnected());
    }
    void preCancelledConnectDoesNotOpenSocketOrLeakPassword()
    {
        auto session = createSftpLogSession(); QString error; SshHostKeyChallenge challenge;
        const QString secret = QStringLiteral("this-is-not-a-network-test-password");
        QVERIFY(!session->connect({"127.0.0.1", 22, "user", secret}, {}, &challenge, &error, [] { return true; }));
        QVERIFY(!session->isConnected()); QVERIFY(error.contains("cancel", Qt::CaseInsensitive));
        QVERIFY(!error.contains(secret)); QVERIFY(challenge.presentedFingerprint.isEmpty());
    }
    void downloadMetadataConsistency_data()
    {
        QTest::addColumn<qint64>("beforeSize"); QTest::addColumn<qint64>("afterSize");
        QTest::addColumn<qint64>("beforeMs"); QTest::addColumn<qint64>("afterMs");
        QTest::addColumn<bool>("compatible");
        const qint64 time = 1700000000000LL;
        QTest::newRow("unchanged") << qint64(10) << qint64(10) << time << time << true;
        QTest::newRow("same-second-growth") << qint64(10) << qint64(20) << time << time << true;
        QTest::newRow("later-growth") << qint64(10) << qint64(20) << time << time + 1000 << true;
        QTest::newRow("empty-start-growth") << qint64(0) << qint64(20) << time << time + 1000 << true;
        QTest::newRow("size-shrank") << qint64(20) << qint64(10) << time << time + 1000 << false;
        QTest::newRow("time-went-back") << qint64(10) << qint64(20) << time << time - 1000 << false;
        QTest::newRow("same-size-modified") << qint64(10) << qint64(10) << time << time + 1000 << false;
        QTest::newRow("negative-length") << qint64(-1) << qint64(10) << time << time << false;
        QTest::newRow("maximum") << qint64(10) << SftpLogSession::MaximumFileBytes << time << time << true;
        QTest::newRow("too-large") << qint64(10) << SftpLogSession::MaximumFileBytes + 1 << time << time << false;
        QTest::newRow("pre-epoch") << qint64(10) << qint64(10) << qint64(-1000) << qint64(-1000) << false;
        QTest::newRow("fractional-time") << qint64(10) << qint64(10) << time + 1 << time + 1 << false;
        QTest::newRow("outside-v3-time") << qint64(10) << qint64(20) << time << qint64(4294967296000LL) << false;
    }
    void downloadMetadataConsistency()
    {
        QFETCH(qint64, beforeSize); QFETCH(qint64, afterSize);
        QFETCH(qint64, beforeMs); QFETCH(qint64, afterMs); QFETCH(bool, compatible);
        const SftpLogEntry before{"/logs", "fixture.bin", beforeSize, QDateTime::fromMSecsSinceEpoch(beforeMs, Qt::UTC)};
        const SftpLogEntry after{"/logs", "fixture.bin", afterSize, QDateTime::fromMSecsSinceEpoch(afterMs, Qt::UTC)};
        QCOMPARE(SftpLogSession::compatibleDownloadMetadata(before, after), compatible);
    }
    void downloadMetadataComparesBothPathHandleBoundaries()
    {
        const auto stamp = QDateTime::fromSecsSinceEpoch(1700000000, Qt::UTC);
        const SftpLogEntry pathBefore{"/logs", "fixture.bin", 100, stamp};
        const SftpLogEntry handle{"/logs", "fixture.bin", 120, stamp.addSecs(1)};
        const SftpLogEntry pathAfter{"/logs", "fixture.bin", 140, stamp.addSecs(2)};
        QVERIFY(SftpLogSession::compatibleDownloadMetadata(pathBefore, handle));
        QVERIFY(SftpLogSession::compatibleDownloadMetadata(handle, pathAfter));
        // Individually valid regular metadata can still describe a stale
        // handle or a replaced/shrunk path; neither boundary may be skipped.
        QVERIFY(!SftpLogSession::compatibleDownloadMetadata(pathAfter, handle));
        QVERIFY(!SftpLogSession::compatibleDownloadMetadata(handle, pathBefore));
        auto unknownTime = pathAfter; unknownTime.lastWriteTimeUtc = {};
        QVERIFY(!SftpLogSession::compatibleDownloadMetadata(handle, unknownTime));
    }
    void silentPeerCancellationIsBounded_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("cancel") << 0;
        QTest::newRow("stop-from-cancel-observer") << 1;
        QTest::newRow("delete-from-cancel-observer") << 2;
    }
    void silentPeerCancellationIsBounded()
    {
        QFETCH(int, action);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QByteArray received; int peers = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (server.hasPendingConnections()) {
                auto *peer = server.nextPendingConnection(); ++peers;
                connect(peer, &QTcpSocket::readyRead, &server, [&, peer] {
                    received += peer->read(65536 - received.size());
                });
            }
        });
        CancelledHandshake worker; worker.port = server.serverPort(); worker.action = action;
        QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
        connect(&worker, &QThread::finished, &loop, &QEventLoop::quit);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(3000);
        worker.start();
        loop.exec();
        const bool finished = worker.isFinished();
        // A bounded test failure must still join its worker before local
        // objects are destroyed; cancellation deadline is independent of UI.
        if (!finished) worker.wait(22000);
        QVERIFY(finished); QVERIFY(!worker.result); QVERIFY(!worker.connected); QVERIFY(!worker.reopenedInvalid);
        QVERIFY(worker.elapsed < 2000); QVERIFY(!worker.error.contains("NEVER-SEND"));
        QCoreApplication::processEvents();
        QCOMPARE(peers, 1);
        QVERIFY(received.startsWith("SSH-2.0-"));
        QVERIFY(!received.contains("NEVER-SEND-THIS-BEFORE-TRUST-6d4502f1"));
        QVERIFY(!received.contains("fixture-user"));
    }
};

QTEST_GUILESS_MAIN(SftpLogSessionTest)
#include "test_sftplogsession.moc"
