#ifndef MISSIONTRANSFERSERVICE_H
#define MISSIONTRANSFERSERVICE_H

#include "QGCMAVLink.h"

#include <QString>
#include <QVector>

#include <optional>
#include <variant>

/**
 * Deterministic MAVLink MISSION_INT transaction state machine.
 *
 * The service neither owns a link nor encodes/sends mavlink_message_t packets.
 * Callers feed decoded payloads into the handle* methods and transport the
 * typed payload returned in Transition::outbound.
 */
class MissionTransferService final
{
public:
    struct Key
    {
        quint8 systemId = 0;
        quint8 componentId = 0;
        MAV_MISSION_TYPE missionType = MAV_MISSION_TYPE_MISSION;

        bool operator==(const Key &other) const;
        bool operator!=(const Key &other) const { return !(*this == other); }
    };

    enum class Direction
    {
        None,
        Download,
        Upload
    };

    enum class State
    {
        Idle,
        WaitingForCount,
        WaitingForItemInt,
        WaitingForRequestInt,
        WaitingForAck,
        Complete,
        Cancelled,
        Error
    };

    enum class MessageType
    {
        MissionRequestList,
        MissionCount,
        MissionRequest,
        MissionRequestInt,
        MissionItem,
        MissionItemInt,
        MissionAck
    };

    using Payload = std::variant<mavlink_mission_request_list_t,
                                 mavlink_mission_count_t,
                                 mavlink_mission_request_t,
                                 mavlink_mission_request_int_t,
                                 mavlink_mission_item_t,
                                 mavlink_mission_item_int_t,
                                 mavlink_mission_ack_t>;

    struct OutboundMessage
    {
        MessageType type = MessageType::MissionRequestList;
        Payload payload = mavlink_mission_request_list_t{};
        bool retry = false;
    };

    struct Transition
    {
        bool handled = false;
        std::optional<OutboundMessage> outbound;
    };

    explicit MissionTransferService(int maxRetries = 3,
                                    quint8 localSystemId = 255,
                                    quint8 localComponentId =
                                            MAV_COMP_ID_PRIMARY);

    Transition startDownload(const Key &key);
    Transition startUpload(const Key &key,
                           const QVector<mavlink_mission_item_int_t> &items);

    Transition handleMissionCount(quint8 sourceSystem,
                                  quint8 sourceComponent,
                                  const mavlink_mission_count_t &count);
    Transition handleMissionRequestInt(
            quint8 sourceSystem, quint8 sourceComponent,
            const mavlink_mission_request_int_t &request);
    Transition handleMissionRequest(
            quint8 sourceSystem, quint8 sourceComponent,
            const mavlink_mission_request_t &request);
    Transition handleMissionItemInt(quint8 sourceSystem,
                                    quint8 sourceComponent,
                                    const mavlink_mission_item_int_t &item);
    Transition handleMissionItem(quint8 sourceSystem,
                                 quint8 sourceComponent,
                                 const mavlink_mission_item_t &item);
    Transition handleMissionAck(quint8 sourceSystem,
                                quint8 sourceComponent,
                                const mavlink_mission_ack_t &ack);

    /** Retry the last outbound protocol step or enter Error when exhausted. */
    Transition onTimeout();
    /** Cancel an active transaction and return MISSION_ACK(CANCELLED). */
    Transition cancel(const QString &reason = QString());
    /** Enter Error due to a transport/decoder failure. */
    Transition fail(const QString &reason,
                    MAV_MISSION_RESULT result = MAV_MISSION_ERROR);
    void reset();

    State state() const { return m_state; }
    Direction direction() const { return m_direction; }
    Key key() const { return m_key; }
    bool isActive() const;
    int maxRetries() const { return m_maxRetries; }
    int retryCount() const { return m_retryCount; }
    quint8 localSystemId() const { return m_localSystemId; }
    quint8 localComponentId() const { return m_localComponentId; }
    quint16 totalCount() const { return m_totalCount; }
    quint16 nextSequence() const { return m_nextSequence; }
    MAV_MISSION_RESULT missionResult() const { return m_result; }
    QString errorString() const { return m_errorString; }
    QVector<mavlink_mission_item_int_t> downloadedItems() const
    {
        return m_downloadItems;
    }

    /** Refresh the local MAVLink source/target identity between operations. */
    bool setLocalIdentity(quint8 systemId, quint8 componentId);

private:
    bool matches(quint8 sourceSystem, quint8 sourceComponent,
                 quint8 missionType, quint8 targetSystem,
                 quint8 targetComponent) const;
    static bool isSupportedMissionType(MAV_MISSION_TYPE type);
    static mavlink_mission_item_int_t toMissionItemInt(
            const mavlink_mission_item_t &item);
    static mavlink_mission_item_t toMissionItem(
            const mavlink_mission_item_int_t &item);

    Transition acceptDownloadedItem(const mavlink_mission_item_int_t &item,
                                    bool useIntRequests);
    Transition acceptUploadRequest(quint16 sequence, bool useIntItem);

    Transition send(const OutboundMessage &message);
    Transition sendTerminal(const OutboundMessage &message);
    Transition setError(const QString &reason, MAV_MISSION_RESULT result,
                        const std::optional<OutboundMessage> &message =
                                std::nullopt);

    OutboundMessage makeRequestList() const;
    OutboundMessage makeCount() const;
    OutboundMessage makeRequest(quint16 sequence, bool useInt) const;
    OutboundMessage makeItem(quint16 sequence, bool useInt) const;
    OutboundMessage makeAck(MAV_MISSION_RESULT result) const;

    int m_maxRetries = 3;
    int m_retryCount = 0;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_PRIMARY;
    State m_state = State::Idle;
    Direction m_direction = Direction::None;
    Key m_key;
    quint16 m_totalCount = 0;
    quint16 m_nextSequence = 0;
    MAV_MISSION_RESULT m_result = MAV_MISSION_ACCEPTED;
    QString m_errorString;
    QVector<mavlink_mission_item_int_t> m_downloadItems;
    QVector<mavlink_mission_item_int_t> m_uploadItems;
    std::optional<OutboundMessage> m_lastOutbound;
};

#endif // MISSIONTRANSFERSERVICE_H
