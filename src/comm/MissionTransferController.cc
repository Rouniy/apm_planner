#include "MissionTransferController.h"

#include <QThread>

#include <limits>
#include <variant>

MissionTransferController::MissionTransferController(
        MissionProtocolCoordinator *coordinator,
        MissionTransferTransport *transport,
        int timeoutMs,
        int maxRetries,
        QObject *parent)
    : QObject(parent)
    , m_coordinator(coordinator)
    , m_transport(transport)
    , m_service(maxRetries,
                transport ? transport->localSystemId() : 255,
                transport ? transport->localComponentId()
                          : MAV_COMP_ID_PRIMARY)
    , m_timeoutMs(qMax(1, timeoutMs))
{
    Q_ASSERT(!coordinator || coordinator->thread() == thread());
    Q_ASSERT(!transport || transport->thread() == thread());

    m_timeoutTimer.setSingleShot(true);

    if (m_transport) {
        connect(m_transport.data(), &MissionTransferTransport::messageReceived,
                this, &MissionTransferController::handleMessage);
        connect(m_transport.data(), &MissionTransferTransport::unavailable,
                this, &MissionTransferController::handleUnavailable);
        connect(m_transport.data(), &QObject::destroyed, this, [this]() {
            m_transport = nullptr;
            m_transportOperation = false;
            failActive(QStringLiteral("Mission transport was destroyed"));
        });
    }

    if (m_coordinator) {
        connect(m_coordinator.data(), &QObject::destroyed, this, [this]() {
            m_coordinator = nullptr;
            m_lease = {};
            failActive(QStringLiteral("Mission protocol coordinator was destroyed"));
        });
    }
}

MissionTransferController::~MissionTransferController()
{
    cleanupWithoutSignal();
}

bool MissionTransferController::startDownload(MAV_MISSION_TYPE missionType)
{
    return startOperation(missionType,
                          MissionTransferService::Direction::Download, {});
}

bool MissionTransferController::startUpload(
        MAV_MISSION_TYPE missionType,
        const QVector<mavlink_mission_item_int_t> &items)
{
    return startOperation(missionType,
                          MissionTransferService::Direction::Upload, items);
}

bool MissionTransferController::cancel(const QString &reason)
{
    if (!m_busy)
        return false;
    if (!ownsLease()) {
        failActive(QStringLiteral("Mission protocol lease was lost"));
        return false;
    }

    return applyTransition(m_service.cancel(reason));
}

bool MissionTransferController::startOperation(
        MAV_MISSION_TYPE missionType,
        MissionTransferService::Direction direction,
        const QVector<mavlink_mission_item_int_t> &items)
{
    if (m_busy || !m_coordinator || !m_transport
            || m_transferId == std::numeric_limits<quint64>::max()) {
        return false;
    }

    MissionProtocolCoordinator::MissionType coordinatorType;
    if (!coordinatorTypeFor(missionType, &coordinatorType))
        return false;

    m_lease = m_coordinator->tryAcquire(this, coordinatorType);
    if (!m_lease.isValid())
        return false;

    if (!m_transport->beginOperation()) {
        m_coordinator->release(m_lease);
        m_lease = {};
        return false;
    }
    m_transportOperation = true;

    const MissionTransferService::Key key =
            m_transport->missionKey(missionType);
    if (key.missionType != missionType
        || !m_service.setLocalIdentity(m_transport->localSystemId(),
                                       m_transport->localComponentId())) {
        m_transportOperation = false;
        m_transport->endOperation();
        m_coordinator->release(m_lease);
        m_lease = {};
        return false;
    }

    ++m_operationEpoch;
    if (m_operationEpoch == 0)
        ++m_operationEpoch;
    disarmTimeout();

    ++m_transferId;
    emit transferIdChanged(m_transferId);
    setProgress(0);
    m_busy = true;
    emit busyChanged(true);

    m_service.reset();
    const MissionTransferService::Transition transition =
            direction == MissionTransferService::Direction::Download
            ? m_service.startDownload(key)
            : m_service.startUpload(key, items);
    return applyTransition(transition);
}

void MissionTransferController::handleMessage(const mavlink_message_t &message)
{
    if (!m_busy)
        return;
    if (!ownsLease()) {
        failActive(QStringLiteral("Mission protocol lease was lost"));
        return;
    }

    MissionTransferService::Transition transition;
    switch (message.msgid) {
    case MAVLINK_MSG_ID_MISSION_COUNT:
    {
        mavlink_mission_count_t payload{};
        mavlink_msg_mission_count_decode(&message, &payload);
        transition = m_service.handleMissionCount(
                message.sysid, message.compid, payload);
        break;
    }
    case MAVLINK_MSG_ID_MISSION_REQUEST:
    {
        mavlink_mission_request_t payload{};
        mavlink_msg_mission_request_decode(&message, &payload);
        transition = m_service.handleMissionRequest(
                message.sysid, message.compid, payload);
        break;
    }
    case MAVLINK_MSG_ID_MISSION_REQUEST_INT:
    {
        mavlink_mission_request_int_t payload{};
        mavlink_msg_mission_request_int_decode(&message, &payload);
        transition = m_service.handleMissionRequestInt(
                message.sysid, message.compid, payload);
        break;
    }
    case MAVLINK_MSG_ID_MISSION_ITEM:
    {
        mavlink_mission_item_t payload{};
        mavlink_msg_mission_item_decode(&message, &payload);
        transition = m_service.handleMissionItem(
                message.sysid, message.compid, payload);
        break;
    }
    case MAVLINK_MSG_ID_MISSION_ITEM_INT:
    {
        mavlink_mission_item_int_t payload{};
        mavlink_msg_mission_item_int_decode(&message, &payload);
        transition = m_service.handleMissionItemInt(
                message.sysid, message.compid, payload);
        break;
    }
    case MAVLINK_MSG_ID_MISSION_ACK:
    {
        mavlink_mission_ack_t payload{};
        mavlink_msg_mission_ack_decode(&message, &payload);
        transition = m_service.handleMissionAck(
                message.sysid, message.compid, payload);
        break;
    }
    case MAVLINK_MSG_ID_MISSION_REQUEST_LIST:
    {
        mavlink_mission_request_list_t payload{};
        mavlink_msg_mission_request_list_decode(&message, &payload);
        Q_UNUSED(payload)
        // The state machine never expects the peer to initiate another mission
        // operation while this controller owns the lease.
        return;
    }
    default:
        return;
    }

    if (transition.handled)
        applyTransition(transition);
}

void MissionTransferController::handleUnavailable()
{
    failActive(QStringLiteral("Mission transport became unavailable"));
}

void MissionTransferController::timeout()
{
    if (!m_busy || m_timerEpoch == 0
            || m_timerEpoch != m_operationEpoch) {
        return;
    }

    disarmTimeout();
    if (!ownsLease()) {
        failActive(QStringLiteral("Mission protocol lease was lost"));
        return;
    }
    applyTransition(m_service.onTimeout());
}

bool MissionTransferController::applyTransition(
        const MissionTransferService::Transition &transition)
{
    if (!m_busy || !transition.handled)
        return false;

    disarmTimeout();
    const quint64 transitionEpoch = m_operationEpoch;

    if (transition.outbound && !sendOutbound(*transition.outbound)) {
        if (m_busy && transitionEpoch == m_operationEpoch) {
            // Completion/cancellation is already decided before its terminal
            // ACK is sent. Preserve downloaded data and the user's cancelled
            // result even if that best-effort final write loses the link.
            if (!isTerminal(m_service.state())) {
                m_service.fail(
                        QStringLiteral("Unable to send mission protocol message"));
            }
            updateProgress();
            finishTransfer();
        }
        return false;
    }

    // A transport may report a synchronous failure from sendMessage().
    if (!m_busy || transitionEpoch != m_operationEpoch)
        return false;

    updateProgress();
    if (isTerminal(m_service.state())) {
        finishTransfer();
    } else if (m_service.isActive()) {
        armTimeout();
    }
    return true;
}

bool MissionTransferController::sendOutbound(
        const MissionTransferService::OutboundMessage &outbound)
{
    if (!m_transport || !ownsLease())
        return false;

    mavlink_message_t message{};
    switch (outbound.type) {
    case MissionTransferService::MessageType::MissionRequestList:
    {
        const auto *payload =
                std::get_if<mavlink_mission_request_list_t>(&outbound.payload);
        if (!payload)
            return false;
        mavlink_msg_mission_request_list_encode(
                m_service.localSystemId(), m_service.localComponentId(),
                &message, payload);
        break;
    }
    case MissionTransferService::MessageType::MissionCount:
    {
        const auto *payload =
                std::get_if<mavlink_mission_count_t>(&outbound.payload);
        if (!payload)
            return false;
        mavlink_msg_mission_count_encode(
                m_service.localSystemId(), m_service.localComponentId(),
                &message, payload);
        break;
    }
    case MissionTransferService::MessageType::MissionRequest:
    {
        const auto *payload =
                std::get_if<mavlink_mission_request_t>(&outbound.payload);
        if (!payload)
            return false;
        mavlink_msg_mission_request_encode(
                m_service.localSystemId(), m_service.localComponentId(),
                &message, payload);
        break;
    }
    case MissionTransferService::MessageType::MissionRequestInt:
    {
        const auto *payload =
                std::get_if<mavlink_mission_request_int_t>(&outbound.payload);
        if (!payload)
            return false;
        mavlink_msg_mission_request_int_encode(
                m_service.localSystemId(), m_service.localComponentId(),
                &message, payload);
        break;
    }
    case MissionTransferService::MessageType::MissionItem:
    {
        const auto *payload =
                std::get_if<mavlink_mission_item_t>(&outbound.payload);
        if (!payload)
            return false;
        mavlink_msg_mission_item_encode(
                m_service.localSystemId(), m_service.localComponentId(),
                &message, payload);
        break;
    }
    case MissionTransferService::MessageType::MissionItemInt:
    {
        const auto *payload =
                std::get_if<mavlink_mission_item_int_t>(&outbound.payload);
        if (!payload)
            return false;
        mavlink_msg_mission_item_int_encode(
                m_service.localSystemId(), m_service.localComponentId(),
                &message, payload);
        break;
    }
    case MissionTransferService::MessageType::MissionAck:
    {
        const auto *payload =
                std::get_if<mavlink_mission_ack_t>(&outbound.payload);
        if (!payload)
            return false;
        mavlink_msg_mission_ack_encode(
                m_service.localSystemId(), m_service.localComponentId(),
                &message, payload);
        break;
    }
    }

    return m_transport->sendMessage(message);
}

void MissionTransferController::armTimeout()
{
    disarmTimeout();
    m_timerEpoch = m_operationEpoch;
    const quint64 operationEpoch = m_operationEpoch;
    const quint64 timerGeneration = m_timerGeneration;
    m_timeoutConnection = connect(
            &m_timeoutTimer, &QTimer::timeout, this,
            [this, operationEpoch, timerGeneration]() {
        if (operationEpoch == m_operationEpoch
                && timerGeneration == m_timerGeneration) {
            timeout();
        }
    });
    m_timeoutTimer.start(m_timeoutMs);
}

void MissionTransferController::disarmTimeout()
{
    m_timeoutTimer.stop();
    if (m_timeoutConnection)
        disconnect(m_timeoutConnection);
    m_timeoutConnection = {};
    m_timerEpoch = 0;
    ++m_timerGeneration;
    if (m_timerGeneration == 0)
        ++m_timerGeneration;
}

void MissionTransferController::updateProgress()
{
    if (m_service.state() == MissionTransferService::State::Complete) {
        setProgress(100);
        return;
    }

    const quint16 total = m_service.totalCount();
    if (total == 0) {
        setProgress(0);
        return;
    }
    const int completed = qMin<int>(m_service.nextSequence(), total);
    setProgress(qMin(99, completed * 100 / static_cast<int>(total)));
}

void MissionTransferController::setProgress(int progress)
{
    progress = qBound(0, progress, 100);
    if (m_progress == progress)
        return;
    m_progress = progress;
    emit progressChanged(m_progress);
}

void MissionTransferController::failActive(const QString &reason)
{
    if (!m_busy)
        return;
    disarmTimeout();
    m_service.fail(reason);
    updateProgress();
    finishTransfer();
}

void MissionTransferController::finishTransfer()
{
    if (!m_busy)
        return;

    MissionTransferResult terminal;
    terminal.transferId = m_transferId;
    terminal.missionType = m_service.key().missionType;
    terminal.direction = m_service.direction();
    terminal.state = m_service.state();
    terminal.result = m_service.missionResult();
    terminal.errorString = m_service.errorString();
    if (terminal.succeeded()
            && terminal.direction
                    == MissionTransferService::Direction::Download) {
        terminal.downloadedItems = m_service.downloadedItems();
    }

    disarmTimeout();
    ++m_operationEpoch;
    if (m_operationEpoch == 0)
        ++m_operationEpoch;

    m_busy = false;
    if (m_transportOperation) {
        m_transportOperation = false;
        if (m_transport)
            m_transport->endOperation();
    }
    if (m_coordinator && m_lease.isValid())
        m_coordinator->release(m_lease);
    m_lease = {};
    m_service.reset();

    emit busyChanged(false);
    emit transferFinished(terminal);
}

void MissionTransferController::cleanupWithoutSignal()
{
    disarmTimeout();

    // A best-effort terminal ACK is sent before relinquishing the transport
    // and the exact coordinator lease. Destruction deliberately emits no UI
    // completion signal.
    std::optional<MissionTransferService::OutboundMessage> terminalOutbound;
    if (m_busy && ownsLease()) {
        const auto transition = m_service.cancel(
                QStringLiteral("Mission transfer controller destroyed"));
        if (transition.outbound)
            terminalOutbound = *transition.outbound;
    }

    // Suppress re-entrant unavailable/message signals while destruction sends
    // its best-effort cancellation.
    m_busy = false;
    if (terminalOutbound)
        sendOutbound(*terminalOutbound);
    if (m_transportOperation) {
        m_transportOperation = false;
        if (m_transport)
            m_transport->endOperation();
    }
    if (m_coordinator && m_lease.isValid())
        m_coordinator->release(m_lease);
    m_lease = {};
    m_service.reset();
}

bool MissionTransferController::ownsLease() const
{
    return m_coordinator && m_lease.isValid()
            && m_coordinator->owns(m_lease);
}

bool MissionTransferController::coordinatorTypeFor(
        MAV_MISSION_TYPE missionType,
        MissionProtocolCoordinator::MissionType *coordinatorType)
{
    if (!coordinatorType)
        return false;

    switch (missionType) {
    case MAV_MISSION_TYPE_MISSION:
        *coordinatorType = MissionProtocolCoordinator::MissionType::Mission;
        return true;
    case MAV_MISSION_TYPE_FENCE:
        *coordinatorType = MissionProtocolCoordinator::MissionType::Fence;
        return true;
    case MAV_MISSION_TYPE_RALLY:
        *coordinatorType = MissionProtocolCoordinator::MissionType::Rally;
        return true;
    default:
        return false;
    }
}

bool MissionTransferController::isTerminal(MissionTransferService::State state)
{
    return state == MissionTransferService::State::Complete
            || state == MissionTransferService::State::Cancelled
            || state == MissionTransferService::State::Error;
}
