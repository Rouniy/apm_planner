#ifndef SERIALBRIDGETCPSERVER_H
#define SERIALBRIDGETCPSERVER_H

#include <QObject>
#include <QByteArray>
#include <QPointer>

class QTcpServer;
class QTcpSocket;

// Owner-thread, event-driven TCP endpoint. No MAVLink parsing or pacing: the
// application service pulls input and schedules its own bounded serial chunks.
class SerialBridgeTcpServer final : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 MaximumInputBytes = 32 * 1024;
    static constexpr qint64 MaximumOutputBytes = 64 * 1024;
    explicit SerialBridgeTcpServer(QObject *parent = nullptr);
    ~SerialBridgeTcpServer() override;

    // false binds 127.0.0.1; true explicitly exposes AnyIPv4. Port0 lets the OS
    // choose an ephemeral port. Does not replace an already-running listener.
    bool start(quint16 port, bool allowRemote, QString *error = nullptr);
    void stop();
    bool isListening() const;
    // Logical ownership includes a graceful EOF whose accepted input has not
    // yet been acknowledged as drained by finishInput().
    bool hasClient() const;
    bool inputEnded() const;
    // Retire a graceful EOF only after both the socket and the caller's own
    // paced batch have drained. Does nothing while socket input remains.
    void finishInput();
    quint16 boundPort() const;
    QString peerDescription() const;
    // Outgoing Qt socket queue only (not input or peer acknowledgement).
    qint64 pendingBytes() const;
    QByteArray takeInput(qint64 maximumBytes);
    // Over-cap input is rejected before write(), with no output side effects.
    bool sendBytes(const QByteArray &bytes);

signals:
    void clientConnected();
    void clientDisconnected();
    void readyRead();
    void bytesWritten(qint64 bytes);
    void failure(const QString &reason);

private:
    void acceptConnections();
    bool isCurrent(QTcpSocket *socket, quint64 generation) const;
    void retireClient(bool notify);
    void endInput(QTcpSocket *socket, quint64 generation);
    QTcpServer *m_server = nullptr;
    QPointer<QTcpSocket> m_client;
    quint64 m_generation = 0;
    bool m_stopping = false;
    bool m_inputEnded = false;
};

#endif
