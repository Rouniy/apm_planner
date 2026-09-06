#include "SerialBridgeTcpServer.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

SerialBridgeTcpServer::SerialBridgeTcpServer(QObject *parent) : QObject(parent) {}

SerialBridgeTcpServer::~SerialBridgeTcpServer()
{
    // Do not invoke application callbacks from a partially destroyed facade.
    blockSignals(true);
    stop();
}

bool SerialBridgeTcpServer::start(quint16 port, bool allowRemote, QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread() || m_stopping || m_server) {
        if (error) *error = QStringLiteral("TCP bridge is already running or called from the wrong thread.");
        return false;
    }
    auto *server = new QTcpServer(this);
    server->setMaxPendingConnections(1);
    if (!server->listen(allowRemote ? QHostAddress::AnyIPv4 : QHostAddress::LocalHost, port)) {
        if (error) *error = server->errorString();
        delete server;
        return false;
    }
    m_server = server;
    ++m_generation;
    connect(server, &QTcpServer::newConnection, this, [this, server] {
        if (m_server == server && !m_stopping) acceptConnections();
    });
    connect(server, &QTcpServer::acceptError, this,
            [this, server](QAbstractSocket::SocketError) {
        if (m_server != server || m_stopping) return;
        const QString reason = server->errorString();
        emit failure(QStringLiteral("TCP bridge accept failed: %1").arg(reason));
    });
    return true;
}

void SerialBridgeTcpServer::stop()
{
    if (QThread::currentThread() != thread() || m_stopping) return;
    m_stopping = true;
    ++m_generation;
    const bool hadClient = !m_client.isNull();
    const QPointer<QTcpSocket> socket = m_client;
    const QPointer<QTcpServer> server = m_server;
    const QPointer<SerialBridgeTcpServer> guard(this);
    m_client.clear(); m_server = nullptr; m_inputEnded = false;
    if (socket) {
        disconnect(socket, nullptr, this, nullptr);
        // deleteLater alone is insufficient when the facade itself is deleted
        // from readyRead/error: QObject's child teardown would still destroy
        // this socket before QtNetwork has returned from its notification.
        socket->setParent(nullptr);
        socket->deleteLater();
    }
    if (server) {
        disconnect(server, nullptr, this, nullptr);
        // The same rule applies when clientConnected deletes the facade while
        // QTcpServer is still delivering newConnection.
        server->setParent(nullptr);
        server->deleteLater();
        server->close();
    }
    if (!guard) return;
    if (socket) socket->abort(); // Discard all queued input/output from this client.
    if (!guard) return;
    m_stopping = false;
    if (hadClient) emit clientDisconnected();
}

bool SerialBridgeTcpServer::isListening() const { return m_server && m_server->isListening(); }
bool SerialBridgeTcpServer::hasClient() const
{
    return m_client && (m_inputEnded || m_client->state() == QAbstractSocket::ConnectedState);
}
bool SerialBridgeTcpServer::inputEnded() const { return m_client && m_inputEnded; }

void SerialBridgeTcpServer::finishInput()
{
    if (QThread::currentThread() != thread() || !m_client || !m_inputEnded
        || m_client->bytesAvailable() != 0) return;
    retireClient(true);
}
quint16 SerialBridgeTcpServer::boundPort() const { return isListening() ? m_server->serverPort() : 0; }
QString SerialBridgeTcpServer::peerDescription() const
{
    return hasClient() ? QStringLiteral("%1:%2").arg(m_client->peerAddress().toString()).arg(m_client->peerPort()) : QString();
}
qint64 SerialBridgeTcpServer::pendingBytes() const { return hasClient() ? m_client->bytesToWrite() : 0; }

bool SerialBridgeTcpServer::isCurrent(QTcpSocket *socket, quint64 generation) const
{
    return !m_stopping && m_server && m_generation == generation && m_client == socket;
}

void SerialBridgeTcpServer::retireClient(bool notify)
{
    const QPointer<QTcpSocket> socket = m_client;
    if (!socket) return;
    const QPointer<SerialBridgeTcpServer> guard(this);
    m_client.clear(); m_inputEnded = false; ++m_generation;
    disconnect(socket, nullptr, this, nullptr);
    socket->setParent(nullptr);
    socket->deleteLater(); socket->abort();
    if (guard && notify) emit clientDisconnected();
}

void SerialBridgeTcpServer::endInput(QTcpSocket *socket, quint64 generation)
{
    if (!isCurrent(socket, generation) || m_inputEnded) return;
    // Qt keeps already received bytes readable after a graceful remote close.
    // Keep the bounded socket buffer and exclusive client slot until the owner
    // has submitted its entire paced batch, including the final short chunk.
    m_inputEnded = true;
    emit readyRead(); // Also wake the owner for an empty EOF.
}

void SerialBridgeTcpServer::acceptConnections()
{
    const QPointer<SerialBridgeTcpServer> guard(this);
    const QPointer<QTcpServer> server(m_server);
    while (guard && server && m_server == server && server->hasPendingConnections()) {
        QTcpSocket *socket = server->nextPendingConnection();
        if (!socket) break;
        socket->setReadBufferSize(MaximumInputBytes);
        if (m_client) {
            // A second client cannot evict the established peer or generate a
            // session-fatal failure signal for that peer.
            socket->abort(); socket->deleteLater();
            continue;
        }
        socket->setParent(this);
        m_client = socket;
        m_inputEnded = false;
        const quint64 generation = ++m_generation;
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, generation] {
            if (isCurrent(socket, generation)) emit readyRead();
        });
        connect(socket, &QTcpSocket::bytesWritten, this, [this, socket, generation](qint64 bytes) {
            if (isCurrent(socket, generation)) emit bytesWritten(bytes);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket, generation] {
            endInput(socket, generation);
        });
        const auto onError = [this, socket, generation](QAbstractSocket::SocketError error) {
            if (!isCurrent(socket, generation)) return;
            if (error == QAbstractSocket::RemoteHostClosedError) {
                endInput(socket, generation);
                return;
            }
            const QString reason = socket->errorString();
            const QPointer<SerialBridgeTcpServer> self(this);
            retireClient(true);
            // A disconnect observer may stop/restart and accept another peer.
            if (self && m_generation == generation + 1 && !m_client)
                emit failure(QStringLiteral("TCP bridge client failed: %1").arg(reason));
        };
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(socket, &QAbstractSocket::errorOccurred, this, onError);
#else
        connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this, onError);
#endif
        emit clientConnected();
        if (!guard || !isCurrent(socket, generation)) return;
        // Cover bytes already queued before the readyRead connection existed.
        if (socket->bytesAvailable() > 0) emit readyRead();
        if (!guard || !isCurrent(socket, generation)) return;
    }
}

QByteArray SerialBridgeTcpServer::takeInput(qint64 maximumBytes)
{
    if (QThread::currentThread() != thread() || maximumBytes <= 0 || !hasClient()) return {};
    const QPointer<SerialBridgeTcpServer> guard(this);
    const QPointer<QTcpSocket> socket = m_client;
    const quint64 generation = m_generation;
    const QByteArray bytes = socket->read(qMin(maximumBytes, MaximumInputBytes));
    return guard && socket && isCurrent(socket, generation) ? bytes : QByteArray();
}

bool SerialBridgeTcpServer::sendBytes(const QByteArray &bytes)
{
    if (QThread::currentThread() != thread() || !hasClient() || m_inputEnded) return false;
    if (bytes.size() > MaximumOutputBytes || m_client->bytesToWrite() > MaximumOutputBytes - bytes.size()) return false;
    if (bytes.isEmpty()) return true;
    const QPointer<SerialBridgeTcpServer> guard(this);
    const QPointer<QTcpSocket> socket = m_client;
    const quint64 generation = m_generation;
    const qint64 accepted = socket->write(bytes);
    if (!guard || !socket || !isCurrent(socket, generation)) return false;
    if (accepted == bytes.size()) return true;
    retireClient(true);
    if (guard && m_generation == generation + 1 && !m_client)
        emit failure(QStringLiteral("TCP bridge output was not completely accepted; the client was closed."));
    return false;
}
