#ifndef COTOUTPUTTRANSPORT_H
#define COTOUTPUTTRANSPORT_H

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

class QSerialPort;
class QHostInfo;
class QTcpServer;
class QTcpSocket;
class QUdpSocket;

/*
 * Event-driven transports for Mission Planner 10 Cursor-on-Target output.
 *
 * The class owns no timer and does not generate CoT: its caller supplies one
 * complete UTF-8 event, including its trailing LF, to send().  UDP modes map
 * one successful send() call to exactly one datagram.  Stream transports use
 * Qt's asynchronous write queue, bounded below, and never wait for I/O.
 *
 * The tool/service remains responsible for preventing a UDP/TCP host port from
 * colliding with the primary vehicle link before start() is called.
 */
class CotOutputTransport final : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        TakMulticast,
        UdpClient,
        UdpHost,
        TcpClient,
        TcpHost,
        Serial
    };
    Q_ENUM(Mode)

    enum class State {
        Stopped,
        Resolving,
        Connecting,
        Listening,
        Ready,
        Error
    };
    Q_ENUM(State)

    enum class SendResult {
        Sent,
        NotReady,
        NoPeer,
        InvalidPayload,
        Backpressure,
        IoError
    };
    Q_ENUM(SendResult)

    struct Settings
    {
        Mode mode = Mode::TakMulticast;
        QString host = QStringLiteral("239.2.3.1");
        quint16 port = 6969;
        QString serialPort;
        int baud = 57600;
    };

    struct PeerInfo
    {
        QString description;
        QHostAddress address;
        quint16 port = 0;
        qint64 pendingBytes = 0;
    };

    // One slow TCP peer can retain at most this many bytes in Qt's queue.
    static constexpr qint64 PerPeerPendingLimitBytes = 64 * 1024;
    // A host deliberately has a small, fixed fan-out to bound descriptors and
    // aggregate queued data (MaxTcpClients * PerPeerPendingLimitBytes).
    static constexpr int MaxTcpClients = 16;
    // Conservative maximum IPv4 UDP payload. CoT events are normally far smaller.
    static constexpr int MaximumPayloadBytes = 65507;

    static constexpr quint16 DefaultTakPort = 6969;
    static constexpr quint16 DefaultNetworkPort = 14551;
    static constexpr int DefaultSerialBaud = 57600;

    static QString ModeLabel(Mode mode);
    static Settings Defaults(Mode mode);

    explicit CotOutputTransport(const Settings &settings, QObject *parent = nullptr);
    ~CotOutputTransport() override;

    Settings settings() const { return m_settings; }
    State state() const { return m_state; }
    bool isStarted() const;
    bool isReady() const { return m_state == State::Ready; }
    bool hasPeer() const;
    int peerCount() const;
    QList<PeerInfo> peers() const;
    qint64 pendingBytes() const;
    QString statusText() const { return m_statusText; }
    QString lastError() const { return m_lastError; }

    // For host modes, returns the actual bound port (useful when port 0 was
    // requested by a test). Returns zero for an inactive/non-host transport.
    quint16 localPort() const;

    // Starts asynchronous DNS/connect work where required. A true return means
    // the start request was accepted; observe stateChanged() for readiness.
    bool start(QString *error = nullptr);
    void stop();

    // payload must be non-empty, no larger than MaximumPayloadBytes, and end LF.
    // No method blocks and no implicit retry/reconnect is performed.
    SendResult send(const QByteArray &payload);

signals:
    void stateChanged(CotOutputTransport::State state);
    void statusChanged(const QString &status);
    void peersChanged();
    // Stream bytes accepted by the OS/Qt; useful to retry after Backpressure.
    void bytesWritten(qint64 bytes);
    // A whole payload was queued/sent. peerCount is the fan-out for this send.
    void payloadSent(qint64 bytes, int peerCount);
    // Fatal and recoverable transport errors. state()/lastError() disambiguate.
    void errorOccurred(const QString &error);

private slots:
    void hostLookupFinished(const QHostInfo &hostInfo);

private:
    bool validateSettings(QString *error) const;
    bool startUdpDestination(QString *error);
    bool startUdpHost(QString *error);
    bool startTcpClient(QString *error);
    bool startTcpHost(QString *error);
    bool startSerial(QString *error);

    void completeUdpDestination(const QHostAddress &address, quint64 generation);
    void readUdpHostDatagrams();
    void acceptTcpClients();
    void removeTcpHostClient(QTcpSocket *client);
    void handleTcpHostClientFailure(QTcpSocket *client, const QString &reason);

    bool transition(State state, const QString &status);
    bool notifyPeers(const QString &status);
    void reportRecoverableError(const QString &error);
    void fail(const QString &error);
    void shutdown(bool notify);
    QString tcpHostStatus() const;

    Settings m_settings;
    State m_state = State::Stopped;
    QString m_statusText = QStringLiteral("Stopped.");
    QString m_lastError;
    quint64 m_generation = 0;
    int m_hostLookupId = -1;

    QSerialPort *m_serial = nullptr;
    QUdpSocket *m_udp = nullptr;
    QTcpSocket *m_tcpClient = nullptr;
    QTcpServer *m_tcpServer = nullptr;
    QList<QTcpSocket *> m_tcpClients;

    bool m_hasUdpPeer = false;
    QHostAddress m_udpPeerAddress;
    quint16 m_udpPeerPort = 0;
};

Q_DECLARE_METATYPE(CotOutputTransport::State)
Q_DECLARE_METATYPE(CotOutputTransport::SendResult)

#endif // COTOUTPUTTRANSPORT_H
