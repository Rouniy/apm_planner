#include "Px4FlowFrameAssembler.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

Px4FlowFrameAssembler::Px4FlowFrameAssembler(quint32 maximumFrameBytes)
    : m_maximumFrameBytes(std::min(maximumFrameBytes,
                                   AbsoluteMaximumFrameBytes))
{
}

Px4FlowFrameAssembler::BeginResult Px4FlowFrameAssembler::begin(
        const Descriptor &descriptor, QString *error)
{
    clearAssembly();
    if (error) error->clear();

    if (descriptor.type != Raw8UStreamType) {
        return rejectBegin(BeginResult::UnsupportedType,
                           QStringLiteral("Only RAW8U PX4Flow frames are supported."),
                           error);
    }
    if (descriptor.size == 0 || descriptor.width == 0
            || descriptor.height == 0 || descriptor.packets == 0
            || descriptor.payload == 0
            || descriptor.payload > WirePayloadBytes) {
        return rejectBegin(BeginResult::InvalidDescriptor,
                           QStringLiteral("The PX4Flow frame descriptor is invalid."),
                           error);
    }

    const quint64 pixels = quint64(descriptor.width) * descriptor.height;
    if (descriptor.size > m_maximumFrameBytes
            || pixels > m_maximumFrameBytes) {
        return rejectBegin(BeginResult::FrameTooLarge,
                           QStringLiteral("The PX4Flow frame exceeds the configured size limit."),
                           error);
    }
    if (pixels != descriptor.size) {
        return rejectBegin(BeginResult::InvalidDescriptor,
                           QStringLiteral("RAW8U frame size does not match its dimensions."),
                           error);
    }

    const quint64 expectedPackets =
            (quint64(descriptor.size) - 1U) / descriptor.payload + 1U;
    if (expectedPackets != descriptor.packets) {
        return rejectBegin(BeginResult::InvalidDescriptor,
                           QStringLiteral("PX4Flow packet count does not match the frame layout."),
                           error);
    }
    if (m_generation == std::numeric_limits<quint64>::max()) {
        return rejectBegin(BeginResult::GenerationExhausted,
                           QStringLiteral("PX4Flow frame generation is exhausted."),
                           error);
    }

    m_frame = QByteArray(static_cast<int>(descriptor.size), char(0));
    m_received = QBitArray(descriptor.packets, false);
    m_frameSize = descriptor.size;
    m_width = descriptor.width;
    m_height = descriptor.height;
    m_packetCount = descriptor.packets;
    m_payloadSize = descriptor.payload;
    m_receivedCount = 0;
    ++m_generation;
    m_active = true;
    return BeginResult::Started;
}

Px4FlowFrameAssembler::PacketOutcome Px4FlowFrameAssembler::add(
        quint16 sequence, const quint8 *wireData, int wireLength)
{
    if (!m_active) {
        return {};
    }
    if (sequence >= m_packetCount) {
        return rejectPacket(PacketResult::InvalidSequence);
    }

    const quint64 offset = quint64(sequence) * m_payloadSize;
    if (offset >= m_frameSize) {
        return rejectPacket(PacketResult::InvalidSequence);
    }
    const int expectedLength = static_cast<int>(std::min(
            quint64(m_payloadSize), quint64(m_frameSize) - offset));
    if (!wireData || wireLength < expectedLength
            || wireLength > WirePayloadBytes) {
        return rejectPacket(PacketResult::InvalidPayload);
    }

    char *const destination = m_frame.data() + static_cast<int>(offset);
    if (m_received.testBit(sequence)) {
        if (std::memcmp(destination, wireData,
                        static_cast<std::size_t>(expectedLength)) == 0) {
            return {PacketResult::Duplicate, std::nullopt};
        }
        return rejectPacket(PacketResult::ConflictingDuplicate);
    }

    std::memcpy(destination, wireData,
                static_cast<std::size_t>(expectedLength));
    m_received.setBit(sequence);
    ++m_receivedCount;
    if (m_receivedCount != m_packetCount) {
        return {PacketResult::Accepted, std::nullopt};
    }

    Frame completed;
    completed.width = m_width;
    completed.height = m_height;
    completed.grayscale = std::move(m_frame);
    completed.generation = m_generation;
    clearAssembly();
    return {PacketResult::Completed,
            std::optional<Frame>(std::move(completed))};
}

void Px4FlowFrameAssembler::reset()
{
    clearAssembly();
}

Px4FlowFrameAssembler::BeginResult Px4FlowFrameAssembler::rejectBegin(
        BeginResult result, const QString &message, QString *error)
{
    if (error) *error = message;
    return result;
}

Px4FlowFrameAssembler::PacketOutcome
Px4FlowFrameAssembler::rejectPacket(PacketResult result)
{
    clearAssembly();
    return {result, std::nullopt};
}

void Px4FlowFrameAssembler::clearAssembly()
{
    m_frame.clear();
    m_received.clear();
    m_frameSize = 0;
    m_width = 0;
    m_height = 0;
    m_packetCount = 0;
    m_payloadSize = 0;
    m_receivedCount = 0;
    m_active = false;
}
