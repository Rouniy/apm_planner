#include "MissionTransferService.h"

#include "MissionItemProtocol.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
quint8 intFrameFor(quint8 frame)
{
    switch (frame)
    {
    case MAV_FRAME_GLOBAL:
        return MAV_FRAME_GLOBAL_INT;
    case MAV_FRAME_GLOBAL_RELATIVE_ALT:
        return MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    case MAV_FRAME_GLOBAL_TERRAIN_ALT:
        return MAV_FRAME_GLOBAL_TERRAIN_ALT_INT;
    default:
        return frame;
    }
}

quint8 floatFrameFor(quint8 frame)
{
    switch (frame)
    {
    case MAV_FRAME_GLOBAL_INT:
        return MAV_FRAME_GLOBAL;
    case MAV_FRAME_GLOBAL_RELATIVE_ALT_INT:
        return MAV_FRAME_GLOBAL_RELATIVE_ALT;
    case MAV_FRAME_GLOBAL_TERRAIN_ALT_INT:
        return MAV_FRAME_GLOBAL_TERRAIN_ALT;
    default:
        return frame;
    }
}

qint32 scaledCoordinate(float coordinate, double scale)
{
    const double scaled = std::round(static_cast<double>(coordinate) * scale);
    return static_cast<qint32>(std::clamp(
            scaled,
            static_cast<double>(std::numeric_limits<qint32>::min()),
            static_cast<double>(std::numeric_limits<qint32>::max())));
}
}

bool MissionTransferService::Key::operator==(const Key &other) const
{
    return systemId == other.systemId
            && componentId == other.componentId
            && missionType == other.missionType;
}

MissionTransferService::MissionTransferService(int maxRetries,
                                               quint8 localSystemId,
                                               quint8 localComponentId)
    : m_maxRetries(qMax(0, maxRetries))
    , m_localSystemId(localSystemId)
    , m_localComponentId(localComponentId)
{
}

bool MissionTransferService::isActive() const
{
    return m_state == State::WaitingForCount
            || m_state == State::WaitingForItemInt
            || m_state == State::WaitingForRequestInt
            || m_state == State::WaitingForAck;
}

bool MissionTransferService::setLocalIdentity(
        quint8 systemId, quint8 componentId)
{
    if (isActive())
        return false;
    m_localSystemId = systemId;
    m_localComponentId = componentId;
    return true;
}

void MissionTransferService::reset()
{
    m_retryCount = 0;
    m_state = State::Idle;
    m_direction = Direction::None;
    m_key = Key{};
    m_totalCount = 0;
    m_nextSequence = 0;
    m_result = MAV_MISSION_ACCEPTED;
    m_errorString.clear();
    m_downloadItems.clear();
    m_uploadItems.clear();
    m_lastOutbound.reset();
}

bool MissionTransferService::isSupportedMissionType(MAV_MISSION_TYPE type)
{
    return type == MAV_MISSION_TYPE_MISSION
            || type == MAV_MISSION_TYPE_FENCE
            || type == MAV_MISSION_TYPE_RALLY;
}

bool MissionTransferService::matches(quint8 sourceSystem,
                                     quint8 sourceComponent,
                                     quint8 missionType,
                                     quint8 targetSystem,
                                     quint8 targetComponent) const
{
    return sourceSystem == m_key.systemId
            && sourceComponent == m_key.componentId
            && missionType == static_cast<quint8>(m_key.missionType)
            && targetSystem == m_localSystemId
            && targetComponent == m_localComponentId;
}

MissionTransferService::Transition MissionTransferService::startDownload(
        const Key &key)
{
    if (isActive())
        return {};

    reset();
    m_key = key;
    m_direction = Direction::Download;
    if (!isSupportedMissionType(key.missionType))
        return setError(QStringLiteral("Unsupported mission type"),
                        MAV_MISSION_UNSUPPORTED);

    m_state = State::WaitingForCount;
    return send(makeRequestList());
}

MissionTransferService::Transition MissionTransferService::startUpload(
        const Key &key, const QVector<mavlink_mission_item_int_t> &items)
{
    if (isActive())
        return {};

    reset();
    m_key = key;
    m_direction = Direction::Upload;
    if (!isSupportedMissionType(key.missionType))
        return setError(QStringLiteral("Unsupported mission type"),
                        MAV_MISSION_UNSUPPORTED);
    if (items.size() > std::numeric_limits<quint16>::max())
        return setError(QStringLiteral("Mission contains too many items"),
                        MAV_MISSION_NO_SPACE);

    m_uploadItems = items;
    for (int index = 0; index < m_uploadItems.size(); ++index)
    {
        mavlink_mission_item_int_t &item = m_uploadItems[index];
        item.target_system = key.systemId;
        item.target_component = key.componentId;
        item.seq = static_cast<quint16>(index);
        item.mission_type = static_cast<quint8>(key.missionType);
    }
    m_totalCount = static_cast<quint16>(m_uploadItems.size());
    m_state = m_uploadItems.isEmpty() ? State::WaitingForAck
                                      : State::WaitingForRequestInt;
    return send(makeCount());
}

MissionTransferService::Transition MissionTransferService::handleMissionCount(
        quint8 sourceSystem, quint8 sourceComponent,
        const mavlink_mission_count_t &count)
{
    if (m_direction != Direction::Download
            || m_state != State::WaitingForCount
            || !matches(sourceSystem, sourceComponent, count.mission_type,
                        count.target_system, count.target_component))
        return {};

    m_totalCount = count.count;
    m_nextSequence = 0;
    m_downloadItems.clear();
    m_downloadItems.reserve(count.count);
    if (count.count == 0)
    {
        m_state = State::Complete;
        m_result = MAV_MISSION_ACCEPTED;
        return sendTerminal(makeAck(MAV_MISSION_ACCEPTED));
    }

    m_state = State::WaitingForItemInt;
    return send(makeRequest(0, true));
}

MissionTransferService::Transition
MissionTransferService::handleMissionItemInt(
        quint8 sourceSystem, quint8 sourceComponent,
        const mavlink_mission_item_int_t &item)
{
    if (m_direction != Direction::Download
            || m_state != State::WaitingForItemInt
            || !matches(sourceSystem, sourceComponent, item.mission_type,
                        item.target_system, item.target_component))
        return {};

    return acceptDownloadedItem(item, true);
}

MissionTransferService::Transition MissionTransferService::handleMissionItem(
        quint8 sourceSystem, quint8 sourceComponent,
        const mavlink_mission_item_t &item)
{
    if (m_direction != Direction::Download
            || m_state != State::WaitingForItemInt
            || !matches(sourceSystem, sourceComponent, item.mission_type,
                        item.target_system, item.target_component))
        return {};

    return acceptDownloadedItem(toMissionItemInt(item), false);
}

MissionTransferService::Transition MissionTransferService::acceptDownloadedItem(
        const mavlink_mission_item_int_t &item, bool useIntRequests)
{
    if (m_nextSequence > 0
            && item.seq == static_cast<quint16>(m_nextSequence - 1))
    {
        // The peer may repeat its previous item when our request for the next
        // item was lost. Do not append it twice; ask for the expected item.
        return send(makeRequest(m_nextSequence, useIntRequests));
    }

    if (item.seq != m_nextSequence)
    {
        return setError(
                    QStringLiteral("Expected mission item %1, received %2")
                            .arg(m_nextSequence).arg(item.seq),
                    MAV_MISSION_INVALID_SEQUENCE,
                    makeAck(MAV_MISSION_INVALID_SEQUENCE));
    }

    m_downloadItems.append(item);
    ++m_nextSequence;
    if (m_nextSequence < m_totalCount)
        return send(makeRequest(m_nextSequence, useIntRequests));

    m_state = State::Complete;
    m_result = MAV_MISSION_ACCEPTED;
    return sendTerminal(makeAck(MAV_MISSION_ACCEPTED));
}

MissionTransferService::Transition
MissionTransferService::handleMissionRequestInt(
        quint8 sourceSystem, quint8 sourceComponent,
        const mavlink_mission_request_int_t &request)
{
    const bool uploadState = m_state == State::WaitingForRequestInt
            || m_state == State::WaitingForAck;
    if (m_direction != Direction::Upload || !uploadState
            || !matches(sourceSystem, sourceComponent, request.mission_type,
                        request.target_system, request.target_component))
        return {};

    return acceptUploadRequest(request.seq, true);
}

MissionTransferService::Transition
MissionTransferService::handleMissionRequest(
        quint8 sourceSystem, quint8 sourceComponent,
        const mavlink_mission_request_t &request)
{
    const bool uploadState = m_state == State::WaitingForRequestInt
            || m_state == State::WaitingForAck;
    if (m_direction != Direction::Upload || !uploadState
            || !matches(sourceSystem, sourceComponent, request.mission_type,
                        request.target_system, request.target_component))
        return {};

    return acceptUploadRequest(request.seq, false);
}

MissionTransferService::Transition MissionTransferService::acceptUploadRequest(
        quint16 sequence, bool useIntItem)
{

    if (sequence >= m_totalCount)
    {
        return setError(
                    QStringLiteral("Requested mission item %1 outside count %2")
                            .arg(sequence).arg(m_totalCount),
                    MAV_MISSION_INVALID_SEQUENCE);
    }

    if (sequence == m_nextSequence)
    {
        const quint16 sequence = m_nextSequence;
        ++m_nextSequence;
        m_state = m_nextSequence == m_totalCount
                ? State::WaitingForAck : State::WaitingForRequestInt;
        return send(makeItem(sequence, useIntItem));
    }

    if (m_nextSequence > 0
            && sequence == static_cast<quint16>(m_nextSequence - 1))
    {
        // The peer may repeat its last request when our item was lost.
        return send(makeItem(sequence, useIntItem));
    }

    return setError(
                QStringLiteral("Expected mission request %1, received %2")
                        .arg(m_nextSequence).arg(sequence),
                MAV_MISSION_INVALID_SEQUENCE);
}

MissionTransferService::Transition MissionTransferService::handleMissionAck(
        quint8 sourceSystem, quint8 sourceComponent,
        const mavlink_mission_ack_t &ack)
{
    if (!isActive()
            || !matches(sourceSystem, sourceComponent, ack.mission_type,
                        ack.target_system, ack.target_component))
        return {};

    const MAV_MISSION_RESULT result = ack.type < MAV_MISSION_RESULT_ENUM_END
            ? static_cast<MAV_MISSION_RESULT>(ack.type) : MAV_MISSION_ERROR;
    if (m_direction == Direction::Download)
    {
        if (result == MAV_MISSION_ACCEPTED)
            return {};
        return setError(QStringLiteral("Vehicle rejected mission download (%1)")
                                .arg(static_cast<int>(result)),
                        result);
    }
    if (m_direction != Direction::Upload)
        return {};

    if (result != MAV_MISSION_ACCEPTED)
    {
        return setError(QStringLiteral("Vehicle rejected mission (%1)")
                                .arg(static_cast<int>(result)),
                        result);
    }
    if (m_nextSequence != m_totalCount)
    {
        return setError(QStringLiteral("Mission accepted before all items were requested"),
                        MAV_MISSION_INVALID_SEQUENCE);
    }

    m_state = State::Complete;
    m_result = MAV_MISSION_ACCEPTED;
    m_retryCount = 0;
    m_lastOutbound.reset();
    return {true, std::nullopt};
}

MissionTransferService::Transition MissionTransferService::onTimeout()
{
    if (!isActive() || !m_lastOutbound)
        return {};
    if (m_retryCount >= m_maxRetries)
    {
        return setError(QStringLiteral("Mission transfer timed out"),
                        MAV_MISSION_ERROR);
    }

    ++m_retryCount;
    OutboundMessage retryMessage = *m_lastOutbound;
    retryMessage.retry = true;
    return {true, retryMessage};
}

MissionTransferService::Transition MissionTransferService::cancel(
        const QString &reason)
{
    if (!isActive())
        return {};

    const OutboundMessage ack = makeAck(MAV_MISSION_OPERATION_CANCELLED);
    m_state = State::Cancelled;
    m_result = MAV_MISSION_OPERATION_CANCELLED;
    m_errorString = reason.isEmpty() ? QStringLiteral("Mission transfer cancelled")
                                     : reason;
    return sendTerminal(ack);
}

MissionTransferService::Transition MissionTransferService::fail(
        const QString &reason, MAV_MISSION_RESULT result)
{
    return setError(reason.isEmpty() ? QStringLiteral("Mission transfer failed")
                                     : reason,
                    result);
}

MissionTransferService::Transition MissionTransferService::send(
        const OutboundMessage &message)
{
    OutboundMessage initial = message;
    initial.retry = false;
    m_retryCount = 0;
    m_lastOutbound = initial;
    return {true, initial};
}

MissionTransferService::Transition MissionTransferService::sendTerminal(
        const OutboundMessage &message)
{
    OutboundMessage terminal = message;
    terminal.retry = false;
    m_retryCount = 0;
    m_lastOutbound.reset();
    return {true, terminal};
}

MissionTransferService::Transition MissionTransferService::setError(
        const QString &reason, MAV_MISSION_RESULT result,
        const std::optional<OutboundMessage> &message)
{
    m_state = State::Error;
    m_result = result;
    m_errorString = reason;
    m_retryCount = 0;
    m_lastOutbound.reset();
    if (!message)
        return {true, std::nullopt};

    OutboundMessage terminal = *message;
    terminal.retry = false;
    return {true, terminal};
}

MissionTransferService::OutboundMessage
MissionTransferService::makeRequestList() const
{
    mavlink_mission_request_list_t request{};
    request.target_system = m_key.systemId;
    request.target_component = m_key.componentId;
    request.mission_type = static_cast<quint8>(m_key.missionType);
    return {MessageType::MissionRequestList, request, false};
}

MissionTransferService::OutboundMessage MissionTransferService::makeCount() const
{
    mavlink_mission_count_t count{};
    count.target_system = m_key.systemId;
    count.target_component = m_key.componentId;
    count.count = m_totalCount;
    count.mission_type = static_cast<quint8>(m_key.missionType);
    return {MessageType::MissionCount, count, false};
}

MissionTransferService::OutboundMessage
MissionTransferService::makeRequest(quint16 sequence, bool useInt) const
{
    if (!useInt)
    {
        mavlink_mission_request_t request{};
        request.target_system = m_key.systemId;
        request.target_component = m_key.componentId;
        request.seq = sequence;
        request.mission_type = static_cast<quint8>(m_key.missionType);
        return {MessageType::MissionRequest, request, false};
    }

    mavlink_mission_request_int_t request{};
    request.target_system = m_key.systemId;
    request.target_component = m_key.componentId;
    request.seq = sequence;
    request.mission_type = static_cast<quint8>(m_key.missionType);
    return {MessageType::MissionRequestInt, request, false};
}

MissionTransferService::OutboundMessage
MissionTransferService::makeItem(quint16 sequence, bool useInt) const
{
    if (!useInt)
        return {MessageType::MissionItem,
                toMissionItem(m_uploadItems.at(sequence)), false};
    return {MessageType::MissionItemInt, m_uploadItems.at(sequence), false};
}

MissionTransferService::OutboundMessage MissionTransferService::makeAck(
        MAV_MISSION_RESULT result) const
{
    mavlink_mission_ack_t ack{};
    ack.target_system = m_key.systemId;
    ack.target_component = m_key.componentId;
    ack.type = static_cast<quint8>(result);
    ack.mission_type = static_cast<quint8>(m_key.missionType);
    return {MessageType::MissionAck, ack, false};
}

mavlink_mission_item_int_t MissionTransferService::toMissionItemInt(
        const mavlink_mission_item_t &item)
{
    mavlink_mission_item_int_t converted{};
    converted.param1 = item.param1;
    converted.param2 = item.param2;
    converted.param3 = item.param3;
    converted.param4 = item.param4;
    const double scale = MissionItemProtocol::coordinateScale(
            item.frame, item.command);
    converted.x = scaledCoordinate(item.x, scale);
    converted.y = scaledCoordinate(item.y, scale);
    converted.z = item.z;
    converted.seq = item.seq;
    converted.command = item.command;
    converted.target_system = item.target_system;
    converted.target_component = item.target_component;
    converted.frame = intFrameFor(item.frame);
    converted.current = item.current;
    converted.autocontinue = item.autocontinue;
    converted.mission_type = item.mission_type;
    return converted;
}

mavlink_mission_item_t MissionTransferService::toMissionItem(
        const mavlink_mission_item_int_t &item)
{
    mavlink_mission_item_t converted{};
    converted.param1 = item.param1;
    converted.param2 = item.param2;
    converted.param3 = item.param3;
    converted.param4 = item.param4;
    const double scale = MissionItemProtocol::coordinateScale(
            item.frame, item.command);
    converted.x = static_cast<float>(item.x / scale);
    converted.y = static_cast<float>(item.y / scale);
    converted.z = item.z;
    converted.seq = item.seq;
    converted.command = item.command;
    converted.target_system = item.target_system;
    converted.target_component = item.target_component;
    converted.frame = floatFrameFor(item.frame);
    converted.current = item.current;
    converted.autocontinue = item.autocontinue;
    converted.mission_type = item.mission_type;
    return converted;
}
