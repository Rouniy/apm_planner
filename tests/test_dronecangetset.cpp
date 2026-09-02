#include <QtTest>

#include "comm/DroneCanGetSetClient.h"

#include <QSignalSpy>

#include <cstring>

namespace {
constexpr quint32 RequestIdNode42 = 0x9E0BAAFFU;
constexpr quint32 ResponseIdNode42 = 0x9E0B7FAAU;

QByteArray hex(const char *text)
{
    return QByteArray::fromHex(text);
}

class TestBitWriter final
{
public:
    void scalar(quint64 value, int bitCount)
    {
        int shift = 0;
        while (bitCount >= 8) {
            bits(quint8((value >> shift) & 0xFFU), 8);
            shift += 8;
            bitCount -= 8;
        }
        if (bitCount > 0) {
            bits(quint8((value >> shift)
                        & ((quint64(1) << bitCount) - 1U)), bitCount);
        }
    }

    void zeros(int count)
    {
        while (count-- > 0) {
            bits(0, 1);
        }
    }

    void bytes(const QByteArray &value)
    {
        for (char byte : value) {
            bits(quint8(byte), 8);
        }
    }

    QByteArray result() const
    {
        return m_data;
    }

private:
    void bits(quint8 value, int count)
    {
        for (int bit = count - 1; bit >= 0; --bit) {
            if ((m_offset % 8) == 0) {
                m_data.append(char(0));
            }
            if (((value >> bit) & 1U) != 0) {
                const int byteIndex = m_offset / 8;
                m_data[byteIndex] = char(
                    quint8(m_data.at(byteIndex))
                    | quint8(1U << (7 - (m_offset % 8))));
            }
            ++m_offset;
        }
    }

    QByteArray m_data;
    int m_offset = 0;
};

void writeValue(TestBitWriter *writer,
                const DroneCanGetSetClient::Value &value)
{
    writer->scalar(quint8(value.type), 3);
    switch (value.type) {
    case DroneCanGetSetClient::Value::Empty:
        return;
    case DroneCanGetSetClient::Value::Integer: {
        quint64 raw = 0;
        std::memcpy(&raw, &value.integerValue, sizeof(raw));
        writer->scalar(raw, 64);
        return;
    }
    case DroneCanGetSetClient::Value::Real: {
        quint32 raw = 0;
        std::memcpy(&raw, &value.realValue, sizeof(raw));
        writer->scalar(raw, 32);
        return;
    }
    case DroneCanGetSetClient::Value::Boolean:
        writer->scalar(value.booleanValue ? 1U : 0U, 8);
        return;
    case DroneCanGetSetClient::Value::String:
        writer->scalar(quint8(value.stringValue.size()), 8);
        writer->bytes(value.stringValue);
        return;
    }
}

void writeNumeric(TestBitWriter *writer,
                  const DroneCanGetSetClient::NumericValue &value)
{
    writer->scalar(quint8(value.type), 2);
    if (value.type == DroneCanGetSetClient::NumericValue::Integer) {
        quint64 raw = 0;
        std::memcpy(&raw, &value.integerValue, sizeof(raw));
        writer->scalar(raw, 64);
    } else if (value.type == DroneCanGetSetClient::NumericValue::Real) {
        quint32 raw = 0;
        std::memcpy(&raw, &value.realValue, sizeof(raw));
        writer->scalar(raw, 32);
    }
}

QByteArray requestPayload(quint16 index,
                          const DroneCanGetSetClient::Value &value,
                          const QByteArray &name, bool canFd)
{
    TestBitWriter writer;
    writer.scalar(index, 13);
    writeValue(&writer, value);
    if (canFd) {
        writer.scalar(quint8(name.size()), 7);
    }
    writer.bytes(name);
    return writer.result();
}

QByteArray responsePayload(
    const DroneCanGetSetClient::Value &value,
    const DroneCanGetSetClient::Value &defaultValue,
    const DroneCanGetSetClient::NumericValue &maximumValue,
    const DroneCanGetSetClient::NumericValue &minimumValue,
    const QByteArray &name, bool canFd)
{
    TestBitWriter writer;
    writer.zeros(5);
    writeValue(&writer, value);
    writer.zeros(5);
    writeValue(&writer, defaultValue);
    writer.zeros(6);
    writeNumeric(&writer, maximumValue);
    writer.zeros(6);
    writeNumeric(&writer, minimumValue);
    if (canFd) {
        writer.scalar(quint8(name.size()), 7);
    }
    writer.bytes(name);
    return writer.result();
}

quint16 addCrcByte(quint16 crc, quint8 byte)
{
    crc ^= quint16(byte) << 8U;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000U) != 0
            ? quint16((crc << 1U) ^ 0x1021U)
            : quint16(crc << 1U);
    }
    return crc;
}

quint16 payloadCrc(const QByteArray &payload)
{
    quint16 crc = DroneCanGetSetClient::GetSetBaseCrc;
    for (char byte : payload) {
        crc = addCrcByte(crc, quint8(byte));
    }
    return crc;
}

int nextFdContentLength(int minimum)
{
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

QList<QByteArray> responseFrames(QByteArray payload, bool canFd,
                                 quint8 transferId)
{
    const int capacity = canFd ? 63 : 7;
    if (canFd) {
        if (payload.size() <= capacity) {
            const int padded = nextFdContentLength(payload.size());
            payload.append(QByteArray(padded - payload.size(), char(0)));
        } else {
            const int remainder = (payload.size() + 2) % capacity;
            if (remainder != 0) {
                const int padded = nextFdContentLength(remainder);
                payload.append(QByteArray(padded - remainder, char(0)));
            }
        }
    }

    if (payload.size() <= capacity) {
        payload.append(char(0xC0U | transferId));
        return {payload};
    }

    const quint16 crc = payloadCrc(payload);
    QByteArray transfer;
    transfer.append(char(crc & 0xFFU));
    transfer.append(char(crc >> 8U));
    transfer.append(payload);
    QList<QByteArray> result;
    bool toggle = false;
    for (int offset = 0; offset < transfer.size(); offset += capacity) {
        const int amount = qMin(capacity, transfer.size() - offset);
        quint8 tail = transferId;
        if (offset == 0) {
            tail |= 0x80U;
        }
        if (offset + amount == transfer.size()) {
            tail |= 0x40U;
        }
        if (toggle) {
            tail |= 0x20U;
        }
        QByteArray frame = transfer.mid(offset, amount);
        frame.append(char(tail));
        result.append(frame);
        toggle = !toggle;
    }
    return result;
}

QByteArray outgoingPayload(const QSignalSpy &transmissions,
                           bool *validCrc = nullptr)
{
    if (validCrc) {
        *validCrc = true;
    }
    if (transmissions.isEmpty()) {
        return {};
    }
    const QByteArray first = transmissions.first().at(1).toByteArray();
    const quint8 firstTail = quint8(first.at(first.size() - 1));
    if ((firstTail & 0xC0U) == 0xC0U) {
        return first.left(first.size() - 1);
    }

    QByteArray transfer;
    for (int index = 0; index < transmissions.size(); ++index) {
        const QByteArray frame = transmissions.at(index).at(1).toByteArray();
        transfer.append(frame.constData(), frame.size() - 1);
    }
    if (transfer.size() < 2) {
        if (validCrc) {
            *validCrc = false;
        }
        return {};
    }
    const quint16 expected = quint16(quint8(transfer.at(0)))
        | (quint16(quint8(transfer.at(1))) << 8U);
    const QByteArray payload = transfer.mid(2);
    if (validCrc) {
        *validCrc = expected == payloadCrc(payload);
    }
    return payload;
}

bool deliver(DroneCanGetSetClient *client,
             const QList<QByteArray> &frames, bool canFd,
             qint64 startMs = 10)
{
    bool result = false;
    qint64 nowMs = startMs;
    for (int index = 0; index < frames.size(); ++index) {
        const QByteArray &frame = frames.at(index);
        result = client->acceptFrame(91, 1, ResponseIdNode42,
                                     frame, canFd, nowMs++);
        if (!result && index != frames.size() - 1) {
            return false;
        }
    }
    return result;
}
}

class DroneCanGetSetClientTest final : public QObject
{
    Q_OBJECT

private slots:
    void canonicalIdsAndEmptyRequestVectors();
    void requestEncoderHandlesEveryValueVariant();
    void responseDecoderHandlesEveryValueVariant();
    void defaultAndNumericMetadataAreDecoded();
    void strictCorrelationRejectsUnrelatedAndMalformedResponses();
    void classicAndFdMultiFrameTransfersEnforceTransportRules();
    void requestsAreSerializedAndCanBeCancelled();
    void deadlinesRetryAndEventuallyFail();
    void transferIdsAreIndependentPerBusAndDestination();
    void synchronousCancellationStopsMultiFrameTransmission();
};

void DroneCanGetSetClientTest::canonicalIdsAndEmptyRequestVectors()
{
    DroneCanGetSetClient client;
    QSignalSpy transmissions(
        &client, &DroneCanGetSetClient::transmitRequested);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.getByIndex(42, 0, false, 10));
    QCOMPARE(transmissions.count(), 1);
    QCOMPARE(transmissions.first().at(0).toUInt(), RequestIdNode42);
    QCOMPARE(transmissions.first().at(1).toByteArray(), hex("0000c0"));
    QCOMPARE(transmissions.first().at(2).toBool(), false);

    QVERIFY(client.cancelPendingRequest());
    transmissions.clear();
    QVERIFY(client.getByIndex(42, 0, true, 20));
    QCOMPARE(transmissions.count(), 1);
    QCOMPARE(transmissions.first().at(1).toByteArray(), hex("000000c1"));
    QCOMPARE(transmissions.first().at(2).toBool(), true);
}

void DroneCanGetSetClientTest::requestEncoderHandlesEveryValueVariant()
{
    const QList<DroneCanGetSetClient::Value> values = {
        DroneCanGetSetClient::Value{},
        DroneCanGetSetClient::Value::fromInteger(-1234567890123LL),
        DroneCanGetSetClient::Value::fromReal(12.5F),
        DroneCanGetSetClient::Value::fromBoolean(true),
        DroneCanGetSetClient::Value::fromString(
            QByteArray("binary\0value", 12)),
    };

    for (const auto &value : values) {
        DroneCanGetSetClient client;
        QSignalSpy transmissions(
            &client, &DroneCanGetSetClient::transmitRequested);
        QVERIFY(client.bindSession({91, 1, 127}));
        QVERIFY(client.requestParameter(42, 8191, QByteArray("PARAM"),
                                        value, false, 0));
        bool validCrc = false;
        const QByteArray actual = outgoingPayload(transmissions, &validCrc);
        QVERIFY(validCrc);
        QCOMPARE(actual, requestPayload(8191, value,
                                        QByteArray("PARAM"), false));
    }
}

void DroneCanGetSetClientTest::responseDecoderHandlesEveryValueVariant()
{
    const QList<DroneCanGetSetClient::Value> values = {
        DroneCanGetSetClient::Value{},
        DroneCanGetSetClient::Value::fromInteger(-9223372036854775000LL),
        DroneCanGetSetClient::Value::fromReal(-0.125F),
        DroneCanGetSetClient::Value::fromBoolean(true),
        DroneCanGetSetClient::Value::fromString(
            QByteArray(128, char(0xA5))),
    };

    for (const auto &value : values) {
        DroneCanGetSetClient client;
        QSignalSpy received(&client,
                            &DroneCanGetSetClient::parameterReceived);
        QVERIFY(client.bindSession({91, 1, 127}));
        QVERIFY(client.getByName(42, QByteArray("PARAM"), false, 0));
        const QByteArray payload = responsePayload(
            value, {}, {}, {}, QByteArray("PARAM"), false);
        QVERIFY(deliver(&client, responseFrames(payload, false, 0), false));
        QCOMPARE(received.count(), 1);
        const auto parameter =
            qvariant_cast<DroneCanGetSetClient::Parameter>(
                received.first().at(0));
        QVERIFY(parameter.value == value);
        QCOMPARE(parameter.name, QByteArray("PARAM"));
        QCOMPARE(parameter.requestedName, QByteArray("PARAM"));
        QCOMPARE(parameter.nodeId, 42);
        QCOMPARE(parameter.transferId, quint8(0));
        QVERIFY(!parameter.canFd);
    }
}

void DroneCanGetSetClientTest::defaultAndNumericMetadataAreDecoded()
{
    DroneCanGetSetClient::NumericValue maximum;
    maximum.type = DroneCanGetSetClient::NumericValue::Integer;
    maximum.integerValue = 1000;
    DroneCanGetSetClient::NumericValue minimum;
    minimum.type = DroneCanGetSetClient::NumericValue::Real;
    minimum.realValue = -2.5F;

    DroneCanGetSetClient client;
    QSignalSpy received(&client,
                        &DroneCanGetSetClient::parameterReceived);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.getByIndex(42, 17, true, 0));
    const QByteArray payload = responsePayload(
        DroneCanGetSetClient::Value::fromReal(1.5F),
        DroneCanGetSetClient::Value::fromReal(0.25F), maximum, minimum,
        QByteArray("GAIN"), true);
    const QList<QByteArray> frames = responseFrames(payload, true, 0);
    QVERIFY(deliver(&client, frames, true));
    QCOMPARE(received.count(), 1);
    const auto parameter = qvariant_cast<DroneCanGetSetClient::Parameter>(
        received.first().at(0));
    QCOMPARE(parameter.requestedIndex, quint16(17));
    QVERIFY(parameter.defaultValue
            == DroneCanGetSetClient::Value::fromReal(0.25F));
    QVERIFY(parameter.maximumValue == maximum);
    QVERIFY(parameter.minimumValue == minimum);
    QCOMPARE(parameter.name, QByteArray("GAIN"));
    QVERIFY(parameter.canFd);
}

void DroneCanGetSetClientTest::strictCorrelationRejectsUnrelatedAndMalformedResponses()
{
    DroneCanGetSetClient client;
    QSignalSpy received(&client,
                        &DroneCanGetSetClient::parameterReceived);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.getByName(42, QByteArray("PARAM"), false, 0));
    const QByteArray mismatchPayload = responsePayload(
        DroneCanGetSetClient::Value::fromBoolean(true), {}, {}, {},
        QByteArray("OTHER"), false);
    const QList<QByteArray> mismatchFrames =
        responseFrames(mismatchPayload, false, 0);
    QVERIFY(mismatchFrames.size() > 1);
    const QByteArray mismatch = mismatchFrames.first();
    QVERIFY(!client.acceptFrame(90, 1, ResponseIdNode42,
                                mismatch, false, 10));
    QVERIFY(!client.acceptFrame(91, 0, ResponseIdNode42,
                                mismatch, false, 10));
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42 + 1U,
                                mismatch, false, 10));
    QByteArray wrongTid = mismatch;
    wrongTid[wrongTid.size() - 1] = char(0xC1);
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                wrongTid, false, 10));
    // The parameter name is the tail array, so a classic-CAN receiver can
    // only detect a mismatch after assembling the complete transfer.
    for (int index = 0; index < mismatchFrames.size(); ++index) {
        const bool accepted = client.acceptFrame(
            91, 1, ResponseIdNode42, mismatchFrames.at(index), false, 10);
        if (index + 1 == mismatchFrames.size()) {
            QVERIFY(!accepted);
        } else {
            QVERIFY(accepted);
        }
    }
    QVERIFY(client.hasPendingRequest());
    QCOMPARE(received.count(), 0);

    QByteArray malformed = responsePayload(
        DroneCanGetSetClient::Value::fromBoolean(true), {}, {}, {},
        QByteArray("PARAM"), false);
    malformed[0] = char(7); // Value union tag 7 is not defined.
    const QList<QByteArray> malformedFrames =
        responseFrames(malformed, false, 0);
    QVERIFY(!deliver(&client, malformedFrames, false, 500));
    QVERIFY(client.hasPendingRequest());

    const QByteArray validPayload = responsePayload(
        DroneCanGetSetClient::Value::fromBoolean(true), {}, {}, {},
        QByteArray("PARAM"), false);
    QVERIFY(deliver(&client, responseFrames(validPayload, false, 0), false,
                    501));
    QCOMPARE(received.count(), 1);
}

void DroneCanGetSetClientTest::classicAndFdMultiFrameTransfersEnforceTransportRules()
{
    const auto longValue = DroneCanGetSetClient::Value::fromString(
        QByteArray(128, char('x')));
    const QByteArray classicPayload = responsePayload(
        longValue, {}, {}, {}, QByteArray("LONG"), false);
    QList<QByteArray> badCrcFrames = responseFrames(
        classicPayload, false, 0);
    QVERIFY(badCrcFrames.size() > 1);
    badCrcFrames[0][0] = char(quint8(badCrcFrames[0].at(0)) ^ 1U);

    DroneCanGetSetClient classicClient;
    QVERIFY(classicClient.bindSession({91, 1, 127}));
    QVERIFY(classicClient.getByName(42, QByteArray("LONG"), false, 0));
    for (int index = 0; index < badCrcFrames.size() - 1; ++index) {
        QVERIFY(classicClient.acceptFrame(
            91, 1, ResponseIdNode42, badCrcFrames.at(index), false,
            10 + index));
    }
    QVERIFY(!classicClient.acceptFrame(
        91, 1, ResponseIdNode42, badCrcFrames.last(), false, 100));
    QVERIFY(classicClient.hasPendingRequest());

    DroneCanGetSetClient fdClient;
    QSignalSpy received(&fdClient,
                        &DroneCanGetSetClient::parameterReceived);
    QVERIFY(fdClient.bindSession({91, 1, 127}));
    QVERIFY(fdClient.getByName(42, QByteArray("LONG"), true, 0));
    QList<QByteArray> fdFrames = responseFrames(
        responsePayload(longValue, {}, {}, {}, QByteArray("LONG"), true),
        true, 0);
    QVERIFY(fdFrames.size() > 1);
    QVERIFY(deliver(&fdClient, fdFrames, true));
    QCOMPARE(received.count(), 1);

    DroneCanGetSetClient toggleClient;
    QVERIFY(toggleClient.bindSession({91, 1, 127}));
    QVERIFY(toggleClient.getByName(42, QByteArray("LONG"), true, 0));
    fdFrames[1][fdFrames[1].size() - 1] = char(0x00);
    QVERIFY(toggleClient.acceptFrame(91, 1, ResponseIdNode42,
                                     fdFrames[0], true, 10));
    QVERIFY(!toggleClient.acceptFrame(91, 1, ResponseIdNode42,
                                      fdFrames[1], true, 11));
}

void DroneCanGetSetClientTest::requestsAreSerializedAndCanBeCancelled()
{
    DroneCanGetSetClient client;
    QSignalSpy cancelled(&client,
                         &DroneCanGetSetClient::requestCancelled);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.getByIndex(42, 0, false, 0));
    QVERIFY(!client.getByName(43, QByteArray("P"), false, 1));
    QCOMPARE(client.pendingRemoteNodeId(), 42);
    QVERIFY(client.cancelPendingRequest(QStringLiteral("operator request")));
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(cancelled.first().at(0).toInt(), 42);
    QCOMPARE(cancelled.first().at(1).toString(),
             QStringLiteral("operator request"));
    QVERIFY(!client.hasPendingRequest());
    QVERIFY(!client.cancelPendingRequest());
    QVERIFY(client.getByName(43, QByteArray("P"), false, 2));
}

void DroneCanGetSetClientTest::deadlinesRetryAndEventuallyFail()
{
    DroneCanGetSetClient client;
    QSignalSpy transmissions(
        &client, &DroneCanGetSetClient::transmitRequested);
    QSignalSpy failures(&client, &DroneCanGetSetClient::requestFailed);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.getByIndex(42, 0, false, 0));
    for (int retry = 1; retry < DroneCanGetSetClient::MaximumAttempts;
         ++retry) {
        client.tick(qint64(retry) * 1000);
        QCOMPARE(transmissions.count(), retry + 1);
        QCOMPARE(quint8(quint8(
                     transmissions.last().at(1).toByteArray().back())
                     & 0x1FU),
                 quint8(retry));
    }
    client.tick(qint64(DroneCanGetSetClient::MaximumAttempts) * 1000);
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.first().at(0).toInt(), 42);
    QVERIFY(!client.hasPendingRequest());
}

void DroneCanGetSetClientTest::transferIdsAreIndependentPerBusAndDestination()
{
    DroneCanGetSetClient client;
    QSignalSpy transmissions(
        &client, &DroneCanGetSetClient::transmitRequested);
    QVERIFY(client.bindSession({91, 0, 127}));
    QVERIFY(client.getByIndex(42, 0, false, 0));
    QCOMPARE(quint8(quint8(
                 transmissions.last().at(1).toByteArray().back())
                 & 0x1FU),
             quint8(0));
    client.clearPendingRequest();
    QVERIFY(client.getByIndex(43, 0, false, 1));
    QCOMPARE(quint8(quint8(
                 transmissions.last().at(1).toByteArray().back())
                 & 0x1FU),
             quint8(0));

    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.getByIndex(42, 0, false, 2));
    QCOMPARE(quint8(quint8(
                 transmissions.last().at(1).toByteArray().back())
                 & 0x1FU),
             quint8(0));

    QVERIFY(client.bindSession({91, 0, 127}));
    QVERIFY(client.getByIndex(42, 0, false, 3));
    QCOMPARE(quint8(quint8(
                 transmissions.last().at(1).toByteArray().back())
                 & 0x1FU),
             quint8(1));
}

void DroneCanGetSetClientTest::synchronousCancellationStopsMultiFrameTransmission()
{
    DroneCanGetSetClient client;
    int transmissions = 0;
    QVERIFY(client.bindSession({91, 1, 127}));
    connect(&client, &DroneCanGetSetClient::transmitRequested,
            &client, [&client, &transmissions]() {
                ++transmissions;
                client.cancelPendingRequest();
            });
    const auto longValue = DroneCanGetSetClient::Value::fromString(
        QByteArray(128, char('x')));
    QVERIFY(!client.setByName(42, QByteArray("LONG"), longValue,
                              false, 0));
    QCOMPARE(transmissions, 1);
    QVERIFY(!client.hasPendingRequest());
}

QTEST_GUILESS_MAIN(DroneCanGetSetClientTest)
#include "test_dronecangetset.moc"
