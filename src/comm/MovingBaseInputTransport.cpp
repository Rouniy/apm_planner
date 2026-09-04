#include "MovingBaseInputTransport.h"

#include <QAbstractSocket>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QPointer>
#include <QSerialPort>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

QString MovingBaseInputTransport::modeLabel(Mode mode)
{
    switch (mode) {
    case Mode::Serial:
        return QStringLiteral("Serial");
    case Mode::TcpHost:
        return QStringLiteral("TCP Host");
    case Mode::TcpClient:
        return QStringLiteral("TCP Client");
    case Mode::UdpHost:
        return QStringLiteral("UDP Host");
    case Mode::UdpClient:
        return QStringLiteral("UDP Client");
    }
    return QString();
}

MovingBaseInputTransport::Settings
MovingBaseInputTransport::defaults(Mode mode)
{
    Settings result;
    result.mode = mode;
    result.baud = DefaultSerialBaud;
    result.port = DefaultNetworkPort;
    switch (mode) {
    case Mode::Serial:
        result.host.clear();
        result.port = 0;
        break;
    case Mode::TcpHost:
    case Mode::UdpHost:
        result.host = QStringLiteral("0.0.0.0");
        break;
    case Mode::TcpClient:
    case Mode::UdpClient:
        result.host = QStringLiteral("127.0.0.1");
        break;
    }
    return result;
}

QList<int> MovingBaseInputTransport::supportedBaudRates()
{
    return {4800, 9600, 14400, 19200,
            28800, 38400, 57600, 115200};
}

MovingBaseInputTransport::MovingBaseInputTransport(
    const Settings &settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    qRegisterMetaType<MovingBaseInputTransport::Mode>();
    qRegisterMetaType<MovingBaseInputTransport::State>();
    qRegisterMetaType<NmeaGgaFix>();
    qRegisterMetaType<NmeaGgaParseError>();
}

MovingBaseInputTransport::~MovingBaseInputTransport()
{
    shutdown(false);
}

bool MovingBaseInputTransport::isStarted() const noexcept
{
    return m_state == State::Resolving
        || m_state == State::Connecting
        || m_state == State::Listening
        || m_state == State::Ready;
}

quint16 MovingBaseInputTransport::localPort() const
{
    if (m_tcpServer) {
        return m_tcpServer->serverPort();
    }
    return m_udp ? m_udp->localPort() : 0;
}

bool MovingBaseInputTransport::start(QString *error)
{
    shutdown(false);
    const quint64 generation = m_generation;
    m_lastError.clear();
    m_acceptedFixCount = 0;
    m_rejectedLineCount = 0;
    m_ignoredDatagramCount = 0;
    if (error) {
        error->clear();
    }

    QString localError;
    if (!validateSettings(&localError)) {
        if (error) {
            *error = localError;
        }
        fail(localError, generation);
        return false;
    }

    QPointer<MovingBaseInputTransport> guard(this);
    bool accepted = false;
    switch (m_settings.mode) {
    case Mode::Serial:
        accepted = startSerial(&localError, generation);
        break;
    case Mode::TcpHost:
        accepted = startTcpHost(&localError, generation);
        break;
    case Mode::TcpClient:
        accepted = startTcpClient(&localError, generation);
        break;
    case Mode::UdpHost:
        accepted = startUdpHost(&localError, generation);
        break;
    case Mode::UdpClient:
        accepted = startUdpClient(&localError, generation);
        break;
    }
    if (!guard || generation != m_generation) {
        return false;
    }
    if (!accepted && m_state != State::Error) {
        const QString reason = localError.isEmpty()
            ? tr("Unable to start Moving Base input.") : localError;
        if (error) {
            *error = reason;
        }
        fail(reason, generation);
    }
    return accepted;
}

void MovingBaseInputTransport::stop()
{
    shutdown(true);
}

bool MovingBaseInputTransport::validateSettings(QString *error) const
{
    switch (m_settings.mode) {
    case Mode::Serial:
        if (m_settings.serialPort.trimmed().isEmpty()) {
            if (error) {
                *error = tr("Select a Moving Base serial port first.");
            }
            return false;
        }
        if (!supportedBaudRates().contains(m_settings.baud)) {
            if (error) {
                *error = tr("Unsupported Moving Base baud rate %1.")
                    .arg(m_settings.baud);
            }
            return false;
        }
        return true;
    case Mode::TcpClient:
    case Mode::UdpClient:
        if (m_settings.host.trimmed().isEmpty()) {
            if (error) {
                *error = tr("Enter a remote Moving Base host.");
            }
            return false;
        }
        if (m_settings.port == 0) {
            if (error) {
                *error = tr("Remote Moving Base port must be between 1 and 65535.");
            }
            return false;
        }
        return true;
    case Mode::TcpHost:
    case Mode::UdpHost: {
        QHostAddress address;
        if (!parseListenAddress(m_settings.host, &address)) {
            if (error) {
                *error = tr("Moving Base listen address is not a numeric IP address: %1")
                    .arg(m_settings.host);
            }
            return false;
        }
        // Port zero deliberately requests an ephemeral port in tests.
        return true;
    }
    }
    if (error) {
        *error = tr("Unknown Moving Base input mode.");
    }
    return false;
}

bool MovingBaseInputTransport::startSerial(
    QString *error, quint64 generation)
{
    auto *serial = new QSerialPort(this);
    serial->setReadBufferSize(ReadBufferLimitBytes);
    serial->setPortName(m_settings.serialPort.trimmed());
    const bool configured = serial->setBaudRate(m_settings.baud)
        && serial->setDataBits(QSerialPort::Data8)
        && serial->setParity(QSerialPort::NoParity)
        && serial->setStopBits(QSerialPort::OneStop)
        && serial->setFlowControl(QSerialPort::NoFlowControl);
    if (!configured) {
        if (error) {
            *error = tr("Cannot configure Moving Base serial port %1 at %2 baud: %3")
                .arg(m_settings.serialPort.trimmed())
                .arg(m_settings.baud)
                .arg(serial->errorString());
        }
        delete serial;
        return false;
    }
    if (!serial->open(QIODevice::ReadOnly)) {
        if (error) {
            *error = tr("Cannot open Moving Base serial port %1: %2")
                .arg(m_settings.serialPort.trimmed(), serial->errorString());
        }
        delete serial;
        return false;
    }
    m_serial = serial;
    connect(serial, &QSerialPort::readyRead, this,
            [this, serial, generation]() {
        if (generation == m_generation && serial == m_serial) {
            ingestBytes(serial->readAll(), generation);
        }
    });
    connect(serial, &QSerialPort::errorOccurred, this,
            [this, serial, generation](QSerialPort::SerialPortError code) {
        if (generation != m_generation || serial != m_serial
            || code == QSerialPort::NoError) {
            return;
        }
        if (code == QSerialPort::ResourceError
            || code == QSerialPort::DeviceNotFoundError
            || code == QSerialPort::PermissionError
            || code == QSerialPort::ReadError) {
            fail(tr("Moving Base serial port %1 stopped: %2")
                     .arg(m_settings.serialPort.trimmed(),
                          serial->errorString()),
                 generation);
        }
    });

    if (!setSourceDescription(m_settings.serialPort.trimmed(), generation)) {
        return false;
    }
    return transition(
        State::Ready,
        tr("Reading Moving Base NMEA from serial port %1.")
            .arg(m_settings.serialPort.trimmed()),
        generation);
}

bool MovingBaseInputTransport::startTcpHost(
    QString *error, quint64 generation)
{
    QHostAddress address;
    if (!parseListenAddress(m_settings.host, &address)) {
        return false;
    }
    auto *server = new QTcpServer(this);
    // The run still retains only one active socket, but a small bounded Qt
    // backlog lets a burst be drained in order so the newest accepted client
    // replaces the older ones instead of the second SYN being rejected.
    server->setMaxPendingConnections(MaximumPendingTcpClients);
    if (!server->listen(address, m_settings.port)) {
        if (error) {
            *error = tr("Cannot listen for Moving Base TCP on %1: %2")
                .arg(endpointText(address, m_settings.port),
                     server->errorString());
        }
        delete server;
        return false;
    }
    m_tcpServer = server;
    connect(server, &QTcpServer::newConnection,
            this, &MovingBaseInputTransport::acceptTcpClients);
    connect(server, &QTcpServer::acceptError, this,
            [this, server, generation](QAbstractSocket::SocketError) {
        if (generation == m_generation && server == m_tcpServer) {
            fail(tr("Moving Base TCP accept failed: %1")
                     .arg(server->errorString()), generation);
        }
    });
    return transition(
        State::Listening,
        tr("Listening for Moving Base NMEA on TCP %1.")
            .arg(endpointText(server->serverAddress(), server->serverPort())),
        generation);
}

bool MovingBaseInputTransport::startTcpClient(
    QString *error, quint64 generation)
{
    Q_UNUSED(error)
    auto *socket = new QTcpSocket(this);
    socket->setReadBufferSize(ReadBufferLimitBytes);
    m_tcpClient = socket;
    connect(socket, &QTcpSocket::readyRead, this,
            [this, socket, generation]() {
        if (generation == m_generation && socket == m_tcpClient) {
            ingestBytes(socket->readAll(), generation);
        }
    });
    connect(socket, &QTcpSocket::connected, this,
            [this, socket, generation]() {
        if (generation != m_generation || socket != m_tcpClient) {
            return;
        }
        const QString source = endpointText(socket->peerAddress(),
                                            socket->peerPort());
        if (!setSourceDescription(source, generation)) {
            return;
        }
        transition(State::Ready,
                   tr("Reading Moving Base NMEA from TCP %1.").arg(source),
                   generation);
    });
    connect(socket, &QTcpSocket::disconnected, this,
            [this, socket, generation]() {
        if (generation == m_generation && socket == m_tcpClient
            && m_state != State::Stopped && m_state != State::Error) {
            fail(tr("Moving Base TCP client disconnected; automatic reconnect is disabled."),
                 generation);
        }
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this, socket, generation](QAbstractSocket::SocketError) {
#else
    connect(socket,
            QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, [this, socket, generation](QAbstractSocket::SocketError) {
#endif
        if (generation == m_generation && socket == m_tcpClient
            && m_state != State::Stopped && m_state != State::Error) {
            fail(tr("Moving Base TCP client error: %1. Automatic reconnect is disabled.")
                     .arg(socket->errorString()), generation);
        }
    });

    if (!transition(
            State::Connecting,
            tr("Connecting Moving Base TCP client to %1:%2…")
                .arg(m_settings.host.trimmed()).arg(m_settings.port),
            generation)) {
        return false;
    }
    socket->connectToHost(m_settings.host.trimmed(), m_settings.port);
    return generation == m_generation && socket == m_tcpClient;
}

bool MovingBaseInputTransport::startUdpHost(
    QString *error, quint64 generation)
{
    QHostAddress address;
    if (!parseListenAddress(m_settings.host, &address)) {
        return false;
    }
    auto *socket = new QUdpSocket(this);
    socket->setReadBufferSize(ReadBufferLimitBytes);
    if (!socket->bind(address, m_settings.port,
                      QAbstractSocket::DontShareAddress)) {
        if (error) {
            *error = tr("Cannot bind Moving Base UDP host %1: %2")
                .arg(endpointText(address, m_settings.port),
                     socket->errorString());
        }
        delete socket;
        return false;
    }
    m_udp = socket;
    connect(socket, &QUdpSocket::readyRead,
            this, &MovingBaseInputTransport::readUdpDatagrams);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this, socket, generation](QAbstractSocket::SocketError) {
#else
    connect(socket,
            QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, [this, socket, generation](QAbstractSocket::SocketError) {
#endif
        if (generation == m_generation && socket == m_udp
            && m_state != State::Stopped && m_state != State::Error) {
            fail(tr("Moving Base UDP host failed: %1")
                     .arg(socket->errorString()), generation);
        }
    });
    return transition(
        State::Listening,
        tr("Listening for Moving Base NMEA on UDP %1; the first sender will be pinned.")
            .arg(endpointText(socket->localAddress(), socket->localPort())),
        generation);
}

bool MovingBaseInputTransport::startUdpClient(
    QString *error, quint64 generation)
{
    auto *socket = new QUdpSocket(this);
    socket->setReadBufferSize(ReadBufferLimitBytes);
    m_udp = socket;
    connect(socket, &QUdpSocket::readyRead,
            this, &MovingBaseInputTransport::readUdpDatagrams);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this, socket, generation](QAbstractSocket::SocketError) {
#else
    connect(socket,
            QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, [this, socket, generation](QAbstractSocket::SocketError) {
#endif
        if (generation == m_generation && socket == m_udp
            && m_state != State::Stopped && m_state != State::Error) {
            fail(tr("Moving Base UDP client failed: %1")
                     .arg(socket->errorString()), generation);
        }
    });

    QHostAddress address;
    if (address.setAddress(m_settings.host.trimmed())) {
        return completeUdpClient(address, generation, error);
    }

    if (!transition(
            State::Resolving,
            tr("Resolving Moving Base UDP source %1:%2…")
                .arg(m_settings.host.trimmed()).arg(m_settings.port),
            generation)) {
        return false;
    }
    const int lookupId = QHostInfo::lookupHost(
        m_settings.host.trimmed(), this,
        [this, generation](const QHostInfo &hostInfo) {
            hostLookupFinished(hostInfo, generation);
        });
    if (generation != m_generation || socket != m_udp
        || m_state != State::Resolving) {
        if (lookupId >= 0) {
            QHostInfo::abortHostLookup(lookupId);
        }
        return false;
    }
    m_hostLookupId = lookupId;
    if (lookupId < 0) {
        if (error) {
            *error = tr("Unable to start DNS lookup for %1.")
                .arg(m_settings.host.trimmed());
        }
        return false;
    }
    return true;
}

bool MovingBaseInputTransport::completeUdpClient(
    const QHostAddress &address, quint64 generation, QString *error)
{
    if (generation != m_generation || !m_udp || address.isNull()) {
        return false;
    }
    const QHostAddress bindAddress =
        address.protocol() == QAbstractSocket::IPv6Protocol
        ? QHostAddress(QHostAddress::AnyIPv6)
        : QHostAddress(QHostAddress::AnyIPv4);
    if (!m_udp->bind(bindAddress, 0)) {
        const QString reason = tr("Cannot bind Moving Base UDP client: %1")
            .arg(m_udp->errorString());
        if (error) {
            *error = reason;
        }
        return false;
    }
    // An input-only UDP client otherwise leaves its ephemeral receive port
    // unknowable to the configured server. A zero-length discovery datagram
    // announces that return endpoint without injecting bytes into NMEA data.
    if (m_udp->writeDatagram(QByteArray(), address, m_settings.port) < 0) {
        const QString reason = tr(
            "Cannot announce the Moving Base UDP client to %1: %2")
            .arg(endpointText(address, m_settings.port),
                 m_udp->errorString());
        if (error) {
            *error = reason;
        }
        return false;
    }
    m_udpSourcePinned = true;
    m_udpSourceAddress = address;
    m_udpSourcePort = m_settings.port;
    const QString source = endpointText(address, m_settings.port);
    if (!setSourceDescription(source, generation)) {
        return false;
    }
    return transition(
        State::Ready,
        tr("Reading Moving Base NMEA from UDP source %1; the return endpoint was announced.")
            .arg(source),
        generation);
}

void MovingBaseInputTransport::hostLookupFinished(
    const QHostInfo &hostInfo, quint64 generation)
{
    if (generation != m_generation
        || hostInfo.lookupId() != m_hostLookupId) {
        return;
    }
    m_hostLookupId = -1;
    if (m_settings.mode != Mode::UdpClient
        || m_state != State::Resolving || !m_udp) {
        return;
    }
    if (hostInfo.error() != QHostInfo::NoError) {
        fail(tr("Cannot resolve Moving Base UDP source %1: %2")
                 .arg(m_settings.host.trimmed(), hostInfo.errorString()),
             generation);
        return;
    }

    QHostAddress selected;
    for (const QHostAddress &candidate : hostInfo.addresses()) {
        if (selected.isNull()
            || candidate.protocol() == QAbstractSocket::IPv4Protocol) {
            selected = candidate;
        }
        if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
            break;
        }
    }
    if (selected.isNull()) {
        fail(tr("Moving Base UDP source %1 has no usable IP address.")
                 .arg(m_settings.host.trimmed()), generation);
        return;
    }
    QString error;
    QPointer<MovingBaseInputTransport> guard(this);
    const bool completed = completeUdpClient(
        selected, generation, &error);
    if (!guard || generation != m_generation) {
        return;
    }
    if (!completed && m_state != State::Error
        && m_state != State::Stopped) {
        fail(error.isEmpty()
                 ? tr("Unable to start Moving Base UDP client.") : error,
             generation);
    }
}

void MovingBaseInputTransport::acceptTcpClients()
{
    const quint64 generation = m_generation;
    QTcpServer *server = m_tcpServer;
    if (!server || m_settings.mode != Mode::TcpHost) {
        return;
    }
    while (generation == m_generation && server == m_tcpServer
           && server->hasPendingConnections()) {
        QTcpSocket *client = server->nextPendingConnection();
        if (!client) {
            break;
        }
        if (m_tcpClient) {
            dropTcpClient(m_tcpClient);
        }
        m_lineFramer.reset();
        m_tcpClient = client;
        client->setParent(this);
        client->setReadBufferSize(ReadBufferLimitBytes);
        connect(client, &QTcpSocket::readyRead, this,
                [this, client, generation]() {
            if (generation == m_generation && client == m_tcpClient) {
                ingestBytes(client->readAll(), generation);
            }
        });
        connect(client, &QTcpSocket::disconnected, this,
                [this, client, generation]() {
            if (generation != m_generation || client != m_tcpClient) {
                return;
            }
            dropTcpClient(client);
            m_lineFramer.reset();
            if (!setSourceDescription(QString(), generation)) {
                return;
            }
            transition(
                State::Listening,
                tr("Listening for a new Moving Base NMEA TCP client on port %1.")
                    .arg(localPort()),
                generation);
        });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(client, &QAbstractSocket::errorOccurred, this,
                [this, client, generation](QAbstractSocket::SocketError) {
#else
        connect(client,
                QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
                this, [this, client, generation](QAbstractSocket::SocketError) {
#endif
            if (generation != m_generation || client != m_tcpClient) {
                return;
            }
            const QString reason = tr("Moving Base TCP client %1 failed: %2")
                .arg(endpointText(client->peerAddress(), client->peerPort()),
                     client->errorString());
            dropTcpClient(client);
            m_lineFramer.reset();
            if (!setSourceDescription(QString(), generation)) {
                return;
            }
            QPointer<MovingBaseInputTransport> guard(this);
            emit errorOccurred(reason);
            if (!guard || generation != m_generation) {
                return;
            }
            transition(
                State::Listening,
                tr("Listening for a new Moving Base NMEA TCP client on port %1 after: %2")
                    .arg(localPort()).arg(reason),
                generation);
        });

        const QString source = endpointText(client->peerAddress(),
                                            client->peerPort());
        if (!setSourceDescription(source, generation)) {
            return;
        }
        if (!transition(
                State::Ready,
                tr("Reading Moving Base NMEA from TCP client %1.")
                    .arg(source),
                generation)) {
            return;
        }
        QPointer<MovingBaseInputTransport> guard(this);
        emit noPositionFix(tr(
            "A Moving Base TCP source connected; waiting for its first "
            "valid GGA fix."));
        if (!guard || generation != m_generation
            || client != m_tcpClient) {
            return;
        }
    }
}

void MovingBaseInputTransport::dropTcpClient(QTcpSocket *client)
{
    if (!client) {
        return;
    }
    if (client == m_tcpClient) {
        m_tcpClient = nullptr;
    }
    disconnect(client, nullptr, this, nullptr);
    if (client->state() != QAbstractSocket::UnconnectedState) {
        client->abort();
    }
    client->deleteLater();
}

void MovingBaseInputTransport::readUdpDatagrams()
{
    const quint64 generation = m_generation;
    QUdpSocket *socket = m_udp;
    if (!socket || (m_settings.mode != Mode::UdpHost
                    && m_settings.mode != Mode::UdpClient)) {
        return;
    }
    int processed = 0;
    while (generation == m_generation && socket == m_udp
           && socket->hasPendingDatagrams()
           && processed < MaximumDatagramsPerDrain) {
        ++processed;
        const QNetworkDatagram datagram =
            socket->receiveDatagram(MaximumDatagramBytes);
        if (!datagram.isValid()) {
            const QString reason = tr("Moving Base UDP receive failed: %1")
                .arg(socket->errorString());
            fail(reason, generation);
            return;
        }
        if (datagram.data().isEmpty()) {
            continue;
        }
        const QHostAddress senderAddress = datagram.senderAddress();
        const quint16 senderPort = static_cast<quint16>(
            qMax<qint64>(0, datagram.senderPort()));
        const QString sender = endpointText(senderAddress, senderPort);

        if (m_settings.mode == Mode::UdpHost && !m_udpSourcePinned) {
            m_udpSourcePinned = true;
            m_udpSourceAddress = senderAddress;
            m_udpSourcePort = senderPort;
            m_lineFramer.reset();
            if (!setSourceDescription(sender, generation)) {
                return;
            }
            if (!transition(
                    State::Ready,
                    tr("Reading Moving Base NMEA from pinned UDP source %1.")
                        .arg(sender),
                    generation)) {
                return;
            }
        }

        if (!m_udpSourcePinned || senderAddress != m_udpSourceAddress
            || senderPort != m_udpSourcePort) {
            ++m_ignoredDatagramCount;
            if (!notifyStatistics(generation)) {
                return;
            }
            QPointer<MovingBaseInputTransport> guard(this);
            emit foreignDatagramIgnored(sender);
            if (!guard || generation != m_generation) {
                return;
            }
            continue;
        }
        QPointer<MovingBaseInputTransport> guard(this);
        ingestBytes(datagram.data(), generation);
        if (!guard || generation != m_generation) {
            return;
        }
    }
    if (generation == m_generation && socket == m_udp
        && socket->hasPendingDatagrams()) {
        // Yield between bounded drains so a datagram flood cannot monopolize
        // the GUI event loop. The QObject context cancels this callback on
        // destruction; generation and socket identity reject a restarted run.
        QTimer::singleShot(0, this, [this, socket, generation]() {
            if (generation == m_generation && socket == m_udp) {
                readUdpDatagrams();
            }
        });
    }
}

void MovingBaseInputTransport::ingestBytes(
    const QByteArray &bytes, quint64 generation)
{
    if (generation != m_generation || bytes.isEmpty()) {
        return;
    }
    const NmeaLineFramerResult framed = m_lineFramer.ingest(bytes);
    for (int index = 0; index < framed.oversizedLines; ++index) {
        if (generation != m_generation) {
            return;
        }
        ++m_rejectedLineCount;
        if (!notifyStatistics(generation)) {
            return;
        }
        QPointer<MovingBaseInputTransport> guard(this);
        emit lineRejected(
            tr("Moving Base NMEA line exceeds %1 bytes.")
                .arg(MaximumLineBytes));
        if (!guard || generation != m_generation) {
            return;
        }
    }
    for (const QByteArray &line : framed.lines) {
        if (generation != m_generation) {
            return;
        }
        QPointer<MovingBaseInputTransport> guard(this);
        processLine(line, generation);
        if (!guard || generation != m_generation) {
            return;
        }
    }
}

void MovingBaseInputTransport::processLine(
    const QByteArray &line, quint64 generation)
{
    QPointer<MovingBaseInputTransport> guard(this);
    emit rawLineReceived(line);
    if (!guard || generation != m_generation) {
        return;
    }

    const NmeaGgaParseResult result = NmeaGgaParser::parse(
        QString::fromLatin1(line));
    if (result.isValid()) {
        ++m_acceptedFixCount;
        if (!notifyStatistics(generation)) {
            return;
        }
        emit fixReceived(result.fix);
        return;
    }

    ++m_rejectedLineCount;
    if (!notifyStatistics(generation)) {
        return;
    }
    if (result.errorCode == NmeaGgaParseError::NoPositionFix) {
        emit noPositionFix(result.error);
        return;
    }
    emit sentenceRejected(result.errorCode, result.error);
}

bool MovingBaseInputTransport::transition(
    State state, const QString &status, quint64 generation)
{
    if (generation != m_generation) {
        return false;
    }
    QPointer<MovingBaseInputTransport> guard(this);
    if (m_state != state) {
        m_state = state;
        emit stateChanged(state);
        if (!guard || generation != m_generation) {
            return false;
        }
    }
    if (m_statusText != status) {
        m_statusText = status;
        emit statusChanged(status);
        if (!guard || generation != m_generation) {
            return false;
        }
    }
    return true;
}

bool MovingBaseInputTransport::setSourceDescription(
    const QString &source, quint64 generation)
{
    if (generation != m_generation) {
        return false;
    }
    if (m_sourceDescription == source) {
        return true;
    }
    m_sourceDescription = source;
    QPointer<MovingBaseInputTransport> guard(this);
    emit sourceChanged(source);
    return guard && generation == m_generation;
}

bool MovingBaseInputTransport::notifyStatistics(quint64 generation)
{
    if (generation != m_generation) {
        return false;
    }
    QPointer<MovingBaseInputTransport> guard(this);
    emit statisticsChanged();
    return guard && generation == m_generation;
}

void MovingBaseInputTransport::fail(
    const QString &error, quint64 generation)
{
    if (generation != m_generation) {
        return;
    }
    const QString reason = error.trimmed().isEmpty()
        ? tr("Moving Base input failed.") : error;
    closeDevices();
    m_lineFramer.reset();
    m_udpSourcePinned = false;
    m_udpSourceAddress = QHostAddress();
    m_udpSourcePort = 0;
    m_sourceDescription.clear();
    m_lastError = reason;
    if (!transition(State::Error, reason, generation)) {
        return;
    }
    QPointer<MovingBaseInputTransport> guard(this);
    emit sourceChanged(QString());
    if (!guard || generation != m_generation) {
        return;
    }
    emit errorOccurred(reason);
}

void MovingBaseInputTransport::closeDevices()
{
    if (m_hostLookupId >= 0) {
        QHostInfo::abortHostLookup(m_hostLookupId);
        m_hostLookupId = -1;
    }
    if (m_tcpClient) {
        dropTcpClient(m_tcpClient);
    }
    if (m_tcpServer) {
        QTcpServer *server = m_tcpServer;
        m_tcpServer = nullptr;
        disconnect(server, nullptr, this, nullptr);
        server->close();
        server->deleteLater();
    }
    if (m_udp) {
        QUdpSocket *socket = m_udp;
        m_udp = nullptr;
        disconnect(socket, nullptr, this, nullptr);
        socket->close();
        socket->deleteLater();
    }
    if (m_serial) {
        QSerialPort *serial = m_serial;
        m_serial = nullptr;
        disconnect(serial, nullptr, this, nullptr);
        serial->close();
        serial->deleteLater();
    }
}

void MovingBaseInputTransport::shutdown(bool notify)
{
    ++m_generation;
    const quint64 generation = m_generation;
    closeDevices();
    m_lineFramer.reset();
    m_udpSourcePinned = false;
    m_udpSourceAddress = QHostAddress();
    m_udpSourcePort = 0;
    const bool hadSource = !m_sourceDescription.isEmpty();
    m_sourceDescription.clear();
    m_lastError.clear();
    const bool stateChangedValue = m_state != State::Stopped;
    const bool statusChangedValue = m_statusText != QStringLiteral("Stopped.");
    m_state = State::Stopped;
    m_statusText = QStringLiteral("Stopped.");
    if (!notify) {
        return;
    }
    QPointer<MovingBaseInputTransport> guard(this);
    if (hadSource) {
        emit sourceChanged(QString());
        if (!guard || generation != m_generation) {
            return;
        }
    }
    if (stateChangedValue) {
        emit stateChanged(State::Stopped);
        if (!guard || generation != m_generation) {
            return;
        }
    }
    if (statusChangedValue) {
        emit statusChanged(QStringLiteral("Stopped."));
    }
}

bool MovingBaseInputTransport::parseListenAddress(
    const QString &text, QHostAddress *address)
{
    if (!address) {
        return false;
    }
    const QString value = text.trimmed();
    if (value.isEmpty() || value == QStringLiteral("*")
        || value == QStringLiteral("0.0.0.0")) {
        *address = QHostAddress::AnyIPv4;
        return true;
    }
    return address->setAddress(value);
}

QString MovingBaseInputTransport::endpointText(
    const QHostAddress &address, quint16 port)
{
    const QString host =
        address.protocol() == QAbstractSocket::IPv6Protocol
        ? QStringLiteral("[%1]").arg(address.toString())
        : address.toString();
    return QStringLiteral("%1:%2").arg(host).arg(port);
}
