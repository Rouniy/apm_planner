#include <QtTest>

#include "comm/DroneCanGetNodeInfoClient.h"

#include <QSignalSpy>

namespace {
constexpr quint32 RequestIdNode42 = 0x9E01AAFFU;
constexpr quint32 RequestIdNode43 = 0x9E01ABFFU;
constexpr quint32 ResponseIdNode42 = 0x9E017FAAU;

QByteArray hex(const char *value)
{
    return QByteArray::fromHex(value);
}

void advanceNode42ToTransferId5(DroneCanGetNodeInfoClient *client,
                                bool canFd)
{
    QVERIFY(client->requestNodeInfo(42, canFd, 0));
    for (qint64 time = 1000; time <= 5000; time += 1000) {
        client->tick(time);
    }
}

QList<QByteArray> classicResponse()
{
    return {
        hex("5b32040302015385"),
        hex("efbe010203443325"),
        hex("2211080706050405"),
        hex("0302010304000125"),
        hex("0203040506070805"),
        hex("090a0b0c0d0e0f25"),
        hex("02cafe6f72672e05"),
        hex("7465737465"),
    };
}

QList<QByteArray> fdMultiResponse()
{
    return {
        hex("bec70403020153efbe0102034433221108070605040302010304000102030405060708090a0b0c0d0e0f14a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b385"),
        hex("20dee4ce5ccaf0c2dae0d8ca5cdcdec8ca000065"),
    };
}

QList<QByteArray> withTransferId(QList<QByteArray> frames, quint8 transferId)
{
    for (QByteArray &frame : frames) {
        const int tailIndex = frame.size() - 1;
        frame[tailIndex] = char((quint8(frame.at(tailIndex)) & 0xE0U)
                                | (transferId & 0x1FU));
    }
    return frames;
}

void compareCommonInfo(const DroneCanGetNodeInfoClient::NodeInfo &info,
                       const QString &name)
{
    QCOMPARE(info.brokerGeneration, quint64(91));
    QCOMPARE(info.busIndex, 1);
    QCOMPARE(info.nodeId, 42);
    QCOMPARE(info.transferId, quint8(5));
    QCOMPARE(info.status.uptimeSeconds, quint32(0x01020304U));
    QCOMPARE(info.status.health, quint8(1));
    QCOMPARE(info.status.mode, quint8(2));
    QCOMPARE(info.status.subMode, quint8(3));
    QCOMPARE(info.status.vendorSpecificStatusCode, quint16(0xBEEFU));
    QCOMPARE(info.softwareVersion.major, quint8(1));
    QCOMPARE(info.softwareVersion.minor, quint8(2));
    QCOMPARE(info.softwareVersion.optionalFieldFlags, quint8(3));
    QVERIFY(info.softwareVersion.hasVcsCommit());
    QVERIFY(info.softwareVersion.hasImageCrc());
    QCOMPARE(info.softwareVersion.vcsCommit, quint32(0x11223344U));
    QCOMPARE(info.softwareVersion.imageCrc,
             Q_UINT64_C(0x0102030405060708));
    QCOMPARE(info.hardwareVersion.major, quint8(3));
    QCOMPARE(info.hardwareVersion.minor, quint8(4));
    QCOMPARE(info.hardwareVersion.uniqueId,
             hex("000102030405060708090a0b0c0d0e0f"));
    QCOMPARE(info.name, name);
}
}

class DroneCanGetNodeInfoClientTest final : public QObject
{
    Q_OBJECT

private slots:
    void requestUsesCanonicalServiceIdAndTransferTail();
    void classicMultiFrameVectorIsReassembledAndDecoded();
    void fdSingleFrameVectorHonorsLengthAndZeroPadding();
    void fdMultiFrameVectorHonorsCrcAndCertificate();
    void staleSessionBusNodeAndTransferIdAreIgnored();
    void badToggleCrcAndPaddingAreRejected();
    void deadlinesRetryAndEventuallyFail();
    void mixedTransportAndAssemblyTimeoutRestartSafely();
    void assemblyLimitDropsOnlyTheTransfer();
    void refreshEpochPreservesTransferIdSequence();
    void synchronousResetDoesNotReportPendingRequest();
    void resetAndRebindInvalidateOldTraffic();
};

void DroneCanGetNodeInfoClientTest::requestUsesCanonicalServiceIdAndTransferTail()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy transmissions(
        &client, &DroneCanGetNodeInfoClient::transmitRequested);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.requestNodeInfo(42, false, 10));
    QCOMPARE(transmissions.count(), 1);
    QCOMPARE(transmissions.first().at(0).toUInt(), RequestIdNode42);
    QCOMPARE(transmissions.first().at(1).toByteArray(), hex("c0"));
    QCOMPARE(transmissions.first().at(2).toBool(), false);

    client.tick(1009);
    QCOMPARE(transmissions.count(), 1);
    client.tick(1010);
    QCOMPARE(transmissions.count(), 2);
    QCOMPARE(transmissions.last().at(1).toByteArray(), hex("c1"));
    for (qint64 time = 2010; time <= 5010; time += 1000) {
        client.tick(time);
    }
    QCOMPARE(transmissions.count(), 6);
    QCOMPARE(transmissions.last().at(0).toUInt(), RequestIdNode42);
    QCOMPARE(transmissions.last().at(1).toByteArray(), hex("c5"));
}

void DroneCanGetNodeInfoClientTest::classicMultiFrameVectorIsReassembledAndDecoded()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy received(&client,
                        &DroneCanGetNodeInfoClient::nodeInfoReceived);
    QSignalSpy failures(&client,
                        &DroneCanGetNodeInfoClient::requestFailed);
    QVERIFY(client.bindSession({91, 1, 127}));
    advanceNode42ToTransferId5(&client, false);

    qint64 now = 5001;
    for (const QByteArray &frame : classicResponse()) {
        QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                                   frame, false, now++));
    }
    QCOMPARE(failures.count(), 0);
    QCOMPARE(received.count(), 1);
    const auto info = qvariant_cast<DroneCanGetNodeInfoClient::NodeInfo>(
        received.first().at(0));
    QVERIFY(!info.canFd);
    compareCommonInfo(info, QStringLiteral("org.test"));
    QCOMPARE(info.hardwareVersion.certificateOfAuthenticity,
             hex("cafe"));
    QCOMPARE(client.pendingRequestCount(), 0);
}

void DroneCanGetNodeInfoClientTest::fdSingleFrameVectorHonorsLengthAndZeroPadding()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy received(&client,
                        &DroneCanGetNodeInfoClient::nodeInfoReceived);
    QVERIFY(client.bindSession({91, 1, 127}));
    advanceNode42ToTransferId5(&client, true);
    const QByteArray frame = hex(
        "0403020153efbe0102034433221108070605040302010304000102030405060708090a0b0c0d0e0f02cafe10dee4ce5ce8cae6e80000000000000000000000c5");
    QCOMPARE(frame.size(), 64);
    QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                               frame, true, 5001));
    QCOMPARE(received.count(), 1);
    const auto info = qvariant_cast<DroneCanGetNodeInfoClient::NodeInfo>(
        received.first().at(0));
    QVERIFY(info.canFd);
    compareCommonInfo(info, QStringLiteral("org.test"));
    QCOMPARE(info.hardwareVersion.certificateOfAuthenticity,
             hex("cafe"));
}

void DroneCanGetNodeInfoClientTest::fdMultiFrameVectorHonorsCrcAndCertificate()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy received(&client,
                        &DroneCanGetNodeInfoClient::nodeInfoReceived);
    QVERIFY(client.bindSession({91, 1, 127}));
    advanceNode42ToTransferId5(&client, true);

    qint64 now = 5001;
    for (const QByteArray &frame : fdMultiResponse()) {
        QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                                   frame, true, now++));
    }
    QCOMPARE(received.count(), 1);
    const auto info = qvariant_cast<DroneCanGetNodeInfoClient::NodeInfo>(
        received.first().at(0));
    QVERIFY(info.canFd);
    compareCommonInfo(info, QStringLiteral("org.example.node"));
    QCOMPARE(info.hardwareVersion.certificateOfAuthenticity,
             hex("a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3"));
}

void DroneCanGetNodeInfoClientTest::staleSessionBusNodeAndTransferIdAreIgnored()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy received(&client,
                        &DroneCanGetNodeInfoClient::nodeInfoReceived);
    QSignalSpy failures(&client,
                        &DroneCanGetNodeInfoClient::requestFailed);
    QVERIFY(client.bindSession({91, 1, 127}));
    advanceNode42ToTransferId5(&client, false);
    const QByteArray first = classicResponse().first();

    QVERIFY(!client.acceptFrame(90, 1, ResponseIdNode42,
                                first, false, 5001));
    QVERIFY(!client.acceptFrame(91, 0, ResponseIdNode42,
                                first, false, 5001));
    QVERIFY(!client.acceptFrame(91, 1, 0x9E017FABU,
                                first, false, 5001));
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42 | 0x8000U,
                                first, false, 5001));
    QVERIFY(!client.acceptFrame(91, 1, 0x9E017EAAU,
                                first, false, 5001));
    QByteArray wrongTid = first;
    wrongTid[wrongTid.size() - 1] = char(0x84);
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                wrongTid, false, 5001));
    QCOMPARE(received.count(), 0);
    QCOMPARE(failures.count(), 0);
    QCOMPARE(client.pendingRequestCount(), 1);

    qint64 now = 5002;
    for (const QByteArray &frame : classicResponse()) {
        QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                                   frame, false, now++));
    }
    QCOMPARE(received.count(), 1);
}

void DroneCanGetNodeInfoClientTest::badToggleCrcAndPaddingAreRejected()
{
    {
        DroneCanGetNodeInfoClient client;
        QSignalSpy failures(&client,
                            &DroneCanGetNodeInfoClient::requestFailed);
        QVERIFY(client.bindSession({91, 1, 127}));
        advanceNode42ToTransferId5(&client, false);
        QList<QByteArray> frames = classicResponse();
        frames[1][7] = char(0x05);
        QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                                   frames[0], false, 5001));
        QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                    frames[1], false, 5002));
        QCOMPARE(failures.count(), 0);
        QCOMPARE(client.pendingRequestCount(), 1);
    }
    {
        DroneCanGetNodeInfoClient client;
        QSignalSpy failures(&client,
                            &DroneCanGetNodeInfoClient::requestFailed);
        QVERIFY(client.bindSession({91, 1, 127}));
        advanceNode42ToTransferId5(&client, false);
        QList<QByteArray> frames = classicResponse();
        frames[0][0] = char(quint8(frames[0].at(0)) ^ 1U);
        qint64 now = 5001;
        for (int i = 0; i < frames.size() - 1; ++i) {
            QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                                       frames[i], false, now++));
        }
        QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                    frames.last(), false, now));
        QCOMPARE(failures.count(), 0);
        QCOMPARE(client.pendingRequestCount(), 1);
    }
    {
        DroneCanGetNodeInfoClient client;
        QSignalSpy failures(&client,
                            &DroneCanGetNodeInfoClient::requestFailed);
        QVERIFY(client.bindSession({91, 1, 127}));
        advanceNode42ToTransferId5(&client, true);
        QByteArray frame = hex(
            "0403020153efbe0102034433221108070605040302010304000102030405060708090a0b0c0d0e0f02cafe10dee4ce5ce8cae6e80000000000000000000000c5");
        frame[62] = char(1);
        QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                    frame, true, 5001));
        QCOMPARE(failures.count(), 0);
        QCOMPARE(client.pendingRequestCount(), 1);
    }
    {
        DroneCanGetNodeInfoClient client;
        QSignalSpy failures(&client,
                            &DroneCanGetNodeInfoClient::requestFailed);
        QVERIFY(client.bindSession({91, 1, 127}));
        advanceNode42ToTransferId5(&client, true);
        QList<QByteArray> frames = fdMultiResponse();
        frames[1][18] = char(1); // CAN-FD DLC padding is CRC-covered.
        QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                                   frames[0], true, 5001));
        QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                    frames[1], true, 5002));
        QCOMPARE(failures.count(), 0);
        QCOMPARE(client.pendingRequestCount(), 1);
    }
}

void DroneCanGetNodeInfoClientTest::deadlinesRetryAndEventuallyFail()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy transmissions(
        &client, &DroneCanGetNodeInfoClient::transmitRequested);
    QSignalSpy failures(&client,
                        &DroneCanGetNodeInfoClient::requestFailed);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.requestNodeInfo(42, false, 0));
    for (int retry = 1; retry < DroneCanGetNodeInfoClient::MaximumAttempts;
         ++retry) {
        client.tick(qint64(retry) * 1000);
        QCOMPARE(transmissions.count(), retry + 1);
    }
    QCOMPARE(failures.count(), 0);
    QCOMPARE(transmissions.count(),
             DroneCanGetNodeInfoClient::MaximumAttempts);
    client.tick(qint64(DroneCanGetNodeInfoClient::MaximumAttempts) * 1000);
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.first().at(0).toInt(), 42);
    QCOMPARE(client.pendingRequestCount(), 0);
}

void DroneCanGetNodeInfoClientTest::mixedTransportAndAssemblyTimeoutRestartSafely()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy transmissions(
        &client, &DroneCanGetNodeInfoClient::transmitRequested);
    QSignalSpy failures(&client,
                        &DroneCanGetNodeInfoClient::requestFailed);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.requestNodeInfo(42, false, 0));

    QByteArray first = classicResponse().first();
    first[first.size() - 1] = char(0x80); // TID 0, SOT.
    QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                               first, false, 10));
    QByteArray mixed = classicResponse().at(1);
    mixed[mixed.size() - 1] = char(0x20); // TID 0, toggle one.
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                mixed, true, 11));
    QCOMPARE(failures.count(), 0);
    client.tick(499);
    QCOMPARE(transmissions.count(), 1);
    client.tick(500);
    QCOMPARE(transmissions.count(), 2);
    QCOMPARE(transmissions.last().at(1).toByteArray(), hex("c1"));

    // A late tail belonging to the abandoned TID cannot complete the retry.
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                mixed, false, 501));
    QByteArray restarted = classicResponse().first();
    restarted[restarted.size() - 1] = char(0x81); // New SOT, TID 1.
    QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                               restarted, false, 502));
    client.tick(2501);
    QCOMPARE(transmissions.count(), 2);
    client.tick(2502);
    QCOMPARE(transmissions.count(), 3);
    QCOMPARE(transmissions.last().at(1).toByteArray(), hex("c2"));
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                restarted, false, 2503));
    QCOMPARE(failures.count(), 0);
}

void DroneCanGetNodeInfoClientTest::assemblyLimitDropsOnlyTheTransfer()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy failures(&client,
                        &DroneCanGetNodeInfoClient::requestFailed);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.requestNodeInfo(42, true, 0));

    QByteArray first(64, char(0x5A));
    first[0] = 0;
    first[1] = 0;
    first[63] = char(0x80); // TID 0, SOT, first 61 data bytes.
    QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                               first, true, 10));
    for (int index = 0; index < 5; ++index) {
        QByteArray middle(64, char(0x5A));
        middle[63] = char((index % 2 == 0) ? 0x20 : 0x00);
        QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                                   middle, true, 11 + index));
    }
    // 61 + 5*63 == 376; one more frame would exceed the 392-byte cap.
    QByteArray overflow(64, char(0x5A));
    overflow[63] = char(0x00);
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                overflow, true, 20));
    QCOMPARE(failures.count(), 0);
    QCOMPARE(client.pendingRequestCount(), 1);
}

void DroneCanGetNodeInfoClientTest::refreshEpochPreservesTransferIdSequence()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy transmissions(
        &client, &DroneCanGetNodeInfoClient::transmitRequested);
    QSignalSpy received(&client,
                        &DroneCanGetNodeInfoClient::nodeInfoReceived);
    QVERIFY(client.bindSession({91, 1, 127}));
    QVERIFY(client.requestNodeInfo(42, false, 0));
    QVERIFY(client.requestNodeInfo(43, false, 0));
    QCOMPARE(transmissions.count(), 2);
    QCOMPARE(transmissions.at(0).at(0).toUInt(), RequestIdNode42);
    QCOMPARE(transmissions.at(0).at(1).toByteArray(), hex("c0"));
    QCOMPARE(transmissions.at(1).at(0).toUInt(), RequestIdNode43);
    QCOMPARE(transmissions.at(1).at(1).toByteArray(), hex("c0"));

    client.clearPendingRequests();
    QVERIFY(client.requestNodeInfo(42, false, 10));
    QVERIFY(client.requestNodeInfo(43, false, 10));
    QCOMPARE(transmissions.count(), 4);
    QCOMPARE(transmissions.at(2).at(0).toUInt(), RequestIdNode42);
    QCOMPARE(transmissions.at(2).at(1).toByteArray(), hex("c1"));
    QCOMPARE(transmissions.at(3).at(0).toUInt(), RequestIdNode43);
    QCOMPARE(transmissions.at(3).at(1).toByteArray(), hex("c1"));

    qint64 now = 11;
    for (const QByteArray &frame : withTransferId(classicResponse(), 0)) {
        QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                    frame, false, now++));
    }
    QCOMPARE(received.count(), 0);
    for (const QByteArray &frame : withTransferId(classicResponse(), 1)) {
        QVERIFY(client.acceptFrame(91, 1, ResponseIdNode42,
                                   frame, false, now++));
    }
    QCOMPARE(received.count(), 1);
}

void DroneCanGetNodeInfoClientTest::synchronousResetDoesNotReportPendingRequest()
{
    DroneCanGetNodeInfoClient client;
    QVERIFY(client.bindSession({91, 1, 127}));
    connect(&client, &DroneCanGetNodeInfoClient::transmitRequested,
            &client, [&client](quint32, const QByteArray &, bool) {
        client.resetSession();
    });

    QVERIFY(!client.requestNodeInfo(42, false, 0));
    QVERIFY(!client.isBound());
    QCOMPARE(client.pendingRequestCount(), 0);
}

void DroneCanGetNodeInfoClientTest::resetAndRebindInvalidateOldTraffic()
{
    DroneCanGetNodeInfoClient client;
    QSignalSpy received(&client,
                        &DroneCanGetNodeInfoClient::nodeInfoReceived);
    QVERIFY(client.bindSession({91, 1, 127}));
    advanceNode42ToTransferId5(&client, false);
    client.resetSession();
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                classicResponse().first(), false, 5001));
    QVERIFY(client.bindSession({92, 0, 126}));
    QVERIFY(client.requestNodeInfo(42, false, 6000));
    QVERIFY(!client.acceptFrame(91, 1, ResponseIdNode42,
                                classicResponse().first(), false, 6001));
    QCOMPARE(received.count(), 0);
}

QTEST_MAIN(DroneCanGetNodeInfoClientTest)
#include "test_dronecangetnodeinfo.moc"
