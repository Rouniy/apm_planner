#include "DroneCanGetNodeInfoClient.h"

#include <QList>

namespace {
class BitReader final
{
public:
    explicit BitReader(const QByteArray &data)
        : m_data(data)
    {
    }

    bool readBits(int count, quint8 *value)
    {
        if (!value || count < 1 || count > 8
            || m_bitOffset > m_data.size() * 8 - count) {
            return false;
        }

        quint8 decoded = 0;
        for (int i = 0; i < count; ++i) {
            const int absoluteBit = m_bitOffset + i;
            const quint8 byte = quint8(m_data.at(absoluteBit / 8));
            decoded = quint8((decoded << 1U)
                             | ((byte >> (7 - (absoluteBit % 8))) & 1U));
        }
        m_bitOffset += count;
        *value = decoded;
        return true;
    }

    bool readU8(quint8 *value)
    {
        return readBits(8, value);
    }

    bool readU16(quint16 *value)
    {
        quint8 low = 0;
        quint8 high = 0;
        if (!value || !readU8(&low) || !readU8(&high)) {
            return false;
        }
        *value = quint16(low) | (quint16(high) << 8U);
        return true;
    }

    bool readU32(quint32 *value)
    {
        if (!value) {
            return false;
        }
        quint32 decoded = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            quint8 byte = 0;
            if (!readU8(&byte)) {
                return false;
            }
            decoded |= quint32(byte) << shift;
        }
        *value = decoded;
        return true;
    }

    bool readU64(quint64 *value)
    {
        if (!value) {
            return false;
        }
        quint64 decoded = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            quint8 byte = 0;
            if (!readU8(&byte)) {
                return false;
            }
            decoded |= quint64(byte) << shift;
        }
        *value = decoded;
        return true;
    }

    bool readBytes(int count, QByteArray *value)
    {
        if (!value || count < 0) {
            return false;
        }
        QByteArray decoded;
        decoded.reserve(count);
        for (int i = 0; i < count; ++i) {
            quint8 byte = 0;
            if (!readU8(&byte)) {
                return false;
            }
            decoded.append(char(byte));
        }
        *value = decoded;
        return true;
    }

    int remainingBits() const
    {
        return m_data.size() * 8 - m_bitOffset;
    }

    int consumedBits() const
    {
        return m_bitOffset;
    }

    bool remainingBitsAreZero() const
    {
        for (int bit = m_bitOffset; bit < m_data.size() * 8; ++bit) {
            const quint8 byte = quint8(m_data.at(bit / 8));
            if (((byte >> (7 - (bit % 8))) & 1U) != 0) {
                return false;
            }
        }
        return true;
    }

private:
    const QByteArray &m_data;
    int m_bitOffset = 0;
};

QString invalidResponse(const QString &detail)
{
    return QStringLiteral("Invalid GetNodeInfo response: %1").arg(detail);
}
}

bool DroneCanGetNodeInfoClient::Session::isValid() const
{
    return brokerGeneration != 0 && busIndex >= 0 && busIndex <= 1
        && localNodeId >= 1 && localNodeId <= 127;
}

bool DroneCanGetNodeInfoClient::Session::operator==(
    const Session &other) const
{
    return brokerGeneration == other.brokerGeneration
        && busIndex == other.busIndex
        && localNodeId == other.localNodeId;
}

DroneCanGetNodeInfoClient::DroneCanGetNodeInfoClient(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<DroneCanGetNodeInfoClient::NodeInfo>();
}

bool DroneCanGetNodeInfoClient::bindSession(const Session &session)
{
    if (!session.isValid()) {
        return false;
    }
    if (m_session == session) {
        return true;
    }

    resetSession();
    m_session = session;
    return true;
}

void DroneCanGetNodeInfoClient::resetSession()
{
    clearPendingRequests();
    m_session = Session{};
    m_nextTransferIdByNode.clear();
}

void DroneCanGetNodeInfoClient::clearPendingRequests()
{
    m_requests.clear();
}

bool DroneCanGetNodeInfoClient::isBound() const
{
    return m_session.isValid();
}

DroneCanGetNodeInfoClient::Session DroneCanGetNodeInfoClient::session() const
{
    return m_session;
}

int DroneCanGetNodeInfoClient::pendingRequestCount() const
{
    return m_requests.size();
}

bool DroneCanGetNodeInfoClient::requestNodeInfo(
    int remoteNodeId, bool preferCanFd, qint64 nowMs)
{
    if (!isBound()) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("DroneCAN session is not bound"));
        return false;
    }
    if (remoteNodeId < 1 || remoteNodeId > 127
        || remoteNodeId == m_session.localNodeId) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("Invalid remote DroneCAN node ID"));
        return false;
    }
    if (nowMs < 0) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("Invalid monotonic timestamp"));
        return false;
    }
    if (m_requests.contains(remoteNodeId)) {
        return false;
    }

    Request request;
    request.remoteNodeId = remoteNodeId;
    request.preferCanFd = preferCanFd;
    request.transferId = allocateTransferId(remoteNodeId);
    const Session requestSession = m_session;
    const quint8 requestTransferId = request.transferId;
    m_requests.insert(remoteNodeId, request);
    transmit(&m_requests[remoteNodeId], nowMs);
    const auto active = m_requests.constFind(remoteNodeId);
    return m_session == requestSession && active != m_requests.constEnd()
        && active->transferId == requestTransferId;
}

bool DroneCanGetNodeInfoClient::acceptFrame(
    quint64 brokerGeneration, int busIndex, quint32 canId,
    const QByteArray &data, bool canFd, qint64 nowMs)
{
    if (!isBound() || brokerGeneration != m_session.brokerGeneration
        || busIndex != m_session.busIndex || nowMs < 0
        || !isCanonicalFrameLength(data.size(), canFd)
        || (canId & ExtendedFrameFlag) == 0
        || (canId & (RemoteTransmissionFlag | ErrorFrameFlag)) != 0) {
        return false;
    }

    const quint32 cleanId = canId & ExtendedIdMask;
    const int priority = int((cleanId >> 24U) & 0x1FU);
    const int sourceNodeId = int(cleanId & 0x7FU);
    const bool service = (cleanId & 0x80U) != 0;
    const int destinationNodeId = int((cleanId >> 8U) & 0x7FU);
    const bool requestNotResponse = (cleanId & 0x8000U) != 0;
    const int dataTypeId = int((cleanId >> 16U) & 0xFFU);
    if (priority != ServicePriority || !service || requestNotResponse
        || destinationNodeId != m_session.localNodeId
        || dataTypeId != GetNodeInfoDataTypeId) {
        return false;
    }

    auto it = m_requests.find(sourceNodeId);
    if (it == m_requests.end()) {
        return false;
    }
    Request *request = &it.value();
    const quint8 tail = quint8(data.at(data.size() - 1));
    if ((tail & 0x1FU) != request->transferId) {
        return false;
    }

    const bool startOfTransfer = (tail & 0x80U) != 0;
    const bool endOfTransfer = (tail & 0x40U) != 0;
    const bool toggle = (tail & 0x20U) != 0;
    const int framePayloadSize = data.size() - 1;

    if (startOfTransfer && endOfTransfer) {
        if (toggle) {
            dropAssembly(request, nowMs);
            return false;
        }
        return finishTransfer(request, data.left(framePayloadSize), canFd,
                              nowMs);
    }

    const int fullFrameLength = canFd ? 64 : 8;
    if (startOfTransfer) {
        if (toggle || data.size() != fullFrameLength
            || framePayloadSize < 3) {
            dropAssembly(request, nowMs);
            return false;
        }

        Assembly assembly;
        assembly.active = true;
        assembly.canFd = canFd;
        assembly.expectedToggle = true;
        assembly.startedAtMs = nowMs;
        assembly.expectedCrc = quint16(quint8(data.at(0)))
            | (quint16(quint8(data.at(1))) << 8U);
        assembly.payload = data.mid(2, framePayloadSize - 2);
        if (assembly.payload.size() > MaximumAssemblyBytes) {
            dropAssembly(request, nowMs);
            return false;
        }
        request->assembly = assembly;
        request->deadlineMs = nowMs + AssemblyTimeoutMs;
        return true;
    }

    if (!request->assembly.active) {
        return false;
    }
    if (request->assembly.canFd != canFd) {
        dropAssembly(request, nowMs);
        return false;
    }
    if (toggle != request->assembly.expectedToggle
        || (!endOfTransfer && data.size() != fullFrameLength)) {
        dropAssembly(request, nowMs);
        return false;
    }

    request->assembly.payload.append(data.constData(), framePayloadSize);
    if (request->assembly.payload.size() > MaximumAssemblyBytes) {
        dropAssembly(request, nowMs);
        return false;
    }
    request->assembly.expectedToggle = !request->assembly.expectedToggle;
    if (!endOfTransfer) {
        return true;
    }

    if (payloadCrc(request->assembly.payload)
        != request->assembly.expectedCrc) {
        dropAssembly(request, nowMs);
        return false;
    }
    return finishTransfer(request, request->assembly.payload, canFd, nowMs);
}

void DroneCanGetNodeInfoClient::tick(qint64 nowMs)
{
    if (!isBound() || nowMs < 0) {
        return;
    }

    const QList<int> nodeIds = m_requests.keys();
    for (int nodeId : nodeIds) {
        auto it = m_requests.find(nodeId);
        if (it == m_requests.end()) {
            continue;
        }
        Request *request = &it.value();
        if (nowMs < request->deadlineMs) {
            continue;
        }

        if (request->assembly.active
            && nowMs - request->assembly.startedAtMs
                < AssemblyTimeoutMs) {
            continue;
        }
        request->assembly = Assembly{};
        if (request->retryCount >= MaximumRetries) {
            failRequest(nodeId,
                        QStringLiteral("GetNodeInfo request timed out"));
            continue;
        }
        if (nowMs - request->lastTransmitMs
            < MinimumRetryIntervalMs) {
            request->deadlineMs = request->lastTransmitMs
                + MinimumRetryIntervalMs;
            continue;
        }

        ++request->retryCount;
        request->transferId = allocateTransferId(nodeId);
        transmit(request, nowMs);
    }
}

bool DroneCanGetNodeInfoClient::isCanonicalFrameLength(
    int length, bool canFd)
{
    if (length < 1) {
        return false;
    }
    if (!canFd) {
        return length <= 8;
    }
    return length <= 8 || length == 12 || length == 16
        || length == 20 || length == 24 || length == 32
        || length == 48 || length == 64;
}

quint16 DroneCanGetNodeInfoClient::addCrcByte(
    quint16 crc, quint8 byte)
{
    crc ^= quint16(byte) << 8U;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000U) != 0
            ? quint16((crc << 1U) ^ 0x1021U)
            : quint16(crc << 1U);
    }
    return crc;
}

quint16 DroneCanGetNodeInfoClient::payloadCrc(
    const QByteArray &payload)
{
    quint16 crc = GetNodeInfoBaseCrc;
    for (char byte : payload) {
        crc = addCrcByte(crc, quint8(byte));
    }
    return crc;
}

bool DroneCanGetNodeInfoClient::decodeResponse(
    const QByteArray &payload, bool canFd, NodeInfo *info,
    QString *error)
{
    if (!info || payload.isEmpty() || payload.size() > MaximumAssemblyBytes
        || (!canFd
            && payload.size() > MaximumClassicResponsePayloadBytes)) {
        if (error) {
            *error = invalidResponse(QStringLiteral("payload size"));
        }
        return false;
    }

    BitReader reader(payload);
    NodeInfo decoded = *info;
    quint8 certificateLength = 0;
    QByteArray nameBytes;
    if (!reader.readU32(&decoded.status.uptimeSeconds)
        || !reader.readBits(2, &decoded.status.health)
        || !reader.readBits(3, &decoded.status.mode)
        || !reader.readBits(3, &decoded.status.subMode)
        || !reader.readU16(&decoded.status.vendorSpecificStatusCode)
        || !reader.readU8(&decoded.softwareVersion.major)
        || !reader.readU8(&decoded.softwareVersion.minor)
        || !reader.readU8(&decoded.softwareVersion.optionalFieldFlags)
        || !reader.readU32(&decoded.softwareVersion.vcsCommit)
        || !reader.readU64(&decoded.softwareVersion.imageCrc)
        || !reader.readU8(&decoded.hardwareVersion.major)
        || !reader.readU8(&decoded.hardwareVersion.minor)
        || !reader.readBytes(16, &decoded.hardwareVersion.uniqueId)
        || !reader.readU8(&certificateLength)
        || !reader.readBytes(certificateLength,
                             &decoded.hardwareVersion
                                  .certificateOfAuthenticity)) {
        if (error) {
            *error = invalidResponse(QStringLiteral("truncated DSDL fields"));
        }
        return false;
    }

    if (canFd) {
        quint8 nameLength = 0;
        if (!reader.readBits(7, &nameLength) || nameLength > 80
            || !reader.readBytes(nameLength, &nameBytes)
            || !reader.remainingBitsAreZero()) {
            if (error) {
                *error = invalidResponse(
                    QStringLiteral("CAN-FD length or padding"));
            }
            return false;
        }
    } else {
        if ((reader.remainingBits() % 8) != 0
            || reader.remainingBits() / 8 > 80
            || !reader.readBytes(reader.remainingBits() / 8,
                                 &nameBytes)) {
            if (error) {
                *error = invalidResponse(
                    QStringLiteral("classic-CAN tail array"));
            }
            return false;
        }
    }

    if (!validNodeName(nameBytes)) {
        if (error) {
            *error = invalidResponse(QStringLiteral("node name"));
        }
        return false;
    }
    if ((reader.consumedBits() + 7) / 8
        > (canFd ? MaximumFdResponsePayloadBytes
                 : MaximumClassicResponsePayloadBytes)) {
        if (error) {
            *error = invalidResponse(QStringLiteral("payload limit"));
        }
        return false;
    }
    decoded.name = QString::fromLatin1(nameBytes);
    *info = decoded;
    return true;
}

bool DroneCanGetNodeInfoClient::validNodeName(
    const QByteArray &name)
{
    if (name.isEmpty() || name.size() > 80) {
        return false;
    }
    for (char raw : name) {
        const quint8 value = quint8(raw);
        const bool valid = (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '.'
            || value == '-' || value == '_';
        if (!valid) {
            return false;
        }
    }
    return true;
}

quint32 DroneCanGetNodeInfoClient::requestCanId(
    int remoteNodeId) const
{
    const quint32 cleanId = (quint32(ServicePriority) << 24U)
        | (quint32(GetNodeInfoDataTypeId) << 16U) | (1U << 15U)
        | (quint32(remoteNodeId) << 8U) | (1U << 7U)
        | quint32(m_session.localNodeId);
    return cleanId | ExtendedFrameFlag;
}

quint8 DroneCanGetNodeInfoClient::allocateTransferId(int remoteNodeId)
{
    const quint8 transferId = m_nextTransferIdByNode.value(remoteNodeId, 0);
    m_nextTransferIdByNode.insert(
        remoteNodeId, quint8((transferId + 1U) & 0x1FU));
    return transferId;
}

void DroneCanGetNodeInfoClient::transmit(
    Request *request, qint64 nowMs)
{
    if (!request) {
        return;
    }
    request->assembly = Assembly{};
    request->lastTransmitMs = nowMs;
    request->deadlineMs = nowMs + RequestTimeoutMs;
    const QByteArray data(1, char(0xC0U | request->transferId));
    emit transmitRequested(requestCanId(request->remoteNodeId), data,
                           request->preferCanFd);
}

void DroneCanGetNodeInfoClient::failRequest(
    int remoteNodeId, const QString &reason)
{
    if (m_requests.remove(remoteNodeId) > 0) {
        emit requestFailed(remoteNodeId, reason);
    }
}

void DroneCanGetNodeInfoClient::dropAssembly(
    Request *request, qint64 nowMs)
{
    if (!request) {
        return;
    }
    request->assembly = Assembly{};
    request->deadlineMs = qMax(nowMs,
                               request->lastTransmitMs
                                   + MinimumRetryIntervalMs);
}

bool DroneCanGetNodeInfoClient::finishTransfer(
    Request *request, const QByteArray &payload, bool canFd,
    qint64 nowMs)
{
    if (!request) {
        return false;
    }
    const int remoteNodeId = request->remoteNodeId;
    NodeInfo info;
    info.brokerGeneration = m_session.brokerGeneration;
    info.busIndex = m_session.busIndex;
    info.nodeId = remoteNodeId;
    info.transferId = request->transferId;
    info.canFd = canFd;
    QString error;
    if (!decodeResponse(payload, canFd, &info, &error)) {
        Q_UNUSED(error);
        dropAssembly(request, nowMs);
        return false;
    }

    m_requests.remove(remoteNodeId);
    emit nodeInfoReceived(info);
    return true;
}
