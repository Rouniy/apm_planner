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

SwarmVehicleInstanceLease swarmLease(
    int linkId, int systemId, int componentId = 1,
    quint64 linkSessionEpoch = 1, quint64 instanceEpoch = 1)
{
    SwarmVehicleInstanceLease lease;
    lease.endpoint = endpoint(linkId, systemId, componentId);
    lease.linkSessionEpoch = linkSessionEpoch;
    lease.instanceEpoch = instanceEpoch;
    return lease;
}

bool containsLease(
    const QList<SwarmVehicleInstanceLease> &active,
    const SwarmVehicleInstanceLease &lease)
{
    return std::any_of(
        active.cbegin(), active.cend(),
        [&lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        });
}

ParameterService::ExactOperationReport exactReportAt(
    const QSignalSpy &spy, int index)
{
    return qvariant_cast<ParameterService::ExactOperationReport>(
        spy.at(index).at(0));
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
    void exactReadsDisambiguateEqualIdsOnDifferentLinks();
    void exactReentrantRepliesAndSameLeaseSkipAreSafe();
    void exactMismatchingWriteEchoIsDefiniteRejection();
    void exactTransportFailureClassifiesReadAndWriteSafely();
    void exactSigningRejectionBeforeWriterIsDefinite();
    void exactSigningFailureAfterWriteIsUncertain();
    void exactReadRetriesHaveDefiniteBoundedTimeout();
    void exactWriteRetriesStopAtAbsoluteDeadlineAndQuarantine();
    void exactCancellationStopsReadAndWriteRetries();
    void exactWriteDeadlineIsRecheckedAfterRouteCallback();
    void exactReservationValidatesRouteAndExcludesLegacyOperations();
    void exactValidatorCallbacksCanDeleteService();
    void legacyTrafficFenceCoversDirectAndCancelledReads();
    void exactOwnerDestructionDrainsAndRetirementIsUncertain();
    void exactForgetLinkDrainsAndRemovesCache();
    void exactTerminalSignalsCanDeleteService();
    void exactSynchronousWriteInterruptionIsUncertain_data();
    void exactSynchronousWriteInterruptionIsUncertain();
    void exactSignerCancellationBeforeWriterIsDefinite();
    void exactReentrantReplacementDoesNotInheritAttemptFlag();
    void exactValueNormalizationUsesEncodingWithoutAuthorization();
    void exactInFlightAttemptSurvivesServiceDeletion_data();
    void exactInFlightAttemptSurvivesServiceDeletion();
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

void ParameterServiceTest::exactReadsDisambiguateEqualIdsOnDifferentLinks()
{
    VehicleTargetManager targets;
    QVector<CapturedFrame> frames;
    ExactLinkTransmitter transmitter(
        [&frames](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const SwarmVehicleInstanceLease radio = swarmLease(31, 42, 1, 2, 3);
    const SwarmVehicleInstanceLease simulator = swarmLease(32, 42, 1, 4, 5);
    QList<SwarmVehicleInstanceLease> active{radio, simulator};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &lease) {
            return containsLease(active, lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));

    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);

    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("SYSID_THISMAV");
    QCOMPARE(service.submitExactRead(
                 reservation, radio, read),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().linkId, 31);

    service.observeMessage(
        32, parameterValue(42, 1, read.name, qint32(84),
                           ParameterType::Int32));
    QCOMPARE(finished.count(), 0);
    QVERIFY(!service.store()->snapshot(simulator.endpoint)
                 .contains(1, read.name));

    service.observeMessage(
        31, parameterValue(42, 1, read.name, qint32(42),
                           ParameterType::Int32));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(exactReportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::ReadSucceeded);
    QCOMPARE(exactReportAt(finished, 0).value.toInt(), 42);
    QCOMPARE(service.store()->snapshot(radio.endpoint)
                 .value(1, read.name).value.toInt(), 42);

    QCOMPARE(service.submitExactRead(
                 reservation, simulator, read),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.last().linkId, 32);
    service.observeMessage(
        32, parameterValue(42, 1, read.name, qint32(84),
                           ParameterType::Int32));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(exactReportAt(finished, 1).value.toInt(), 84);
    QVERIFY(service.releaseExactReservation(reservation));
}

void ParameterServiceTest::exactReentrantRepliesAndSameLeaseSkipAreSafe()
{
    VehicleTargetManager targets;
    ParameterService *servicePointer = nullptr;
    int transmissions = 0;
    ExactLinkTransmitter transmitter(
        [&servicePointer, &transmissions](
            int linkId, const QByteArray &bytes) {
            ++transmissions;
            const mavlink_message_t message = decodeFrame(bytes);
            QString name;
            QVariant value = qint32(5);
            ParameterType type = ParameterType::Int32;
            if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
                mavlink_param_request_read_t request{};
                mavlink_msg_param_request_read_decode(&message, &request);
                name = QString::fromLatin1(parameterId(request.param_id));
            } else if (message.msgid == MAVLINK_MSG_ID_PARAM_SET) {
                mavlink_param_set_t request{};
                mavlink_msg_param_set_decode(&message, &request);
                name = QString::fromLatin1(parameterId(request.param_id));
                type = static_cast<ParameterType>(request.param_type);
                bool decoded = false;
                value = ParameterCodec::decodeClassic(
                    request.param_value, type,
                    ParameterEncoding::Bytewise, &decoded);
                Q_ASSERT(decoded);
            }
            servicePointer->observeMessage(
                linkId, parameterValue(55, 1, name, value, type));
            return true;
        });
    ParameterService service(&targets, &transmitter);
    servicePointer = &service;
    const SwarmVehicleInstanceLease lease = swarmLease(33, 55, 1, 7, 9);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);

    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("EXACT_GAIN");
    ParameterService::ExactOperationToken readToken;
    QCOMPARE(service.submitExactRead(
                 reservation, lease, read, &readToken),
             ParameterService::ExactSubmitResult::Started);
    QVERIFY(readToken.isValid());
    QCOMPARE(finished.count(), 1);
    QCOMPARE(exactReportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::ReadSucceeded);
    QCOMPARE(transmissions, 1);

    ParameterService::ExactWriteRequest write;
    write.name = read.name;
    write.value = qint32(5);
    write.type = ParameterType::Int32;
    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(finished.count(), 2);
    QCOMPARE(exactReportAt(finished, 1).terminalResult,
             ParameterService::ExactTerminalResult::WriteSkipped);
    QCOMPARE(transmissions, 1);

    write.value = qint32(6);
    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(finished.count(), 3);
    QCOMPARE(exactReportAt(finished, 2).terminalResult,
             ParameterService::ExactTerminalResult::WriteSucceeded);
    QVERIFY(exactReportAt(finished, 2).frameAttempted);
    QCOMPARE(transmissions, 2);

    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(finished.count(), 4);
    const ParameterService::ExactOperationReport skipped =
        exactReportAt(finished, 3);
    QCOMPARE(skipped.terminalResult,
             ParameterService::ExactTerminalResult::WriteSkipped);
    QVERIFY(!skipped.frameAttempted);
    QCOMPARE(skipped.attempts, 0);
    QCOMPARE(transmissions, 2);
}

void ParameterServiceTest::exactMismatchingWriteEchoIsDefiniteRejection()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const SwarmVehicleInstanceLease lease = swarmLease(34, 56, 1, 2, 8);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("EXACT_MODE");
    write.value = qint32(9);
    write.type = ParameterType::Int32;

    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    service.observeMessage(
        34, parameterValue(56, 1, write.name, qint32(8), write.type));
    QCOMPARE(finished.count(), 1);
    const ParameterService::ExactOperationReport report =
        exactReportAt(finished, 0);
    QCOMPARE(report.terminalResult,
             ParameterService::ExactTerminalResult::Rejected);
    QCOMPARE(report.value.toInt(), 8);
    QVERIFY(report.frameAttempted);
    QVERIFY(!service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));
}

void ParameterServiceTest::
exactTransportFailureClassifiesReadAndWriteSafely()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return false; });
    ParameterService service(&targets, &transmitter);
    const SwarmVehicleInstanceLease lease = swarmLease(40, 62, 1, 8, 14);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);

    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("FAILED_READ");
    QCOMPARE(service.submitExactRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::TransportUnavailable);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(exactReportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::ReadTransportFailure);

    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("FAILED_WRITE");
    write.value = qint32(12);
    write.type = ParameterType::Int32;
    write.force = true;
    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::
                 TransportOutcomeUncertain);
    QCOMPARE(finished.count(), 2);
    const ParameterService::ExactOperationReport report =
        exactReportAt(finished, 1);
    QCOMPARE(report.terminalResult,
             ParameterService::ExactTerminalResult::
                 WriteTransportOutcomeUncertain);
    QVERIFY(report.frameAttempted);
    QVERIFY(service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));
}

void ParameterServiceTest::
exactSigningRejectionBeforeWriterIsDefinite()
{
    VehicleTargetManager targets;
    int writes = 0;
    ExactLinkTransmitter transmitter(
        [&writes](int, const QByteArray &) {
            ++writes;
            return true;
        });
    transmitter.setFrameSigner(
        [](int, const QByteArray &, QByteArray *) { return false; });
    ParameterService service(&targets, &transmitter);
    const SwarmVehicleInstanceLease lease = swarmLease(41, 63, 1, 9, 15);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("SIGNING_REQUIRED");
    write.value = qint32(12);
    write.type = ParameterType::Int32;
    write.force = true;

    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::TransportUnavailable);
    QCOMPARE(writes, 0);
    QCOMPARE(finished.count(), 1);
    const auto rejected = exactReportAt(finished, 0);
    QCOMPARE(rejected.terminalResult,
             ParameterService::ExactTerminalResult::Rejected);
    QVERIFY(!rejected.frameAttempted);
    QVERIFY(!service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));

    transmitter.setFrameSigner(ExactLinkTransmitter::FrameSigner());
    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(writes, 1);
    service.observeMessage(
        lease.endpoint.linkId,
        parameterValue(lease.endpoint.systemId,
                       lease.endpoint.componentId,
                       write.name, write.value, write.type));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(exactReportAt(finished, 1).terminalResult,
             ParameterService::ExactTerminalResult::WriteSucceeded);
}

void ParameterServiceTest::
exactSigningFailureAfterWriteIsUncertain()
{
    VehicleTargetManager targets;
    int writes = 0;
    ExactLinkTransmitter transmitter(
        [&writes](int, const QByteArray &) {
            ++writes;
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(10, 1, 10, 2, 200, 200);
    const SwarmVehicleInstanceLease lease = swarmLease(42, 64, 1, 10, 16);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("SIGNED_RETRY");
    write.value = qint32(13);
    write.type = ParameterType::Int32;
    write.force = true;

    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(writes, 1);
    transmitter.setFrameSigner(
        [](int, const QByteArray &, QByteArray *) { return false; });
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 250);
    QCOMPARE(writes, 1);
    const auto uncertain = exactReportAt(finished, 0);
    QCOMPARE(uncertain.terminalResult,
             ParameterService::ExactTerminalResult::
                 WriteTransportOutcomeUncertain);
    QVERIFY(uncertain.frameAttempted);
    QCOMPARE(uncertain.attempts, 2);
    QVERIFY(service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));
}

void ParameterServiceTest::exactReadRetriesHaveDefiniteBoundedTimeout()
{
    VehicleTargetManager targets;
    int transmissions = 0;
    ExactLinkTransmitter transmitter(
        [&transmissions](int, const QByteArray &) {
            ++transmissions;
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(15, 2, 20, 3, 200, 200);
    const SwarmVehicleInstanceLease lease = swarmLease(35, 57, 1, 3, 9);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    QSignalSpy retried(
        &service, &ParameterService::exactOperationRetried);
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("NO_REPLY");

    QCOMPARE(service.submitExactRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 250);
    QCOMPARE(transmissions, 3);
    QCOMPARE(retried.count(), 2);
    const ParameterService::ExactOperationReport report =
        exactReportAt(finished, 0);
    QCOMPARE(report.terminalResult,
             ParameterService::ExactTerminalResult::ReadTimedOut);
    QCOMPARE(report.attempts, 3);
    QVERIFY(report.frameAttempted);
    QVERIFY(!service.isExactWriteQuarantined(
        lease, read.name, qint32(1), ParameterType::Int32));
}

void ParameterServiceTest::
exactWriteRetriesStopAtAbsoluteDeadlineAndQuarantine()
{
    VehicleTargetManager targets;
    int transmissions = 0;
    ExactLinkTransmitter transmitter(
        [&transmissions](int, const QByteArray &) {
            ++transmissions;
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(20, 1, 15, 100, 80, 300);
    const SwarmVehicleInstanceLease lease = swarmLease(36, 58, 1, 4, 10);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    QSignalSpy released(
        &service, &ParameterService::exactReservationReleased);
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("ABSOLUTE_BOUND");
    write.value = qint32(77);
    write.type = ParameterType::Int32;
    write.force = true;
    QElapsedTimer elapsed;
    elapsed.start();

    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    QVERIFY(service.releaseExactReservation(reservation));
    QCOMPARE(released.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 300);
    QVERIFY(elapsed.elapsed() < 250);
    QVERIFY(transmissions >= 3);
    const ParameterService::ExactOperationReport report =
        exactReportAt(finished, 0);
    QCOMPARE(report.terminalResult,
             ParameterService::ExactTerminalResult::
                 WriteTimedOutOutcomeUncertain);
    QCOMPARE(report.attempts, transmissions);
    QVERIFY(report.frameAttempted);
    QVERIFY(report.description.contains(
        QStringLiteral("maximum lifetime"), Qt::CaseInsensitive));
    QCOMPARE(released.count(), 1);
    QVERIFY(service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));
    QVERIFY(service.isExactWriteQuarantined(
        lease, write.name, quint16(5), ParameterType::UInt16));

    // PARAM_VALUE does not identify the PARAM_SET it answers.  A vehicle may
    // clamp or reject the requested value, so quarantine must consume every
    // same-name echo regardless of value and type.
    service.observeMessage(
        36, parameterValue(58, 1, write.name, qint16(12),
                           ParameterType::Int16));
    service.observeMessage(
        36, parameterValue(58, 1, write.name, write.value, write.type));
    QVERIFY(!service.store()->snapshot(lease.endpoint)
                 .contains(1, write.name));

    QObject secondOwner;
    ParameterService::ExactReservationToken secondReservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &secondOwner, active, &secondReservation),
             ParameterService::ExactReservationResult::Reserved);
    service.setLocalIdentity(201, 77);
    ParameterService::ExactReadRequest quarantinedRead;
    quarantinedRead.name = write.name;
    QCOMPARE(service.submitExactRead(
                 secondReservation, lease, quarantinedRead),
             ParameterService::ExactSubmitResult::Quarantined);
    write.value = quint16(5);
    write.type = ParameterType::UInt16;
    QCOMPARE(service.submitExactWrite(
                 secondReservation, lease, write),
             ParameterService::ExactSubmitResult::Quarantined);
    QVERIFY(service.releaseExactReservation(secondReservation));
}

void ParameterServiceTest::exactCancellationStopsReadAndWriteRetries()
{
    VehicleTargetManager targets;
    int transmissions = 0;
    ExactLinkTransmitter transmitter(
        [&transmissions](int, const QByteArray &) {
            ++transmissions;
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(15, 20, 15, 20, 500, 250);
    const SwarmVehicleInstanceLease lease = swarmLease(52, 74, 1, 14, 21);
    const QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));

    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(&owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    QSignalSpy retried(
        &service, &ParameterService::exactOperationRetried);
    QSignalSpy released(
        &service, &ParameterService::exactReservationReleased);

    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("CANCEL_READ");
    ParameterService::ExactOperationToken readToken;
    QCOMPARE(service.submitExactRead(
                 reservation, lease, read, &readToken),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(transmissions, 1);
    ParameterService::ExactOperationToken wrongToken = readToken;
    ++wrongToken.operationId;
    QVERIFY(!service.cancelExactOperation(
        reservation, wrongToken, QStringLiteral("wrong token")));
    QCOMPARE(finished.count(), 0);
    QVERIFY(service.cancelExactOperation(
        reservation, readToken, QStringLiteral("operator stop")));
    QCOMPARE(finished.count(), 1);
    const ParameterService::ExactOperationReport readReport =
        exactReportAt(finished, 0);
    QCOMPARE(readReport.terminalResult,
             ParameterService::ExactTerminalResult::ReadCancelled);
    QCOMPARE(readReport.attempts, 1);
    QVERIFY(readReport.frameAttempted);
    QVERIFY(readReport.description.contains(
        QStringLiteral("operator stop")));
    QTest::qWait(60);
    QCOMPARE(transmissions, 1);
    QCOMPARE(retried.count(), 0);

    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("CANCEL_WRITE");
    write.value = qint32(23);
    write.type = ParameterType::Int32;
    write.force = true;
    ParameterService::ExactOperationToken writeToken;
    QCOMPARE(service.submitExactWrite(
                 reservation, lease, write, &writeToken),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(transmissions, 2);
    QVERIFY(service.cancelExactOperation(reservation, writeToken));
    QCOMPARE(finished.count(), 2);
    const ParameterService::ExactOperationReport writeReport =
        exactReportAt(finished, 1);
    QCOMPARE(writeReport.terminalResult,
             ParameterService::ExactTerminalResult::
                 WriteCancelledOutcomeUncertain);
    QCOMPARE(writeReport.attempts, 1);
    QVERIFY(writeReport.frameAttempted);
    QVERIFY(writeReport.description.contains(
        QStringLiteral("uncertain"), Qt::CaseInsensitive));
    QVERIFY(service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));
    QTest::qWait(60);
    QCOMPARE(transmissions, 2);
    QCOMPARE(retried.count(), 0);

    service.observeMessage(
        lease.endpoint.linkId,
        parameterValue(
            lease.endpoint.systemId, lease.endpoint.componentId,
            write.name, write.value, write.type));
    QVERIFY(!service.store()->snapshot(lease.endpoint)
                 .contains(lease.endpoint.componentId, write.name));

    QVERIFY(service.releaseExactReservation(reservation));
    QCOMPARE(released.count(), 1);
    QVERIFY(!service.cancelExactOperation(reservation, writeToken));
}

void ParameterServiceTest::
exactWriteDeadlineIsRecheckedAfterRouteCallback()
{
    VehicleTargetManager targets;
    int transmissions = 0;
    int routeCalls = 0;
    ExactLinkTransmitter transmitter(
        [&transmissions](int, const QByteArray &) {
            ++transmissions;
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(10, 1, 10, 20, 35, 100);
    const SwarmVehicleInstanceLease lease = swarmLease(46, 68, 1, 9, 15);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [&routeCalls](const SwarmVehicleInstanceLease &, QString *) {
            ++routeCalls;
            // reserve, initial submit, then the first retry.  Cross the hard
            // lifetime inside application policy after the timer's early check.
            if (routeCalls == 3) {
                QTest::qWait(50);
            }
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(&owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("SLOW_ROUTE");
    write.value = qint32(17);
    write.type = ParameterType::Int32;
    write.force = true;

    QCOMPARE(service.submitExactWrite(reservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 200);
    QCOMPARE(transmissions, 1);
    QCOMPARE(exactReportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::
                 WriteTimedOutOutcomeUncertain);
    QVERIFY(service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));
}

void ParameterServiceTest::
exactReservationValidatesRouteAndExcludesLegacyOperations()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(20, 1, 20, 1, 100, 25);
    const SwarmVehicleInstanceLease lease = swarmLease(37, 59, 1, 5, 11);
    const SwarmVehicleInstanceLease stale = swarmLease(37, 59, 1, 5, 12);
    QList<SwarmVehicleInstanceLease> active{lease};
    bool routeAvailable = false;
    QObject owner;
    ParameterService::ExactReservationToken reservation;

    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::ContextUnavailable);
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [&routeAvailable](const SwarmVehicleInstanceLease &, QString *) {
            return routeAvailable;
        }));
    QCOMPARE(service.reserveExactEndpoints(
                 nullptr, active, &reservation),
             ParameterService::ExactReservationResult::InvalidOwner);
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, QList<SwarmVehicleInstanceLease>(), &reservation),
             ParameterService::ExactReservationResult::InvalidLease);
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, QList<SwarmVehicleInstanceLease>{stale},
                 &reservation),
             ParameterService::ExactReservationResult::StaleLease);
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::RouteUnavailable);

    routeAvailable = true;
    QVERIFY(targets.observeEndpoint(lease.endpoint, true));
    const VehicleTargetLease selected = targets.acquireTarget();
    QCOMPARE(service.requestParameterRead(
                 selected, 250, 190, QStringLiteral("LEGACY_PENDING")),
             ParameterService::SendResult::Sent);
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Busy);
    service.observeMessage(
        37, parameterValue(59, 1, QStringLiteral("LEGACY_PENDING"),
                           qint32(1), ParameterType::Int32));

    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Busy);
    QTest::qWait(35);
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QCOMPARE(service.requestParameterList(selected, 250, 190),
             ParameterService::SendResult::Busy);
    QCOMPARE(service.requestParameterRead(
                 selected, 250, 190, QStringLiteral("BLOCKED")),
             ParameterService::SendResult::Busy);
    QCOMPARE(service.requestParameterReadByIndex(
                 selected, 250, 190, 3),
             ParameterService::SendResult::Busy);
    QCOMPARE(service.setParameter(
                 selected, 250, 190, QStringLiteral("BLOCKED"), qint32(2),
                 ParameterType::Int32),
             ParameterService::SendResult::Busy);
    QCOMPARE(service.writeCurrentParameter(
                 QStringLiteral("LEGACY_PENDING"), qint32(2)),
             qulonglong(0));
    QCOMPARE(service.writeCurrentParameters(
                 QVariantList{QVariantMap{
                     {QStringLiteral("name"),
                      QStringLiteral("LEGACY_PENDING")},
                     {QStringLiteral("value"), qint32(2)}}}),
             qulonglong(0));

    QObject otherOwner;
    ParameterService::ExactReservationToken otherReservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &otherOwner, active, &otherReservation),
             ParameterService::ExactReservationResult::Busy);

    routeAvailable = false;
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("EXACT_BLOCKED");
    QCOMPARE(service.submitExactRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::RouteUnavailable);
    QVERIFY(service.releaseExactReservation(reservation));
    QCOMPARE(service.requestParameterRead(
                 selected, 250, 190, QStringLiteral("UNBLOCKED")),
             ParameterService::SendResult::Sent);
}

void ParameterServiceTest::exactValidatorCallbacksCanDeleteService()
{
    const SwarmVehicleInstanceLease lease = swarmLease(47, 69, 1, 10, 16);
    const QList<SwarmVehicleInstanceLease> active{lease};

    // Policy validation is a reentrancy boundary.  A recursive operation must
    // not slip through before the outer waiter is published.
    {
        VehicleTargetManager targets;
        ExactLinkTransmitter transmitter(
            [](int, const QByteArray &) { return true; });
        ParameterService service(&targets, &transmitter);
        QObject owner;
        ParameterService::ExactReservationToken reservation;
        ParameterService::ExactReadRequest read;
        read.name = QStringLiteral("REENTRANT_POLICY");
        bool recurse = false;
        ParameterService::ExactSubmitResult nestedResult =
            ParameterService::ExactSubmitResult::Started;
        QVERIFY(service.configureExactTransactions(
            [&active](const SwarmVehicleInstanceLease &candidate) {
                return containsLease(active, candidate);
            },
            [&service, &reservation, &lease, &read, &recurse, &nestedResult](
                const SwarmVehicleInstanceLease &, QString *) {
                if (recurse) {
                    recurse = false;
                    nestedResult = service.submitExactRead(
                        reservation, lease, read);
                }
                return true;
            }));
        QCOMPARE(service.reserveExactEndpoints(&owner, active, &reservation),
                 ParameterService::ExactReservationResult::Reserved);
        recurse = true;
        QCOMPARE(service.submitExactRead(reservation, lease, read),
                 ParameterService::ExactSubmitResult::Started);
        QCOMPARE(nestedResult, ParameterService::ExactSubmitResult::Busy);
        service.observeMessage(
            lease.endpoint.linkId,
            parameterValue(lease.endpoint.systemId,
                           lease.endpoint.componentId,
                           read.name, qint32(1), ParameterType::Int32));
    }

    // A route validator may destroy the service while reservation policy is
    // running.  The validator callable itself must remain alive until return.
    {
        VehicleTargetManager targets;
        ExactLinkTransmitter transmitter(
            [](int, const QByteArray &) { return true; });
        ParameterService *service = new ParameterService(&targets, &transmitter);
        QPointer<ParameterService> guarded(service);
        QVERIFY(service->configureExactTransactions(
            [&active](const SwarmVehicleInstanceLease &candidate) {
                return containsLease(active, candidate);
            },
            [&service](const SwarmVehicleInstanceLease &, QString *) {
                ParameterService *victim = service;
                service = nullptr;
                delete victim;
                return true;
            }));
        QObject owner;
        ParameterService::ExactReservationToken reservation;
        QCOMPARE(service->reserveExactEndpoints(&owner, active, &reservation),
                 ParameterService::ExactReservationResult::ContextUnavailable);
        QVERIFY(guarded.isNull());
    }

    // Submission has an independent pre-route lease callback boundary.
    {
        VehicleTargetManager targets;
        ExactLinkTransmitter transmitter(
            [](int, const QByteArray &) { return true; });
        ParameterService *service = new ParameterService(&targets, &transmitter);
        QPointer<ParameterService> guarded(service);
        bool destroyOnLease = false;
        QVERIFY(service->configureExactTransactions(
            [&active, &destroyOnLease, &service](
                const SwarmVehicleInstanceLease &candidate) {
                if (destroyOnLease) {
                    ParameterService *victim = service;
                    service = nullptr;
                    delete victim;
                }
                return containsLease(active, candidate);
            },
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
        QObject owner;
        ParameterService::ExactReservationToken reservation;
        QCOMPARE(service->reserveExactEndpoints(&owner, active, &reservation),
                 ParameterService::ExactReservationResult::Reserved);
        destroyOnLease = true;
        ParameterService::ExactReadRequest read;
        read.name = QStringLiteral("DELETE_ON_SUBMIT");
        QCOMPARE(service->submitExactRead(reservation, lease, read),
                 ParameterService::ExactSubmitResult::ContextUnavailable);
        QVERIFY(guarded.isNull());
    }

    // Retry route policy can delete the service from the timer callback.
    {
        VehicleTargetManager targets;
        ExactLinkTransmitter transmitter(
            [](int, const QByteArray &) { return true; });
        ParameterService *service = new ParameterService(&targets, &transmitter);
        QPointer<ParameterService> guarded(service);
        int routeCalls = 0;
        service->setExactRetryPolicyForTesting(10, 2, 10, 2, 100, 100);
        QVERIFY(service->configureExactTransactions(
            [&active](const SwarmVehicleInstanceLease &candidate) {
                return containsLease(active, candidate);
            },
            [&routeCalls, &service](
                const SwarmVehicleInstanceLease &, QString *) {
                ++routeCalls;
                if (routeCalls == 3) {
                    ParameterService *victim = service;
                    service = nullptr;
                    delete victim;
                }
                return true;
            }));
        QObject owner;
        ParameterService::ExactReservationToken reservation;
        QCOMPARE(service->reserveExactEndpoints(&owner, active, &reservation),
                 ParameterService::ExactReservationResult::Reserved);
        ParameterService::ExactReadRequest read;
        read.name = QStringLiteral("DELETE_ON_RETRY");
        QCOMPARE(service->submitExactRead(reservation, lease, read),
                 ParameterService::ExactSubmitResult::Started);
        QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 150);
    }

    // Inbound observation revalidates the lease before touching the store.
    {
        VehicleTargetManager targets;
        ExactLinkTransmitter transmitter(
            [](int, const QByteArray &) { return true; });
        ParameterService *service = new ParameterService(&targets, &transmitter);
        QPointer<ParameterService> guarded(service);
        bool destroyOnLease = false;
        QVERIFY(service->configureExactTransactions(
            [&active, &destroyOnLease, &service](
                const SwarmVehicleInstanceLease &candidate) {
                if (destroyOnLease) {
                    ParameterService *victim = service;
                    service = nullptr;
                    delete victim;
                }
                return containsLease(active, candidate);
            },
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
        QObject owner;
        ParameterService::ExactReservationToken reservation;
        QCOMPARE(service->reserveExactEndpoints(&owner, active, &reservation),
                 ParameterService::ExactReservationResult::Reserved);
        ParameterService::ExactReadRequest read;
        read.name = QStringLiteral("DELETE_OBSERVE");
        QCOMPARE(service->submitExactRead(reservation, lease, read),
                 ParameterService::ExactSubmitResult::Started);
        destroyOnLease = true;
        service->observeMessage(
            lease.endpoint.linkId,
            parameterValue(lease.endpoint.systemId,
                           lease.endpoint.componentId,
                           read.name, qint32(1), ParameterType::Int32));
        QVERIFY(guarded.isNull());
    }
}

void ParameterServiceTest::
legacyTrafficFenceCoversDirectAndCancelledReads()
{
    VehicleTargetManager targets;
    int transmissions = 0;
    ExactLinkTransmitter transmitter(
        [&transmissions](int, const QByteArray &) {
            ++transmissions;
            return true;
        });
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(10, 1, 10, 1, 100, 100);
    const SwarmVehicleInstanceLease lease = swarmLease(48, 70, 1, 11, 17);
    const QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QVERIFY(targets.observeEndpoint(lease.endpoint, true));
    const VehicleTargetLease selected = targets.acquireTarget();
    QObject owner;
    ParameterService::ExactReservationToken reservation;

    QCOMPARE(service.reserveExactEndpoints(&owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    ParameterService::ExactReadRequest seedRead;
    seedRead.name = QStringLiteral("INDEXED");
    QCOMPARE(service.submitExactRead(reservation, lease, seedRead),
             ParameterService::ExactSubmitResult::Started);
    service.observeMessage(
        lease.endpoint.linkId,
        parameterValue(lease.endpoint.systemId, lease.endpoint.componentId,
                       seedRead.name, qint32(3), ParameterType::Int32));
    QVERIFY(service.releaseExactReservation(reservation));
    QCOMPARE(transmissions, 1);

    // Direct index reads have no name waiter, but their wire response is still
    // ambiguous.  An accepted response refreshes the same bounded fence.
    QCOMPARE(service.requestParameterReadByIndex(selected, 250, 190, 0),
             ParameterService::SendResult::Sent);
    QCOMPARE(service.reserveExactEndpoints(&owner, active, &reservation),
             ParameterService::ExactReservationResult::Busy);
    QTest::qWait(20);
    service.observeMessage(
        lease.endpoint.linkId,
        parameterValue(lease.endpoint.systemId, lease.endpoint.componentId,
                       QStringLiteral("INDEXED"), qint32(3),
                       ParameterType::Int32));
    QTest::qWait(20);
    QCOMPARE(service.reserveExactEndpoints(&owner, active, &reservation),
             ParameterService::ExactReservationResult::Busy);
    QTest::qWait(90);
    QCOMPARE(service.reserveExactEndpoints(&owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    ParameterService::ExactWriteRequest verifyInvalidatedCache;
    verifyInvalidatedCache.name = seedRead.name;
    verifyInvalidatedCache.value = qint32(3);
    verifyInvalidatedCache.type = ParameterType::Int32;
    QCOMPARE(service.submitExactWrite(
                 reservation, lease, verifyInvalidatedCache),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(transmissions, 3);
    service.observeMessage(
        lease.endpoint.linkId,
        parameterValue(lease.endpoint.systemId, lease.endpoint.componentId,
                       verifyInvalidatedCache.name,
                       verifyInvalidatedCache.value,
                       verifyInvalidatedCache.type));
    QVERIFY(service.releaseExactReservation(reservation));

    // Cancelling the legacy name waiter on target switch must not erase the
    // physical-traffic ambiguity window.
    QCOMPARE(service.requestParameterRead(
                 selected, 250, 190, QStringLiteral("CANCELLED_READ")),
             ParameterService::SendResult::Sent);
    const VehicleEndpoint other = endpoint(49, 71, 1);
    QVERIFY(targets.observeEndpoint(other));
    QVERIFY(targets.selectTarget(other.linkId, other.systemId,
                                 other.componentId));
    QCOMPARE(service.reserveExactEndpoints(&owner, active, &reservation),
             ParameterService::ExactReservationResult::Busy);
    QTest::qWait(110);
    QCOMPARE(service.reserveExactEndpoints(&owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QVERIFY(service.releaseExactReservation(reservation));
}

void ParameterServiceTest::
exactOwnerDestructionDrainsAndRetirementIsUncertain()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const SwarmVehicleInstanceLease lease = swarmLease(38, 60, 1, 6, 12);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    QSignalSpy released(
        &service, &ParameterService::exactReservationReleased);

    auto *owner = new QObject;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("OWNER_DRAIN");
    QCOMPARE(service.submitExactRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::Started);
    delete owner;
    QCOMPARE(released.count(), 0);
    service.observeMessage(
        38, parameterValue(60, 1, read.name, qint32(3),
                           ParameterType::Int32));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(exactReportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::ReadSucceeded);
    QVERIFY(exactReportAt(finished, 0).ownerDetached);
    QCOMPARE(released.count(), 1);
    QVERIFY(service.store()->snapshot(lease.endpoint)
                .contains(1, read.name));

    QObject writeOwner;
    ParameterService::ExactReservationToken writeReservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &writeOwner, active, &writeReservation),
             ParameterService::ExactReservationResult::Reserved);
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("RETIRE_WRITE");
    write.value = qint32(4);
    write.type = ParameterType::Int32;
    write.force = true;
    QCOMPARE(service.submitExactWrite(
                 writeReservation, lease, write),
             ParameterService::ExactSubmitResult::Started);
    active.clear();
    service.retireExactVehicle(lease);
    QCOMPARE(finished.count(), 2);
    QCOMPARE(exactReportAt(finished, 1).terminalResult,
             ParameterService::ExactTerminalResult::
                 WriteLeaseRetiredOutcomeUncertain);
    QVERIFY(exactReportAt(finished, 1).frameAttempted);
    QCOMPARE(released.count(), 2);
    QVERIFY(!service.store()->hasSnapshot(lease.endpoint));
    QVERIFY(service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));
}

void ParameterServiceTest::exactForgetLinkDrainsAndRemovesCache()
{
    VehicleTargetManager targets;
    ExactLinkTransmitter transmitter(
        [](int, const QByteArray &) { return true; });
    ParameterService service(&targets, &transmitter);
    const SwarmVehicleInstanceLease lease = swarmLease(39, 61, 1, 7, 13);
    QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(
                 &owner, active, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    QSignalSpy finished(
        &service, &ParameterService::exactOperationFinished);
    QSignalSpy released(
        &service, &ParameterService::exactReservationReleased);
    ParameterService::ExactReadRequest read;
    read.name = QStringLiteral("LINK_CACHE");
    QCOMPARE(service.submitExactRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::Started);
    service.observeMessage(
        39, parameterValue(61, 1, read.name, qint32(5),
                           ParameterType::Int32));
    QVERIFY(service.store()->hasSnapshot(lease.endpoint));

    read.name = QStringLiteral("LINK_PENDING");
    QCOMPARE(service.submitExactRead(reservation, lease, read),
             ParameterService::ExactSubmitResult::Started);
    service.forgetLink(39);
    QCOMPARE(finished.count(), 2);
    QCOMPARE(exactReportAt(finished, 1).terminalResult,
             ParameterService::ExactTerminalResult::ReadLinkForgotten);
    QCOMPARE(released.count(), 1);
    QVERIFY(!service.store()->hasSnapshot(lease.endpoint));
}

void ParameterServiceTest::exactTerminalSignalsCanDeleteService()
{
    // finishExactOperation() emits before retireExactVehicle() has completed;
    // the outer terminal path must not touch a deleted service afterwards.
    {
        VehicleTargetManager targets;
        ExactLinkTransmitter transmitter(
            [](int, const QByteArray &) { return true; });
        ParameterService *service = new ParameterService(&targets, &transmitter);
        QPointer<ParameterService> guarded(service);
        const SwarmVehicleInstanceLease lease = swarmLease(50, 72, 1, 12, 18);
        const QList<SwarmVehicleInstanceLease> active{lease};
        QVERIFY(service->configureExactTransactions(
            [&active](const SwarmVehicleInstanceLease &candidate) {
                return containsLease(active, candidate);
            },
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
        QObject owner;
        ParameterService::ExactReservationToken reservation;
        QCOMPARE(service->reserveExactEndpoints(&owner, active, &reservation),
                 ParameterService::ExactReservationResult::Reserved);
        ParameterService::ExactWriteRequest write;
        write.name = QStringLiteral("DELETE_ON_FINISH");
        write.value = qint32(1);
        write.type = ParameterType::Int32;
        write.force = true;
        QCOMPARE(service->submitExactWrite(reservation, lease, write),
                 ParameterService::ExactSubmitResult::Started);
        connect(service, &ParameterService::exactOperationFinished,
                [&service](const ParameterService::ExactOperationReport &) {
                    ParameterService *victim = service;
                    service = nullptr;
                    delete victim;
                });
        service->retireExactVehicle(lease);
        QVERIFY(guarded.isNull());
    }

    // Multiple closing reservations exercise the release loop: deletion from
    // the first release signal must prevent access to the second iterator.
    {
        VehicleTargetManager targets;
        ExactLinkTransmitter transmitter(
            [](int, const QByteArray &) { return true; });
        ParameterService *service = new ParameterService(&targets, &transmitter);
        QPointer<ParameterService> guarded(service);
        const SwarmVehicleInstanceLease first = swarmLease(51, 73, 1, 13, 19);
        const SwarmVehicleInstanceLease second = swarmLease(51, 74, 1, 13, 20);
        const QList<SwarmVehicleInstanceLease> active{first, second};
        QVERIFY(service->configureExactTransactions(
            [&active](const SwarmVehicleInstanceLease &candidate) {
                return containsLease(active, candidate);
            },
            [](const SwarmVehicleInstanceLease &, QString *) {
                return true;
            }));
        QObject firstOwner;
        QObject secondOwner;
        ParameterService::ExactReservationToken firstReservation;
        ParameterService::ExactReservationToken secondReservation;
        QCOMPARE(service->reserveExactEndpoints(
                     &firstOwner,
                     QList<SwarmVehicleInstanceLease>{first},
                     &firstReservation),
                 ParameterService::ExactReservationResult::Reserved);
        QCOMPARE(service->reserveExactEndpoints(
                     &secondOwner,
                     QList<SwarmVehicleInstanceLease>{second},
                     &secondReservation),
                 ParameterService::ExactReservationResult::Reserved);
        connect(service, &ParameterService::exactReservationReleased,
                [&service](qulonglong) {
                    ParameterService *victim = service;
                    service = nullptr;
                    delete victim;
                });
        service->forgetLink(first.endpoint.linkId);
        QVERIFY(guarded.isNull());
    }
}

void ParameterServiceTest::exactSynchronousWriteInterruptionIsUncertain_data()
{
    QTest::addColumn<bool>("fromSubmitted");
    QTest::addColumn<int>("interruption");
    for (int kind = 0; kind < 4; ++kind) {
        const QByteArray name = kind == 0 ? "cancel" : kind == 1 ? "retire"
            : kind == 2 ? "forget" : "target-change";
        QTest::newRow((name + "-writer").constData()) << false << kind;
        QTest::newRow((name + "-submitted").constData()) << true << kind;
    }
}

void ParameterServiceTest::exactSynchronousWriteInterruptionIsUncertain()
{
    QFETCH(bool, fromSubmitted);
    QFETCH(int, interruption);
    VehicleTargetManager targets;
    int writes = 0;
    int submissions = 0;
    std::function<void()> interrupt;
    ExactLinkTransmitter transmitter(
        [&](int, const QByteArray &bytes) {
            ++writes;
            if (decodeFrame(bytes).msgid != MAVLINK_MSG_ID_PARAM_SET)
                return false;
            if (!fromSubmitted && interrupt) interrupt();
            return true;
        });
    const auto lease = swarmLease(61, 91);
    transmitter.setLinkSessionEpoch(61, 1);
    ParameterService service(&targets, &transmitter);
    service.setExactRetryPolicyForTesting(10, 2, 10, 2, 500, 2000);
    const QList<SwarmVehicleInstanceLease> active{lease};
    QVERIFY(service.configureExactTransactions(
        [&active](const SwarmVehicleInstanceLease &candidate) {
            return containsLease(active, candidate);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QVERIFY(service.configureSingleVehicleExactRoute(
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QVERIFY(targets.observeEndpoint(lease.endpoint, true));
    const auto alternate = endpoint(62, 92);
    QVERIFY(targets.observeEndpoint(alternate));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveSingleVehicleEndpoint(
                 &owner, targets.acquireTarget(), lease, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("DEVICE_ID");
    write.type = ParameterType::Int32;
    write.value = qint32(0);
    write.force = true;
    ParameterService::ExactOperationToken token;
    QSignalSpy finished(&service, &ParameterService::exactOperationFinished);
    QSignalSpy retried(&service, &ParameterService::exactOperationRetried);
    bool interrupted = false;
    interrupt = [&]() {
        if (interrupted) return;
        interrupted = true;
        QVERIFY(token.isValid()); // Published before either callback boundary.
        QCOMPARE(writes, 1);
        if (interruption == 0) {
            QVERIFY(service.cancelExactOperation(reservation, token));
        } else if (interruption == 1) {
            service.retireExactVehicle(lease);
        } else if (interruption == 2) {
            service.forgetLink(61);
        } else {
            QVERIFY(targets.selectTarget(alternate.linkId, alternate.systemId,
                                         alternate.componentId));
        }
        QCOMPARE(finished.count(), 1);
        QVERIFY(exactReportAt(finished, 0).frameAttempted);
    };
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted, this,
            [&](int, qulonglong, mavlink_message_t message) {
        if (message.msgid != MAVLINK_MSG_ID_PARAM_SET) return;
        ++submissions;
        if (fromSubmitted) interrupt();
    });
    QCOMPARE(service.submitExactWrite(reservation, lease, write, &token),
             ParameterService::ExactSubmitResult::Started);
    QVERIFY(interrupted);
    QCOMPARE(writes, 1);
    QCOMPARE(submissions, 1);
    QCOMPARE(finished.count(), 1);
    const auto report = exactReportAt(finished, 0);
    const auto expected = interruption == 0
        ? ParameterService::ExactTerminalResult::WriteCancelledOutcomeUncertain
        : interruption == 2
            ? ParameterService::ExactTerminalResult::WriteLinkForgottenOutcomeUncertain
            : ParameterService::ExactTerminalResult::WriteLeaseRetiredOutcomeUncertain;
    QCOMPARE(report.terminalResult, expected);
    QCOMPARE(report.token.operationId, token.operationId);
    QCOMPARE(report.attempts, 1);
    QVERIFY(report.frameAttempted);
    QVERIFY(service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));
    QTest::qWait(35);
    QCOMPARE(writes, 1);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(retried.count(), 0);
}

void ParameterServiceTest::exactSignerCancellationBeforeWriterIsDefinite()
{
    VehicleTargetManager targets;
    int writes = 0;
    ExactLinkTransmitter transmitter(
        [&](int, const QByteArray &) { ++writes; return true; });
    ParameterService service(&targets, &transmitter);
    const auto lease = swarmLease(63, 93);
    QVERIFY(service.configureExactTransactions(
        [lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(&owner, {lease}, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    ParameterService::ExactOperationToken token;
    bool cancelled = false;
    transmitter.setFrameSigner(
        [&](int, const QByteArray &, QByteArray *) {
            cancelled = service.cancelExactOperation(reservation, token);
            return false;
        });
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("NO_WRITE");
    write.value = qint32(12);
    write.type = ParameterType::Int32;
    write.force = true;
    QSignalSpy finished(&service, &ParameterService::exactOperationFinished);
    QCOMPARE(service.submitExactWrite(reservation, lease, write, &token),
             ParameterService::ExactSubmitResult::Started);
    QVERIFY(cancelled);
    QCOMPARE(writes, 0);
    QCOMPARE(finished.count(), 1);
    const auto report = exactReportAt(finished, 0);
    QCOMPARE(report.terminalResult,
             ParameterService::ExactTerminalResult::WriteCancelled);
    QVERIFY(!report.frameAttempted);
    QVERIFY(!service.isExactWriteQuarantined(
        lease, write.name, write.value, write.type));
}

void ParameterServiceTest::exactReentrantReplacementDoesNotInheritAttemptFlag()
{
    VehicleTargetManager targets;
    int writes = 0;
    std::function<void()> cancelFirst;
    ExactLinkTransmitter transmitter(
        [&](int, const QByteArray &) {
            ++writes;
            if (cancelFirst) cancelFirst();
            return true;
        });
    ParameterService service(&targets, &transmitter);
    const auto lease = swarmLease(64, 94);
    QVERIFY(service.configureExactTransactions(
        [lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service.reserveExactEndpoints(&owner, {lease}, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    ParameterService::ExactWriteRequest first;
    first.name = QStringLiteral("FIRST");
    first.value = qint32(12);
    first.type = ParameterType::Int32;
    first.force = true;
    auto second = first;
    second.name = QStringLiteral("SECOND");
    ParameterService::ExactOperationToken firstToken;
    ParameterService::ExactOperationToken secondToken;
    QSignalSpy finished(&service, &ParameterService::exactOperationFinished);
    cancelFirst = [&]() {
        QVERIFY(service.cancelExactOperation(reservation, firstToken));
    };
    connect(&service, &ParameterService::exactOperationFinished, this,
            [&](const ParameterService::ExactOperationReport &report) {
        if (report.token.name != first.name) return;
        transmitter.setFrameSigner(
            [](int, const QByteArray &, QByteArray *) { return false; });
        QCOMPARE(service.submitExactWrite(reservation, lease, second, &secondToken),
                 ParameterService::ExactSubmitResult::TransportUnavailable);
    });
    QCOMPARE(service.submitExactWrite(reservation, lease, first, &firstToken),
             ParameterService::ExactSubmitResult::Started);
    QCOMPARE(writes, 1);
    QCOMPARE(finished.count(), 2);
    QVERIFY(firstToken.operationId != secondToken.operationId);
    QCOMPARE(exactReportAt(finished, 0).terminalResult,
             ParameterService::ExactTerminalResult::WriteCancelledOutcomeUncertain);
    QVERIFY(exactReportAt(finished, 0).frameAttempted);
    QCOMPARE(exactReportAt(finished, 1).terminalResult,
             ParameterService::ExactTerminalResult::Rejected);
    QVERIFY(!exactReportAt(finished, 1).frameAttempted);
    QVERIFY(service.isExactWriteQuarantined(lease, first.name, first.value, first.type));
    QVERIFY(!service.isExactWriteQuarantined(lease, second.name, second.value, second.type));
}

void ParameterServiceTest::exactValueNormalizationUsesEncodingWithoutAuthorization()
{
    VehicleTargetManager targets;
    int writes = 0;
    int callbacks = 0;
    ExactLinkTransmitter transmitter(
        [&](int, const QByteArray &) { ++writes; return true; });
    ParameterService service(&targets, &transmitter);
    const auto lease = swarmLease(65, 95);
    QVERIFY(service.configureExactTransactions(
        [&](const SwarmVehicleInstanceLease &) { ++callbacks; return false; },
        [&](const SwarmVehicleInstanceLease &, QString *) {
            ++callbacks;
            return false;
        }));
    QVariant normalized = 123;
    QString error = QStringLiteral("old");
    service.setEncoding(lease.endpoint, ParameterEncoding::CStyleCast);
    QVERIFY(!service.normalizeExactValue(
        lease, quint32(16777217), ParameterType::UInt32, &normalized, &error));
    QVERIFY(!normalized.isValid());
    QVERIFY(!error.isEmpty());
    QVERIFY(service.normalizeExactValue(
        lease, quint32(16777216), ParameterType::UInt32, &normalized, &error));
    QCOMPARE(normalized.toUInt(), quint32(16777216));
    QVERIFY(error.isEmpty());
    service.setEncoding(lease.endpoint, ParameterEncoding::Bytewise);
    QVERIFY(service.normalizeExactValue(
        lease, quint32(16777217), ParameterType::UInt32, &normalized, &error));
    QCOMPARE(normalized.toUInt(), quint32(16777217));
    QVERIFY(service.normalizeExactValue(
        lease, quint32(0xffffffffU), ParameterType::UInt32, &normalized, &error));
    QCOMPARE(normalized.toUInt(), quint32(0xffffffffU));
    QVERIFY(!service.normalizeExactValue(
        {}, quint32(1), ParameterType::UInt32, &normalized, &error));
    QVERIFY(!normalized.isValid());
    QVERIFY(!error.isEmpty());
    QVERIFY(!service.normalizeExactValue(
        lease, 1.0, ParameterType::Real64, &normalized, &error));
    QVERIFY(!normalized.isValid());
    // A syntactically valid but unauthorized/stale lease is deliberately
    // convertible. This helper cannot reserve, validate or send anything.
    QCOMPARE(callbacks, 0);
    QCOMPARE(writes, 0);
    QVERIFY(!targets.acquireTarget().isValid());
}

void ParameterServiceTest::exactInFlightAttemptSurvivesServiceDeletion_data()
{
    QTest::addColumn<int>("boundary");
    QTest::newRow("signer-before-write") << 0;
    QTest::newRow("writer") << 1;
    QTest::newRow("submitted-after-write") << 2;
}

void ParameterServiceTest::exactInFlightAttemptSurvivesServiceDeletion()
{
    QFETCH(int, boundary);
    VehicleTargetManager targets;
    QPointer<ParameterService> service;
    int writes = 0;
    ExactLinkTransmitter transmitter(
        [&](int, const QByteArray &) {
            ++writes;
            if (boundary == 1) delete service.data();
            return true;
        });
    const auto lease = swarmLease(66, 96);
    transmitter.setLinkSessionEpoch(66, 1);
    service = new ParameterService(&targets, &transmitter, &targets);
    QVERIFY(service->configureExactTransactions(
        [lease](const SwarmVehicleInstanceLease &candidate) {
            return candidate.sameInstance(lease);
        },
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }));
    if (boundary == 0) {
        transmitter.setFrameSigner(
            [&](int, const QByteArray &, QByteArray *) {
                delete service.data();
                return false;
            });
    }
    connect(&transmitter, &ExactLinkTransmitter::messageSubmitted, this,
            [&](int, qulonglong, mavlink_message_t) {
        if (boundary == 2) delete service.data();
    });
    QObject owner;
    ParameterService::ExactReservationToken reservation;
    QCOMPARE(service->reserveExactEndpoints(&owner, {lease}, &reservation),
             ParameterService::ExactReservationResult::Reserved);
    ParameterService::ExactWriteRequest write;
    write.name = QStringLiteral("LIFETIME");
    write.type = ParameterType::Int32;
    write.value = qint32(1);
    write.force = true;
    const auto submitted = service->submitExactWrite(reservation, lease, write);
    QVERIFY(service.isNull());
    QCOMPARE(writes, boundary == 0 ? 0 : 1);
    QCOMPARE(submitted, boundary == 0
        ? ParameterService::ExactSubmitResult::TransportUnavailable
        : ParameterService::ExactSubmitResult::TransportOutcomeUncertain);
}

QTEST_GUILESS_MAIN(ParameterServiceTest)

#include "test_parameterservice.moc"
