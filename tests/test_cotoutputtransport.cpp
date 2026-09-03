#include <QtTest>

#include "comm/CotOutputTransport.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <memory>

class CotOutputTransportTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndValidationMatchMp10();
    void udpClientSendsOneWholeDatagramAndResolvesDns();
    void udpHostRepliesOnlyToNewestSender();
    void tcpClientConnectsOnceAndDoesNotReconnect();
    void tcpHostFansOutAndRemovesBrokenClients();
    void sendErrorsMayStopTransportReentrantly();
};

void CotOutputTransportTest::defaultsAndValidationMatchMp10()
{
    QCOMPARE(CotOutputTransport::ModeLabel(CotOutputTransport::Mode::TakMulticast),
             QStringLiteral("TAK Multicast"));
    QCOMPARE(CotOutputTransport::ModeLabel(CotOutputTransport::Mode::UdpClient),
             QStringLiteral("UDP Client"));
    QCOMPARE(CotOutputTransport::ModeLabel(CotOutputTransport::Mode::UdpHost),
             QStringLiteral("UDP Host"));
    QCOMPARE(CotOutputTransport::ModeLabel(CotOutputTransport::Mode::TcpClient),
             QStringLiteral("TCP Client"));
    QCOMPARE(CotOutputTransport::ModeLabel(CotOutputTransport::Mode::TcpHost),
             QStringLiteral("TCP Host"));

    const auto tak = CotOutputTransport::Defaults(CotOutputTransport::Mode::TakMulticast);
    QCOMPARE(tak.host, QStringLiteral("239.2.3.1"));
    QCOMPARE(tak.port, quint16(6969));
    const auto udpHost = CotOutputTransport::Defaults(CotOutputTransport::Mode::UdpHost);
    QCOMPARE(udpHost.host, QStringLiteral("0.0.0.0"));
    QCOMPARE(udpHost.port, quint16(14551));
    const auto serial = CotOutputTransport::Defaults(CotOutputTransport::Mode::Serial);
    QCOMPARE(serial.baud, 57600);

    auto invalidTak = tak;
    invalidTak.host = QStringLiteral("127.0.0.1");
    CotOutputTransport badMulticast(invalidTak);
    QString error;
    QVERIFY(!badMulticast.start(&error));
    QCOMPARE(badMulticast.state(), CotOutputTransport::State::Error);
    QVERIFY(error.contains(QStringLiteral("multicast"), Qt::CaseInsensitive));

    CotOutputTransport missingSerial(serial);
    QVERIFY(!missingSerial.start(&error));
    QCOMPARE(missingSerial.state(), CotOutputTransport::State::Error);
    QVERIFY(error.contains(QStringLiteral("serial port"), Qt::CaseInsensitive));

    CotOutputTransport multicast(tak);
    QVERIFY2(multicast.start(&error), qPrintable(error));
    QCOMPARE(multicast.state(), CotOutputTransport::State::Ready);
    QCOMPARE(multicast.peerCount(), 1);
    QCOMPARE(multicast.peers().first().address, QHostAddress(QStringLiteral("239.2.3.1")));
    multicast.stop();
}

void CotOutputTransportTest::udpClientSendsOneWholeDatagramAndResolvesDns()
{
    QUdpSocket receiver;
    QVERIFY(receiver.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));

    auto settings = CotOutputTransport::Defaults(CotOutputTransport::Mode::UdpClient);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = receiver.localPort();
    CotOutputTransport transport(settings);
    QSignalSpy sentSpy(&transport, &CotOutputTransport::payloadSent);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QCOMPARE(transport.state(), CotOutputTransport::State::Ready);
    QCOMPARE(transport.send(QByteArray("missing-lf")),
             CotOutputTransport::SendResult::InvalidPayload);
    QVERIFY(!receiver.hasPendingDatagrams());

    // Embedded LFs do not split a CoT event: one send() is one UDP datagram.
    const QByteArray payload("<event>\n<detail/>\n");
    QCOMPARE(transport.send(payload), CotOutputTransport::SendResult::Sent);
    QCOMPARE(sentSpy.count(), 1);
    QTRY_VERIFY(receiver.hasPendingDatagrams());
    QCOMPARE(receiver.pendingDatagramSize(), qint64(payload.size()));
    QByteArray received(payload.size(), '\0');
    QHostAddress sender;
    quint16 senderPort = 0;
    QCOMPARE(receiver.readDatagram(received.data(), received.size(), &sender, &senderPort),
             qint64(payload.size()));
    QCOMPARE(received, payload);
    QVERIFY(!receiver.hasPendingDatagrams());

    // UDP Client accepts DNS names as well as numeric destinations.
    settings.host = QStringLiteral("localhost");
    CotOutputTransport dnsTransport(settings);
    QVERIFY2(dnsTransport.start(&error), qPrintable(error));
    QVERIFY(dnsTransport.state() == CotOutputTransport::State::Resolving
            || dnsTransport.state() == CotOutputTransport::State::Ready);
    QTRY_COMPARE(dnsTransport.state(), CotOutputTransport::State::Ready);
    QCOMPARE(dnsTransport.send(QByteArray("<event/>\n")), CotOutputTransport::SendResult::Sent);
    QTRY_VERIFY(receiver.hasPendingDatagrams());
    receiver.readDatagram(received.data(), received.size());
}

void CotOutputTransportTest::udpHostRepliesOnlyToNewestSender()
{
    auto settings = CotOutputTransport::Defaults(CotOutputTransport::Mode::UdpHost);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 0;
    CotOutputTransport transport(settings);
    QSignalSpy peerSpy(&transport, &CotOutputTransport::peersChanged);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QCOMPARE(transport.state(), CotOutputTransport::State::Listening);
    QVERIFY(transport.localPort() != 0);
    QCOMPARE(transport.send(QByteArray("<event/>\n")), CotOutputTransport::SendResult::NoPeer);

    QUdpSocket first;
    QUdpSocket newest;
    QVERIFY(first.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QVERIFY(newest.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    first.writeDatagram(QByteArray("discover-1"), QHostAddress::LocalHost, transport.localPort());
    QTRY_COMPARE(transport.peerCount(), 1);
    QCOMPARE(transport.peers().first().port, first.localPort());

    newest.writeDatagram(QByteArray("discover-2"), QHostAddress::LocalHost, transport.localPort());
    QTRY_COMPARE(transport.peers().first().port, newest.localPort());
    QVERIFY(peerSpy.count() >= 2);

    const QByteArray payload("<event uid='newest'/>\n");
    QCOMPARE(transport.send(payload), CotOutputTransport::SendResult::Sent);
    QTRY_VERIFY(newest.hasPendingDatagrams());
    QByteArray received(payload.size(), '\0');
    QCOMPARE(newest.readDatagram(received.data(), received.size()), qint64(payload.size()));
    QCOMPARE(received, payload);
    QTest::qWait(20);
    QVERIFY(!first.hasPendingDatagrams());
}

void CotOutputTransportTest::tcpClientConnectsOnceAndDoesNotReconnect()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    auto settings = CotOutputTransport::Defaults(CotOutputTransport::Mode::TcpClient);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = server.serverPort();
    CotOutputTransport transport(settings);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QCOMPARE(transport.state(), CotOutputTransport::State::Connecting);
    QTRY_VERIFY(server.hasPendingConnections());
    std::unique_ptr<QTcpSocket> accepted(server.nextPendingConnection());
    QVERIFY(accepted);
    QTRY_COMPARE(transport.state(), CotOutputTransport::State::Ready);
    QCOMPARE(transport.peerCount(), 1);

    const QByteArray payload("<event uid='tcp-client'/>\n");
    QCOMPARE(transport.send(payload), CotOutputTransport::SendResult::Sent);
    QTRY_COMPARE(accepted->bytesAvailable(), qint64(payload.size()));
    QCOMPARE(accepted->readAll(), payload);

    accepted->abort();
    accepted.reset();
    QTRY_COMPARE(transport.state(), CotOutputTransport::State::Error);
    QVERIFY(!transport.isStarted());
    QVERIFY(transport.lastError().contains(QStringLiteral("reconnect"), Qt::CaseInsensitive));
    QTest::qWait(50);
    QVERIFY(!server.hasPendingConnections()); // Start means one attempt, never an implicit retry.
}

void CotOutputTransportTest::tcpHostFansOutAndRemovesBrokenClients()
{
    auto settings = CotOutputTransport::Defaults(CotOutputTransport::Mode::TcpHost);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 0;
    CotOutputTransport transport(settings);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QVERIFY(transport.localPort() != 0);

    QTcpSocket first;
    QTcpSocket second;
    first.connectToHost(QHostAddress::LocalHost, transport.localPort());
    second.connectToHost(QHostAddress::LocalHost, transport.localPort());
    QTRY_COMPARE(transport.peerCount(), 2);
    QCOMPARE(transport.state(), CotOutputTransport::State::Ready);

    const QByteArray payload("<event uid='fanout'/>\n");
    QSignalSpy sentSpy(&transport, &CotOutputTransport::payloadSent);
    QCOMPARE(transport.send(payload), CotOutputTransport::SendResult::Sent);
    QCOMPARE(sentSpy.count(), 1);
    QCOMPARE(sentSpy.first().at(1).toInt(), 2);
    QTRY_COMPARE(first.bytesAvailable(), qint64(payload.size()));
    QTRY_COMPARE(second.bytesAvailable(), qint64(payload.size()));
    QCOMPARE(first.readAll(), payload);
    QCOMPARE(second.readAll(), payload);

    first.abort();
    QTRY_COMPARE(transport.peerCount(), 1);
    QCOMPARE(transport.send(payload), CotOutputTransport::SendResult::Sent);
    QTRY_COMPARE(second.bytesAvailable(), qint64(payload.size()));
    QCOMPARE(second.readAll(), payload);
    for (const CotOutputTransport::PeerInfo &peer : transport.peers()) {
        QVERIFY(peer.pendingBytes <= CotOutputTransport::PerPeerPendingLimitBytes);
    }

    second.abort();
    QTRY_COMPARE(transport.peerCount(), 0);
    QCOMPARE(transport.state(), CotOutputTransport::State::Listening);
    QCOMPARE(transport.send(payload), CotOutputTransport::SendResult::NoPeer);
}

void CotOutputTransportTest::sendErrorsMayStopTransportReentrantly()
{
    auto settings = CotOutputTransport::Defaults(CotOutputTransport::Mode::UdpClient);
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 14551;
    CotOutputTransport transport(settings);
    QString error;
    QVERIFY2(transport.start(&error), qPrintable(error));
    QObject::connect(&transport, &CotOutputTransport::errorOccurred,
                     &transport, &CotOutputTransport::stop, Qt::DirectConnection);

    QCOMPARE(transport.send(QByteArray("invalid")),
             CotOutputTransport::SendResult::InvalidPayload);
    QCOMPARE(transport.state(), CotOutputTransport::State::Stopped);
    QVERIFY(!transport.hasPeer());
    transport.stop(); // Teardown remains idempotent after re-entrant stop.
}

QTEST_GUILESS_MAIN(CotOutputTransportTest)
#include "test_cotoutputtransport.moc"
