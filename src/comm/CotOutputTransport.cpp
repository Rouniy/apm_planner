#include "CotOutputTransport.h"

#include <QAbstractSocket>
#include <QHostInfo>
#include <QIODevice>
#include <QNetworkDatagram>
#include <QPointer>
#include <QSerialPort>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <limits>

namespace {

QString endpointText(const QHostAddress &address, quint16 port)
{
    const QString host = address.protocol() == QAbstractSocket::IPv6Protocol
        ? QStringLiteral("[%1]").arg(address.toString())
        : address.toString();
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

QString configuredEndpointText(const QString &host, quint16 port)
{
    return QStringLiteral("%1:%2").arg(host.trimmed()).arg(port);
}

bool parseListenAddress(const QString &text, QHostAddress *address)
{
    const QString host = text.trimmed();
    if (host.isEmpty() || host == QStringLiteral("0.0.0.0") || host == QStringLiteral("*")) {
        *address = QHostAddress::AnyIPv4;
        return true;
    }
    return address->setAddress(host);
}

QString socketPeerText(const QTcpSocket *socket)
{
    if (!socket) {
        return QString();
    }
    return endpointText(socket->peerAddress(), socket->peerPort());
}

} // namespace

QString CotOutputTransport::ModeLabel(Mode mode)
{
    switch (mode) {
    case Mode::TakMulticast:
        return QStringLiteral("TAK Multicast");
    case Mode::UdpClient:
        return QStringLiteral("UDP Client");
    case Mode::UdpHost:
        return QStringLiteral("UDP Host");
    case Mode::TcpClient:
        return QStringLiteral("TCP Client");
    case Mode::TcpHost:
        return QStringLiteral("TCP Host");
    case Mode::Serial:
        return QStringLiteral("Serial");
    }
    return QString();
}

CotOutputTransport::Settings CotOutputTransport::Defaults(Mode mode)
{
    Settings result;
    result.mode = mode;
    result.baud = DefaultSerialBaud;
    switch (mode) {
    case Mode::TakMulticast:
        result.host = QStringLiteral("239.2.3.1");
        result.port = DefaultTakPort;
        break;
    case Mode::UdpClient:
    case Mode::TcpClient:
        result.host = QStringLiteral("127.0.0.1");
        result.port = DefaultNetworkPort;
        break;
    case Mode::UdpHost:
    case Mode::TcpHost:
        result.host = QStringLiteral("0.0.0.0");
        result.port = DefaultNetworkPort;
        break;
    case Mode::Serial:
        result.host.clear();
        result.port = 0;
        break;
    }
    return result;
}

CotOutputTransport::CotOutputTransport(const Settings &settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
}

CotOutputTransport::~CotOutputTransport()
{
    shutdown(false);
}

bool CotOutputTransport::isStarted() const
{
    return m_state == State::Resolving || m_state == State::Connecting
        || m_state == State::Listening || m_state == State::Ready;
}

bool CotOutputTransport::hasPeer() const
{
    return peerCount() != 0;
}

int CotOutputTransport::peerCount() const
{
    switch (m_settings.mode) {
    case Mode::TakMulticast:
    case Mode::UdpClient:
    case Mode::UdpHost:
        return m_udp && m_hasUdpPeer ? 1 : 0;
    case Mode::TcpClient:
        return m_tcpClient && m_tcpClient->state() == QAbstractSocket::ConnectedState ? 1 : 0;
    case Mode::TcpHost:
        return m_tcpClients.size();
    case Mode::Serial:
        return m_serial && m_serial->isOpen() ? 1 : 0;
    }
    return 0;
}

QList<CotOutputTransport::PeerInfo> CotOutputTransport::peers() const
{
    QList<PeerInfo> result;
    switch (m_settings.mode) {
    case Mode::TakMulticast:
    case Mode::UdpClient:
    case Mode::UdpHost:
        if (m_udp && m_hasUdpPeer) {
            PeerInfo peer;
            peer.address = m_udpPeerAddress;
            peer.port = m_udpPeerPort;
            peer.description = endpointText(peer.address, peer.port);
            peer.pendingBytes = m_udp->bytesToWrite();
            result.append(peer);
        }
        break;
    case Mode::TcpClient:
        if (m_tcpClient && m_tcpClient->state() == QAbstractSocket::ConnectedState) {
            PeerInfo peer;
            peer.address = m_tcpClient->peerAddress();
            peer.port = m_tcpClient->peerPort();
            peer.description = endpointText(peer.address, peer.port);
            peer.pendingBytes = m_tcpClient->bytesToWrite();
            result.append(peer);
        }
        break;
    case Mode::TcpHost:
        for (QTcpSocket *client : m_tcpClients) {
            if (!client || client->state() != QAbstractSocket::ConnectedState) {
                continue;
            }
            PeerInfo peer;
            peer.address = client->peerAddress();
            peer.port = client->peerPort();
            peer.description = endpointText(peer.address, peer.port);
            peer.pendingBytes = client->bytesToWrite();
            result.append(peer);
        }
        break;
    case Mode::Serial:
        if (m_serial && m_serial->isOpen()) {
            PeerInfo peer;
            peer.description = m_settings.serialPort;
            peer.pendingBytes = m_serial->bytesToWrite();
            result.append(peer);
        }
        break;
    }
    return result;
}

qint64 CotOutputTransport::pendingBytes() const
{
    qint64 result = 0;
    const QList<PeerInfo> currentPeers = peers();
    for (const PeerInfo &peer : currentPeers) {
        if (peer.pendingBytes > std::numeric_limits<qint64>::max() - result) {
            return std::numeric_limits<qint64>::max();
        }
        result += peer.pendingBytes;
    }
    return result;
}

quint16 CotOutputTransport::localPort() const
{
    if (m_settings.mode == Mode::UdpHost && m_udp) {
        return m_udp->localPort();
    }
    if (m_settings.mode == Mode::TcpHost && m_tcpServer) {
        return m_tcpServer->serverPort();
    }
    return 0;
}

bool CotOutputTransport::validateSettings(QString *error) const
{
    switch (m_settings.mode) {
    case Mode::TakMulticast:
    case Mode::UdpClient:
    case Mode::TcpClient:
        if (m_settings.host.trimmed().isEmpty()) {
            if (error) {
                *error = QStringLiteral("CoT destination host is empty.");
            }
            return false;
        }
        if (m_settings.port == 0) {
            if (error) {
                *error = QStringLiteral("CoT destination port must be between 1 and 65535.");
            }
            return false;
        }
        if (m_settings.mode == Mode::TakMulticast) {
            QHostAddress multicastAddress;
            if (multicastAddress.setAddress(m_settings.host.trimmed())
                && !multicastAddress.isMulticast()) {
                if (error) {
                    *error = QStringLiteral("TAK Multicast requires a multicast destination address.");
                }
                return false;
            }
        }
        return true;
    case Mode::UdpHost:
    case Mode::TcpHost: {
        QHostAddress listenAddress;
        if (!parseListenAddress(m_settings.host, &listenAddress)) {
            if (error) {
                *error = QStringLiteral("CoT listen address is not a numeric IP address: %1")
                             .arg(m_settings.host);
            }
            return false;
        }
        return true; // Port zero deliberately requests an ephemeral test port.
    }
    case Mode::Serial:
        if (m_settings.serialPort.trimmed().isEmpty()) {
            if (error) {
                *error = QStringLiteral("No CoT serial port selected.");
            }
            return false;
        }
        if (m_settings.baud <= 0) {
            if (error) {
                *error = QStringLiteral("Invalid CoT serial baud rate %1.").arg(m_settings.baud);
            }
            return false;
        }
        return true;
    }
    if (error) {
        *error = QStringLiteral("Unknown CoT transport mode.");
    }
    return false;
}

bool CotOutputTransport::start(QString *error)
{
    // Restarting is supported, but old asynchronous callbacks are invalidated
    // and disconnected before any new object is installed.
    shutdown(false);
    m_lastError.clear();
    if (error) {
        error->clear();
    }

    QString localError;
    if (!validateSettings(&localError)) {
        if (error) {
            *error = localError;
        }
        fail(localError);
        return false;
    }

    QPointer<CotOutputTransport> guard(this);
    bool started = false;
    switch (m_settings.mode) {
    case Mode::TakMulticast:
    case Mode::UdpClient:
        started = startUdpDestination(&localError);
        break;
    case Mode::UdpHost:
        started = startUdpHost(&localError);
        break;
    case Mode::TcpClient:
        started = startTcpClient(&localError);
        break;
    case Mode::TcpHost:
        started = startTcpHost(&localError);
        break;
    case Mode::Serial:
        started = startSerial(&localError);
        break;
    }
    if (!guard) {
        return false;
    }
    if (!started && m_state == State::Stopped
        && (localError.isEmpty() || localError.endsWith(QStringLiteral("was cancelled.")))) {
        if (error) {
            *error = localError;
        }
        return false;
    }
    if (!started && m_state != State::Error) {
        if (error) {
            *error = localError;
        }
        fail(localError.isEmpty() ? QStringLiteral("Unable to start CoT transport.") : localError);
    }
    return started;
}

bool CotOutputTransport::startUdpDestination(QString *error)
{
    auto *socket = new QUdpSocket(this);
    m_udp = socket;
    connect(socket, &QUdpSocket::bytesWritten, this, &CotOutputTransport::bytesWritten);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this,
            [this, socket](QAbstractSocket::SocketError) {
#endif
        if (socket != m_udp || m_state == State::Stopped || m_state == State::Error) {
            return;
        }
        reportRecoverableError(QStringLiteral("CoT UDP error: %1").arg(socket->errorString()));
    });

    QHostAddress address;
    if (address.setAddress(m_settings.host.trimmed())) {
        QPointer<CotOutputTransport> guard(this);
        completeUdpDestination(address, m_generation);
        return guard && m_state == State::Ready;
    }

    const QString status = QStringLiteral("Resolving CoT UDP destination %1…")
                               .arg(configuredEndpointText(m_settings.host, m_settings.port));
    if (!transition(State::Resolving, status)) {
        if (error) {
            *error = QStringLiteral("CoT UDP start was cancelled.");
        }
        return false;
    }
    m_hostLookupId = QHostInfo::lookupHost(m_settings.host.trimmed(), this,
                                           SLOT(hostLookupFinished(QHostInfo)));
    if (m_hostLookupId < 0) {
        if (error) {
            *error = QStringLiteral("Unable to start DNS lookup for %1.").arg(m_settings.host);
        }
        return false;
    }
    return true;
}

void CotOutputTransport::hostLookupFinished(const QHostInfo &hostInfo)
{
    m_hostLookupId = -1;
    if (m_state != State::Resolving || !m_udp
        || (m_settings.mode != Mode::TakMulticast && m_settings.mode != Mode::UdpClient)) {
        return;
    }
    if (hostInfo.error() != QHostInfo::NoError) {
        fail(QStringLiteral("Cannot resolve CoT UDP destination %1: %2")
                 .arg(m_settings.host, hostInfo.errorString()));
        return;
    }

    QHostAddress selected;
    for (const QHostAddress &candidate : hostInfo.addresses()) {
        if (m_settings.mode == Mode::TakMulticast && !candidate.isMulticast()) {
            continue;
        }
        if (selected.isNull() || candidate.protocol() == QAbstractSocket::IPv4Protocol) {
            selected = candidate;
        }
        if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
            break;
        }
    }
    if (selected.isNull()) {
        const QString reason = m_settings.mode == Mode::TakMulticast
            ? QStringLiteral("TAK destination %1 did not resolve to a multicast address.")
                  .arg(m_settings.host)
            : QStringLiteral("CoT UDP destination %1 has no usable IP address.")
                  .arg(m_settings.host);
        fail(reason);
        return;
    }
    completeUdpDestination(selected, m_generation);
}

void CotOutputTransport::completeUdpDestination(const QHostAddress &address, quint64 generation)
{
    if (generation != m_generation || !m_udp) {
        return;
    }
    if (m_settings.mode == Mode::TakMulticast && !address.isMulticast()) {
        fail(QStringLiteral("TAK Multicast requires a multicast destination address."));
        return;
    }
    if (m_settings.mode == Mode::TakMulticast) {
        m_udp->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    }
    m_hasUdpPeer = true;
    m_udpPeerAddress = address;
    m_udpPeerPort = m_settings.port;
    const QString status = m_settings.mode == Mode::TakMulticast
        ? QStringLiteral("Emitting Cursor-on-Target events to TAK multicast %1.")
              .arg(endpointText(address, m_settings.port))
        : QStringLiteral("Emitting Cursor-on-Target events to UDP destination %1.")
              .arg(endpointText(address, m_settings.port));
    if (!transition(State::Ready, status)) {
        return;
    }
    notifyPeers(status);
}

bool CotOutputTransport::startUdpHost(QString *error)
{
    QHostAddress listenAddress;
    if (!parseListenAddress(m_settings.host, &listenAddress)) {
        if (error) {
            *error = QStringLiteral("Invalid CoT UDP listen address %1.").arg(m_settings.host);
        }
        return false;
    }

    auto *socket = new QUdpSocket(this);
    if (!socket->bind(listenAddress, m_settings.port,
                      QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        if (error) {
            *error = QStringLiteral("Cannot bind CoT UDP host %1: %2")
                         .arg(configuredEndpointText(m_settings.host, m_settings.port),
                              socket->errorString());
        }
        delete socket;
        return false;
    }
    m_udp = socket;
    connect(socket, &QUdpSocket::readyRead, this, &CotOutputTransport::readUdpHostDatagrams);
    connect(socket, &QUdpSocket::bytesWritten, this, &CotOutputTransport::bytesWritten);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this,
            [this, socket](QAbstractSocket::SocketError) {
#endif
        if (socket == m_udp && m_state != State::Stopped && m_state != State::Error) {
            reportRecoverableError(QStringLiteral("CoT UDP host error: %1")
                                       .arg(socket->errorString()));
        }
    });
    return transition(State::Listening,
                      QStringLiteral("Listening for a UDP peer on %1.")
                          .arg(endpointText(socket->localAddress(), socket->localPort())));
}

void CotOutputTransport::readUdpHostDatagrams()
{
    QUdpSocket *socket = m_udp;
    if (!socket || m_settings.mode != Mode::UdpHost) {
        return;
    }
    QHostAddress newestAddress;
    quint16 newestPort = 0;
    bool received = false;
    while (socket == m_udp && socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket->receiveDatagram();
        if (!datagram.isValid()) {
            break;
        }
        if (!datagram.senderAddress().isNull() && datagram.senderPort() > 0) {
            newestAddress = datagram.senderAddress();
            newestPort = static_cast<quint16>(datagram.senderPort());
            received = true;
        }
    }
    if (!received || socket != m_udp) {
        return;
    }

    const bool changed = !m_hasUdpPeer || newestAddress != m_udpPeerAddress
        || newestPort != m_udpPeerPort;
    m_hasUdpPeer = true;
    m_udpPeerAddress = newestAddress;
    m_udpPeerPort = newestPort;
    if (!changed) {
        return;
    }
    const QString status = QStringLiteral("Emitting Cursor-on-Target events to UDP peer %1.")
                               .arg(endpointText(newestAddress, newestPort));
    if (!transition(State::Ready, status)) {
        return;
    }
    notifyPeers(status);
}

bool CotOutputTransport::startTcpClient(QString *error)
{
    Q_UNUSED(error)
    auto *socket = new QTcpSocket(this);
    m_tcpClient = socket;
    socket->setReadBufferSize(4096);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        if (socket == m_tcpClient) {
            socket->readAll(); // This transport is output-only; keep input bounded.
        }
    });
    connect(socket, &QTcpSocket::bytesWritten, this, &CotOutputTransport::bytesWritten);
    connect(socket, &QTcpSocket::connected, this, [this, socket]() {
        if (socket != m_tcpClient) {
            return;
        }
        const QString status = QStringLiteral("CoT TCP client connected: %1.")
                                   .arg(socketPeerText(socket));
        if (!transition(State::Ready, status)) {
            return;
        }
        notifyPeers(status);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        if (socket == m_tcpClient && m_state != State::Stopped && m_state != State::Error) {
            fail(QStringLiteral("CoT TCP client disconnected; automatic reconnect is disabled."));
        }
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this,
            [this, socket](QAbstractSocket::SocketError) {
#endif
        if (socket == m_tcpClient && m_state != State::Stopped && m_state != State::Error) {
            fail(QStringLiteral("CoT TCP client error: %1. Automatic reconnect is disabled.")
                     .arg(socket->errorString()));
        }
    });

    const QString status = QStringLiteral("Connecting CoT TCP client to %1…")
                               .arg(configuredEndpointText(m_settings.host, m_settings.port));
    if (!transition(State::Connecting, status) || socket != m_tcpClient) {
        if (error) {
            *error = QStringLiteral("CoT TCP client start was cancelled.");
        }
        return false;
    }
    socket->connectToHost(m_settings.host.trimmed(), m_settings.port);
    return true;
}

bool CotOutputTransport::startTcpHost(QString *error)
{
    QHostAddress listenAddress;
    if (!parseListenAddress(m_settings.host, &listenAddress)) {
        if (error) {
            *error = QStringLiteral("Invalid CoT TCP listen address %1.").arg(m_settings.host);
        }
        return false;
    }

    auto *server = new QTcpServer(this);
    server->setMaxPendingConnections(MaxTcpClients);
    if (!server->listen(listenAddress, m_settings.port)) {
        if (error) {
            *error = QStringLiteral("Cannot listen on CoT TCP host %1: %2")
                         .arg(configuredEndpointText(m_settings.host, m_settings.port),
                              server->errorString());
        }
        delete server;
        return false;
    }
    m_tcpServer = server;
    connect(server, &QTcpServer::newConnection, this, &CotOutputTransport::acceptTcpClients);
    connect(server, &QTcpServer::acceptError, this, [this, server](QAbstractSocket::SocketError) {
        if (server == m_tcpServer) {
            reportRecoverableError(QStringLiteral("CoT TCP host accept error: %1")
                                       .arg(server->errorString()));
        }
    });
    return transition(State::Listening, tcpHostStatus());
}

void CotOutputTransport::acceptTcpClients()
{
    QTcpServer *server = m_tcpServer;
    if (!server) {
        return;
    }
    int accepted = 0;
    int rejected = 0;
    while (server == m_tcpServer && server->hasPendingConnections()) {
        QTcpSocket *client = server->nextPendingConnection();
        if (!client) {
            break;
        }
        if (m_tcpClients.size() >= MaxTcpClients) {
            ++rejected;
            client->abort();
            client->deleteLater();
            continue;
        }
        client->setParent(this);
        client->setReadBufferSize(4096);
        m_tcpClients.append(client);
        ++accepted;
        connect(client, &QTcpSocket::readyRead, this, [this, client]() {
            if (m_tcpClients.contains(client)) {
                client->readAll(); // Output-only, but do not retain arbitrary peer input.
            }
        });
        connect(client, &QTcpSocket::bytesWritten, this, &CotOutputTransport::bytesWritten);
        connect(client, &QTcpSocket::disconnected, this, [this, client]() {
            if (!m_tcpClients.contains(client)) {
                return;
            }
            removeTcpHostClient(client);
            const State nextState = m_tcpClients.isEmpty() ? State::Listening : State::Ready;
            const QString status = tcpHostStatus();
            if (!transition(nextState, status)) {
                return;
            }
            notifyPeers(status);
        });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(client, &QAbstractSocket::errorOccurred, this,
                [this, client](QAbstractSocket::SocketError) {
#else
        connect(client, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this,
                [this, client](QAbstractSocket::SocketError) {
#endif
            if (m_tcpClients.contains(client)) {
                handleTcpHostClientFailure(client, client->errorString());
            }
        });
    }

    if (accepted > 0) {
        const QString status = tcpHostStatus();
        if (!transition(State::Ready, status)) {
            return;
        }
        if (!notifyPeers(status)) {
            return;
        }
    }
    if (rejected > 0 && server == m_tcpServer) {
        reportRecoverableError(QStringLiteral("CoT TCP host client limit (%1) reached; rejected %2 client(s).")
                                   .arg(MaxTcpClients).arg(rejected));
    }
}

void CotOutputTransport::removeTcpHostClient(QTcpSocket *client)
{
    if (!client || !m_tcpClients.removeOne(client)) {
        return;
    }
    disconnect(client, nullptr, this, nullptr);
    client->abort();
    client->deleteLater();
}

void CotOutputTransport::handleTcpHostClientFailure(QTcpSocket *client, const QString &reason)
{
    const QString peer = socketPeerText(client);
    removeTcpHostClient(client);
    const State nextState = m_tcpClients.isEmpty() ? State::Listening : State::Ready;
    const QString status = tcpHostStatus();
    if (!transition(nextState, status)) {
        return;
    }
    if (!notifyPeers(status)) {
        return;
    }
    reportRecoverableError(QStringLiteral("Removed broken CoT TCP client %1: %2")
                               .arg(peer.isEmpty() ? QStringLiteral("client") : peer, reason));
}

bool CotOutputTransport::startSerial(QString *error)
{
    auto *port = new QSerialPort(this);
    port->setPortName(m_settings.serialPort.trimmed());
    const bool configured = port->setBaudRate(m_settings.baud)
        && port->setDataBits(QSerialPort::Data8)
        && port->setParity(QSerialPort::NoParity)
        && port->setStopBits(QSerialPort::OneStop)
        && port->setFlowControl(QSerialPort::NoFlowControl);
    if (!configured || !port->open(QIODevice::ReadWrite)) {
        if (error) {
            *error = QStringLiteral("Cannot open CoT serial port %1 at %2 baud (8N1): %3")
                         .arg(m_settings.serialPort).arg(m_settings.baud).arg(port->errorString());
        }
        delete port;
        return false;
    }
    port->clear();
    m_serial = port;
    connect(port, &QSerialPort::readyRead, this, [this, port]() {
        if (port == m_serial) {
            port->readAll(); // Output-only, but keep unsolicited input bounded.
        }
    });
    connect(port, &QSerialPort::bytesWritten, this, &CotOutputTransport::bytesWritten);
    connect(port, &QSerialPort::errorOccurred, this,
            [this, port](QSerialPort::SerialPortError serialError) {
        if (port != m_serial || serialError == QSerialPort::NoError) {
            return;
        }
        const QString reason = QStringLiteral("CoT serial port %1: %2")
                                   .arg(m_settings.serialPort, port->errorString());
        if (serialError == QSerialPort::ResourceError
            || serialError == QSerialPort::DeviceNotFoundError
            || serialError == QSerialPort::PermissionError
            || serialError == QSerialPort::WriteError) {
            fail(reason);
        } else {
            reportRecoverableError(reason);
        }
    });
    const QString status = QStringLiteral("Emitting Cursor-on-Target events on %1 at %2 baud (8N1).")
                               .arg(m_settings.serialPort).arg(m_settings.baud);
    if (!transition(State::Ready, status)) {
        return false;
    }
    return notifyPeers(status);
}

CotOutputTransport::SendResult CotOutputTransport::send(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MaximumPayloadBytes || !payload.endsWith('\n')) {
        reportRecoverableError(QStringLiteral("CoT payload must be 1..%1 bytes and end with LF.")
                                   .arg(MaximumPayloadBytes));
        return SendResult::InvalidPayload;
    }
    if (!isStarted()) {
        return SendResult::NotReady;
    }

    switch (m_settings.mode) {
    case Mode::TakMulticast:
    case Mode::UdpClient:
    case Mode::UdpHost: {
        if (m_state != State::Ready || !m_udp) {
            return m_state == State::Listening ? SendResult::NoPeer : SendResult::NotReady;
        }
        if (!m_hasUdpPeer) {
            return SendResult::NoPeer;
        }
        if (m_udp->bytesToWrite() + payload.size() > PerPeerPendingLimitBytes) {
            reportRecoverableError(QStringLiteral("CoT UDP output queue is full."));
            return SendResult::Backpressure;
        }
        // Exactly one call, hence exactly one datagram, for every accepted CoT event.
        const qint64 written = m_udp->writeDatagram(payload, m_udpPeerAddress, m_udpPeerPort);
        if (written != payload.size()) {
            reportRecoverableError(QStringLiteral("Unable to send complete CoT UDP datagram: %1")
                                       .arg(m_udp->errorString()));
            return SendResult::IoError;
        }
        emit payloadSent(payload.size(), 1);
        return SendResult::Sent;
    }
    case Mode::TcpClient: {
        QTcpSocket *socket = m_tcpClient;
        if (m_state != State::Ready || !socket
            || socket->state() != QAbstractSocket::ConnectedState) {
            return SendResult::NotReady;
        }
        if (socket->bytesToWrite() + payload.size() > PerPeerPendingLimitBytes) {
            reportRecoverableError(QStringLiteral("CoT TCP client output queue is full."));
            return SendResult::Backpressure;
        }
        const qint64 written = socket->write(payload);
        if (written != payload.size()) {
            fail(QStringLiteral("CoT TCP client could not queue a complete event: %1")
                     .arg(socket->errorString()));
            return SendResult::IoError;
        }
        emit payloadSent(payload.size(), 1);
        return SendResult::Sent;
    }
    case Mode::TcpHost: {
        if (!m_tcpServer || !m_tcpServer->isListening()) {
            return SendResult::NotReady;
        }
        if (m_tcpClients.isEmpty()) {
            return SendResult::NoPeer;
        }
        const QList<QTcpSocket *> clients = m_tcpClients;
        int delivered = 0;
        int removed = 0;
        for (QTcpSocket *client : clients) {
            if (!m_tcpClients.contains(client)
                || client->state() != QAbstractSocket::ConnectedState
                || client->bytesToWrite() + payload.size() > PerPeerPendingLimitBytes) {
                removeTcpHostClient(client);
                ++removed;
                continue;
            }
            if (client->write(payload) != payload.size()) {
                removeTcpHostClient(client);
                ++removed;
                continue;
            }
            ++delivered;
        }
        if (removed > 0) {
            const State nextState = m_tcpClients.isEmpty() ? State::Listening : State::Ready;
            const QString status = tcpHostStatus();
            if (!transition(nextState, status)) {
                return delivered > 0 ? SendResult::Sent : SendResult::Backpressure;
            }
            if (!notifyPeers(status)) {
                return delivered > 0 ? SendResult::Sent : SendResult::Backpressure;
            }
            QPointer<CotOutputTransport> guard(this);
            reportRecoverableError(QStringLiteral("Removed %1 slow or broken CoT TCP client(s).")
                                       .arg(removed));
            if (!guard || !m_tcpServer || !m_tcpServer->isListening()) {
                return delivered > 0 ? SendResult::Sent : SendResult::Backpressure;
            }
        }
        if (delivered == 0) {
            return removed > 0 ? SendResult::Backpressure : SendResult::NoPeer;
        }
        emit payloadSent(payload.size(), delivered);
        return SendResult::Sent;
    }
    case Mode::Serial: {
        QSerialPort *port = m_serial;
        if (m_state != State::Ready || !port || !port->isOpen()) {
            return SendResult::NotReady;
        }
        if (port->bytesToWrite() + payload.size() > PerPeerPendingLimitBytes) {
            reportRecoverableError(QStringLiteral("CoT serial output queue is full."));
            return SendResult::Backpressure;
        }
        const qint64 written = port->write(payload);
        if (written != payload.size()) {
            fail(QStringLiteral("CoT serial port could not queue a complete event: %1")
                     .arg(port->errorString()));
            return SendResult::IoError;
        }
        emit payloadSent(payload.size(), 1);
        return SendResult::Sent;
    }
    }
    return SendResult::NotReady;
}

bool CotOutputTransport::transition(State state, const QString &status)
{
    const bool stateChangedValue = m_state != state;
    const bool statusChangedValue = m_statusText != status;
    m_state = state;
    m_statusText = status;

    QPointer<CotOutputTransport> guard(this);
    if (stateChangedValue) {
        emit stateChanged(state);
    }
    if (!guard || m_state != state || m_statusText != status) {
        return false;
    }
    if (statusChangedValue) {
        emit statusChanged(status);
    }
    return guard && m_state == state && m_statusText == status;
}

bool CotOutputTransport::notifyPeers(const QString &status)
{
    const bool statusChangedValue = m_statusText != status;
    m_statusText = status;
    QPointer<CotOutputTransport> guard(this);
    emit peersChanged();
    if (!guard || m_statusText != status) {
        return false;
    }
    if (statusChangedValue) {
        emit statusChanged(status);
    }
    return guard && m_statusText == status;
}

void CotOutputTransport::reportRecoverableError(const QString &error)
{
    m_lastError = error;
    emit errorOccurred(error); // No member access after this re-entrant signal.
}

void CotOutputTransport::fail(const QString &error)
{
    shutdown(false);
    m_lastError = error;
    m_state = State::Error;
    m_statusText = error;

    QPointer<CotOutputTransport> guard(this);
    emit stateChanged(State::Error);
    if (!guard || m_state != State::Error || m_statusText != error) {
        return;
    }
    emit statusChanged(error);
    if (!guard || m_state != State::Error || m_statusText != error) {
        return;
    }
    emit errorOccurred(error); // Last operation: error handlers may stop/delete us.
}

void CotOutputTransport::stop()
{
    shutdown(true);
}

void CotOutputTransport::shutdown(bool notify)
{
    const bool hadPeers = hasPeer();
    const bool stateWasStopped = m_state == State::Stopped;
    const bool statusWasStopped = m_statusText == QStringLiteral("Stopped.");
    ++m_generation;

    if (m_hostLookupId >= 0) {
        QHostInfo::abortHostLookup(m_hostLookupId);
        m_hostLookupId = -1;
    }

    QSerialPort *serial = m_serial;
    m_serial = nullptr;
    QUdpSocket *udp = m_udp;
    m_udp = nullptr;
    QTcpSocket *tcpClient = m_tcpClient;
    m_tcpClient = nullptr;
    QTcpServer *tcpServer = m_tcpServer;
    m_tcpServer = nullptr;
    const QList<QTcpSocket *> tcpClients = m_tcpClients;
    m_tcpClients.clear();
    m_hasUdpPeer = false;
    m_udpPeerAddress = QHostAddress();
    m_udpPeerPort = 0;

    if (serial) {
        disconnect(serial, nullptr, this, nullptr);
        serial->close();
        serial->deleteLater();
    }
    if (udp) {
        disconnect(udp, nullptr, this, nullptr);
        udp->close();
        udp->deleteLater();
    }
    if (tcpClient) {
        disconnect(tcpClient, nullptr, this, nullptr);
        tcpClient->abort();
        tcpClient->deleteLater();
    }
    for (QTcpSocket *client : tcpClients) {
        if (!client) {
            continue;
        }
        disconnect(client, nullptr, this, nullptr);
        client->abort();
        client->deleteLater();
    }
    if (tcpServer) {
        disconnect(tcpServer, nullptr, this, nullptr);
        tcpServer->close();
        tcpServer->deleteLater();
    }

    m_state = State::Stopped;
    m_statusText = QStringLiteral("Stopped.");
    if (!notify || (stateWasStopped && statusWasStopped && !hadPeers)) {
        return;
    }

    QPointer<CotOutputTransport> guard(this);
    if (hadPeers) {
        emit peersChanged();
    }
    if (!guard || m_state != State::Stopped) {
        return;
    }
    if (!stateWasStopped) {
        emit stateChanged(State::Stopped);
    }
    if (!guard || m_state != State::Stopped) {
        return;
    }
    if (!statusWasStopped) {
        emit statusChanged(m_statusText);
    }
}

QString CotOutputTransport::tcpHostStatus() const
{
    const quint16 port = localPort();
    if (m_tcpClients.isEmpty()) {
        return QStringLiteral("Listening for CoT TCP clients on port %1.").arg(port);
    }
    if (m_tcpClients.size() == 1) {
        return QStringLiteral("CoT TCP host on port %1 has 1 client.").arg(port);
    }
    return QStringLiteral("CoT TCP host on port %1 has %2 clients.")
        .arg(port).arg(m_tcpClients.size());
}
