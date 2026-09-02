#ifndef DRONECANFORWARDINGBROKER_H
#define DRONECANFORWARDINGBROKER_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class QTimer;

class DroneCanForwardingBroker final : public QObject
{
    Q_OBJECT

public:
    struct Endpoint
    {
        int uasId = -1;
        int gcsSystemId = -1;
        int gcsComponentId = -1;
        int autopilotComponentId = 1;
        int linkId = -1;

        bool isValid() const;
        bool operator==(const Endpoint &other) const;
        bool operator!=(const Endpoint &other) const
        {
            return !(*this == other);
        }
    };

    struct LeaseToken
    {
        QPointer<QObject> owner;
        quint64 sessionGeneration = 0;
        quint64 leaseGeneration = 0;
        int busIndex = -1;

        bool isValid() const;
    };

    static constexpr int CanForwardCommand = 32000;
    static constexpr int KeepalivePeriodMs = 1000;
    static constexpr int StartupTimeoutMs = 3000;
    // COMMAND_ACK carries neither CAN bus nor request sequence.  This bounded
    // quarantine rejects ordinary late stop/keepalive ACKs; it cannot provide
    // wire-level correlation for an ACK delayed beyond the drain period.
    static constexpr int AckDrainPeriodMs = 1000;

    explicit DroneCanForwardingBroker(QObject *parent = nullptr);

    quint64 bindSession(const Endpoint &endpoint, qint64 nowMs);
    void transportLost(quint64 sessionGeneration);
    void shutdown();

    LeaseToken acquire(QObject *owner, int busIndex, qint64 nowMs);
    bool owns(const LeaseToken &token) const;
    bool confirmed(const LeaseToken &token) const;
    bool release(const LeaseToken &token, qint64 nowMs);
    bool transmitFrame(const LeaseToken &token, quint32 canId,
                       const QByteArray &data, bool canFd);

    void tick(qint64 nowMs);
    void handleAck(quint64 sessionGeneration, int uasId,
                   int sourceComponent, int command, int result,
                   int targetSystem, int targetComponent);
    void handleFrame(quint64 sessionGeneration, int uasId,
                     int sourceComponent, int targetSystem,
                     int targetComponent, int busIndex, quint32 canId,
                     const QByteArray &data, bool canFd, qint64 nowMs);

    bool isBound() const { return m_bound; }
    bool isForwarding() const { return m_activeBusIndex >= 0; }
    bool isConfirmed() const { return m_confirmed; }
    int activeBusIndex() const { return m_activeBusIndex; }
    int leaseCount() const { return m_leases.size(); }
    quint64 sessionGeneration() const { return m_sessionGeneration; }
    Endpoint endpoint() const { return m_endpoint; }
    QString lastError() const { return m_lastError; }

signals:
    // mavlinkBus is one-based for enable (1=CAN1, 2=CAN2), zero for stop.
    void forwardingCommandRequested(quint64 sessionGeneration,
                                    int targetComponent,
                                    int mavlinkBus);
    void frameAccepted(quint64 sessionGeneration, int busIndex,
                       quint32 canId, QByteArray data,
                       bool canFd, qint64 nowMs);
    void frameTransmitRequested(quint64 sessionGeneration,
                                int targetComponent, int busIndex,
                                quint32 canId, QByteArray data, bool canFd);
    void sessionFailed(quint64 sessionGeneration, QString reason);
    void sessionInvalidated(quint64 sessionGeneration);
    void stateChanged();

private:
    struct LeaseRecord
    {
        quint64 generation = 0;
        QMetaObject::Connection destroyedConnection;
    };

    void assertBrokerThread() const;
    bool ackEnvelopeMatches(quint64 sessionGeneration, int uasId,
                            int sourceComponent, int command,
                            int targetSystem, int targetComponent) const;
    bool frameEnvelopeMatches(quint64 sessionGeneration, int uasId,
                              int sourceComponent, int targetSystem,
                              int targetComponent, int busIndex,
                              quint32 canId, int payloadSize,
                              bool canFd) const;
    static bool validDroneCanId(quint32 canId);
    static bool validFramePayload(int payloadSize, bool canFd);
    void releaseOwner(QObject *owner, quint64 leaseGeneration);
    void clearLeases();
    void requestForwardingStart(qint64 nowMs);
    void stopForwarding(bool requestStop, qint64 nowMs = -1);
    void invalidateSession(bool requestStop, const QString &reason,
                           qint64 nowMs = -1);
    quint64 nextGeneration(quint64 current) const;

    Endpoint m_endpoint;
    QTimer *m_keepaliveTimer = nullptr;
    QHash<QObject *, LeaseRecord> m_leases;
    QString m_lastError;
    quint64 m_sessionGeneration = 0;
    quint64 m_leaseGeneration = 0;
    qint64 m_startedAtMs = 0;
    qint64 m_nextKeepaliveMs = 0;
    qint64 m_ackDrainUntilMs = 0;
    int m_activeBusIndex = -1;
    bool m_bound = false;
    bool m_confirmed = false;
    bool m_startCommandSent = false;
    bool m_shutdown = false;
};

Q_DECLARE_METATYPE(DroneCanForwardingBroker::Endpoint)
Q_DECLARE_METATYPE(DroneCanForwardingBroker::LeaseToken)

#endif // DRONECANFORWARDINGBROKER_H
