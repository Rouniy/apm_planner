#ifndef PX4FLOWFRAMEASSEMBLER_H
#define PX4FLOWFRAMEASSEMBLER_H

#include <QBitArray>
#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <optional>

/**
 * Bounded, transport-free assembler for PX4Flow RAW8U image messages.
 *
 * A DATA_TRANSMISSION_HANDSHAKE starts a generation and each following
 * ENCAPSULATED_DATA packet contributes one negotiated-payload-sized range.
 * Link, vehicle and timeout ownership deliberately remain with the caller.
 */
class Px4FlowFrameAssembler final
{
public:
    static constexpr quint8 Raw8UStreamType = 2;
    static constexpr int WirePayloadBytes = 253;
    static constexpr quint32 DefaultMaximumFrameBytes = 1024U * 1024U;
    static constexpr quint32 AbsoluteMaximumFrameBytes = 16U * 1024U * 1024U;

    struct Descriptor
    {
        quint8 type = 0;
        quint32 size = 0;
        quint16 width = 0;
        quint16 height = 0;
        quint16 packets = 0;
        quint8 payload = 0;
        quint8 jpegQuality = 0;
    };

    struct Frame
    {
        quint16 width = 0;
        quint16 height = 0;
        QByteArray grayscale;
        quint64 generation = 0;
    };

    enum class BeginResult
    {
        Started,
        UnsupportedType,
        InvalidDescriptor,
        FrameTooLarge,
        GenerationExhausted
    };

    enum class PacketResult
    {
        IgnoredNoFrame,
        Accepted,
        Duplicate,
        Completed,
        InvalidSequence,
        InvalidPayload,
        ConflictingDuplicate
    };

    struct PacketOutcome
    {
        PacketResult result = PacketResult::IgnoredNoFrame;
        std::optional<Frame> completedFrame;
    };

    explicit Px4FlowFrameAssembler(
            quint32 maximumFrameBytes = DefaultMaximumFrameBytes);

    BeginResult begin(const Descriptor &descriptor,
                      QString *error = nullptr);
    PacketOutcome add(quint16 sequence, const quint8 *wireData,
                      int wireLength);

    /** Drop an incomplete frame without reusing its generation number. */
    void reset();

    bool active() const { return m_active; }
    quint64 generation() const { return m_generation; }
    int receivedPackets() const { return m_receivedCount; }
    int expectedPackets() const { return m_packetCount; }
    quint32 maximumFrameBytes() const { return m_maximumFrameBytes; }

private:
    BeginResult rejectBegin(BeginResult result, const QString &message,
                            QString *error);
    PacketOutcome rejectPacket(PacketResult result);
    void clearAssembly();

    quint32 m_maximumFrameBytes = DefaultMaximumFrameBytes;
    QByteArray m_frame;
    QBitArray m_received;
    quint32 m_frameSize = 0;
    quint16 m_width = 0;
    quint16 m_height = 0;
    int m_packetCount = 0;
    int m_payloadSize = 0;
    int m_receivedCount = 0;
    quint64 m_generation = 0;
    bool m_active = false;
};

#endif // PX4FLOWFRAMEASSEMBLER_H
