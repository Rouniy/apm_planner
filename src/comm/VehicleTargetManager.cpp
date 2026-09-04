#include "VehicleTargetManager.h"

#include <algorithm>

VehicleTargetManager::VehicleTargetManager(QObject *parent)
    : QObject(parent)
{
    m_monotonicClock.start();
    qRegisterMetaType<VehicleEndpoint>("VehicleEndpoint");
    qRegisterMetaType<VehicleTargetLease>("VehicleTargetLease");
}

void VehicleTargetManager::observeHeartbeat(
    const VehicleEndpoint &endpoint, bool armed,
    int autopilot, int vehicleType)
{
    if (endpoint.isValid()) {
        m_heartbeats.insert(endpoint,
                            HeartbeatSnapshot{m_monotonicClock.elapsed(), armed,
                                              autopilot, vehicleType});
    }
}

bool VehicleTargetManager::hasFreshHeartbeat(
    const VehicleTargetLease &lease, int maximumAgeMs) const
{
    if (!lease.isValid() || maximumAgeMs < 1) {
        return false;
    }
    const auto observed = m_heartbeats.constFind(lease.endpoint);
    return observed != m_heartbeats.constEnd()
        && m_monotonicClock.elapsed() - observed->observedMs <= maximumAgeMs;
}

bool VehicleTargetManager::heartbeatArmed(
    const VehicleTargetLease &lease) const
{
    const auto observed = m_heartbeats.constFind(lease.endpoint);
    return lease.isValid() && observed != m_heartbeats.constEnd()
        && observed->armed;
}

int VehicleTargetManager::heartbeatAutopilot(
    const VehicleTargetLease &lease) const
{
    const auto observed = m_heartbeats.constFind(lease.endpoint);
    return lease.isValid() && observed != m_heartbeats.constEnd()
        ? observed->autopilot : -1;
}

int VehicleTargetManager::heartbeatVehicleType(
    const VehicleTargetLease &lease) const
{
    const auto observed = m_heartbeats.constFind(lease.endpoint);
    return lease.isValid() && observed != m_heartbeats.constEnd()
        ? observed->vehicleType : -1;
}

bool VehicleTargetManager::isVisibleDiscoveryMessage(
    quint32 messageId) noexcept
{
    // Mission Planner MAVList promotes only HEARTBEAT, HIGH_LATENCY2 and
    // UAVCAN_NODE_STATUS sources from its hidden lookup cache to masterlist.
    return messageId == 0U || messageId == 235U || messageId == 310U;
}

QVariantList VehicleTargetManager::endpointVariants() const
{
    QVariantList result;
    result.reserve(m_endpoints.size());
    for (const VehicleEndpoint &endpoint : m_endpoints) {
        result.append(endpoint.toVariantMap());
    }
    return result;
}

VehicleTargetLease VehicleTargetManager::acquireTarget() const
{
    VehicleTargetLease lease;
    lease.generation = m_targetGeneration;
    const int index = indexOf(m_currentIdentity.linkId,
                              m_currentIdentity.systemId,
                              m_currentIdentity.componentId);
    if (index >= 0) {
        lease.endpoint = m_endpoints.at(index);
    }
    return lease;
}

QVariantMap VehicleTargetManager::currentTargetVariant() const
{
    const VehicleTargetLease lease = acquireTarget();
    QVariantMap result = lease.isValid()
        ? lease.endpoint.toVariantMap()
        : QVariantMap{{QStringLiteral("valid"), false}};
    result.insert(QStringLiteral("generation"),
                  QVariant::fromValue<qulonglong>(m_targetGeneration));
    return result;
}

bool VehicleTargetManager::contains(int linkId, int systemId,
                                    int componentId) const
{
    return indexOf(linkId, systemId, componentId) >= 0;
}

bool VehicleTargetManager::observeEndpoint(const VehicleEndpoint &value,
                                           bool selectIfUnset)
{
    VehicleEndpoint endpoint = value;
    endpoint.linkName = endpoint.linkName.trimmed();
    endpoint.componentName = endpoint.componentName.trimmed();
    if (!endpoint.isValid()) {
        return false;
    }

    const int existingIndex = indexOf(endpoint.linkId, endpoint.systemId,
                                      endpoint.componentId);
    const bool added = existingIndex < 0;
    bool currentMetadataChanged = false;
    if (existingIndex >= 0) {
        if (m_endpoints.at(existingIndex).metadataEquals(endpoint)) {
            return false;
        }
        currentMetadataChanged = m_currentIdentity.sameIdentity(endpoint);
        m_endpoints[existingIndex] = endpoint;
    } else {
        m_endpoints.append(endpoint);
        std::sort(m_endpoints.begin(), m_endpoints.end());
    }

    const bool selectedNow = selectIfUnset && !hasCurrentTarget();
    if (selectedNow) {
        m_currentIdentity = endpoint;
        ++m_targetGeneration;
    }
    ++m_revision;
    emit revisionChanged();
    if (added) {
        emit endpointAdded(endpoint.linkId, endpoint.systemId,
                           endpoint.componentId);
    } else {
        emit endpointUpdated(endpoint.linkId, endpoint.systemId,
                             endpoint.componentId);
    }
    emit endpointsChanged();

    if (currentMetadataChanged
        && m_currentIdentity.sameIdentity(endpoint)) {
        emit currentTargetChanged();
    } else if (selectedNow) {
        notifyTargetChanged();
    }
    return true;
}

bool VehicleTargetManager::removeLink(int linkId)
{
    bool selectedRemoved = false;
    bool anyRemoved = false;
    QList<VehicleEndpoint> removed;
    for (int index = m_endpoints.size() - 1; index >= 0; --index) {
        if (m_endpoints.at(index).linkId != linkId) {
            continue;
        }
        selectedRemoved = selectedRemoved
            || m_currentIdentity.sameIdentity(m_endpoints.at(index));
        removed.prepend(m_endpoints.at(index));
        m_heartbeats.remove(m_endpoints.at(index));
        m_endpoints.removeAt(index);
        anyRemoved = true;
    }
    if (!anyRemoved) {
        return false;
    }

    if (selectedRemoved) {
        m_currentIdentity = VehicleEndpoint();
        ++m_targetGeneration;
    }
    ++m_revision;
    emit revisionChanged();
    for (const VehicleEndpoint &endpoint : removed) {
        emit endpointRemoved(endpoint.linkId, endpoint.systemId,
                             endpoint.componentId);
    }
    emit endpointsChanged();
    if (selectedRemoved) {
        notifyTargetChanged();
    }
    return true;
}

void VehicleTargetManager::clear()
{
    const bool hadCurrentTarget = hasCurrentTarget();
    if (hadCurrentTarget) {
        m_currentIdentity = VehicleEndpoint();
        ++m_targetGeneration;
    }
    if (!m_endpoints.isEmpty()) {
        m_endpoints.clear();
        m_heartbeats.clear();
        ++m_revision;
        emit revisionChanged();
        emit endpointsReset();
        emit endpointsChanged();
    }
    if (hadCurrentTarget) {
        notifyTargetChanged();
    }
}

bool VehicleTargetManager::selectTarget(int linkId, int systemId,
                                        int componentId)
{
    const int index = indexOf(linkId, systemId, componentId);
    if (index < 0) {
        return false;
    }
    if (!m_currentIdentity.sameIdentity(m_endpoints.at(index))) {
        setCurrentIdentity(m_endpoints.at(index));
    }
    return true;
}

bool VehicleTargetManager::selectTargetIfGeneration(
    int linkId, int systemId, int componentId,
    qulonglong expectedGeneration)
{
    return expectedGeneration == m_targetGeneration
        && selectTarget(linkId, systemId, componentId);
}

void VehicleTargetManager::clearTarget()
{
    invalidateCurrentTarget();
}

bool VehicleTargetManager::isCurrentTarget(
    int linkId, int systemId, int componentId,
    qulonglong generation) const
{
    return generation == m_targetGeneration
        && m_currentIdentity.linkId == linkId
        && m_currentIdentity.systemId == systemId
        && m_currentIdentity.componentId == componentId
        && contains(linkId, systemId, componentId);
}

int VehicleTargetManager::indexOf(int linkId, int systemId,
                                  int componentId) const
{
    for (int index = 0; index < m_endpoints.size(); ++index) {
        const VehicleEndpoint &endpoint = m_endpoints.at(index);
        if (endpoint.linkId == linkId && endpoint.systemId == systemId
            && endpoint.componentId == componentId) {
            return index;
        }
    }
    return -1;
}

void VehicleTargetManager::setCurrentIdentity(
    const VehicleEndpoint &endpoint)
{
    m_currentIdentity = endpoint;
    ++m_targetGeneration;
    notifyTargetChanged();
}

void VehicleTargetManager::invalidateCurrentTarget()
{
    if (!m_currentIdentity.isValid()) {
        return;
    }
    m_currentIdentity = VehicleEndpoint();
    ++m_targetGeneration;
    notifyTargetChanged();
}

void VehicleTargetManager::notifyTargetChanged()
{
    // Endpoint and target observers are allowed to make nested selections.
    // The outermost call serializes every resulting generation and emits one
    // settled notification for the final state.
    if (m_targetNotificationDepth > 0
        || m_lastNotifiedTargetGeneration == m_targetGeneration) {
        return;
    }
    ++m_targetNotificationDepth;
    while (m_lastNotifiedTargetGeneration != m_targetGeneration) {
        const quint64 announcedGeneration = m_targetGeneration;
        m_lastNotifiedTargetGeneration = announcedGeneration;
        emit targetGenerationChanged(announcedGeneration);
        // A targetGenerationChanged listener may select another endpoint.
        // Announce currentTarget only for the generation this iteration still
        // represents; the loop will publish any nested generation next.
        if (announcedGeneration == m_targetGeneration) {
            emit currentTargetChanged();
        }
    }
    --m_targetNotificationDepth;
    // Consumers may queue work from targetGenerationChanged. The settled
    // phase runs only after every invalidation/current-target listener,
    // including nested selections, has observed the final generation.
    emit targetGenerationSettled(m_targetGeneration);
}
