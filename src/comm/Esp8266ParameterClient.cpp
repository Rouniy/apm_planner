#include "Esp8266ParameterClient.h"

#include "ExactLinkTransmitter.h"
#include "VehicleTargetManager.h"

#include <QPointer>
#include <QSet>

#include <cstring>

namespace {

QString parameterId(const char id[16])
{
    int length = 0;
    while (length < 16 && id[length] != '\0') {
        ++length;
    }
    return QString::fromLatin1(id, length);
}

QString transportFailure(ExactLinkTransmitter::SendResult result)
{
    switch (result) {
    case ExactLinkTransmitter::SendResult::Sent:
        return QString();
    case ExactLinkTransmitter::SendResult::InvalidLink:
        return Esp8266ParameterClient::tr("the selected link is invalid");
    case ExactLinkTransmitter::SendResult::InvalidMessage:
        return Esp8266ParameterClient::tr("the MAVLink message is unavailable");
    case ExactLinkTransmitter::SendResult::IncompatibleVersion:
        return Esp8266ParameterClient::tr("the selected link cannot carry the message");
    case ExactLinkTransmitter::SendResult::TransportUnavailable:
        return Esp8266ParameterClient::tr("the selected link rejected the frame");
    }
    return Esp8266ParameterClient::tr("the selected link rejected the frame");
}

QString commandFailure(quint16 command, quint8 result)
{
    return Esp8266ParameterClient::tr(
               "ESP8266 command %1 was rejected (MAV_RESULT %2).")
        .arg(command)
        .arg(result);
}

} // namespace

Esp8266ParameterClient::Esp8266ParameterClient(
    VehicleTargetManager *targetManager, ExactLinkTransmitter *transmitter,
    QObject *parent)
    : QObject(parent),
      m_targetManager(targetManager),
      m_transmitter(transmitter)
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_transmitter);
    qRegisterMetaType<Esp8266ParameterClient::Operation>(
        "Esp8266ParameterClient::Operation");
    qRegisterMetaType<Esp8266RawParameters>("Esp8266RawParameters");
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout,
            this, &Esp8266ParameterClient::handleTimeout);
    if (m_targetManager) {
        connect(m_targetManager,
                &VehicleTargetManager::targetGenerationChanged,
                this,
                &Esp8266ParameterClient::handleTargetGenerationChanged);
    }
}

Esp8266ParameterClient::~Esp8266ParameterClient()
{
    // Destruction is intentionally silent: pages own their client and may be
    // destroyed during application shutdown while signals have live slots.
    m_timer.stop();
    clearOperation();
}

void Esp8266ParameterClient::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

bool Esp8266ParameterClient::bind(const VehicleTargetLease &primaryTarget)
{
    m_primaryTarget = VehicleTargetLease{};
    if (isBusy()) {
        finishFailure(tr("the client was bound to another target"), true);
    }
    if (!primaryTarget.isValid() || !m_targetManager
        || primaryTarget.endpoint.systemId <= 0
        || primaryTarget.endpoint.systemId > 255
        || !m_targetManager->isCurrentTarget(
            primaryTarget.endpoint.linkId,
            primaryTarget.endpoint.systemId,
            primaryTarget.endpoint.componentId,
            primaryTarget.generation)) {
        return false;
    }
    m_primaryTarget = primaryTarget;
    return true;
}

void Esp8266ParameterClient::unbind()
{
    m_primaryTarget = VehicleTargetLease{};
    if (isBusy()) {
        finishFailure(tr("the client was unbound"), true);
    }
}

bool Esp8266ParameterClient::targetIsCurrent() const
{
    return m_targetManager && m_primaryTarget.isValid()
        && m_targetManager->isCurrentTarget(
            m_primaryTarget.endpoint.linkId,
            m_primaryTarget.endpoint.systemId,
            m_primaryTarget.endpoint.componentId,
            m_primaryTarget.generation);
}

bool Esp8266ParameterClient::sourceMatches(
    int linkId, const mavlink_message_t &message) const
{
    return targetIsCurrent()
        && linkId == m_primaryTarget.endpoint.linkId
        && message.sysid == m_primaryTarget.endpoint.systemId
        && message.compid == UdpBridgeComponentId;
}

Esp8266ParameterClient::SendResult
Esp8266ParameterClient::validateStart() const
{
    if (!m_primaryTarget.isValid()) {
        return SendResult::NotBound;
    }
    if (!targetIsCurrent()) {
        return SendResult::StaleTarget;
    }
    if (isBusy()) {
        return SendResult::Busy;
    }
    if (m_stage != Stage::Idle) {
        return SendResult::Busy;
    }
    if (!m_transmitter) {
        return SendResult::TransportUnavailable;
    }
    return SendResult::Sent;
}

Esp8266ParameterClient::SendResult
Esp8266ParameterClient::sendMessage(mavlink_message_t message)
{
    if (!m_primaryTarget.isValid()) {
        m_lastError = tr("no target is bound");
        return SendResult::NotBound;
    }
    if (!targetIsCurrent()) {
        m_lastError = tr("the selected vehicle changed");
        return SendResult::StaleTarget;
    }
    if (!m_transmitter) {
        m_lastError = tr("the transmitter is unavailable");
        return SendResult::TransportUnavailable;
    }
    const ExactLinkTransmitter::SendResult result =
        m_transmitter->sendMessage(
            m_primaryTarget.endpoint.linkId,
            m_localSystemId, m_localComponentId, message);
    m_lastError = transportFailure(result);
    return result == ExactLinkTransmitter::SendResult::Sent
        ? SendResult::Sent : SendResult::TransportUnavailable;
}

Esp8266ParameterClient::SendResult
Esp8266ParameterClient::requestParameters()
{
    const SendResult ready = validateStart();
    if (ready != SendResult::Sent) {
        return ready;
    }
    m_operation = Operation::Loading;
    m_operationGeneration = m_primaryTarget.generation;
    m_stage = Stage::LoadReply;
    m_values.clear();
    m_attempts = 0;
    const SendResult sent = sendParameterListRequest();
    if (sent != SendResult::Sent) {
        clearOperation();
        return sent;
    }
    m_timer.start(m_loadTimeoutMs);
    emit loadStarted(m_operationGeneration);
    return SendResult::Sent;
}

Esp8266ParameterClient::SendResult Esp8266ParameterClient::save(
    const QList<Esp8266ParameterWrite> &writes)
{
    const SendResult ready = validateStart();
    if (ready != SendResult::Sent) {
        return ready;
    }
    if (writes.isEmpty()) {
        m_lastError = tr("the parameter write list is empty");
        return SendResult::InvalidRequest;
    }
    QSet<QString> names;
    for (const Esp8266ParameterWrite &write : writes) {
        const QByteArray id = write.name.toLatin1();
        if (id.isEmpty() || id.size() > 16 || names.contains(write.name)) {
            m_lastError = tr("a parameter name is empty, too long or duplicated");
            return SendResult::InvalidRequest;
        }
        names.insert(write.name);
    }

    m_operation = Operation::Saving;
    m_operationGeneration = m_primaryTarget.generation;
    m_stage = Stage::ParameterEcho;
    m_writes = writes;
    m_writeIndex = 0;
    m_attempts = 0;
    const SendResult sent = sendCurrentParameterWrite();
    if (sent != SendResult::Sent) {
        clearOperation();
        return sent;
    }
    startTimer();
    emit saveStarted(m_operationGeneration, m_writes.size());
    return SendResult::Sent;
}

Esp8266ParameterClient::SendResult
Esp8266ParameterClient::resetDefaults()
{
    const SendResult ready = validateStart();
    if (ready != SendResult::Sent) {
        return ready;
    }
    m_operation = Operation::Resetting;
    m_operationGeneration = m_primaryTarget.generation;
    m_stage = Stage::ResetEraseAck;
    m_attempts = 0;
    const SendResult sent = sendCurrentCommand();
    if (sent != SendResult::Sent) {
        clearOperation();
        return sent;
    }
    startTimer();
    emit resetStarted(m_operationGeneration);
    return SendResult::Sent;
}

Esp8266ParameterClient::SendResult
Esp8266ParameterClient::sendParameterListRequest()
{
    mavlink_param_request_list_t payload{};
    payload.target_system =
        static_cast<quint8>(m_primaryTarget.endpoint.systemId);
    payload.target_component = UdpBridgeComponentId;
    mavlink_message_t message{};
    mavlink_msg_param_request_list_encode(
        m_localSystemId, m_localComponentId, &message, &payload);
    return sendMessage(message);
}

Esp8266ParameterClient::SendResult
Esp8266ParameterClient::sendCurrentParameterWrite()
{
    if (m_writeIndex < 0 || m_writeIndex >= m_writes.size()) {
        return SendResult::InvalidRequest;
    }
    const Esp8266ParameterWrite &write = m_writes.at(m_writeIndex);
    const QByteArray name = write.name.toLatin1();
    if (name.isEmpty() || name.size() > 16) {
        return SendResult::InvalidRequest;
    }

    mavlink_param_set_t payload{};
    payload.target_system =
        static_cast<quint8>(m_primaryTarget.endpoint.systemId);
    payload.target_component = UdpBridgeComponentId;
    payload.param_type = MAV_PARAM_TYPE_UINT32;
    const quint32 bits = write.value;
    static_assert(sizeof(payload.param_value) == sizeof(quint32),
                  "classic MAVLink parameter values are 32-bit");
    std::memcpy(&payload.param_value, &bits, sizeof(payload.param_value));
    std::memcpy(payload.param_id, name.constData(),
                static_cast<size_t>(name.size()));
    mavlink_message_t message{};
    mavlink_msg_param_set_encode(
        m_localSystemId, m_localComponentId, &message, &payload);
    ++m_attempts;
    return sendMessage(message);
}

Esp8266ParameterClient::SendResult
Esp8266ParameterClient::sendCommand(
    MAV_CMD command, float param1, float param2, quint8 confirmation)
{
    mavlink_command_long_t payload{};
    payload.target_system =
        static_cast<quint8>(m_primaryTarget.endpoint.systemId);
    payload.target_component = UdpBridgeComponentId;
    payload.command = static_cast<quint16>(command);
    payload.confirmation = confirmation;
    payload.param1 = param1;
    payload.param2 = param2;
    mavlink_message_t message{};
    mavlink_msg_command_long_encode(
        m_localSystemId, m_localComponentId, &message, &payload);
    ++m_attempts;
    return sendMessage(message);
}

Esp8266ParameterClient::SendResult
Esp8266ParameterClient::sendCurrentCommand()
{
    const quint8 confirmation = static_cast<quint8>(qMin(m_attempts, 255));
    switch (m_stage) {
    case Stage::SaveStorageAck:
        return sendCommand(MAV_CMD_PREFLIGHT_STORAGE, 1.0F, 0.0F,
                           confirmation);
    case Stage::SaveRebootAck:
        return sendCommand(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, 0.0F, 1.0F,
                           confirmation);
    case Stage::ResetEraseAck:
        return sendCommand(MAV_CMD_PREFLIGHT_STORAGE, 2.0F, 0.0F,
                           confirmation);
    case Stage::ResetStorageAck:
        return sendCommand(MAV_CMD_PREFLIGHT_STORAGE, 1.0F, 0.0F,
                           confirmation);
    default:
        return SendResult::InvalidRequest;
    }
}

void Esp8266ParameterClient::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (!isBusy() || !sourceMatches(linkId, message)) {
        return;
    }
    if (message.msgid == MAVLINK_MSG_ID_PARAM_VALUE) {
        handleParameterValue(message);
    } else if (message.msgid == MAVLINK_MSG_ID_COMMAND_ACK) {
        handleCommandAck(message);
    }
}

void Esp8266ParameterClient::handleParameterValue(
    const mavlink_message_t &message)
{
    mavlink_param_value_t payload{};
    mavlink_msg_param_value_decode(&message, &payload);
    const QString name = parameterId(payload.param_id);
    quint32 bits = 0;
    std::memcpy(&bits, &payload.param_value, sizeof(payload.param_value));
    const QByteArray raw = Esp8266SettingsCodec::RawFromUInt32(bits);

    if (m_stage == Stage::LoadReply) {
        if (!Esp8266SettingsCodec::RequiredParameterNames().contains(name)) {
            return;
        }
        m_values.insert(name, raw);
        if (missingParameters().isEmpty()) {
            finishLoad();
        }
        return;
    }
    if (m_stage != Stage::ParameterEcho
        || m_writeIndex < 0 || m_writeIndex >= m_writes.size()) {
        return;
    }
    const Esp8266ParameterWrite &expected = m_writes.at(m_writeIndex);
    if (name != expected.name || raw != expected.rawBytes()) {
        return;
    }

    m_timer.stop();
    const QString completedName = expected.name;
    ++m_writeIndex;
    const quint64 generation = m_operationGeneration;
    const int completedWrites = m_writeIndex;
    const int totalWrites = m_writes.size();
    const QPointer<Esp8266ParameterClient> guard(this);
    emit saveProgress(generation, completedWrites,
                      totalWrites, completedName);
    // A direct slot may cancel/unbind/rebind the client or destroy its owning
    // page. Never continue the save after such a re-entrant state change.
    if (!guard || m_operation != Operation::Saving
        || m_operationGeneration != generation
        || m_stage != Stage::ParameterEcho
        || m_writeIndex != completedWrites
        || m_writes.size() != totalWrites) {
        return;
    }
    if (m_writeIndex < m_writes.size()) {
        m_attempts = 0;
        const SendResult result = sendCurrentParameterWrite();
        if (result != SendResult::Sent) {
            finishFailure(tr("ESP8266 parameter send failed: %1")
                              .arg(m_lastError), false);
            return;
        }
        startTimer();
        return;
    }
    beginCommandStage(Stage::SaveStorageAck);
}

void Esp8266ParameterClient::handleCommandAck(
    const mavlink_message_t &message)
{
    if (m_stage != Stage::SaveStorageAck
        && m_stage != Stage::SaveRebootAck
        && m_stage != Stage::ResetEraseAck
        && m_stage != Stage::ResetStorageAck) {
        return;
    }
    mavlink_command_ack_t acknowledgement{};
    mavlink_msg_command_ack_decode(&message, &acknowledgement);
    const quint16 expectedCommand =
        m_stage == Stage::SaveRebootAck
        ? static_cast<quint16>(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN)
        : static_cast<quint16>(MAV_CMD_PREFLIGHT_STORAGE);
    if (acknowledgement.command != expectedCommand
        || (acknowledgement.target_system != 0
            && acknowledgement.target_system != m_localSystemId)
        || (acknowledgement.target_component != 0
            && acknowledgement.target_component != m_localComponentId)) {
        return;
    }
    if (acknowledgement.result == MAV_RESULT_IN_PROGRESS) {
        startTimer();
        return;
    }
    if (acknowledgement.result != MAV_RESULT_ACCEPTED) {
        finishFailure(commandFailure(acknowledgement.command,
                                     acknowledgement.result), false);
        return;
    }

    m_timer.stop();
    switch (m_stage) {
    case Stage::SaveStorageAck:
        beginCommandStage(Stage::SaveRebootAck);
        return;
    case Stage::ResetEraseAck:
        beginCommandStage(Stage::ResetStorageAck);
        return;
    case Stage::SaveRebootAck:
    case Stage::ResetStorageAck:
        finishSuccess();
        return;
    default:
        return;
    }
}

void Esp8266ParameterClient::beginCommandStage(Stage stage)
{
    m_timer.stop();
    m_stage = stage;
    m_attempts = 0;
    const SendResult result = sendCurrentCommand();
    if (result != SendResult::Sent) {
        finishFailure(tr("ESP8266 command send failed: %1")
                          .arg(m_lastError), false);
        return;
    }
    startTimer();
}

void Esp8266ParameterClient::startTimer()
{
    m_timer.start(m_stage == Stage::LoadReply
                      ? m_loadTimeoutMs : m_acknowledgementTimeoutMs);
}

void Esp8266ParameterClient::handleTimeout()
{
    if (!isBusy()) {
        return;
    }
    if (!targetIsCurrent()) {
        const quint64 invalidated = m_primaryTarget.generation;
        m_primaryTarget = VehicleTargetLease{};
        finishFailure(tr("the selected vehicle changed"), true);
        emit leaseInvalidated(invalidated);
        return;
    }
    if (m_stage == Stage::LoadReply) {
        const QStringList missing = missingParameters();
        const QString reason = m_values.isEmpty()
            ? tr("No ESP8266 / UDP-bridge component responded.")
            : tr("Incomplete ESP8266 response; missing: %1.")
                  .arg(missing.join(QStringLiteral(", ")));
        const quint64 generation = m_operationGeneration;
        clearOperation();
        emit loadFailed(generation, reason, missing);
        return;
    }
    if (m_attempts > m_maximumRetries) {
        finishFailure(
            m_stage == Stage::ParameterEcho
                ? tr("The ESP8266 did not acknowledge parameter %1.")
                      .arg(m_writeIndex >= 0 && m_writeIndex < m_writes.size()
                               ? m_writes.at(m_writeIndex).name : QString())
                : tr("The ESP8266 did not acknowledge the command."),
            false);
        return;
    }
    const SendResult result = m_stage == Stage::ParameterEcho
        ? sendCurrentParameterWrite() : sendCurrentCommand();
    if (result != SendResult::Sent) {
        finishFailure(tr("ESP8266 retry failed: %1").arg(m_lastError),
                      false);
        return;
    }
    startTimer();
}

QStringList Esp8266ParameterClient::missingParameters() const
{
    QStringList missing;
    const QStringList required =
        Esp8266SettingsCodec::RequiredParameterNames();
    for (const QString &name : required) {
        if (!m_values.contains(name)) {
            missing.append(name);
        }
    }
    return missing;
}

void Esp8266ParameterClient::finishLoad()
{
    const quint64 generation = m_operationGeneration;
    const Esp8266RawParameters values = m_values;
    clearOperation();
    emit parametersLoaded(generation, values);
}

void Esp8266ParameterClient::finishSuccess()
{
    const Operation completed = m_operation;
    const quint64 generation = m_operationGeneration;
    clearOperation();
    if (completed == Operation::Saving) {
        emit saveCompleted(generation);
    } else if (completed == Operation::Resetting) {
        emit resetCompleted(generation);
    }
}

void Esp8266ParameterClient::finishFailure(
    const QString &reason, bool cancelled)
{
    const Operation failed = m_operation;
    const quint64 generation = m_operationGeneration;
    m_lastError = reason;
    clearOperation();
    if (failed != Operation::None) {
        emit operationFailed(failed, generation, reason, cancelled);
    }
}

void Esp8266ParameterClient::clearOperation()
{
    m_timer.stop();
    m_operation = Operation::None;
    m_operationGeneration = 0;
    m_stage = Stage::Idle;
    m_values.clear();
    m_writes.clear();
    m_writeIndex = 0;
    m_attempts = 0;
}

void Esp8266ParameterClient::cancel()
{
    if (isBusy()) {
        finishFailure(tr("cancelled"), true);
    }
}

void Esp8266ParameterClient::setTimingForTesting(
    int loadTimeoutMs, int acknowledgementTimeoutMs, int maximumRetries)
{
    m_loadTimeoutMs = qMax(1, loadTimeoutMs);
    m_acknowledgementTimeoutMs = qMax(1, acknowledgementTimeoutMs);
    m_maximumRetries = qMax(0, maximumRetries);
}

void Esp8266ParameterClient::handleTargetGenerationChanged(
    qulonglong generation)
{
    if (!m_primaryTarget.isValid()
        || generation == m_primaryTarget.generation
        || targetIsCurrent()) {
        return;
    }
    const quint64 invalidated = m_primaryTarget.generation;
    m_primaryTarget = VehicleTargetLease{};
    if (isBusy()) {
        finishFailure(tr("the selected vehicle changed"), true);
    }
    emit leaseInvalidated(invalidated);
}
