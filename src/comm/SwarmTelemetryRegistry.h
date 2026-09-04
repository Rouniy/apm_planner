#ifndef SWARMTELEMETRYREGISTRY_H
#define SWARMTELEMETRYREGISTRY_H

#include "VehicleEndpoint.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QTimer>

#include <functional>

#include <mavlink.h>

/**
 * Immutable identity of one autopilot instance within one physical-link
 * session.  Reusing a link id, or rediscovering an endpoint after its heartbeat
 * expires, creates a different token even when sysid/compid are unchanged.
 */
struct SwarmVehicleInstanceLease
{
    VehicleEndpoint endpoint;
    quint64 linkSessionEpoch = 0;
    quint64 instanceEpoch = 0;

    bool isValid() const noexcept
    {
        return endpoint.isValid() && linkSessionEpoch != 0
            && instanceEpoch != 0;
    }

    bool sameInstance(const SwarmVehicleInstanceLease &other) const noexcept
    {
        return endpoint.sameIdentity(other.endpoint)
            && linkSessionEpoch == other.linkSessionEpoch
            && instanceEpoch == other.instanceEpoch;
    }
};

inline bool operator==(const SwarmVehicleInstanceLease &left,
                       const SwarmVehicleInstanceLease &right) noexcept
{
    return left.sameInstance(right);
}

inline bool operator!=(const SwarmVehicleInstanceLease &left,
                       const SwarmVehicleInstanceLease &right) noexcept
{
    return !(left == right);
}

/** Value snapshot pinned to a SwarmVehicleInstanceLease. */
struct SwarmTelemetrySnapshot
{
    SwarmVehicleInstanceLease lease;
    qint64 lastMessageMs = -1;
    qint64 heartbeatObservedMs = -1;

    bool heartbeatValid = false;
    bool armed = false;
    int autopilot = MAV_AUTOPILOT_INVALID;
    int vehicleType = MAV_TYPE_GENERIC;
    quint8 baseMode = 0;
    quint32 customMode = 0;
    int systemStatus = MAV_STATE_UNINIT;

    bool positionValid = false;
    qint64 positionObservedMs = -1;
    double latitudeDegrees = 0.0;
    double longitudeDegrees = 0.0;
    double altitudeAmslM = 0.0;
    double relativeAltitudeM = 0.0;
    bool velocityValid = false;
    double velocityNorthMps = 0.0;
    double velocityEastMps = 0.0;
    double velocityDownMps = 0.0;
    bool headingValid = false;
    double headingDegrees = 0.0;

    bool vfrHudValid = false;
    qint64 vfrHudObservedMs = -1;
    double airspeedMps = 0.0;
    double groundSpeedMps = 0.0;
    double vfrAltitudeAmslM = 0.0;
    double climbMps = 0.0;
    int throttlePercent = 0;

    bool attitudeValid = false;
    qint64 attitudeObservedMs = -1;
    double rollRadians = 0.0;
    double pitchRadians = 0.0;
    double yawRadians = 0.0;
    double rollRateRadiansPerSecond = 0.0;
    double pitchRateRadiansPerSecond = 0.0;
    double yawRateRadiansPerSecond = 0.0;

    bool extendedSystemStateValid = false;
    qint64 extendedSystemStateObservedMs = -1;
    int vtolState = MAV_VTOL_STATE_UNDEFINED;
    int landedState = MAV_LANDED_STATE_UNDEFINED;
};

/** A group of exact instances captured for one all-or-nothing validation. */
struct SwarmVehicleGroupLease
{
    QList<SwarmVehicleInstanceLease> members;
    qint64 acquiredAtMs = -1;

    bool isValid() const noexcept { return !members.isEmpty(); }
};

/**
 * Exact-endpoint telemetry cache for multi-vehicle command preparation.
 *
 * The registry is intentionally independent of the application's selected
 * vehicle.  It accepts telemetry only from an explicitly active link-session
 * epoch and admits an endpoint only after a command-capable autopilot
 * HEARTBEAT.  All methods and signals run on the registry's QObject thread.
 */
class SwarmTelemetryRegistry final : public QObject
{
    Q_OBJECT

public:
    using Clock = std::function<qint64()>;

    enum class RetirementReason {
        LinkSessionEnded,
        HeartbeatStale,
        NoLongerCommandCapable,
        BootTimeReset
    };
    Q_ENUM(RetirementReason)

    static constexpr int MaximumVehicleEndpoints = 24;
    static constexpr int DefaultHeartbeatMaximumAgeMs = 5000;

    explicit SwarmTelemetryRegistry(QObject *parent = nullptr);
    explicit SwarmTelemetryRegistry(Clock clock, QObject *parent = nullptr);

    quint64 beginLinkSession(int linkId,
                             const QString &linkName = QString());
    bool endLinkSession(int linkId, quint64 expectedSessionEpoch);
    quint64 currentLinkSessionEpoch(int linkId) const;

    /**
     * Returns true only when a supported message updated an admitted endpoint.
     * Unknown endpoints can be created exclusively by a qualifying HEARTBEAT.
     */
    bool observeMessage(int linkId, quint64 linkSessionEpoch,
                        const mavlink_message_t &message);

    QList<VehicleEndpoint> endpoints() const;
    int endpointCount() const noexcept { return m_vehicles.size(); }
    quint64 revision() const noexcept { return m_revision; }
    int heartbeatMaximumAgeMs() const noexcept
    {
        return m_heartbeatMaximumAgeMs;
    }

    SwarmVehicleInstanceLease acquireVehicle(
        const VehicleEndpoint &endpoint,
        int heartbeatMaximumAgeMs = -1) const;
    bool acquireSnapshot(
        const VehicleEndpoint &endpoint, SwarmTelemetrySnapshot *snapshot,
        int heartbeatMaximumAgeMs = -1) const;
    bool snapshotForLease(const SwarmVehicleInstanceLease &lease,
                          SwarmTelemetrySnapshot *snapshot) const;
    bool validateLease(
        const SwarmVehicleInstanceLease &lease,
        int heartbeatMaximumAgeMs = -1) const;

    SwarmVehicleGroupLease acquireGroup(
        const QList<VehicleEndpoint> &endpoints,
        int heartbeatMaximumAgeMs = -1) const;
    bool validateGroup(
        const SwarmVehicleGroupLease &group,
        QList<SwarmTelemetrySnapshot> *snapshots = nullptr,
        int heartbeatMaximumAgeMs = -1) const;

    /**
     * Compares an observation timestamp with the registry's injected
     * monotonic clock. Consumers must not mix these values with a separately
     * started QElapsedTimer.
     */
    bool observationIsFresh(qint64 observedMs, int maximumAgeMs) const;
    qint64 observationClockNowMs() const { return nowMs(); }

    /** Retires endpoints whose most recent HEARTBEAT is older than the limit. */
    int retireStaleEndpoints();

    /** Deterministic test seam; production uses the fixed MP10-compatible age. */
    void setHeartbeatMaximumAgeForTesting(int maximumAgeMs);

signals:
    void linkSessionBegan(int linkId, qulonglong sessionEpoch);
    void linkSessionEnded(int linkId, qulonglong sessionEpoch);
    void endpointActivated(SwarmTelemetrySnapshot snapshot);
    void endpointUpdated(SwarmTelemetrySnapshot snapshot);
    void endpointRetired(SwarmVehicleInstanceLease lease,
                         SwarmTelemetryRegistry::RetirementReason reason);
    void registryChanged(qulonglong revision);

private:
    struct LinkSession
    {
        quint64 epoch = 0;
        QString name;
    };

    struct VehicleRecord
    {
        SwarmTelemetrySnapshot snapshot;
        bool bootTimeValid = false;
        quint32 latestBootTimeMs = 0;
    };

    enum class BootTimeObservation {
        NotPresent,
        Accepted,
        Reset
    };

    enum class PendingEventType {
        LinkBegan,
        LinkEnded,
        Activated,
        Updated,
        Retired,
        RegistryChanged
    };

    struct PendingEvent
    {
        PendingEventType type = PendingEventType::RegistryChanged;
        int linkId = -1;
        quint64 epoch = 0;
        SwarmTelemetrySnapshot snapshot;
        SwarmVehicleInstanceLease lease;
        RetirementReason retirementReason =
            RetirementReason::HeartbeatStale;
        quint64 revision = 0;
    };

    static bool isParsedMessage(quint32 messageId) noexcept;
    static bool isCommandCapableHeartbeat(
        const mavlink_message_t &message,
        const mavlink_heartbeat_t &heartbeat) noexcept;
    static bool validPosition(qint32 latitudeE7, qint32 longitudeE7) noexcept;
    static bool messageBootTimeMs(const mavlink_message_t &message,
                                  quint32 *bootTimeMs) noexcept;

    VehicleEndpoint endpointFor(int linkId, const LinkSession &session,
                                const mavlink_message_t &message) const;
    bool leaseMatchesRecord(const SwarmVehicleInstanceLease &lease,
                            const VehicleRecord &record) const noexcept;
    bool heartbeatIsFresh(const SwarmTelemetrySnapshot &snapshot,
                          qint64 now, int maximumAgeMs) const noexcept;
    void updateHeartbeat(SwarmTelemetrySnapshot *snapshot,
                         const mavlink_heartbeat_t &heartbeat,
                         qint64 observedMs) const;
    void updateMessage(SwarmTelemetrySnapshot *snapshot,
                       const mavlink_message_t &message,
                       qint64 observedMs) const;
    BootTimeObservation observeBootTime(VehicleRecord *record,
                                        const mavlink_message_t &message);
    int retireStaleAt(qint64 now, QList<PendingEvent> *events);
    bool retireEndpoint(const VehicleEndpoint &endpoint,
                        RetirementReason reason,
                        QList<PendingEvent> *events);
    void publishMutation(QList<PendingEvent> events);
    void drainPendingEvents();
    qint64 nowMs() const;
    quint64 nextLinkSessionEpoch();
    quint64 nextInstanceEpoch();
    void updateRetirementTimer();

    Clock m_clock;
    mutable QElapsedTimer m_monotonicClock;
    mutable qint64 m_lastNowMs = 0;
    QTimer m_retirementTimer;
    QHash<int, LinkSession> m_linkSessions;
    QHash<VehicleEndpoint, VehicleRecord> m_vehicles;
    QQueue<PendingEvent> m_pendingEvents;
    quint64 m_revision = 0;
    quint64 m_nextLinkEpoch = 0;
    quint64 m_nextVehicleInstanceEpoch = 0;
    int m_heartbeatMaximumAgeMs = DefaultHeartbeatMaximumAgeMs;
    bool m_drainingEvents = false;
};

Q_DECLARE_METATYPE(SwarmVehicleInstanceLease)
Q_DECLARE_METATYPE(SwarmTelemetrySnapshot)
Q_DECLARE_METATYPE(SwarmVehicleGroupLease)
Q_DECLARE_METATYPE(SwarmTelemetryRegistry::RetirementReason)

#endif // SWARMTELEMETRYREGISTRY_H
