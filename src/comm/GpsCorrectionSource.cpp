#include "GpsCorrectionSource.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDateTime>
#include <QIODevice>
#include <QPointer>
#include <QRegularExpression>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSslSocket>
#include <QTcpSocket>
#include <QThread>
#include <QTime>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kConnectTimeoutMs = 10000;
constexpr int kDataWatchdogMs = 30000;
constexpr int kGgaIntervalMs = 30000;
constexpr int kMaximumHeaderSize = 32768;
constexpr qint64 kMaximumChunkSize = 4 * 1024 * 1024;
constexpr qint64 kSocketReadBufferSize = 256 * 1024;

struct NtripEndpoint
{
    QString host;
    int port = -1;
    bool tls = false;
    bool hasUserInfo = false;
    bool valid = false;
};

NtripEndpoint parseEndpoint(const QString &text, int defaultPort)
{
    QString candidate = text.trimmed();
    if (!candidate.contains(QStringLiteral("://"))) {
        candidate.prepend(QStringLiteral("ntrip://"));
    }
    const QUrl url(candidate, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();

    NtripEndpoint endpoint;
    endpoint.host = url.host();
    endpoint.port = url.port(defaultPort);
    endpoint.tls = scheme == QLatin1String("https")
        || (scheme == QLatin1String("ntrip") && endpoint.port == 443);
    endpoint.hasUserInfo = !url.userInfo().isEmpty();
    endpoint.valid = url.isValid() && !endpoint.host.isEmpty()
        && endpoint.port > 0 && endpoint.port <= 65535
        && (scheme == QLatin1String("ntrip")
            || scheme == QLatin1String("http")
            || scheme == QLatin1String("https"));
    return endpoint;
}

QByteArray hostHeader(const NtripEndpoint &endpoint)
{
    QString host = endpoint.host;
    if (host.contains(QLatin1Char(':'))
        && !host.startsWith(QLatin1Char('['))) {
        host = QLatin1Char('[') + host + QLatin1Char(']');
    }
    return host.toUtf8() + ':' + QByteArray::number(endpoint.port);
}

bool containsControlCharacters(const QString &text)
{
    static const QRegularExpression controls(
        QStringLiteral("[\\r\\n\\x00-\\x1f]"));
    return text.contains(controls);
}

QByteArray nmeaCoordinate(double degrees, bool latitude)
{
    double absolute = std::abs(degrees);
    int wholeDegrees = static_cast<int>(absolute);
    double minutes = (absolute - wholeDegrees) * 60.0;
    minutes = std::round(minutes * 10000.0) / 10000.0;
    if (minutes >= 60.0) {
        minutes -= 60.0;
        ++wholeDegrees;
    }
    QByteArray minuteText = QByteArray::number(minutes, 'f', 4);
    if (minutes < 10.0) {
        minuteText.prepend('0');
    }
    return QByteArray::number(wholeDegrees).rightJustified(
               latitude ? 2 : 3, '0') + minuteText;
}

quint8 nmeaChecksum(const QByteArray &body)
{
    quint8 checksum = 0;
    for (char character : body) {
        checksum ^= static_cast<quint8>(character);
    }
    return checksum;
}
} // namespace

quint64 GpsCorrectionSource::receiverSession() const noexcept
{
    return 0;
}

bool GpsCorrectionSource::canConfigureReceiver() const noexcept
{
    return false;
}

int GpsCorrectionSource::receiverBaudRate() const noexcept
{
    return 0;
}

bool GpsCorrectionSource::setReceiverBaudRate(
    int, quint64, QString *error)
{
    if (error) {
        *error = tr("This correction source cannot change receiver baud rate.");
    }
    return false;
}

bool GpsCorrectionSource::writeReceiverData(
    const QByteArray &, quint64, QString *error)
{
    if (error) {
        *error = tr("This correction source does not expose a writable receiver.");
    }
    return false;
}

QString GpsCorrectionSourceSettings::validationError() const
{
    if (isNtrip()) {
        if (host.trimmed().isEmpty()) {
            return QCoreApplication::translate(
                "GpsCorrectionSource", "NTRIP host is required.");
        }
        if (casterPort <= 0 || casterPort > 65535) {
            return QCoreApplication::translate(
                "GpsCorrectionSource", "NTRIP port is invalid.");
        }
        if (containsControlCharacters(host)
            || containsControlCharacters(mountPoint)
            || containsControlCharacters(username)
            || containsControlCharacters(password)) {
            return QCoreApplication::translate(
                "GpsCorrectionSource",
                "NTRIP settings contain invalid control characters.");
        }
        if (username.contains(QLatin1Char(':'))) {
            return QCoreApplication::translate(
                "GpsCorrectionSource",
                "NTRIP username must not contain ':'.");
        }
        const NtripEndpoint endpoint = parseEndpoint(host, casterPort);
        if (!endpoint.valid) {
            return QCoreApplication::translate(
                "GpsCorrectionSource", "NTRIP host is invalid.");
        }
        if (endpoint.hasUserInfo) {
            return QCoreApplication::translate(
                "GpsCorrectionSource",
                "Enter NTRIP credentials in the username and password fields.");
        }
        return {};
    }
    if (selectedPort.trimmed().isEmpty()) {
        return QCoreApplication::translate(
            "GpsCorrectionSource", "Select a serial port.");
    }
    if (baudRate <= 0) {
        return QCoreApplication::translate(
            "GpsCorrectionSource", "Serial baud rate is invalid.");
    }
    return {};
}

QtGpsCorrectionSource::QtGpsCorrectionSource(QObject *parent)
    : GpsCorrectionSource(parent),
      m_connectTimer(new QTimer(this)),
      m_watchdogTimer(new QTimer(this)),
      m_ggaTimer(new QTimer(this))
{
    m_connectTimer->setSingleShot(true);
    m_watchdogTimer->setSingleShot(true);
    m_ggaTimer->setInterval(kGgaIntervalMs);
    connect(m_connectTimer, &QTimer::timeout, this, [this]() {
        fail(tr("Connect failed: connection timeout."));
    });
    connect(m_watchdogTimer, &QTimer::timeout, this, [this]() {
        fail(tr("Inject error: no correction data received for 30 seconds."));
    });
    connect(m_ggaTimer, &QTimer::timeout,
            this, &QtGpsCorrectionSource::sendGga);
}

QtGpsCorrectionSource::~QtGpsCorrectionSource()
{
    stop();
}

QStringList QtGpsCorrectionSource::availablePorts() const
{
    QStringList ports;
    for (const QSerialPortInfo &port : QSerialPortInfo::availablePorts()) {
        ports.append(port.portName());
    }
    ports.removeDuplicates();
    std::sort(ports.begin(), ports.end(), [](const QString &left,
                                             const QString &right) {
        return QString::localeAwareCompare(left, right) < 0;
    });
    ports.append(QStringLiteral("NTRIP"));
    return ports;
}

bool QtGpsCorrectionSource::start(
    const GpsCorrectionSourceSettings &settings)
{
    if (m_active) {
        emit statusChanged(tr("Disconnect the current correction source first."));
        return false;
    }
    const QString validation = settings.validationError();
    if (!validation.isEmpty()) {
        emit statusChanged(validation);
        return false;
    }

    m_settings = settings;
    m_parser.reset();
    m_responseBuffer.clear();
    m_chunkBuffer.clear();
    m_chunkBytesRemaining = -1;
    m_handshakeComplete = false;
    m_chunkedTransfer = false;
    m_chunkedFinished = false;
    setState(true, false);
    if (m_settings.isNtrip()) {
        startNtrip();
    } else {
        startSerial();
    }
    return m_active;
}

void QtGpsCorrectionSource::startSerial()
{
    m_serialPort = new QSerialPort(this);
    m_serialPort->setPortName(m_settings.selectedPort);
    m_serialPort->setBaudRate(m_settings.baudRate);
    m_serialPort->setDataBits(QSerialPort::Data8);
    m_serialPort->setParity(QSerialPort::NoParity);
    m_serialPort->setStopBits(QSerialPort::OneStop);
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);
    m_serialPort->setReadBufferSize(64 * 1024);
    if (!m_serialPort->open(QIODevice::ReadWrite)) {
        fail(tr("Connect failed: %1").arg(m_serialPort->errorString()));
        return;
    }
    if (m_nextReceiverSession == 0) {
        fail(tr("Connect failed: receiver session identifiers are exhausted."));
        return;
    }
    m_receiverSession = m_nextReceiverSession++;
    m_device = m_serialPort;
    connect(m_serialPort, &QSerialPort::readyRead,
            this, &QtGpsCorrectionSource::readAvailable);
    connect(m_serialPort, &QSerialPort::errorOccurred,
            this, [this](QSerialPort::SerialPortError error) {
        if (m_closing || error == QSerialPort::NoError) {
            return;
        }
        fail(tr("Inject error: %1").arg(m_serialPort->errorString()));
    });
    setState(true, true);
    emit statusChanged(tr("Connected — receiving RTCM correction data."));
    m_watchdogTimer->start(kDataWatchdogMs);
}

QString QtGpsCorrectionSource::normalizedHost() const
{
    return parseEndpoint(m_settings.host, m_settings.casterPort).host;
}

int QtGpsCorrectionSource::normalizedPort() const
{
    return parseEndpoint(m_settings.host, m_settings.casterPort).port;
}

bool QtGpsCorrectionSource::useTls() const
{
    return parseEndpoint(m_settings.host, m_settings.casterPort).tls;
}

void QtGpsCorrectionSource::startNtrip()
{
    if (useTls()) {
        auto *ssl = new QSslSocket(this);
        m_socket = ssl;
        connect(ssl, &QSslSocket::encrypted,
                this, &QtGpsCorrectionSource::handleNtripConnected);
    } else {
        m_socket = new QTcpSocket(this);
        connect(m_socket, &QTcpSocket::connected,
                this, &QtGpsCorrectionSource::handleNtripConnected);
    }
    m_device = m_socket;
    m_socket->setReadBufferSize(kSocketReadBufferSize);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &QtGpsCorrectionSource::readAvailable);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &QtGpsCorrectionSource::handleSocketDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
        if (!m_closing && m_active && m_socket) {
            fail(tr("Connect failed: %1").arg(m_socket->errorString()));
        }
    });

    emit statusChanged(tr("Connecting to NTRIP caster…"));
    m_connectTimer->start(kConnectTimeoutMs);
    if (auto *ssl = qobject_cast<QSslSocket *>(m_socket)) {
        ssl->connectToHostEncrypted(normalizedHost(),
                                    static_cast<quint16>(normalizedPort()));
    } else {
        m_socket->connectToHost(normalizedHost(),
                                static_cast<quint16>(normalizedPort()));
    }
}

QByteArray QtGpsCorrectionSource::buildNtripRequest(
    const GpsCorrectionSourceSettings &settings,
    bool *credentialsInClear)
{
    const NtripEndpoint endpoint = parseEndpoint(
        settings.host, settings.casterPort);
    QString mount = settings.mountPoint.trimmed();
    while (mount.startsWith(QLatin1Char('/'))) {
        mount.remove(0, 1);
    }
    const QByteArray encodedMount = QUrl::toPercentEncoding(mount);

    QByteArray request = "GET /" + encodedMount
        + (settings.ntripV1 ? " HTTP/1.0\r\n" : " HTTP/1.1\r\n");
    if (!settings.ntripV1) {
        request += "Host: " + hostHeader(endpoint) + "\r\n";
        request += "Ntrip-Version: Ntrip/2.0\r\n";
    }
    request += "User-Agent: NTRIP APMPlanner3/1.0\r\n";
    const bool hasCredentials = !settings.username.isEmpty()
        || !settings.password.isEmpty();
    if (hasCredentials) {
        const QByteArray credentials = settings.username.toUtf8() + ':'
            + settings.password.toUtf8();
        request += "Authorization: Basic " + credentials.toBase64()
            + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    if (credentialsInClear) {
        *credentialsInClear = hasCredentials && !endpoint.tls;
    }
    return request;
}

int QtGpsCorrectionSource::parseNtripStatusCode(
    const QByteArray &statusLine)
{
    static const QRegularExpression expression(
        QStringLiteral("^\\S+\\s+(\\d{3})(?:\\s+.*)?$"));
    const QRegularExpressionMatch match = expression.match(
        QString::fromUtf8(statusLine).trimmed());
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

void QtGpsCorrectionSource::handleNtripConnected()
{
    if (!m_active || !m_socket) {
        return;
    }
    // These options require an initialized native socket engine, which is
    // only guaranteed after TCP connect / TLS encryption completes.
    m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    bool clearCredentials = false;
    const QByteArray request = buildNtripRequest(
        m_settings, &clearCredentials);
    if (clearCredentials) {
        emit plaintextCredentialsWarning();
    }
    m_socket->write(request);
    emit statusChanged(tr("Waiting for NTRIP caster response…"));
}

void QtGpsCorrectionSource::readAvailable()
{
    if (!m_active || !m_device) {
        return;
    }
    const QByteArray bytes = m_device->readAll();
    if (bytes.isEmpty()) {
        return;
    }
    if (m_serialPort && m_device == m_serialPort
        && m_receiverSession != 0) {
        const QPointer<QtGpsCorrectionSource> guard(this);
        const quint64 session = m_receiverSession;
        // During survey-in a receiver can legitimately emit UBX NAV status
        // for longer than the RTCM watchdog interval.  Raw serial activity
        // proves that this exact local receiver session is alive; NTRIP still
        // refreshes its watchdog only after a CRC-valid RTCM frame below.
        m_watchdogTimer->start(kDataWatchdogMs);
        emit receiverBytes(bytes, session);
        if (!guard || !m_active || !m_connected
            || m_receiverSession != session) {
            return;
        }
    }
    if (m_settings.isNtrip() && !m_handshakeComplete) {
        handleNtripResponse(bytes);
        return;
    }
    if (m_settings.isNtrip()) {
        processNtripPayload(bytes);
    } else {
        ingestBytes(bytes);
    }
}

void QtGpsCorrectionSource::handleNtripResponse(const QByteArray &bytes)
{
    m_responseBuffer.append(bytes);
    const int firstLineEnd = m_responseBuffer.indexOf("\r\n");
    if (firstLineEnd < 0) {
        if (m_responseBuffer.size() > kMaximumHeaderSize) {
            fail(tr("Connect failed: NTRIP response header is too large."));
        }
        return;
    }
    int headerEnd = -1;
    int dataOffset = -1;
    const bool icyResponse = m_responseBuffer.startsWith("ICY ");
    if (icyResponse) {
        // NTRIP v1 ICY responses conventionally end at the status line.
        // Do not search binary RTCM for a false CRLF/CRLF delimiter.
        headerEnd = firstLineEnd;
        dataOffset = firstLineEnd + 2;
    } else {
        headerEnd = m_responseBuffer.indexOf("\r\n\r\n");
        dataOffset = headerEnd >= 0 ? headerEnd + 4 : -1;
    }
    if (headerEnd < 0) {
        if (m_responseBuffer.size() > kMaximumHeaderSize) {
            fail(tr("Connect failed: NTRIP response header is too large."));
        }
        return;
    }
    if (headerEnd > kMaximumHeaderSize) {
        fail(tr("Connect failed: NTRIP response header is too large."));
        return;
    }

    const QByteArray firstLine = m_responseBuffer.left(
        firstLineEnd > 0 ? firstLineEnd : headerEnd).trimmed();
    if (QString::fromLatin1(firstLine).startsWith(
            QLatin1String("SOURCETABLE"), Qt::CaseInsensitive)) {
        fail(tr("Connect failed: bad NTRIP mount point (source table returned)."));
        return;
    }
    const int code = parseNtripStatusCode(firstLine);
    if (code < 200 || code >= 300) {
        if (code == 401) {
            fail(tr("Connect failed: NTRIP authentication failed (401)."));
        } else if (code > 0) {
            fail(tr("Connect failed: NTRIP caster returned HTTP %1.")
                     .arg(code));
        } else {
            fail(tr("Connect failed: invalid NTRIP caster response."));
        }
        return;
    }

    if (!icyResponse) {
        const QByteArray headers = m_responseBuffer.left(dataOffset);
        const QList<QByteArray> lines = headers.split('\n');
        for (QByteArray line : lines) {
            line = line.trimmed();
            const QByteArray lower = line.toLower();
            if (lower.startsWith("content-type:")
                && lower.contains("gnss/sourcetable")) {
                fail(tr("Connect failed: bad NTRIP mount point (source table returned)."));
                return;
            }
            if (lower.startsWith("transfer-encoding:")) {
                const QList<QByteArray> encodings =
                    lower.mid(sizeof("transfer-encoding:") - 1).split(',');
                for (const QByteArray &encoding : encodings) {
                    const QByteArray name = encoding.trimmed();
                    if (name == "chunked") {
                        m_chunkedTransfer = true;
                    } else if (!name.isEmpty()) {
                        fail(tr("Connect failed: unsupported NTRIP transfer encoding."));
                        return;
                    }
                }
            }
        }
    }

    const QByteArray trailing = m_responseBuffer.mid(dataOffset);
    m_responseBuffer.clear();
    completeNtripHandshake(trailing);
}

void QtGpsCorrectionSource::completeNtripHandshake(
    const QByteArray &trailingData)
{
    m_connectTimer->stop();
    m_handshakeComplete = true;
    setState(true, true);
    emit statusChanged(tr("Connected — receiving RTCM correction data."));
    m_watchdogTimer->start(kDataWatchdogMs);
    if (m_settings.sendGga) {
        sendGga();
        m_ggaTimer->start();
    }
    if (!trailingData.isEmpty()) {
        processNtripPayload(trailingData);
    }
}

void QtGpsCorrectionSource::processNtripPayload(const QByteArray &bytes)
{
    if (bytes.isEmpty() || m_chunkedFinished) {
        return;
    }
    if (!m_chunkedTransfer) {
        ingestBytes(bytes);
        return;
    }

    m_chunkBuffer.append(bytes);
    while (!m_chunkedFinished) {
        if (m_chunkBytesRemaining < 0) {
            const int lineEnd = m_chunkBuffer.indexOf("\r\n");
            if (lineEnd < 0) {
                if (m_chunkBuffer.size() > kMaximumHeaderSize) {
                    fail(tr("Inject error: invalid NTRIP chunk header."));
                }
                return;
            }
            QByteArray sizeText = m_chunkBuffer.left(lineEnd).trimmed();
            m_chunkBuffer.remove(0, lineEnd + 2);
            const int extension = sizeText.indexOf(';');
            if (extension >= 0) {
                sizeText.truncate(extension);
                sizeText = sizeText.trimmed();
            }
            bool ok = false;
            const qulonglong chunkSize = sizeText.toULongLong(&ok, 16);
            if (!ok || chunkSize > static_cast<qulonglong>(kMaximumChunkSize)) {
                fail(tr("Inject error: invalid NTRIP chunk size."));
                return;
            }
            if (chunkSize == 0) {
                m_chunkedFinished = true;
                m_chunkBuffer.clear();
                return;
            }
            m_chunkBytesRemaining = static_cast<qint64>(chunkSize);
        }

        if (m_chunkBuffer.size() < m_chunkBytesRemaining + 2) {
            return;
        }
        if (m_chunkBuffer.mid(
                static_cast<int>(m_chunkBytesRemaining), 2) != "\r\n") {
            fail(tr("Inject error: malformed NTRIP chunk."));
            return;
        }
        const QByteArray payload = m_chunkBuffer.left(
            static_cast<int>(m_chunkBytesRemaining));
        m_chunkBuffer.remove(
            0, static_cast<int>(m_chunkBytesRemaining) + 2);
        m_chunkBytesRemaining = -1;
        ingestBytes(payload);
    }
}

void QtGpsCorrectionSource::ingestBytes(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    emit inputBytes(bytes.size());
    for (char character : bytes) {
        if (!m_parser.addByte(static_cast<quint8>(character))) {
            continue;
        }
        const quint16 id = m_parser.messageId();
        if (m_parser.validateCrc()) {
            if (!m_settings.isNtrip() || m_handshakeComplete) {
                m_watchdogTimer->start(kDataWatchdogMs);
            }
            emit rtcmFrame(m_parser.currentFrame(), id);
        } else {
            emit invalidRtcmFrame(id);
        }
        m_parser.reset();
    }
}

QByteArray QtGpsCorrectionSource::makeGga(
    double latitude, double longitude, double altitudeMsl)
{
    if (!std::isfinite(latitude) || !std::isfinite(longitude)
        || !std::isfinite(altitudeMsl)
        || latitude < -90.0 || latitude > 90.0
        || longitude < -180.0 || longitude > 180.0) {
        return {};
    }
    const QTime utc = QDateTime::currentDateTimeUtc().time();
    QByteArray body = "GPGGA,";
    body += QByteArray::number(utc.hour()).rightJustified(2, '0');
    body += QByteArray::number(utc.minute()).rightJustified(2, '0');
    body += QByteArray::number(utc.second()).rightJustified(2, '0');
    body += ',' + nmeaCoordinate(latitude, true) + ',';
    body += latitude < 0.0 ? 'S' : 'N';
    body += ',' + nmeaCoordinate(longitude, false) + ',';
    body += longitude < 0.0 ? 'W' : 'E';
    body += ",1,10,1.0," + QByteArray::number(altitudeMsl, 'f', 1)
        + ",M,0.0,M,,";
    return '$' + body + '*'
        + QByteArray::number(nmeaChecksum(body), 16)
              .rightJustified(2, '0').toUpper()
        + "\r\n";
}

void QtGpsCorrectionSource::setGgaPosition(
    double latitude, double longitude, double altitudeMsl, bool valid)
{
    m_ggaLatitude = latitude;
    m_ggaLongitude = longitude;
    m_ggaAltitude = altitudeMsl;
    m_ggaValid = valid && std::isfinite(latitude)
        && std::isfinite(longitude) && std::isfinite(altitudeMsl);
}

quint64 QtGpsCorrectionSource::receiverSession() const noexcept
{
    return canConfigureReceiver() ? m_receiverSession : 0;
}

bool QtGpsCorrectionSource::canConfigureReceiver() const noexcept
{
    return m_active && m_connected && m_serialPort
        && m_device == m_serialPort && m_serialPort->isOpen()
        && m_receiverSession != 0;
}

int QtGpsCorrectionSource::receiverBaudRate() const noexcept
{
    return canConfigureReceiver()
        ? static_cast<int>(m_serialPort->baudRate()) : 0;
}

bool QtGpsCorrectionSource::setReceiverBaudRate(
    int baudRate, quint64 expectedSession, QString *error)
{
    if (error) {
        error->clear();
    }
    if (QThread::currentThread() != thread()) {
        if (error) {
            *error = tr("Receiver baud-rate changes must run on the source thread.");
        }
        return false;
    }
    if (!canConfigureReceiver() || expectedSession == 0
        || expectedSession != m_receiverSession) {
        if (error) {
            *error = tr("The serial receiver session changed or is unavailable.");
        }
        return false;
    }
    if (baudRate <= 0 || !m_serialPort->setBaudRate(baudRate)) {
        if (error) {
            *error = tr("Receiver baud-rate change failed: %1")
                .arg(m_serialPort->errorString());
        }
        return false;
    }
    return true;
}

bool QtGpsCorrectionSource::writeReceiverData(
    const QByteArray &bytes, quint64 expectedSession, QString *error)
{
    if (error) {
        error->clear();
    }
    if (QThread::currentThread() != thread()) {
        if (error) {
            *error = tr("Receiver writes must run on the source thread.");
        }
        return false;
    }
    if (bytes.isEmpty()) {
        if (error) {
            *error = tr("Receiver configuration data is empty.");
        }
        return false;
    }
    if (!canConfigureReceiver() || expectedSession == 0
        || expectedSession != m_receiverSession) {
        if (error) {
            *error = tr("The serial receiver session changed or is unavailable.");
        }
        return false;
    }
    const qint64 accepted = m_serialPort->write(bytes);
    if (accepted != bytes.size()) {
        if (error) {
            *error = accepted < 0
                ? tr("Receiver write failed: %1")
                      .arg(m_serialPort->errorString())
                : tr("Receiver accepted only %1 of %2 configuration bytes.")
                      .arg(accepted).arg(bytes.size());
        }
        return false;
    }
    return true;
}

void QtGpsCorrectionSource::sendGga()
{
    if (!m_connected || !m_settings.isNtrip() || !m_settings.sendGga
        || !m_ggaValid || !m_socket) {
        return;
    }
    const QByteArray sentence = makeGga(
        m_ggaLatitude, m_ggaLongitude, m_ggaAltitude);
    if (!sentence.isEmpty()) {
        m_socket->write(sentence);
    }
}

void QtGpsCorrectionSource::handleSocketDisconnected()
{
    if (!m_closing && m_active) {
        fail(tr("NTRIP connection closed."));
    }
}

void QtGpsCorrectionSource::stop()
{
    if (!m_active && !m_device && !m_socket && !m_serialPort) {
        return;
    }
    m_connectTimer->stop();
    m_watchdogTimer->stop();
    m_ggaTimer->stop();
    closeDevice();
    m_parser.reset();
    m_responseBuffer.clear();
    m_chunkBuffer.clear();
    m_chunkBytesRemaining = -1;
    m_handshakeComplete = false;
    m_chunkedTransfer = false;
    m_chunkedFinished = false;
    setState(false, false);
}

void QtGpsCorrectionSource::closeDevice()
{
    m_closing = true;
    m_receiverSession = 0;
    if (m_device) {
        m_device->disconnect(this);
        m_device->close();
    }
    if (m_socket) {
        m_socket->deleteLater();
    } else if (m_serialPort) {
        m_serialPort->deleteLater();
    }
    m_device = nullptr;
    m_socket = nullptr;
    m_serialPort = nullptr;
    m_closing = false;
}

void QtGpsCorrectionSource::setState(bool active, bool connected)
{
    if (m_active == active && m_connected == connected) {
        return;
    }
    m_active = active;
    m_connected = connected;
    emit stateChanged(m_active, m_connected);
}

void QtGpsCorrectionSource::fail(const QString &status)
{
    m_connectTimer->stop();
    m_watchdogTimer->stop();
    m_ggaTimer->stop();
    closeDevice();
    m_parser.reset();
    m_responseBuffer.clear();
    m_chunkBuffer.clear();
    m_chunkBytesRemaining = -1;
    m_handshakeComplete = false;
    m_chunkedTransfer = false;
    m_chunkedFinished = false;
    setState(false, false);
    emit statusChanged(status);
}
