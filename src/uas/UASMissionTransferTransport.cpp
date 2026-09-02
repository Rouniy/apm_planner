#include "UASMissionTransferTransport.h"

#include "LinkInterface.h"
#include "UAS.h"

UASMissionTransferTransport::UASMissionTransferTransport(UAS *uas)
    : MissionTransferTransport(nullptr)
    , m_uas(uas)
{
    Q_ASSERT(m_uas);
}

UASMissionTransferTransport::~UASMissionTransferTransport()
{
    endOperation();
}

quint8 UASMissionTransferTransport::localSystemId() const
{
    return m_operationActive ? m_pinnedLocalSystemId
                             : (m_uas ? m_uas->gcsSystemId() : 255);
}

quint8 UASMissionTransferTransport::localComponentId() const
{
    return m_operationActive ? m_pinnedLocalComponentId
                             : (m_uas ? m_uas->gcsComponentId()
                                      : MAV_COMP_ID_PRIMARY);
}

MissionTransferService::Key UASMissionTransferTransport::missionKey(
        MAV_MISSION_TYPE missionType) const
{
    MissionTransferService::Key key;
    if (m_operationActive) {
        key.systemId = m_pinnedSystemId;
        key.componentId = m_pinnedComponentId;
    } else if (m_uas) {
        key.systemId = static_cast<quint8>(m_uas->getUASID());
        key.componentId = m_uas->primaryComponentId();
    }
    key.missionType = missionType;
    return key;
}

bool UASMissionTransferTransport::beginOperation()
{
    if (m_operationActive)
        return false;

    LinkInterface *link = selectLink();
    if (!link || !link->isConnected())
        return false;

    ++m_linkGeneration;
    if (m_linkGeneration == 0)
        ++m_linkGeneration;
    const quint64 generation = m_linkGeneration;

    m_pinnedLink = link;
    m_pinnedSystemId = static_cast<quint8>(m_uas->getUASID());
    m_pinnedComponentId = m_uas->primaryComponentId();
    m_pinnedLocalSystemId = m_uas->gcsSystemId();
    m_pinnedLocalComponentId = m_uas->gcsComponentId();
    m_messageConnection = connect(
            m_uas, &UASInterface::mavlinkMessageRecieved,
            this,
            [this, generation, link](LinkInterface *incomingLink,
                                     mavlink_message_t message) {
        if (!m_operationActive || generation != m_linkGeneration
            || m_pinnedLink.data() != link || incomingLink != link
            || message.sysid != m_pinnedSystemId
            || message.compid != m_pinnedComponentId) {
            return;
        }
        emit messageReceived(message);
    });
    m_disconnectedConnection = connect(
            link,
            static_cast<void (LinkInterface::*)()>(
                    &LinkInterface::disconnected),
            this,
            [this, generation]() {
        handlePinnedLinkUnavailable(generation);
    });
    m_destroyedConnection = connect(
            link, &QObject::destroyed, this,
            [this, generation](QObject *) {
        handlePinnedLinkUnavailable(generation);
    });

    // A link can change state on its worker thread while the UI thread installs
    // these connections.  A failed begin must leave no partially pinned state.
    if (!link->isConnected()) {
        clearPinnedLink();
        return false;
    }
    m_operationActive = true;
    return true;
}

void UASMissionTransferTransport::endOperation()
{
    clearPinnedLink();
}

bool UASMissionTransferTransport::sendMessage(
        const mavlink_message_t &message)
{
    LinkInterface *link = m_pinnedLink.data();
    if (!m_operationActive || !m_uas || !link || !link->isConnected())
        return false;

    m_uas->sendMessage(link, message);
    return m_operationActive && m_pinnedLink.data() == link
            && link->isConnected();
}

LinkInterface *UASMissionTransferTransport::pinnedLink() const
{
    return m_pinnedLink.data();
}

void UASMissionTransferTransport::observeMessage(
        LinkInterface *link, mavlink_message_t message)
{
    if (!m_uas || !link
        || message.sysid != m_uas->getUASID()
        || message.compid != m_uas->primaryComponentId()) {
        return;
    }

    if (link->isConnected())
        m_lastPrimaryLink = link;

}

LinkInterface *UASMissionTransferTransport::selectLink() const
{
    LinkInterface *last = m_lastPrimaryLink.data();
    if (last && last->isConnected())
        return last;

    if (!m_uas || !m_uas->getLinks())
        return nullptr;

    // UAS::receiveMessage associates links only after checking the vehicle
    // sysid, so this fallback cannot select a link belonging solely to a
    // different vehicle.
    for (LinkInterface *link : *m_uas->getLinks()) {
        if (link && link->isConnected())
            return link;
    }
    return nullptr;
}

void UASMissionTransferTransport::handlePinnedLinkUnavailable(
        quint64 generation)
{
    if (!m_operationActive || generation != m_linkGeneration)
        return;

    clearPinnedLink();
    emit unavailable();
}

void UASMissionTransferTransport::clearPinnedLink()
{
    // Invalidate first: queued calls from disconnected signal connections can
    // still be delivered after QObject::disconnect().
    m_operationActive = false;
    ++m_linkGeneration;
    if (m_linkGeneration == 0)
        ++m_linkGeneration;

    if (m_messageConnection)
        disconnect(m_messageConnection);
    if (m_disconnectedConnection)
        disconnect(m_disconnectedConnection);
    if (m_destroyedConnection)
        disconnect(m_destroyedConnection);
    m_messageConnection = {};
    m_disconnectedConnection = {};
    m_destroyedConnection = {};
    m_pinnedLink = nullptr;
    m_pinnedSystemId = 0;
    m_pinnedComponentId = 0;
    m_pinnedLocalSystemId = 0;
    m_pinnedLocalComponentId = 0;
}
