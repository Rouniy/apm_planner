#ifndef MAVLINKCOMPONENTREGISTRY_H
#define MAVLINKCOMPONENTREGISTRY_H

#include "MavlinkComponentInstanceLease.h"
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <functional>
#include <mavlink.h>

// Generic peripheral discovery, independent of both active UAS and Swarm.
// Activity proves only presence, never flight-command capability.
class MavlinkComponentRegistry final : public QObject
{
    Q_OBJECT
public:
    using Clock = std::function<qint64()>;
    static constexpr int MaximumComponents = 64;
    static constexpr qint64 StaleAfterMs = 10000;
    explicit MavlinkComponentRegistry(QObject *parent = nullptr, Clock clock = {});
    void beginLinkSession(int linkId, quint64 epoch);
    void endLinkSession(int linkId, quint64 epoch);
    void observeMessage(int linkId, quint64 epoch, const mavlink_message_t &message);
    void expireStale();
    QList<MavlinkComponentInstanceLease> components() const;
    bool validateLease(const MavlinkComponentInstanceLease &lease) const;
signals:
    void componentsChanged();
    void componentRetired(MavlinkComponentInstanceLease lease);
private:
    struct Entry {
        MavlinkComponentInstanceLease lease;
        qint64 lastSeenMs = 0;
        bool heartbeatKnown = false;
        quint8 type = 0;
        quint8 autopilot = 0;
    };
    qint64 now() const;
    QElapsedTimer m_elapsed;
    Clock m_clock;
    QTimer m_timer;
    QHash<int, quint64> m_sessions;
    QHash<VehicleEndpoint, Entry> m_entries;
    quint64 m_nextInstance = 0;
    QHash<int, quint64> m_sessionRevisions;
};
#endif
