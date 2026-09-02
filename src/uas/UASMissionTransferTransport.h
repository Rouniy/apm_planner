#ifndef UASMISSIONTRANSFERTRANSPORT_H
#define UASMISSIONTRANSFERTRANSPORT_H

#include "comm/MissionTransferTransport.h"

#include <QMetaObject>
#include <QPointer>

class LinkInterface;
class UAS;

/**
 * Link-pinned MissionTransferTransport for one concrete UAS.
 *
 * A mission operation never migrates between links.  The most recently seen
 * link for the vehicle's primary component is selected at beginOperation(); a
 * disconnect or destruction of that link makes the transport unavailable.
 */
class UASMissionTransferTransport final : public MissionTransferTransport
{
    Q_OBJECT

public:
    explicit UASMissionTransferTransport(UAS *uas);
    ~UASMissionTransferTransport() override;

    quint8 localSystemId() const override;
    quint8 localComponentId() const override;
    MissionTransferService::Key missionKey(
            MAV_MISSION_TYPE missionType) const override;

    bool beginOperation() override;
    void endOperation() override;
    bool sendMessage(const mavlink_message_t &message) override;

    LinkInterface *pinnedLink() const;

public slots:
    void observeMessage(LinkInterface *link, mavlink_message_t message);

private:
    LinkInterface *selectLink() const;
    void handlePinnedLinkUnavailable(quint64 generation);
    void clearPinnedLink();

    UAS *const m_uas;
    QPointer<LinkInterface> m_lastPrimaryLink;
    QPointer<LinkInterface> m_pinnedLink;
    QMetaObject::Connection m_messageConnection;
    QMetaObject::Connection m_disconnectedConnection;
    QMetaObject::Connection m_destroyedConnection;
    quint64 m_linkGeneration = 0;
    quint8 m_pinnedSystemId = 0;
    quint8 m_pinnedComponentId = 0;
    quint8 m_pinnedLocalSystemId = 0;
    quint8 m_pinnedLocalComponentId = 0;
    bool m_operationActive = false;

    Q_DISABLE_COPY(UASMissionTransferTransport)
};

#endif // UASMISSIONTRANSFERTRANSPORT_H
