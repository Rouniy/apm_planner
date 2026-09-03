#include "DeviceOperationsViewModel.h"

#include <QStringList>

#include <utility>

DeviceOperationsViewModel::DeviceOperationsViewModel(
    DeviceOperationService *service, Dependencies dependencies,
    QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_dependencies(std::move(dependencies))
{
    if (m_service) {
        connect(m_service, &DeviceOperationService::stateChanged,
                this, &DeviceOperationsViewModel::changed);
        connect(m_service, &DeviceOperationService::operationFinished,
                this, &DeviceOperationsViewModel::operationFinished);
        connect(m_service, &DeviceOperationService::bindingInvalidated,
                this, [this](const QString &reason) {
            setOutput(reason.isEmpty()
                          ? tr("The active modem or vehicle changed. The old "
                               "DEVICE_OP result was discarded; choose Use "
                               "Active Target before another operation.")
                          : reason);
        });
    }
    bindActiveTarget(true);
}

DeviceOperationsViewModel::~DeviceOperationsViewModel()
{
    shutdown();
}

QStringList DeviceOperationsViewModel::busTypes() const
{
    return {QStringLiteral("SPI"), QStringLiteral("I2C")};
}

bool DeviceOperationsViewModel::isSpi() const
{
    return m_busType == QLatin1String("SPI");
}

bool DeviceOperationsViewModel::isBusy() const
{
    return m_service && m_service->isBusy();
}

bool DeviceOperationsViewModel::requiresTargetRebind() const
{
    return !m_service || m_service->requiresRebind();
}

bool DeviceOperationsViewModel::canOperate() const
{
    return m_service && m_service->isBound() && !m_service->isBusy()
        && !m_service->requiresRebind() && !m_shuttingDown;
}

bool DeviceOperationsViewModel::canTest() const
{
    // MP10's command silently returns when I2C is selected. Disabling the
    // destructive SPI-only test communicates that guard before it is clicked.
    return canOperate() && isSpi();
}

QString DeviceOperationsViewModel::FormatStatus(quint8 status)
{
    switch (status) {
    case 0:
        return QStringLiteral("result 0 (OK)");
    case 1:
        return QStringLiteral("result 1 (bad bus)");
    case 2:
        return QStringLiteral("result 2 (bad device)");
    case 3:
        return QStringLiteral("result 3 (semaphore unavailable)");
    case 4:
        return QStringLiteral("result 4 (bad response)");
    default:
        return QStringLiteral("result %1 (unknown)").arg(status);
    }
}

QString DeviceOperationsViewModel::FormatResult(
    quint8 status, quint8 registerStart, const QByteArray &data,
    bool timedOut)
{
    if (timedOut || (status == 0 && data.isEmpty())) {
        return QStringLiteral(
            "No DEVICE_OP reply data was received before the one-second "
            "upstream timeout.");
    }
    if (data.isEmpty()) {
        return QStringLiteral("DEVICE_OP failed with %1.")
            .arg(FormatStatus(status));
    }

    QStringList lines;
    lines.append(status == 0
                     ? QStringLiteral("DEVICE_OP succeeded: %1 byte(s).")
                           .arg(data.size())
                     : QStringLiteral("DEVICE_OP returned %1: %2 byte(s).")
                           .arg(FormatStatus(status))
                           .arg(data.size()));
    for (int offset = 0; offset < data.size(); offset += 16) {
        QStringList values;
        const int lineEnd = qMin(offset + 16, data.size());
        for (int index = offset; index < lineEnd; ++index) {
            values.append(QStringLiteral("%1").arg(
                static_cast<int>(static_cast<quint8>(data.at(index))), 2, 16,
                QLatin1Char('0')).toUpper());
        }
        lines.append(QStringLiteral("%1: %2")
                         .arg((static_cast<int>(registerStart) + offset) & 0xff,
                              2, 16, QLatin1Char('0'))
                         .arg(values.join(QLatin1Char(' ')))
                         .toUpper());
    }
    return lines.join(QLatin1Char('\n'));
}

void DeviceOperationsViewModel::setSystemId(int value)
{
    if (m_systemId != value) {
        m_systemId = value;
        emit changed();
    }
}

void DeviceOperationsViewModel::setComponentId(int value)
{
    if (m_componentId != value) {
        m_componentId = value;
        emit changed();
    }
}

void DeviceOperationsViewModel::setBusType(const QString &value)
{
    const QString normalized = value.trimmed().toUpper();
    if ((normalized == QLatin1String("SPI")
         || normalized == QLatin1String("I2C"))
        && m_busType != normalized) {
        m_busType = normalized;
        emit changed();
    }
}

void DeviceOperationsViewModel::setBusName(const QString &value)
{
    if (m_busName != value) {
        m_busName = value;
        emit changed();
    }
}

void DeviceOperationsViewModel::setBusNumber(int value)
{
    if (m_busNumber != value) {
        m_busNumber = value;
        emit changed();
    }
}

void DeviceOperationsViewModel::setAddress(int value)
{
    if (m_address != value) {
        m_address = value;
        emit changed();
    }
}

void DeviceOperationsViewModel::setRegisterStart(int value)
{
    if (m_registerStart != value) {
        m_registerStart = value;
        emit changed();
    }
}

void DeviceOperationsViewModel::setCount(int value)
{
    if (m_count != value) {
        m_count = value;
        emit changed();
    }
}

void DeviceOperationsViewModel::useActiveTarget()
{
    bindActiveTarget(false);
}

void DeviceOperationsViewModel::readRegisters()
{
    if (!m_service) {
        setOutput(tr("DEVICE_OP service is unavailable."));
        return;
    }
    const QString validationError = validate();
    if (!validationError.isEmpty()) {
        setOutput(validationError);
        return;
    }

    // A test transport, loopback link or reentrant writer can finish before
    // startRead() returns. Publish the pending state first so it can never
    // overwrite that synchronous completion result.
    setOutput(tr("Waiting for DEVICE_OP_READ_REPLY…"));
    const DeviceOperationService::StartResult result =
        m_service->startRead(request());
    if (result != DeviceOperationService::StartResult::Started) {
        applyStartFailure(result, tr("read"));
    }
}

void DeviceOperationsViewModel::testIcm20948()
{
    if (!m_service) {
        setOutput(tr("DEVICE_OP service is unavailable."));
        return;
    }
    const QString validationError = validate(true, true);
    if (!validationError.isEmpty()) {
        setOutput(validationError);
        return;
    }

    const QString title = tr("ICM20948 DEVICE_OP Test");
    const QString message = tr(
        "This reproduces Mission Planner's developer test on %1:%2: write "
        "72 00 to register FF of SPI device '%3', then read two bytes back. "
        "It can disrupt or damage incorrectly selected hardware. Continue "
        "only if this exact device is an ICM20948 and the vehicle is "
        "disarmed?")
                                .arg(m_systemId)
                                .arg(m_componentId)
                                .arg(m_busName.trimmed());
    if (!m_dependencies.confirm
        || !m_dependencies.confirm(title, message)) {
        return;
    }

    // A target change can run through a nested event loop while the warning
    // is visible. The service rechecks its exact lease and armed state too,
    // but update the UI with the validation failure before asking it to send.
    const QString postConfirmationError = validate(true, true);
    if (!postConfirmationError.isEmpty()) {
        setOutput(postConfirmationError);
        return;
    }

    // As with reads, make this assignment before the send in case a loopback
    // reply completes synchronously from the transmitter callback.
    setOutput(tr("Running the ICM20948 write/read test…"));
    const DeviceOperationService::StartResult result =
        m_service->startIcm20948Test(request());
    if (result != DeviceOperationService::StartResult::Started) {
        applyStartFailure(result, tr("ICM20948 write/read test"));
    }
}

void DeviceOperationsViewModel::cancel()
{
    if (m_service) {
        m_service->cancel();
    }
}

void DeviceOperationsViewModel::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    if (m_service) {
        m_service->shutdown();
    }
}

void DeviceOperationsViewModel::bindActiveTarget(bool initial)
{
    if (m_shuttingDown || !m_service || m_service->isBusy()) {
        return;
    }
    const VehicleTargetLease lease = m_dependencies.resolveTarget
        ? m_dependencies.resolveTarget() : VehicleTargetLease();
    if (!lease.isValid() || !m_service->bind(lease)) {
        m_service->unbind();
        if (!initial) {
            setOutput(tr("No active connected target. Connect a vehicle, "
                         "then choose Use Active Target."));
        } else {
            emit changed();
        }
        return;
    }

    m_systemId = lease.endpoint.systemId;
    m_componentId = lease.endpoint.componentId;
    if (!initial) {
        const QString link = lease.endpoint.linkName.isEmpty()
            ? tr("link %1").arg(lease.endpoint.linkId)
            : lease.endpoint.linkName;
        setOutput(tr("Bound DEVICE_OP to active target %1:%2 on %3.")
                      .arg(m_systemId)
                      .arg(m_componentId)
                      .arg(link));
    } else {
        emit changed();
    }
}

DeviceOperationService::Request DeviceOperationsViewModel::request() const
{
    DeviceOperationService::Request result;
    result.destinationSystemId = m_systemId;
    result.destinationComponentId = m_componentId;
    result.busType = isSpi() ? DeviceOperationService::BusType::SPI
                             : DeviceOperationService::BusType::I2C;
    result.busName = isSpi() ? m_busName.trimmed() : QString();
    result.bus = m_busNumber;
    result.address = m_address;
    result.registerStart = m_registerStart;
    result.count = m_count;
    return result;
}

QString DeviceOperationsViewModel::validate(
    bool requireSpi, bool requireDisarmed) const
{
    if (m_service && m_service->requiresRebind()) {
        return tr("The active modem or vehicle changed. Choose Use Active "
                  "Target before another operation.");
    }
    if (!m_service || !m_service->isBound()) {
        return tr("Connect a vehicle and choose Use Active Target before "
                  "using DEVICE_OP.");
    }
    if (m_service->isBusy()) {
        return tr("A DEVICE_OP operation is already running.");
    }
    if (requireSpi && !isSpi()) {
        return tr("The ICM20948 write/read test is available only for SPI.");
    }
    if (m_systemId < 1 || m_systemId > 255
        || m_componentId < 1 || m_componentId > 255
        || m_busNumber < 0 || m_busNumber > 255
        || m_address < 0 || m_address > 255
        || m_registerStart < 0 || m_registerStart > 255
        || m_count < 1 || m_count > 128) {
        return tr("System/component IDs and bus values must fit in one byte; "
                  "count must be 1–128.");
    }
    if (isSpi() && m_busName.trimmed().isEmpty()) {
        return tr("Enter the ArduPilot SPI bus name.");
    }
    if (isSpi()
        && m_busName.trimmed().toUtf8().size()
               > DeviceOperationService::MaximumBusNameBytes) {
        return tr("The ArduPilot SPI bus name must fit in the 40-byte "
                  "DEVICE_OP field.");
    }
    if (requireDisarmed) {
        const VehicleTargetLease lease = m_service->lease();
        if (m_systemId != lease.endpoint.systemId
            || m_componentId != lease.endpoint.componentId) {
            return tr("The ICM20948 test runs only against the bound target "
                      "%1:%2.")
                .arg(lease.endpoint.systemId)
                .arg(lease.endpoint.componentId);
        }
        if (m_service->armState() == DeviceOperationService::ArmState::Armed) {
            return tr("The ICM20948 write/read test is blocked while the "
                      "selected vehicle is armed.");
        }
        if (m_service->armState()
            != DeviceOperationService::ArmState::Disarmed) {
            return tr("The selected vehicle's armed state is unknown. Wait "
                      "for a HEARTBEAT before running the ICM20948 test.");
        }
    }

    return DeviceOperationService::ValidateRequest(request());
}

void DeviceOperationsViewModel::applyStartFailure(
    DeviceOperationService::StartResult result, const QString &operation)
{
    QString reason = m_service ? m_service->lastError() : QString();
    if (reason.isEmpty()) {
        switch (result) {
        case DeviceOperationService::StartResult::NotBound:
            reason = tr("choose Use Active Target first");
            break;
        case DeviceOperationService::StartResult::StaleTarget:
            reason = tr("the selected target changed");
            break;
        case DeviceOperationService::StartResult::Busy:
            reason = tr("another DEVICE_OP operation is already running");
            break;
        case DeviceOperationService::StartResult::InvalidRequest:
            reason = tr("the request is invalid");
            break;
        case DeviceOperationService::StartResult::UnsafeArmState:
            reason = tr("the vehicle is armed or its armed state is unknown");
            break;
        case DeviceOperationService::StartResult::Mavlink1Unsupported:
            reason = tr("DEVICE_OP requires MAVLink 2");
            break;
        case DeviceOperationService::StartResult::TransportUnavailable:
            reason = tr("the selected link rejected the request");
            break;
        case DeviceOperationService::StartResult::ShuttingDown:
            reason = tr("the window is closing");
            break;
        case DeviceOperationService::StartResult::Started:
            return;
        }
    }
    setOutput(tr("DEVICE_OP %1 could not start: %2.")
                  .arg(operation, reason));
}

void DeviceOperationsViewModel::operationFinished(
    const DeviceOperationService::OperationResult &result)
{
    if (m_shuttingDown) {
        return;
    }
    if (result.cancelled) {
        setOutput(tr("DEVICE_OP operation canceled."));
        return;
    }
    if (!result.error.isEmpty()) {
        setOutput(tr("DEVICE_OP failed: %1").arg(result.error));
        return;
    }

    const QString readText = FormatResult(
        result.readResult,
        static_cast<quint8>(result.request.registerStart),
        result.readData, result.readTimedOut);
    if (!result.icm20948Test) {
        setOutput(readText);
        return;
    }

    QString writeText;
    if (result.writeTimedOut || !result.writeReplyReceived) {
        writeText = tr("Write result: no DEVICE_OP_WRITE_REPLY was received "
                       "before the one-second upstream timeout.");
    } else {
        writeText = tr("Write result: %1")
                        .arg(FormatStatus(result.writeResult));
    }
    setOutput(writeText + QLatin1Char('\n') + readText);
}

void DeviceOperationsViewModel::setOutput(const QString &text)
{
    if (m_output == text) {
        emit changed();
        return;
    }
    m_output = text;
    emit changed();
}
