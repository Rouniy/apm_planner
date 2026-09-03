#include "DeviceOperationService.h"

#include "ExactLinkTransmitter.h"
#include "VehicleTargetManager.h"

#include <QPointer>

#include <atomic>
#include <cstring>

namespace {

std::atomic<quint32> s_nextDeviceOperationRequestId{0};

QByteArray encodedBusName(const DeviceOperationService::Request &request)
{
    return request.busType == DeviceOperationService::BusType::SPI
        ? request.busName.trimmed().toUtf8() : QByteArray();
}

} // namespace

DeviceOperationService::DeviceOperationService(
    VehicleTargetManager *targetManager, ExactLinkTransmitter *transmitter,
    QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_transmitter(transmitter)
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_transmitter);
    qRegisterMetaType<DeviceOperationService::Request>(
        "DeviceOperationService::Request");
    qRegisterMetaType<DeviceOperationService::OperationResult>(
        "DeviceOperationService::OperationResult");

    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout,
            this, &DeviceOperationService::handleTimeout);
    if (m_targetManager) {
        connect(m_targetManager,
                &VehicleTargetManager::targetGenerationChanged,
                this, [this](qulonglong generation) {
            if (m_lease.isValid() && generation != m_lease.generation) {
                invalidateBinding(tr(
                    "The active modem or vehicle changed. Rebind before "
                    "another DEVICE_OP operation."));
            }
        });
    }
}

DeviceOperationService::~DeviceOperationService()
{
    // Destruction is deliberately silent: detach all asynchronous state before
    // QObject removes signal connections to a closing window/view model.
    m_shuttingDown = true;
    m_timeout.stop();
    m_operation.reset();
}

void DeviceOperationService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

void DeviceOperationService::setTimeoutMs(int timeoutMs)
{
    m_timeoutMs = qMax(1, timeoutMs);
}

bool DeviceOperationService::bind(const VehicleTargetLease &lease)
{
    if (m_shuttingDown) {
        m_lastError = StartFailureText(StartResult::ShuttingDown);
        return false;
    }

    if (m_operation) {
        QPointer<DeviceOperationService> guard(this);
        cancel();
        if (!guard) {
            return false;
        }
        // A cancellation callback is allowed to start another operation.
        // Never replace its target lease underneath that new transaction.
        if (m_operation) {
            m_lastError = StartFailureText(StartResult::Busy);
            return false;
        }
    }

    if (!lease.isValid() || !m_targetManager
        || !m_targetManager->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation)) {
        m_lease = VehicleTargetLease();
        m_requiresRebind = false;
        m_armState = ArmState::Unknown;
        m_lastError = tr("No current exact vehicle target is available.");
        emit stateChanged();
        return false;
    }

    m_lease = lease;
    m_requiresRebind = false;
    m_armState = knownArmState(lease.endpoint);
    m_lastError.clear();
    emit stateChanged();
    return true;
}

void DeviceOperationService::unbind()
{
    if (!m_lease.isValid() && !m_operation && !m_requiresRebind) {
        return;
    }

    m_lease = VehicleTargetLease();
    m_requiresRebind = false;
    m_armState = ArmState::Unknown;
    if (m_operation) {
        OperationResult result = m_operation->result;
        result.cancelled = true;
        result.error = tr("The DEVICE_OP target was unbound.");
        finish(result);
    } else {
        m_lastError.clear();
        emit stateChanged();
    }
}

bool DeviceOperationService::isBound() const
{
    return !m_shuttingDown && !m_requiresRebind && targetIsCurrent();
}

bool DeviceOperationService::targetIsCurrent() const
{
    return m_targetManager && m_lease.isValid()
        && m_targetManager->isCurrentTarget(
            m_lease.endpoint.linkId, m_lease.endpoint.systemId,
            m_lease.endpoint.componentId, m_lease.generation);
}

DeviceOperationService::StartResult DeviceOperationService::startRead(
    const Request &request)
{
    return begin(request, false);
}

DeviceOperationService::StartResult
DeviceOperationService::startIcm20948Test(const Request &request)
{
    return begin(request, true);
}

DeviceOperationService::StartResult DeviceOperationService::begin(
    const Request &request, bool icm20948Test)
{
    if (m_shuttingDown) {
        m_lastError = StartFailureText(StartResult::ShuttingDown);
        return StartResult::ShuttingDown;
    }
    if (m_operation) {
        m_lastError = StartFailureText(StartResult::Busy);
        return StartResult::Busy;
    }
    if (!m_lease.isValid() || m_requiresRebind) {
        const StartResult result = m_requiresRebind
            ? StartResult::StaleTarget : StartResult::NotBound;
        m_lastError = StartFailureText(result);
        return result;
    }
    if (!targetIsCurrent()) {
        invalidateBinding(tr(
            "The active modem or vehicle changed. Rebind before another "
            "DEVICE_OP operation."));
        return StartResult::StaleTarget;
    }

    const QString validationError = ValidateRequest(request);
    if (!validationError.isEmpty()
        || (icm20948Test && request.busType != BusType::SPI)) {
        m_lastError = validationError.isEmpty()
            ? tr("The ICM20948 test requires an SPI device name.")
            : validationError;
        return StartResult::InvalidRequest;
    }
    if (icm20948Test
        && (request.destinationSystemId != m_lease.endpoint.systemId
            || request.destinationComponentId
                   != m_lease.endpoint.componentId)) {
        m_lastError = tr(
            "The ICM20948 test runs only against the bound target %1:%2.")
                          .arg(m_lease.endpoint.systemId)
                          .arg(m_lease.endpoint.componentId);
        return StartResult::InvalidRequest;
    }
    if (icm20948Test && m_armState != ArmState::Disarmed) {
        m_lastError = m_armState == ArmState::Armed
            ? tr("The ICM20948 test is blocked while the bound vehicle is armed.")
            : tr("The ICM20948 test requires a confirmed disarmed heartbeat "
                 "from the exact bound vehicle.");
        return StartResult::UnsafeArmState;
    }

    Request actual = request;
    actual.busName = actual.busType == BusType::SPI
        ? actual.busName.trimmed() : QString();
    if (icm20948Test) {
        actual.busType = BusType::SPI;
        actual.bus = 0;
        actual.address = 0;
        actual.registerStart = 0xff;
        actual.count = 2;
    }

    auto operation = std::make_unique<Operation>();
    operation->stage = icm20948Test ? Stage::IcmWrite : Stage::Read;
    operation->requestId = nextRequestId();
    operation->targetGeneration = m_lease.generation;
    operation->result.request = actual;
    operation->result.icm20948Test = icm20948Test;
    operation->result.readRegister = static_cast<quint8>(actual.registerStart);
    m_operation = std::move(operation);

    Operation *const startedOperation = m_operation.get();
    const Stage startedStage = m_operation->stage;
    const quint32 startedRequestId = m_operation->requestId;
    QPointer<DeviceOperationService> guard(this);
    const StartResult sendResult = icm20948Test
        ? sendIcmWrite(*m_operation) : sendRead(*m_operation);
    if (!guard) {
        // A synchronous writer may deliver the reply and a completion slot may
        // delete this service before sendMessage() unwinds. The send did start;
        // do not turn that successful result into a UI-visible start failure.
        return StartResult::Started;
    }
    if (!m_operation || m_operation.get() != startedOperation
        || m_operation->stage != startedStage
        || m_operation->requestId != startedRequestId) {
        return m_requiresRebind
            ? StartResult::StaleTarget : StartResult::Started;
    }
    if (sendResult != StartResult::Started) {
        m_operation.reset();
        m_lastError = StartFailureText(sendResult);
        emit stateChanged();
        return sendResult;
    }

    m_lastError.clear();
    m_timeout.start(m_timeoutMs);
    emit stateChanged();
    return StartResult::Started;
}

DeviceOperationService::StartResult DeviceOperationService::sendRead(
    Operation &operation)
{
    const Request &request = operation.result.request;
    const QByteArray name = encodedBusName(request);
    char busName[MaximumBusNameBytes]{};
    if (!name.isEmpty()) {
        std::memcpy(busName, name.constData(),
                    static_cast<size_t>(name.size()));
    }

    mavlink_message_t message{};
    mavlink_msg_device_op_read_pack(
        m_localSystemId, m_localComponentId, &message,
        static_cast<quint8>(request.destinationSystemId),
        static_cast<quint8>(request.destinationComponentId),
        operation.requestId, static_cast<quint8>(request.busType),
        static_cast<quint8>(request.bus),
        static_cast<quint8>(request.address), busName,
        static_cast<quint8>(request.registerStart),
        static_cast<quint8>(request.count), 0);
    return sendMessage(message);
}

DeviceOperationService::StartResult DeviceOperationService::sendIcmWrite(
    Operation &operation)
{
    const Request &request = operation.result.request;
    const QByteArray name = encodedBusName(request);
    char busName[MaximumBusNameBytes]{};
    std::memcpy(busName, name.constData(), static_cast<size_t>(name.size()));
    quint8 data[MaximumDataLength]{};
    data[0] = 0x72;
    data[1] = 0x00;

    mavlink_message_t message{};
    mavlink_msg_device_op_write_pack(
        m_localSystemId, m_localComponentId, &message,
        static_cast<quint8>(request.destinationSystemId),
        static_cast<quint8>(request.destinationComponentId),
        operation.requestId, static_cast<quint8>(BusType::SPI), 0, 0,
        busName, 0xff, 2, data, 0);
    return sendMessage(message);
}

DeviceOperationService::StartResult DeviceOperationService::sendMessage(
    mavlink_message_t message)
{
    if (!targetIsCurrent()) {
        return StartResult::StaleTarget;
    }
    if (!m_transmitter) {
        return StartResult::TransportUnavailable;
    }
    const ExactLinkTransmitter::SendResult result =
        m_transmitter->sendMessage(
            m_lease.endpoint.linkId, m_localSystemId,
            m_localComponentId, message);
    if (result == ExactLinkTransmitter::SendResult::Sent) {
        return StartResult::Started;
    }
    if (result == ExactLinkTransmitter::SendResult::IncompatibleVersion) {
        return StartResult::Mavlink1Unsupported;
    }
    return StartResult::TransportUnavailable;
}

void DeviceOperationService::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    QPointer<DeviceOperationService> guard(this);
    updateArmState(linkId, message);
    if (!guard || !m_operation) {
        return;
    }
    if (!targetIsCurrent()
        || m_operation->targetGeneration != m_lease.generation) {
        invalidateBinding(tr(
            "The active modem or vehicle changed. The DEVICE_OP result was "
            "discarded; rebind before another operation."));
        return;
    }

    const Request &request = m_operation->result.request;
    if (linkId != m_lease.endpoint.linkId
        || message.sysid != request.destinationSystemId
        || message.compid != request.destinationComponentId) {
        return;
    }

    if (m_operation->stage == Stage::IcmWrite) {
        if (message.msgid != MAVLINK_MSG_ID_DEVICE_OP_WRITE_REPLY) {
            return;
        }
        mavlink_device_op_write_reply_t reply{};
        mavlink_msg_device_op_write_reply_decode(&message, &reply);
        if (reply.request_id != m_operation->requestId) {
            return;
        }
        m_timeout.stop();
        m_operation->result.writeReplyReceived = true;
        m_operation->result.writeResult = reply.result;
        beginIcmRead();
        return;
    }

    if (message.msgid != MAVLINK_MSG_ID_DEVICE_OP_READ_REPLY) {
        return;
    }
    mavlink_device_op_read_reply_t reply{};
    mavlink_msg_device_op_read_reply_decode(&message, &reply);
    if (reply.request_id != m_operation->requestId) {
        return;
    }

    m_timeout.stop();
    OperationResult result = m_operation->result;
    result.readReplyReceived = true;
    result.readResult = reply.result;
    // MP10 addresses the dump from the requested register even if a malformed
    // or unusual peer echoes another regstart value.
    result.readRegister = static_cast<quint8>(request.registerStart);
    const int length = qMin<int>(reply.count, MaximumDataLength);
    result.readData = QByteArray(
        reinterpret_cast<const char *>(reply.data), length);
    if (reply.count > MaximumDataLength) {
        result.error = tr(
            "The DEVICE_OP reply declared %1 bytes; the protocol field holds "
            "at most %2.")
            .arg(reply.count).arg(MaximumDataLength);
    }
    finish(result);
}

void DeviceOperationService::beginIcmRead()
{
    if (!m_operation || m_operation->stage != Stage::IcmWrite) {
        return;
    }
    if (!targetIsCurrent()
        || m_operation->targetGeneration != m_lease.generation) {
        invalidateBinding(tr(
            "The active modem or vehicle changed during the ICM20948 test."));
        return;
    }
    if (m_armState != ArmState::Disarmed) {
        OperationResult result = m_operation->result;
        result.cancelled = true;
        result.error = m_armState == ArmState::Armed
            ? tr("The ICM20948 test stopped because the bound vehicle armed.")
            : tr("The ICM20948 test stopped because exact disarmed state was lost.");
        finish(result);
        return;
    }

    m_operation->stage = Stage::IcmRead;
    m_operation->requestId = nextRequestId();
    Operation *const startedOperation = m_operation.get();
    const quint32 startedRequestId = m_operation->requestId;
    QPointer<DeviceOperationService> guard(this);
    const StartResult result = sendRead(*m_operation);
    if (!guard || !m_operation || m_operation.get() != startedOperation
        || m_operation->stage != Stage::IcmRead
        || m_operation->requestId != startedRequestId) {
        return;
    }
    if (result != StartResult::Started) {
        OperationResult operationResult = m_operation->result;
        operationResult.error = StartFailureText(result);
        finish(operationResult);
        return;
    }
    m_timeout.start(m_timeoutMs);
}

void DeviceOperationService::handleTimeout()
{
    if (!m_operation) {
        return;
    }
    if (m_operation->stage == Stage::IcmWrite) {
        m_operation->result.writeTimedOut = true;
        // MP10 proceeds with the diagnostic read even when the write operation
        // times out or returns an error. Preserve that behavior, with the exact
        // target and arm-state checks performed by beginIcmRead().
        beginIcmRead();
        return;
    }

    OperationResult result = m_operation->result;
    result.readTimedOut = true;
    finish(result);
}

void DeviceOperationService::finish(OperationResult result)
{
    m_timeout.stop();
    m_operation.reset();
    m_lastError = result.error;

    QPointer<DeviceOperationService> guard(this);
    emit stateChanged();
    if (guard) {
        emit operationFinished(result);
    }
}

void DeviceOperationService::invalidateBinding(const QString &reason)
{
    if (m_requiresRebind) {
        return;
    }
    m_requiresRebind = true;
    m_armState = ArmState::Unknown;
    m_lastError = reason;

    QPointer<DeviceOperationService> guard(this);
    if (m_operation) {
        OperationResult result = m_operation->result;
        result.cancelled = true;
        result.error = reason;
        finish(result);
    } else {
        emit stateChanged();
    }
    if (guard && m_requiresRebind) {
        emit bindingInvalidated(reason);
    }
}

void DeviceOperationService::updateArmState(
    int linkId, const mavlink_message_t &message)
{
    if (message.msgid != MAVLINK_MSG_ID_HEARTBEAT || linkId < 0) {
        return;
    }

    mavlink_heartbeat_t heartbeat{};
    mavlink_msg_heartbeat_decode(&message, &heartbeat);
    VehicleEndpoint endpoint;
    endpoint.linkId = linkId;
    endpoint.systemId = message.sysid;
    endpoint.componentId = message.compid;
    const ArmState state =
        (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED)
        ? ArmState::Armed : ArmState::Disarmed;
    m_armStates.insert(endpoint, state);

    if (!m_lease.isValid()
        || !m_lease.endpoint.sameIdentity(endpoint)
        || m_armState == state) {
        return;
    }
    m_armState = state;
    if (m_operation && m_operation->result.icm20948Test
        && state != ArmState::Disarmed) {
        OperationResult result = m_operation->result;
        result.cancelled = true;
        result.error = tr(
            "The ICM20948 test stopped because the exact bound vehicle armed.");
        finish(result);
    } else {
        emit stateChanged();
    }
}

DeviceOperationService::ArmState DeviceOperationService::knownArmState(
    const VehicleEndpoint &endpoint) const
{
    return m_armStates.value(endpoint, ArmState::Unknown);
}

void DeviceOperationService::forgetLink(int linkId)
{
    for (auto it = m_armStates.begin(); it != m_armStates.end();) {
        if (it.key().linkId == linkId) {
            it = m_armStates.erase(it);
        } else {
            ++it;
        }
    }
    if (m_lease.isValid() && m_lease.endpoint.linkId == linkId) {
        invalidateBinding(tr(
            "The bound DEVICE_OP link was removed. Rebind after reconnecting."));
    }
}

void DeviceOperationService::cancel()
{
    if (!m_operation) {
        return;
    }
    OperationResult result = m_operation->result;
    result.cancelled = true;
    result.error = tr("The DEVICE_OP operation was cancelled.");
    finish(result);
}

void DeviceOperationService::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    // Shutdown is silent just like destruction. The owner is tearing down and
    // must not receive a last callback into partially destroyed UI state.
    m_shuttingDown = true;
    m_timeout.stop();
    m_operation.reset();
    m_lease = VehicleTargetLease();
    m_armStates.clear();
    m_armState = ArmState::Unknown;
    m_requiresRebind = false;
    m_lastError.clear();
}

QString DeviceOperationService::ValidateRequest(const Request &request)
{
    if (request.destinationSystemId < 1
        || request.destinationSystemId > 255
        || request.destinationComponentId < 1
        || request.destinationComponentId > 255
        || request.bus < 0 || request.bus > 255
        || request.address < 0 || request.address > 255
        || request.registerStart < 0 || request.registerStart > 255
        || request.count < 1 || request.count > MaximumDataLength) {
        return tr("System/component IDs and bus values must fit in one byte; "
                  "count must be 1-128.");
    }
    if (request.busType != BusType::SPI
        && request.busType != BusType::I2C) {
        return tr("Select SPI or I2C bus type.");
    }
    if (request.busType == BusType::SPI) {
        const QByteArray busName = request.busName.trimmed().toUtf8();
        if (busName.isEmpty()) {
            return tr("Enter the ArduPilot SPI bus name.");
        }
        if (busName.size() > MaximumBusNameBytes) {
            return tr("The ArduPilot SPI bus name must fit in the 40-byte "
                      "DEVICE_OP field.");
        }
    }
    return QString();
}

quint32 DeviceOperationService::nextRequestId()
{
    return s_nextDeviceOperationRequestId.fetch_add(
        1, std::memory_order_relaxed);
}

QString DeviceOperationService::StartFailureText(StartResult result)
{
    switch (result) {
    case StartResult::Started:
        return QString();
    case StartResult::NotBound:
        return tr("Bind DEVICE_OP to the active target first.");
    case StartResult::StaleTarget:
        return tr("The bound DEVICE_OP target changed; use Active Target again.");
    case StartResult::Busy:
        return tr("A DEVICE_OP operation is already running.");
    case StartResult::InvalidRequest:
        return tr("The DEVICE_OP request is invalid.");
    case StartResult::UnsafeArmState:
        return tr("The ICM20948 test requires an exact confirmed disarmed target.");
    case StartResult::Mavlink1Unsupported:
        return tr("DEVICE_OP requires MAVLink 2; the selected link uses MAVLink 1.");
    case StartResult::TransportUnavailable:
        return tr("The selected link could not accept the DEVICE_OP frame.");
    case StartResult::ShuttingDown:
        return tr("The DEVICE_OP service is shutting down.");
    }
    return tr("DEVICE_OP could not start.");
}
