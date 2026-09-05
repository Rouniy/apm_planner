#include "comm/Px4FlowFrameAssembler.h"

#include <QtTest/QTest>

#include <algorithm>

class Px4FlowFrameAssemblerTest final : public QObject
{
    Q_OBJECT

private slots:
    void assemblesActualVideoFrameOutOfOrder();
    void acceptsActualFocusDescriptor();
    void lastPacketDoesNotCompleteMissingFrame();
    void duplicatePacketsAreIdempotentAndConflictsFail();
    void negotiatedPayloadDoesNotCopyWirePadding();
    void newOrInvalidHandshakeClearsPartialFrame();
    void rejectsInvalidDescriptors();
    void rejectsInvalidPacketsTransactionally();
    void completionMovesOneFrameAndGenerationIsMonotonic();
};

using BeginResult = Px4FlowFrameAssembler::BeginResult;
using PacketResult = Px4FlowFrameAssembler::PacketResult;

namespace {

Px4FlowFrameAssembler::Descriptor descriptorFor(
        quint32 size, quint16 width, quint16 height, quint8 payload)
{
    Px4FlowFrameAssembler::Descriptor descriptor;
    descriptor.type = Px4FlowFrameAssembler::Raw8UStreamType;
    descriptor.size = size;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.payload = payload;
    descriptor.packets = static_cast<quint16>(
            (quint64(size) - 1U) / payload + 1U);
    descriptor.jpegQuality = 100;
    return descriptor;
}

QByteArray wirePacket(const QByteArray &image, int sequence, int payload,
                      char padding = char(0x7f))
{
    QByteArray packet(Px4FlowFrameAssembler::WirePayloadBytes, padding);
    const int offset = sequence * payload;
    const int count = std::min(payload, image.size() - offset);
    if (count > 0) {
        std::copy_n(image.constData() + offset, count, packet.data());
    }
    return packet;
}

Px4FlowFrameAssembler::PacketOutcome addPacket(
        Px4FlowFrameAssembler *assembler, int sequence,
        const QByteArray &packet)
{
    return assembler->add(
            static_cast<quint16>(sequence),
            reinterpret_cast<const quint8 *>(packet.constData()),
            packet.size());
}

} // namespace

void Px4FlowFrameAssemblerTest::assemblesActualVideoFrameOutOfOrder()
{
    Px4FlowFrameAssembler assembler;
    const auto descriptor = descriptorFor(4096, 64, 64, 253);
    QCOMPARE(descriptor.packets, quint16(17));
    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);

    QByteArray expected(4096, char(0));
    for (int index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<char>(index % 251);
    }

    QCOMPARE(addPacket(&assembler, 16,
                       wirePacket(expected, 16, descriptor.payload)).result,
             PacketResult::Accepted);
    for (int sequence = 0; sequence < 15; ++sequence) {
        QCOMPARE(addPacket(&assembler, sequence,
                           wirePacket(expected, sequence,
                                      descriptor.payload)).result,
                 PacketResult::Accepted);
    }

    const auto completed = addPacket(
            &assembler, 15, wirePacket(expected, 15, descriptor.payload));
    QCOMPARE(completed.result, PacketResult::Completed);
    QVERIFY(completed.completedFrame);
    QCOMPARE(completed.completedFrame->width, quint16(64));
    QCOMPARE(completed.completedFrame->height, quint16(64));
    QCOMPARE(completed.completedFrame->grayscale, expected);
    QCOMPARE(completed.completedFrame->generation, quint64(1));
    QVERIFY(!assembler.active());
}

void Px4FlowFrameAssemblerTest::acceptsActualFocusDescriptor()
{
    Px4FlowFrameAssembler assembler;
    const auto descriptor = descriptorFor(90240, 376, 240, 253);
    QCOMPARE(descriptor.packets, quint16(357));
    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    QVERIFY(assembler.active());
    QCOMPARE(assembler.expectedPackets(), 357);
    QCOMPARE(assembler.receivedPackets(), 0);
}

void Px4FlowFrameAssemblerTest::lastPacketDoesNotCompleteMissingFrame()
{
    Px4FlowFrameAssembler assembler;
    const auto descriptor = descriptorFor(4096, 64, 64, 253);
    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    const QByteArray image(4096, char(0x2a));

    for (int sequence = 0; sequence < descriptor.packets; ++sequence) {
        if (sequence == 15) continue;
        const auto outcome = addPacket(
                &assembler, sequence,
                wirePacket(image, sequence, descriptor.payload));
        QCOMPARE(outcome.result, PacketResult::Accepted);
        QVERIFY(!outcome.completedFrame);
    }
    QVERIFY(assembler.active());
    QCOMPARE(assembler.receivedPackets(), 16);
}

void Px4FlowFrameAssemblerTest::duplicatePacketsAreIdempotentAndConflictsFail()
{
    Px4FlowFrameAssembler assembler;
    const auto descriptor = descriptorFor(8, 4, 2, 3);
    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    const QByteArray image = QByteArray::fromHex("0001020304050607");
    const QByteArray first = wirePacket(image, 0, descriptor.payload);

    QCOMPARE(addPacket(&assembler, 0, first).result,
             PacketResult::Accepted);
    QCOMPARE(addPacket(&assembler, 0, first).result,
             PacketResult::Duplicate);
    QCOMPARE(assembler.receivedPackets(), 1);

    QByteArray conflicting = first;
    conflicting[0] = char(0x55);
    QCOMPARE(addPacket(&assembler, 0, conflicting).result,
             PacketResult::ConflictingDuplicate);
    QVERIFY(!assembler.active());
    QCOMPARE(addPacket(&assembler, 1,
                       wirePacket(image, 1, descriptor.payload)).result,
             PacketResult::IgnoredNoFrame);
}

void Px4FlowFrameAssemblerTest::negotiatedPayloadDoesNotCopyWirePadding()
{
    Px4FlowFrameAssembler assembler;
    const auto descriptor = descriptorFor(8, 4, 2, 3);
    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    const QByteArray expected = QByteArray::fromHex("0001020304050607");

    QCOMPARE(addPacket(&assembler, 2,
                       wirePacket(expected, 2, descriptor.payload,
                                  char(0x66))).result,
             PacketResult::Accepted);
    QCOMPARE(addPacket(&assembler, 1,
                       wirePacket(expected, 1, descriptor.payload,
                                  char(0x55))).result,
             PacketResult::Accepted);
    const auto completed = addPacket(
            &assembler, 0,
            wirePacket(expected, 0, descriptor.payload, char(0x44)));
    QCOMPARE(completed.result, PacketResult::Completed);
    QVERIFY(completed.completedFrame);
    QCOMPARE(completed.completedFrame->grayscale, expected);
}

void Px4FlowFrameAssemblerTest::newOrInvalidHandshakeClearsPartialFrame()
{
    Px4FlowFrameAssembler assembler;
    const auto valid = descriptorFor(8, 4, 2, 3);
    QCOMPARE(assembler.begin(valid), BeginResult::Started);
    const QByteArray image(8, char(1));
    QCOMPARE(addPacket(&assembler, 0,
                       wirePacket(image, 0, valid.payload)).result,
             PacketResult::Accepted);

    QCOMPARE(assembler.begin(valid), BeginResult::Started);
    QCOMPARE(assembler.generation(), quint64(2));
    QCOMPARE(assembler.receivedPackets(), 0);

    auto invalid = valid;
    invalid.size = 0;
    QString error;
    QCOMPARE(assembler.begin(invalid, &error),
             BeginResult::InvalidDescriptor);
    QVERIFY(!error.isEmpty());
    QVERIFY(!assembler.active());
    QCOMPARE(assembler.generation(), quint64(2));
}

void Px4FlowFrameAssemblerTest::rejectsInvalidDescriptors()
{
    Px4FlowFrameAssembler assembler(10);
    const auto valid = descriptorFor(8, 4, 2, 3);

    auto descriptor = valid;
    descriptor.type = 0;
    QCOMPARE(assembler.begin(descriptor), BeginResult::UnsupportedType);

    descriptor = valid;
    descriptor.width = 0;
    QCOMPARE(assembler.begin(descriptor), BeginResult::InvalidDescriptor);

    descriptor = valid;
    descriptor.size = 7;
    descriptor.packets = 3;
    QCOMPARE(assembler.begin(descriptor), BeginResult::InvalidDescriptor);

    descriptor = valid;
    descriptor.payload = 254;
    QCOMPARE(assembler.begin(descriptor), BeginResult::InvalidDescriptor);

    descriptor = valid;
    descriptor.packets = 2;
    QCOMPARE(assembler.begin(descriptor), BeginResult::InvalidDescriptor);

    descriptor = descriptorFor(12, 4, 3, 3);
    QCOMPARE(assembler.begin(descriptor), BeginResult::FrameTooLarge);
    QVERIFY(!assembler.active());
    QCOMPARE(assembler.generation(), quint64(0));

    Px4FlowFrameAssembler capped(
            Px4FlowFrameAssembler::AbsoluteMaximumFrameBytes + 1U);
    QCOMPARE(capped.maximumFrameBytes(),
             Px4FlowFrameAssembler::AbsoluteMaximumFrameBytes);
}

void Px4FlowFrameAssemblerTest::rejectsInvalidPacketsTransactionally()
{
    Px4FlowFrameAssembler assembler;
    const auto descriptor = descriptorFor(8, 4, 2, 3);
    const QByteArray image(8, char(1));
    const QByteArray packet = wirePacket(image, 0, descriptor.payload);

    QCOMPARE(addPacket(&assembler, 0, packet).result,
             PacketResult::IgnoredNoFrame);

    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    QCOMPARE(addPacket(&assembler, descriptor.packets, packet).result,
             PacketResult::InvalidSequence);
    QVERIFY(!assembler.active());

    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    QCOMPARE(assembler.add(0, nullptr, packet.size()).result,
             PacketResult::InvalidPayload);
    QVERIFY(!assembler.active());

    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    QCOMPARE(assembler.add(
                     0,
                     reinterpret_cast<const quint8 *>(packet.constData()),
                     2).result,
             PacketResult::InvalidPayload);
    QVERIFY(!assembler.active());

    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    QByteArray oversized = packet;
    oversized.append(char(0));
    QCOMPARE(addPacket(&assembler, 0, oversized).result,
             PacketResult::InvalidPayload);
    QVERIFY(!assembler.active());
}

void Px4FlowFrameAssemblerTest::completionMovesOneFrameAndGenerationIsMonotonic()
{
    Px4FlowFrameAssembler assembler;
    const auto descriptor = descriptorFor(1, 1, 1, 1);
    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    const quint8 pixel = 173;
    auto completed = assembler.add(0, &pixel, 1);
    QCOMPARE(completed.result, PacketResult::Completed);
    QVERIFY(completed.completedFrame);
    QCOMPARE(completed.completedFrame->grayscale,
             QByteArray(1, static_cast<char>(pixel)));
    QCOMPARE(completed.completedFrame->generation, quint64(1));

    QCOMPARE(assembler.add(0, &pixel, 1).result,
             PacketResult::IgnoredNoFrame);
    assembler.reset();
    QCOMPARE(assembler.generation(), quint64(1));
    QCOMPARE(assembler.begin(descriptor), BeginResult::Started);
    QCOMPARE(assembler.generation(), quint64(2));
}

QTEST_APPLESS_MAIN(Px4FlowFrameAssemblerTest)

#include "test_px4flowframeassembler.moc"
