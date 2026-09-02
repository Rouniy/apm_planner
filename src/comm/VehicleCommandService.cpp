#include "VehicleCommandService.h"

#include "ExactLinkTransmitter.h"
#include "VehicleTargetManager.h"

#include <limits>

VehicleCommandService::VehicleCommandService(
    VehicleTargetManager *targetManager, ExactLinkTransmitter *transmitter,
    QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_transmitter(transmitter)
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_transmitter);
    connect(m_targetManager, &VehicleTargetManager::targetGenerationChanged,
            this, [this](qulonglong generation) {
        if (generation == m_targetManager->targetGeneration()) {
            clearPendingCommands();
        }
    });
}

void VehicleCommandService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

int VehicleCommandService::sendCurrentCommandLong(
    int command, int confirmation,
    float param1, float param2, float param3, float param4,
    float param5, float param6, float param7)
{
    if (command < 0 || command > std::numeric_limits<quint16>::max()
        || confirmation < 0
        || confirmation > std::numeric_limits<quint8>::max()) {
        return static_cast<int>(SendResult::InvalidTarget);
    }
    return static_cast<int>(sendCommandLong(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId,
        static_cast<MAV_CMD>(command), static_cast<quint8>(confirmation),
        param1, param2, param3, param4, param5, param6, param7));
}

VehicleCommandService::SendResult VehicleCommandService::sendCommandLong(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    MAV_CMD command, quint8 confirmation,
    float param1, float param2, float param3, float param4,
    float param5, float param6, float param7)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }

    mavlink_command_long_t payload{};
    payload.target_system = static_cast<quint8>(target.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(target.endpoint.componentId);
    payload.command = static_cast<quint16>(command);
    payload.confirmation = confirmation;
    payload.param1 = param1;
    payload.param2 = param2;
    payload.param3 = param3;
    payload.param4 = param4;
    payload.param5 = param5;
    payload.param6 = param6;
    payload.param7 = param7;

    mavlink_message_t message{};
    // Generated encoders populate the dialect-correct payload.  Re-finalizing
    // below supplies the independent sequence/version state of this link.
    mavlink_msg_command_long_encode(localSystemId, localComponentId,
                                    &message, &payload);
    return finalizeAndWrite(
        target, localSystemId, localComponentId, payload.command, message);
}

VehicleCommandService::SendResult VehicleCommandService::sendCommandInt(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    MAV_CMD command, MAV_FRAME frame,
    float param1, float param2, float param3, float param4,
    qint32 x, qint32 y, float z, quint8 current, quint8 autocontinue)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }

    mavlink_command_int_t payload{};
    payload.target_system = static_cast<quint8>(target.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(target.endpoint.componentId);
    payload.command = static_cast<quint16>(command);
    payload.frame = static_cast<quint8>(frame);
    payload.current = current;
    payload.autocontinue = autocontinue;
    payload.param1 = param1;
    payload.param2 = param2;
    payload.param3 = param3;
    payload.param4 = param4;
    payload.x = x;
    payload.y = y;
    payload.z = z;

    mavlink_message_t message{};
    mavlink_msg_command_int_encode(localSystemId, localComponentId,
                                   &message, &payload);
    return finalizeAndWrite(
        target, localSystemId, localComponentId, payload.command, message);
}

void VehicleCommandService::forgetLink(int linkId)
{
    for (auto generation = m_pendingCommands.begin();
         generation != m_pendingCommands.end();) {
        QHash<quint16, SenderIdentity> &commands = generation.value();
        for (auto command = commands.begin(); command != commands.end();) {
            if (command.value().endpoint.linkId == linkId) {
                command = commands.erase(command);
            } else {
                ++command;
            }
        }
        if (commands.isEmpty()) {
            generation = m_pendingCommands.erase(generation);
        } else {
            ++generation;
        }
    }
}

void VehicleCommandService::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (linkId < 0 || message.msgid != MAVLINK_MSG_ID_COMMAND_ACK) {
        return;
    }

    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid() || target.endpoint.linkId != linkId
        || target.endpoint.systemId != message.sysid
        || target.endpoint.componentId != message.compid
        || !targetIsCurrent(target)) {
        return;
    }

    mavlink_command_ack_t acknowledgement{};
    mavlink_msg_command_ack_decode(&message, &acknowledgement);
    auto generation = m_pendingCommands.find(target.generation);
    if (generation == m_pendingCommands.end()) {
        return;
    }
    auto pending = generation->find(acknowledgement.command);
    if (pending == generation->end()) {
        return;
    }

    const SenderIdentity sender = pending.value();
    if (!sender.endpoint.sameIdentity(target.endpoint)
        || (acknowledgement.target_system != 0
            && acknowledgement.target_system != sender.systemId)
        || (acknowledgement.target_component != 0
            && acknowledgement.target_component != sender.componentId)) {
        return;
    }

    if (acknowledgement.result != MAV_RESULT_IN_PROGRESS) {
        generation->erase(pending);
        if (generation->isEmpty()) {
            m_pendingCommands.erase(generation);
        }
    }

    emit commandAckReceived(
        target.generation,
        linkId, message.sysid, message.compid,
        acknowledgement.command, acknowledgement.result,
        acknowledgement.progress, acknowledgement.result_param2,
        acknowledgement.target_system, acknowledgement.target_component);
}

bool VehicleCommandService::targetIsCurrent(
    const VehicleTargetLease &target) const
{
    return m_targetManager && target.isValid()
        && m_targetManager->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation);
}

VehicleCommandService::SendResult VehicleCommandService::finalizeAndWrite(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    quint16 command, mavlink_message_t message)
{
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (!m_transmitter) {
        return SendResult::TransportUnavailable;
    }

    QHash<quint16, SenderIdentity> &generationCommands =
        m_pendingCommands[target.generation];
    const bool hadPrevious = generationCommands.contains(command);
    const SenderIdentity previous = generationCommands.value(command);
    generationCommands.insert(
        command,
        SenderIdentity{target.endpoint, localSystemId, localComponentId});

    const ExactLinkTransmitter::SendResult result =
        m_transmitter->sendMessage(
            target.endpoint.linkId, localSystemId, localComponentId,
            message);
    if (result != ExactLinkTransmitter::SendResult::Sent) {
        auto generation = m_pendingCommands.find(target.generation);
        if (generation != m_pendingCommands.end()) {
            if (hadPrevious) {
                generation->insert(command, previous);
            } else {
                generation->remove(command);
            }
            if (generation->isEmpty()) {
                m_pendingCommands.erase(generation);
            }
        }
        return SendResult::TransportUnavailable;
    }
    return SendResult::Sent;
}

void VehicleCommandService::clearPendingCommands()
{
    m_pendingCommands.clear();
}
