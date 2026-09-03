#include <QtTest>

#include "comm/MavlinkMirrorOutput.h"

#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QUdpSocket>

class MavlinkMirrorOutputTest final : public QObject
{
    Q_OBJECT

private slots:
    void selectionsAndFactoryFollowMp10();
    void serialMissingPortFailsToOpen();
    void tcpHostServesOneClientNewestWins();
    void tcpHostBoundsPendingBytes();
    void udpHostLearnsPeerFromDatagrams();
    void udpHostCapsLearnedPeers();
};

void MavlinkMirrorOutputTest::selectionsAndFactoryFollowMp10()
{
    QCOMPARE(MavlinkMirrorOutput::TcpHostSelection(), QStringLiteral("TCP Host - 14550"));
    QCOMPARE(MavlinkMirrorOutput::UdpHostSelection(), QStringLiteral("UDP Host - 14550"));
    QCOMPARE(MavlinkMirrorOutput::Selections({}),
             (QStringList{QStringLiteral("TCP Host - 14550"), QStringLiteral("UDP Host - 14550")}));
    QCOMPARE(MavlinkMirrorOutput::Selections({QStringLiteral("ttyUSB0"), QStringLiteral("ttyUSB0"),
                                              QStringLiteral("ttyACM1")}),
             (QStringList{QStringLiteral("ttyUSB0"), QStringLiteral("ttyACM1"),
                          QStringLiteral("TCP Host - 14550"), QStringLiteral("UDP Host - 14550")}));

    QString error;
    MavlinkMirrorSettings settings;
    settings.portSelection = MavlinkMirrorOutput::TcpHostSelection();
    std::unique_ptr<MavlinkMirrorOutput> tcp = MavlinkMirrorOutput::Create(settings, &error);
    QVERIFY(tcp);
    QCOMPARE(tcp->kind(), MavlinkMirrorOutput::Kind::TcpHost);
    QVERIFY(!tcp->isOpen());   // creating never listens
    QCOMPARE(tcp->statusText(), QStringLiteral("Stopped."));

    settings.portSelection = MavlinkMirrorOutput::UdpHostSelection();
    std::unique_ptr<MavlinkMirrorOutput> udp = MavlinkMirrorOutput::Create(settings, &error);
    QVERIFY(udp);
    QCOMPARE(udp->kind(), MavlinkMirrorOutput::Kind::UdpHost);
    QVERIFY(!udp->isOpen());   // creating never binds

    settings.portSelection = QStringLiteral("ttyS99");
    settings.baud = 57600;
    std::unique_ptr<MavlinkMirrorOutput> serial = MavlinkMirrorOutput::Create(settings, &error);
    QVERIFY(serial);
    QCOMPARE(serial->kind(), MavlinkMirrorOutput::Kind::Serial);
    QCOMPARE(serial->selection(), QStringLiteral("ttyS99"));

    settings.portSelection = QStringLiteral("   ");
    QVERIFY(!MavlinkMirrorOutput::Create(settings, &error));
    QCOMPARE(error, QStringLiteral("No port selected."));
    settings.portSelection = QStringLiteral("ttyS99");
    settings.baud = 0;
    QVERIFY(!MavlinkMirrorOutput::Create(settings, &error));
    QVERIFY(error.contains(QStringLiteral("baud")));
}

void MavlinkMirrorOutputTest::serialMissingPortFailsToOpen()
{
    SerialMirrorOutput output(QStringLiteral("/dev/apm-planner-no-such-port"), 115200);
    QString error;
    QVERIFY(!output.open(&error));
    QVERIFY2(error.contains(QStringLiteral("apm-planner-no-such-port")), qPrintable(error));
    QVERIFY(!output.isOpen());
    QVERIFY(!output.hasPeer());
    QCOMPARE(output.write(QByteArray("x")), qint64(-1));
    QCOMPARE(output.statusText(), QStringLiteral("Stopped."));
    output.close();   // idempotent
}

void MavlinkMirrorOutputTest::tcpHostServesOneClientNewestWins()
{
    TcpHostMirrorOutput output(0, QHostAddress::LocalHost);
    QSignalSpy peerSpy(&output, &MavlinkMirrorOutput::peerChanged);
    QSignalSpy rxSpy(&output, &MavlinkMirrorOutput::peerBytesReceived);
    QString error;
    QVERIFY2(output.open(&error), qPrintable(error));
    QVERIFY(output.isOpen());
    QVERIFY(!output.hasPeer());
    const quint16 port = output.serverPort();
    QVERIFY(port != 0);
    QCOMPARE(output.statusText(),
             QStringLiteral("Listening on TCP %1 — waiting for a client…").arg(port));
    QCOMPARE(output.write(QByteArray("dropped")), qint64(0));   // nobody connected

    QTcpSocket first;
    first.connectToHost(QHostAddress::LocalHost, port);
    QTRY_VERIFY(output.hasPeer());
    QCOMPARE(peerSpy.count(), 1);
    QTRY_COMPARE(first.state(), QAbstractSocket::ConnectedState);
    QVERIFY(output.statusText().startsWith(QStringLiteral("Mirroring on TCP %1 (client ").arg(port)));

    const QByteArray frame = QByteArray::fromHex("fd09000000010100000000000000000000000000");
    QCOMPARE(output.write(frame), qint64(frame.size()));
    QTRY_COMPARE(first.bytesAvailable(), qint64(frame.size()));
    QCOMPARE(first.readAll(), frame);

    first.write("peer-bytes");
    const auto received = [&rxSpy]() {
        QByteArray all;
        for (const QList<QVariant> &args : rxSpy) {
            all += args.at(0).toByteArray();
        }
        return all;
    };
    QTRY_COMPARE(received(), QByteArray("peer-bytes"));   // may arrive in several reads

    // MP10 TcpSerialHostListener: a new client replaces and closes the previous socket.
    QTcpSocket second;
    second.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(second.state(), QAbstractSocket::ConnectedState);
    QTRY_COMPARE(first.state(), QAbstractSocket::UnconnectedState);
    QTRY_VERIFY(output.hasPeer());
    QCOMPARE(output.write(frame), qint64(frame.size()));
    QTRY_COMPARE(second.bytesAvailable(), qint64(frame.size()));
    QCOMPARE(second.readAll(), frame);

    // Client goes away: back to listening.
    second.disconnectFromHost();
    QTRY_VERIFY(!output.hasPeer());
    QVERIFY(output.statusText().startsWith(QStringLiteral("Listening on TCP")));

    output.close();
    QVERIFY(!output.isOpen());
    QCOMPARE(output.statusText(), QStringLiteral("Stopped."));
}

void MavlinkMirrorOutputTest::tcpHostBoundsPendingBytes()
{
    TcpHostMirrorOutput output(0, QHostAddress::LocalHost);
    QString error;
    QVERIFY2(output.open(&error), qPrintable(error));
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, output.serverPort());
    QTRY_VERIFY(output.hasPeer());

    // Without running the event loop nothing is flushed, so the output must stop
    // accepting once OutputPendingLimitBytes are pending.
    const QByteArray chunk(16 * 1024, 'm');
    qint64 accepted = 0;
    for (int i = 0; i < 8; ++i) {
        const qint64 written = output.write(chunk);
        QVERIFY(written >= 0);
        accepted += written;
    }
    QVERIFY(accepted <= MavlinkMirrorOutput::OutputPendingLimitBytes);
    QVERIFY(output.pendingBytes() <= MavlinkMirrorOutput::OutputPendingLimitBytes);
    // A partial acceptance is allowed but must never exceed the room left.
    const qint64 extra = output.write(chunk);
    QVERIFY(extra >= 0 && extra <= chunk.size());
    QVERIFY(output.pendingBytes() <= MavlinkMirrorOutput::OutputPendingLimitBytes);
    QTRY_VERIFY(client.bytesAvailable() > 0);
    output.close();
}

void MavlinkMirrorOutputTest::udpHostLearnsPeerFromDatagrams()
{
    UdpHostMirrorOutput output(0, QHostAddress::LocalHost);
    QSignalSpy peerSpy(&output, &MavlinkMirrorOutput::peerChanged);
    QSignalSpy rxSpy(&output, &MavlinkMirrorOutput::peerBytesReceived);
    QString error;
    QVERIFY2(output.open(&error), qPrintable(error));
    QVERIFY(output.isOpen());
    QVERIFY(!output.hasPeer());
    const quint16 port = output.localPort();
    QVERIFY(port != 0);
    QCOMPARE(output.statusText(),
             QStringLiteral("Listening on UDP %1 — waiting for a client…").arg(port));
    QCOMPARE(output.write(QByteArray("silent")), qint64(0));   // no peer yet: nothing is sent

    QUdpSocket client;
    QVERIFY(client.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QCOMPARE(client.writeDatagram(QByteArray("hello"), QHostAddress::LocalHost, port), qint64(5));
    QTRY_VERIFY(output.hasPeer());
    QCOMPARE(peerSpy.count(), 1);
    QCOMPARE(output.peerCount(), 1);
    QCOMPARE(output.peers().first().port, client.localPort());
    QTRY_COMPARE(rxSpy.count(), 1);
    QCOMPARE(rxSpy.takeFirst().at(0).toByteArray(), QByteArray("hello"));
    QCOMPARE(output.statusText(),
             QStringLiteral("Mirroring on UDP %1 (client %2:%3 connected).")
                 .arg(port).arg(QHostAddress(QHostAddress::LocalHost).toString()).arg(client.localPort()));

    const QByteArray frame = QByteArray::fromHex("fe0900000101000000000000000000ffff");
    const auto receiveOne = [](QUdpSocket &socket) {
        QByteArray received(static_cast<int>(socket.pendingDatagramSize()), '\0');
        socket.readDatagram(received.data(), received.size());
        return received;
    };
    QCOMPARE(output.write(frame), qint64(frame.size()));
    QTRY_VERIFY(client.hasPendingDatagrams());
    QCOMPARE(receiveOne(client), frame);

    // MP10 EndPointList: a second distinct sender is added, both receive every frame.
    QUdpSocket other;
    QVERIFY(other.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    other.writeDatagram(QByteArray("me"), QHostAddress::LocalHost, port);
    QTRY_COMPARE(output.peerCount(), 2);
    QCOMPARE(peerSpy.count(), 2);
    QCOMPARE(output.statusText(),
             QStringLiteral("Mirroring on UDP %1 (2 clients connected).").arg(port));
    client.writeDatagram(QByteArray("again"), QHostAddress::LocalHost, port);   // known sender
    QTRY_COMPARE(rxSpy.count(), 2);
    QCOMPARE(output.peerCount(), 2);
    QCOMPARE(peerSpy.count(), 2);
    QCOMPARE(output.write(frame), qint64(frame.size()));   // counted once, delivered twice
    QTRY_VERIFY(client.hasPendingDatagrams());
    QTRY_VERIFY(other.hasPendingDatagrams());
    QCOMPARE(receiveOne(client), frame);
    QCOMPARE(receiveOne(other), frame);

    output.close();
    QVERIFY(!output.isOpen());
    QVERIFY(!output.hasPeer());
    QCOMPARE(output.peerCount(), 0);
    QCOMPARE(peerSpy.count(), 3);
}

void MavlinkMirrorOutputTest::udpHostCapsLearnedPeers()
{
    UdpHostMirrorOutput output(0, QHostAddress::LocalHost);
    QString error;
    QVERIFY2(output.open(&error), qPrintable(error));
    const quint16 port = output.localPort();
    QSignalSpy rxSpy(&output, &MavlinkMirrorOutput::peerBytesReceived);

    QList<QUdpSocket *> senders;
    for (int i = 0; i < UdpHostMirrorOutput::MaxPeers + 1; ++i) {
        auto *sender = new QUdpSocket(this);
        QVERIFY(sender->bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
        sender->writeDatagram(QByteArray(1, static_cast<char>(i)), QHostAddress::LocalHost, port);
        senders.append(sender);
    }
    // Every datagram carries one byte, so the receive count is the sync point
    // for "all 33 senders have been seen".
    QTRY_COMPARE(rxSpy.count(), UdpHostMirrorOutput::MaxPeers + 1);
    QCOMPARE(output.peerCount(), UdpHostMirrorOutput::MaxPeers);
    // The oldest learned peer was evicted, the newest is present.
    const QList<UdpHostMirrorOutput::Peer> peers = output.peers();
    bool oldestPresent = false;
    bool newestPresent = false;
    for (const UdpHostMirrorOutput::Peer &peer : peers) {
        oldestPresent = oldestPresent || peer.port == senders.first()->localPort();
        newestPresent = newestPresent || peer.port == senders.last()->localPort();
    }
    QVERIFY(!oldestPresent);
    QVERIFY(newestPresent);

    const QByteArray frame = QByteArray::fromHex("fd0300000001010000000000");
    QCOMPARE(output.write(frame), qint64(frame.size()));
    QTRY_VERIFY(senders.last()->hasPendingDatagrams());
    QTest::qWait(20);
    QVERIFY(!senders.first()->hasPendingDatagrams());   // evicted peers get nothing
    qDeleteAll(senders);
    output.close();
}

QTEST_GUILESS_MAIN(MavlinkMirrorOutputTest)
#include "test_mavlinkmirroroutput.moc"
