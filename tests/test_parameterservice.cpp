#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/ParameterService.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterStore.h"
#include "uas/QGCUASParamManager.h"

#include <QtTest>

#include <algorithm>
#include <cstring>

namespace
{

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("Link %1").arg(linkId);
    result.componentName = QStringLiteral("Component %1").arg(componentId);
    return result;
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    if (state != MAVLINK_FRAMING_OK) {
        return {};
    }
    return message;
}

QByteArray parameterId(const char id[16])
{
    int length = 0;
    while (length < 16 && id[length] != '\0') {
        ++length;
    }
    return QByteArray(id, length);
}

void copyParameterId(const QString &name, char id[16])
{
    const QByteArray bytes = name.toLatin1();
    const int count = std::min(16, static_cast<int>(bytes.size()));
    std::memset(id, 0, 16);
    if (count > 0) {
        std::memcpy(id, bytes.constData(), static_cast<size_t>(count));
    }
}

mavlink_message_t parameterValue(
    int sourceSystem, int sourceComponent, const QString &name,
    const QVariant &value, ParameterType type,
    quint16 count = 1, quint16 index = 0,
    ParameterEncoding encoding = ParameterEncoding::Bytewise)
{
    bool encoded = false;
    const float wireValue = ParameterCodec::encodeClassic(
        value, type, encoding, &encoded);
    Q_ASSERT(encoded);

    mavlink_param_value_t payload{};
    copyParameterId(name, payload.param_id);
    payload.param_value = wireValue;
    payload.param_type = static_cast<quint8>(type);
    payload.param_count = count;
    payload.param_index = index;

    mavlink_message_t message{};
    mavlink_msg_param_value_encode(
        static_cast<quint8>(sourceSystem),
        static_cast<quint8>(sourceComponent), &message, &payload);
    return message;
}

} // namespace

class ParameterServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void requestsUseOnlyTheExactSelectedEndpoint();
    void cachesWithEqualMavIdsAreIsolatedByLink();
    void listIsCommittedOnlyWhenAllIndexedValuesArrive();
    void listRetriesTwiceThenRequestsMissingIndicesInBoundedBursts();
    void listAtSeventyFivePercentSkipsWholeListRetries();
    void listRetryTimersStopAfterCancellationAndTargetSwitch();
    void terminalListFailurePreservesCommittedSnapshot();
    void readByIndexUsesOnlyTheExactSelectedEndpointAndBlankId();
    void invalidStaleAndUnavailableTargetsFailClosed();
    void readAndWriteAcknowledgementsRequireExactTransactions();
    void mismatchingWriteEchoRemainsPendingAndUpdatesCache();
    void writesAreSerializedAndRetryExactlyThreeTimes();
    void sameValueWriteIsSkippedWithoutTraffic();
    void noOpDecisionIsRecheckedWhenWriteBecomesActive();
    void queuedWriteUsesActivationEncodingAndCurrentSchema();
    void batchWritesNormalParametersBeforeEnableParameters();
    void parameterListFormsABoundaryInTheWriteQueue();
    void cancellingQueuedWriteReleasesListBoundary();
    void synchronousAckCannotArmATimerForTheNextWrite();
    void storeSignalCanQueueSameNameWriteDuringAcknowledgement();
    void targetSwitchCancelsActiveAndQueuedWritesWithoutLateRetry();
    void targetSwitchPreservesReentrantNewTargetWrite();
    void nestedTargetSwitchDefersListAndRejectsEarlyRead();
    void compatibilityFacadeUsesCommittedTypeAndExactTarget();
    void compatibilityFacadeHidesStagedRefreshUntilCommit();
    void compatibilityFacadeReportsInitialSendFailure();
    void compatibilityFacadeOrdersSnapshotBeforeReady();
    void compatibilityFacadePreservesReentrantNewTargetList();
    void compatibilityFacadeDefersNewTargetRequestDuringDispatch();
    void compatibilityFacadeDefersReadDuringInvalidation();
    void compatibilityFacadeStopsReplayAfterNestedSwitch();
    void compatibilityFacadeCoalescesSameTurnFailures();
    void commandAndParameterTrafficShareOneLinkSequence();
    void targetSwitchCancelsPendingTransactions();
};

void ParameterServiceTest::requestsUseOnlyTheExactSelectedEndpoint()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);

    QVERIFY(targets.observeEndpoint(endpoint(4, 42, 1), true));
    QVERIFY(targets.observeEndpoint(endpoint(9, 42, 100)));
    QVERIFY(targets.selectTarget(9, 42, 100));
    const VehicleTargetLease lease = targets.acquireTarget();
    service.setEncoding(lease.endpoint, ParameterEncoding::Bytewise);

    QCOMPARE(service.requestParameterList(lease, 250, 190),
             ParameterService::SendResult::Sent);
    QCOMPARE(service.requestParameterRead(
                 lease, 250, 190, QStringLiteral("SERVO1_FUNCTION")),
             ParameterService::SendResult::Sent);
    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("SERVO1_MIN"),
                 qint32(-123), ParameterType::Int16),
             ParameterService::SendResult::Sent);
    // PARAM_SET waits behind the active list transaction so a PARAM_VALUE can
    // never be mistaken for both list data and a write acknowledgement.
    QCOMPARE(frames.size(), 2);
    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("LIST_DONE"), qint32(1),
                          ParameterType::Int32));
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames.at(0).linkId, 9);
    QCOMPARE(frames.at(1).linkId, 9);
    QCOMPARE(frames.at(2).linkId, 9);

    const mavlink_message_t listMessage = decodeFrame(frames.at(0).bytes);
    QCOMPARE(listMessage.msgid, quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));
    QCOMPARE(listMessage.sysid, quint8(250));
    QCOMPARE(listMessage.compid, quint8(190));
    mavlink_param_request_list_t list{};
    mavlink_msg_param_request_list_decode(&listMessage, &list);
    QCOMPARE(list.target_system, quint8(42));
    QCOMPARE(list.target_component, quint8(100));

    const mavlink_message_t readMessage = decodeFrame(frames.at(1).bytes);
    QCOMPARE(readMessage.msgid, quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
    QCOMPARE(readMessage.sysid, quint8(250));
    QCOMPARE(readMessage.compid, quint8(190));
    mavlink_param_request_read_t read{};
    mavlink_msg_param_request_read_decode(&readMessage, &read);
    QCOMPARE(read.target_system, quint8(42));
    QCOMPARE(read.target_component, quint8(100));
    QCOMPARE(read.param_index, qint16(-1));
    QCOMPARE(parameterId(read.param_id), QByteArray("SERVO1_FUNCTION"));

    const mavlink_message_t setMessage = decodeFrame(frames.at(2).bytes);
    QCOMPARE(setMessage.msgid, quint32(MAVLINK_MSG_ID_PARAM_SET));
    QCOMPARE(setMessage.sysid, quint8(250));
    QCOMPARE(setMessage.compid, quint8(190));
    mavlink_param_set_t set{};
    mavlink_msg_param_set_decode(&setMessage, &set);
    QCOMPARE(set.target_system, quint8(42));
    QCOMPARE(set.target_component, quint8(100));
    QCOMPARE(parameterId(set.param_id), QByteArray("SERVO1_MIN"));
    QCOMPARE(set.param_type, quint8(ParameterType::Int16));
    bool decoded = false;
    const QVariant setValue = ParameterCodec::decodeClassic(
        set.param_value, ParameterType::Int16,
        ParameterEncoding::Bytewise, &decoded);
    QVERIFY(decoded);
    QCOMPARE(setValue.toInt(), -123);
}

void ParameterServiceTest::cachesWithEqualMavIdsAreIsolatedByLink()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint radio = endpoint(7, 42, 1);
    const VehicleEndpoint simulator = endpoint(8, 42, 1);
    QVERIFY(targets.observeEndpoint(radio, true));
    QVERIFY(targets.observeEndpoint(simulator));

    QCOMPARE(service.requestParameterList(
                 targets.acquireTarget(), 250, 190),
             ParameterService::SendResult::Sent);
    service.observeMessage(
        7, parameterValue(42, 1, QStringLiteral("SYSID_THISMAV"),
                          qint32(42), ParameterType::Int32));

    QVERIFY(targets.selectTarget(8, 42, 1));
    QCOMPARE(service.requestParameterList(
                 targets.acquireTarget(), 250, 190),
             ParameterService::SendResult::Sent);
    service.observeMessage(
        8, parameterValue(42, 1, QStringLiteral("SYSID_THISMAV"),
                          qint32(84), ParameterType::Int32));

    QCOMPARE(service.store()->snapshot(radio)
                 .value(1, QStringLiteral("SYSID_THISMAV")).value.toInt(), 42);
    QCOMPARE(service.store()->snapshot(simulator)
                 .value(1, QStringLiteral("SYSID_THISMAV")).value.toInt(), 84);
    QCOMPARE(service.store()->cachedEndpoints().size(), 2);
}

void ParameterServiceTest::listIsCommittedOnlyWhenAllIndexedValuesArrive()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(11, 70, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    const VehicleTargetLease lease = targets.acquireTarget();
    QCOMPARE(service.requestParameterList(lease, 250, 190),
             ParameterService::SendResult::Sent);
    QSignalSpy completed(&service, &ParameterService::listCompleted);

    service.observeMessage(
        11, parameterValue(70, 1, QStringLiteral("_HASH_CHECK"),
                           quint32(100), ParameterType::UInt32,
                           65535, 65535));
    QCOMPARE(completed.count(), 0);
    QCOMPARE(service.store()->snapshot(target).records().size(), 0);
    QCOMPARE(service.store()->snapshot(target).progress().received, 0);
    QCOMPARE(service.store()->snapshot(target).progress().reported, 0);

    service.observeMessage(
        11, parameterValue(70, 1, QStringLiteral("A"), qint32(1),
                           ParameterType::Int32, 2, 0));
    QCOMPARE(completed.count(), 0);
    QCOMPARE(service.store()->snapshot(target).records().size(), 0);

    service.observeMessage(
        11, parameterValue(70, 1, QStringLiteral("B"), qint32(2),
                           ParameterType::Int32, 65535, 1));
    QCOMPARE(completed.count(), 1);
    QCOMPARE(completed.first().at(0).toULongLong(), lease.generation);
    QCOMPARE(completed.first().at(1).toInt(), 11);
    QCOMPARE(completed.first().at(2).toInt(), 70);
    QCOMPARE(completed.first().at(3).toInt(), 1);
    const ParameterSnapshot committed = service.store()->snapshot(target);
    QVERIFY(committed.isComplete());
    QCOMPARE(committed.progress().received, 2);
    QCOMPARE(committed.progress().reported, 2);
    QVERIFY(committed.contains(1, QStringLiteral("_HASH_CHECK")));
    QVERIFY(committed.contains(1, QStringLiteral("A")));
    QVERIFY(committed.contains(1, QStringLiteral("B")));
}

void ParameterServiceTest::listRetriesTwiceThenRequestsMissingIndicesInBoundedBursts()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    // Production uses 4000 ms of inactivity and 1000 ms between bursts.
    // Keep the burst interval long enough that this test observes exactly the
    // first burst before a later retry can be scheduled.
    service.setRetryPolicyForTesting(30, 1000, 2, 3);

    const VehicleEndpoint target = endpoint(12, 73, 101);
    QVERIFY(targets.observeEndpoint(target, true));
    const VehicleTargetLease lease = targets.acquireTarget();
    QCOMPARE(service.requestParameterList(lease, 251, 191),
             ParameterService::SendResult::Sent);
    service.observeMessage(
        12, parameterValue(73, 101, QStringLiteral("P0"), qint32(0),
                           ParameterType::Int32, 20, 0));

    // Less than 75% progress permits no more than two whole-list retries.
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= 3, 500);
    for (int frameIndex = 0; frameIndex < 3; ++frameIndex) {
        const mavlink_message_t message =
            decodeFrame(frames.at(frameIndex).bytes);
        QCOMPARE(message.msgid,
                 quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));
        QCOMPARE(frames.at(frameIndex).linkId, 12);
    }

    // The next inactivity transition enters one-by-one recovery. A burst is
    // bounded to ten missing indices even though nineteen values are absent.
    QTRY_COMPARE_WITH_TIMEOUT(frames.size(), 13, 500);
    for (int frameIndex = 3; frameIndex < frames.size(); ++frameIndex) {
        QCOMPARE(frames.at(frameIndex).linkId, 12);
        const mavlink_message_t message =
            decodeFrame(frames.at(frameIndex).bytes);
        QCOMPARE(message.msgid,
                 quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
        QCOMPARE(message.sysid, quint8(251));
        QCOMPARE(message.compid, quint8(191));

        mavlink_param_request_read_t request{};
        mavlink_msg_param_request_read_decode(&message, &request);
        QCOMPARE(request.target_system, quint8(73));
        QCOMPARE(request.target_component, quint8(101));
        QCOMPARE(request.param_index, qint16(frameIndex - 2));
        const int parameterIdSize = int(sizeof(request.param_id));
        QCOMPARE(QByteArray(request.param_id, parameterIdSize),
                 QByteArray(parameterIdSize, '\0'));
    }
}

void ParameterServiceTest::listAtSeventyFivePercentSkipsWholeListRetries()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setRetryPolicyForTesting(30, 1000, 2, 3);

    const VehicleEndpoint target = endpoint(13, 74, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    const VehicleTargetLease lease = targets.acquireTarget();
    QCOMPARE(service.requestParameterList(lease, 252, 192),
             ParameterService::SendResult::Sent);
    for (quint16 index = 0; index < 6; ++index) {
        service.observeMessage(
            13, parameterValue(74, 1,
                               QStringLiteral("P%1").arg(index),
                               qint32(index), ParameterType::Int32,
                               8, index));
    }

    // Exactly 75% goes directly to missing-index recovery: only indices 6
    // and 7 are requested and there is no whole-list retry.
    QTRY_COMPARE_WITH_TIMEOUT(frames.size(), 3, 500);
    QCOMPARE(decodeFrame(frames.first().bytes).msgid,
             quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));
    for (int frameIndex = 1; frameIndex < frames.size(); ++frameIndex) {
        const mavlink_message_t message =
            decodeFrame(frames.at(frameIndex).bytes);
        QCOMPARE(message.msgid,
                 quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
        mavlink_param_request_read_t request{};
        mavlink_msg_param_request_read_decode(&message, &request);
        QCOMPARE(request.param_index, qint16(frameIndex + 5));
    }
}

void ParameterServiceTest::listRetryTimersStopAfterCancellationAndTargetSwitch()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setRetryPolicyForTesting(25, 25, 2, 3);

    const VehicleEndpoint first = endpoint(14, 75, 1);
    const VehicleEndpoint second = endpoint(15, 75, 1);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    QSignalSpy listCancelled(&service, &ParameterService::listCancelled);

    const VehicleTargetLease firstLease = targets.acquireTarget();
    QCOMPARE(service.requestParameterList(firstLease, 253, 193),
             ParameterService::SendResult::Sent);
    QCOMPARE(frames.size(), 1);
    QVERIFY(service.cancelCurrentParameterList());
    QCOMPARE(listCancelled.count(), 1);
    QVERIFY(!service.cancelCurrentParameterList());
    QTest::qWait(125);
    QCOMPARE(frames.size(), 1);

    QCOMPARE(service.requestParameterList(firstLease, 253, 193),
             ParameterService::SendResult::Sent);
    QCOMPARE(frames.size(), 2);
    QVERIFY(targets.selectTarget(15, 75, 1));
    QCOMPARE(listCancelled.count(), 2);
    QTest::qWait(125);
    QCOMPARE(frames.size(), 2);
}

void ParameterServiceTest::terminalListFailurePreservesCommittedSnapshot()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(16, 76, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    const VehicleTargetLease lease = targets.acquireTarget();

    // Establish a complete, usable cache first.
    service.setRetryPolicyForTesting(1000, 1000, 2, 3);
    QCOMPARE(service.requestParameterList(lease, 254, 194),
             ParameterService::SendResult::Sent);
    service.observeMessage(
        16, parameterValue(76, 1, QStringLiteral("OLD_A"), qint32(10),
                           ParameterType::Int32, 2, 0));
    service.observeMessage(
        16, parameterValue(76, 1, QStringLiteral("OLD_B"), qint32(20),
                           ParameterType::Int32, 2, 1));
    QVERIFY(service.store()->snapshot(target).isComplete());

    // A refresh that never supplies index 1 is terminal after its sole
    // index attempt. Failed staging data must not replace the prior snapshot.
    service.setRetryPolicyForTesting(25, 25, 0, 1);
    QSignalSpy failed(&service, &ParameterService::listFailed);
    QCOMPARE(service.requestParameterList(lease, 254, 194),
             ParameterService::SendResult::Sent);
    service.observeMessage(
        16, parameterValue(76, 1, QStringLiteral("NEW_A"), qint32(99),
                           ParameterType::Int32, 2, 0));

    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 500);
    QCOMPARE(failed.first().at(0).toULongLong(), lease.generation);
    QCOMPARE(failed.first().at(1).toInt(), 16);
    QCOMPARE(failed.first().at(2).toInt(), 76);
    QCOMPARE(failed.first().at(3).toInt(), 1);
    QVERIFY(!failed.first().at(4).toString().isEmpty());

    const ParameterSnapshot preserved = service.store()->snapshot(target);
    QCOMPARE(preserved.state(), ParameterLoadState::Failed);
    QVERIFY(preserved.isComplete());
    QCOMPARE(preserved.records().size(), 2);
    QCOMPARE(preserved.value(1, QStringLiteral("OLD_A")).value.toInt(), 10);
    QCOMPARE(preserved.value(1, QStringLiteral("OLD_B")).value.toInt(), 20);
    QVERIFY(!preserved.contains(1, QStringLiteral("NEW_A")));
}

void ParameterServiceTest::readByIndexUsesOnlyTheExactSelectedEndpointAndBlankId()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);

    QVERIFY(targets.observeEndpoint(endpoint(17, 77, 1), true));
    QVERIFY(targets.observeEndpoint(endpoint(18, 77, 100)));
    QVERIFY(targets.selectTarget(18, 77, 100));
    const VehicleTargetLease lease = targets.acquireTarget();
    QCOMPARE(service.requestParameterReadByIndex(
                 lease, 249, 189, 123),
             ParameterService::SendResult::Sent);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().linkId, 18);

    const mavlink_message_t message = decodeFrame(frames.first().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
    QCOMPARE(message.sysid, quint8(249));
    QCOMPARE(message.compid, quint8(189));
    mavlink_param_request_read_t request{};
    mavlink_msg_param_request_read_decode(&message, &request);
    QCOMPARE(request.target_system, quint8(77));
    QCOMPARE(request.target_component, quint8(100));
    QCOMPARE(request.param_index, qint16(123));
    const int parameterIdSize = int(sizeof(request.param_id));
    QCOMPARE(QByteArray(request.param_id, parameterIdSize),
             QByteArray(parameterIdSize, '\0'));
}

void ParameterServiceTest::invalidStaleAndUnavailableTargetsFailClosed()
{
    VehicleTargetManager targets;
    int writes = 0;
    bool writerSucceeds = true;
    ExactLinkTransmitter transmitter(
        [&writes, &writerSucceeds](int, const QByteArray &) {
            ++writes;
            return writerSucceeds;
        });
    ParameterService service(&targets, &transmitter);

    QCOMPARE(service.requestParameterList({}, 250, 190),
             ParameterService::SendResult::InvalidTarget);
    QCOMPARE(writes, 0);

    QVERIFY(targets.observeEndpoint(endpoint(1), true));
    QVERIFY(targets.observeEndpoint(endpoint(2)));
    const VehicleTargetLease stale = targets.acquireTarget();
    QVERIFY(targets.selectTarget(2, 42, 1));
    QCOMPARE(service.requestParameterRead(
                 stale, 250, 190, QStringLiteral("ARMING_CHECK")),
             ParameterService::SendResult::StaleTarget);
    QCOMPARE(writes, 0);

    QCOMPARE(service.requestParameterRead(
                 targets.acquireTarget(), 250, 190, QString()),
             ParameterService::SendResult::InvalidParameter);
    QCOMPARE(service.requestParameterRead(
                 targets.acquireTarget(), 250, 190,
                 QStringLiteral("PARAMETER_NAME_IS_TOO_LONG")),
             ParameterService::SendResult::InvalidParameter);
    QCOMPARE(service.setParameter(
                 targets.acquireTarget(), 250, 190,
                 QStringLiteral("BAD"), QVariant(), ParameterType::Int32),
             ParameterService::SendResult::InvalidParameter);
    service.setEncoding(targets.acquireTarget().endpoint,
                        ParameterEncoding::CStyleCast);
    QCOMPARE(service.setParameter(
                 targets.acquireTarget(), 250, 190,
                 QStringLiteral("LOSSY_INT"), qint32(16777217),
                 ParameterType::Int32),
             ParameterService::SendResult::InvalidParameter);
    QCOMPARE(writes, 0);

    writerSucceeds = false;
    QCOMPARE(service.requestParameterList(
                 targets.acquireTarget(), 250, 190),
             ParameterService::SendResult::TransportUnavailable);
    QCOMPARE(writes, 1);
}

void ParameterServiceTest::readAndWriteAcknowledgementsRequireExactTransactions()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(9, 42, 100);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    const VehicleTargetLease lease = targets.acquireTarget();

    QCOMPARE(service.requestParameterRead(
                 lease, 250, 190, QStringLiteral("RATE_RLL_P")),
             ParameterService::SendResult::Sent);
    QSignalSpy reads(&service, &ParameterService::parameterRead);
    service.observeMessage(
        8, parameterValue(42, 100, QStringLiteral("RATE_RLL_P"), 0.15F,
                          ParameterType::Real32));
    service.observeMessage(
        9, parameterValue(41, 100, QStringLiteral("RATE_RLL_P"), 0.15F,
                          ParameterType::Real32));
    service.observeMessage(
        9, parameterValue(42, 1, QStringLiteral("RATE_RLL_P"), 0.15F,
                          ParameterType::Real32));
    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("RATE_PIT_P"), 0.15F,
                          ParameterType::Real32));
    QCOMPARE(reads.count(), 0);

    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("RATE_RLL_P"), 0.15F,
                          ParameterType::Real32));
    QCOMPARE(reads.count(), 1);
    QCOMPARE(reads.first().at(0).toULongLong(), lease.generation);
    QCOMPARE(reads.first().at(1).toInt(), 9);
    QCOMPARE(reads.first().at(2).toInt(), 42);
    QCOMPARE(reads.first().at(3).toInt(), 100);
    QCOMPARE(reads.first().at(4).toString(), QStringLiteral("RATE_RLL_P"));
    QCOMPARE(reads.first().at(5).toFloat(), 0.15F);
    QCOMPARE(reads.first().at(6).toInt(), int(ParameterType::Real32));

    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("INS_GYRO_FILTER"),
                 qint32(123456), ParameterType::Int32),
             ParameterService::SendResult::Sent);
    QSignalSpy writes(&service, &ParameterService::parameterWriteAcknowledged);
    service.observeMessage(
        8, parameterValue(42, 100, QStringLiteral("INS_GYRO_FILTER"),
                          qint32(123456), ParameterType::Int32));
    service.observeMessage(
        9, parameterValue(41, 100, QStringLiteral("INS_GYRO_FILTER"),
                          qint32(123456), ParameterType::Int32));
    service.observeMessage(
        9, parameterValue(42, 1, QStringLiteral("INS_GYRO_FILTER"),
                          qint32(123456), ParameterType::Int32));
    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("INS_ACCEL_FILTER"),
                          qint32(123456), ParameterType::Int32));
    QCOMPARE(writes.count(), 0);

    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("INS_GYRO_FILTER"),
                          qint32(123456), ParameterType::Int32));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toULongLong(), lease.generation);
    QCOMPARE(writes.first().at(1).toInt(), 9);
    QCOMPARE(writes.first().at(2).toInt(), 42);
    QCOMPARE(writes.first().at(3).toInt(), 100);
    QCOMPARE(writes.first().at(4).toString(),
             QStringLiteral("INS_GYRO_FILTER"));
    QCOMPARE(writes.first().at(5).toInt(), 123456);
    QCOMPARE(writes.first().at(6).toInt(), int(ParameterType::Int32));

    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("INS_GYRO_FILTER"),
                          qint32(123456), ParameterType::Int32));
    QCOMPARE(writes.count(), 1);
}

void ParameterServiceTest::mismatchingWriteEchoRemainsPendingAndUpdatesCache()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(9, 42, 100);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    const VehicleTargetLease lease = targets.acquireTarget();
    QSignalSpy failures(&service, &ParameterService::parameterWriteFailed);
    QSignalSpy acknowledgements(
        &service, &ParameterService::parameterWriteAcknowledged);

    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("INS_GYRO_FILTER"),
                 qint32(123456), ParameterType::Int32),
             ParameterService::SendResult::Sent);
    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("INS_GYRO_FILTER"),
                          qint32(123455), ParameterType::Int32));
    QCOMPARE(failures.count(), 0);
    QCOMPARE(acknowledgements.count(), 0);
    QCOMPARE(service.store()->snapshot(target)
                 .value(100, QStringLiteral("INS_GYRO_FILTER")).value.toInt(),
             123455);
    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("INS_GYRO_FILTER"),
                          qint32(123456), ParameterType::Int32));
    QCOMPARE(acknowledgements.count(), 1);

    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("INS_GYRO_FILTER"),
                 qint32(12345), ParameterType::Int32),
             ParameterService::SendResult::Sent);
    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("INS_GYRO_FILTER"),
                          qint32(12345), ParameterType::Int16));
    QCOMPARE(failures.count(), 0);
    QCOMPARE(acknowledgements.count(), 1);
    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("INS_GYRO_FILTER"),
                          qint32(12345), ParameterType::Int32));
    QCOMPARE(acknowledgements.count(), 2);
}

void ParameterServiceTest::writesAreSerializedAndRetryExactlyThreeTimes()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setWriteRetryPolicyForTesting(10, 3);
    const VehicleEndpoint target = endpoint(6, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    const VehicleTargetLease lease = targets.acquireTarget();
    QSignalSpy started(&service, &ParameterService::parameterWriteStarted);
    QSignalSpy retried(&service, &ParameterService::parameterWriteRetried);
    QSignalSpy failed(&service, &ParameterService::parameterWriteFailed);

    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("A"), qint32(1),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("B"), qint32(2),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(started.count(), 1);
    const mavlink_message_t firstMessage = decodeFrame(frames.first().bytes);
    mavlink_param_set_t firstPayload{};
    mavlink_msg_param_set_decode(&firstMessage, &firstPayload);
    QCOMPARE(parameterId(firstPayload.param_id), QByteArray("A"));

    service.observeMessage(
        6, parameterValue(42, 1, QStringLiteral("A"), qint32(1),
                          ParameterType::Int32));
    QCOMPARE(frames.size(), 2);
    QCOMPARE(started.count(), 2);

    QTRY_COMPARE_WITH_TIMEOUT(frames.size(), 5, 250);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 250);
    QCOMPARE(retried.count(), 3);
    QCOMPARE(retried.at(0).at(1).toInt(), 2);
    QCOMPARE(retried.at(1).at(1).toInt(), 3);
    QCOMPARE(retried.at(2).at(1).toInt(), 4);
    QCOMPARE(failed.constFirst().at(6).toString(), QStringLiteral("B"));
    QCOMPARE(failed.constFirst().at(7).toInt(),
             int(ParameterService::WriteFailureReason::Timeout));
}

void ParameterServiceTest::sameValueWriteIsSkippedWithoutTraffic()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(7, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    service.observeMessage(
        7, parameterValue(42, 1, QStringLiteral("UNCHANGED"), qint32(17),
                          ParameterType::Int32));
    frames.clear();
    QSignalSpy skipped(&service, &ParameterService::parameterWriteSkipped);
    QSignalSpy acknowledged(
        &service, &ParameterService::parameterWriteAcknowledged);

    const qulonglong transaction = service.writeCurrentParameter(
        QStringLiteral("UNCHANGED"), qint32(17));
    QVERIFY(transaction > 0);
    QCoreApplication::processEvents();
    QCOMPARE(frames.size(), 0);
    QCOMPARE(skipped.count(), 1);
    QCOMPARE(skipped.constFirst().at(0).toULongLong(), transaction);
    QCOMPARE(acknowledged.count(), 1);

    const qulonglong forced = service.writeCurrentParameter(
        QStringLiteral("UNCHANGED"), qint32(17), true);
    QVERIFY(forced > transaction);
    QCoreApplication::processEvents();
    QCOMPARE(frames.size(), 1);
}

void ParameterServiceTest::noOpDecisionIsRecheckedWhenWriteBecomesActive()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(17, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    service.observeMessage(
        17, parameterValue(42, 1, QStringLiteral("P"), qint32(0),
                           ParameterType::Int32));
    frames.clear();

    QVERIFY(service.writeCurrentParameter(
                QStringLiteral("P"), qint32(1)) > 0);
    QVERIFY(service.writeCurrentParameter(
                QStringLiteral("P"), qint32(0)) > 0);
    QCoreApplication::processEvents();
    QCOMPARE(frames.size(), 1);

    service.observeMessage(
        17, parameterValue(42, 1, QStringLiteral("P"), qint32(1),
                           ParameterType::Int32));
    QCOMPARE(frames.size(), 2);
    mavlink_param_set_t second{};
    const mavlink_message_t secondMessage =
        decodeFrame(frames.constLast().bytes);
    mavlink_msg_param_set_decode(&secondMessage, &second);
    bool decoded = false;
    QCOMPARE(ParameterCodec::decodeClassic(
                 second.param_value, ParameterType::Int32,
                 ParameterEncoding::Bytewise, &decoded).toInt(), 0);
    QVERIFY(decoded);
    service.observeMessage(
        17, parameterValue(42, 1, QStringLiteral("P"), qint32(0),
                           ParameterType::Int32));
    QCOMPARE(service.currentParameterValue(QStringLiteral("P")).toInt(), 0);
}

void ParameterServiceTest::queuedWriteUsesActivationEncodingAndCurrentSchema()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(18, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    service.observeMessage(
        18, parameterValue(42, 1, QStringLiteral("A"), qint32(0),
                           ParameterType::Int32, 2, 0));
    service.observeMessage(
        18, parameterValue(42, 1, QStringLiteral("P"), qint32(0),
                           ParameterType::Int32, 2, 1));
    frames.clear();
    const VehicleTargetLease lease = targets.acquireTarget();
    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("A"), qint32(1),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QVERIFY(service.writeCurrentParameter(
                QStringLiteral("P"), qint32(16777217)) > 0);
    service.setEncoding(target, ParameterEncoding::CStyleCast);
    QSignalSpy failures(&service, &ParameterService::parameterWriteFailed);
    service.observeMessage(
        18, parameterValue(42, 1, QStringLiteral("A"), qint32(1),
                           ParameterType::Int32, 2, 0));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.constFirst().at(7).toInt(),
             int(ParameterService::WriteFailureReason::InvalidParameter));

    // A schema-bound write queued after a list boundary is rejected if that
    // refresh removes the parameter from the committed snapshot.
    service.setEncoding(target, ParameterEncoding::Bytewise);
    frames.clear();
    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("A"), qint32(2),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(service.requestParameterList(lease, 250, 190),
             ParameterService::SendResult::Sent);
    QVERIFY(service.writeCurrentParameter(
                QStringLiteral("P"), qint32(2)) > 0);
    service.observeMessage(
        18, parameterValue(42, 1, QStringLiteral("A"), qint32(2),
                           ParameterType::Int32));
    QCOMPARE(frames.size(), 2);
    QCOMPARE(decodeFrame(frames.constLast().bytes).msgid,
             quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));
    service.observeMessage(
        18, parameterValue(42, 1, QStringLiteral("A"), qint32(2),
                           ParameterType::Int32, 1, 0));
    QCOMPARE(frames.size(), 2);
    QCOMPARE(failures.count(), 2);
    QCOMPARE(failures.constLast().at(7).toInt(),
             int(ParameterService::WriteFailureReason::InvalidParameter));
}

void ParameterServiceTest::batchWritesNormalParametersBeforeEnableParameters()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(8, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    const QStringList names{QStringLiteral("Z_ENABLE"), QStringLiteral("Z"),
                            QStringLiteral("A_ENABLE"), QStringLiteral("A")};
    for (int index = 0; index < names.size(); ++index) {
        service.observeMessage(
            8, parameterValue(42, 1, names.at(index), qint32(0),
                              ParameterType::Int32,
                              static_cast<quint16>(names.size()),
                              static_cast<quint16>(index)));
    }
    frames.clear();
    QSignalSpy completed(&service, &ParameterService::parameterBatchCompleted);

    QVariantList changes;
    for (const QString &name : names) {
        changes.append(QVariantMap{{QStringLiteral("name"), name},
                                   {QStringLiteral("value"), qint32(1)}});
    }
    const qulonglong batchId = service.writeCurrentParameters(changes);
    QVERIFY(batchId > 0);
    QCoreApplication::processEvents();

    QStringList sentNames;
    while (sentNames.size() < 4) {
        QCOMPARE(frames.size(), sentNames.size() + 1);
        mavlink_param_set_t payload{};
        const mavlink_message_t message =
            decodeFrame(frames.constLast().bytes);
        mavlink_msg_param_set_decode(&message, &payload);
        const QString name = QString::fromLatin1(parameterId(payload.param_id));
        sentNames.append(name);
        service.observeMessage(
            8, parameterValue(42, 1, name, qint32(1),
                              ParameterType::Int32, 4,
                              static_cast<quint16>(sentNames.size() - 1)));
    }
    QCOMPARE(sentNames,
             QStringList({QStringLiteral("A"), QStringLiteral("Z"),
                          QStringLiteral("A_ENABLE"),
                          QStringLiteral("Z_ENABLE")}));
    QCOMPARE(completed.count(), 1);
    QCOMPARE(completed.constFirst().at(0).toULongLong(), batchId);
    QCOMPARE(completed.constFirst().at(1).toInt(), 4);
    QCOMPARE(completed.constFirst().at(2).toInt(), 0);
}

void ParameterServiceTest::cancellingQueuedWriteReleasesListBoundary()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(19, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    const QStringList names{QStringLiteral("A"), QStringLiteral("B"),
                            QStringLiteral("C")};
    for (int index = 0; index < names.size(); ++index) {
        service.observeMessage(
            19, parameterValue(42, 1, names.at(index), qint32(0),
                               ParameterType::Int32, 3,
                               static_cast<quint16>(index)));
    }
    frames.clear();
    const qulonglong first = service.writeCurrentParameter(
        QStringLiteral("A"), qint32(1));
    const qulonglong cancelled = service.writeCurrentParameter(
        QStringLiteral("B"), qint32(1));
    QVERIFY(first > 0);
    QVERIFY(cancelled > first);
    const VehicleTargetLease lease = targets.acquireTarget();
    QCOMPARE(service.requestParameterList(lease, 250, 190),
             ParameterService::SendResult::Sent);
    QVERIFY(service.writeCurrentParameter(
                QStringLiteral("C"), qint32(1)) > cancelled);
    QVERIFY(service.cancelParameterWrite(cancelled));
    QCoreApplication::processEvents();
    QCOMPARE(frames.size(), 1);
    service.observeMessage(
        19, parameterValue(42, 1, QStringLiteral("A"), qint32(1),
                           ParameterType::Int32));
    QCOMPARE(frames.size(), 2);
    QCOMPARE(decodeFrame(frames.constLast().bytes).msgid,
             quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));
}

void ParameterServiceTest::parameterListFormsABoundaryInTheWriteQueue()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(10, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    const VehicleTargetLease lease = targets.acquireTarget();
    for (int index = 0; index < 3; ++index) {
        service.observeMessage(
            10, parameterValue(42, 1, QString(QChar('A' + index)), qint32(0),
                               ParameterType::Int32, 3,
                               static_cast<quint16>(index)));
    }
    frames.clear();

    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("A"), qint32(1),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("B"), qint32(1),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(service.requestParameterList(lease, 250, 190),
             ParameterService::SendResult::Sent);
    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("C"), qint32(1),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(frames.size(), 1);

    service.observeMessage(
        10, parameterValue(42, 1, QStringLiteral("A"), qint32(1),
                           ParameterType::Int32));
    QCOMPARE(frames.size(), 2);
    service.observeMessage(
        10, parameterValue(42, 1, QStringLiteral("B"), qint32(1),
                           ParameterType::Int32));
    QCOMPARE(frames.size(), 3);
    QCOMPARE(decodeFrame(frames.at(2).bytes).msgid,
             quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));

    for (int index = 0; index < 3; ++index) {
        service.observeMessage(
            10, parameterValue(42, 1, QString(QChar('A' + index)), qint32(0),
                               ParameterType::Int32, 3,
                               static_cast<quint16>(index)));
    }
    QCOMPARE(frames.size(), 4);
    mavlink_param_set_t finalWrite{};
    const mavlink_message_t finalMessage = decodeFrame(frames.at(3).bytes);
    mavlink_msg_param_set_decode(&finalMessage, &finalWrite);
    QCOMPARE(parameterId(finalWrite.param_id), QByteArray("C"));
}

void ParameterServiceTest::synchronousAckCannotArmATimerForTheNextWrite()
{
    VehicleTargetManager targets;
    ParameterService *servicePointer = nullptr;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&servicePointer, &frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            if (!servicePointer) {
                return true;
            }
            const mavlink_message_t message = decodeFrame(bytes);
            mavlink_param_set_t set{};
            mavlink_msg_param_set_decode(&message, &set);
            bool decoded = false;
            const ParameterType type =
                static_cast<ParameterType>(set.param_type);
            const QVariant value = ParameterCodec::decodeClassic(
                set.param_value, type, ParameterEncoding::Bytewise, &decoded);
            if (decoded) {
                servicePointer->observeMessage(
                    linkId,
                    parameterValue(
                        set.target_system, set.target_component,
                        QString::fromLatin1(parameterId(set.param_id)),
                        value, type));
            }
            return true;
        });
    ParameterService service(&targets, &transmitter);
    servicePointer = &service;
    service.setWriteRetryPolicyForTesting(5, 3);
    const VehicleEndpoint target = endpoint(11, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    const VehicleTargetLease lease = targets.acquireTarget();
    QSignalSpy acknowledged(
        &service, &ParameterService::parameterWriteAcknowledged);
    QSignalSpy retried(&service, &ParameterService::parameterWriteRetried);

    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("A"), qint32(1),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("B"), qint32(2),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(acknowledged.count(), 2);
    QTest::qWait(25);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(retried.count(), 0);
}

void ParameterServiceTest::storeSignalCanQueueSameNameWriteDuringAcknowledgement()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(12, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    service.observeMessage(
        12, parameterValue(42, 1, QStringLiteral("P"), qint32(0),
                           ParameterType::Int32));
    frames.clear();
    bool queuedReplacement = false;
    connect(service.store(), &ParameterStore::parameterChanged,
            this, [&service, &queuedReplacement](int component,
                                                  const QString &name) {
        if (!queuedReplacement && component == 1
            && name == QLatin1String("P")
            && service.currentParameterValue(name).toInt() == 1) {
            queuedReplacement = true;
            QVERIFY(service.writeCurrentParameter(name, qint32(2)) > 0);
        }
    });
    QSignalSpy acknowledged(
        &service, &ParameterService::parameterWriteAcknowledged);

    QVERIFY(service.writeCurrentParameter(QStringLiteral("P"), qint32(1)) > 0);
    QCoreApplication::processEvents();
    QCOMPARE(frames.size(), 1);
    service.observeMessage(
        12, parameterValue(42, 1, QStringLiteral("P"), qint32(1),
                           ParameterType::Int32));
    QVERIFY(queuedReplacement);
    QCOMPARE(acknowledged.count(), 1);
    QCOMPARE(frames.size(), 2);
    service.observeMessage(
        12, parameterValue(42, 1, QStringLiteral("P"), qint32(2),
                           ParameterType::Int32));
    QCOMPARE(acknowledged.count(), 2);
    QCOMPARE(service.currentParameterValue(QStringLiteral("P")).toInt(), 2);
}

void ParameterServiceTest::targetSwitchPreservesReentrantNewTargetWrite()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint first = endpoint(20, 42, 1);
    const VehicleEndpoint second = endpoint(21, 43, 1);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    const VehicleTargetLease oldLease = targets.acquireTarget();
    QCOMPARE(service.setParameter(
                 oldLease, 250, 190, QStringLiteral("OLD"), qint32(1),
                 ParameterType::Int32), ParameterService::SendResult::Sent);

    bool queuedForNewTarget = false;
    connect(&service, &ParameterService::parameterWriteCancelled,
            this, [&service, &targets, &queuedForNewTarget](
                      qulonglong, qulonglong, qulonglong,
                      int, int, int, const QString &) {
        if (queuedForNewTarget) {
            return;
        }
        queuedForNewTarget = true;
        const VehicleTargetLease newLease = targets.acquireTarget();
        QCOMPARE(service.setParameter(
                     newLease, 250, 190, QStringLiteral("NEW"), qint32(2),
                     ParameterType::Int32),
                 ParameterService::SendResult::Sent);
    });
    QVERIFY(targets.selectTarget(21, 43, 1));
    QVERIFY(queuedForNewTarget);
    QCoreApplication::processEvents();
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.constLast().linkId, 21);
    mavlink_param_set_t payload{};
    const mavlink_message_t message = decodeFrame(frames.constLast().bytes);
    mavlink_msg_param_set_decode(&message, &payload);
    QCOMPARE(parameterId(payload.param_id), QByteArray("NEW"));
}

void ParameterServiceTest::nestedTargetSwitchDefersListAndRejectsEarlyRead()
{
    VehicleTargetManager targets;
    const VehicleEndpoint first = endpoint(30, 42, 1);
    const VehicleEndpoint intermediate = endpoint(31, 43, 1);
    const VehicleEndpoint final = endpoint(32, 44, 42);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(intermediate));
    QVERIFY(targets.observeEndpoint(final));
    const quint64 initialGeneration = targets.targetGeneration();

    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService *servicePointer = nullptr;
    ParameterService::SendResult earlyRead =
        ParameterService::SendResult::InvalidTarget;
    ParameterService::SendResult queuedList =
        ParameterService::SendResult::InvalidTarget;
    connect(&targets, &VehicleTargetManager::targetGenerationChanged,
            this, [&targets, &servicePointer, &earlyRead, &queuedList,
                   initialGeneration](qulonglong generation) {
        if (generation != initialGeneration + 1 || !servicePointer) {
            return;
        }
        QVERIFY(targets.selectTarget(32, 44, 42));
        const VehicleTargetLease finalLease = targets.acquireTarget();
        earlyRead = servicePointer->requestParameterRead(
            finalLease, 250, 190, QStringLiteral("EARLY"));
        queuedList = servicePointer->requestParameterList(
            finalLease, 250, 190);
    });
    ParameterService service(&targets, &transmitter);
    servicePointer = &service;
    QSignalSpy cancellations(
        &service, &ParameterService::transactionsCancelled);

    QCOMPARE(service.requestCurrentParameterList(),
             int(ParameterService::SendResult::Sent));
    QCOMPARE(frames.size(), 1);
    QVERIFY(targets.selectTarget(31, 43, 1));

    QCOMPARE(earlyRead, ParameterService::SendResult::StaleTarget);
    QCOMPARE(queuedList, ParameterService::SendResult::Sent);
    QCOMPARE(targets.acquireTarget().endpoint.linkId, 32);
    QCOMPARE(cancellations.count(), 1);
    QCOMPARE(cancellations.constFirst().at(0).toULongLong(),
             initialGeneration);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.constLast().linkId, 32);
    QCOMPARE(decodeFrame(frames.constLast().bytes).msgid,
             quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));

    QSignalSpy reads(&service, &ParameterService::parameterRead);
    const VehicleTargetLease settledTarget = targets.acquireTarget();
    QCOMPARE(service.requestParameterRead(
                 settledTarget, 250, 190, QStringLiteral("EARLY")),
             ParameterService::SendResult::Sent);
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames.constLast().linkId, 32);
    service.observeMessage(
        32, parameterValue(44, 42, QStringLiteral("EARLY"), qint32(5),
                           ParameterType::Int32));
    QCOMPARE(reads.count(), 1);
}

void ParameterServiceTest::targetSwitchCancelsActiveAndQueuedWritesWithoutLateRetry()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setWriteRetryPolicyForTesting(5, 3);
    const VehicleEndpoint first = endpoint(13, 42, 1);
    const VehicleEndpoint second = endpoint(14, 43, 1);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    const VehicleTargetLease lease = targets.acquireTarget();
    QSignalSpy cancelled(&service, &ParameterService::parameterWriteCancelled);
    QSignalSpy generations(&service, &ParameterService::transactionsCancelled);

    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("A"), qint32(1),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(service.setParameter(
                 lease, 250, 190, QStringLiteral("B"), qint32(2),
                 ParameterType::Int32), ParameterService::SendResult::Sent);
    QCOMPARE(frames.size(), 1);
    QVERIFY(targets.selectTarget(14, 43, 1));
    QCOMPARE(cancelled.count(), 2);
    QCOMPARE(generations.count(), 1);
    QCOMPARE(generations.constFirst().at(0).toULongLong(), lease.generation);
    QTest::qWait(25);
    QCOMPARE(frames.size(), 1);

    service.observeMessage(
        13, parameterValue(42, 1, QStringLiteral("A"), qint32(1),
                           ParameterType::Int32));
    QVERIFY(!service.store()->snapshot(first)
                 .contains(1, QStringLiteral("A")));
}

void ParameterServiceTest::compatibilityFacadeUsesCommittedTypeAndExactTarget()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setLocalIdentity(251, 190);
    const VehicleEndpoint target = endpoint(9, 42, 100);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    QGCUASParamManager manager(&service, &targets);
    QSignalSpy acknowledged(
        &manager, &QGCUASParamManager::parameterWriteAcknowledged);
    QSignalSpy targetChanges(
        &manager, &QGCUASParamManager::parameterTargetChanged);

    QCOMPARE(service.requestCurrentParameterList(),
             int(ParameterService::SendResult::Sent));
    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("SERVO1_MIN"),
                          qint32(-123), ParameterType::Int16));
    QVERIFY(manager.parameterListReady());
    frames.clear();

    manager.setParameter(100, QStringLiteral("SERVO1_MIN"), qint32(-321));
    QCoreApplication::processEvents();
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.constFirst().linkId, 9);
    mavlink_message_t message = decodeFrame(frames.constFirst().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_PARAM_SET));
    QCOMPARE(message.sysid, quint8(251));
    QCOMPARE(message.compid, quint8(190));
    mavlink_param_set_t set{};
    mavlink_msg_param_set_decode(&message, &set);
    QCOMPARE(set.target_system, quint8(42));
    QCOMPARE(set.target_component, quint8(100));
    QCOMPARE(parameterId(set.param_id), QByteArray("SERVO1_MIN"));
    QCOMPARE(set.param_type, quint8(ParameterType::Int16));
    bool decoded = false;
    QCOMPARE(ParameterCodec::decodeClassic(
                 set.param_value, ParameterType::Int16,
                 ParameterEncoding::Bytewise, &decoded).toInt(), -321);
    QVERIFY(decoded);
    service.observeMessage(
        9, parameterValue(42, 100, QStringLiteral("SERVO1_MIN"),
                          qint32(-321), ParameterType::Int16));
    QCOMPARE(acknowledged.count(), 1);
    QCOMPARE(acknowledged.constFirst().at(0).toInt(), 100);
    QCOMPARE(acknowledged.constFirst().at(1).toString(),
             QStringLiteral("SERVO1_MIN"));
    QCOMPARE(acknowledged.constFirst().at(2).toInt(), -321);

    manager.setParameter(1, QStringLiteral("SERVO1_MIN"), qint32(100));
    manager.setParameter(100, QStringLiteral("UNKNOWN"), qint32(100));
    QCOMPARE(frames.size(), 1);

    manager.requestParameterUpdate(100, QStringLiteral("SERVO1_MIN"));
    QCOMPARE(frames.size(), 2);
    message = decodeFrame(frames.constLast().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
    mavlink_param_request_read_t read{};
    mavlink_msg_param_request_read_decode(&message, &read);
    QCOMPARE(read.target_system, quint8(42));
    QCOMPARE(read.target_component, quint8(100));
    QCOMPARE(parameterId(read.param_id), QByteArray("SERVO1_MIN"));

    manager.requestParameterUpdate(1, QStringLiteral("SERVO1_MIN"));
    QCOMPARE(frames.size(), 2);

    const VehicleEndpoint replacement = endpoint(10, 43, 100);
    QVERIFY(targets.observeEndpoint(replacement));
    QVERIFY(targets.selectTarget(10, 43, 100));
    QCOMPARE(targetChanges.count(), 1);
}

void ParameterServiceTest::compatibilityFacadeHidesStagedRefreshUntilCommit()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint target = endpoint(7, 42, 1);
    QVERIFY(targets.observeEndpoint(target, true));
    service.setEncoding(target, ParameterEncoding::Bytewise);
    QGCUASParamManager manager(&service, &targets);
    QSignalSpy snapshotChanges(
        &manager, &QGCUASParamManager::parameterSnapshotAboutToChange);
    QSignalSpy targetChanges(
        &manager, &QGCUASParamManager::parameterTargetChanged);

    int namedChanges = 0;
    QStringList changedNames;
    connect(&manager,
            static_cast<void (QGCUASParamManager::*)(
                int, QString, QVariant)>(
                &QGCUASParamManager::parameterChanged),
            this,
            [&namedChanges, &changedNames](int, const QString &name,
                                           const QVariant &) {
                ++namedChanges;
                changedNames.append(name);
            });

    manager.requestParameterList();
    service.observeMessage(
        7, parameterValue(42, 1, QStringLiteral("P"), qint32(1),
                          ParameterType::Int32));
    QCOMPARE(namedChanges, 1);
    QCOMPARE(manager.getParameterValue(1, QStringLiteral("P")).toInt(), 1);
    QVERIFY(manager.parameterListReady());
    QCOMPARE(snapshotChanges.count(), 1);

    namedChanges = 0;
    changedNames.clear();
    manager.requestParameterList();
    QVERIFY(manager.parameterListInProgress());
    QVERIFY(!manager.parameterListReady());
    service.observeMessage(
        7, parameterValue(42, 1, QStringLiteral("P"), qint32(2),
                          ParameterType::Int32, 2, 0));
    QCOMPARE(namedChanges, 0);
    QCOMPARE(manager.getParameterValue(1, QStringLiteral("P")).toInt(), 1);

    VehicleEndpoint renamed = target;
    renamed.linkName = QStringLiteral("Renamed link");
    QVERIFY(targets.observeEndpoint(renamed));
    QCOMPARE(targetChanges.count(), 0);
    QVERIFY(manager.parameterListInProgress());

    service.observeMessage(
        7, parameterValue(42, 1, QStringLiteral("Q"), qint32(3),
                          ParameterType::Int32, 2, 1));
    QVERIFY(!manager.parameterListInProgress());
    QVERIFY(manager.parameterListReady());
    QCOMPARE(namedChanges, 2);
    QCOMPARE(changedNames, QStringList({QStringLiteral("P"),
                                        QStringLiteral("Q")}));
    QCOMPARE(manager.getParameterValue(1, QStringLiteral("P")).toInt(), 2);
    QCOMPARE(manager.getParameterValue(1, QStringLiteral("Q")).toInt(), 3);
    QCOMPARE(snapshotChanges.count(), 2);
}

void ParameterServiceTest::compatibilityFacadeReportsInitialSendFailure()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return false; });
    ParameterService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(12, 77, 1), true));
    QGCUASParamManager manager(&service, &targets);
    QSignalSpy started(
        &manager, &QGCUASParamManager::parameterListLoadStarted);
    QSignalSpy failed(
        &manager, &QGCUASParamManager::parameterListLoadFailed);

    manager.requestParameterList();
    QCOMPARE(started.count(), 1);
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.constFirst().at(0).toString().contains(
        QStringLiteral("Unable to send")));
    QVERIFY(!manager.parameterListInProgress());
    QVERIFY(!manager.parameterListReady());
    QCOMPARE(service.store()->state(), ParameterLoadState::Failed);
}

void ParameterServiceTest::compatibilityFacadeOrdersSnapshotBeforeReady()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(12, 77, 1), true));
    service.setEncoding(targets.acquireTarget().endpoint,
                        ParameterEncoding::Bytewise);
    QGCUASParamManager manager(&service, &targets);
    QStringList order;
    connect(&manager, &QGCUASParamManager::parameterSnapshotAboutToChange,
            this, [&order]() { order.append(QStringLiteral("snapshot")); });
    connect(&manager, &QGCUASParamManager::parameterListReadyChanged,
            this, [&order](bool ready) {
        if (ready) {
            order.append(QStringLiteral("ready"));
        }
    });

    manager.requestParameterList();
    service.observeMessage(
        12, parameterValue(77, 1, QStringLiteral("P"), qint32(1),
                           ParameterType::Int32));
    QCOMPARE(order, QStringList({QStringLiteral("snapshot"),
                                 QStringLiteral("ready")}));
}

void ParameterServiceTest::compatibilityFacadePreservesReentrantNewTargetList()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint first = endpoint(3, 42, 1);
    const VehicleEndpoint second = endpoint(4, 43, 42);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    service.setEncoding(first, ParameterEncoding::Bytewise);
    service.setEncoding(second, ParameterEncoding::Bytewise);
    QGCUASParamManager manager(&service, &targets);
    QSignalSpy facadeCancellations(
        &manager, &QGCUASParamManager::parameterListLoadCanceled);

    QCOMPARE(service.requestCurrentParameterList(),
             int(ParameterService::SendResult::Sent));
    connect(&service, &ParameterService::listCancelled,
            this, [&service](qulonglong, int, int, int) {
        QCOMPARE(service.requestCurrentParameterList(),
                 int(ParameterService::SendResult::Sent));
    });
    QVERIFY(targets.selectTarget(4, 43, 42));
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.constLast().linkId, 4);
    QVERIFY(manager.parameterListInProgress());
    QCOMPARE(facadeCancellations.count(), 0);

    service.observeMessage(
        4, parameterValue(43, 42, QStringLiteral("NEW_TARGET"), qint32(9),
                          ParameterType::Int32));
    QVERIFY(!manager.parameterListInProgress());
    QVERIFY(manager.parameterListReady());
    QVariant value;
    QVERIFY(manager.getParameterValue(
        42, QStringLiteral("NEW_TARGET"), value));
    QCOMPARE(value.toInt(), 9);
}

void ParameterServiceTest::compatibilityFacadeDefersNewTargetRequestDuringDispatch()
{
    VehicleTargetManager targets;
    const VehicleEndpoint first = endpoint(40, 42, 1);
    const VehicleEndpoint second = endpoint(41, 43, 42);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));

    QVector<CapturedFrame> frames;
    QGCUASParamManager *facade = nullptr;
    bool switchedDuringSend = false;
    ExactLinkTransmitter transmitter(
        [&frames, &targets, &facade, &switchedDuringSend](
            int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            if (frames.size() == 1 && facade) {
                switchedDuringSend = targets.selectTarget(41, 43, 42);
                facade->requestParameterList();
            }
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setEncoding(first, ParameterEncoding::Bytewise);
    service.setEncoding(second, ParameterEncoding::Bytewise);
    QGCUASParamManager manager(&service, &targets);
    facade = &manager;

    manager.requestParameterList();
    QCoreApplication::processEvents();
    QVERIFY(switchedDuringSend);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0).linkId, 40);
    QCOMPARE(frames.at(1).linkId, 41);
    QVERIFY(manager.parameterListInProgress());

    service.observeMessage(
        41, parameterValue(43, 42, QStringLiteral("FINAL"), qint32(7),
                           ParameterType::Int32));
    QVERIFY(manager.parameterListReady());
    QCOMPARE(manager.getParameterValue(
                 42, QStringLiteral("FINAL")).toInt(), 7);
}

void ParameterServiceTest::compatibilityFacadeDefersReadDuringInvalidation()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint first = endpoint(50, 42, 1);
    const VehicleEndpoint second = endpoint(51, 43, 42);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    QGCUASParamManager manager(&service, &targets);
    connect(&manager, &QGCUASParamManager::parameterTargetChanged,
            this, [&manager, &targets]() {
        const VehicleTargetLease target = targets.acquireTarget();
        if (target.isValid() && target.endpoint.linkId == 51) {
            manager.requestParameterUpdate(
                42, QStringLiteral("DEFERRED_READ"));
        }
    });

    QVERIFY(targets.selectTarget(51, 43, 42));
    QCOMPARE(frames.size(), 0);
    QCoreApplication::processEvents();
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.constFirst().linkId, 51);
    const mavlink_message_t message = decodeFrame(frames.constFirst().bytes);
    QCOMPARE(message.msgid, quint32(MAVLINK_MSG_ID_PARAM_REQUEST_READ));
    mavlink_param_request_read_t read{};
    mavlink_msg_param_request_read_decode(&message, &read);
    QCOMPARE(parameterId(read.param_id), QByteArray("DEFERRED_READ"));
}

void ParameterServiceTest::compatibilityFacadeStopsReplayAfterNestedSwitch()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint first = endpoint(60, 42, 1);
    const VehicleEndpoint second = endpoint(61, 43, 42);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    service.setEncoding(first, ParameterEncoding::Bytewise);
    QGCUASParamManager manager(&service, &targets);
    QSignalSpy upToDate(
        &manager, &QGCUASParamManager::parameterListUpToDate);
    int namedReplays = 0;
    connect(&manager,
            static_cast<void (QGCUASParamManager::*)(
                int, QString, QVariant)>(
                &QGCUASParamManager::parameterChanged),
            this, [&namedReplays](int, const QString &, const QVariant &) {
        ++namedReplays;
    });
    connect(&manager, &QGCUASParamManager::parameterSnapshotAboutToChange,
            this, [&targets]() {
        const VehicleTargetLease target = targets.acquireTarget();
        if (target.isValid() && target.endpoint.linkId == 60) {
            QVERIFY(targets.selectTarget(61, 43, 42));
        }
    });

    manager.requestParameterList();
    service.observeMessage(
        60, parameterValue(42, 1, QStringLiteral("STALE"), qint32(1),
                           ParameterType::Int32));
    QCOMPARE(targets.acquireTarget().endpoint.linkId, 61);
    QCOMPARE(upToDate.count(), 0);
    QCOMPARE(namedReplays, 0);
}

void ParameterServiceTest::compatibilityFacadeCoalescesSameTurnFailures()
{
    VehicleTargetManager targets;
    int sends = 0;
    ExactLinkTransmitter transmitter(
        [&sends](int, const QByteArray &) {
            ++sends;
            return false;
        });
    ParameterService service(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(12, 77, 1), true));
    QGCUASParamManager manager(&service, &targets);

    manager.requestParameterList();
    manager.requestParameterList();
    QCOMPARE(sends, 1);
    QCoreApplication::processEvents();
    manager.requestParameterList();
    QCOMPARE(sends, 2);
}

void ParameterServiceTest::commandAndParameterTrafficShareOneLinkSequence()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    VehicleCommandService commands(&targets, &transmitter);
    ParameterService parameters(&targets, &transmitter);
    QVERIFY(targets.observeEndpoint(endpoint(5, 42, 1), true));
    const VehicleTargetLease lease = targets.acquireTarget();

    QCOMPARE(commands.sendCommandLong(
                 lease, 250, 190, MAV_CMD_NAV_RETURN_TO_LAUNCH, 0,
                 0, 0, 0, 0, 0, 0, 0),
             VehicleCommandService::SendResult::Sent);
    QCOMPARE(parameters.requestParameterList(lease, 250, 190),
             ParameterService::SendResult::Sent);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0).linkId, 5);
    QCOMPARE(frames.at(1).linkId, 5);

    const mavlink_message_t command = decodeFrame(frames.at(0).bytes);
    const mavlink_message_t parameter = decodeFrame(frames.at(1).bytes);
    QCOMPARE(command.msgid, quint32(MAVLINK_MSG_ID_COMMAND_LONG));
    QCOMPARE(parameter.msgid, quint32(MAVLINK_MSG_ID_PARAM_REQUEST_LIST));
    QCOMPARE(command.seq, quint8(0));
    QCOMPARE(parameter.seq, quint8(1));
}

void ParameterServiceTest::targetSwitchCancelsPendingTransactions()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const VehicleEndpoint first = endpoint(3, 42, 1);
    const VehicleEndpoint second = endpoint(4, 42, 1);
    QVERIFY(targets.observeEndpoint(first, true));
    QVERIFY(targets.observeEndpoint(second));
    const VehicleTargetLease oldTarget = targets.acquireTarget();
    QCOMPARE(service.requestParameterList(oldTarget, 250, 190),
             ParameterService::SendResult::Sent);
    QCOMPARE(service.requestParameterRead(
                 oldTarget, 250, 190, QStringLiteral("OLD_READ")),
             ParameterService::SendResult::Sent);
    QSignalSpy cancellations(
        &service, &ParameterService::transactionsCancelled);
    QSignalSpy completions(&service, &ParameterService::listCompleted);
    QSignalSpy reads(&service, &ParameterService::parameterRead);

    QVERIFY(targets.selectTarget(4, 42, 1));
    QCOMPARE(cancellations.count(), 1);
    QCOMPARE(cancellations.first().at(0).toULongLong(),
             oldTarget.generation);

    service.observeMessage(
        3, parameterValue(42, 1, QStringLiteral("OLD_READ"), qint32(10),
                          ParameterType::Int32));
    QCOMPARE(completions.count(), 0);
    QCOMPARE(reads.count(), 0);
    QVERIFY(!service.store()->snapshot(first)
                 .contains(1, QStringLiteral("OLD_READ")));

    const VehicleTargetLease newTarget = targets.acquireTarget();
    QCOMPARE(service.requestParameterList(newTarget, 250, 190),
             ParameterService::SendResult::Sent);
    service.observeMessage(
        4, parameterValue(42, 1, QStringLiteral("NEW"), qint32(20),
                          ParameterType::Int32));
    QCOMPARE(completions.count(), 1);
    QCOMPARE(completions.first().at(0).toULongLong(),
             newTarget.generation);
    QVERIFY(service.store()->snapshot(second)
                .contains(1, QStringLiteral("NEW")));
}

QTEST_GUILESS_MAIN(ParameterServiceTest)

#include "test_parameterservice.moc"
