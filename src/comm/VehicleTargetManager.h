#ifndef VEHICLETARGETMANAGER_H
#define VEHICLETARGETMANAGER_H

#include "VehicleEndpoint.h"

#include <QList>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

/**
 * Registry and selection state for exact MAVLink transport targets.
 *
 * This class does not yet reroute legacy UAS commands.  It establishes the
 * truthful address/lifetime contract first, so UI and QML consumers cannot
 * collapse equal sysids seen on different links.
 */
class VehicleTargetManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList endpoints READ endpointVariants
               NOTIFY endpointsChanged)
    Q_PROPERTY(QVariantMap currentTarget READ currentTargetVariant
               NOTIFY currentTargetChanged)
    Q_PROPERTY(bool hasCurrentTarget READ hasCurrentTarget
               NOTIFY currentTargetChanged)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(qulonglong targetGeneration READ targetGeneration
               NOTIFY targetGenerationChanged)

public:
    explicit VehicleTargetManager(QObject *parent = nullptr);

    static bool isVisibleDiscoveryMessage(quint32 messageId) noexcept;

    QList<VehicleEndpoint> endpoints() const { return m_endpoints; }
    QVariantList endpointVariants() const;
    VehicleTargetLease acquireTarget() const;
    QVariantMap currentTargetVariant() const;
    bool hasCurrentTarget() const { return m_currentIdentity.isValid(); }
    quint64 revision() const { return m_revision; }
    quint64 targetGeneration() const { return m_targetGeneration; }
    bool isTargetGenerationSettled() const
    {
        return m_targetNotificationDepth == 0
            && m_lastNotifiedTargetGeneration == m_targetGeneration;
    }

    bool contains(int linkId, int systemId, int componentId) const;
    bool observeEndpoint(const VehicleEndpoint &endpoint,
                         bool selectIfUnset = false);
    void observeHeartbeat(const VehicleEndpoint &endpoint, bool armed,
                          int autopilot, int vehicleType);
    bool hasFreshHeartbeat(const VehicleTargetLease &lease,
                           int maximumAgeMs) const;
    bool heartbeatArmed(const VehicleTargetLease &lease) const;
    int heartbeatAutopilot(const VehicleTargetLease &lease) const;
    int heartbeatVehicleType(const VehicleTargetLease &lease) const;
    bool removeLink(int linkId);
    void clear();

    Q_INVOKABLE bool selectTarget(int linkId, int systemId, int componentId);
    Q_INVOKABLE bool selectTargetIfGeneration(
        int linkId, int systemId, int componentId,
        qulonglong expectedGeneration);
    Q_INVOKABLE void clearTarget();
    Q_INVOKABLE bool isCurrentTarget(
        int linkId, int systemId, int componentId,
        qulonglong generation) const;

signals:
    void endpointAdded(int linkId, int systemId, int componentId);
    void endpointUpdated(int linkId, int systemId, int componentId);
    void endpointRemoved(int linkId, int systemId, int componentId);
    void endpointsReset();
    void endpointsChanged();
    void currentTargetChanged();
    void revisionChanged();
    void targetGenerationChanged(qulonglong generation);
    void targetGenerationSettled(qulonglong generation);

private:
    int indexOf(int linkId, int systemId, int componentId) const;
    void setCurrentIdentity(const VehicleEndpoint &endpoint);
    void invalidateCurrentTarget();
    void notifyTargetChanged();

    QList<VehicleEndpoint> m_endpoints;
    struct HeartbeatSnapshot {
        qint64 observedMs = 0;
        quint64 targetGeneration = 0;
        bool armed = false;
        int autopilot = -1;
        int vehicleType = -1;
    };
    QHash<VehicleEndpoint, HeartbeatSnapshot> m_heartbeats;
    QElapsedTimer m_monotonicClock;
    VehicleEndpoint m_currentIdentity;
    quint64 m_revision = 0;
    quint64 m_targetGeneration = 0;
    quint64 m_lastNotifiedTargetGeneration = 0;
    int m_targetNotificationDepth = 0;
};

#endif // VEHICLETARGETMANAGER_H
