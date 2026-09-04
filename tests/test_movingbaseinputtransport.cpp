#include "comm/MovingBaseInputTransport.h"

#include <QPointer>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtTest/QtTest>

#include <memory>

class MovingBaseInputTransportTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndValidationMatchReference();
    void tcpHostNewestClientResetsPartialLine();
    void tcpClientReadsAndFailsWithoutReconnect();
    void udpHostPinsFirstSourceAndFiltersOthers();
    void udpClientAnnouncesAndFiltersConfiguredResolvedEndpoint();
    void framingBoundsAndParseOutcomesAreExplicit();
    void reentrantLifecycleSupersedesOldGeneration();
    void staleDnsCompletionCannotReviveStoppedRun();
};

namespace
{
const QByteArray kFix = QByteArrayLiteral(
    "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");

QByteArray noFixSentence()
{
    const QString body = QStringLiteral(
        "$GNGGA,123519,4807.038,N,01131.000,E,0,00,99.9,545.4,M,46.9,M,,");
    return (body + QLatin1Char('*') + NmeaGgaParser::checksum(body)
            + QStringLiteral("\r\n")).toLatin1();
}
}

void MovingBaseInputTransportTest::defaultsAndValidationMatchReference()
{
    QCOMPARE(MovingBaseInputTransport::modeLabel(
                 MovingBaseInputTransport::Mode::Serial),
             QStringLiteral("Serial"));
    QCOMPARE(MovingBaseInputTransport::modeLabel(
                 MovingBaseInputTransport::Mode::TcpHost),
             QStringLiteral("TCP Host"));
    QCOMPARE(MovingBaseInputTransport::modeLabel(
                 MovingBaseInputTransport::Mode::TcpClient),
             QStringLiteral("TCP Client"));
    QCOMPARE(MovingBaseInputTransport::modeLabel(
                 MovingBaseInputTransport::Mode::UdpHost),
             QStringLiteral("UDP Host"));
    QCOMPARE(MovingBaseInputTransport::modeLabel(
                 MovingBaseInputTransport::Mode::UdpClient),
             QStringLiteral("UDP Client"));
    QCOMPARE(MovingBaseInputTransport::supportedBaudRates(),
             QList<int>({4800, 9600, 14400, 19200,
                         28800, 38400, 57600, 115200}));

    const auto tcpHost = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::TcpHost);
    QCOMPARE(tcpHost.host, QStringLiteral("0.0.0.0"));
    QCOMPARE(tcpHost.port, quint16(14551));
    const auto udpClient = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::UdpClient);
    QCOMPARE(udpClient.host, QStringLiteral("127.0.0.1"));
    QCOMPARE(udpClient.port, quint16(14551));
    const auto serial = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::Serial);
    QCOMPARE(serial.baud, 4800);

    MovingBaseInputTransport missingSerial(serial);
    QString error;
    QVERIFY(!missingSerial.start(&error));
    QCOMPARE(missingSerial.state(), MovingBaseInputTransport::State::Error);
    QVERIFY(error.contains(QStringLiteral("serial port"),
                           Qt::CaseInsensitive));

    auto badHost = tcpHost;
    badHost.host = QStringLiteral("not-a-listen-address");
    MovingBaseInputTransport invalidHost(badHost);
    QVERIFY(!invalidHost.start(&error));
    QCOMPARE(invalidHost.state(), MovingBaseInputTransport::State::Error);
    QVERIFY(error.contains(QStringLiteral("numeric IP"),
                           Qt::CaseInsensitive));

    auto missingRemote = udpClient;
    missingRemote.host.clear();
    MovingBaseInputTransport invalidRemote(missingRemote);
    QVERIFY(!invalidRemote.start(&error));
    QVERIFY(error.contains(QStringLiteral("remote"), Qt::CaseInsensitive));
}

void MovingBaseInputTransportTest::tcpHostNewestClientResetsPartialLine()
{
    auto settings = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::TcpHost);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 0;
    MovingBaseInputTransport transport(settings);
    QSignalSpy fixes(&transport, &MovingBaseInputTransport::fixReceived);
    QSignalSpy rejected(&transport,
                        &MovingBaseInputTransport::sentenceRejected);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QCOMPARE(transport.state(), MovingBaseInputTransport::State::Listening);
    QVERIFY(transport.localPort() != 0);

    QTcpSocket first;
    first.connectToHost(QHostAddress::LocalHost, transport.localPort());
    QTRY_COMPARE(transport.state(), MovingBaseInputTransport::State::Ready);
    first.write(kFix.left(28));
    first.flush();
    QTRY_COMPARE(transport.bufferedLineBytes(), 28);
    QCOMPARE(fixes.count(), 0);

    QTcpSocket newest;
    newest.connectToHost(QHostAddress::LocalHost, transport.localPort());
    QTRY_VERIFY(newest.state() == QAbstractSocket::ConnectedState);
    QTRY_VERIFY(transport.sourceDescription().endsWith(
        QStringLiteral(":%1").arg(newest.localPort())));
    QTRY_VERIFY(first.state() == QAbstractSocket::UnconnectedState);

    // If the old partial line leaked across clients, this tail would become
    // the complete valid sentence. It must instead be rejected in isolation.
    newest.write(kFix.mid(28));
    newest.flush();
    QTRY_COMPARE(rejected.count(), 1);
    QCOMPARE(fixes.count(), 0);

    newest.write(kFix);
    newest.flush();
    QTRY_COMPARE(fixes.count(), 1);
    QCOMPARE(transport.acceptedFixCount(), quint64(1));

    newest.abort();
    QTRY_COMPARE(transport.state(), MovingBaseInputTransport::State::Listening);
    QVERIFY(transport.sourceDescription().isEmpty());
}

void MovingBaseInputTransportTest::tcpClientReadsAndFailsWithoutReconnect()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    auto settings = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::TcpClient);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = server.serverPort();
    MovingBaseInputTransport transport(settings);
    QSignalSpy fixes(&transport, &MovingBaseInputTransport::fixReceived);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QCOMPARE(transport.state(), MovingBaseInputTransport::State::Connecting);
    QTRY_VERIFY(server.hasPendingConnections());
    std::unique_ptr<QTcpSocket> peer(server.nextPendingConnection());
    QVERIFY(peer);
    QTRY_COMPARE(transport.state(), MovingBaseInputTransport::State::Ready);

    peer->write(kFix);
    peer->flush();
    QTRY_COMPARE(fixes.count(), 1);
    peer->abort();
    peer.reset();
    QTRY_COMPARE(transport.state(), MovingBaseInputTransport::State::Error);
    QVERIFY(transport.lastError().contains(QStringLiteral("reconnect"),
                                           Qt::CaseInsensitive));
    QTest::qWait(40);
    QVERIFY(!server.hasPendingConnections());
}

void MovingBaseInputTransportTest::udpHostPinsFirstSourceAndFiltersOthers()
{
    auto settings = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::UdpHost);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 0;
    MovingBaseInputTransport transport(settings);
    QSignalSpy fixes(&transport, &MovingBaseInputTransport::fixReceived);
    QSignalSpy ignored(&transport,
                       &MovingBaseInputTransport::foreignDatagramIgnored);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QCOMPARE(transport.state(), MovingBaseInputTransport::State::Listening);

    QUdpSocket pinned;
    QUdpSocket foreign;
    QVERIFY(pinned.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QVERIFY(foreign.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QCOMPARE(pinned.writeDatagram(kFix.left(25), QHostAddress::LocalHost,
                                 transport.localPort()), qint64(25));
    QTRY_COMPARE(transport.state(), MovingBaseInputTransport::State::Ready);
    QVERIFY(transport.sourceDescription().endsWith(
        QStringLiteral(":%1").arg(pinned.localPort())));

    QCOMPARE(foreign.writeDatagram(kFix, QHostAddress::LocalHost,
                                   transport.localPort()),
             qint64(kFix.size()));
    QTRY_COMPARE(ignored.count(), 1);
    QCOMPARE(fixes.count(), 0);
    QCOMPARE(transport.ignoredDatagramCount(), quint64(1));

    QCOMPARE(pinned.writeDatagram(kFix.mid(25), QHostAddress::LocalHost,
                                  transport.localPort()),
             qint64(kFix.size() - 25));
    QTRY_COMPARE(fixes.count(), 1);
}

void MovingBaseInputTransportTest::
udpClientAnnouncesAndFiltersConfiguredResolvedEndpoint()
{
    QUdpSocket configured;
    QUdpSocket foreign;
    QVERIFY(configured.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QVERIFY(foreign.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));

    auto settings = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::UdpClient);
    settings.host = QStringLiteral("localhost");
    settings.port = configured.localPort();
    MovingBaseInputTransport transport(settings);
    QSignalSpy fixes(&transport, &MovingBaseInputTransport::fixReceived);
    QSignalSpy ignored(&transport,
                       &MovingBaseInputTransport::foreignDatagramIgnored);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QVERIFY(transport.state() == MovingBaseInputTransport::State::Resolving
            || transport.state() == MovingBaseInputTransport::State::Ready);
    QTRY_COMPARE(transport.state(), MovingBaseInputTransport::State::Ready);
    QVERIFY(transport.localPort() != 0);

    QTRY_VERIFY(configured.hasPendingDatagrams());
    const QNetworkDatagram discovery = configured.receiveDatagram();
    QVERIFY(discovery.isValid());
    QVERIFY(discovery.data().isEmpty());
    QCOMPARE(discovery.senderAddress(),
             QHostAddress(QHostAddress::LocalHost));
    QCOMPARE(discovery.senderPort(), transport.localPort());

    QCOMPARE(foreign.writeDatagram(kFix, QHostAddress::LocalHost,
                                   transport.localPort()),
             qint64(kFix.size()));
    QTRY_COMPARE(ignored.count(), 1);
    QCOMPARE(fixes.count(), 0);

    QCOMPARE(configured.writeDatagram(kFix, discovery.senderAddress(),
                                      discovery.senderPort()),
             qint64(kFix.size()));
    QTRY_COMPARE(fixes.count(), 1);
}

void MovingBaseInputTransportTest::framingBoundsAndParseOutcomesAreExplicit()
{
    auto settings = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::UdpHost);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 0;
    MovingBaseInputTransport transport(settings);
    QSignalSpy raw(&transport, &MovingBaseInputTransport::rawLineReceived);
    QSignalSpy noFixSpy(&transport,
                        &MovingBaseInputTransport::noPositionFix);
    QSignalSpy malformed(&transport,
                         &MovingBaseInputTransport::sentenceRejected);
    QSignalSpy oversized(&transport, &MovingBaseInputTransport::lineRejected);
    QSignalSpy fixes(&transport, &MovingBaseInputTransport::fixReceived);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QByteArray tooLong(MovingBaseInputTransport::MaximumLineBytes + 1, 'x');
    tooLong.append('\n');
    QCOMPARE(sender.writeDatagram(tooLong, QHostAddress::LocalHost,
                                  transport.localPort()),
             qint64(tooLong.size()));
    QTRY_COMPARE(oversized.count(), 1);
    QCOMPARE(raw.count(), 0);

    const QByteArray noFixBytes = noFixSentence();
    QCOMPARE(sender.writeDatagram(noFixBytes, QHostAddress::LocalHost,
                                  transport.localPort()),
             qint64(noFixBytes.size()));
    QTRY_COMPARE(noFixSpy.count(), 1);

    const QByteArray batch = QByteArrayLiteral("$GPGGA,bad*00\r\n") + kFix;
    QCOMPARE(sender.writeDatagram(batch, QHostAddress::LocalHost,
                                  transport.localPort()),
             qint64(batch.size()));
    QTRY_COMPARE(malformed.count(), 1);
    QTRY_COMPARE(fixes.count(), 1);
    QCOMPARE(raw.count(), 3);
    QCOMPARE(transport.acceptedFixCount(), quint64(1));
    QCOMPARE(transport.rejectedLineCount(), quint64(3));
}

void MovingBaseInputTransportTest::reentrantLifecycleSupersedesOldGeneration()
{
    auto settings = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::UdpClient);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 14551;

    MovingBaseInputTransport stopped(settings);
    QObject::connect(
        &stopped, &MovingBaseInputTransport::stateChanged, &stopped,
        [&stopped](MovingBaseInputTransport::State state) {
            if (state == MovingBaseInputTransport::State::Ready) {
                stopped.stop();
            }
        }, Qt::DirectConnection);
    QVERIFY(!stopped.start());
    QCOMPARE(stopped.state(), MovingBaseInputTransport::State::Stopped);

    auto *deleted = new MovingBaseInputTransport(settings);
    QPointer<MovingBaseInputTransport> guard(deleted);
    QObject::connect(
        deleted, &MovingBaseInputTransport::stateChanged, deleted,
        [deleted](MovingBaseInputTransport::State state) {
            if (state == MovingBaseInputTransport::State::Ready) {
                delete deleted;
            }
        }, Qt::DirectConnection);
    QVERIFY(!deleted->start());
    QVERIFY(guard.isNull());
}

void MovingBaseInputTransportTest::staleDnsCompletionCannotReviveStoppedRun()
{
    auto settings = MovingBaseInputTransport::defaults(
        MovingBaseInputTransport::Mode::UdpClient);
    settings.host = QStringLiteral("localhost");
    settings.port = 14551;
    MovingBaseInputTransport transport(settings);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QCOMPARE(transport.state(), MovingBaseInputTransport::State::Resolving);
    transport.stop();

    // Exercise the actual queued/aborted lookup. Whether Qt suppresses its
    // callback or delivers it late, it must not bind a new socket or revive
    // the stopped generation.
    QTest::qWait(100);
    QCOMPARE(transport.state(), MovingBaseInputTransport::State::Stopped);
    QCOMPARE(transport.localPort(), quint16(0));
}

QTEST_GUILESS_MAIN(MovingBaseInputTransportTest)

#include "test_movingbaseinputtransport.moc"
