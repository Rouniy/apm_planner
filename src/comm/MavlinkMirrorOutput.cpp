#include "MavlinkMirrorOutput.h"

#include <QNetworkDatagram>
#include <QSerialPort>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

namespace {

qint64 boundedWrite(QIODevice *device, const QByteArray &bytes)
{
    if (!device || !device->isOpen()) {
        return -1;
    }
    const qint64 room = MavlinkMirrorOutput::OutputPendingLimitBytes - device->bytesToWrite();
    if (room <= 0 || bytes.isEmpty()) {
        return 0;
    }
    const qint64 count = qMin<qint64>(room, bytes.size());
    const qint64 written = device->write(bytes.constData(), count);
    return written < 0 ? -1 : written;
}

QString endpointText(const QHostAddress &address, quint16 port)
{
    return QStringLiteral("%1:%2").arg(address.toString()).arg(port);
}

} // namespace

// --- base ---------------------------------------------------------------------------------

MavlinkMirrorOutput::MavlinkMirrorOutput(QObject *parent)
    : QObject(parent)
{
}

MavlinkMirrorOutput::~MavlinkMirrorOutput() = default;

QString MavlinkMirrorOutput::TcpHostSelection()
{
    return QStringLiteral("TCP Host - 14550");
}

QString MavlinkMirrorOutput::UdpHostSelection()
{
    return QStringLiteral("UDP Host - 14550");
}

bool MavlinkMirrorOutput::IsTcpHostSelection(const QString &selection)
{
    return selection == TcpHostSelection();
}

bool MavlinkMirrorOutput::IsUdpHostSelection(const QString &selection)
{
    return selection == UdpHostSelection();
}

QStringList MavlinkMirrorOutput::Selections(const QStringList &serialPorts)
{
    QStringList selections;
    for (const QString &port : serialPorts) {
        if (!port.isEmpty() && !selections.contains(port)) {
            selections.append(port);   // MP10: SerialPort.GetPortNames().Distinct()
        }
    }
    selections.append(TcpHostSelection());
    selections.append(UdpHostSelection());
    return selections;
}

std::unique_ptr<MavlinkMirrorOutput> MavlinkMirrorOutput::Create(const MavlinkMirrorSettings &settings,
                                                                 QString *error)
{
    const QString selection = settings.portSelection.trimmed();
    if (selection.isEmpty()) {
        if (error) {
            *error = QStringLiteral("No port selected.");
        }
        return nullptr;
    }
    if (IsTcpHostSelection(selection)) {
        return std::make_unique<TcpHostMirrorOutput>(DefaultHostPort);
    }
    if (IsUdpHostSelection(selection)) {
        return std::make_unique<UdpHostMirrorOutput>(DefaultHostPort);
    }
    if (settings.baud <= 0) {
        if (error) {
            *error = QStringLiteral("Invalid baud rate %1.").arg(settings.baud);
        }
        return nullptr;
    }
    return std::make_unique<SerialMirrorOutput>(selection, settings.baud);
}

// --- serial -------------------------------------------------------------------------------

SerialMirrorOutput::SerialMirrorOutput(const QString &portName, int baud, QObject *parent)
    : MavlinkMirrorOutput(parent)
    , m_portName(portName)
    , m_baud(baud)
{
}

SerialMirrorOutput::~SerialMirrorOutput()
{
    close();
}

bool SerialMirrorOutput::open(QString *error)
{
    close();
    auto *port = new QSerialPort(this);
    port->setPortName(m_portName);
    if (!port->open(QIODevice::ReadWrite)) {
        if (error) {
            *error = QStringLiteral("Cannot open serial port %1: %2")
                         .arg(m_portName, port->errorString());
        }
        delete port;
        return false;
    }
    // MP10 SerialPort defaults: 8N1, no flow control.
    if (!port->setBaudRate(m_baud) || !port->setDataBits(QSerialPort::Data8)
        || !port->setParity(QSerialPort::NoParity) || !port->setStopBits(QSerialPort::OneStop)
        || !port->setFlowControl(QSerialPort::NoFlowControl)) {
        if (error) {
            *error = QStringLiteral("Cannot configure serial port %1 at %2 baud: %3")
                         .arg(m_portName).arg(m_baud).arg(port->errorString());
        }
        port->close();
        delete port;
        return false;
    }
    port->clear();
    m_port = port;
    connect(port, &QSerialPort::readyRead, this, [this]() {
        if (m_port) {
            emit peerBytesReceived(m_port->readAll());
        }
    });
    connect(port, &QSerialPort::bytesWritten, this, &MavlinkMirrorOutput::bytesWritten);
    connect(port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError code) {
        if (code == QSerialPort::NoError || !m_port) {
            return;
        }
        if (code == QSerialPort::ResourceError || code == QSerialPort::DeviceNotFoundError
            || code == QSerialPort::PermissionError || code == QSerialPort::WriteError) {
            emit errorOccurred(QStringLiteral("Serial port %1: %2")
                                   .arg(m_portName, m_port->errorString()));
        }
    });
    return true;
}

void SerialMirrorOutput::close()
{
    if (!m_port) {
        return;
    }
    QSerialPort *port = m_port;
    m_port = nullptr;
    disconnect(port, nullptr, this, nullptr);
    if (port->isOpen()) {
        port->close();
    }
    port->deleteLater();
}

bool SerialMirrorOutput::isOpen() const
{
    return m_port && m_port->isOpen();
}

qint64 SerialMirrorOutput::write(const QByteArray &bytes)
{
    return boundedWrite(m_port, bytes);
}

qint64 SerialMirrorOutput::pendingBytes() const
{
    return m_port ? m_port->bytesToWrite() : 0;
}

QString SerialMirrorOutput::statusText() const
{
    if (!isOpen()) {
        return QStringLiteral("Stopped.");
    }
    return QStringLiteral("Mirroring on %1.").arg(m_portName);   // MP10: $"Mirroring on {SelectedPort}."
}

// --- TCP host -----------------------------------------------------------------------------

TcpHostMirrorOutput::TcpHostMirrorOutput(quint16 port, const QHostAddress &address, QObject *parent)
    : MavlinkMirrorOutput(parent)
    , m_port(port)
    , m_address(address)
{
}

TcpHostMirrorOutput::~TcpHostMirrorOutput()
{
    close();
}

bool TcpHostMirrorOutput::open(QString *error)
{
    close();
    auto *server = new QTcpServer(this);
    server->setMaxPendingConnections(1);
    if (!server->listen(m_address, m_port)) {
        if (error) {
            *error = QStringLiteral("Cannot listen on TCP %1: %2")
                         .arg(m_port).arg(server->errorString());
        }
        delete server;
        return false;
    }
    m_server = server;
    connect(server, &QTcpServer::newConnection, this, &TcpHostMirrorOutput::acceptPending);
    return true;
}

void TcpHostMirrorOutput::close()
{
    const bool hadPeer = m_client != nullptr;
    if (m_client) {
        dropClient(m_client);
    }
    if (m_server) {
        QTcpServer *server = m_server;
        m_server = nullptr;
        disconnect(server, nullptr, this, nullptr);
        server->close();
        server->deleteLater();
    }
    if (hadPeer) {
        emit peerChanged();
    }
}

bool TcpHostMirrorOutput::isOpen() const
{
    return m_server && m_server->isListening();
}

quint16 TcpHostMirrorOutput::serverPort() const
{
    return m_server ? m_server->serverPort() : 0;
}

QString TcpHostMirrorOutput::peerDescription() const
{
    if (!m_client) {
        return QString();
    }
    return endpointText(m_client->peerAddress(), m_client->peerPort());
}

void TcpHostMirrorOutput::acceptPending()
{
    if (!m_server) {
        return;
    }
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        // MP10 TcpSerialHostListener: a new client replaces and closes the
        // previous socket.
        if (m_client) {
            dropClient(m_client);
        }
        m_client = socket;
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            if (socket == m_client) {
                emit peerBytesReceived(socket->readAll());
            }
        });
        connect(socket, &QTcpSocket::bytesWritten, this, &MavlinkMirrorOutput::bytesWritten);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            if (socket == m_client) {
                dropClient(socket);
                emit peerChanged();
            }
        });
        emit peerChanged();
    }
}

void TcpHostMirrorOutput::dropClient(QTcpSocket *client)
{
    if (!client) {
        return;
    }
    if (client == m_client) {
        m_client = nullptr;
    }
    disconnect(client, nullptr, this, nullptr);
    if (client->state() != QAbstractSocket::UnconnectedState) {
        client->disconnectFromHost();
    }
    client->deleteLater();
}

qint64 TcpHostMirrorOutput::write(const QByteArray &bytes)
{
    if (!m_client) {
        return 0;
    }
    return boundedWrite(m_client, bytes);
}

qint64 TcpHostMirrorOutput::pendingBytes() const
{
    return m_client ? m_client->bytesToWrite() : 0;
}

QString TcpHostMirrorOutput::statusText() const
{
    if (!isOpen()) {
        return QStringLiteral("Stopped.");
    }
    const quint16 port = serverPort();
    if (!m_client) {
        return QStringLiteral("Listening on TCP %1 — waiting for a client…").arg(port);
    }
    return QStringLiteral("Mirroring on TCP %1 (client %2 connected).").arg(port).arg(peerDescription());
}

// --- UDP host -----------------------------------------------------------------------------

UdpHostMirrorOutput::UdpHostMirrorOutput(quint16 port, const QHostAddress &address, QObject *parent)
    : MavlinkMirrorOutput(parent)
    , m_port(port)
    , m_address(address)
{
}

UdpHostMirrorOutput::~UdpHostMirrorOutput()
{
    close();
}

bool UdpHostMirrorOutput::open(QString *error)
{
    close();
    auto *socket = new QUdpSocket(this);
    // MP10 CreateSharedListener: ReuseAddress on Any. Callers guard the port
    // against the primary UDP link before open() is reached.
    if (!socket->bind(m_address, m_port,
                      QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        if (error) {
            *error = QStringLiteral("Cannot bind UDP %1: %2").arg(m_port).arg(socket->errorString());
        }
        delete socket;
        return false;
    }
    m_socket = socket;
    connect(socket, &QUdpSocket::readyRead, this, &UdpHostMirrorOutput::readDatagrams);
    connect(socket, &QUdpSocket::bytesWritten, this, &MavlinkMirrorOutput::bytesWritten);
    return true;
}

void UdpHostMirrorOutput::close()
{
    const bool hadPeer = hasPeer();
    m_peers.clear();
    if (m_socket) {
        QUdpSocket *socket = m_socket;
        m_socket = nullptr;
        disconnect(socket, nullptr, this, nullptr);
        socket->close();
        socket->deleteLater();
    }
    if (hadPeer) {
        emit peerChanged();
    }
}

bool UdpHostMirrorOutput::isOpen() const
{
    return m_socket && m_socket->state() == QAbstractSocket::BoundState;
}

quint16 UdpHostMirrorOutput::localPort() const
{
    return m_socket ? m_socket->localPort() : 0;
}

void UdpHostMirrorOutput::readDatagrams()
{
    if (!m_socket) {
        return;
    }
    while (m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram();
        if (!datagram.isValid()) {
            break;
        }
        // MP10 CommsUdpSerial.Read: every distinct sender joins EndPointList.
        Peer peer;
        peer.address = datagram.senderAddress();
        peer.port = static_cast<quint16>(datagram.senderPort());
        if (!m_peers.contains(peer)) {
            m_peers.append(peer);
            while (m_peers.size() > MaxPeers) {
                m_peers.removeFirst();   // bounded: evict the oldest learned peer
            }
            emit peerChanged();
        }
        if (!datagram.data().isEmpty()) {
            emit peerBytesReceived(datagram.data());
        }
    }
}

qint64 UdpHostMirrorOutput::write(const QByteArray &bytes)
{
    if (!m_socket || !hasPeer() || bytes.isEmpty()) {
        return 0;
    }
    // One frame per datagram per peer; never split a frame across datagrams.
    const qint64 burst = static_cast<qint64>(bytes.size()) * m_peers.size();
    if (m_socket->bytesToWrite() + burst > OutputPendingLimitBytes) {
        return 0;
    }
    bool anyDelivered = false;
    bool anyFailed = false;
    for (const Peer &peer : m_peers) {
        const qint64 written = m_socket->writeDatagram(bytes, peer.address, peer.port);
        if (written == bytes.size()) {
            anyDelivered = true;
        } else {
            anyFailed = true;
        }
    }
    if (!anyDelivered && !isOpen()) {
        return -1;
    }
    // MP10 swallows a send failure for an individual learned endpoint. UDP is
    // best effort: keep the host alive while its socket remains bound even if
    // every peer transiently rejects this datagram.
    Q_UNUSED(anyFailed)
    return bytes.size();   // the frame left the mirror once; peers are a fan-out
}

qint64 UdpHostMirrorOutput::pendingBytes() const
{
    return m_socket ? m_socket->bytesToWrite() : 0;
}

QString UdpHostMirrorOutput::statusText() const
{
    if (!isOpen()) {
        return QStringLiteral("Stopped.");
    }
    const quint16 port = localPort();
    if (m_peers.isEmpty()) {
        return QStringLiteral("Listening on UDP %1 — waiting for a client…").arg(port);
    }
    if (m_peers.size() == 1) {
        return QStringLiteral("Mirroring on UDP %1 (client %2 connected).")
            .arg(port).arg(endpointText(m_peers.first().address, m_peers.first().port));
    }
    return QStringLiteral("Mirroring on UDP %1 (%2 clients connected).")
        .arg(port).arg(m_peers.size());
}
