#ifndef MAVLINKMIRROROUTPUT_H
#define MAVLINKMIRROROUTPUT_H

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

class QSerialPort;
class QTcpServer;
class QTcpSocket;
class QUdpSocket;

/*
 * Secondary-port transports for the Mission Planner 10 TOOLS > MAVLink Mirror
 * window (ViewModels/SerialPassThroughViewModel.cs). One output carries the
 * mirrored vehicle frames to the peer and hands raw peer bytes back for the
 * optional write-back.
 *
 * Every output lives on the GUI thread, is event driven (no worker thread) and
 * never blocks: write() accepts at most OutputPendingLimitBytes of not yet
 * transmitted data and returns how much it took (0 = try again after
 * bytesWritten, -1 = fatal). The owner (MavlinkMirrorService) keeps its own
 * bounded queue in front of this limit, so mirror-owned memory is bounded end
 * to end.
 */
struct MavlinkMirrorSettings
{
    QString portSelection;   // serial port name, TcpHostSelection or UdpHostSelection
    int baud = 115200;       // serial only (MP10 default)
    bool allowWriteBack = false;
};

class MavlinkMirrorOutput : public QObject
{
    Q_OBJECT

public:
    enum class Kind {
        Serial,
        TcpHost,
        UdpHost
    };
    Q_ENUM(Kind)

    static constexpr qint64 OutputPendingLimitBytes = 64 * 1024;
    static constexpr quint16 DefaultHostPort = 14550;

    // Exact MP10 combo entries (SerialPassThroughViewModel.cs:82-83).
    static QString TcpHostSelection();   // "TCP Host - 14550"
    static QString UdpHostSelection();   // "UDP Host - 14550"
    static bool IsTcpHostSelection(const QString &selection);
    static bool IsUdpHostSelection(const QString &selection);
    // Serial ports first (MP10 order), then the two host entries.
    static QStringList Selections(const QStringList &serialPorts);
    // Production factory: does not open or bind anything; open() does.
    static std::unique_ptr<MavlinkMirrorOutput> Create(const MavlinkMirrorSettings &settings,
                                                       QString *error);

    explicit MavlinkMirrorOutput(QObject *parent = nullptr);
    ~MavlinkMirrorOutput() override;

    virtual Kind kind() const = 0;
    virtual QString selection() const = 0;   // the combo text this output was created for
    virtual bool open(QString *error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // A destination exists: serial open, TCP client connected, UDP peer learned.
    virtual bool hasPeer() const = 0;
    virtual qint64 write(const QByteArray &bytes) = 0;
    virtual qint64 pendingBytes() const = 0;
    // MP10 status texts, e.g. "Listening on TCP 14550 — waiting for a client…".
    virtual QString statusText() const = 0;

signals:
    void peerChanged();
    void bytesWritten(qint64 bytes);
    void peerBytesReceived(const QByteArray &bytes);
    void errorOccurred(const QString &text);
};

/** QSerialPort output, 8N1, no flow control. */
class SerialMirrorOutput final : public MavlinkMirrorOutput
{
    Q_OBJECT

public:
    SerialMirrorOutput(const QString &portName, int baud, QObject *parent = nullptr);
    ~SerialMirrorOutput() override;

    Kind kind() const override { return Kind::Serial; }
    QString selection() const override { return m_portName; }
    bool open(QString *error) override;
    void close() override;
    bool isOpen() const override;
    bool hasPeer() const override { return isOpen(); }
    qint64 write(const QByteArray &bytes) override;
    qint64 pendingBytes() const override;
    QString statusText() const override;

private:
    QString m_portName;
    int m_baud;
    QSerialPort *m_port = nullptr;
};

/** QTcpServer host; a single client, the newest connection replaces the previous one. */
class TcpHostMirrorOutput final : public MavlinkMirrorOutput
{
    Q_OBJECT

public:
    explicit TcpHostMirrorOutput(quint16 port = DefaultHostPort,
                                 const QHostAddress &address = QHostAddress::Any,
                                 QObject *parent = nullptr);
    ~TcpHostMirrorOutput() override;

    Kind kind() const override { return Kind::TcpHost; }
    QString selection() const override { return TcpHostSelection(); }
    bool open(QString *error) override;
    void close() override;
    bool isOpen() const override;
    bool hasPeer() const override { return m_client != nullptr; }
    qint64 write(const QByteArray &bytes) override;
    qint64 pendingBytes() const override;
    QString statusText() const override;

    quint16 serverPort() const;   // actual port (tests listen on 0)
    QString peerDescription() const;

private:
    void acceptPending();
    void dropClient(QTcpSocket *client);

    quint16 m_port;
    QHostAddress m_address;
    QTcpServer *m_server = nullptr;
    QTcpSocket *m_client = nullptr;
};

/**
 * QUdpSocket host. Like MP10 CommsUdpSerial.EndPointList, every distinct sender
 * becomes a peer and each mirrored frame is fanned out to all of them. The list
 * is capped at MaxPeers (oldest evicted) so a datagram flood cannot grow memory.
 */
class UdpHostMirrorOutput final : public MavlinkMirrorOutput
{
    Q_OBJECT

public:
    struct Peer
    {
        QHostAddress address;
        quint16 port = 0;
        bool operator==(const Peer &other) const
        {
            return port == other.port && address == other.address;
        }
    };

    static constexpr int MaxPeers = 32;

    explicit UdpHostMirrorOutput(quint16 port = DefaultHostPort,
                                 const QHostAddress &address = QHostAddress::AnyIPv4,
                                 QObject *parent = nullptr);
    ~UdpHostMirrorOutput() override;

    Kind kind() const override { return Kind::UdpHost; }
    QString selection() const override { return UdpHostSelection(); }
    bool open(QString *error) override;
    void close() override;
    bool isOpen() const override;
    bool hasPeer() const override { return !m_peers.isEmpty(); }
    // Fans one whole frame to every learned peer; never splits a frame.
    qint64 write(const QByteArray &bytes) override;
    qint64 pendingBytes() const override;
    QString statusText() const override;

    quint16 localPort() const;    // actual port (tests bind 0)
    QList<Peer> peers() const { return m_peers; }
    int peerCount() const { return m_peers.size(); }

private:
    void readDatagrams();

    quint16 m_port;
    QHostAddress m_address;
    QUdpSocket *m_socket = nullptr;
    QList<Peer> m_peers;   // oldest first
};

#endif // MAVLINKMIRROROUTPUT_H
