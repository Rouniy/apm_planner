#include "DroneCanMavlinkTransport.h"

#include "LinkInterface.h"
#include "UASInterface.h"

#include <QDateTime>

#include <algorithm>

namespace {
constexpr quint32 kExtendedFrameFlag = 0x80000000U;
constexpr quint32 kUnsupportedFrameFlags = 0x60000000U;

bool validMavlinkId(int value)
{
    return value > 0 && value <= 255;
}
}

DroneCanMavlinkTransport::DroneCanMavlinkTransport(
    DroneCanForwardingBroker *broker, QObject *parent)
    : QObject(parent),
      m_broker(broker)
{
    if (!m_broker) {
        return;
    }
    connect(m_broker,
            &DroneCanForwardingBroker::forwardingCommandRequested,
            this, &DroneCanMavlinkTransport::sendForwardingCommand);
    connect(m_broker, &DroneCanForwardingBroker::frameTransmitRequested,
            this, &DroneCanMavlinkTransport::sendFrame);
    connect(m_broker, &DroneCanForwardingBroker::sessionInvalidated,
            this, [this](quint64 generation) {
        unbindEndpoint(false, generation);
    });
}

DroneCanMavlinkTransport::~DroneCanMavlinkTransport()
{
    unbindEndpoint(true);
}

UASInterface *DroneCanMavlinkTransport::pinnedUas() const
{
    return m_pinnedUas.data();
}

LinkInterface *DroneCanMavlinkTransport::pinnedLink() const
{
    return m_pinnedLink.data();
}

quint64 DroneCanMavlinkTransport::bindEndpoint(
    UASInterface *uas, LinkInterface *link, qint64 nowMs)
{
    if (!m_broker || !uas || !link || !link->isConnected()
        || !validMavlinkId(uas->getUASID())
        || !validMavlinkId(uas->getSystemId())
        || !validMavlinkId(uas->getComponentId())
        || link->getId() <= 0) {
        return 0;
    }

    const DroneCanForwardingBroker::Endpoint endpoint{
        uas->getUASID(), uas->getSystemId(), uas->getComponentId(),
        MAV_COMP_ID_AUTOPILOT1, link->getId()
    };
    if (m_generation != 0 && m_pinnedUas == uas
        && m_pinnedLink == link && link->isConnected()
        && m_broker->isBound()
        && m_broker->sessionGeneration() == m_generation
        && m_broker->endpoint() == endpoint) {
        return m_generation;
    }

    if (m_broker->isForwarding() || m_broker->leaseCount() != 0) {
        return 0;
    }

    // Ask the broker first so a rejected migration leaves the old endpoint
    // and its signal connections usable.
    const quint64 newGeneration = m_broker->bindSession(endpoint, nowMs);
    if (newGeneration == 0) {
        return 0;
    }
    if (!m_broker->isBound()
        || m_broker->sessionGeneration() != newGeneration
        || m_broker->endpoint() != endpoint) {
        unbindEndpoint(false);
        return 0;
    }
    if (m_broker->isForwarding() || m_broker->leaseCount() != 0) {
        // A reentrant consumer started a new session before the physical
        // endpoint was installed, so its first enable command was missed.
        m_broker->transportLost(newGeneration);
        unbindEndpoint(false);
        return 0;
    }

    disconnectEndpointSignals();
    m_pinnedUas = uas;
    m_pinnedLink = link;
    m_generation = newGeneration;
    m_filterResetGeneration = newGeneration;
    m_filterResetBusMask = 0;

    QPointer<UASInterface> guardedUas(uas);
    QPointer<LinkInterface> guardedLink(link);
    m_messageConnection = connect(
        uas, &UASInterface::mavlinkMessageRecieved, this,
        [this, newGeneration, guardedUas, guardedLink](
            LinkInterface *incomingLink, mavlink_message_t message) {
            if (newGeneration != m_generation
                || m_pinnedUas.data() != guardedUas.data()
                || m_pinnedLink.data() != guardedLink.data()) {
                return;
            }
            receiveMessage(incomingLink, message);
        });
    m_linkDisconnectedConnection = connect(
        link,
        static_cast<void (LinkInterface::*)()>(
            &LinkInterface::disconnected),
        this, [this, newGeneration]() {
            unbindEndpoint(true, newGeneration);
        });
    m_linkDestroyedConnection = connect(
        link, &QObject::destroyed, this,
        [this, newGeneration](QObject *) {
            unbindEndpoint(true, newGeneration);
        });
    m_uasDestroyedConnection = connect(
        uas, &QObject::destroyed, this,
        [this, newGeneration](QObject *) {
            unbindEndpoint(true, newGeneration);
        });

    if (!link->isConnected()) {
        unbindEndpoint(true, newGeneration);
        return 0;
    }
    return newGeneration;
}

void DroneCanMavlinkTransport::unbindEndpoint(
    bool reportLoss, quint64 expectedGeneration)
{
    if (expectedGeneration != 0 && expectedGeneration != m_generation) {
        return;
    }

    const quint64 oldGeneration = m_generation;
    m_generation = 0;
    disconnectEndpointSignals();
    m_pinnedUas.clear();
    m_pinnedLink.clear();
    m_filterResetGeneration = 0;
    m_filterResetBusMask = 0;

    if (reportLoss && oldGeneration != 0 && m_broker) {
        m_broker->transportLost(oldGeneration);
    }
}

bool DroneCanMavlinkTransport::validCanId(quint32 canId)
{
    return (canId & kExtendedFrameFlag) != 0
        && (canId & kUnsupportedFrameFlags) == 0;
}

bool DroneCanMavlinkTransport::validPayloadSize(int size, bool canFd)
{
    if (size < 1) {
        return false;
    }
    if (!canFd) {
        return size <= 8;
    }
    return size <= 8 || size == 12 || size == 16 || size == 20
        || size == 24 || size == 32 || size == 48 || size == 64;
}

bool DroneCanMavlinkTransport::currentEndpoint(quint64 generation) const
{
    UASInterface *const uas = m_pinnedUas.data();
    LinkInterface *const link = m_pinnedLink.data();
    if (generation == 0 || generation != m_generation || !m_broker
        || !m_broker->isBound()
        || m_broker->sessionGeneration() != generation
        || !uas || !link || !link->isConnected()) {
        return false;
    }
    const DroneCanForwardingBroker::Endpoint endpoint =
        m_broker->endpoint();
    return endpoint.uasId == uas->getUASID()
        && endpoint.gcsSystemId == uas->getSystemId()
        && endpoint.gcsComponentId == uas->getComponentId()
        && endpoint.autopilotComponentId == MAV_COMP_ID_AUTOPILOT1
        && endpoint.linkId == link->getId();
}

void DroneCanMavlinkTransport::disconnectEndpointSignals()
{
    disconnect(m_messageConnection);
    disconnect(m_linkDisconnectedConnection);
    disconnect(m_linkDestroyedConnection);
    disconnect(m_uasDestroyedConnection);
    m_messageConnection = {};
    m_linkDisconnectedConnection = {};
    m_linkDestroyedConnection = {};
    m_uasDestroyedConnection = {};
}

void DroneCanMavlinkTransport::invalidateGeneration(quint64 generation)
{
    if (generation == m_generation) {
        unbindEndpoint(true, generation);
    }
}

void DroneCanMavlinkTransport::receiveMessage(
    LinkInterface *incomingLink, const mavlink_message_t &message)
{
    const quint64 activeGeneration = m_generation;
    if (!currentEndpoint(activeGeneration)
        || incomingLink != m_pinnedLink.data()) {
        return;
    }

    if (message.msgid == MAVLINK_MSG_ID_COMMAND_ACK) {
        mavlink_command_ack_t ack{};
        mavlink_msg_command_ack_decode(&message, &ack);
        m_broker->handleAck(
            activeGeneration, message.sysid, message.compid,
            ack.command, ack.result, ack.target_system,
            ack.target_component);
        return;
    }

    if (message.msgid == MAVLINK_MSG_ID_CAN_FRAME) {
        mavlink_can_frame_t frame{};
        mavlink_msg_can_frame_decode(&message, &frame);
        if (frame.bus > 1 || !validCanId(frame.id)
            || !validPayloadSize(int(frame.len), false)) {
            return;
        }
        m_broker->handleFrame(
            activeGeneration, message.sysid, message.compid,
            frame.target_system, frame.target_component, frame.bus,
            frame.id,
            QByteArray(reinterpret_cast<const char *>(frame.data),
                       int(frame.len)),
            false, QDateTime::currentMSecsSinceEpoch());
        return;
    }

    if (message.msgid == MAVLINK_MSG_ID_CANFD_FRAME) {
        mavlink_canfd_frame_t frame{};
        mavlink_msg_canfd_frame_decode(&message, &frame);
        if (frame.bus > 1 || !validCanId(frame.id)
            || !validPayloadSize(int(frame.len), true)) {
            return;
        }
        m_broker->handleFrame(
            activeGeneration, message.sysid, message.compid,
            frame.target_system, frame.target_component, frame.bus,
            frame.id,
            QByteArray(reinterpret_cast<const char *>(frame.data),
                       int(frame.len)),
            true, QDateTime::currentMSecsSinceEpoch());
    }
}

void DroneCanMavlinkTransport::sendForwardingCommand(
    quint64 sessionGeneration, int targetComponent, int mavlinkBus)
{
    if (mavlinkBus < 0 || mavlinkBus > 2) {
        return;
    }
    if (!currentEndpoint(sessionGeneration)) {
        if (mavlinkBus != 0) {
            invalidateGeneration(sessionGeneration);
        }
        return;
    }

    UASInterface *const uas = m_pinnedUas.data();
    LinkInterface *const link = m_pinnedLink.data();
    const DroneCanForwardingBroker::Endpoint endpoint =
        m_broker->endpoint();
    if (targetComponent != endpoint.autopilotComponentId) {
        return;
    }

    if (mavlinkBus != 0) {
        if (m_filterResetGeneration != sessionGeneration) {
            m_filterResetGeneration = sessionGeneration;
            m_filterResetBusMask = 0;
        }
        const quint8 busBit = quint8(1U << (mavlinkBus - 1));
        if ((m_filterResetBusMask & busBit) == 0) {
            mavlink_message_t filterMessage{};
            const uint16_t emptyIds[16]{};
            mavlink_msg_can_filter_modify_pack(
                static_cast<uint8_t>(endpoint.gcsSystemId),
                static_cast<uint8_t>(endpoint.gcsComponentId),
                &filterMessage,
                static_cast<uint8_t>(endpoint.uasId),
                static_cast<uint8_t>(targetComponent),
                static_cast<uint8_t>(mavlinkBus),
                CAN_FILTER_REPLACE, 0, emptyIds);
            const bool sent = uas->sendMessageOnLink(link, filterMessage);
            if (!sent) {
                invalidateGeneration(sessionGeneration);
                return;
            }
            if (!currentEndpoint(sessionGeneration)
                || uas != m_pinnedUas.data()
                || link != m_pinnedLink.data()) {
                return;
            }
            m_filterResetBusMask |= busBit;
        }
    }

    const bool sent = uas->executeCommandOnLink(
        link,
        static_cast<MAV_CMD>(
            DroneCanForwardingBroker::CanForwardCommand),
        0, float(mavlinkBus), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, targetComponent);
    if (!sent && mavlinkBus != 0) {
        invalidateGeneration(sessionGeneration);
    }
}

void DroneCanMavlinkTransport::sendFrame(
    quint64 sessionGeneration, int targetComponent, int busIndex,
    quint32 canId, const QByteArray &data, bool canFd)
{
    if (!currentEndpoint(sessionGeneration)) {
        invalidateGeneration(sessionGeneration);
        return;
    }

    const DroneCanForwardingBroker::Endpoint endpoint =
        m_broker->endpoint();
    if (targetComponent != endpoint.autopilotComponentId
        || busIndex < 0 || busIndex > 1 || !validCanId(canId)
        || !validPayloadSize(data.size(), canFd)) {
        return;
    }

    UASInterface *const uas = m_pinnedUas.data();
    LinkInterface *const link = m_pinnedLink.data();
    mavlink_message_t message{};
    if (canFd) {
        uint8_t payload[64]{};
        std::copy(data.constBegin(), data.constEnd(), payload);
        mavlink_msg_canfd_frame_pack(
            static_cast<uint8_t>(endpoint.gcsSystemId),
            static_cast<uint8_t>(endpoint.gcsComponentId), &message,
            static_cast<uint8_t>(endpoint.uasId),
            static_cast<uint8_t>(targetComponent),
            static_cast<uint8_t>(busIndex),
            static_cast<uint8_t>(data.size()), canId, payload);
    } else {
        uint8_t payload[8]{};
        std::copy(data.constBegin(), data.constEnd(), payload);
        mavlink_msg_can_frame_pack(
            static_cast<uint8_t>(endpoint.gcsSystemId),
            static_cast<uint8_t>(endpoint.gcsComponentId), &message,
            static_cast<uint8_t>(endpoint.uasId),
            static_cast<uint8_t>(targetComponent),
            static_cast<uint8_t>(busIndex),
            static_cast<uint8_t>(data.size()), canId, payload);
    }

    if (!uas->sendMessageOnLink(link, message)) {
        invalidateGeneration(sessionGeneration);
    }
}
