#include "comm/SerialBridgeTcpServer.h"

#include <QHostAddress>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

class SerialBridgeTcpServerTest final : public QObject {
    Q_OBJECT
private slots:
    void listenerAdmissionAndAddresses();
    void binaryTrafficAndOutputLimit();
    void inputBackpressureDoesNotDropBytes();
    void secondClientCannotReplaceFirst();
    void gracefulClosePreservesTailBeforeReaccept();
    void stopDuringGracefulDrainDiscardsTail();
    void stopClearsQueuesAndCanRestartFromNotification();
    void callbackLifetime_data();
    void callbackLifetime();
};

void SerialBridgeTcpServerTest::listenerAdmissionAndAddresses()
{
    SerialBridgeTcpServer bridge;
    QVERIFY(!bridge.isListening()); QVERIFY(!bridge.hasClient());
    QCOMPARE(bridge.boundPort(), quint16(0)); QVERIFY(bridge.peerDescription().isEmpty());
    QVERIFY(!bridge.sendBytes("no client")); QVERIFY(bridge.takeInput(280).isEmpty());
    QString error;
    QVERIFY2(bridge.start(0, false, &error), qPrintable(error));
    QVERIFY(error.isEmpty()); QVERIFY(bridge.boundPort() != 0);
    QCOMPARE(bridge.findChild<QTcpServer *>()->serverAddress(), QHostAddress(QHostAddress::LocalHost));
    const quint16 port = bridge.boundPort();
    QVERIFY(!bridge.start(0, true, &error)); QVERIFY(!error.isEmpty());
    QCOMPARE(bridge.boundPort(), port);
    SerialBridgeTcpServer occupied;
    QVERIFY(!occupied.start(port, false, &error)); QVERIFY(!error.isEmpty());
    QVERIFY(!occupied.isListening()); QCOMPARE(occupied.boundPort(), quint16(0));
    bridge.stop(); QVERIFY(!bridge.isListening());
    SerialBridgeTcpServer exposed;
    QVERIFY(exposed.start(0, true));
    QCOMPARE(exposed.findChild<QTcpServer *>()->serverAddress(), QHostAddress(QHostAddress::AnyIPv4));
}

void SerialBridgeTcpServerTest::binaryTrafficAndOutputLimit()
{
    SerialBridgeTcpServer bridge; QTcpSocket peer;
    QSignalSpy connected(&bridge, &SerialBridgeTcpServer::clientConnected);
    QSignalSpy ready(&bridge, &SerialBridgeTcpServer::readyRead);
    QSignalSpy written(&bridge, &SerialBridgeTcpServer::bytesWritten);
    QSignalSpy failures(&bridge, &SerialBridgeTcpServer::failure);
    QVERIFY(bridge.start(0, false)); peer.connectToHost(QHostAddress::LocalHost, bridge.boundPort());
    QTRY_VERIFY(bridge.hasClient()); QCOMPARE(connected.size(), 1);
    QVERIFY(bridge.peerDescription().startsWith("127.0.0.1:"));
    const QByteArray input = QByteArray::fromHex("0001027f80ff0d0a00") + QByteArray(600, char(0xff));
    QCOMPARE(peer.write(input), qint64(input.size())); QTRY_VERIFY(!ready.isEmpty());
    QVERIFY(bridge.takeInput(0).isEmpty()); QVERIFY(bridge.takeInput(-1).isEmpty());
    QByteArray received;
    const auto pull = [&] { const auto chunk = bridge.takeInput(280); received += chunk; return received.size(); };
    QTRY_COMPARE(pull(), input.size()); QCOMPARE(received, input);
    QVERIFY(bridge.takeInput(280).isEmpty());

    QVERIFY(!bridge.sendBytes(QByteArray(int(SerialBridgeTcpServer::MaximumOutputBytes + 1), 'x')));
    QCOMPARE(bridge.pendingBytes(), qint64(0)); QCOMPARE(peer.bytesAvailable(), qint64(0));
    QByteArray output(int(SerialBridgeTcpServer::MaximumOutputBytes), 'b');
    output.replace(0, 4, QByteArray::fromHex("00ff7f80"));
    QVERIFY(bridge.sendBytes(output));
    const qint64 pending = bridge.pendingBytes();
    QVERIFY(pending >= 0 && pending <= SerialBridgeTcpServer::MaximumOutputBytes);
    // Whatever the OS has already accepted, exceeding the remaining queue
    // capacity must neither append a prefix nor disturb previously queued data.
    const QByteArray overflow(int(SerialBridgeTcpServer::MaximumOutputBytes - pending + 1), 'x');
    QVERIFY(!bridge.sendBytes(overflow)); QCOMPARE(bridge.pendingBytes(), pending);
    QVERIFY(bridge.sendBytes(QByteArray()));
    QTRY_COMPARE(peer.bytesAvailable(), qint64(output.size()));
    QCOMPARE(peer.readAll(), output); QTRY_COMPARE(bridge.pendingBytes(), qint64(0));
    QTRY_VERIFY(!written.isEmpty()); QCOMPARE(failures.size(), 0);
}

void SerialBridgeTcpServerTest::inputBackpressureDoesNotDropBytes()
{
    SerialBridgeTcpServer bridge; QTcpSocket peer;
    QVERIFY(bridge.start(0, false)); peer.connectToHost(QHostAddress::LocalHost, bridge.boundPort());
    QTRY_VERIFY(bridge.hasClient());
    auto *accepted = bridge.findChild<QTcpSocket *>(); QVERIFY(accepted);
    QCOMPARE(accepted->readBufferSize(), SerialBridgeTcpServer::MaximumInputBytes);
    QByteArray input(int(SerialBridgeTcpServer::MaximumInputBytes * 4), '\0');
    for (int i = 0; i < input.size(); ++i) input[i] = char(i % 251);
    QCOMPARE(peer.write(input), qint64(input.size()));
    peer.disconnectFromHost(); // FIN follows all buffered bytes, including data beyond our read cap.
    QTRY_VERIFY(accepted->bytesAvailable() > 0);
    QVERIFY(accepted->bytesAvailable() <= SerialBridgeTcpServer::MaximumInputBytes);
    QByteArray received;
    bool bounded = true;
    const auto pull = [&] {
        bounded &= accepted->bytesAvailable() <= SerialBridgeTcpServer::MaximumInputBytes;
        const QByteArray chunk = bridge.takeInput(1000000);
        bounded &= chunk.size() <= SerialBridgeTcpServer::MaximumInputBytes;
        received += chunk; return received.size();
    };
    QTRY_COMPARE(pull(), input.size()); QVERIFY(bounded); QCOMPARE(received, input);
    QTRY_VERIFY(bridge.inputEnded()); QVERIFY(bridge.hasClient());
    bridge.finishInput(); QVERIFY(!bridge.hasClient());
}

void SerialBridgeTcpServerTest::secondClientCannotReplaceFirst()
{
    SerialBridgeTcpServer bridge; QTcpSocket first, second;
    QSignalSpy connections(&bridge, &SerialBridgeTcpServer::clientConnected);
    QSignalSpy disconnections(&bridge, &SerialBridgeTcpServer::clientDisconnected);
    QSignalSpy failures(&bridge, &SerialBridgeTcpServer::failure);
    QVERIFY(bridge.start(0, false)); first.connectToHost(QHostAddress::LocalHost, bridge.boundPort());
    QTRY_VERIFY(bridge.hasClient()); const QString original = bridge.peerDescription();
    second.connectToHost(QHostAddress::LocalHost, bridge.boundPort());
    QTRY_COMPARE(second.state(), QAbstractSocket::UnconnectedState);
    QVERIFY(bridge.hasClient()); QCOMPARE(bridge.peerDescription(), original);
    QCOMPARE(connections.size(), 1); QCOMPARE(disconnections.size(), 0); QCOMPARE(failures.size(), 0);
    QVERIFY(bridge.sendBytes("original")); QTRY_COMPARE(first.bytesAvailable(), qint64(8));
    QCOMPARE(first.readAll(), QByteArray("original"));
}

void SerialBridgeTcpServerTest::gracefulClosePreservesTailBeforeReaccept()
{
    SerialBridgeTcpServer bridge; QTcpSocket first, premature, replacement;
    QSignalSpy ready(&bridge, &SerialBridgeTcpServer::readyRead);
    QSignalSpy disconnected(&bridge, &SerialBridgeTcpServer::clientDisconnected);
    QVERIFY(bridge.start(0, false)); first.connectToHost(QHostAddress::LocalHost, bridge.boundPort());
    QTRY_VERIFY(bridge.hasClient()); QVERIFY(!bridge.inputEnded());
    bridge.finishInput(); QVERIFY(bridge.hasClient()); // Not an EOF yet.
    QByteArray tail(4097, '\0');
    for (int i = 0; i < tail.size(); ++i) tail[i] = char(i % 251);
    QCOMPARE(first.write(tail), qint64(tail.size()));
    first.disconnectFromHost();
    QTRY_VERIFY(bridge.inputEnded()); QVERIFY(bridge.hasClient());
    QVERIFY(!ready.isEmpty()); QCOMPARE(disconnected.size(), 0);
    QVERIFY(!bridge.sendBytes("peer input has ended"));
    bridge.finishInput(); QVERIFY(bridge.hasClient()); // Unread tail cannot be discarded.
    premature.connectToHost(QHostAddress::LocalHost, bridge.boundPort());
    QTRY_COMPARE(premature.state(), QAbstractSocket::UnconnectedState);
    QVERIFY(bridge.hasClient()); QVERIFY(bridge.inputEnded());
    QByteArray received;
    while (received.size() < tail.size()) {
        const QByteArray chunk = bridge.takeInput(70);
        QVERIFY(!chunk.isEmpty()); QVERIFY(chunk.size() <= 70); received += chunk;
    }
    QCOMPARE(received, tail); QVERIFY(bridge.takeInput(280).isEmpty());
    // Empty socket is insufficient until the owner confirms its paced batch.
    QCOMPARE(disconnected.size(), 0); QVERIFY(bridge.hasClient());
    bridge.finishInput(); QCOMPARE(disconnected.size(), 1); QVERIFY(!bridge.hasClient());
    QVERIFY(!bridge.inputEnded()); bridge.finishInput(); QCOMPARE(disconnected.size(), 1);
    QVERIFY(bridge.takeInput(280).isEmpty()); QCOMPARE(bridge.pendingBytes(), qint64(0));
    replacement.connectToHost(QHostAddress::LocalHost, bridge.boundPort()); QTRY_VERIFY(bridge.hasClient());
    QVERIFY(bridge.takeInput(280).isEmpty());
    ready.clear(); replacement.write("new"); QTRY_VERIFY(!ready.isEmpty());
    QCOMPARE(bridge.takeInput(280), QByteArray("new"));
}

void SerialBridgeTcpServerTest::stopDuringGracefulDrainDiscardsTail()
{
    SerialBridgeTcpServer bridge; QTcpSocket peer;
    QSignalSpy disconnected(&bridge, &SerialBridgeTcpServer::clientDisconnected);
    QVERIFY(bridge.start(0, false)); peer.connectToHost(QHostAddress::LocalHost, bridge.boundPort());
    QTRY_VERIFY(bridge.hasClient());
    peer.write(QByteArray(2000, char(0xff))); peer.disconnectFromHost();
    QTRY_VERIFY(bridge.inputEnded()); QCOMPARE(bridge.takeInput(70).size(), 70);
    bridge.stop(); QCOMPARE(disconnected.size(), 1);
    QVERIFY(!bridge.hasClient()); QVERIFY(!bridge.inputEnded()); QVERIFY(!bridge.isListening());
    QVERIFY(bridge.takeInput(280).isEmpty()); bridge.finishInput(); QCOMPARE(disconnected.size(), 1);
    QVERIFY(bridge.start(0, false)); QVERIFY(!bridge.inputEnded());
}

void SerialBridgeTcpServerTest::stopClearsQueuesAndCanRestartFromNotification()
{
    SerialBridgeTcpServer bridge; QTcpSocket first, replacement;
    QVERIFY(bridge.start(0, false)); first.connectToHost(QHostAddress::LocalHost, bridge.boundPort());
    QTRY_VERIFY(bridge.hasClient());
    QVERIFY(bridge.sendBytes(QByteArray(32000, 'q')));
    bool restarted = false, observedCleared = false;
    const auto connection = connect(&bridge, &SerialBridgeTcpServer::clientDisconnected, &bridge, [&] {
        observedCleared = !bridge.isListening() && !bridge.hasClient() && bridge.pendingBytes() == 0
            && bridge.takeInput(280).isEmpty();
        restarted = bridge.start(0, false);
    });
    bridge.stop(); QVERIFY(observedCleared); QVERIFY(restarted); QVERIFY(bridge.isListening());
    disconnect(connection);
    replacement.connectToHost(QHostAddress::LocalHost, bridge.boundPort()); QTRY_VERIFY(bridge.hasClient());
    QVERIFY(bridge.takeInput(280).isEmpty()); QCOMPARE(bridge.pendingBytes(), qint64(0));
    QVERIFY(bridge.sendBytes("replacement")); QTRY_COMPARE(replacement.bytesAvailable(), qint64(11));
    QCOMPARE(replacement.readAll(), QByteArray("replacement"));
    QSignalSpy disconnected(&bridge, &SerialBridgeTcpServer::clientDisconnected);
    bridge.stop(); bridge.stop(); QCOMPARE(disconnected.size(), 1);
    QVERIFY(!bridge.isListening()); QVERIFY(!bridge.hasClient());
}

void SerialBridgeTcpServerTest::callbackLifetime_data()
{
    QTest::addColumn<QString>("notification"); QTest::addColumn<bool>("destroy");
    for (const QString &name : {QString("connected"), QString("ready"), QString("written"), QString("ended"), QString("disconnected")}) {
        QTest::newRow(qPrintable(name + "-stop")) << name << false;
        QTest::newRow(qPrintable(name + "-delete")) << name << true;
    }
}

void SerialBridgeTcpServerTest::callbackLifetime()
{
    QFETCH(QString, notification); QFETCH(bool, destroy);
    QPointer<SerialBridgeTcpServer> bridge = new SerialBridgeTcpServer;
    QTcpSocket peer; bool called = false;
    QPointer<QTcpSocket> eventSocket;
    QPointer<QTcpServer> eventListener;
    bool socketSurvivedCallback = false, listenerSurvivedCallback = false;
    const auto callback = [&] {
        if (called) return;
        called = true;
        eventSocket = bridge->findChild<QTcpSocket *>();
        eventListener = bridge->findChild<QTcpServer *>();
        if (destroy) delete bridge.data(); else bridge->stop();
        // Underlying QtNetwork signal frames may still use their sender after
        // this callback returns. Facade teardown must detach and defer those
        // objects, not destroy them synchronously through QObject parenting.
        socketSurvivedCallback = !eventSocket.isNull();
        listenerSurvivedCallback = !eventListener.isNull();
    };
    if (notification == "connected") connect(bridge, &SerialBridgeTcpServer::clientConnected, this, callback);
    if (notification == "ready") connect(bridge, &SerialBridgeTcpServer::readyRead, this, callback);
    if (notification == "written") connect(bridge, &SerialBridgeTcpServer::bytesWritten, this, callback);
    if (notification == "ended") connect(bridge, &SerialBridgeTcpServer::readyRead, this, [&] {
        if (bridge && bridge->inputEnded()) callback();
    });
    if (notification == "disconnected") connect(bridge, &SerialBridgeTcpServer::clientDisconnected, this, callback);
    QVERIFY(bridge->start(0, false)); peer.connectToHost(QHostAddress::LocalHost, bridge->boundPort());
    if (notification != "connected") {
        QTRY_VERIFY(bridge && bridge->hasClient());
        if (notification == "ready") peer.write("trigger");
        if (notification == "written") bridge->sendBytes("trigger");
        if (notification == "ended") peer.disconnectFromHost();
        if (notification == "disconnected") {
            peer.disconnectFromHost(); QTRY_VERIFY(bridge && bridge->inputEnded());
            bridge->finishInput();
        }
    }
    QTRY_VERIFY(called);
    QVERIFY(listenerSurvivedCallback);
    if (notification != "disconnected") QVERIFY(socketSurvivedCallback);
    if (destroy) QVERIFY(bridge.isNull());
    else { QVERIFY(bridge); QVERIFY(!bridge->isListening()); QVERIFY(!bridge->hasClient()); delete bridge.data(); }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(eventSocket.isNull()); QVERIFY(eventListener.isNull());
}

QTEST_GUILESS_MAIN(SerialBridgeTcpServerTest)
#include "test_serialbridgetcpserver.moc"
