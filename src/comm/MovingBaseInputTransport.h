#ifndef MOVINGBASEINPUTTRANSPORT_H
#define MOVINGBASEINPUTTRANSPORT_H

#include "NmeaGgaParser.h"
#include "NmeaLineFramer.h"

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

class QHostInfo;
class QSerialPort;
class QTcpServer;
class QTcpSocket;
class QUdpSocket;

/**
 * Event-driven NMEA GGA input used by the Moving Base tool.
 *
 * One instance owns one serial or network input. Stream input is framed with
 * NmeaLineFramer's fixed per-record limit, Qt socket/serial read buffers are
 * capped, and all asynchronous callbacks are tied to a start generation.
 * TCP Host keeps only the newest client and discards the previous client's
 * partial line. UDP Host pins the first non-empty sender for the run. UDP
 * Client sends one empty discovery datagram after its ephemeral bind, then
 * accepts datagrams only from the configured resolved endpoint.
 */
class MovingBaseInputTransport final : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        Serial,
        TcpHost,
        TcpClient,
        UdpHost,
        UdpClient
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

    struct Settings
    {
        Mode mode = Mode::Serial;
        QString serialPort;
        int baud = 4800;
        QString host = QStringLiteral("127.0.0.1");
        quint16 port = 14551;
    };

    static constexpr int MaximumLineBytes =
        NmeaLineFramer::DefaultMaximumLineBytes;
    static constexpr qint64 ReadBufferLimitBytes = 64 * 1024;
    static constexpr qint64 MaximumDatagramBytes = 65507;
    static constexpr int MaximumPendingTcpClients = 16;
    static constexpr int MaximumDatagramsPerDrain = 256;
    static constexpr quint16 DefaultNetworkPort = 14551;
    static constexpr int DefaultSerialBaud = 4800;

    static QString modeLabel(Mode mode);
    static Settings defaults(Mode mode);
    static QList<int> supportedBaudRates();

    explicit MovingBaseInputTransport(
        const Settings &settings, QObject *parent = nullptr);
    ~MovingBaseInputTransport() override;

    Settings settings() const { return m_settings; }
    State state() const noexcept { return m_state; }
    bool isStarted() const noexcept;
    bool isReady() const noexcept { return m_state == State::Ready; }
    QString statusText() const { return m_statusText; }
    QString lastError() const { return m_lastError; }
    QString sourceDescription() const { return m_sourceDescription; }
    int bufferedLineBytes() const noexcept
    {
        return m_lineFramer.bufferedBytes();
    }

    /** Actual TCP-host or UDP local port; zero while not bound. */
    quint16 localPort() const;

    quint64 acceptedFixCount() const noexcept { return m_acceptedFixCount; }
    quint64 rejectedLineCount() const noexcept { return m_rejectedLineCount; }
    quint64 ignoredDatagramCount() const noexcept
    {
        return m_ignoredDatagramCount;
    }

    /**
     * Begin one input run. TCP/DNS readiness is asynchronous. A false return
     * means validation/open failed or a direct signal handler superseded the
     * run with stop()/start().
     */
    bool start(QString *error = nullptr);
    void stop();

signals:
    void stateChanged(MovingBaseInputTransport::State state);
    void statusChanged(const QString &status);
    void sourceChanged(const QString &source);

    /** One bounded NMEA line without CR/LF, suitable for the log owner. */
    void rawLineReceived(const QByteArray &line);
    void fixReceived(const NmeaGgaFix &fix);
    void noPositionFix(const QString &description);
    void sentenceRejected(NmeaGgaParseError errorCode,
                          const QString &description);
    void lineRejected(const QString &description);

    /** A UDP datagram from outside the pinned/configured endpoint was drained. */
    void foreignDatagramIgnored(const QString &source);
    void statisticsChanged();
    // Inspect state(): a TCP-host peer error is recoverable (Listening), while
    // serial/TCP-client/socket ownership failures transition to Error.
    void errorOccurred(const QString &error);

private slots:
    void hostLookupFinished(const QHostInfo &hostInfo, quint64 generation);

private:
    bool validateSettings(QString *error) const;
    bool startSerial(QString *error, quint64 generation);
    bool startTcpHost(QString *error, quint64 generation);
    bool startTcpClient(QString *error, quint64 generation);
    bool startUdpHost(QString *error, quint64 generation);
    bool startUdpClient(QString *error, quint64 generation);
    bool completeUdpClient(const QHostAddress &address, quint64 generation,
                           QString *error = nullptr);

    void acceptTcpClients();
    void dropTcpClient(QTcpSocket *client);
    void readUdpDatagrams();
    void ingestBytes(const QByteArray &bytes, quint64 generation);
    void processLine(const QByteArray &line, quint64 generation);

    bool transition(State state, const QString &status,
                    quint64 generation);
    bool setSourceDescription(const QString &source,
                              quint64 generation);
    bool notifyStatistics(quint64 generation);
    void fail(const QString &error, quint64 generation);
    void closeDevices();
    void shutdown(bool notify);

    static bool parseListenAddress(const QString &text,
                                   QHostAddress *address);
    static QString endpointText(const QHostAddress &address, quint16 port);

    Settings m_settings;
    State m_state = State::Stopped;
    QString m_statusText = QStringLiteral("Stopped.");
    QString m_lastError;
    QString m_sourceDescription;
    quint64 m_generation = 0;
    int m_hostLookupId = -1;

    QSerialPort *m_serial = nullptr;
    QTcpServer *m_tcpServer = nullptr;
    QTcpSocket *m_tcpClient = nullptr;
    QUdpSocket *m_udp = nullptr;

    bool m_udpSourcePinned = false;
    QHostAddress m_udpSourceAddress;
    quint16 m_udpSourcePort = 0;
    NmeaLineFramer m_lineFramer;

    quint64 m_acceptedFixCount = 0;
    quint64 m_rejectedLineCount = 0;
    quint64 m_ignoredDatagramCount = 0;
};

Q_DECLARE_METATYPE(MovingBaseInputTransport::Mode)
Q_DECLARE_METATYPE(MovingBaseInputTransport::State)

#endif // MOVINGBASEINPUTTRANSPORT_H
