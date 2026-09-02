#ifndef DRONECANMAVLINKTRANSPORT_H
#define DRONECANMAVLINKTRANSPORT_H

#include "DroneCanForwardingBroker.h"
#include "QGCMAVLink.h"

#include <QMetaObject>
#include <QObject>
#include <QPointer>

class LinkInterface;
class UASInterface;

class DroneCanMavlinkTransport final : public QObject
{
    Q_OBJECT

public:
    explicit DroneCanMavlinkTransport(
        DroneCanForwardingBroker *broker, QObject *parent = nullptr);
    ~DroneCanMavlinkTransport() override;

    quint64 bindEndpoint(UASInterface *uas, LinkInterface *link,
                         qint64 nowMs);
    void unbindEndpoint(bool reportLoss = true,
                        quint64 expectedGeneration = 0);

    quint64 generation() const { return m_generation; }
    UASInterface *pinnedUas() const;
    LinkInterface *pinnedLink() const;

private:
    static bool validCanId(quint32 canId);
    static bool validPayloadSize(int size, bool canFd);
    bool currentEndpoint(quint64 generation) const;
    void disconnectEndpointSignals();
    void invalidateGeneration(quint64 generation);
    void receiveMessage(LinkInterface *incomingLink,
                        const mavlink_message_t &message);
    void sendForwardingCommand(quint64 sessionGeneration,
                               int targetComponent, int mavlinkBus);
    void sendFrame(quint64 sessionGeneration, int targetComponent,
                   int busIndex, quint32 canId, const QByteArray &data,
                   bool canFd);

    QPointer<DroneCanForwardingBroker> m_broker;
    QPointer<UASInterface> m_pinnedUas;
    QPointer<LinkInterface> m_pinnedLink;
    QMetaObject::Connection m_messageConnection;
    QMetaObject::Connection m_linkDisconnectedConnection;
    QMetaObject::Connection m_linkDestroyedConnection;
    QMetaObject::Connection m_uasDestroyedConnection;
    quint64 m_generation = 0;
    quint64 m_filterResetGeneration = 0;
    quint8 m_filterResetBusMask = 0;
};

#endif // DRONECANMAVLINKTRANSPORT_H
