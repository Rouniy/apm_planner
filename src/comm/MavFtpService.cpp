#include "MavFtpService.h"

#include "VehicleTargetManager.h"

#include <QPointer>

#include <cstring>
#include <limits>

namespace {

QString startFailureText(MavFtpServiceInterface::StartResult result)
{
    using StartResult = MavFtpServiceInterface::StartResult;
    switch (result) {
    case StartResult::Started:
        return QString();
    case StartResult::Busy:
        return MavFtpService::tr("Another MAVFTP operation is already running.");
    case StartResult::NoTarget:
        return MavFtpService::tr("No current exact vehicle target is available.");
    case StartResult::StaleTarget:
        return MavFtpService::tr("The selected vehicle changed before the operation started.");
    case StartResult::InvalidPath:
        return MavFtpService::tr("The remote path is empty or cannot be encoded safely.");
    case StartResult::InvalidData:
        return MavFtpService::tr("The transfer data exceeds the safe in-memory limit.");
    case StartResult::TransportUnavailable:
        return MavFtpService::tr("The exact-link transmitter is unavailable.");
    case StartResult::ShuttingDown:
        return MavFtpService::tr("The MAVFTP service is shutting down.");
    }
    return MavFtpService::tr("The MAVFTP operation could not be started.");
}

quint32 decodeLittleEndian32(const QByteArray &bytes)
{
    return static_cast<quint8>(bytes.at(0))
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(1))) << 8)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(2))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(3))) << 24);
}

QString appendCleanupError(const QString &terminalError,
                           const QString &cleanupError)
{
    if (cleanupError.isEmpty()) {
        return terminalError;
    }
    if (terminalError.isEmpty()) {
        return MavFtpService::tr("MAVFTP cleanup failed: %1")
            .arg(cleanupError);
    }
    return terminalError + MavFtpService::tr(" Cleanup also failed: %1")
        .arg(cleanupError);
}

} // namespace

MavFtpService::MavFtpService(
    VehicleTargetManager *targetManager,
    ExactLinkTransmitter *transmitter,
    QObject *parent)
    : MavFtpServiceInterface(parent)
    , m_targetManager(targetManager)
    , m_transmitter(transmitter)
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_transmitter);
    qRegisterMetaType<MavFtpServiceInterface::Operation>(
        "MavFtpServiceInterface::Operation");
    qRegisterMetaType<MavFtpServiceInterface::Result>(
        "MavFtpServiceInterface::Result");

    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout,
            this, &MavFtpService::handleTimeout);
    if (m_targetManager) {
        connect(m_targetManager,
                &VehicleTargetManager::targetGenerationChanged,
                this, &MavFtpService::handleTargetGenerationChanged);
        connect(m_targetManager, &VehicleTargetManager::targetGenerationSettled,
                this, [this](qulonglong) {
            if (!m_shuttingDown) emit targetChanged();
        });
        connect(m_targetManager, &QObject::destroyed, this, [this] {
            m_targetManager = nullptr;
            if (m_shuttingDown) return;
            QPointer<MavFtpService> guard(this);
            emit targetChanged();
            if (!guard || !m_active) return;
            Result result = m_active->result;
            result.cancelled = true;
            result.error = tr("The MAVFTP target manager is unavailable.");
            finishImmediate(result);
        });
    }
}

MavFtpService::~MavFtpService()
{
    shutdownInternal(false);
}

void MavFtpService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    // The destination fields of every response are correlated against this
    // identity, so it is immutable for the duration of an operation.
    if (isBusy()) {
        return;
    }
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

void MavFtpService::setTimingForTesting(
    int timeoutMs, int maximumRetries,
    int openCreateTimeoutMs, int crcTimeoutMs)
{
    if (isBusy()) {
        return;
    }
    m_timeoutMs = qMax(1, timeoutMs);
    m_openCreateTimeoutMs = openCreateTimeoutMs > 0
        ? openCreateTimeoutMs : m_timeoutMs;
    m_crcTimeoutMs = crcTimeoutMs > 0
        ? crcTimeoutMs : m_timeoutMs;
    m_maximumRetries = qMax(0, maximumRetries);
}

MavFtpService::Operation MavFtpService::operation() const
{
    return m_active ? m_active->result.operation : Operation::None;
}

quint64 MavFtpService::activeTargetGeneration() const
{
    return m_active ? m_active->lease.generation : 0;
}

quint64 MavFtpService::activeOperationId() const
{
    return m_active ? m_active->identity : 0;
}

VehicleTargetLease MavFtpService::currentTargetLease() const
{
    if (!m_targetManager) return {};
    VehicleTargetLease lease = m_targetManager->acquireTarget();
    if (m_shuttingDown || !m_transmitter || !targetIsCurrent(lease))
        lease.endpoint = VehicleEndpoint();
    return lease;
}

MavFtpService::StartResult MavFtpService::validateStart(
    const QString &remotePath, QByteArray *encodedPath) const
{
    if (m_shuttingDown) {
        return StartResult::ShuttingDown;
    }
    if (isBusy()) {
        return StartResult::Busy;
    }
    if (!m_targetManager || !m_transmitter) {
        return StartResult::TransportUnavailable;
    }
    if (remotePath.isEmpty()) {
        return StartResult::InvalidPath;
    }
    QString error;
    if (!MavFtpProtocol::encodePath(remotePath, encodedPath, &error)) {
        return StartResult::InvalidPath;
    }
    const VehicleTargetLease lease = m_targetManager->acquireTarget();
    if (!lease.isValid()) {
        return StartResult::NoTarget;
    }
    if (!targetIsCurrent(lease)) {
        return StartResult::StaleTarget;
    }
    return StartResult::Started;
}

MavFtpService::StartResult MavFtpService::startList(
    const QString &remotePath)
{
    QByteArray path;
    const StartResult ready = validateStart(remotePath, &path);
    if (ready != StartResult::Started) {
        m_lastError = startFailureText(ready);
        return ready;
    }
    return begin(Operation::ListDirectory, remotePath, path);
}

MavFtpService::StartResult MavFtpService::startDownload(
    const QString &remotePath)
{
    QByteArray path;
    const StartResult ready = validateStart(remotePath, &path);
    if (ready != StartResult::Started) {
        m_lastError = startFailureText(ready);
        return ready;
    }
    return begin(Operation::Download, remotePath, path);
}

MavFtpService::StartResult MavFtpService::startOperation(
    Operation operation, const QString &remotePath, const QByteArray &uploadData,
    quint64 *operationIdOut)
{
    return startOwnedOperation(operation, remotePath, uploadData, nullptr, operationIdOut);
}

MavFtpService::StartResult MavFtpService::startOperationForTarget(
    Operation operation, const QString &remotePath, const QByteArray &uploadData,
    const VehicleTargetLease &expected, quint64 *operationIdOut)
{
    return startOwnedOperation(operation, remotePath, uploadData, &expected, operationIdOut);
}

MavFtpService::StartResult MavFtpService::startOwnedOperation(
    Operation operation, const QString &remotePath, const QByteArray &uploadData,
    const VehicleTargetLease *expected, quint64 *operationIdOut)
{
    if (operationIdOut) *operationIdOut = 0;
    QByteArray path;
    const StartResult ready = validateStart(remotePath, &path);
    if (ready != StartResult::Started) {
        m_lastError = startFailureText(ready);
        return ready;
    }
    switch (operation) {
    case Operation::ListDirectory: case Operation::Download:
    case Operation::MakeDirectory: case Operation::RemoveFile:
    case Operation::RemoveDirectory:
        if (uploadData.isEmpty()) break;
        m_lastError = startFailureText(StartResult::InvalidData);
        return StartResult::InvalidData;
    case Operation::Upload:
        if (uploadData.size() <= MaximumTransferBytes) break;
        m_lastError = startFailureText(StartResult::InvalidData);
        return StartResult::InvalidData;
    default:
        m_lastError = startFailureText(StartResult::InvalidData);
        return StartResult::InvalidData;
    }
    return begin(operation, remotePath, path, uploadData, expected, operationIdOut);
}

MavFtpService::StartResult MavFtpService::startDownloadForTarget(
    const QString &remotePath, const VehicleTargetLease &expected,
    quint64 *operationIdOut)
{
    return startOwnedOperation(Operation::Download, remotePath, {}, &expected, operationIdOut);
}

MavFtpService::StartResult MavFtpService::startUpload(
    const QString &remotePath, const QByteArray &data)
{
    QByteArray path;
    const StartResult ready = validateStart(remotePath, &path);
    if (ready != StartResult::Started) {
        m_lastError = startFailureText(ready);
        return ready;
    }
    if (data.size() > MaximumTransferBytes) {
        m_lastError = startFailureText(StartResult::InvalidData);
        return StartResult::InvalidData;
    }
    return begin(Operation::Upload, remotePath, path, data);
}

MavFtpService::StartResult MavFtpService::startMakeDirectory(
    const QString &remotePath)
{
    QByteArray path;
    const StartResult ready = validateStart(remotePath, &path);
    if (ready != StartResult::Started) {
        m_lastError = startFailureText(ready);
        return ready;
    }
    return begin(Operation::MakeDirectory, remotePath, path);
}

MavFtpService::StartResult MavFtpService::startRemoveFile(
    const QString &remotePath)
{
    QByteArray path;
    const StartResult ready = validateStart(remotePath, &path);
    if (ready != StartResult::Started) {
        m_lastError = startFailureText(ready);
        return ready;
    }
    return begin(Operation::RemoveFile, remotePath, path);
}

MavFtpService::StartResult MavFtpService::startRemoveDirectory(
    const QString &remotePath)
{
    QByteArray path;
    const StartResult ready = validateStart(remotePath, &path);
    if (ready != StartResult::Started) {
        m_lastError = startFailureText(ready);
        return ready;
    }
    return begin(Operation::RemoveDirectory, remotePath, path);
}

MavFtpService::StartResult MavFtpService::begin(
    Operation operation, const QString &remotePath,
    const QByteArray &encodedPath, const QByteArray &uploadData,
    const VehicleTargetLease *expected, quint64 *operationIdOut)
{
    if (m_shuttingDown) {
        m_lastError = startFailureText(StartResult::ShuttingDown);
        return StartResult::ShuttingDown;
    }
    if (isBusy()) {
        m_lastError = startFailureText(StartResult::Busy);
        return StartResult::Busy;
    }

    const VehicleTargetLease lease = m_targetManager->acquireTarget();
    if (!lease.isValid()) {
        m_lastError = startFailureText(StartResult::NoTarget);
        return StartResult::NoTarget;
    }
    if (!targetIsCurrent(lease)
        || (expected && (!expected->isValid()
                         || expected->generation != lease.generation
                         || expected->endpoint != lease.endpoint))) {
        m_lastError = startFailureText(StartResult::StaleTarget);
        return StartResult::StaleTarget;
    }

    auto active = std::make_unique<ActiveOperation>();
    active->identity = ++m_nextOperationIdentity;
    if (active->identity == 0) {
        active->identity = ++m_nextOperationIdentity;
    }
    active->result.operation = operation;
    active->result.operationId = active->identity;
    active->result.targetGeneration = lease.generation;
    active->result.remotePath = remotePath;
    active->lease = lease;
    active->encodedPath = encodedPath;
    active->uploadData = uploadData;
    if (operation == Operation::Upload) {
        active->result.localCrc = MavFtpProtocol::crc32(uploadData);
    }
    switch (operation) {
    case Operation::ListDirectory:
        active->stage = Stage::ListPage;
        break;
    case Operation::Download:
        active->stage = Stage::ResetBeforeDownload;
        break;
    case Operation::Upload:
        active->stage = Stage::ResetBeforeUpload;
        break;
    case Operation::MakeDirectory:
    case Operation::RemoveFile:
    case Operation::RemoveDirectory:
        active->stage = Stage::Simple;
        break;
    case Operation::None:
        m_lastError = tr("The MAVFTP operation is invalid.");
        return StartResult::InvalidData;
    }

    m_active = std::move(active);
    const quint64 startedIdentity = m_active->identity;
    if (operationIdOut) *operationIdOut = startedIdentity;
    m_lastError.clear();
    QPointer<MavFtpService> guard(this);
    emit stateChanged();
    if (!guard || !m_active || m_active->identity != startedIdentity) {
        return StartResult::Started;
    }
    emit operationStarted(operation, lease.generation, remotePath);
    if (!guard || !m_active || m_active->identity != startedIdentity) {
        return StartResult::Started;
    }

    switch (operation) {
    case Operation::ListDirectory:
        issueListPage();
        break;
    case Operation::Download:
        issue(request(MavFtpProtocol::Opcode::ResetSessions),
              tr("reset sessions before download"));
        break;
    case Operation::Upload:
        if (!publishProgress(0, uploadData.size())) return StartResult::Started;
        if (!guard || !m_active || m_active->identity != startedIdentity) {
            return StartResult::Started;
        }
        issue(request(MavFtpProtocol::Opcode::ResetSessions),
              tr("reset sessions before upload"));
        break;
    case Operation::MakeDirectory:
        issue(request(MavFtpProtocol::Opcode::CreateDirectory,
                      0, 0, encodedPath),
              tr("create directory"));
        break;
    case Operation::RemoveFile:
        issue(request(MavFtpProtocol::Opcode::RemoveFile,
                      0, 0, encodedPath),
              tr("remove file"));
        break;
    case Operation::RemoveDirectory:
        issue(request(MavFtpProtocol::Opcode::RemoveDirectory,
                      0, 0, encodedPath),
              tr("remove directory"));
        break;
    case Operation::None:
        break;
    }
    return StartResult::Started;
}

bool MavFtpService::targetIsCurrent(
    const VehicleTargetLease &lease) const
{
    return m_targetManager && lease.isValid()
        && m_targetManager->isTargetGenerationSettled()
        && m_targetManager->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation);
}

bool MavFtpService::responseSourceMatches(
    int linkId, const mavlink_message_t &message,
    const mavlink_file_transfer_protocol_t &outer) const
{
    if (!m_active || !targetIsCurrent(m_active->lease)) {
        return false;
    }
    return linkId == m_active->lease.endpoint.linkId
        && message.sysid == m_active->lease.endpoint.systemId
        && message.compid == m_active->lease.endpoint.componentId
        && outer.target_network == 0
        && outer.target_system == m_localSystemId
        && outer.target_component == m_localComponentId;
}

MavFtpProtocol::PayloadHeader MavFtpService::request(
    MavFtpProtocol::Opcode opcode, quint8 session,
    quint32 offset, const QByteArray &data, int sizeOverride)
{
    MavFtpProtocol::PayloadHeader header;
    header.sequence = m_nextSequence++;
    header.session = session;
    header.opcode = opcode;
    header.offset = offset;
    header.data = data;
    header.size = static_cast<quint8>(
        sizeOverride >= 0 ? sizeOverride : data.size());
    return header;
}

MavFtpService::DispatchResult MavFtpService::dispatch(
    const MavFtpProtocol::PayloadHeader &ftpRequest)
{
    if (!m_active) {
        return DispatchResult::Superseded;
    }
    if (!targetIsCurrent(m_active->lease)) {
        return DispatchResult::StaleTarget;
    }
    QString codecError;
    if (MavFtpProtocol::encodePayload(ftpRequest, &codecError).isEmpty()) {
        m_lastError = codecError;
        return DispatchResult::EncodingFailure;
    }

    m_request = ftpRequest;
    m_attempts = 0;
    ++m_requestToken;
    return resend();
}

MavFtpService::DispatchResult MavFtpService::resend()
{
    if (!m_active) {
        return DispatchResult::Superseded;
    }
    if (!targetIsCurrent(m_active->lease)) {
        return DispatchResult::StaleTarget;
    }

    const quint64 token = m_requestToken;
    const quint64 activeIdentity = m_active->identity;
    const VehicleTargetLease lease = m_active->lease;
    const MavFtpProtocol::PayloadHeader ftpRequest = m_request;
    ++m_attempts;
    QString error;
    QPointer<MavFtpService> guard(this);
    const ExactLinkTransmitter::SendResult sent =
        sendForLease(lease, ftpRequest, &error);
    if (!guard || !m_active || m_active->identity != activeIdentity
        || m_requestToken != token) {
        return DispatchResult::Superseded;
    }
    if (sent != ExactLinkTransmitter::SendResult::Sent) {
        m_lastError = error;
        return DispatchResult::TransportFailure;
    }
    m_timer.start(timeoutForRequest(ftpRequest));
    return DispatchResult::Sent;
}

ExactLinkTransmitter::SendResult MavFtpService::sendForLease(
    const VehicleTargetLease &lease,
    const MavFtpProtocol::PayloadHeader &ftpRequest,
    QString *error)
{
    if (error) {
        error->clear();
    }
    if (!targetIsCurrent(lease)) {
        if (error) {
            *error = tr("the selected target is no longer current");
        }
        return ExactLinkTransmitter::SendResult::InvalidLink;
    }

    QString codecError;
    const QByteArray wire =
        MavFtpProtocol::encodePayload(ftpRequest, &codecError);
    if (wire.size() != MavFtpProtocol::PayloadSize) {
        if (error) {
            *error = codecError;
        }
        return ExactLinkTransmitter::SendResult::InvalidMessage;
    }

    quint8 payload[MavFtpProtocol::PayloadSize]{};
    std::memcpy(payload, wire.constData(), sizeof(payload));
    mavlink_message_t message{};
    mavlink_msg_file_transfer_protocol_pack(
        m_localSystemId, m_localComponentId, &message, 0,
        static_cast<quint8>(lease.endpoint.systemId),
        static_cast<quint8>(lease.endpoint.componentId), payload);
    QPointer<MavFtpService> guard(this);
    const ExactLinkTransmitter::SendResult result =
        m_transmitter
        ? m_transmitter->sendMessage(
            lease.endpoint.linkId, m_localSystemId,
            m_localComponentId, message)
        : ExactLinkTransmitter::SendResult::TransportUnavailable;
    if (!guard) {
        return result;
    }
    if (result == ExactLinkTransmitter::SendResult::Sent || !error) {
        return result;
    }
    switch (result) {
    case ExactLinkTransmitter::SendResult::InvalidLink:
        *error = tr("the target link is invalid");
        break;
    case ExactLinkTransmitter::SendResult::InvalidMessage:
        *error = tr("FILE_TRANSFER_PROTOCOL is unavailable");
        break;
    case ExactLinkTransmitter::SendResult::IncompatibleVersion:
        *error = tr("the target link cannot carry FILE_TRANSFER_PROTOCOL");
        break;
    case ExactLinkTransmitter::SendResult::TransportUnavailable:
        *error = tr("the target link rejected the MAVFTP frame");
        break;
    case ExactLinkTransmitter::SendResult::Sent:
        break;
    }
    return result;
}

bool MavFtpService::issue(
    const MavFtpProtocol::PayloadHeader &ftpRequest,
    const QString &context)
{
    const DispatchResult sent = dispatch(ftpRequest);
    if (sent == DispatchResult::Sent
        || sent == DispatchResult::Superseded) {
        return true;
    }
    const QString error = dispatchErrorText(sent, context);
    if (m_active && (m_active->stage == Stage::TerminateCleanup
                     || m_active->stage == Stage::ResetCleanup)) {
        advanceCleanup(error);
    } else {
        finishFailure(error, sent == DispatchResult::StaleTarget);
    }
    return false;
}

void MavFtpService::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (m_shuttingDown || !m_active
        || message.msgid != MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL) {
        return;
    }
    if (!targetIsCurrent(m_active->lease)) {
        finishFailure(tr("The selected vehicle changed; the MAVFTP reply was discarded."),
                      true);
        return;
    }

    mavlink_file_transfer_protocol_t outer{};
    mavlink_msg_file_transfer_protocol_decode(&message, &outer);
    if (!responseSourceMatches(linkId, message, outer)) {
        return;
    }

    const QByteArray wire(
        reinterpret_cast<const char *>(outer.payload),
        MavFtpProtocol::PayloadSize);
    const quint16 responseSequence =
        static_cast<quint8>(wire.at(0))
        | (static_cast<quint16>(static_cast<quint8>(wire.at(1))) << 8);
    const auto responseRequestOpcode = static_cast<MavFtpProtocol::Opcode>(
        static_cast<quint8>(wire.at(5)));
    if (responseRequestOpcode != m_request.opcode
        || responseSequence
            != static_cast<quint16>(m_request.sequence + 1u)) {
        return;
    }

    MavFtpProtocol::PayloadHeader response;
    QString error;
    if (!MavFtpProtocol::decodePayload(wire, &response, &error)) {
        finishFailure(tr("Malformed MAVFTP response: %1").arg(error));
        return;
    }

    if (!MavFtpProtocol::validateResponse(m_request, response, &error)) {
        finishFailure(tr("Malformed MAVFTP response: %1").arg(error));
        return;
    }
    if (stageRequiresSession(m_active->stage)
        && response.session != m_request.session) {
        return;
    }
    if (response.opcode == MavFtpProtocol::Opcode::Ack
        && stageRequiresOffset(m_active->stage)
        && response.offset != m_request.offset) {
        return;
    }

    m_timer.stop();
    m_attempts = 0;
    handleResponse(response);
}

void MavFtpService::handleResponse(
    const MavFtpProtocol::PayloadHeader &response)
{
    if (!m_active) {
        return;
    }
    if (response.opcode == MavFtpProtocol::Opcode::Nak) {
        handleNak(response);
    } else {
        handleAck(response);
    }
}

void MavFtpService::handleAck(
    const MavFtpProtocol::PayloadHeader &response)
{
    if (!m_active) {
        return;
    }
    switch (m_active->stage) {
    case Stage::ListPage:
        handleListAck(response);
        break;
    case Stage::ResetBeforeDownload:
        issueDownloadOpen();
        break;
    case Stage::OpenDownload:
        handleOpenDownloadAck(response);
        break;
    case Stage::ReadDownload:
        handleReadDownloadAck(response);
        break;
    case Stage::ResetBeforeUpload:
        issueUploadCreate();
        break;
    case Stage::CreateUpload:
        handleCreateUploadAck(response);
        break;
    case Stage::WriteUpload:
        handleWriteUploadAck(response);
        break;
    case Stage::TerminateBeforeVerify:
        m_active->sessionOpen = false;
        issueUploadResetBeforeVerify();
        break;
    case Stage::ResetBeforeVerify:
        issueUploadVerify();
        break;
    case Stage::VerifyUpload:
        handleVerifyUploadAck(response);
        break;
    case Stage::Simple:
        finishSuccess();
        break;
    case Stage::TerminateCleanup:
    case Stage::ResetCleanup:
        advanceCleanup();
        break;
    case Stage::None:
        break;
    }
}

void MavFtpService::handleNak(
    const MavFtpProtocol::PayloadHeader &response)
{
    if (!m_active) {
        return;
    }
    const auto errorCode = static_cast<MavFtpProtocol::ErrorCode>(
        static_cast<quint8>(response.data.at(0)));
    if (m_active->stage == Stage::ListPage
        && errorCode == MavFtpProtocol::ErrorCode::EndOfFile) {
        finishSuccess();
        return;
    }
    if (m_active->stage == Stage::ReadDownload
        && errorCode == MavFtpProtocol::ErrorCode::EndOfFile
        && static_cast<quint32>(m_active->result.data.size())
            == m_active->expectedSize) {
        beginCleanup();
        return;
    }
    const QString error = remoteErrorText(response);
    if (m_active->stage == Stage::TerminateCleanup
        || m_active->stage == Stage::ResetCleanup) {
        advanceCleanup(error);
        return;
    }
    finishFailure(error);
}

void MavFtpService::handleListAck(
    const MavFtpProtocol::PayloadHeader &response)
{
    if (!m_active) {
        return;
    }
    QVector<DirectoryEntry> page;
    QString error;
    if (!MavFtpProtocol::parseDirectoryEntries(
            response.data, response.size, &page, &error)) {
        finishFailure(tr("Malformed directory response at offset %1: %2")
                          .arg(m_active->directoryRecordOffset)
                          .arg(error));
        return;
    }
    if (page.isEmpty()) {
        finishSuccess();
        return;
    }
    if (m_active->directoryRecordOffset
            > std::numeric_limits<int>::max() - page.size()
        || m_active->directoryRecordOffset + page.size()
            > MaximumDirectoryEntries) {
        finishFailure(tr("The remote directory exceeds the safe entry limit."));
        return;
    }

    m_active->directoryRecordOffset += page.size();
    for (const DirectoryEntry &entry : page) {
        if (entry.type != MavFtpProtocol::DirectoryEntryType::Skip) {
            m_active->result.entries.append(entry);
        }
    }
    const quint64 activeIdentity = m_active->identity;
    const int records = m_active->directoryRecordOffset;
    QPointer<MavFtpService> guard(this);
    if (!publishProgress(records, -1)) return;
    if (!guard || !m_active || m_active->identity != activeIdentity) {
        return;
    }
    issueListPage();
}

void MavFtpService::handleOpenDownloadAck(
    const MavFtpProtocol::PayloadHeader &response)
{
    if (!m_active) {
        return;
    }
    m_active->session = response.session;
    m_active->sessionOpen = true;
    if (response.data.size() < 4) {
        finishFailure(tr("The MAVFTP open reply has no 32-bit file size."));
        return;
    }
    m_active->expectedSize = decodeLittleEndian32(response.data);
    if (m_active->expectedSize
        > static_cast<quint32>(MaximumTransferBytes)) {
        finishFailure(tr("The remote file exceeds the %1 MiB transfer limit.")
                          .arg(MaximumTransferBytes / (1024 * 1024)));
        return;
    }

    const quint64 activeIdentity = m_active->identity;
    const quint32 total = m_active->expectedSize;
    QPointer<MavFtpService> guard(this);
    if (!publishProgress(0, total)) return;
    if (!guard || !m_active || m_active->identity != activeIdentity) {
        return;
    }
    if (total == 0) {
        beginCleanup();
    } else {
        issueDownloadRead();
    }
}

void MavFtpService::handleReadDownloadAck(
    const MavFtpProtocol::PayloadHeader &response)
{
    if (!m_active) {
        return;
    }
    const quint32 received = static_cast<quint32>(response.data.size());
    const quint32 requested = m_request.size;
    if (received == 0 || received > requested
        || static_cast<quint64>(m_active->offset) + received
            > m_active->expectedSize) {
        finishFailure(tr("The MAVFTP read reply has an invalid chunk size."));
        return;
    }

    m_active->result.data.append(response.data);
    m_active->offset += received;
    const quint64 activeIdentity = m_active->identity;
    const quint32 completed = m_active->offset;
    const quint32 total = m_active->expectedSize;
    QPointer<MavFtpService> guard(this);
    if (!publishProgress(completed, total)) return;
    if (!guard || !m_active || m_active->identity != activeIdentity) {
        return;
    }
    if (completed == total) {
        beginCleanup();
    } else {
        issueDownloadRead();
    }
}

void MavFtpService::handleCreateUploadAck(
    const MavFtpProtocol::PayloadHeader &response)
{
    if (!m_active) {
        return;
    }
    m_active->session = response.session;
    m_active->sessionOpen = true;
    m_active->offset = 0;
    issueUploadWriteOrVerify();
}

void MavFtpService::handleWriteUploadAck(
    const MavFtpProtocol::PayloadHeader &)
{
    if (!m_active) {
        return;
    }
    const int acknowledged = m_request.data.size();
    if (acknowledged <= 0
        || static_cast<quint64>(m_active->offset)
               + static_cast<quint64>(acknowledged)
            > static_cast<quint64>(m_active->uploadData.size())) {
        finishFailure(tr("The MAVFTP write acknowledgement is invalid."));
        return;
    }
    m_active->offset += static_cast<quint32>(acknowledged);
    const quint64 activeIdentity = m_active->identity;
    const quint32 completed = m_active->offset;
    const int total = m_active->uploadData.size();
    QPointer<MavFtpService> guard(this);
    if (!publishProgress(completed, total)) return;
    if (!guard || !m_active || m_active->identity != activeIdentity) {
        return;
    }
    issueUploadWriteOrVerify();
}

void MavFtpService::handleVerifyUploadAck(
    const MavFtpProtocol::PayloadHeader &response)
{
    if (!m_active) {
        return;
    }
    if (response.data.size() != 4) {
        finishFailure(tr("The MAVFTP CRC reply is not four bytes."));
        return;
    }
    m_active->result.remoteCrc = decodeLittleEndian32(response.data);
    if (m_active->result.remoteCrc != m_active->result.localCrc) {
        Result result = m_active->result;
        result.error = tr("Upload CRC mismatch: local 0x%1, remote 0x%2.")
            .arg(result.localCrc, 8, 16, QLatin1Char('0'))
            .arg(result.remoteCrc, 8, 16, QLatin1Char('0'));
        finishImmediate(result);
        return;
    }
    finishImmediate(m_active->result);
}

void MavFtpService::issueListPage()
{
    if (!m_active) {
        return;
    }
    m_active->stage = Stage::ListPage;
    issue(request(MavFtpProtocol::Opcode::ListDirectory, 0,
                  static_cast<quint32>(m_active->directoryRecordOffset),
                  m_active->encodedPath),
          tr("list directory"));
}

void MavFtpService::issueDownloadOpen()
{
    if (!m_active) {
        return;
    }
    m_active->stage = Stage::OpenDownload;
    issue(request(MavFtpProtocol::Opcode::OpenFileReadOnly,
                  0, 0, m_active->encodedPath),
          tr("open remote file"));
}

void MavFtpService::issueDownloadRead()
{
    if (!m_active || m_active->offset >= m_active->expectedSize) {
        return;
    }
    const quint32 remaining = m_active->expectedSize - m_active->offset;
    const int requested = static_cast<int>(
        qMin<quint32>(TransferChunkSize, remaining));
    m_active->stage = Stage::ReadDownload;
    issue(request(MavFtpProtocol::Opcode::ReadFile,
                  m_active->session, m_active->offset,
                  QByteArray(), requested),
          tr("read remote file"));
}

void MavFtpService::issueUploadCreate()
{
    if (!m_active) {
        return;
    }
    m_active->stage = Stage::CreateUpload;
    issue(request(MavFtpProtocol::Opcode::CreateFile,
                  0, 0, m_active->encodedPath),
          tr("create remote file"));
}

void MavFtpService::issueUploadWriteOrVerify()
{
    if (!m_active) {
        return;
    }
    if (m_active->offset
        >= static_cast<quint32>(m_active->uploadData.size())) {
        issueUploadFinalize();
        return;
    }
    const QByteArray chunk = m_active->uploadData.mid(
        static_cast<int>(m_active->offset), TransferChunkSize);
    m_active->stage = Stage::WriteUpload;
    issue(request(MavFtpProtocol::Opcode::WriteFile,
                  m_active->session, m_active->offset, chunk),
          tr("write remote file"));
}

void MavFtpService::issueUploadFinalize()
{
    if (!m_active) {
        return;
    }
    if (m_active->sessionOpen) {
        m_active->stage = Stage::TerminateBeforeVerify;
        issue(request(MavFtpProtocol::Opcode::TerminateSession,
                      m_active->session),
              tr("close uploaded file before CRC verification"));
        return;
    }
    issueUploadResetBeforeVerify();
}

void MavFtpService::issueUploadResetBeforeVerify()
{
    if (!m_active) {
        return;
    }
    m_active->stage = Stage::ResetBeforeVerify;
    issue(request(MavFtpProtocol::Opcode::ResetSessions),
          tr("flush MAVFTP sessions before CRC verification"));
}

void MavFtpService::issueUploadVerify()
{
    if (!m_active) {
        return;
    }
    m_active->stage = Stage::VerifyUpload;
    issue(request(MavFtpProtocol::Opcode::CalculateFileCrc32,
                  0, 0, m_active->encodedPath),
          tr("verify remote file CRC"));
}

void MavFtpService::beginCleanup(
    const QString &terminalError, bool cancelled)
{
    if (!m_active) {
        return;
    }
    m_active->terminalError = terminalError;
    m_active->terminalCancelled = cancelled;
    if (!targetIsCurrent(m_active->lease)) {
        Result result = m_active->result;
        result.error = terminalError.isEmpty()
            ? tr("The selected vehicle changed before MAVFTP cleanup.")
            : terminalError;
        result.cancelled = cancelled;
        finishImmediate(result);
        return;
    }
    if (m_active->sessionOpen) {
        m_active->stage = Stage::TerminateCleanup;
        issue(request(MavFtpProtocol::Opcode::TerminateSession,
                      m_active->session),
              tr("terminate MAVFTP session"));
    } else {
        m_active->stage = Stage::ResetCleanup;
        issue(request(MavFtpProtocol::Opcode::ResetSessions),
              tr("reset MAVFTP sessions"));
    }
}

void MavFtpService::advanceCleanup(const QString &cleanupError)
{
    if (!m_active) {
        return;
    }
    m_active->terminalError = appendCleanupError(
        m_active->terminalError, cleanupError);
    if (m_active->stage == Stage::TerminateCleanup) {
        m_active->sessionOpen = false;
        m_active->stage = Stage::ResetCleanup;
        issue(request(MavFtpProtocol::Opcode::ResetSessions),
              tr("reset MAVFTP sessions"));
        return;
    }

    Result result = m_active->result;
    result.error = m_active->terminalError;
    result.cancelled = m_active->terminalCancelled;
    finishImmediate(result);
}

void MavFtpService::sendCleanupBestEffort(
    const ActiveOperation &active)
{
    if (!targetIsCurrent(active.lease)) {
        return;
    }
    QString ignored;
    const quint64 lastOperationIdentity = m_nextOperationIdentity;
    QPointer<MavFtpService> guard(this);
    if (active.sessionOpen) {
        MavFtpProtocol::PayloadHeader terminate;
        terminate.sequence = m_nextSequence++;
        terminate.session = active.session;
        terminate.opcode = MavFtpProtocol::Opcode::TerminateSession;
        sendForLease(active.lease, terminate, &ignored);
        if (!guard || m_nextOperationIdentity != lastOperationIdentity) {
            return;
        }
    }
    MavFtpProtocol::PayloadHeader reset;
    reset.sequence = m_nextSequence++;
    reset.opcode = MavFtpProtocol::Opcode::ResetSessions;
    sendForLease(active.lease, reset, &ignored);
}

void MavFtpService::handleTimeout()
{
    if (!m_active) {
        return;
    }
    if (!targetIsCurrent(m_active->lease)) {
        finishFailure(tr("The selected vehicle changed during the MAVFTP operation."),
                      true);
        return;
    }
    if (m_attempts <= m_maximumRetries) {
        const DispatchResult sent = resend();
        if (sent == DispatchResult::Sent
            || sent == DispatchResult::Superseded) {
            return;
        }
        const QString error = dispatchErrorText(sent, tr("retry MAVFTP request"));
        if (m_active && (m_active->stage == Stage::TerminateCleanup
                         || m_active->stage == Stage::ResetCleanup)) {
            advanceCleanup(error);
        } else {
            finishFailure(error, sent == DispatchResult::StaleTarget);
        }
        return;
    }

    const QString error = tr("MAVFTP request timed out after %1 attempts.")
        .arg(m_attempts);
    if (m_active->stage == Stage::TerminateCleanup
        || m_active->stage == Stage::ResetCleanup) {
        advanceCleanup(error);
    } else {
        finishFailure(error);
    }
}

void MavFtpService::handleTargetGenerationChanged(
    qulonglong generation)
{
    if (m_shuttingDown) return;
    const quint64 interruptedIdentity = activeOperationId();
    QPointer<MavFtpService> guard(this);
    emit targetChanged();
    if (!guard) return;
    if (!m_active || m_active->identity != interruptedIdentity
        || generation == m_active->lease.generation) {
        return;
    }
    Result result = m_active->result;
    result.cancelled = true;
    result.error = tr("The selected vehicle changed; the MAVFTP operation was cancelled.");
    finishImmediate(result);
}

void MavFtpService::forgetLink(int linkId)
{
    if (!m_active || m_active->lease.endpoint.linkId != linkId) {
        return;
    }
    ActiveOperation operation = *m_active;
    Result result = operation.result;
    result.cancelled = true;
    result.error = tr("The MAVFTP target link was removed.");
    clearActive();
    QPointer<MavFtpService> guard(this);
    sendCleanupBestEffort(operation);
    if (!guard) {
        return;
    }
    m_lastError = result.error;
    emit stateChanged();
    if (guard) {
        emit operationFinished(result);
    }
}

bool MavFtpService::cancelOperation(quint64 operationId)
{
    if (!operationId || !m_active || m_active->identity != operationId)
        return false;
    cancel();
    return true;
}

void MavFtpService::cancel()
{
    if (!m_active) {
        return;
    }
    ActiveOperation operation = *m_active;
    Result result = operation.result;
    result.cancelled = true;
    result.error = tr("The MAVFTP operation was cancelled.");
    clearActive();
    QPointer<MavFtpService> guard(this);
    sendCleanupBestEffort(operation);
    if (!guard) {
        return;
    }
    m_lastError = result.error;
    emit stateChanged();
    if (guard) {
        emit operationFinished(result);
    }
}

void MavFtpService::shutdown()
{
    shutdownInternal(true);
}

void MavFtpService::shutdownInternal(bool notify)
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    QPointer<MavFtpService> guard(this);
    if (m_active) {
        ActiveOperation operation = *m_active;
        clearActive();
        sendCleanupBestEffort(operation);
    }
    if (guard && notify) emit targetChanged();
}

void MavFtpService::finishSuccess()
{
    if (!m_active) {
        return;
    }
    if (m_active->result.operation == Operation::Download
        || m_active->result.operation == Operation::Upload) {
        beginCleanup();
        return;
    }
    finishImmediate(m_active->result);
}

void MavFtpService::finishFailure(
    const QString &error, bool cancelled)
{
    if (!m_active) {
        return;
    }
    const bool transfer =
        m_active->result.operation == Operation::Download
        || m_active->result.operation == Operation::Upload;
    const bool alreadyCleaning =
        m_active->stage == Stage::TerminateCleanup
        || m_active->stage == Stage::ResetCleanup;
    const bool resetAlreadyExhausted =
        m_active->stage == Stage::ResetBeforeDownload
        || m_active->stage == Stage::ResetBeforeUpload;
    if (transfer && !alreadyCleaning && !resetAlreadyExhausted
        && targetIsCurrent(m_active->lease)) {
        beginCleanup(error, cancelled);
        return;
    }
    Result result = m_active->result;
    result.error = error;
    result.cancelled = cancelled;
    finishImmediate(result);
}

void MavFtpService::finishImmediate(Result result)
{
    const bool hadActive = static_cast<bool>(m_active);
    clearActive();
    m_lastError = result.error;
    if (!hadActive) {
        return;
    }
    QPointer<MavFtpService> guard(this);
    emit stateChanged();
    if (guard) {
        emit operationFinished(result);
    }
}

bool MavFtpService::publishProgress(qint64 completed, qint64 total)
{
    if (!m_active) return false;
    const quint64 identity = m_active->identity;
    const quint64 generation = m_active->lease.generation;
    QPointer<MavFtpService> guard(this);
    emit operationProgress(identity, generation, completed, total);
    if (!guard || !m_active || m_active->identity != identity) return false;
    emit progressChanged(generation, completed, total);
    return guard && m_active && m_active->identity == identity;
}

void MavFtpService::clearActive()
{
    m_timer.stop();
    m_active.reset();
    m_request = MavFtpProtocol::PayloadHeader();
    m_attempts = 0;
    ++m_requestToken;
}

bool MavFtpService::stageRequiresSession(Stage stage)
{
    return stage != Stage::None
        && stage != Stage::OpenDownload
        && stage != Stage::CreateUpload;
}

bool MavFtpService::stageRequiresOffset(Stage stage)
{
    return stage == Stage::ListPage
        || stage == Stage::ReadDownload
        || stage == Stage::WriteUpload;
}

int MavFtpService::timeoutForRequest(
    const MavFtpProtocol::PayloadHeader &ftpRequest) const
{
    switch (ftpRequest.opcode) {
    case MavFtpProtocol::Opcode::OpenFileReadOnly:
    case MavFtpProtocol::Opcode::CreateFile:
        return m_openCreateTimeoutMs;
    case MavFtpProtocol::Opcode::CalculateFileCrc32:
        return m_crcTimeoutMs;
    default:
        return m_timeoutMs;
    }
}

QString MavFtpService::remoteErrorText(
    const MavFtpProtocol::PayloadHeader &response)
{
    const auto code = static_cast<MavFtpProtocol::ErrorCode>(
        static_cast<quint8>(response.data.at(0)));
    QString name;
    switch (code) {
    case MavFtpProtocol::ErrorCode::None:
        name = tr("no error");
        break;
    case MavFtpProtocol::ErrorCode::Fail:
        name = tr("operation failed");
        break;
    case MavFtpProtocol::ErrorCode::FailErrno:
        name = tr("remote errno %1")
            .arg(static_cast<quint8>(response.data.at(1)));
        break;
    case MavFtpProtocol::ErrorCode::InvalidDataSize:
        name = tr("invalid data size");
        break;
    case MavFtpProtocol::ErrorCode::InvalidSession:
        name = tr("invalid session");
        break;
    case MavFtpProtocol::ErrorCode::NoSessionsAvailable:
        name = tr("no sessions available");
        break;
    case MavFtpProtocol::ErrorCode::EndOfFile:
        name = tr("end of file");
        break;
    case MavFtpProtocol::ErrorCode::UnknownCommand:
        name = tr("unknown command");
        break;
    case MavFtpProtocol::ErrorCode::FileExists:
        name = tr("file exists");
        break;
    case MavFtpProtocol::ErrorCode::FileProtected:
        name = tr("file protected");
        break;
    case MavFtpProtocol::ErrorCode::FileNotFound:
        name = tr("file not found");
        break;
    }
    return tr("MAVFTP remote rejected %1: %2.")
        .arg(static_cast<int>(response.requestOpcode))
        .arg(name);
}

QString MavFtpService::dispatchErrorText(
    DispatchResult result, const QString &context)
{
    switch (result) {
    case DispatchResult::Sent:
    case DispatchResult::Superseded:
        return QString();
    case DispatchResult::StaleTarget:
        return tr("Cannot %1 because the selected target changed.")
            .arg(context);
    case DispatchResult::EncodingFailure:
        return tr("Cannot %1 because the MAVFTP request is invalid.")
            .arg(context);
    case DispatchResult::TransportFailure:
        return tr("Cannot %1 because the exact-link transport failed.")
            .arg(context);
    }
    return tr("Cannot %1.").arg(context);
}
