#include "ParameterService.h"

#include "ExactLinkTransmitter.h"
#include "VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"

#include <QSet>

#include <algorithm>
#include <cstring>
#include <limits>

namespace
{

QString parameterName(const char *bytes, int size)
{
    QByteArray name(bytes, size);
    const int terminator = name.indexOf('\0');
    if (terminator >= 0) {
        name.truncate(terminator);
    }
    return QString::fromLatin1(name);
}

} // namespace

ParameterService::ParameterService(
    VehicleTargetManager *targetManager,
    ExactLinkTransmitter *transmitter,
    QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_transmitter(transmitter)
    , m_store(new ParameterStore(this))
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_transmitter);
    m_listRetryTimer.setSingleShot(true);
    connect(&m_listRetryTimer, &QTimer::timeout,
            this, &ParameterService::handleListRetryTimeout);
    connect(m_targetManager,
            &VehicleTargetManager::targetGenerationChanged,
            this, &ParameterService::handleTargetGenerationChanged);
    connect(m_targetManager,
            &VehicleTargetManager::targetGenerationSettled,
            this, &ParameterService::handleTargetGenerationSettled);
    connect(m_targetManager, &VehicleTargetManager::currentTargetChanged,
            this, &ParameterService::syncSelectedEndpoint);
    m_lastHandledTargetGeneration = m_targetManager->targetGeneration();
    m_cancellingTransactions =
        !m_targetManager->isTargetGenerationSettled();
    syncSelectedEndpoint();
}

QObject *ParameterService::storeObject() const
{
    return m_store;
}

void ParameterService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

int ParameterService::requestCurrentParameterList()
{
    return static_cast<int>(requestParameterList(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId));
}

int ParameterService::requestCurrentParameterRead(const QString &name)
{
    return static_cast<int>(requestParameterRead(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId, name));
}

int ParameterService::requestCurrentParameterReadByIndex(int index)
{
    return static_cast<int>(requestParameterReadByIndex(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId, index));
}

bool ParameterService::cancelCurrentParameterList()
{
    const VehicleTargetLease current = m_targetManager->acquireTarget();
    if (m_listActive) {
        if (!current.isValid()
            || current.generation != m_listTarget.generation
            || !current.endpoint.sameIdentity(m_listTarget.endpoint)) {
            return false;
        }
        cancelParameterList(true);
        return true;
    }
    if (!m_queuedList.active || !current.isValid()
        || current.generation != m_queuedList.target.generation
        || !current.endpoint.sameIdentity(m_queuedList.target.endpoint)) {
        return false;
    }
    cancelQueuedParameterList(true);
    return true;
}

int ParameterService::setCurrentParameter(
    const QString &name, const QVariant &value, int type)
{
    if (type < static_cast<int>(ParameterType::UInt8)
        || type > static_cast<int>(ParameterType::Real64)) {
        return static_cast<int>(SendResult::InvalidParameter);
    }
    return static_cast<int>(setParameter(
        m_targetManager->acquireTarget(),
        m_localSystemId, m_localComponentId,
        name, value, static_cast<ParameterType>(type)));
}

qulonglong ParameterService::writeCurrentParameter(
    const QString &name, const QVariant &value, bool force)
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid()) {
        return 0;
    }
    const ParameterSnapshot snapshot = m_store->snapshot(target.endpoint);
    const quint8 componentId =
        static_cast<quint8>(target.endpoint.componentId);
    if (!snapshot.contains(componentId, name)) {
        return 0;
    }
    const ParameterRecord record = snapshot.value(componentId, name);
    quint64 transactionId = 0;
    const bool wasPumping = m_pumpingTransactions;
    m_pumpingTransactions = true;
    const SendResult result = enqueueParameterWrite(
        target, m_localSystemId, m_localComponentId,
        name, value, record.type, force, true, 0, &transactionId);
    m_pumpingTransactions = wasPumping;
    if (!wasPumping && result == SendResult::Sent) {
        // The transaction ID must be returned to QML before a no-op,
        // synchronous acknowledgement or immediate transport failure can
        // emit the terminal signal carrying that ID.
        QTimer::singleShot(0, this, [this]() { pumpTransactions(); });
    }
    return result == SendResult::Sent ? transactionId : 0;
}

qulonglong ParameterService::writeCurrentParameters(
    const QVariantList &changes, bool force)
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid() || !targetIsCurrent(target) || changes.isEmpty()) {
        return 0;
    }

    struct Candidate
    {
        QString name;
        QVariant value;
        ParameterType type = ParameterType::Unknown;
    };
    QList<Candidate> candidates;
    candidates.reserve(changes.size());
    QSet<QString> names;
    const ParameterSnapshot snapshot = m_store->snapshot(target.endpoint);
    const quint8 componentId =
        static_cast<quint8>(target.endpoint.componentId);
    for (const QVariant &change : changes) {
        const QVariantMap map = change.toMap();
        const QString name = map.value(QStringLiteral("name")).toString();
        const QVariant value = map.value(QStringLiteral("value"));
        if (name.isEmpty() || !value.isValid() || names.contains(name)
            || !snapshot.contains(componentId, name)) {
            return 0;
        }
        const ParameterType type = snapshot.value(componentId, name).type;
        if (!validateParameterWrite(target, name, value, type)) {
            return 0;
        }
        names.insert(name);
        candidates.append(Candidate{name, value, type});
    }

    // Mission Planner intends enabling parameters to be written last. Its
    // historical comparator accidentally did the opposite; preserve the safe
    // documented behaviour and make ordering deterministic within each group.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &left, const Candidate &right) {
        const bool leftEnables = left.name.endsWith(
            QStringLiteral("ENABLE"), Qt::CaseSensitive);
        const bool rightEnables = right.name.endsWith(
            QStringLiteral("ENABLE"), Qt::CaseSensitive);
        if (leftEnables != rightEnables) {
            return !leftEnables;
        }
        return left.name < right.name;
    });

    const quint64 batchId = m_nextBatchId++;
    m_pendingBatches.insert(batchId, PendingBatch{candidates.size(), 0, 0, 0});

    // Queue the complete, prevalidated batch before any item can start. This
    // keeps a signal handler from interleaving another write inside the batch.
    const bool wasPumping = m_pumpingTransactions;
    m_pumpingTransactions = true;
    for (const Candidate &candidate : candidates) {
        quint64 ignoredTransactionId = 0;
        const SendResult result = enqueueParameterWrite(
            target, m_localSystemId, m_localComponentId,
            candidate.name, candidate.value, candidate.type,
            force, true, batchId, &ignoredTransactionId);
        Q_ASSERT(result == SendResult::Sent);
    }
    m_pumpingTransactions = wasPumping;

    emit parameterBatchStarted(
        batchId, target.generation,
        target.endpoint.linkId, target.endpoint.systemId,
        target.endpoint.componentId, candidates.size());
    if (!wasPumping) {
        // As with a single high-level write, let the caller receive batchId
        // before any terminal batch signal can be emitted.
        QTimer::singleShot(0, this, [this]() { pumpTransactions(); });
    }
    return batchId;
}

bool ParameterService::cancelParameterWrite(qulonglong transactionId)
{
    if (transactionId == 0) {
        return false;
    }
    if (m_writeActive
        && m_activeWrite.transactionId == transactionId) {
        cancelActiveWrite();
        return true;
    }
    for (int index = 0; index < m_writeQueue.size(); ++index) {
        if (m_writeQueue.at(index).transactionId != transactionId) {
            continue;
        }
        const PendingWrite cancelled = m_writeQueue.takeAt(index);
        m_writesBeforeQueuedList.remove(cancelled.transactionId);
        emit parameterWriteCancelled(
            cancelled.transactionId, cancelled.batchId,
            cancelled.target.generation,
            cancelled.target.endpoint.linkId,
            cancelled.target.endpoint.systemId,
            cancelled.target.endpoint.componentId,
            cancelled.name);
        finishBatchItem(cancelled.batchId, false);
        pumpTransactions();
        return true;
    }
    return false;
}

QVariant ParameterService::currentParameterValue(const QString &name) const
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid()) {
        return {};
    }
    const ParameterSnapshot snapshot = m_store->snapshot(target.endpoint);
    const quint8 componentId =
        static_cast<quint8>(target.endpoint.componentId);
    return snapshot.contains(componentId, name)
        ? snapshot.value(componentId, name).value : QVariant{};
}

QVariantList ParameterService::currentParameters() const
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid()) {
        return {};
    }
    QList<ParameterRecord> records =
        m_store->snapshot(target.endpoint).records();
    std::sort(records.begin(), records.end(),
              [](const ParameterRecord &left,
                 const ParameterRecord &right) {
        return left.key.name < right.key.name;
    });
    QVariantList result;
    result.reserve(records.size());
    for (const ParameterRecord &record : records) {
        result.append(QVariantMap{
            {QStringLiteral("componentId"), record.key.componentId},
            {QStringLiteral("name"), record.key.name},
            {QStringLiteral("value"), record.value},
            {QStringLiteral("type"), static_cast<int>(record.type)},
            {QStringLiteral("index"), record.index},
            {QStringLiteral("reportedCount"), record.reportedCount}
        });
    }
    return result;
}

ParameterService::SendResult ParameterService::requestParameterList(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }

    // The manager has already installed the exact final identity, but signal
    // delivery may still be invalidating the previous generation. Preserve
    // the new intent without cancelling/transmitting from inside that phase.
    if (m_cancellingTransactions
        || !m_targetManager->isTargetGenerationSettled()) {
        if (m_queuedList.active
            && m_queuedList.target.generation == target.generation
            && m_queuedList.target.endpoint.sameIdentity(target.endpoint)) {
            return SendResult::Sent;
        }
        if (m_queuedList.active) {
            cancelQueuedParameterList(true);
        }
        m_queuedList.active = true;
        m_queuedList.target = target;
        m_queuedList.localSystemId = localSystemId;
        m_queuedList.localComponentId = localComponentId;
        m_writesBeforeQueuedList.clear();
        if (m_writeActive) {
            m_writesBeforeQueuedList.insert(m_activeWrite.transactionId);
        }
        for (const PendingWrite &write : m_writeQueue) {
            m_writesBeforeQueuedList.insert(write.transactionId);
        }
        return SendResult::Sent;
    }

    if (m_listActive
        && m_listTarget.generation == target.generation
        && m_listTarget.endpoint.sameIdentity(target.endpoint)) {
        // Setup, Config and QML consumers can coexist. A second refresh for
        // the same exact target joins the active transfer instead of
        // discarding its staged progress and flooding the link.
        return SendResult::Sent;
    }
    if (m_queuedList.active
        && m_queuedList.target.generation == target.generation
        && m_queuedList.target.endpoint.sameIdentity(target.endpoint)) {
        return SendResult::Sent;
    }
    if (m_listActive) {
        cancelParameterList(true);
    }
    if (m_queuedList.active) {
        cancelQueuedParameterList(true);
    }

    // A PARAM_VALUE is both list data and the acknowledgement for PARAM_SET.
    // Never overlap these protocols: a refresh requested during writes becomes
    // the next queue boundary and subsequent writes wait behind it.
    if (m_writeActive || !m_writeQueue.isEmpty()) {
        m_queuedList.active = true;
        m_queuedList.target = target;
        m_queuedList.localSystemId = localSystemId;
        m_queuedList.localComponentId = localComponentId;
        m_writesBeforeQueuedList.clear();
        if (m_writeActive) {
            m_writesBeforeQueuedList.insert(m_activeWrite.transactionId);
        }
        for (const PendingWrite &write : m_writeQueue) {
            m_writesBeforeQueuedList.insert(write.transactionId);
        }
        return SendResult::Sent;
    }
    return startParameterList(target, localSystemId, localComponentId);
}

ParameterService::SendResult ParameterService::startParameterList(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    m_listActive = true;
    m_listTarget = target;
    m_listLocalSystemId = localSystemId;
    m_listLocalComponentId = localComponentId;
    m_listReportedCount = 0;
    m_listReceivedIndices.clear();
    m_listIndexAttempts.clear();
    m_listNextMissingIndex = 0;
    m_listWholeRetries = 0;
    m_store->beginLoad(target.endpoint);
    emit listStarted(target.generation,
                     target.endpoint.linkId,
                     target.endpoint.systemId,
                     target.endpoint.componentId);

    const SendResult result = transmitParameterListRequest();
    if (result != SendResult::Sent) {
        failParameterList(QStringLiteral(
            "Unable to send PARAM_REQUEST_LIST on the selected link."));
        return result;
    }
    restartListTimer(m_listInactivityMs);
    return result;
}

ParameterService::SendResult ParameterService::requestParameterRead(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    const QString &name)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    QByteArray nameBytes;
    if (!parameterNameBytes(name, &nameBytes)) {
        return SendResult::InvalidParameter;
    }
    // A targetGenerationChanged listener can request data for the newly
    // selected endpoint while older listeners are still cancelling the old
    // generation. Do not transmit into that half-settled state: the explicit
    // StaleTarget result lets direct C++/QML callers retry after settlement.
    if (m_cancellingTransactions
        || !m_targetManager->isTargetGenerationSettled()) {
        return SendResult::StaleTarget;
    }

    const bool hadPrevious = m_pendingReads.contains(name);
    const PendingRead previous = m_pendingReads.value(name);
    m_pendingReads.insert(name, PendingRead{target});

    mavlink_param_request_read_t payload{};
    payload.target_system = static_cast<quint8>(target.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(target.endpoint.componentId);
    payload.param_index = -1;
    std::memcpy(payload.param_id, nameBytes.constData(),
                static_cast<size_t>(nameBytes.size()));
    mavlink_message_t message{};
    mavlink_msg_param_request_read_encode(
        localSystemId, localComponentId, &message, &payload);

    const SendResult result = send(
        target, localSystemId, localComponentId, message);
    if (result != SendResult::Sent) {
        const auto pending = m_pendingReads.constFind(name);
        if (pending != m_pendingReads.constEnd()
            && pending->target.generation == target.generation
            && pending->target.endpoint.sameIdentity(target.endpoint)) {
            if (hadPrevious) {
                m_pendingReads.insert(name, previous);
            } else {
                m_pendingReads.remove(name);
            }
        }
    }
    return result;
}

ParameterService::SendResult ParameterService::requestParameterReadByIndex(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    int index)
{
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (index < 0 || index > std::numeric_limits<qint16>::max()) {
        return SendResult::InvalidParameter;
    }
    if (m_cancellingTransactions
        || !m_targetManager->isTargetGenerationSettled()) {
        return SendResult::StaleTarget;
    }

    mavlink_param_request_read_t payload{};
    payload.target_system = static_cast<quint8>(target.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(target.endpoint.componentId);
    payload.param_index = static_cast<qint16>(index);
    mavlink_message_t message{};
    mavlink_msg_param_request_read_encode(
        localSystemId, localComponentId, &message, &payload);
    return send(target, localSystemId, localComponentId, message);
}

ParameterService::SendResult ParameterService::setParameter(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    const QString &name, const QVariant &value, ParameterType type,
    bool force)
{
    return enqueueParameterWrite(
        target, localSystemId, localComponentId,
        name, value, type, force, false, 0, nullptr);
}

void ParameterService::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (linkId < 0) {
        return;
    }

    const VehicleEndpoint source = endpointFor(linkId, message);
    if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        mavlink_heartbeat_t heartbeat{};
        mavlink_msg_heartbeat_decode(&message, &heartbeat);
        if (heartbeat.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA) {
            setEncoding(source, ParameterEncoding::CStyleCast);
        }
        return;
    }
    if (message.msgid == MAVLINK_MSG_ID_AUTOPILOT_VERSION) {
        mavlink_autopilot_version_t version{};
        mavlink_msg_autopilot_version_decode(&message, &version);
        if (version.capabilities & MAV_PROTOCOL_CAPABILITY_PARAM_FLOAT) {
            setEncoding(source, ParameterEncoding::CStyleCast);
        } else if (version.capabilities
                   & MAV_PROTOCOL_CAPABILITY_PARAM_UNION) {
            setEncoding(source, ParameterEncoding::Bytewise);
        }
        return;
    }
    if (message.msgid != MAVLINK_MSG_ID_PARAM_VALUE
        || !source.isValid()
        || !m_targetManager->isTargetGenerationSettled()) {
        return;
    }

    // Parameter operations are leased to the selected endpoint.  Once the
    // generation changes, late packets from the cancelled transaction must
    // not mutate a committed cache that the user may later select again.
    const VehicleTargetLease currentTarget =
        m_targetManager->acquireTarget();
    if (!sameEnvelope(currentTarget, linkId, message)
        || !targetIsCurrent(currentTarget)) {
        return;
    }

    mavlink_param_value_t payload{};
    mavlink_msg_param_value_decode(&message, &payload);
    const QString name = parameterName(
        payload.param_id, MAVLINK_MSG_PARAM_VALUE_FIELD_PARAM_ID_LEN);
    const ParameterType type = parameterType(payload.param_type);
    const quint64 candidateWriteId =
        m_writeActive && m_activeWrite.name == name
            && sameEnvelope(m_activeWrite.target, linkId, message)
            && targetIsCurrent(m_activeWrite.target)
        ? m_activeWrite.transactionId : 0;
    const ParameterEncoding decodeEncoding = candidateWriteId != 0
        ? m_activeWrite.encoding : encodingFor(source);
    bool decoded = false;
    const QVariant value = ParameterCodec::decodeClassic(
        payload.param_value, type, decodeEncoding, &decoded);
    if (name.isEmpty() || !decoded) {
        return;
    }

    if (!m_store->ingest(source, payload.param_count, payload.param_index,
                         name, value, type)) {
        return;
    }
    if (!m_targetManager->isTargetGenerationSettled()
        || !targetIsCurrent(currentTarget)) {
        return;
    }

    emit parameterValueReceived(
        currentTarget.generation,
        source.linkId, source.systemId, source.componentId,
        payload.param_count, payload.param_index,
        name, value, static_cast<int>(type));
    if (!m_targetManager->isTargetGenerationSettled()
        || !targetIsCurrent(currentTarget)) {
        return;
    }

    if (m_listActive && sameEnvelope(m_listTarget, linkId, message)
        && targetIsCurrent(m_listTarget)) {
        if (payload.param_count != std::numeric_limits<quint16>::max()
            && payload.param_count > 0) {
            m_listReportedCount = qMax(
                m_listReportedCount, static_cast<int>(payload.param_count));
        }
        const bool countableIndex =
            payload.param_index != std::numeric_limits<quint16>::max()
            && payload.param_index < m_listReportedCount;
        const bool newIndex = countableIndex
            && !m_listReceivedIndices.contains(payload.param_index);
        if (newIndex) {
            m_listReceivedIndices.insert(payload.param_index);
            m_listIndexAttempts.remove(payload.param_index);
        }

        // ParameterSnapshot intentionally keeps exposing the last committed
        // data while a refresh is staged. Its isComplete() can therefore be
        // true for the old snapshot; completion must use this transaction's
        // own unique wire indices instead.
        if (m_listReportedCount > 0
            && m_listReceivedIndices.size() >= m_listReportedCount) {
            completeParameterList(source);
        } else if (newIndex) {
            // Mission Planner immediately enters recovery after seeing the
            // nominal final index with gaps; otherwise inactivity is measured
            // from the most recent unique list value.
            const bool nominalLast = m_listReportedCount > 0
                && payload.param_index == m_listReportedCount - 1;
            restartListTimer(nominalLast ? 0 : m_listInactivityMs);
        }
    }

    const auto read = m_pendingReads.find(name);
    if (read != m_pendingReads.end()
        && sameEnvelope(read->target, linkId, message)
        && targetIsCurrent(read->target)) {
        const VehicleTargetLease completed = read->target;
        m_pendingReads.erase(read);
        emit parameterRead(
            completed.generation, source.linkId, source.systemId,
            source.componentId, name, value, static_cast<int>(type));
    }

    if (candidateWriteId != 0
        && m_writeActive
        && m_activeWrite.transactionId == candidateWriteId
        && m_activeWrite.name == name
        && sameEnvelope(m_activeWrite.target, linkId, message)
        && targetIsCurrent(m_activeWrite.target)) {
        // Classic PARAM_VALUE has no transaction identifier. A same-name
        // value can be an unsolicited update, a read response, another GCS's
        // write, or a delayed echo. Only an exact type/value match is a safe
        // acknowledgement; mismatches remain authoritative cache updates but
        // leave this transaction pending for its exact echo or timeout.
        if (m_activeWrite.type == type
            && ParameterCodec::valuesEqual(
                m_activeWrite.value, value, type)) {
            acknowledgeActiveWrite(value, type);
        }
    }
}

void ParameterService::setEncoding(
    const VehicleEndpoint &endpoint, ParameterEncoding encoding)
{
    if (endpoint.isValid()) {
        m_encodings.insert(endpoint, encoding);
    }
}

void ParameterService::forgetLink(int linkId)
{
    QSet<quint64> cancelled;
    m_cancellingTransactions = true;
    if (m_listActive && m_listTarget.endpoint.linkId == linkId) {
        cancelled.insert(m_listTarget.generation);
        cancelParameterList(true);
    }
    if (m_queuedList.active
        && m_queuedList.target.endpoint.linkId == linkId) {
        cancelled.insert(m_queuedList.target.generation);
        cancelQueuedParameterList(true);
    }
    for (auto it = m_pendingReads.begin(); it != m_pendingReads.end();) {
        if (it->target.endpoint.linkId == linkId) {
            cancelled.insert(it->target.generation);
            it = m_pendingReads.erase(it);
        } else {
            ++it;
        }
    }
    if (m_writeActive && m_activeWrite.target.endpoint.linkId == linkId) {
        cancelled.insert(m_activeWrite.target.generation);
        cancelActiveWrite();
    }
    for (const PendingWrite &write : m_writeQueue) {
        if (write.target.endpoint.linkId == linkId) {
            cancelled.insert(write.target.generation);
        }
    }
    cancelQueuedWritesForGeneration(0, linkId);
    for (auto it = m_encodings.begin(); it != m_encodings.end();) {
        if (it.key().linkId == linkId) {
            it = m_encodings.erase(it);
        } else {
            ++it;
        }
    }
    const QList<VehicleEndpoint> endpoints = m_store->cachedEndpoints();
    for (const VehicleEndpoint &endpoint : endpoints) {
        if (endpoint.linkId == linkId) {
            m_store->removeEndpoint(endpoint);
        }
    }
    for (quint64 generation : cancelled) {
        emit transactionsCancelled(generation);
    }
    m_cancellingTransactions = false;
    pumpTransactions();
}

void ParameterService::setRetryPolicyForTesting(
    int inactivityMs, int missingBurstIntervalMs,
    int maximumWholeListRetries, int maximumIndexAttempts)
{
    m_listInactivityMs = qMax(1, inactivityMs);
    m_listMissingBurstIntervalMs = qMax(1, missingBurstIntervalMs);
    m_listMaximumWholeRetries = qMax(0, maximumWholeListRetries);
    m_listMaximumIndexAttempts = qMax(1, maximumIndexAttempts);
}

void ParameterService::setWriteRetryPolicyForTesting(
    int acknowledgementTimeoutMs, int maximumRetries)
{
    m_writeAcknowledgementTimeoutMs = qMax(1, acknowledgementTimeoutMs);
    m_writeMaximumRetries = qMax(0, maximumRetries);
}

ParameterService::SendResult ParameterService::enqueueParameterWrite(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    const QString &name, const QVariant &value, ParameterType type,
    bool force, bool schemaBound, quint64 batchId,
    quint64 *transactionId)
{
    if (transactionId) {
        *transactionId = 0;
    }
    if (!target.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (!validateParameterWrite(target, name, value, type)) {
        return SendResult::InvalidParameter;
    }

    PendingWrite write;
    write.transactionId = m_nextWriteTransactionId++;
    if (write.transactionId == 0) {
        write.transactionId = m_nextWriteTransactionId++;
    }
    write.batchId = batchId;
    write.target = target;
    write.localSystemId = localSystemId;
    write.localComponentId = localComponentId;
    write.name = name;
    write.type = type;
    write.requestedValue = value;
    if (!normalizeParameterWrite(
            target, value, type, &write.value,
            &write.wireValue, &write.encoding)) {
        return SendResult::InvalidParameter;
    }
    write.force = force;
    write.schemaBound = schemaBound;

    if (transactionId) {
        *transactionId = write.transactionId;
    }
    m_writeQueue.enqueue(write);
    pumpTransactions();
    return SendResult::Sent;
}

bool ParameterService::validateParameterWrite(
    const VehicleTargetLease &target,
    const QString &name, const QVariant &value, ParameterType type,
    QByteArray *nameBytes) const
{
    if (!target.isValid() || !targetIsCurrent(target) || !value.isValid()) {
        return false;
    }
    QByteArray encodedName;
    if (!parameterNameBytes(name, &encodedName)) {
        return false;
    }
    if (!normalizeParameterWrite(
            target, value, type, nullptr, nullptr, nullptr)) {
        return false;
    }
    if (nameBytes) {
        *nameBytes = encodedName;
    }
    return true;
}

bool ParameterService::normalizeParameterWrite(
    const VehicleTargetLease &target,
    const QVariant &value, ParameterType type,
    QVariant *normalizedValue, float *wireValue,
    ParameterEncoding *encoding) const
{
    const ParameterEncoding selectedEncoding = encodingFor(target.endpoint);
    bool encoded = false;
    const float encodedValue = ParameterCodec::encodeClassic(
        value, type, selectedEncoding, &encoded);
    if (!encoded) {
        return false;
    }
    bool decoded = false;
    const QVariant normalized = ParameterCodec::decodeClassic(
        encodedValue, type, selectedEncoding, &decoded);
    if (!decoded) {
        return false;
    }
    // C-style integer parameters travel through a float. Values that cannot
    // make that round trip exactly would otherwise wait forever for an ACK the
    // vehicle can never echo as requested.
    if (ParameterCodec::isInteger(type)
        && !ParameterCodec::valuesEqual(value, normalized, type)) {
        return false;
    }
    if (normalizedValue) {
        *normalizedValue = normalized;
    }
    if (wireValue) {
        *wireValue = encodedValue;
    }
    if (encoding) {
        *encoding = selectedEncoding;
    }
    return true;
}

void ParameterService::pumpTransactions()
{
    if (m_cancellingTransactions
        || !m_targetManager->isTargetGenerationSettled()) {
        return;
    }
    if (m_pumpingTransactions) {
        m_pumpAgain = true;
        return;
    }

    m_pumpingTransactions = true;
    do {
        m_pumpAgain = false;
        if (m_listActive || m_writeActive) {
            break;
        }

        if (m_queuedList.active
            && m_writesBeforeQueuedList.isEmpty()) {
            const QueuedListRequest queued = m_queuedList;
            m_queuedList = {};
            const SendResult result = startParameterList(
                queued.target, queued.localSystemId,
                queued.localComponentId);
            if (result == SendResult::Sent && m_listActive) {
                break;
            }
            continue;
        }

        if (m_writeQueue.isEmpty()) {
            // A boundary can only retain IDs for queued/active writes. Avoid a
            // deadlock if an operation was removed by a future cancellation
            // path without reaching a terminal helper.
            if (m_queuedList.active) {
                m_writesBeforeQueuedList.clear();
                m_pumpAgain = true;
            }
            continue;
        }

        m_activeWrite = m_writeQueue.dequeue();
        m_writeActive = true;
        startActiveWrite();
    } while (m_pumpAgain
             || (!m_listActive && !m_writeActive
                 && (m_queuedList.active || !m_writeQueue.isEmpty())));
    m_pumpingTransactions = false;
}

void ParameterService::startActiveWrite()
{
    if (!m_writeActive) {
        return;
    }
    if (!targetIsCurrent(m_activeWrite.target)) {
        failActiveWrite(
            WriteFailureReason::StaleTarget,
            QStringLiteral("The selected vehicle target changed."));
        return;
    }

    const ParameterSnapshot snapshot = m_store->snapshot(
        m_activeWrite.target.endpoint);
    const quint8 componentId = static_cast<quint8>(
        m_activeWrite.target.endpoint.componentId);
    if (m_activeWrite.schemaBound) {
        if (!snapshot.contains(componentId, m_activeWrite.name)) {
            failActiveWrite(
                WriteFailureReason::InvalidParameter,
                QStringLiteral("The parameter is no longer present in the current vehicle snapshot."));
            return;
        }
        if (snapshot.value(componentId, m_activeWrite.name).type
            != m_activeWrite.type) {
            failActiveWrite(
                WriteFailureReason::TypeMismatch,
                QStringLiteral("The parameter type changed while the write was queued."));
            return;
        }
    }
    if (!normalizeParameterWrite(
            m_activeWrite.target, m_activeWrite.requestedValue,
            m_activeWrite.type, &m_activeWrite.value,
            &m_activeWrite.wireValue, &m_activeWrite.encoding)) {
        failActiveWrite(
            WriteFailureReason::InvalidParameter,
            QStringLiteral("The value cannot be represented using the current parameter encoding."));
        return;
    }
    m_activeWrite.skip = false;
    if (!m_activeWrite.force
        && snapshot.contains(componentId, m_activeWrite.name)) {
        const ParameterRecord committed = snapshot.value(
            componentId, m_activeWrite.name);
        m_activeWrite.skip = committed.type == m_activeWrite.type
            && ParameterCodec::valuesEqual(
                committed.value, m_activeWrite.value,
                m_activeWrite.type);
    }

    const quint64 transactionId = m_activeWrite.transactionId;
    emit parameterWriteStarted(
        transactionId, m_activeWrite.batchId,
        m_activeWrite.target.generation,
        m_activeWrite.target.endpoint.linkId,
        m_activeWrite.target.endpoint.systemId,
        m_activeWrite.target.endpoint.componentId,
        m_activeWrite.name, m_activeWrite.value,
        static_cast<int>(m_activeWrite.type));
    if (!m_writeActive
        || m_activeWrite.transactionId != transactionId) {
        return;
    }
    if (m_activeWrite.skip) {
        skipActiveWrite();
        return;
    }

    const SendResult result = transmitActiveWrite();
    if (!m_writeActive
        || m_activeWrite.transactionId != transactionId) {
        // The transmitter callback may synchronously deliver PARAM_VALUE and
        // complete this operation before sendMessage() returns.
        return;
    }
    if (result != SendResult::Sent) {
        failActiveWrite(
            result == SendResult::StaleTarget
                ? WriteFailureReason::StaleTarget
                : result == SendResult::InvalidParameter
                    ? WriteFailureReason::InvalidParameter
                    : WriteFailureReason::TransportUnavailable,
            QStringLiteral("Unable to send PARAM_SET on the selected link."));
        return;
    }
    scheduleActiveWriteTimeout();
}

ParameterService::SendResult ParameterService::transmitActiveWrite()
{
    if (!m_writeActive) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(m_activeWrite.target)) {
        return SendResult::StaleTarget;
    }
    QByteArray nameBytes;
    if (!parameterNameBytes(m_activeWrite.name, &nameBytes)) {
        return SendResult::InvalidParameter;
    }

    mavlink_param_set_t payload{};
    payload.target_system = static_cast<quint8>(
        m_activeWrite.target.endpoint.systemId);
    payload.target_component = static_cast<quint8>(
        m_activeWrite.target.endpoint.componentId);
    payload.param_value = m_activeWrite.wireValue;
    payload.param_type = static_cast<quint8>(m_activeWrite.type);
    std::memcpy(payload.param_id, nameBytes.constData(),
                static_cast<size_t>(nameBytes.size()));
    mavlink_message_t message{};
    mavlink_msg_param_set_encode(
        m_activeWrite.localSystemId, m_activeWrite.localComponentId,
        &message, &payload);
    ++m_activeWrite.attempts;
    return send(m_activeWrite.target,
                m_activeWrite.localSystemId,
                m_activeWrite.localComponentId, message);
}

void ParameterService::scheduleActiveWriteTimeout()
{
    if (!m_writeActive) {
        return;
    }
    const quint64 transactionId = m_activeWrite.transactionId;
    const int attempt = m_activeWrite.attempts;
    QTimer::singleShot(
        m_writeAcknowledgementTimeoutMs, this,
        [this, transactionId, attempt]() {
            handleWriteRetryTimeout(transactionId, attempt);
        });
}

void ParameterService::handleWriteRetryTimeout(
    quint64 transactionId, int attempt)
{
    if (!m_writeActive
        || m_activeWrite.transactionId != transactionId
        || m_activeWrite.attempts != attempt) {
        return;
    }
    if (!targetIsCurrent(m_activeWrite.target)) {
        failActiveWrite(
            WriteFailureReason::StaleTarget,
            QStringLiteral("The selected vehicle target changed."));
        return;
    }
    if (m_activeWrite.attempts >= 1 + m_writeMaximumRetries) {
        failActiveWrite(
            WriteFailureReason::Timeout,
            QStringLiteral("The vehicle did not acknowledge PARAM_SET."));
        return;
    }
    const SendResult result = transmitActiveWrite();
    if (!m_writeActive
        || m_activeWrite.transactionId != transactionId) {
        return;
    }
    if (result != SendResult::Sent) {
        failActiveWrite(
            result == SendResult::StaleTarget
                ? WriteFailureReason::StaleTarget
                : WriteFailureReason::TransportUnavailable,
            QStringLiteral("A PARAM_SET retry could not use the selected link."));
        return;
    }
    emit parameterWriteRetried(
        m_activeWrite.transactionId, m_activeWrite.attempts);
    scheduleActiveWriteTimeout();
}

void ParameterService::acknowledgeActiveWrite(
    const QVariant &value, ParameterType type)
{
    if (!m_writeActive) {
        return;
    }
    const PendingWrite completed = m_activeWrite;
    m_writeActive = false;
    m_activeWrite = {};
    m_writesBeforeQueuedList.remove(completed.transactionId);

    emit parameterWriteAcknowledged(
        completed.target.generation,
        completed.target.endpoint.linkId,
        completed.target.endpoint.systemId,
        completed.target.endpoint.componentId,
        completed.name, value, static_cast<int>(type));
    emit parameterWriteCompleted(
        completed.transactionId, completed.batchId,
        completed.target.generation,
        completed.target.endpoint.linkId,
        completed.target.endpoint.systemId,
        completed.target.endpoint.componentId,
        completed.name, value, static_cast<int>(type));
    finishBatchItem(completed.batchId, true);
    pumpTransactions();
}

void ParameterService::skipActiveWrite()
{
    if (!m_writeActive) {
        return;
    }
    const PendingWrite skipped = m_activeWrite;
    m_writeActive = false;
    m_activeWrite = {};
    m_writesBeforeQueuedList.remove(skipped.transactionId);

    // Existing widgets treat the legacy acknowledgement as terminal success.
    emit parameterWriteAcknowledged(
        skipped.target.generation,
        skipped.target.endpoint.linkId,
        skipped.target.endpoint.systemId,
        skipped.target.endpoint.componentId,
        skipped.name, skipped.value, static_cast<int>(skipped.type));
    emit parameterWriteSkipped(
        skipped.transactionId, skipped.batchId,
        skipped.target.generation,
        skipped.target.endpoint.linkId,
        skipped.target.endpoint.systemId,
        skipped.target.endpoint.componentId,
        skipped.name, skipped.value, static_cast<int>(skipped.type));
    finishBatchItem(skipped.batchId, true);
    pumpTransactions();
}

void ParameterService::failActiveWrite(
    WriteFailureReason reason, const QString &message)
{
    if (!m_writeActive) {
        return;
    }
    const PendingWrite failed = m_activeWrite;
    m_writeActive = false;
    m_activeWrite = {};
    m_writesBeforeQueuedList.remove(failed.transactionId);
    emit parameterWriteFailed(
        failed.transactionId, failed.batchId,
        failed.target.generation,
        failed.target.endpoint.linkId,
        failed.target.endpoint.systemId,
        failed.target.endpoint.componentId,
        failed.name, static_cast<int>(reason), message);
    finishBatchItem(failed.batchId, false);
    pumpTransactions();
}

void ParameterService::cancelActiveWrite()
{
    if (!m_writeActive) {
        return;
    }
    const PendingWrite cancelled = m_activeWrite;
    m_writeActive = false;
    m_activeWrite = {};
    m_writesBeforeQueuedList.remove(cancelled.transactionId);
    emit parameterWriteCancelled(
        cancelled.transactionId, cancelled.batchId,
        cancelled.target.generation,
        cancelled.target.endpoint.linkId,
        cancelled.target.endpoint.systemId,
        cancelled.target.endpoint.componentId,
        cancelled.name);
    finishBatchItem(cancelled.batchId, false);
    pumpTransactions();
}

void ParameterService::cancelQueuedWritesForGeneration(
    quint64 generation, int linkId)
{
    QQueue<PendingWrite> retained;
    QList<PendingWrite> cancelled;
    while (!m_writeQueue.isEmpty()) {
        const PendingWrite write = m_writeQueue.dequeue();
        const bool generationMatches = generation == 0
            || write.target.generation == generation;
        const bool linkMatches = linkId < 0
            || write.target.endpoint.linkId == linkId;
        if (generationMatches && linkMatches) {
            cancelled.append(write);
        } else {
            retained.enqueue(write);
        }
    }
    m_writeQueue = retained;
    for (const PendingWrite &write : cancelled) {
        m_writesBeforeQueuedList.remove(write.transactionId);
        emit parameterWriteCancelled(
            write.transactionId, write.batchId,
            write.target.generation,
            write.target.endpoint.linkId,
            write.target.endpoint.systemId,
            write.target.endpoint.componentId,
            write.name);
        finishBatchItem(write.batchId, false);
    }
}

void ParameterService::finishBatchItem(quint64 batchId, bool succeeded)
{
    if (batchId == 0) {
        return;
    }
    auto batch = m_pendingBatches.find(batchId);
    if (batch == m_pendingBatches.end()) {
        return;
    }
    ++batch->completed;
    if (succeeded) {
        ++batch->succeeded;
    } else {
        ++batch->failed;
    }
    const PendingBatch progress = *batch;
    const bool complete = progress.completed >= progress.total;
    if (complete) {
        m_pendingBatches.erase(batch);
    }
    emit parameterBatchProgress(
        batchId, progress.completed, progress.total,
        progress.succeeded, progress.failed);
    if (complete) {
        emit parameterBatchCompleted(
            batchId, progress.succeeded, progress.failed);
    }
}

void ParameterService::cancelQueuedParameterList(bool notify)
{
    if (!m_queuedList.active) {
        return;
    }
    const QueuedListRequest cancelled = m_queuedList;
    m_queuedList = {};
    m_writesBeforeQueuedList.clear();
    if (notify) {
        emit listCancelled(
            cancelled.target.generation,
            cancelled.target.endpoint.linkId,
            cancelled.target.endpoint.systemId,
            cancelled.target.endpoint.componentId);
    }
    pumpTransactions();
}

bool ParameterService::targetIsCurrent(
    const VehicleTargetLease &target) const
{
    return m_targetManager && target.isValid()
        && m_targetManager->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation);
}

bool ParameterService::parameterNameBytes(
    const QString &name, QByteArray *bytes)
{
    if (!bytes || name.isEmpty() || name.contains(QChar::Null)) {
        return false;
    }
    const QByteArray latin1 = name.toLatin1();
    if (latin1.isEmpty()
        || latin1.size() > MAVLINK_MSG_PARAM_SET_FIELD_PARAM_ID_LEN
        || QString::fromLatin1(latin1) != name) {
        return false;
    }
    *bytes = latin1;
    return true;
}

ParameterType ParameterService::parameterType(quint8 mavlinkType)
{
    if (mavlinkType < static_cast<quint8>(ParameterType::UInt8)
        || mavlinkType > static_cast<quint8>(ParameterType::Real64)) {
        return ParameterType::Unknown;
    }
    return static_cast<ParameterType>(mavlinkType);
}

bool ParameterService::sameEnvelope(
    const VehicleTargetLease &target, int linkId,
    const mavlink_message_t &message)
{
    return target.isValid()
        && target.endpoint.linkId == linkId
        && target.endpoint.systemId == message.sysid
        && target.endpoint.componentId == message.compid;
}

VehicleEndpoint ParameterService::endpointFor(
    int linkId, const mavlink_message_t &message) const
{
    const QList<VehicleEndpoint> endpoints = m_targetManager->endpoints();
    for (const VehicleEndpoint &endpoint : endpoints) {
        if (endpoint.linkId == linkId
            && endpoint.systemId == message.sysid
            && endpoint.componentId == message.compid) {
            return endpoint;
        }
    }
    VehicleEndpoint endpoint;
    endpoint.linkId = linkId;
    endpoint.systemId = message.sysid;
    endpoint.componentId = message.compid;
    return endpoint;
}

ParameterEncoding ParameterService::encodingFor(
    const VehicleEndpoint &endpoint) const
{
    return m_encodings.value(endpoint, ParameterEncoding::Bytewise);
}

ParameterService::SendResult ParameterService::send(
    const VehicleTargetLease &target,
    quint8 localSystemId, quint8 localComponentId,
    mavlink_message_t message)
{
    if (!targetIsCurrent(target)) {
        return SendResult::StaleTarget;
    }
    if (!m_transmitter) {
        return SendResult::TransportUnavailable;
    }
    return m_transmitter->sendMessage(
               target.endpoint.linkId, localSystemId, localComponentId,
               message)
            == ExactLinkTransmitter::SendResult::Sent
        ? SendResult::Sent : SendResult::TransportUnavailable;
}

ParameterService::SendResult ParameterService::transmitParameterListRequest()
{
    if (!m_listActive || !m_listTarget.isValid()) {
        return SendResult::InvalidTarget;
    }
    if (!targetIsCurrent(m_listTarget)) {
        return SendResult::StaleTarget;
    }

    mavlink_param_request_list_t payload{};
    payload.target_system =
        static_cast<quint8>(m_listTarget.endpoint.systemId);
    payload.target_component =
        static_cast<quint8>(m_listTarget.endpoint.componentId);
    mavlink_message_t message{};
    mavlink_msg_param_request_list_encode(
        m_listLocalSystemId, m_listLocalComponentId, &message, &payload);
    return send(m_listTarget,
                m_listLocalSystemId, m_listLocalComponentId, message);
}

ParameterService::SendResult ParameterService::transmitParameterIndexRequest(
    int index)
{
    if (!m_listActive || !m_listTarget.isValid()) {
        return SendResult::InvalidTarget;
    }
    return requestParameterReadByIndex(
        m_listTarget, m_listLocalSystemId, m_listLocalComponentId, index);
}

void ParameterService::completeParameterList(const VehicleEndpoint &source)
{
    if (!m_listActive) {
        return;
    }
    const VehicleTargetLease completed = m_listTarget;
    clearParameterListState();
    emit listCompleted(
        completed.generation,
        source.linkId, source.systemId, source.componentId);
    pumpTransactions();
}

void ParameterService::failParameterList(const QString &reason)
{
    if (!m_listActive) {
        return;
    }
    const VehicleTargetLease failed = m_listTarget;
    m_store->failLoad(failed.endpoint);
    clearParameterListState();
    emit listFailed(
        failed.generation,
        failed.endpoint.linkId,
        failed.endpoint.systemId,
        failed.endpoint.componentId,
        reason);
    pumpTransactions();
}

void ParameterService::cancelParameterList(bool notify)
{
    if (!m_listActive) {
        return;
    }
    const VehicleTargetLease cancelled = m_listTarget;
    m_store->cancelLoad(cancelled.endpoint);
    clearParameterListState();
    if (notify) {
        emit listCancelled(
            cancelled.generation,
            cancelled.endpoint.linkId,
            cancelled.endpoint.systemId,
            cancelled.endpoint.componentId);
    }
    pumpTransactions();
}

void ParameterService::clearParameterListState()
{
    m_listRetryTimer.stop();
    m_listActive = false;
    m_listTarget = {};
    m_listReportedCount = 0;
    m_listReceivedIndices.clear();
    m_listIndexAttempts.clear();
    m_listNextMissingIndex = 0;
    m_listWholeRetries = 0;
}

void ParameterService::restartListTimer(int intervalMs)
{
    if (m_listActive) {
        m_listRetryTimer.start(qMax(0, intervalMs));
    }
}

void ParameterService::handleListRetryTimeout()
{
    if (!m_listActive) {
        return;
    }
    if (!targetIsCurrent(m_listTarget)) {
        const quint64 generation = m_listTarget.generation;
        cancelParameterList(true);
        emit transactionsCancelled(generation);
        return;
    }

    const int received = m_listReceivedIndices.size();
    const bool belowSeventyFivePercent = m_listReportedCount <= 0
        || received * 4 < m_listReportedCount * 3;
    if (belowSeventyFivePercent
        && m_listWholeRetries < m_listMaximumWholeRetries) {
        ++m_listWholeRetries;
        const SendResult result = transmitParameterListRequest();
        if (result != SendResult::Sent) {
            failParameterList(QStringLiteral(
                "Parameter list retry could not use the selected link."));
            return;
        }
        restartListTimer(m_listInactivityMs);
        return;
    }

    if (m_listReportedCount <= 0) {
        failParameterList(QStringLiteral(
            "The vehicle did not report a parameter count."));
        return;
    }

    constexpr int MaximumBurstSize = 10;
    int queued = 0;
    int scanned = 0;
    int index = qBound(0, m_listNextMissingIndex,
                       m_listReportedCount - 1);
    while (scanned < m_listReportedCount && queued < MaximumBurstSize) {
        if (!m_listReceivedIndices.contains(index)
            && m_listIndexAttempts.value(index, 0)
                   < m_listMaximumIndexAttempts) {
            const SendResult result = transmitParameterIndexRequest(index);
            if (result != SendResult::Sent) {
                failParameterList(QStringLiteral(
                    "A missing parameter could not be requested on the selected link."));
                return;
            }
            m_listIndexAttempts[index] =
                m_listIndexAttempts.value(index, 0) + 1;
            ++queued;
        }
        index = (index + 1) % m_listReportedCount;
        ++scanned;
    }
    m_listNextMissingIndex = index;

    if (queued == 0) {
        failParameterList(QStringLiteral(
            "Missing parameters did not answer bounded index retries."));
        return;
    }
    restartListTimer(m_listMissingBurstIntervalMs);
}

void ParameterService::handleTargetGenerationChanged(
    qulonglong generation)
{
    // A nested selection can complete while Qt is still delivering an older
    // signal. Its generation argument distinguishes the stale delivery from
    // the final target state.
    if (generation != m_targetManager->targetGeneration()
        || generation == m_lastHandledTargetGeneration) {
        return;
    }
    m_lastHandledTargetGeneration = generation;
    cancelTransactions(generation);
    syncSelectedEndpoint();
}

void ParameterService::handleTargetGenerationSettled(
    qulonglong generation)
{
    if (generation != m_targetManager->targetGeneration()
        || generation != m_lastHandledTargetGeneration) {
        return;
    }
    m_cancellingTransactions = false;
    pumpTransactions();
}

void ParameterService::syncSelectedEndpoint()
{
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    m_store->selectEndpoint(target.isValid()
                                ? target.endpoint : VehicleEndpoint{});
}

void ParameterService::cancelTransactions(quint64 currentGeneration)
{
    QSet<quint64> generations;
    if (m_listActive
        && m_listTarget.generation != currentGeneration) {
        generations.insert(m_listTarget.generation);
    }
    if (m_queuedList.active
        && m_queuedList.target.generation != currentGeneration) {
        generations.insert(m_queuedList.target.generation);
    }
    for (const PendingRead &read : m_pendingReads) {
        if (read.target.generation != currentGeneration) {
            generations.insert(read.target.generation);
        }
    }
    if (m_writeActive
        && m_activeWrite.target.generation != currentGeneration) {
        generations.insert(m_activeWrite.target.generation);
    }
    for (const PendingWrite &write : m_writeQueue) {
        if (write.target.generation != currentGeneration) {
            generations.insert(write.target.generation);
        }
    }

    // Capture the generations before emitting any cancellation signal. A
    // handler is allowed to enqueue work for the already-selected new target;
    // that new work must not be swept up by this old-generation cleanup.
    m_cancellingTransactions = true;
    if (m_queuedList.active
        && m_queuedList.target.generation != currentGeneration) {
        cancelQueuedParameterList(true);
    }
    if (m_listActive
        && m_listTarget.generation != currentGeneration) {
        cancelParameterList(true);
    }
    if (m_writeActive
        && m_activeWrite.target.generation != currentGeneration) {
        cancelActiveWrite();
    }
    for (auto it = m_pendingReads.begin(); it != m_pendingReads.end();) {
        if (it->target.generation != currentGeneration) {
            it = m_pendingReads.erase(it);
        } else {
            ++it;
        }
    }
    for (quint64 generation : generations) {
        cancelQueuedWritesForGeneration(generation);
    }
    for (quint64 generation : generations) {
        emit transactionsCancelled(generation);
    }
    // VehicleTargetManager's settled phase releases the queue only after all
    // generation/current-target listeners have invalidated their old state.
}
