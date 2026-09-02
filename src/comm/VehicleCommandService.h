#ifndef VEHICLECOMMANDSERVICE_H
#define VEHICLECOMMANDSERVICE_H

#include "VehicleEndpoint.h"

#include <QHash>
#include <QObject>

#include <mavlink.h>

class VehicleTargetManager;
class ExactLinkTransmitter;

/**
 * Exact-link MAVLink COMMAND_LONG/COMMAND_INT transport.
 *
 * Every send is authorized by a VehicleTargetLease.  A stale lease or an
 * unavailable selected link fails closed; this service never fans a command
 * out over the links collected by the legacy sysid-only UAS object.
 */
class VehicleCommandService final : public QObject
{
    Q_OBJECT

public:
    enum class SendResult {
        Sent,
        InvalidTarget,
        StaleTarget,
        TransportUnavailable
    };
    Q_ENUM(SendResult)

    explicit VehicleCommandService(VehicleTargetManager *targetManager,
                                   ExactLinkTransmitter *transmitter,
                                   QObject *parent = nullptr);

    void setLocalIdentity(quint8 systemId, quint8 componentId);
    Q_INVOKABLE int sendCurrentCommandLong(
        int command, int confirmation,
        float param1, float param2, float param3, float param4,
        float param5, float param6, float param7);

    SendResult sendCommandLong(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        MAV_CMD command, quint8 confirmation,
        float param1, float param2, float param3, float param4,
        float param5, float param6, float param7);

    SendResult sendCommandInt(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        MAV_CMD command, MAV_FRAME frame,
        float param1, float param2, float param3, float param4,
        qint32 x, qint32 y, float z,
        quint8 current = 0, quint8 autocontinue = 0);

    void forgetLink(int linkId);
    void observeMessage(int linkId, const mavlink_message_t &message);

signals:
    void commandAckReceived(
        qulonglong targetGeneration,
        int linkId, int systemId, int componentId,
        int command, int result, int progress, int resultParam2,
        int targetSystem, int targetComponent);

private:
    struct SenderIdentity
    {
        VehicleEndpoint endpoint;
        quint8 systemId = 0;
        quint8 componentId = 0;
    };

    bool targetIsCurrent(const VehicleTargetLease &target) const;
    SendResult finalizeAndWrite(
        const VehicleTargetLease &target,
        quint8 localSystemId, quint8 localComponentId,
        quint16 command, mavlink_message_t message);
    void clearPendingCommands();

    VehicleTargetManager *const m_targetManager;
    ExactLinkTransmitter *const m_transmitter;
    QHash<quint64, QHash<quint16, SenderIdentity>> m_pendingCommands;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
};

#endif // VEHICLECOMMANDSERVICE_H
