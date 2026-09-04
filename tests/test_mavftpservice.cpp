#include <QtTest>

#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/MavFtpProtocol.h"
#include "comm/MavFtpService.h"
#include "comm/VehicleTargetManager.h"

#include <QPointer>
#include <QSignalSpy>
#include <QVector>

#include <cstring>

namespace {

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(
    int linkId, int systemId = 42, int componentId = 1)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = componentId;
    value.linkName = QStringLiteral("link%1").arg(linkId);
    return value;
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    return state == MAVLINK_FRAMING_OK ? message : mavlink_message_t{};
}

MavFtpProtocol::PayloadHeader ftpPayload(const CapturedFrame &frame)
{
    const mavlink_message_t message = decodeFrame(frame.bytes);
    Q_ASSERT(message.msgid == MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL);
    mavlink_file_transfer_protocol_t outer{};
    mavlink_msg_file_transfer_protocol_decode(&message, &outer);
    const QByteArray wire(
        reinterpret_cast<const char *>(outer.payload),
        MavFtpProtocol::PayloadSize);
    MavFtpProtocol::PayloadHeader payload;
    QString error;
    Q_ASSERT(MavFtpProtocol::decodePayload(wire, &payload, &error));
    Q_UNUSED(error)
    return payload;
}

QByteArray littleEndian32(quint32 value)
{
    QByteArray bytes(4, '\0');
    bytes[0] = static_cast<char>(value & 0xff);
    bytes[1] = static_cast<char>((value >> 8) & 0xff);
    bytes[2] = static_cast<char>((value >> 16) & 0xff);
    bytes[3] = static_cast<char>((value >> 24) & 0xff);
    return bytes;
}

mavlink_message_t ftpResponse(
    const MavFtpProtocol::PayloadHeader &request,
    MavFtpProtocol::Opcode opcode,
    const QByteArray &data = QByteArray(), int session = -1,
    qint64 offset = -1, int sequenceDelta = 1,
    int sourceSystem = 42, int sourceComponent = 1,
    int targetSystem = 250, int targetComponent = 190)
{
    MavFtpProtocol::PayloadHeader response;
    response.sequence = static_cast<quint16>(
        request.sequence + sequenceDelta);
    response.session = session < 0
        ? request.session : static_cast<quint8>(session);
    response.opcode = opcode;
    response.requestOpcode = request.opcode;
    response.offset = offset < 0
        ? request.offset : static_cast<quint32>(offset);
    response.data = data;
    response.size = static_cast<quint8>(data.size());
    QString error;
    const QByteArray wire = MavFtpProtocol::encodePayload(response, &error);
    Q_ASSERT_X(wire.size() == MavFtpProtocol::PayloadSize,
               "ftpResponse", qPrintable(error));
    quint8 payload[MavFtpProtocol::PayloadSize]{};
    std::memcpy(payload, wire.constData(), sizeof(payload));
    mavlink_message_t message{};
    mavlink_msg_file_transfer_protocol_pack(
        static_cast<quint8>(sourceSystem),
        static_cast<quint8>(sourceComponent), &message, 0,
        static_cast<quint8>(targetSystem),
        static_cast<quint8>(targetComponent), payload);
    return message;
}

MavFtpServiceInterface::Result resultAt(
    const QSignalSpy &spy, int index = 0)
{
    return spy.at(index).at(0)
        .value<MavFtpServiceInterface::Result>();
}

struct Fixture
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    bool writerAccepts = true;
    ExactLinkTransmitter transmitter;
    MavFtpService service;

    Fixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
              if (!writerAccepts) {
                  return false;
              }
              frames.append({linkId, bytes});
              return true;
          })
        , service(&targets, &transmitter)
    {
        service.setLocalIdentity(250, 190);
        service.setTimingForTesting(20, 1);
    }

    VehicleTargetLease select(
        int linkId = 9, int systemId = 42, int componentId = 1)
    {
        targets.observeEndpoint(endpoint(linkId, systemId, componentId));
        targets.selectTarget(linkId, systemId, componentId);
        return targets.acquireTarget();
    }

    MavFtpProtocol::PayloadHeader requestAt(int index) const
    {
        return ftpPayload(frames.at(index));
    }

    void ack(int frameIndex, const QByteArray &data = QByteArray(),
             int session = -1, qint64 offset = -1)
    {
        const MavFtpProtocol::PayloadHeader request = requestAt(frameIndex);
        service.observeMessage(
            frames.at(frameIndex).linkId,
            ftpResponse(request, MavFtpProtocol::Opcode::Ack,
                        data, session, offset));
    }

    void nak(int frameIndex, MavFtpProtocol::ErrorCode errorCode,
             int session = -1, qint64 offset = -1)
    {
        QByteArray data(1, static_cast<char>(errorCode));
        if (errorCode == MavFtpProtocol::ErrorCode::FailErrno) {
            data.append(static_cast<char>(5));
        }
        const MavFtpProtocol::PayloadHeader request = requestAt(frameIndex);
        service.observeMessage(
            frames.at(frameIndex).linkId,
            ftpResponse(request, MavFtpProtocol::Opcode::Nak,
                        data, session, offset));
    }
};

} // namespace

class MavFtpServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void listPaginatesAndCompletesOnEof();
    void downloadUsesNonBurstChunksAndCleansUp();
    void uploadWritesChunksVerifiesCrcAndRunsCrud();
    void retryTimeoutIsBounded();
    void slowCreateOpenAndCrcUseStageSpecificTimeouts();
    void remoteNakFailsButEofTerminatesList();
    void wrongLinkSourceTargetAndSequenceAreIgnored();
    void progressCallbackCanCancelAndRestartSafely();
    void nestedTargetChangeCannotRestartDestructiveOperation();
    void targetSwitchCancelsAndLateReplyIsIgnored();
    void explicitCancelTerminatesAndResetsBestEffort();
    void uploadCrcMismatchFailsAfterCleanup();
    void completionCanRestartReentrantlyAndDestructionIsSafe();
};

void MavFtpServiceTest::listPaginatesAndCompletesOnEof()
{
    Fixture fixture;
    fixture.select();
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);

    QCOMPARE(fixture.service.startList(QStringLiteral("/")),
             MavFtpServiceInterface::StartResult::Started);
    QCOMPARE(fixture.frames.size(), 1);
    const auto first = fixture.requestAt(0);
    QCOMPARE(first.opcode, MavFtpProtocol::Opcode::ListDirectory);
    QCOMPARE(first.offset, quint32(0));

    QByteArray page;
    page.append('D');
    page.append("logs", 4);
    page.append('\0');
    page.append('F');
    page.append("flight.bin\t3", 12);
    page.append('\0');
    page.append('S');
    page.append('\0');
    fixture.ack(0, page);

    QCOMPARE(fixture.frames.size(), 2);
    const auto second = fixture.requestAt(1);
    QCOMPARE(second.opcode, MavFtpProtocol::Opcode::ListDirectory);
    QCOMPARE(second.offset, quint32(3));
    fixture.nak(1, MavFtpProtocol::ErrorCode::EndOfFile);

    QCOMPARE(finished.count(), 1);
    const auto result = resultAt(finished);
    QVERIFY(result.succeeded());
    QCOMPARE(result.operation,
             MavFtpServiceInterface::Operation::ListDirectory);
    QCOMPARE(result.entries.size(), 2);
    QCOMPARE(result.entries.at(0).type,
             MavFtpProtocol::DirectoryEntryType::Directory);
    QCOMPARE(result.entries.at(0).name, QStringLiteral("logs"));
    QCOMPARE(result.entries.at(1).type,
             MavFtpProtocol::DirectoryEntryType::File);
    QCOMPARE(result.entries.at(1).name, QStringLiteral("flight.bin"));
    QCOMPARE(result.entries.at(1).size, quint64(3));
    QVERIFY(!fixture.service.isBusy());
}

void MavFtpServiceTest::downloadUsesNonBurstChunksAndCleansUp()
{
    Fixture fixture;
    fixture.select();
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);
    const QByteArray expected = QByteArray(80, 'a') + QByteArray(20, 'b');

    QCOMPARE(fixture.service.startDownload(QStringLiteral("/flight.bin")),
             MavFtpServiceInterface::StartResult::Started);
    QCOMPARE(fixture.requestAt(0).opcode,
             MavFtpProtocol::Opcode::ResetSessions);
    fixture.ack(0);
    QCOMPARE(fixture.requestAt(1).opcode,
             MavFtpProtocol::Opcode::OpenFileReadOnly);
    fixture.ack(1, littleEndian32(expected.size()), 7);

    auto read = fixture.requestAt(2);
    QCOMPARE(read.opcode, MavFtpProtocol::Opcode::ReadFile);
    QCOMPARE(read.session, quint8(7));
    QCOMPARE(read.offset, quint32(0));
    QCOMPARE(read.size, quint8(80));
    fixture.ack(2, expected.left(80), 7, 0);

    read = fixture.requestAt(3);
    QCOMPARE(read.opcode, MavFtpProtocol::Opcode::ReadFile);
    QCOMPARE(read.offset, quint32(80));
    QCOMPARE(read.size, quint8(20));
    fixture.ack(3, expected.mid(80), 7, 80);

    QCOMPARE(fixture.requestAt(4).opcode,
             MavFtpProtocol::Opcode::TerminateSession);
    QCOMPARE(fixture.requestAt(4).session, quint8(7));
    fixture.ack(4);
    QCOMPARE(fixture.requestAt(5).opcode,
             MavFtpProtocol::Opcode::ResetSessions);
    fixture.ack(5);

    QCOMPARE(finished.count(), 1);
    const auto result = resultAt(finished);
    QVERIFY(result.succeeded());
    QCOMPARE(result.data, expected);
}

void MavFtpServiceTest::uploadWritesChunksVerifiesCrcAndRunsCrud()
{
    Fixture fixture;
    fixture.select();
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);
    const QByteArray data = QByteArray(80, 'x') + QByteArray(20, 'y');

    QCOMPARE(fixture.service.startUpload(
                 QStringLiteral("/new.bin"), data),
             MavFtpServiceInterface::StartResult::Started);
    fixture.ack(0);
    QCOMPARE(fixture.requestAt(1).opcode,
             MavFtpProtocol::Opcode::CreateFile);
    fixture.ack(1, QByteArray(), 4);

    auto write = fixture.requestAt(2);
    QCOMPARE(write.opcode, MavFtpProtocol::Opcode::WriteFile);
    QCOMPARE(write.session, quint8(4));
    QCOMPARE(write.offset, quint32(0));
    QCOMPARE(write.data, data.left(80));
    fixture.ack(2, QByteArray(), 4, 0);
    write = fixture.requestAt(3);
    QCOMPARE(write.offset, quint32(80));
    QCOMPARE(write.data, data.mid(80));
    fixture.ack(3, QByteArray(), 4, 80);

    QCOMPARE(fixture.requestAt(4).opcode,
             MavFtpProtocol::Opcode::TerminateSession);
    QCOMPARE(fixture.requestAt(4).session, quint8(4));
    fixture.ack(4);
    QCOMPARE(fixture.requestAt(5).opcode,
             MavFtpProtocol::Opcode::ResetSessions);
    fixture.ack(5);
    QCOMPARE(fixture.requestAt(6).opcode,
             MavFtpProtocol::Opcode::CalculateFileCrc32);
    const quint32 crc = MavFtpProtocol::crc32(data);
    fixture.ack(6, littleEndian32(crc));

    QCOMPARE(finished.count(), 1);
    auto result = resultAt(finished);
    QVERIFY(result.succeeded());
    QCOMPARE(result.localCrc, crc);
    QCOMPARE(result.remoteCrc, crc);

    QCOMPARE(fixture.service.startMakeDirectory(QStringLiteral("/logs")),
             MavFtpServiceInterface::StartResult::Started);
    QCOMPARE(fixture.requestAt(7).opcode,
             MavFtpProtocol::Opcode::CreateDirectory);
    fixture.ack(7);
    QCOMPARE(fixture.service.startRemoveFile(QStringLiteral("/new.bin")),
             MavFtpServiceInterface::StartResult::Started);
    QCOMPARE(fixture.requestAt(8).opcode,
             MavFtpProtocol::Opcode::RemoveFile);
    fixture.ack(8);
    QCOMPARE(fixture.service.startRemoveDirectory(QStringLiteral("/logs")),
             MavFtpServiceInterface::StartResult::Started);
    QCOMPARE(fixture.requestAt(9).opcode,
             MavFtpProtocol::Opcode::RemoveDirectory);
    fixture.ack(9);
    QCOMPARE(finished.count(), 4);
    result = resultAt(finished, 3);
    QVERIFY(result.succeeded());
    QCOMPARE(result.operation,
             MavFtpServiceInterface::Operation::RemoveDirectory);
}

void MavFtpServiceTest::retryTimeoutIsBounded()
{
    Fixture fixture;
    fixture.select();
    fixture.service.setTimingForTesting(10, 1);
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);

    QCOMPARE(fixture.service.startList(QStringLiteral("/")),
             MavFtpServiceInterface::StartResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.frames.size(), 2, 100);
    QCOMPARE(fixture.requestAt(0).sequence,
             fixture.requestAt(1).sequence);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 100);
    const auto result = resultAt(finished);
    QVERIFY(!result.succeeded());
    QVERIFY(result.error.contains(QStringLiteral("timed out"),
                                  Qt::CaseInsensitive));
    QCOMPARE(fixture.frames.size(), 2);
}

void MavFtpServiceTest::slowCreateOpenAndCrcUseStageSpecificTimeouts()
{
    QCOMPARE(MavFtpService::DefaultOpenCreateTimeoutMs, 2000);
    QCOMPARE(MavFtpService::DefaultCrcTimeoutMs, 30000);

    Fixture fixture;
    fixture.select();
    fixture.service.setTimingForTesting(10, 0, 80, 80);
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);
    const QByteArray data("x");

    QCOMPARE(fixture.service.startUpload(QStringLiteral("/slow.bin"), data),
             MavFtpServiceInterface::StartResult::Started);
    fixture.ack(0); // Reset -> CreateFile (slow-operation timeout).
    QCOMPARE(fixture.requestAt(1).opcode,
             MavFtpProtocol::Opcode::CreateFile);
    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 2);
    QCOMPARE(finished.count(), 0);

    fixture.ack(1, QByteArray(), 4);
    fixture.ack(2, QByteArray(), 4, 0);
    QCOMPARE(fixture.requestAt(3).opcode,
             MavFtpProtocol::Opcode::TerminateSession);
    fixture.ack(3);
    fixture.ack(4);
    QCOMPARE(fixture.requestAt(5).opcode,
             MavFtpProtocol::Opcode::CalculateFileCrc32);
    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 6);
    QCOMPARE(finished.count(), 0);
    fixture.ack(5, littleEndian32(MavFtpProtocol::crc32(data)));
    QCOMPARE(finished.count(), 1);
    QVERIFY(resultAt(finished).succeeded());

    QCOMPARE(fixture.service.startDownload(QStringLiteral("/slow.bin")),
             MavFtpServiceInterface::StartResult::Started);
    fixture.ack(6); // Reset -> OpenFileReadOnly (same slow timeout).
    QCOMPARE(fixture.requestAt(7).opcode,
             MavFtpProtocol::Opcode::OpenFileReadOnly);
    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 8);
    QCOMPARE(finished.count(), 1);
    fixture.service.cancel();
}

void MavFtpServiceTest::remoteNakFailsButEofTerminatesList()
{
    Fixture fixture;
    fixture.select();
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);

    QCOMPARE(fixture.service.startMakeDirectory(QStringLiteral("/existing")),
             MavFtpServiceInterface::StartResult::Started);
    fixture.nak(0, MavFtpProtocol::ErrorCode::FileExists);
    QCOMPARE(finished.count(), 1);
    QVERIFY(!resultAt(finished).succeeded());
    QVERIFY(resultAt(finished).error.contains(
        QStringLiteral("file exists"), Qt::CaseInsensitive));

    QCOMPARE(fixture.service.startList(QStringLiteral("/empty")),
             MavFtpServiceInterface::StartResult::Started);
    fixture.nak(1, MavFtpProtocol::ErrorCode::EndOfFile);
    QCOMPARE(finished.count(), 2);
    QVERIFY(resultAt(finished, 1).succeeded());
    QVERIFY(resultAt(finished, 1).entries.isEmpty());

    QCOMPARE(fixture.service.startDownload(QStringLiteral("/broken.bin")),
             MavFtpServiceInterface::StartResult::Started);
    fixture.ack(2);
    fixture.ack(3, littleEndian32(100), 7);
    fixture.ack(4, QByteArray(80, 'x'), 7, 0);
    QCOMPARE(fixture.requestAt(5).offset, quint32(80));
    // ArduPilot error replies do not reliably copy the failing request offset.
    fixture.nak(5, MavFtpProtocol::ErrorCode::FailErrno, 7, 0);
    QCOMPARE(fixture.requestAt(6).opcode,
             MavFtpProtocol::Opcode::TerminateSession);
    fixture.ack(6);
    fixture.ack(7);
    QCOMPARE(finished.count(), 3);
    QVERIFY(!resultAt(finished, 2).succeeded());
    QVERIFY(resultAt(finished, 2).error.contains(
        QStringLiteral("remote errno 5"), Qt::CaseInsensitive));
}

void MavFtpServiceTest::wrongLinkSourceTargetAndSequenceAreIgnored()
{
    Fixture fixture;
    fixture.select();
    fixture.service.setTimingForTesting(500, 0);
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);

    QCOMPARE(fixture.service.startMakeDirectory(QStringLiteral("/new")),
             MavFtpServiceInterface::StartResult::Started);
    const auto request = fixture.requestAt(0);
    const mavlink_message_t correct = ftpResponse(
        request, MavFtpProtocol::Opcode::Ack);

    mavlink_file_transfer_protocol_t malformedOuter{};
    const mavlink_message_t unrelated = ftpResponse(
        request, MavFtpProtocol::Opcode::Ack,
        QByteArray(), -1, -1, 2);
    mavlink_msg_file_transfer_protocol_decode(&unrelated, &malformedOuter);
    malformedOuter.payload[3] = 0xff;
    mavlink_message_t unrelatedMalformed{};
    mavlink_msg_file_transfer_protocol_pack(
        42, 1, &unrelatedMalformed, 0, 250, 190,
        malformedOuter.payload);
    fixture.service.observeMessage(9, unrelatedMalformed);

    fixture.service.observeMessage(10, correct); // wrong physical link
    fixture.service.observeMessage(
        9, ftpResponse(request, MavFtpProtocol::Opcode::Ack,
                       QByteArray(), -1, -1, 1, 43)); // wrong source
    fixture.service.observeMessage(
        9, ftpResponse(request, MavFtpProtocol::Opcode::Ack,
                       QByteArray(), -1, -1, 1, 42, 1,
                       251)); // wrong target system
    fixture.service.observeMessage(
        9, ftpResponse(request, MavFtpProtocol::Opcode::Ack,
                       QByteArray(), -1, -1, 2)); // wrong FTP sequence
    QCOMPARE(finished.count(), 0);
    QVERIFY(fixture.service.isBusy());

    fixture.service.observeMessage(9, correct);
    QCOMPARE(finished.count(), 1);
    QVERIFY(resultAt(finished).succeeded());
}

void MavFtpServiceTest::progressCallbackCanCancelAndRestartSafely()
{
    Fixture fixture;
    fixture.select();
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);
    bool restarted = false;
    connect(&fixture.service, &MavFtpServiceInterface::progressChanged,
            &fixture.service,
            [&fixture, &restarted](qulonglong, qint64, qint64) {
        if (restarted) {
            return;
        }
        restarted = true;
        fixture.service.cancel();
        QCOMPARE(fixture.service.startMakeDirectory(
                     QStringLiteral("/replacement")),
                 MavFtpServiceInterface::StartResult::Started);
    });

    QCOMPARE(fixture.service.startList(QStringLiteral("/")),
             MavFtpServiceInterface::StartResult::Started);
    QByteArray page;
    page.append('D');
    page.append("logs", 4);
    page.append('\0');
    fixture.ack(0, page);

    QVERIFY(restarted);
    QCOMPARE(fixture.frames.size(), 3);
    QCOMPARE(fixture.requestAt(1).opcode,
             MavFtpProtocol::Opcode::ResetSessions);
    QCOMPARE(fixture.requestAt(2).opcode,
             MavFtpProtocol::Opcode::CreateDirectory);
    QCOMPARE(fixture.service.operation(),
             MavFtpServiceInterface::Operation::MakeDirectory);
    fixture.ack(2);
    QCOMPARE(finished.count(), 2);
    QVERIFY(resultAt(finished, 0).cancelled);
    QVERIFY(resultAt(finished, 1).succeeded());
}

void MavFtpServiceTest::nestedTargetChangeCannotRestartDestructiveOperation()
{
    Fixture fixture;
    fixture.select(9);
    fixture.targets.observeEndpoint(endpoint(10));
    MavFtpServiceInterface::StartResult nestedStart =
        MavFtpServiceInterface::StartResult::Started;
    connect(&fixture.service,
            &MavFtpServiceInterface::operationFinished,
            &fixture.service,
            [&fixture, &nestedStart](const MavFtpServiceInterface::Result &) {
        nestedStart = fixture.service.startRemoveFile(
            QStringLiteral("/must-not-run"));
    });

    QCOMPARE(fixture.service.startList(QStringLiteral("/")),
             MavFtpServiceInterface::StartResult::Started);
    QVERIFY(fixture.targets.selectTarget(10, 42, 1));
    QCOMPARE(nestedStart, MavFtpServiceInterface::StartResult::StaleTarget);
    QVERIFY(!fixture.service.isBusy());
    QCOMPARE(fixture.frames.size(), 1);
}

void MavFtpServiceTest::targetSwitchCancelsAndLateReplyIsIgnored()
{
    Fixture fixture;
    fixture.select(9);
    fixture.targets.observeEndpoint(endpoint(10));
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);

    QCOMPARE(fixture.service.startList(QStringLiteral("/")),
             MavFtpServiceInterface::StartResult::Started);
    const auto request = fixture.requestAt(0);
    QVERIFY(fixture.targets.selectTarget(10, 42, 1));
    QCOMPARE(finished.count(), 1);
    QVERIFY(resultAt(finished).cancelled);
    QVERIFY(!fixture.service.isBusy());

    fixture.service.observeMessage(
        9, ftpResponse(request, MavFtpProtocol::Opcode::Ack));
    QCOMPARE(finished.count(), 1);
}

void MavFtpServiceTest::explicitCancelTerminatesAndResetsBestEffort()
{
    Fixture fixture;
    fixture.select();
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);

    QCOMPARE(fixture.service.startDownload(QStringLiteral("/large.bin")),
             MavFtpServiceInterface::StartResult::Started);
    fixture.ack(0);
    fixture.ack(1, littleEndian32(100), 6);
    QCOMPARE(fixture.requestAt(2).opcode,
             MavFtpProtocol::Opcode::ReadFile);

    fixture.service.cancel();
    QCOMPARE(finished.count(), 1);
    QVERIFY(resultAt(finished).cancelled);
    QVERIFY(!fixture.service.isBusy());
    QCOMPARE(fixture.requestAt(3).opcode,
             MavFtpProtocol::Opcode::TerminateSession);
    QCOMPARE(fixture.requestAt(3).session, quint8(6));
    QCOMPARE(fixture.requestAt(4).opcode,
             MavFtpProtocol::Opcode::ResetSessions);
}

void MavFtpServiceTest::uploadCrcMismatchFailsAfterCleanup()
{
    Fixture fixture;
    fixture.select();
    QSignalSpy finished(
        &fixture.service, &MavFtpServiceInterface::operationFinished);
    const QByteArray data("payload");

    QCOMPARE(fixture.service.startUpload(QStringLiteral("/bad.bin"), data),
             MavFtpServiceInterface::StartResult::Started);
    fixture.ack(0);
    fixture.ack(1, QByteArray(), 3);
    fixture.ack(2, QByteArray(), 3, 0);
    QCOMPARE(fixture.requestAt(3).opcode,
             MavFtpProtocol::Opcode::TerminateSession);
    fixture.ack(3);
    QCOMPARE(fixture.requestAt(4).opcode,
             MavFtpProtocol::Opcode::ResetSessions);
    fixture.ack(4);
    QCOMPARE(fixture.requestAt(5).opcode,
             MavFtpProtocol::Opcode::CalculateFileCrc32);
    fixture.ack(5, littleEndian32(
        MavFtpProtocol::crc32(data) ^ 0xffffffffu));

    QCOMPARE(finished.count(), 1);
    const auto result = resultAt(finished);
    QVERIFY(!result.succeeded());
    QVERIFY(result.error.contains(QStringLiteral("CRC mismatch"),
                                  Qt::CaseInsensitive));
    QVERIFY(!result.cancelled);
}

void MavFtpServiceTest::completionCanRestartReentrantlyAndDestructionIsSafe()
{
    Fixture fixture;
    fixture.select();
    bool restarted = false;
    connect(&fixture.service,
            &MavFtpServiceInterface::operationFinished,
            &fixture.service,
            [&fixture, &restarted](const MavFtpServiceInterface::Result &) {
        if (!restarted) {
            restarted = true;
            QCOMPARE(fixture.service.startMakeDirectory(
                         QStringLiteral("/second")),
                     MavFtpServiceInterface::StartResult::Started);
        }
    });

    QCOMPARE(fixture.service.startMakeDirectory(QStringLiteral("/first")),
             MavFtpServiceInterface::StartResult::Started);
    fixture.ack(0);
    QVERIFY(restarted);
    QCOMPARE(fixture.frames.size(), 2);
    QCOMPARE(fixture.service.operation(),
             MavFtpServiceInterface::Operation::MakeDirectory);
    fixture.service.cancel();

    auto *service = new MavFtpService(
        &fixture.targets, &fixture.transmitter);
    service->setTimingForTesting(5, 0);
    QCOMPARE(service->startList(QStringLiteral("/")),
             MavFtpServiceInterface::StartResult::Started);
    QPointer<MavFtpService> guard(service);
    delete service;
    QVERIFY(guard.isNull());
    QTest::qWait(15);

    auto *targets = new VehicleTargetManager;
    auto *transmitter = new ExactLinkTransmitter(
        [](int, const QByteArray &) { return true; });
    auto *orphanSafeService = new MavFtpService(targets, transmitter);
    targets->observeEndpoint(endpoint(11));
    QVERIFY(targets->selectTarget(11, 42, 1));
    QCOMPARE(orphanSafeService->startList(QStringLiteral("/")),
             MavFtpServiceInterface::StartResult::Started);
    delete transmitter;
    delete orphanSafeService;
    delete targets;
}

QTEST_GUILESS_MAIN(MavFtpServiceTest)

#include "test_mavftpservice.moc"
