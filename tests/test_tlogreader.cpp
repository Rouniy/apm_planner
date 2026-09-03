#include <QtTest>

#include "comm/TlogReader.h"

#include <QBuffer>
#include <QByteArray>
#include <QList>

namespace {

QByteArray frameBytes(mavlink_message_t &message)
{
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
    const int size = mavlink_msg_to_send_buffer(buffer, &message);
    return QByteArray(reinterpret_cast<const char *>(buffer), size);
}

QByteArray heartbeatFrame(quint8 systemId, bool mavlink1 = false)
{
    mavlink_status_t *channel = mavlink_get_channel_status(MAVLINK_COMM_0);
    if (mavlink1) {
        channel->flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    } else {
        channel->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    }
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(systemId, 1, &message, MAV_TYPE_QUADROTOR,
                               MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 3, MAV_STATE_ACTIVE);
    channel->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    return frameBytes(message);
}

QByteArray paramFrame(quint8 systemId, const char *name, float value)
{
    mavlink_message_t message{};
    mavlink_msg_param_value_pack(systemId, 1, &message, name, value, MAV_PARAM_TYPE_REAL32, 1, 0);
    return frameBytes(message);
}

QByteArray timestampBytes(qint64 usec)
{
    QByteArray bytes(8, '\0');
    quint64 value = static_cast<quint64>(usec);
    for (int i = 7; i >= 0; --i) {
        bytes[i] = static_cast<char>(value & 0xFF);
        value >>= 8;
    }
    return bytes;
}

QByteArray record(qint64 usec, const QByteArray &frame)
{
    return timestampBytes(usec) + frame;
}

struct OpenBuffer
{
    QByteArray data;
    QBuffer buffer;
    explicit OpenBuffer(const QByteArray &bytes)
        : data(bytes)
        , buffer(&data)
    {
        buffer.open(QIODevice::ReadOnly);
    }
};

class SequentialBuffer final : public QBuffer
{
public:
    bool isSequential() const override { return true; }
};

constexpr qint64 kBase = 1700000000000000LL; // 2023-11-14T22:13:20Z

} // namespace

class TlogReaderTest final : public QObject
{
    Q_OBJECT

private slots:
    void readsMavlink1And2RecordsWithTimestamps();
    void resyncsOverGarbageAndRejectsBadCrc();
    void truncatedTailIsReportedOnce();
    void cancelStopsBetweenRecords();
    void progressAndCountersFollowTheStream();
    void deviceProblemsAreErrors();
};

void TlogReaderTest::readsMavlink1And2RecordsWithTimestamps()
{
    const QByteArray first = heartbeatFrame(1);
    const QByteArray second = heartbeatFrame(2, true);
    const QByteArray third = paramFrame(3, "TEST", 1.5f);
    QVERIFY(static_cast<quint8>(first.at(0)) == MAVLINK_STX);
    QVERIFY(static_cast<quint8>(second.at(0)) == MAVLINK_STX_MAVLINK1);
    OpenBuffer log(record(kBase, first) + record(kBase + 1000, second) + record(kBase + 2000, third));

    TlogReader reader(&log.buffer);
    QCOMPARE(reader.status(), TlogReader::Status::Ok);
    QCOMPARE(reader.bytesTotal(), qint64(log.data.size()));
    TlogRecord rec;
    QCOMPARE(reader.next(&rec), TlogReader::Status::Ok);
    QCOMPARE(rec.timestampUsec, kBase);
    QCOMPARE(rec.offset, qint64(0));
    QCOMPARE(int(rec.message.msgid), int(MAVLINK_MSG_ID_HEARTBEAT));
    QCOMPARE(int(rec.message.sysid), 1);
    QCOMPARE(rec.timestampUtc(), QDateTime(QDate(2023, 11, 14), QTime(22, 13, 20), Qt::UTC));
    QCOMPARE(reader.next(&rec), TlogReader::Status::Ok);
    QCOMPARE(rec.timestampUsec, kBase + 1000);
    QCOMPARE(rec.offset, qint64(8 + first.size()));
    QCOMPARE(int(rec.message.sysid), 2);
    QCOMPARE(int(rec.message.magic), int(MAVLINK_STX_MAVLINK1));
    QCOMPARE(reader.next(&rec), TlogReader::Status::Ok);
    QCOMPARE(int(rec.message.msgid), int(MAVLINK_MSG_ID_PARAM_VALUE));
    mavlink_param_value_t value;
    mavlink_msg_param_value_decode(&rec.message, &value);
    QCOMPARE(value.param_value, 1.5f);
    QCOMPARE(reader.next(&rec), TlogReader::Status::End);
    QCOMPARE(reader.next(&rec), TlogReader::Status::End); // sticky
    QCOMPARE(reader.recordCount(), qint64(3));
    QCOMPARE(reader.skippedBytes(), qint64(0));
    QCOMPARE(reader.rejectedFrames(), qint64(0));
    QCOMPARE(reader.bytesProcessed(), reader.bytesTotal());
}

void TlogReaderTest::resyncsOverGarbageAndRejectsBadCrc()
{
    const QByteArray good = heartbeatFrame(1);
    QByteArray corrupted = heartbeatFrame(9);
    corrupted[corrupted.size() - 3] = static_cast<char>(corrupted.at(corrupted.size() - 3) ^ 0x5A); // payload byte -> bad CRC
    const QByteArray garbage = QByteArray::fromHex("0011FD22FE33");
    OpenBuffer log(record(kBase, good) + garbage + record(kBase + 5, corrupted) + record(kBase + 10, good));

    TlogReader reader(&log.buffer);
    TlogRecord rec;
    QCOMPARE(reader.next(&rec), TlogReader::Status::Ok);
    QCOMPARE(rec.timestampUsec, kBase);
    QCOMPARE(reader.next(&rec), TlogReader::Status::Ok);
    QCOMPARE(rec.timestampUsec, kBase + 10); // the corrupted record never surfaces
    QCOMPARE(int(rec.message.sysid), 1);
    QCOMPARE(reader.next(&rec), TlogReader::Status::End);
    QCOMPARE(reader.recordCount(), qint64(2));
    QCOMPARE(reader.skippedBytes(), qint64(garbage.size() + 8 + corrupted.size()));
    QVERIFY(reader.rejectedFrames() >= 1);
}

void TlogReaderTest::truncatedTailIsReportedOnce()
{
    const QByteArray good = heartbeatFrame(1);
    OpenBuffer log(record(kBase, good) + timestampBytes(kBase + 1) + good.left(5));
    TlogReader reader(&log.buffer);
    TlogRecord rec;
    QCOMPARE(reader.next(&rec), TlogReader::Status::Ok);
    QCOMPARE(reader.next(&rec), TlogReader::Status::Truncated);
    QCOMPARE(reader.next(&rec), TlogReader::Status::Truncated);
    QCOMPARE(reader.skippedBytes(), qint64(13));
    QVERIFY(reader.errorString().contains(QStringLiteral("13 trailing byte")));
    QCOMPARE(reader.bytesProcessed(), reader.bytesTotal());

    // A long run of bytes that never forms a frame is consumed one byte at a time.
    OpenBuffer junk(record(kBase, good) + QByteArray(40, '\x7f'));
    TlogReader junkReader(&junk.buffer);
    QCOMPARE(junkReader.next(&rec), TlogReader::Status::Ok);
    QCOMPARE(junkReader.next(&rec), TlogReader::Status::Truncated);
    QCOMPARE(junkReader.skippedBytes(), qint64(40));
    QCOMPARE(junkReader.recordCount(), qint64(1));
}

void TlogReaderTest::cancelStopsBetweenRecords()
{
    const QByteArray good = heartbeatFrame(1);
    OpenBuffer log(record(kBase, good) + record(kBase + 1, good) + record(kBase + 2, good));
    TlogReader reader(&log.buffer);
    int checks = 0;
    reader.setCancelCheck([&checks]() { return ++checks > 1; });
    TlogRecord rec;
    QCOMPARE(reader.next(&rec), TlogReader::Status::Ok);
    QCOMPARE(reader.next(&rec), TlogReader::Status::Cancelled);
    QCOMPARE(reader.next(&rec), TlogReader::Status::Cancelled); // sticky, no further checks
    QCOMPARE(checks, 2);
    QCOMPARE(reader.recordCount(), qint64(1));
}

void TlogReaderTest::progressAndCountersFollowTheStream()
{
    const QByteArray good = heartbeatFrame(1);
    OpenBuffer log(record(kBase, good) + record(kBase + 1, good));
    TlogReader reader(&log.buffer);
    QList<qint64> processed;
    qint64 total = -1;
    reader.setProgress([&processed, &total](qint64 done, qint64 all) {
        processed.append(done);
        total = all;
    });
    TlogRecord rec;
    while (reader.next(&rec) == TlogReader::Status::Ok) {
    }
    QCOMPARE(processed.size(), 2);
    QCOMPARE(processed.at(0), qint64(8 + good.size()));
    QCOMPARE(processed.at(1), qint64(log.data.size()));
    QCOMPARE(total, qint64(log.data.size()));

    OpenBuffer empty{QByteArray()};
    TlogReader emptyReader(&empty.buffer);
    QCOMPARE(emptyReader.next(&rec), TlogReader::Status::End);
    QCOMPARE(emptyReader.recordCount(), qint64(0));
}

void TlogReaderTest::deviceProblemsAreErrors()
{
    TlogRecord rec;
    TlogReader nullReader(nullptr);
    QCOMPARE(nullReader.status(), TlogReader::Status::Error);
    QCOMPARE(nullReader.next(&rec), TlogReader::Status::Error);
    QVERIFY(!nullReader.errorString().isEmpty());

    QByteArray bytes = record(kBase, heartbeatFrame(1));
    QBuffer closed(&bytes);
    TlogReader closedReader(&closed);
    QCOMPARE(closedReader.next(&rec), TlogReader::Status::Error);

    SequentialBuffer sequential;
    sequential.setData(bytes);
    sequential.open(QIODevice::ReadOnly);
    TlogReader sequentialReader(&sequential);
    QCOMPARE(sequentialReader.next(&rec), TlogReader::Status::Error);
    QVERIFY(sequentialReader.errorString().contains(QStringLiteral("seekable")));

    OpenBuffer ok(bytes);
    TlogReader okReader(&ok.buffer);
    QCOMPARE(okReader.next(nullptr), TlogReader::Status::Error);
}

QTEST_GUILESS_MAIN(TlogReaderTest)
#include "test_tlogreader.moc"
