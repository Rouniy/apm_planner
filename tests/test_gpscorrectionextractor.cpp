#include "comm/GpsCorrectionExtractor.h"

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cstring>

#include <mavlink.h>

namespace
{
constexpr qint64 BaseTimestamp = 1700000000000000LL;

QByteArray timestampBytes(qint64 timestampUsec)
{
    QByteArray result(8, '\0');
    quint64 value = static_cast<quint64>(timestampUsec);
    for (int index = 7; index >= 0; --index) {
        result[index] = static_cast<char>(value & 0xffU);
        value >>= 8;
    }
    return result;
}

QByteArray frameBytes(mavlink_message_t &message)
{
    quint8 bytes[MAVLINK_MAX_PACKET_LEN]{};
    const int size = mavlink_msg_to_send_buffer(bytes, &message);
    return QByteArray(reinterpret_cast<const char *>(bytes), size);
}

template<typename Pack>
QByteArray packedFrame(bool mavlink1, Pack pack)
{
    mavlink_status_t *status = mavlink_get_channel_status(MAVLINK_COMM_0);
    const quint8 previousFlags = status->flags;
    if (mavlink1) {
        status->flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    } else {
        status->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    }
    mavlink_message_t message{};
    pack(&message);
    status->flags = previousFlags;
    return frameBytes(message);
}

QByteArray injectFrame(quint8 sender,
                       const QByteArray &payload,
                       bool mavlink1 = false,
                       int declaredLength = -1)
{
    quint8 data[MAVLINK_MSG_GPS_INJECT_DATA_FIELD_DATA_LEN]{};
    const int copied = std::min<int>(payload.size(), int(sizeof(data)));
    std::memcpy(data, payload.constData(), static_cast<size_t>(copied));
    const quint8 length = static_cast<quint8>(
        declaredLength >= 0 ? declaredLength : payload.size());
    return packedFrame(mavlink1, [&](mavlink_message_t *message) {
        mavlink_msg_gps_inject_data_pack(
            sender, 77, message, 42, 1, length, data);
    });
}

QByteArray rtcmFrame(quint8 sender,
                     const QByteArray &payload,
                     bool mavlink1 = false,
                     int declaredLength = -1)
{
    quint8 data[MAVLINK_MSG_GPS_RTCM_DATA_FIELD_DATA_LEN]{};
    const int copied = std::min<int>(payload.size(), int(sizeof(data)));
    std::memcpy(data, payload.constData(), static_cast<size_t>(copied));
    const quint8 length = static_cast<quint8>(
        declaredLength >= 0 ? declaredLength : payload.size());
    return packedFrame(mavlink1, [&](mavlink_message_t *message) {
        mavlink_msg_gps_rtcm_data_pack(
            sender, 88, message, 0x39, length, data);
    });
}

QByteArray heartbeatFrame(quint8 sender)
{
    return packedFrame(false, [=](mavlink_message_t *message) {
        mavlink_msg_heartbeat_pack(
            sender, 1, message, MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_ACTIVE);
    });
}

QByteArray rawCorrectionFrame(quint32 messageId,
                              bool mavlink1,
                              int wirePayloadLength)
{
    return packedFrame(mavlink1, [&](mavlink_message_t *message) {
        message->msgid = messageId;
        char *payload = _MAV_PAYLOAD_NON_CONST(message);
        std::memset(payload, 0, static_cast<size_t>(wirePayloadLength));
        // MAVLink 2 finalization trims trailing zero payload bytes. Keep the
        // requested outer length observable so oversized-frame rows actually
        // exercise the extractor's schema bound.
        if (wirePayloadLength > 0) {
            payload[wirePayloadLength - 1] = 1;
        }
        const quint8 crc = messageId == MAVLINK_MSG_ID_GPS_INJECT_DATA
            ? MAVLINK_MSG_ID_GPS_INJECT_DATA_CRC
            : MAVLINK_MSG_ID_GPS_RTCM_DATA_CRC;
        mavlink_finalize_message(
            message, 9, 77,
            static_cast<quint8>(wirePayloadLength),
            static_cast<quint8>(wirePayloadLength), crc);
    });
}

QByteArray shortTrimmedInjectFrame()
{
    return packedFrame(false, [](mavlink_message_t *message) {
        message->msgid = MAVLINK_MSG_ID_GPS_INJECT_DATA;
        char *payload = _MAV_PAYLOAD_NON_CONST(message);
        std::memset(payload, 0, 5);
        payload[0] = 42;
        payload[1] = 1;
        payload[2] = 5;
        payload[3] = static_cast<char>(0xaa);
        payload[4] = static_cast<char>(0xbb);
        mavlink_finalize_message(
            message, 6, 77, 5, 5,
            MAVLINK_MSG_ID_GPS_INJECT_DATA_CRC);
    });
}

QByteArray record(qint64 timestampUsec, const QByteArray &frame)
{
    return timestampBytes(timestampUsec) + frame;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(data) != data.size()) {
        return false;
    }
    file.close();
    return true;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
}

class GpsCorrectionExtractorTest final : public QObject
{
    Q_OBJECT

private slots:
    void extractsBothMessageTypesInWireOrder();
    void writesOnlyDeclaredBytesIncludingZeros();
    void invalidDeclaredLengthPreservesOldOutput_data();
    void invalidDeclaredLengthPreservesOldOutput();
    void malformedOuterWireLengthPreservesOldOutput_data();
    void malformedOuterWireLengthPreservesOldOutput();
    void crcResyncAndTruncatedTailAreReported();
    void cancellationNeverPublishes_data();
    void cancellationNeverPublishes();
    void finalProgressCallbackCanCancelBeforeCommit();
    void emptyAndNoCorrectionLogsCreateEmptyOutput_data();
    void emptyAndNoCorrectionLogsCreateEmptyOutput();
    void rejectsSameAndAliasedPaths();
    void outputFailuresAreTruthful();
};

void GpsCorrectionExtractorTest::extractsBothMessageTypesInWireOrder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("mixed.tlog"));
    const QString output = directory.filePath(QStringLiteral("mixed.dat"));
    const QByteArray first = QByteArray::fromHex("010203");
    const QByteArray second = QByteArray::fromHex("040005");
    const QByteArray third = QByteArray::fromHex("06");
    QVERIFY(writeFile(
        input,
        record(BaseTimestamp, heartbeatFrame(1))
            + record(BaseTimestamp + 1, injectFrame(1, first))
            + record(BaseTimestamp + 2, rtcmFrame(2, second, true))
            + record(BaseTimestamp + 3, injectFrame(201, third, true))));

    QList<QPair<qint64, qint64>> progress;
    const auto result = GpsCorrectionExtractor::Extract(
        input, output, {},
        [&progress](qint64 processed, qint64 total) {
            progress.append(qMakePair(processed, total));
        });
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(!result.cancelled);
    QCOMPARE(result.messagesWritten, qint64(3));
    QCOMPARE(result.bytesWritten,
             qint64(first.size() + second.size() + third.size()));
    QCOMPARE(result.recordsRead, qint64(4));
    QCOMPARE(result.skippedBytes, qint64(0));
    QCOMPARE(result.rejectedFrames, qint64(0));
    QVERIFY(!result.truncatedTail);
    QCOMPARE(readFile(output), first + second + third);
    QCOMPARE(progress.size(), 4);
    QCOMPARE(progress.constLast().first, progress.constLast().second);
}

void GpsCorrectionExtractorTest::writesOnlyDeclaredBytesIncludingZeros()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("padding.tlog"));
    const QString output = directory.filePath(QStringLiteral("padding.dat"));
    QByteArray injectData = QByteArray::fromHex("aabb000000");
    QByteArray rtcmData = QByteArray::fromHex("cc0000");
    QVERIFY(writeFile(
        input,
        record(BaseTimestamp, injectFrame(3, injectData, false))
            + record(BaseTimestamp + 1, rtcmFrame(4, rtcmData, true))
            // Arrays on the wire are padded to their full message sizes, but
            // bytes beyond len must never leak into the output.
            + record(BaseTimestamp + 2,
                     injectFrame(5, QByteArray::fromHex("ddeeff"),
                                 false, 1))
            // A valid MAVLink 2 frame can omit the final three zero data
            // bytes; decode materializes them before the len-bounded append.
            + record(BaseTimestamp + 3, shortTrimmedInjectFrame())));

    const auto result = GpsCorrectionExtractor::Extract(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.messagesWritten, qint64(4));
    QCOMPARE(result.bytesWritten, qint64(14));
    QCOMPARE(readFile(output),
             injectData + rtcmData + QByteArray::fromHex("dd")
                 + QByteArray::fromHex("aabb000000"));
}

void GpsCorrectionExtractorTest::
invalidDeclaredLengthPreservesOldOutput_data()
{
    QTest::addColumn<bool>("rtcm");
    QTest::newRow("GPS_INJECT_DATA") << false;
    QTest::newRow("GPS_RTCM_DATA") << true;
}

void GpsCorrectionExtractorTest::invalidDeclaredLengthPreservesOldOutput()
{
    QFETCH(bool, rtcm);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("invalid.tlog"));
    const QString output = directory.filePath(QStringLiteral("existing.dat"));
    const QByteArray oldOutput("existing correction output");
    const QByteArray invalid = rtcm
        ? rtcmFrame(2, QByteArray(), false,
                    MAVLINK_MSG_GPS_RTCM_DATA_FIELD_DATA_LEN + 1)
        : injectFrame(2, QByteArray(), false,
                      MAVLINK_MSG_GPS_INJECT_DATA_FIELD_DATA_LEN + 1);
    QVERIFY(writeFile(input, record(BaseTimestamp, invalid)));
    QVERIFY(writeFile(output, oldOutput));

    const auto result = GpsCorrectionExtractor::Extract(input, output);
    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.recordsRead, qint64(1));
    QVERIFY(result.error.contains(QStringLiteral("declares")));
    QCOMPARE(readFile(output), oldOutput);
}

void GpsCorrectionExtractorTest::
malformedOuterWireLengthPreservesOldOutput_data()
{
    QTest::addColumn<quint32>("messageId");
    QTest::addColumn<bool>("mavlink1");
    QTest::addColumn<int>("wireLength");
    QTest::newRow("inject-short-v1")
        << quint32(MAVLINK_MSG_ID_GPS_INJECT_DATA) << true << 3;
    QTest::newRow("rtcm-short-v1")
        << quint32(MAVLINK_MSG_ID_GPS_RTCM_DATA) << true << 2;
    QTest::newRow("inject-oversized-v2")
        << quint32(MAVLINK_MSG_ID_GPS_INJECT_DATA) << false
        << int(MAVLINK_MSG_ID_GPS_INJECT_DATA_LEN + 1);
    QTest::newRow("rtcm-oversized-v2")
        << quint32(MAVLINK_MSG_ID_GPS_RTCM_DATA) << false
        << int(MAVLINK_MSG_ID_GPS_RTCM_DATA_LEN + 1);
}

void GpsCorrectionExtractorTest::
malformedOuterWireLengthPreservesOldOutput()
{
    QFETCH(quint32, messageId);
    QFETCH(bool, mavlink1);
    QFETCH(int, wireLength);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("outer.tlog"));
    const QString output = directory.filePath(QStringLiteral("existing.dat"));
    const QByteArray oldOutput("unchanged");
    QVERIFY(writeFile(
        input,
        record(BaseTimestamp,
               rawCorrectionFrame(messageId, mavlink1, wireLength))));
    QVERIFY(writeFile(output, oldOutput));

    const auto result = GpsCorrectionExtractor::Extract(input, output);
    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.recordsRead, qint64(1));
    QVERIFY(result.error.contains(QStringLiteral("payload length")));
    QCOMPARE(readFile(output), oldOutput);
}

void GpsCorrectionExtractorTest::crcResyncAndTruncatedTailAreReported()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("damaged.tlog"));
    const QString output = directory.filePath(QStringLiteral("damaged.dat"));
    const QByteArray first = QByteArray::fromHex("1020");
    const QByteArray last = QByteArray::fromHex("3040");
    QByteArray corrupt = injectFrame(8, QByteArray::fromHex("a1a2a3"));
    QVERIFY(corrupt.size() > 5);
    corrupt[corrupt.size() - 3] = static_cast<char>(
        corrupt.at(corrupt.size() - 3) ^ 0x5a);
    const QByteArray garbage = QByteArray::fromHex("0011fd22fe33");
    QVERIFY(writeFile(
        input,
        record(BaseTimestamp, injectFrame(1, first))
            + garbage
            + record(BaseTimestamp + 1, corrupt)
            + record(BaseTimestamp + 2, rtcmFrame(2, last))
            + QByteArray::fromHex("0102030405")));

    const auto result = GpsCorrectionExtractor::Extract(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(readFile(output), first + last);
    QCOMPARE(result.messagesWritten, qint64(2));
    QCOMPARE(result.recordsRead, qint64(2));
    QVERIFY(result.rejectedFrames >= 1);
    QVERIFY(result.skippedBytes >= garbage.size() + corrupt.size() + 8 + 5);
    QVERIFY(result.truncatedTail);
}

void GpsCorrectionExtractorTest::cancellationNeverPublishes_data()
{
    QTest::addColumn<int>("cancelAtCheck");
    QTest::addColumn<qint64>("stagedMessages");
    QTest::newRow("before-open") << 1 << qint64(0);
    // Extract() checks once, then TlogReader checks before each record.
    QTest::newRow("between-records") << 3 << qint64(1);
    // One record: initial, record, EOF, then the explicit pre-commit barrier.
    QTest::newRow("pre-commit") << 4 << qint64(1);
}

void GpsCorrectionExtractorTest::cancellationNeverPublishes()
{
    QFETCH(int, cancelAtCheck);
    QFETCH(qint64, stagedMessages);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("cancel.tlog"));
    const QString output = directory.filePath(QStringLiteral("existing.dat"));
    const QByteArray oldOutput("keep me");
    const QByteArray log = cancelAtCheck == 3
        ? record(BaseTimestamp, injectFrame(1, QByteArray("A")))
            + record(BaseTimestamp + 1,
                     injectFrame(1, QByteArray("B")))
        : record(BaseTimestamp, injectFrame(1, QByteArray("A")));
    QVERIFY(writeFile(input, log));
    QVERIFY(writeFile(output, oldOutput));
    int checks = 0;

    const auto result = GpsCorrectionExtractor::Extract(
        input, output,
        [&checks, cancelAtCheck]() {
            return ++checks == cancelAtCheck;
        });
    QVERIFY(!result.success);
    QVERIFY(result.cancelled);
    QCOMPARE(result.messagesWritten, stagedMessages);
    QCOMPARE(readFile(output), oldOutput);
}

void GpsCorrectionExtractorTest::finalProgressCallbackCanCancelBeforeCommit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("progress.tlog"));
    const QString output = directory.filePath(QStringLiteral("existing.dat"));
    const QByteArray oldOutput("keep old output");
    QVERIFY(writeFile(
        input, record(BaseTimestamp,
                      injectFrame(1, QByteArray("payload")))));
    QVERIFY(writeFile(output, oldOutput));
    bool cancel = false;
    int progressCalls = 0;

    const auto result = GpsCorrectionExtractor::Extract(
        input, output,
        [&cancel]() { return cancel; },
        [&cancel, &progressCalls](qint64 processed, qint64 total) {
            ++progressCalls;
            if (processed == total) {
                cancel = true;
            }
        });
    QVERIFY(!result.success);
    QVERIFY(result.cancelled);
    QCOMPARE(progressCalls, 1);
    QCOMPARE(result.messagesWritten, qint64(1));
    QCOMPARE(readFile(output), oldOutput);
}

void GpsCorrectionExtractorTest::
emptyAndNoCorrectionLogsCreateEmptyOutput_data()
{
    QTest::addColumn<QByteArray>("log");
    QTest::addColumn<qint64>("records");
    QTest::newRow("empty") << QByteArray() << qint64(0);
    QTest::newRow("no-corrections")
        << record(BaseTimestamp, heartbeatFrame(42)) << qint64(1);
}

void GpsCorrectionExtractorTest::emptyAndNoCorrectionLogsCreateEmptyOutput()
{
    QFETCH(QByteArray, log);
    QFETCH(qint64, records);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("empty.tlog"));
    const QString output = directory.filePath(QStringLiteral("existing.dat"));
    QVERIFY(writeFile(input, log));
    QVERIFY(writeFile(output, QByteArray("old")));

    const auto result = GpsCorrectionExtractor::Extract(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.recordsRead, records);
    QCOMPARE(result.messagesWritten, qint64(0));
    QCOMPARE(result.bytesWritten, qint64(0));
    QCOMPARE(readFile(output), QByteArray());
}

void GpsCorrectionExtractorTest::rejectsSameAndAliasedPaths()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("source.tlog"));
    const QByteArray original = record(
        BaseTimestamp, injectFrame(1, QByteArray("correction")));
    QVERIFY(writeFile(input, original));

    auto result = GpsCorrectionExtractor::Extract(input, input);
    QVERIFY(!result.success);
    QCOMPARE(readFile(input), original);

    const QString dotted = directory.path()
        + QStringLiteral("/./source.tlog");
    result = GpsCorrectionExtractor::Extract(input, dotted);
    QVERIFY(!result.success);
    QCOMPARE(readFile(input), original);

    const QString inputAlias =
        directory.filePath(QStringLiteral("source-alias.tlog"));
    if (!QFile::link(input, inputAlias)) {
        QSKIP("This platform cannot create a test symbolic link.");
    }
    const QString separate = directory.filePath(QStringLiteral("safe.dat"));
    result = GpsCorrectionExtractor::Extract(inputAlias, input);
    QVERIFY(!result.success);
    QCOMPARE(readFile(input), original);
    result = GpsCorrectionExtractor::Extract(input, inputAlias);
    QVERIFY(!result.success);
    QCOMPARE(readFile(input), original);

    // A symlink is rejected as an output even when its target differs.
    QVERIFY(writeFile(separate, QByteArray("separate")));
    const QString outputAlias =
        directory.filePath(QStringLiteral("output-alias.dat"));
    QVERIFY(QFile::link(separate, outputAlias));
    result = GpsCorrectionExtractor::Extract(input, outputAlias);
    QVERIFY(!result.success);
    QCOMPARE(readFile(separate), QByteArray("separate"));

    const QString realParent =
        directory.filePath(QStringLiteral("real-parent"));
    const QString aliasParent =
        directory.filePath(QStringLiteral("alias-parent"));
    QVERIFY(QDir().mkpath(realParent));
    if (QFile::link(realParent, aliasParent)) {
        const QString parentInput =
            QDir(realParent).filePath(QStringLiteral("same.tlog"));
        QVERIFY(writeFile(parentInput, original));
        result = GpsCorrectionExtractor::Extract(
            parentInput,
            QDir(aliasParent).filePath(QStringLiteral("same.tlog")));
        QVERIFY(!result.success);
        QCOMPARE(readFile(parentInput), original);
    }
}

void GpsCorrectionExtractorTest::outputFailuresAreTruthful()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("source.tlog"));
    QVERIFY(writeFile(
        input, record(BaseTimestamp,
                      injectFrame(1, QByteArray("correction")))));

    auto result = GpsCorrectionExtractor::Extract(
        input, directory.filePath(QStringLiteral("missing/output.dat")));
    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QVERIFY(!result.error.isEmpty());

    result = GpsCorrectionExtractor::Extract(input, directory.path());
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());

    result = GpsCorrectionExtractor::Extract(directory.path(),
                                             directory.filePath("out.dat"));
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
}

QTEST_GUILESS_MAIN(GpsCorrectionExtractorTest)

#include "test_gpscorrectionextractor.moc"
