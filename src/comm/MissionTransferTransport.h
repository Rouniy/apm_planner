#ifndef MISSIONTRANSFERTRANSPORT_H
#define MISSIONTRANSFERTRANSPORT_H

#include "MissionTransferService.h"

#include <QObject>

/**
 * Vehicle-specific transport boundary for MissionTransferController.
 *
 * Implementations are expected to live on the controller's thread.  An
 * operation pins whatever vehicle/link identity is represented by the
 * transport until endOperation() is called.
 */
class MissionTransferTransport : public QObject
{
    Q_OBJECT

public:
    explicit MissionTransferTransport(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~MissionTransferTransport() override = default;

    virtual quint8 localSystemId() const = 0;
    virtual quint8 localComponentId() const = 0;
    virtual MissionTransferService::Key missionKey(
            MAV_MISSION_TYPE missionType) const = 0;

    /** Pin the active vehicle/link for a single mission operation. */
    virtual bool beginOperation() = 0;
    /** Release resources pinned by beginOperation(). */
    virtual void endOperation() = 0;
    /** Send one already encoded MAVLink message. */
    virtual bool sendMessage(const mavlink_message_t &message) = 0;

signals:
    void messageReceived(const mavlink_message_t &message);
    void unavailable();
};

#endif // MISSIONTRANSFERTRANSPORT_H
