#include "ParameterStore.h"

#include <QReadLocker>
#include <QWriteLocker>

#include <algorithm>

bool ParameterSnapshot::contains(quint8 componentId, const QString &name) const
{
    return m_records.contains(ParameterKey{componentId, name});
}

ParameterRecord ParameterSnapshot::value(quint8 componentId,
                                         const QString &name) const
{
    return m_records.value(ParameterKey{componentId, name});
}

ParameterStore::ParameterStore(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ParameterLoadState>();
}

VehicleTarget ParameterStore::target() const
{
    QReadLocker locker(&m_lock);
    return m_target;
}

VehicleEndpoint ParameterStore::endpoint() const
{
    QReadLocker locker(&m_lock);
    return m_endpoint;
}

QList<VehicleEndpoint> ParameterStore::cachedEndpoints() const
{
    QReadLocker locker(&m_lock);
    QList<VehicleEndpoint> result;
    result.reserve(m_caches.size());
    for (auto it = m_caches.constBegin(); it != m_caches.constEnd(); ++it) {
        result.append(it.value().endpoint);
    }
    std::sort(result.begin(), result.end(),
              [](const VehicleEndpoint &left, const VehicleEndpoint &right) {
        if (left.linkId != right.linkId) {
            return left.linkId < right.linkId;
        }
        if (left.systemId != right.systemId) {
            return left.systemId < right.systemId;
        }
        return left.componentId < right.componentId;
    });
    return result;
}

ParameterLoadState ParameterStore::state() const
{
    QReadLocker locker(&m_lock);
    const EndpointCache *cache = findCacheLocked(m_endpoint);
    return cache ? cache->state : ParameterLoadState::Idle;
}

ParameterProgress ParameterStore::progress() const
{
    QReadLocker locker(&m_lock);
    const EndpointCache *cache = findCacheLocked(m_endpoint);
    return cache ? visibleProgress(*cache) : ParameterProgress{};
}

ParameterSnapshot ParameterStore::snapshot() const
{
    QReadLocker locker(&m_lock);
    return snapshotLocked(m_endpoint, findCacheLocked(m_endpoint));
}

ParameterSnapshot ParameterStore::snapshot(
    const VehicleEndpoint &requestedEndpoint) const
{
    QReadLocker locker(&m_lock);
    return snapshotLocked(requestedEndpoint,
                          findCacheLocked(requestedEndpoint));
}

bool ParameterStore::hasSnapshot(const VehicleEndpoint &requestedEndpoint) const
{
    QReadLocker locker(&m_lock);
    const EndpointCache *cache = findCacheLocked(requestedEndpoint);
    return cache && !cache->committedRecords.isEmpty();
}

bool ParameterStore::specializedPagesReady() const
{
    QReadLocker locker(&m_lock);
    const EndpointCache *cache = findCacheLocked(m_endpoint);
    return cache && cache->committedComplete;
}

void ParameterStore::selectTarget(int linkId, quint8 systemId,
                                  quint8 componentId)
{
    VehicleEndpoint requested;
    requested.linkId = linkId;
    requested.systemId = systemId;
    requested.componentId = componentId;
    selectEndpoint(requested);
}

void ParameterStore::selectEndpoint(const VehicleEndpoint &requestedEndpoint)
{
    VehicleEndpoint selected = requestedEndpoint;
    ParameterLoadState selectedState = ParameterLoadState::Idle;
    ParameterProgress selectedProgress;
    bool changed = false;

    {
        QWriteLocker locker(&m_lock);
        if (endpointIsValid(selected)) {
            EndpointCache &cache = ensureCacheLocked(selected);
            selected = cache.endpoint;
            selectedState = cache.state;
            selectedProgress = visibleProgress(cache);
        }

        const bool identityChanged = !(endpointKey(m_endpoint)
            == endpointKey(selected));
        const bool metadataChanged = m_endpoint.linkName != selected.linkName
            || m_endpoint.componentName != selected.componentName;
        if (!identityChanged && !metadataChanged) {
            return;
        }

        if (identityChanged) {
            ++m_revisionCounter;
        }
        m_endpoint = selected;
        m_target = VehicleTarget{selected.linkId,
                                 static_cast<quint8>(selected.systemId),
                                 static_cast<quint8>(selected.componentId),
                                 m_revisionCounter};
        changed = true;
    }

    if (changed) {
        emit targetChanged();
        emit selectedEndpointChanged(selected.linkId, selected.systemId,
                                     selected.componentId);
        emit stateChanged(selectedState);
        emit progressChanged(selectedProgress.received,
                             selectedProgress.reported,
                             selectedProgress.percent());
    }
}

void ParameterStore::beginLoad()
{
    beginLoad(endpoint());
}

void ParameterStore::beginLoad(const VehicleEndpoint &requestedEndpoint)
{
    if (!endpointIsValid(requestedEndpoint)) {
        return;
    }

    VehicleEndpoint storedEndpoint;
    bool selected = false;
    {
        QWriteLocker locker(&m_lock);
        EndpointCache &cache = ensureCacheLocked(requestedEndpoint);
        cache.stagingRecords.clear();
        cache.stagingReportedByComponent.clear();
        cache.stagingReceivedIndices.clear();
        cache.stagingRevision = ++m_revisionCounter;
        cache.stagingActive = true;
        cache.state = ParameterLoadState::Loading;
        storedEndpoint = cache.endpoint;
        selected = isSelectedLocked(storedEndpoint);
        if (selected) {
            m_target.revision = cache.stagingRevision;
        }
    }

    if (selected) {
        emit targetChanged();
        emit stateChanged(ParameterLoadState::Loading);
        emit progressChanged(0, 0, 0);
    }
    emit endpointStateChanged(storedEndpoint.linkId, storedEndpoint.systemId,
                              storedEndpoint.componentId,
                              ParameterLoadState::Loading);
    emit endpointProgressChanged(storedEndpoint.linkId,
                                 storedEndpoint.systemId,
                                 storedEndpoint.componentId, 0, 0, 0);
}

void ParameterStore::finishLoad()
{
    finishLoad(endpoint());
}

void ParameterStore::finishLoad(const VehicleEndpoint &requestedEndpoint)
{
    VehicleEndpoint storedEndpoint;
    ParameterLoadState newState = ParameterLoadState::Idle;
    ParameterProgress current;
    bool selected = false;
    bool handled = false;

    {
        QWriteLocker locker(&m_lock);
        auto it = m_caches.find(endpointKey(requestedEndpoint));
        if (it == m_caches.end() || !it->stagingActive) {
            return;
        }

        EndpointCache &cache = it.value();
        const ParameterProgress stagedProgress = progressFor(
            cache.stagingReportedByComponent,
            cache.stagingReceivedIndices);
        if (stagedProgress.complete()) {
            commitStagingLocked(&cache);
            cache.state = ParameterLoadState::Complete;
        } else {
            discardStagingLocked(&cache);
            cache.state = ParameterLoadState::Partial;
        }
        storedEndpoint = cache.endpoint;
        newState = cache.state;
        current = visibleProgress(cache);
        selected = isSelectedLocked(storedEndpoint);
        handled = true;
    }

    if (!handled) {
        return;
    }
    if (selected) {
        emit stateChanged(newState);
        emit progressChanged(current.received, current.reported,
                             current.percent());
    }
    emit endpointStateChanged(storedEndpoint.linkId, storedEndpoint.systemId,
                              storedEndpoint.componentId, newState);
    emit endpointProgressChanged(storedEndpoint.linkId,
                                 storedEndpoint.systemId,
                                 storedEndpoint.componentId,
                                 current.received, current.reported,
                                 current.percent());
}

void ParameterStore::cancelLoad()
{
    cancelLoad(endpoint());
}

void ParameterStore::cancelLoad(const VehicleEndpoint &requestedEndpoint)
{
    VehicleEndpoint storedEndpoint;
    ParameterProgress current;
    bool selected = false;
    {
        QWriteLocker locker(&m_lock);
        auto it = m_caches.find(endpointKey(requestedEndpoint));
        if (it == m_caches.end() || !it->stagingActive) {
            return;
        }
        EndpointCache &cache = it.value();
        discardStagingLocked(&cache);
        cache.state = ParameterLoadState::Cancelled;
        storedEndpoint = cache.endpoint;
        current = visibleProgress(cache);
        selected = isSelectedLocked(storedEndpoint);
    }

    if (selected) {
        emit stateChanged(ParameterLoadState::Cancelled);
        emit progressChanged(current.received, current.reported,
                             current.percent());
    }
    emit endpointStateChanged(storedEndpoint.linkId, storedEndpoint.systemId,
                              storedEndpoint.componentId,
                              ParameterLoadState::Cancelled);
    emit endpointProgressChanged(storedEndpoint.linkId,
                                 storedEndpoint.systemId,
                                 storedEndpoint.componentId,
                                 current.received, current.reported,
                                 current.percent());
}

void ParameterStore::failLoad()
{
    failLoad(endpoint());
}

void ParameterStore::failLoad(const VehicleEndpoint &requestedEndpoint)
{
    VehicleEndpoint storedEndpoint;
    ParameterProgress current;
    bool selected = false;
    {
        QWriteLocker locker(&m_lock);
        auto it = m_caches.find(endpointKey(requestedEndpoint));
        if (it == m_caches.end() || !it->stagingActive) {
            return;
        }
        EndpointCache &cache = it.value();
        discardStagingLocked(&cache);
        cache.state = ParameterLoadState::Failed;
        storedEndpoint = cache.endpoint;
        current = visibleProgress(cache);
        selected = isSelectedLocked(storedEndpoint);
    }

    if (selected) {
        emit stateChanged(ParameterLoadState::Failed);
        emit progressChanged(current.received, current.reported,
                             current.percent());
    }
    emit endpointStateChanged(storedEndpoint.linkId, storedEndpoint.systemId,
                              storedEndpoint.componentId,
                              ParameterLoadState::Failed);
    emit endpointProgressChanged(storedEndpoint.linkId,
                                 storedEndpoint.systemId,
                                 storedEndpoint.componentId,
                                 current.received, current.reported,
                                 current.percent());
}

bool ParameterStore::ingest(quint8 componentId,
                            int parameterCount,
                            int parameterIndex,
                            const QString &name,
                            const QVariant &value,
                            ParameterType type)
{
    VehicleEndpoint source = endpoint();
    source.componentId = componentId;
    source.componentName.clear();
    return ingest(source, parameterCount, parameterIndex, name, value, type);
}

bool ParameterStore::ingest(const VehicleEndpoint &sourceEndpoint,
                            int parameterCount,
                            int parameterIndex,
                            const QString &name,
                            const QVariant &value,
                            ParameterType type)
{
    const bool sentinelCount = parameterCount == UnknownWireValue;
    const bool sentinelIndex = parameterIndex == UnknownWireValue;
    if (!endpointIsValid(sourceEndpoint) || name.isEmpty()
        || parameterCount <= 0 || parameterCount > UnknownWireValue
        || parameterIndex < 0 || parameterIndex > UnknownWireValue
        || (!sentinelCount && !sentinelIndex
            && parameterIndex >= parameterCount)
        || type == ParameterType::Unknown || !value.isValid()) {
        return false;
    }

    VehicleEndpoint storedEndpoint;
    ParameterProgress current;
    ParameterLoadState resultingState = ParameterLoadState::Idle;
    bool selected = false;
    bool recordChanged = false;
    bool stateWasChanged = false;

    {
        QWriteLocker locker(&m_lock);
        EndpointCache &cache = ensureCacheLocked(sourceEndpoint);
        storedEndpoint = cache.endpoint;
        selected = isSelectedLocked(storedEndpoint);

        QHash<ParameterKey, ParameterRecord> *records =
            cache.stagingActive ? &cache.stagingRecords
                                : &cache.committedRecords;
        QHash<quint8, int> *reportedByComponent =
            cache.stagingActive ? &cache.stagingReportedByComponent
                                : &cache.committedReportedByComponent;
        QSet<ParameterIndexKey> *receivedIndices =
            cache.stagingActive ? &cache.stagingReceivedIndices
                                : &cache.committedReceivedIndices;
        quint64 recordRevision = cache.stagingRevision;
        if (!cache.stagingActive) {
            if (cache.committedRevision == 0) {
                cache.committedRevision = ++m_revisionCounter;
            }
            recordRevision = cache.committedRevision;
        }

        const quint8 componentId = static_cast<quint8>(
            storedEndpoint.componentId);
        if (!sentinelCount) {
            (*reportedByComponent)[componentId] = qMax(
                reportedByComponent->value(componentId), parameterCount);
        }

        const ParameterIndexKey indexKey{componentId, parameterIndex};
        const bool duplicateIndex = !sentinelIndex
            && receivedIndices->contains(indexKey);
        // During a list transaction Mission Planner treats a repeated index
        // as the same slot and keeps the first record. Outside a transaction,
        // however, PARAM_VALUE is also the acknowledgement for PARAM_SET and
        // must update the committed value even though its index is familiar.
        if (!duplicateIndex || !cache.stagingActive) {
            if (!sentinelIndex) {
                receivedIndices->insert(indexKey);
            }
            const ParameterKey key{componentId, name};
            records->insert(key, ParameterRecord{
                key,
                value,
                type,
                parameterIndex,
                parameterCount,
                recordRevision,
                QDateTime::currentDateTimeUtc()
            });
            recordChanged = true;
        }

        current = progressFor(*reportedByComponent, *receivedIndices);
        const ParameterLoadState previousState = cache.state;
        if (cache.stagingActive && current.complete()) {
            commitStagingLocked(&cache);
            cache.state = ParameterLoadState::Complete;
            current = visibleProgress(cache);
        } else if (!cache.stagingActive) {
            cache.committedComplete = current.complete();
            if (current.complete()
                && (cache.state == ParameterLoadState::Idle
                    || cache.state == ParameterLoadState::Partial
                    || cache.state == ParameterLoadState::Complete)) {
                cache.state = ParameterLoadState::Complete;
            } else if (!current.complete()
                       && cache.state == ParameterLoadState::Complete) {
                cache.state = ParameterLoadState::Partial;
            }
        }
        resultingState = cache.state;
        stateWasChanged = previousState != cache.state;
    }

    if (recordChanged) {
        emit endpointParameterChanged(storedEndpoint.linkId,
                                      storedEndpoint.systemId,
                                      storedEndpoint.componentId, name);
        if (selected) {
            emit parameterChanged(storedEndpoint.componentId, name);
        }
    }
    emit endpointProgressChanged(storedEndpoint.linkId,
                                 storedEndpoint.systemId,
                                 storedEndpoint.componentId,
                                 current.received, current.reported,
                                 current.percent());
    if (selected) {
        emit progressChanged(current.received, current.reported,
                             current.percent());
    }
    if (stateWasChanged) {
        emit endpointStateChanged(storedEndpoint.linkId,
                                  storedEndpoint.systemId,
                                  storedEndpoint.componentId,
                                  resultingState);
        if (selected) {
            emit stateChanged(resultingState);
        }
    }
    return true;
}

bool ParameterStore::removeEndpoint(const VehicleEndpoint &requestedEndpoint)
{
    bool selected = false;
    bool removed = false;
    {
        QWriteLocker locker(&m_lock);
        const EndpointKey key = endpointKey(requestedEndpoint);
        selected = endpointKey(m_endpoint) == key;
        removed = m_caches.remove(key) > 0;
        if (!removed) {
            return false;
        }
        if (selected) {
            ++m_revisionCounter;
            m_endpoint = VehicleEndpoint{};
            m_target = VehicleTarget{-1, 0, 0, m_revisionCounter};
        }
    }

    if (selected) {
        emit targetChanged();
        emit selectedEndpointChanged(-1, 0, 0);
        emit stateChanged(ParameterLoadState::Idle);
        emit progressChanged(0, 0, 0);
    }
    return true;
}

void ParameterStore::clear()
{
    bool hadState = false;
    {
        QWriteLocker locker(&m_lock);
        hadState = !m_caches.isEmpty() || endpointIsValid(m_endpoint);
        m_caches.clear();
        ++m_revisionCounter;
        m_endpoint = VehicleEndpoint{};
        m_target = VehicleTarget{-1, 0, 0, m_revisionCounter};
    }
    if (hadState) {
        emit targetChanged();
        emit selectedEndpointChanged(-1, 0, 0);
        emit stateChanged(ParameterLoadState::Idle);
        emit progressChanged(0, 0, 0);
    }
}

bool ParameterStore::endpointIsValid(const VehicleEndpoint &value)
{
    // Kept local so apm_parameter_core remains a pure QtCore library. The
    // bounds intentionally mirror VehicleEndpoint::isValid().
    return value.linkId >= 0
        && value.systemId > 0 && value.systemId <= 255
        && value.componentId >= 0 && value.componentId <= 255;
}

ParameterStore::EndpointKey ParameterStore::endpointKey(
    const VehicleEndpoint &value)
{
    return EndpointKey{value.linkId, value.systemId, value.componentId};
}

ParameterProgress ParameterStore::progressFor(
    const QHash<quint8, int> &reportedByComponent,
    const QSet<ParameterIndexKey> &receivedIndices)
{
    ParameterProgress result;
    result.received = receivedIndices.size();
    for (auto it = reportedByComponent.constBegin();
         it != reportedByComponent.constEnd(); ++it) {
        result.reported += it.value();
    }
    return result;
}

ParameterProgress ParameterStore::visibleProgress(const EndpointCache &cache)
{
    if (cache.stagingActive) {
        return progressFor(cache.stagingReportedByComponent,
                           cache.stagingReceivedIndices);
    }
    return progressFor(cache.committedReportedByComponent,
                       cache.committedReceivedIndices);
}

void ParameterStore::mergeEndpointMetadata(VehicleEndpoint *stored,
                                           const VehicleEndpoint &incoming)
{
    if (!stored) {
        return;
    }
    stored->linkId = incoming.linkId;
    stored->systemId = incoming.systemId;
    stored->componentId = incoming.componentId;
    if (!incoming.linkName.trimmed().isEmpty()) {
        stored->linkName = incoming.linkName;
    }
    if (!incoming.componentName.trimmed().isEmpty()) {
        stored->componentName = incoming.componentName;
    }
}

ParameterStore::EndpointCache &ParameterStore::ensureCacheLocked(
    const VehicleEndpoint &requestedEndpoint)
{
    const EndpointKey key = endpointKey(requestedEndpoint);
    auto it = m_caches.find(key);
    if (it == m_caches.end()) {
        EndpointCache cache;
        cache.endpoint = requestedEndpoint;
        it = m_caches.insert(key, cache);
    } else {
        mergeEndpointMetadata(&it->endpoint, requestedEndpoint);
    }
    return it.value();
}

const ParameterStore::EndpointCache *ParameterStore::findCacheLocked(
    const VehicleEndpoint &requestedEndpoint) const
{
    if (!endpointIsValid(requestedEndpoint)) {
        return nullptr;
    }
    auto it = m_caches.constFind(endpointKey(requestedEndpoint));
    return it == m_caches.constEnd() ? nullptr : &it.value();
}

bool ParameterStore::isSelectedLocked(
    const VehicleEndpoint &requestedEndpoint) const
{
    return endpointKey(m_endpoint) == endpointKey(requestedEndpoint);
}

ParameterSnapshot ParameterStore::snapshotLocked(
    const VehicleEndpoint &requestedEndpoint,
    const EndpointCache *cache) const
{
    ParameterSnapshot result;
    const VehicleEndpoint resolvedEndpoint = cache
        ? cache->endpoint : requestedEndpoint;
    quint64 revision = cache
        ? qMax(cache->committedRevision, cache->stagingRevision) : 0;
    if (isSelectedLocked(resolvedEndpoint)) {
        revision = m_target.revision;
    }
    result.m_endpoint = resolvedEndpoint;
    result.m_target = VehicleTarget{
        resolvedEndpoint.linkId,
        static_cast<quint8>(resolvedEndpoint.systemId),
        static_cast<quint8>(resolvedEndpoint.componentId),
        revision
    };
    if (cache) {
        result.m_state = cache->state;
        result.m_progress = visibleProgress(*cache);
        result.m_records = cache->committedRecords;
        result.m_complete = cache->committedComplete;
    }
    return result;
}

void ParameterStore::commitStagingLocked(EndpointCache *cache)
{
    if (!cache || !cache->stagingActive) {
        return;
    }
    cache->committedRecords = cache->stagingRecords;
    cache->committedReportedByComponent = cache->stagingReportedByComponent;
    cache->committedReceivedIndices = cache->stagingReceivedIndices;
    cache->committedRevision = cache->stagingRevision;
    cache->committedComplete = progressFor(
        cache->committedReportedByComponent,
        cache->committedReceivedIndices).complete();
    discardStagingLocked(cache);
}

void ParameterStore::discardStagingLocked(EndpointCache *cache)
{
    if (!cache) {
        return;
    }
    cache->stagingRecords.clear();
    cache->stagingReportedByComponent.clear();
    cache->stagingReceivedIndices.clear();
    cache->stagingRevision = 0;
    cache->stagingActive = false;
}
