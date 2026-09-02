#include "DroneCanGetSetClient.h"

#include <QList>

#include <cmath>
#include <cstring>

namespace {
class BitWriter final
{
public:
    bool writeUnsigned(quint64 value, int bitCount)
    {
        if (bitCount < 1 || bitCount > 64) {
            return false;
        }

        int shift = 0;
        while (bitCount >= 8) {
            writeBits(quint8((value >> shift) & 0xFFU), 8);
            shift += 8;
            bitCount -= 8;
        }
        if (bitCount > 0) {
            const quint8 mask = quint8((1U << bitCount) - 1U);
            writeBits(quint8((value >> shift) & mask), bitCount);
        }
        return true;
    }

    void writeZeros(int bitCount)
    {
        while (bitCount > 0) {
            const int chunk = qMin(bitCount, 8);
            writeBits(0, chunk);
            bitCount -= chunk;
        }
    }

    void writeBytes(const QByteArray &bytes)
    {
        for (char byte : bytes) {
            writeBits(quint8(byte), 8);
        }
    }

    QByteArray bytes() const
    {
        return m_data;
    }

private:
    void writeBits(quint8 value, int bitCount)
    {
        for (int bit = bitCount - 1; bit >= 0; --bit) {
            if ((m_bitOffset % 8) == 0) {
                m_data.append(char(0));
            }
            if (((value >> bit) & 1U) != 0) {
                const int byteIndex = m_bitOffset / 8;
                m_data[byteIndex] = char(
                    quint8(m_data.at(byteIndex))
                    | quint8(1U << (7 - (m_bitOffset % 8))));
            }
            ++m_bitOffset;
        }
    }

    QByteArray m_data;
    int m_bitOffset = 0;
};

class BitReader final
{
public:
    explicit BitReader(const QByteArray &data)
        : m_data(data)
    {
    }

    bool readUnsigned(int bitCount, quint64 *value)
    {
        if (!value || bitCount < 1 || bitCount > 64
            || bitCount > remainingBits()) {
            return false;
        }

        quint64 decoded = 0;
        int shift = 0;
        while (bitCount >= 8) {
            quint8 byte = 0;
            if (!readBits(8, &byte)) {
                return false;
            }
            decoded |= quint64(byte) << shift;
            shift += 8;
            bitCount -= 8;
        }
        if (bitCount > 0) {
            quint8 remainder = 0;
            if (!readBits(bitCount, &remainder)) {
                return false;
            }
            decoded |= quint64(remainder) << shift;
        }
        *value = decoded;
        return true;
    }

    bool readZeroBits(int bitCount)
    {
        while (bitCount > 0) {
            const int chunk = qMin(bitCount, 8);
            quint8 value = 0;
            if (!readBits(chunk, &value) || value != 0) {
                return false;
            }
            bitCount -= chunk;
        }
        return true;
    }

    bool readBytes(int count, QByteArray *value)
    {
        if (!value || count < 0 || count > remainingBits() / 8) {
            return false;
        }
        QByteArray decoded;
        decoded.reserve(count);
        for (int i = 0; i < count; ++i) {
            quint8 byte = 0;
            if (!readBits(8, &byte)) {
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
    bool readBits(int bitCount, quint8 *value)
    {
        if (!value || bitCount < 1 || bitCount > 8
            || bitCount > remainingBits()) {
            return false;
        }
        quint8 decoded = 0;
        for (int bit = 0; bit < bitCount; ++bit) {
            const int absoluteBit = m_bitOffset + bit;
            const quint8 byte = quint8(m_data.at(absoluteBit / 8));
            decoded = quint8((decoded << 1U)
                             | ((byte >> (7 - (absoluteBit % 8))) & 1U));
        }
        m_bitOffset += bitCount;
        *value = decoded;
        return true;
    }

    const QByteArray &m_data;
    int m_bitOffset = 0;
};

bool writeValue(BitWriter *writer,
                const DroneCanGetSetClient::Value &value)
{
    if (!writer || !value.isValid()
        || !writer->writeUnsigned(quint8(value.type), 3)) {
        return false;
    }

    switch (value.type) {
    case DroneCanGetSetClient::Value::Empty:
        return true;
    case DroneCanGetSetClient::Value::Integer: {
        quint64 raw = 0;
        static_assert(sizeof(raw) == sizeof(value.integerValue),
                      "unexpected 64-bit integer representation");
        std::memcpy(&raw, &value.integerValue, sizeof(raw));
        return writer->writeUnsigned(raw, 64);
    }
    case DroneCanGetSetClient::Value::Real: {
        quint32 raw = 0;
        static_assert(sizeof(raw) == sizeof(value.realValue),
                      "unexpected float representation");
        std::memcpy(&raw, &value.realValue, sizeof(raw));
        return writer->writeUnsigned(raw, 32);
    }
    case DroneCanGetSetClient::Value::Boolean:
        return writer->writeUnsigned(value.booleanValue ? 1U : 0U, 8);
    case DroneCanGetSetClient::Value::String:
        if (!writer->writeUnsigned(quint8(value.stringValue.size()), 8)) {
            return false;
        }
        writer->writeBytes(value.stringValue);
        return true;
    }
    return false;
}

bool readValue(BitReader *reader, DroneCanGetSetClient::Value *value)
{
    quint64 tag = 0;
    if (!reader || !value || !reader->readUnsigned(3, &tag)
        || tag > DroneCanGetSetClient::Value::String) {
        return false;
    }

    DroneCanGetSetClient::Value decoded;
    decoded.type = DroneCanGetSetClient::Value::Type(tag);
    switch (decoded.type) {
    case DroneCanGetSetClient::Value::Empty:
        break;
    case DroneCanGetSetClient::Value::Integer: {
        quint64 raw = 0;
        if (!reader->readUnsigned(64, &raw)) {
            return false;
        }
        std::memcpy(&decoded.integerValue, &raw, sizeof(raw));
        break;
    }
    case DroneCanGetSetClient::Value::Real: {
        quint64 raw = 0;
        if (!reader->readUnsigned(32, &raw)) {
            return false;
        }
        const quint32 raw32 = quint32(raw);
        std::memcpy(&decoded.realValue, &raw32, sizeof(raw32));
        if (!std::isfinite(decoded.realValue)) {
            return false;
        }
        break;
    }
    case DroneCanGetSetClient::Value::Boolean: {
        quint64 raw = 0;
        if (!reader->readUnsigned(8, &raw)) {
            return false;
        }
        decoded.booleanValue = raw != 0;
        break;
    }
    case DroneCanGetSetClient::Value::String: {
        quint64 length = 0;
        if (!reader->readUnsigned(8, &length)
            || length > DroneCanGetSetClient::MaximumStringValueBytes
            || !reader->readBytes(int(length), &decoded.stringValue)) {
            return false;
        }
        break;
    }
    }
    *value = decoded;
    return true;
}

bool readNumericValue(BitReader *reader,
                      DroneCanGetSetClient::NumericValue *value)
{
    quint64 tag = 0;
    if (!reader || !value || !reader->readUnsigned(2, &tag)
        || tag > DroneCanGetSetClient::NumericValue::Real) {
        return false;
    }

    DroneCanGetSetClient::NumericValue decoded;
    decoded.type = DroneCanGetSetClient::NumericValue::Type(tag);
    switch (decoded.type) {
    case DroneCanGetSetClient::NumericValue::Empty:
        break;
    case DroneCanGetSetClient::NumericValue::Integer: {
        quint64 raw = 0;
        if (!reader->readUnsigned(64, &raw)) {
            return false;
        }
        std::memcpy(&decoded.integerValue, &raw, sizeof(raw));
        break;
    }
    case DroneCanGetSetClient::NumericValue::Real: {
        quint64 raw = 0;
        if (!reader->readUnsigned(32, &raw)) {
            return false;
        }
        const quint32 raw32 = quint32(raw);
        std::memcpy(&decoded.realValue, &raw32, sizeof(raw32));
        if (!std::isfinite(decoded.realValue)) {
            return false;
        }
        break;
    }
    }
    *value = decoded;
    return true;
}

QString invalidResponse(const QString &detail)
{
    return QStringLiteral("Invalid GetSet response: %1").arg(detail);
}

int nextCanonicalFdContentLength(int minimum)
{
    if (minimum < 0 || minimum > 63) {
        return -1;
    }
    if (minimum <= 7) {
        return minimum;
    }
    const int lengths[] = {11, 15, 19, 23, 31, 47, 63};
    for (int length : lengths) {
        if (minimum <= length) {
            return length;
        }
    }
    return -1;
}
}

bool DroneCanGetSetClient::Session::isValid() const
{
    return brokerGeneration != 0 && busIndex >= 0 && busIndex <= 1
        && localNodeId >= 1 && localNodeId <= 127;
}

bool DroneCanGetSetClient::Session::operator==(
    const Session &other) const
{
    return brokerGeneration == other.brokerGeneration
        && busIndex == other.busIndex
        && localNodeId == other.localNodeId;
}

DroneCanGetSetClient::Value DroneCanGetSetClient::Value::fromInteger(
    qint64 value)
{
    Value result;
    result.type = Integer;
    result.integerValue = value;
    return result;
}

DroneCanGetSetClient::Value DroneCanGetSetClient::Value::fromReal(
    float value)
{
    Value result;
    result.type = Real;
    result.realValue = value;
    return result;
}

DroneCanGetSetClient::Value DroneCanGetSetClient::Value::fromBoolean(
    bool value)
{
    Value result;
    result.type = Boolean;
    result.booleanValue = value;
    return result;
}

DroneCanGetSetClient::Value DroneCanGetSetClient::Value::fromString(
    const QByteArray &value)
{
    Value result;
    result.type = String;
    result.stringValue = value;
    return result;
}

bool DroneCanGetSetClient::Value::isValid() const
{
    switch (type) {
    case Empty:
    case Integer:
    case Boolean:
        return true;
    case Real:
        return std::isfinite(realValue);
    case String:
        return stringValue.size() <= MaximumStringValueBytes;
    }
    return false;
}

bool DroneCanGetSetClient::Value::operator==(const Value &other) const
{
    if (type != other.type) {
        return false;
    }
    switch (type) {
    case Empty:
        return true;
    case Integer:
        return integerValue == other.integerValue;
    case Real:
        return realValue == other.realValue;
    case Boolean:
        return booleanValue == other.booleanValue;
    case String:
        return stringValue == other.stringValue;
    }
    return false;
}

bool DroneCanGetSetClient::NumericValue::isValid() const
{
    return type == Empty || type == Integer
        || (type == Real && std::isfinite(realValue));
}

bool DroneCanGetSetClient::NumericValue::operator==(
    const NumericValue &other) const
{
    if (type != other.type) {
        return false;
    }
    switch (type) {
    case Empty:
        return true;
    case Integer:
        return integerValue == other.integerValue;
    case Real:
        return realValue == other.realValue;
    }
    return false;
}

DroneCanGetSetClient::DroneCanGetSetClient(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<DroneCanGetSetClient::Value>();
    qRegisterMetaType<DroneCanGetSetClient::NumericValue>();
    qRegisterMetaType<DroneCanGetSetClient::Parameter>();
}

bool DroneCanGetSetClient::bindSession(const Session &session)
{
    if (!session.isValid()) {
        return false;
    }
    if (m_session == session) {
        return true;
    }

    clearPendingRequest();
    if (!m_session.isValid()
        || m_session.brokerGeneration != session.brokerGeneration
        || m_session.localNodeId != session.localNodeId) {
        m_nextTransferIdByDescriptor.clear();
    }
    m_session = session;
    return true;
}

void DroneCanGetSetClient::clearPendingRequest()
{
    m_hasPendingRequest = false;
    m_request = Request{};
}

void DroneCanGetSetClient::resetSession()
{
    clearPendingRequest();
    m_session = Session{};
    m_nextTransferIdByDescriptor.clear();
}

bool DroneCanGetSetClient::isBound() const
{
    return m_session.isValid();
}

DroneCanGetSetClient::Session DroneCanGetSetClient::session() const
{
    return m_session;
}

bool DroneCanGetSetClient::hasPendingRequest() const
{
    return m_hasPendingRequest;
}

int DroneCanGetSetClient::pendingRemoteNodeId() const
{
    return m_hasPendingRequest ? m_request.remoteNodeId : -1;
}

bool DroneCanGetSetClient::getByIndex(
    int remoteNodeId, quint16 index, bool preferCanFd, qint64 nowMs)
{
    return requestParameter(remoteNodeId, index, QByteArray(), Value{},
                            preferCanFd, nowMs);
}

bool DroneCanGetSetClient::getByName(
    int remoteNodeId, const QByteArray &name, bool preferCanFd,
    qint64 nowMs)
{
    if (name.isEmpty()) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("Parameter name is empty"));
        return false;
    }
    return requestParameter(remoteNodeId, 0, name, Value{}, preferCanFd,
                            nowMs);
}

bool DroneCanGetSetClient::setByName(
    int remoteNodeId, const QByteArray &name, const Value &value,
    bool preferCanFd, qint64 nowMs)
{
    if (name.isEmpty()) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("Parameter name is empty"));
        return false;
    }
    if (value.type == Value::Empty) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("Set value is empty"));
        return false;
    }
    return requestParameter(remoteNodeId, 0, name, value, preferCanFd,
                            nowMs);
}

bool DroneCanGetSetClient::requestParameter(
    int remoteNodeId, quint16 index, const QByteArray &name,
    const Value &value, bool preferCanFd, qint64 nowMs)
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
    if (index > 8191U) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("Parameter index exceeds uint13"));
        return false;
    }
    if (name.size() > MaximumParameterNameBytes) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("Parameter name is too long"));
        return false;
    }
    if (!value.isValid()) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("Invalid parameter value"));
        return false;
    }
    if (nowMs < 0) {
        emit requestFailed(remoteNodeId,
                           QStringLiteral("Invalid monotonic timestamp"));
        return false;
    }
    // GetSet responses carry no request index. One application-scoped
    // transaction at a time makes correlation unambiguous for index walks as
    // well as named gets/sets.
    if (m_hasPendingRequest) {
        return false;
    }

    if (m_nextRequestSerial == 0) {
        m_nextRequestSerial = 1;
    }
    Request request;
    request.serial = m_nextRequestSerial++;
    request.remoteNodeId = remoteNodeId;
    request.index = index;
    request.name = name;
    request.value = value;
    request.preferCanFd = preferCanFd;
    request.transferId = allocateTransferId(remoteNodeId);
    const quint64 serial = request.serial;
    const Session requestSession = m_session;
    m_request = request;
    m_hasPendingRequest = true;
    transmit(nowMs);
    return m_hasPendingRequest && m_request.serial == serial
        && m_session == requestSession;
}

bool DroneCanGetSetClient::cancelPendingRequest(const QString &reason)
{
    if (!m_hasPendingRequest) {
        return false;
    }
    const int nodeId = m_request.remoteNodeId;
    const QString resolvedReason = reason.isEmpty()
        ? QStringLiteral("GetSet request cancelled") : reason;
    clearPendingRequest();
    emit requestCancelled(nodeId, resolvedReason);
    return true;
}

bool DroneCanGetSetClient::acceptFrame(
    quint64 brokerGeneration, int busIndex, quint32 canId,
    const QByteArray &data, bool canFd, qint64 nowMs)
{
    if (!isBound() || !m_hasPendingRequest
        || brokerGeneration != m_session.brokerGeneration
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
        || dataTypeId != GetSetDataTypeId
        || sourceNodeId != m_request.remoteNodeId) {
        return false;
    }

    const quint8 tail = quint8(data.at(data.size() - 1));
    if ((tail & 0x1FU) != m_request.transferId) {
        return false;
    }
    const bool startOfTransfer = (tail & 0x80U) != 0;
    const bool endOfTransfer = (tail & 0x40U) != 0;
    const bool toggle = (tail & 0x20U) != 0;
    const int framePayloadSize = data.size() - 1;

    if (startOfTransfer && endOfTransfer) {
        if (toggle) {
            dropAssembly(nowMs);
            return false;
        }
        return finishTransfer(data.left(framePayloadSize), canFd, nowMs);
    }

    const int fullFrameLength = canFd ? 64 : 8;
    if (startOfTransfer) {
        if (toggle || data.size() != fullFrameLength
            || framePayloadSize < 3) {
            dropAssembly(nowMs);
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
            dropAssembly(nowMs);
            return false;
        }
        m_request.assembly = assembly;
        m_request.deadlineMs = nowMs + AssemblyTimeoutMs;
        return true;
    }

    if (!m_request.assembly.active) {
        return false;
    }
    if (m_request.assembly.canFd != canFd
        || toggle != m_request.assembly.expectedToggle
        || (!endOfTransfer && data.size() != fullFrameLength)) {
        dropAssembly(nowMs);
        return false;
    }

    m_request.assembly.payload.append(data.constData(), framePayloadSize);
    if (m_request.assembly.payload.size() > MaximumAssemblyBytes) {
        dropAssembly(nowMs);
        return false;
    }
    m_request.assembly.expectedToggle =
        !m_request.assembly.expectedToggle;
    if (!endOfTransfer) {
        return true;
    }
    if (payloadCrc(m_request.assembly.payload)
        != m_request.assembly.expectedCrc) {
        dropAssembly(nowMs);
        return false;
    }
    return finishTransfer(m_request.assembly.payload, canFd, nowMs);
}

void DroneCanGetSetClient::tick(qint64 nowMs)
{
    if (!isBound() || !m_hasPendingRequest || nowMs < 0
        || nowMs < m_request.deadlineMs) {
        return;
    }
    if (m_request.assembly.active
        && nowMs - m_request.assembly.startedAtMs < AssemblyTimeoutMs) {
        return;
    }
    m_request.assembly = Assembly{};
    if (m_request.retryCount >= MaximumRetries) {
        failRequest(QStringLiteral("GetSet request timed out"));
        return;
    }
    if (nowMs - m_request.lastTransmitMs < MinimumRetryIntervalMs) {
        m_request.deadlineMs = m_request.lastTransmitMs
            + MinimumRetryIntervalMs;
        return;
    }

    ++m_request.retryCount;
    m_request.transferId = allocateTransferId(m_request.remoteNodeId);
    transmit(nowMs);
}

bool DroneCanGetSetClient::isCanonicalFrameLength(
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

quint16 DroneCanGetSetClient::addCrcByte(quint16 crc, quint8 byte)
{
    crc ^= quint16(byte) << 8U;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000U) != 0
            ? quint16((crc << 1U) ^ 0x1021U)
            : quint16(crc << 1U);
    }
    return crc;
}

quint16 DroneCanGetSetClient::payloadCrc(const QByteArray &payload)
{
    quint16 crc = GetSetBaseCrc;
    for (char byte : payload) {
        crc = addCrcByte(crc, quint8(byte));
    }
    return crc;
}

bool DroneCanGetSetClient::encodeRequestPayload(
    const Request &request, QByteArray *payload)
{
    if (!payload || request.index > 8191U
        || request.name.size() > MaximumParameterNameBytes
        || !request.value.isValid()) {
        return false;
    }

    BitWriter writer;
    if (!writer.writeUnsigned(request.index, 13)
        || !writeValue(&writer, request.value)) {
        return false;
    }
    // Tail-array optimization is used on classic CAN. CAN-FD transfers carry
    // an explicit root-array length so DLC padding cannot become part of the
    // parameter name.
    if (request.preferCanFd
        && !writer.writeUnsigned(quint8(request.name.size()), 7)) {
        return false;
    }
    writer.writeBytes(request.name);
    *payload = writer.bytes();
    return true;
}

bool DroneCanGetSetClient::decodeResponse(
    const QByteArray &payload, bool canFd, Parameter *parameter,
    QString *error)
{
    const int maximumPayload = canFd ? MaximumAssemblyBytes
                                     : MaximumClassicResponsePayloadBytes;
    if (!parameter || payload.size() < 4
        || payload.size() > maximumPayload) {
        if (error) {
            *error = invalidResponse(QStringLiteral("payload size"));
        }
        return false;
    }

    BitReader reader(payload);
    Parameter decoded = *parameter;
    if (!reader.readZeroBits(5)
        || !readValue(&reader, &decoded.value)
        || !reader.readZeroBits(5)
        || !readValue(&reader, &decoded.defaultValue)
        || !reader.readZeroBits(6)
        || !readNumericValue(&reader, &decoded.maximumValue)
        || !reader.readZeroBits(6)
        || !readNumericValue(&reader, &decoded.minimumValue)) {
        if (error) {
            *error = invalidResponse(
                QStringLiteral("truncated or invalid DSDL fields"));
        }
        return false;
    }

    if (canFd) {
        quint64 nameLength = 0;
        if (!reader.readUnsigned(7, &nameLength)
            || nameLength > MaximumParameterNameBytes
            || !reader.readBytes(int(nameLength), &decoded.name)
            || !reader.remainingBitsAreZero()) {
            if (error) {
                *error = invalidResponse(
                    QStringLiteral("CAN-FD name length or padding"));
            }
            return false;
        }
    } else {
        if ((reader.remainingBits() % 8) != 0
            || reader.remainingBits() / 8 > MaximumParameterNameBytes
            || !reader.readBytes(reader.remainingBits() / 8,
                                 &decoded.name)) {
            if (error) {
                *error = invalidResponse(
                    QStringLiteral("classic-CAN tail array"));
            }
            return false;
        }
    }

    *parameter = decoded;
    return true;
}

QList<QByteArray> DroneCanGetSetClient::makeTransferFrames(
    const QByteArray &logicalPayload, bool canFd, quint8 transferId)
{
    QByteArray payload = logicalPayload;
    const int frameContentCapacity = canFd ? 63 : 7;
    if (canFd) {
        if (payload.size() <= frameContentCapacity) {
            const int paddedSize = nextCanonicalFdContentLength(
                payload.size());
            if (paddedSize < 0) {
                return {};
            }
            payload.append(QByteArray(paddedSize - payload.size(), char(0)));
        } else {
            const int remainder = (payload.size() + 2)
                % frameContentCapacity;
            if (remainder != 0) {
                const int paddedRemainder =
                    nextCanonicalFdContentLength(remainder);
                if (paddedRemainder < 0) {
                    return {};
                }
                payload.append(QByteArray(paddedRemainder - remainder,
                                          char(0)));
            }
        }
    }

    QList<QByteArray> frames;
    if (payload.size() <= frameContentCapacity) {
        QByteArray frame = payload;
        frame.append(char(0xC0U | (transferId & 0x1FU)));
        frames.append(frame);
        return frames;
    }

    const quint16 crc = payloadCrc(payload);
    QByteArray transfer;
    transfer.reserve(payload.size() + 2);
    transfer.append(char(crc & 0xFFU));
    transfer.append(char((crc >> 8U) & 0xFFU));
    transfer.append(payload);

    int offset = 0;
    bool toggle = false;
    while (offset < transfer.size()) {
        const int amount = qMin(frameContentCapacity,
                                transfer.size() - offset);
        const bool first = offset == 0;
        const bool last = offset + amount == transfer.size();
        QByteArray frame = transfer.mid(offset, amount);
        quint8 tail = transferId & 0x1FU;
        if (first) {
            tail |= 0x80U;
        }
        if (last) {
            tail |= 0x40U;
        }
        if (toggle) {
            tail |= 0x20U;
        }
        frame.append(char(tail));
        frames.append(frame);
        toggle = !toggle;
        offset += amount;
    }
    return frames;
}

quint32 DroneCanGetSetClient::requestCanId(int remoteNodeId) const
{
    const quint32 cleanId = (quint32(ServicePriority) << 24U)
        | (quint32(GetSetDataTypeId) << 16U) | (1U << 15U)
        | (quint32(remoteNodeId) << 8U) | (1U << 7U)
        | quint32(m_session.localNodeId);
    return cleanId | ExtendedFrameFlag;
}

quint8 DroneCanGetSetClient::allocateTransferId(int remoteNodeId)
{
    const quint16 descriptor = quint16((m_session.busIndex << 8)
                                       | remoteNodeId);
    const quint8 transferId = m_nextTransferIdByDescriptor.value(
        descriptor, 0);
    m_nextTransferIdByDescriptor.insert(
        descriptor, quint8((transferId + 1U) & 0x1FU));
    return transferId;
}

void DroneCanGetSetClient::transmit(qint64 nowMs)
{
    if (!m_hasPendingRequest) {
        return;
    }
    QByteArray payload;
    if (!encodeRequestPayload(m_request, &payload)) {
        failRequest(QStringLiteral("Could not encode GetSet request"));
        return;
    }
    const QList<QByteArray> frames = makeTransferFrames(
        payload, m_request.preferCanFd, m_request.transferId);
    if (frames.isEmpty()) {
        failRequest(QStringLiteral("Could not frame GetSet request"));
        return;
    }

    m_request.assembly = Assembly{};
    m_request.lastTransmitMs = nowMs;
    m_request.deadlineMs = nowMs + RequestTimeoutMs;
    const quint64 serial = m_request.serial;
    const quint8 transferId = m_request.transferId;
    const Session requestSession = m_session;
    const quint32 canId = requestCanId(m_request.remoteNodeId);
    const bool canFd = m_request.preferCanFd;
    for (const QByteArray &frame : frames) {
        emit transmitRequested(canId, frame, canFd);
        // A transport or UI callback is allowed to synchronously cancel or
        // rebind the application-scoped client. Never emit the remainder of
        // an invalidated multi-frame transfer.
        if (!m_hasPendingRequest || m_request.serial != serial
            || m_request.transferId != transferId
            || m_session != requestSession) {
            return;
        }
    }
}

void DroneCanGetSetClient::dropAssembly(qint64 nowMs)
{
    if (!m_hasPendingRequest) {
        return;
    }
    m_request.assembly = Assembly{};
    m_request.deadlineMs = qMax(
        nowMs, m_request.lastTransmitMs + MinimumRetryIntervalMs);
}

void DroneCanGetSetClient::failRequest(const QString &reason)
{
    if (!m_hasPendingRequest) {
        return;
    }
    const int nodeId = m_request.remoteNodeId;
    clearPendingRequest();
    emit requestFailed(nodeId, reason);
}

bool DroneCanGetSetClient::finishTransfer(
    const QByteArray &payload, bool canFd, qint64 nowMs)
{
    if (!m_hasPendingRequest) {
        return false;
    }
    Parameter parameter;
    parameter.brokerGeneration = m_session.brokerGeneration;
    parameter.busIndex = m_session.busIndex;
    parameter.nodeId = m_request.remoteNodeId;
    parameter.transferId = m_request.transferId;
    parameter.canFd = canFd;
    parameter.requestedIndex = m_request.index;
    parameter.requestedName = m_request.name;
    QString error;
    if (!decodeResponse(payload, canFd, &parameter, &error)) {
        dropAssembly(nowMs);
        return false;
    }
    // An empty name is the protocol's not-found result. Any non-empty named
    // response must identify the exact parameter that was requested.
    if (!m_request.name.isEmpty() && !parameter.name.isEmpty()
        && parameter.name != m_request.name) {
        dropAssembly(nowMs);
        return false;
    }

    clearPendingRequest();
    emit parameterReceived(parameter);
    return true;
}
