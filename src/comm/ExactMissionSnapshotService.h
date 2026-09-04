#ifndef EXACTMISSIONSNAPSHOTSERVICE_H
#define EXACTMISSIONSNAPSHOTSERVICE_H

#include "MissionProtocolCoordinator.h"
#include "MissionTransferService.h"
#include "SwarmTelemetryRegistry.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

#include <functional>

class ExactLinkTransmitter;

/** Exact vehicle-instance and MAVLink mission-type identity. */
struct ExactMissionKey
{
    SwarmVehicleInstanceLease vehicle;
    MAV_MISSION_TYPE missionType = MAV_MISSION_TYPE_MISSION;

    bool isValid() const noexcept
    {
        return vehicle.isValid();
    }

    bool operator==(const ExactMissionKey &other) const noexcept
    {
        return vehicle.sameInstance(other.vehicle)
            && missionType == other.missionType;
    }
};

inline bool operator!=(const ExactMissionKey &left,
                       const ExactMissionKey &right) noexcept
{
    return !(left == right);
}

inline uint qHash(const ExactMissionKey &key, uint seed = 0) noexcept
{
    seed = ::qHash(key.vehicle.endpoint, seed);
    seed = ::qHash(key.vehicle.linkSessionEpoch, seed);
    seed = ::qHash(key.vehicle.instanceEpoch, seed);
    return ::qHash(static_cast<quint8>(key.missionType), seed);
}

/**
 * Immutable-by-contract value published after one complete MISSION transfer.
 *
 * items retains the complete canonical MISSION_ITEM_INT sequence, including
 * item zero/Home. The service atomically replaces whole values and never
 * exposes its mutable cache storage.
 */
struct ExactMissionSnapshot
{
    ExactMissionKey key;
    QVector<mavlink_mission_item_int_t> items;
    quint64 contentGeneration = 0;
    quint64 observationRevision = 0;
    QByteArray contentDigest;
    QByteArray waypointLeaderSignature;
    qint64 confirmedAtMs = -1;

    bool isValid() const noexcept
    {
        return key.isValid() && contentGeneration != 0
            && observationRevision != 0 && confirmedAtMs >= 0
            && !contentDigest.isEmpty()
            && !waypointLeaderSignature.isEmpty();
    }
};

/** Pins one exact mission content generation for a long-running consumer. */
struct ExactMissionSnapshotLease
{
    ExactMissionKey key;
    quint64 contentGeneration = 0;
    QByteArray contentDigest;

    bool isValid() const noexcept
    {
        return key.isValid() && contentGeneration != 0
            && !contentDigest.isEmpty();
    }
};

struct ExactMissionTransferToken
{
    quint64 id = 0;

    bool isValid() const noexcept { return id != 0; }
};

struct ExactMissionTransferResult
{
    ExactMissionTransferToken token;
    ExactMissionKey key;
    MissionTransferService::State state = MissionTransferService::State::Idle;
    MAV_MISSION_RESULT missionResult = MAV_MISSION_ERROR;
    QString errorString;
    ExactMissionSnapshot snapshot;

    bool succeeded() const noexcept
    {
        return state == MissionTransferService::State::Complete
            && missionResult == MAV_MISSION_ACCEPTED
            && snapshot.isValid();
    }
};

/**
 * Application-owned exact-instance mission download and snapshot cache.
 *
 * All calls, dependencies and signal receivers which call back directly must
 * live on this object's QObject thread. Incoming packets are filtered by
 * physical link, link-session epoch, sysid, compid and vehicle-instance lease
 * before they reach MissionTransferService. Only one transaction is active so
 * the application has one unambiguous MAVLink mission-protocol owner.
 */
class ExactMissionSnapshotService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY revisionChanged)

public:
    enum class StartResult {
        Started,
        Busy,
        InvalidOwner,
        InvalidLease,
        UnsupportedMissionType,
        IncompatibleProtocolVersion,
        MissionCoordinatorUnavailable,
        MissionOwnerBusy,
        UnsafeRoute,
        TransportUnavailable,
        IdentifierExhausted
    };
    Q_ENUM(StartResult)

    enum class InvalidationReason {
        LinkSessionEnded,
        HeartbeatStale,
        NoLongerCommandCapable,
        BootTimeReset,
        KnownMissionMutation,
        RegistryUnavailable
    };
    Q_ENUM(InvalidationReason)

    using RouteValidator = std::function<bool(
        const SwarmVehicleInstanceLease &vehicle, QString *error)>;
    using CoordinatorResolver = std::function<MissionProtocolCoordinator *(
        const SwarmVehicleInstanceLease &vehicle)>;
    using Clock = std::function<qint64()>;

    explicit ExactMissionSnapshotService(
        SwarmTelemetryRegistry *registry,
        ExactLinkTransmitter *transmitter,
        RouteValidator routeValidator,
        CoordinatorResolver coordinatorResolver,
        int timeoutMs = 1500,
        int maxRetries = 3,
        QObject *parent = nullptr);
    ExactMissionSnapshotService(
        SwarmTelemetryRegistry *registry,
        ExactLinkTransmitter *transmitter,
        RouteValidator routeValidator,
        CoordinatorResolver coordinatorResolver,
        Clock clock,
        int timeoutMs = 1500,
        int maxRetries = 3,
        QObject *parent = nullptr);
    ~ExactMissionSnapshotService() override;

    void setLocalIdentity(quint8 systemId, quint8 componentId);

    StartResult requestDownload(
        QObject *owner,
        const SwarmVehicleInstanceLease &vehicle,
        MAV_MISSION_TYPE missionType,
        ExactMissionTransferToken *token,
        QString *error = nullptr);
    bool cancel(const ExactMissionTransferToken &token,
                const QString &reason = QString());

    bool acquireSnapshot(
        const SwarmVehicleInstanceLease &vehicle,
        MAV_MISSION_TYPE missionType,
        ExactMissionSnapshot *snapshot,
        ExactMissionSnapshotLease *lease = nullptr) const;
    bool validateSnapshotLease(
        const ExactMissionSnapshotLease &lease) const;
    QList<ExactMissionSnapshot> snapshots() const;
    bool invalidateForKnownMissionMutation(
        const SwarmVehicleInstanceLease &vehicle,
        MAV_MISSION_TYPE missionType);

    bool busy() const noexcept { return m_active.token.isValid(); }
    ExactMissionTransferToken activeToken() const noexcept
    {
        return m_active.token;
    }
    quint64 revision() const noexcept { return m_revision; }

    /** Deterministic public codecs used by mission consumers and tests. */
    static QByteArray contentDigestFor(
        const QVector<mavlink_mission_item_int_t> &items);
    static QByteArray waypointLeaderSignatureFor(
        const QVector<mavlink_mission_item_int_t> &items);

    /** Feed one packet together with the already-captured ingress session. */
    void observeMessage(int linkId, quint64 linkSessionEpoch,
                        const mavlink_message_t &message);

signals:
    void busyChanged(bool busy);
    void revisionChanged(qulonglong revision);
    void snapshotReplaced(ExactMissionSnapshot snapshot);
    void snapshotInvalidated(
        ExactMissionKey key,
        ExactMissionSnapshotService::InvalidationReason reason);
    void transferFinished(ExactMissionTransferResult result);

private slots:
    void timeout();

private:
    struct ActiveTransfer
    {
        ExactMissionTransferToken token;
        ExactMissionKey key;
        QPointer<QObject> owner;
        QMetaObject::Connection ownerDestroyed;
        QPointer<MissionProtocolCoordinator> coordinator;
        MissionProtocolCoordinator::LeaseToken coordinatorLease;
    };

    struct PendingSnapshotEvent
    {
        bool replaced = false;
        ExactMissionSnapshot snapshot;
        ExactMissionKey key;
        InvalidationReason reason = InvalidationReason::HeartbeatStale;
    };

    static bool supportedMissionType(MAV_MISSION_TYPE missionType) noexcept;
    static InvalidationReason invalidationReasonFor(
        SwarmTelemetryRegistry::RetirementReason reason) noexcept;
    static bool validCompletedItems(
        const QVector<mavlink_mission_item_int_t> &items,
        MAV_MISSION_TYPE missionType) noexcept;

    bool tokenIsCurrent(const ExactMissionTransferToken &token) const noexcept;
    bool exactLeaseIsCurrent(
        const SwarmVehicleInstanceLease &vehicle) const;
    bool validateRouteWithBarrier(
        const ExactMissionTransferToken &token,
        QString *error);
    bool sendOutbound(
        const ExactMissionTransferToken &token,
        const MissionTransferService::OutboundMessage &outbound,
        QString *error);
    bool applyTransition(
        const ExactMissionTransferToken &token,
        const MissionTransferService::Transition &transition);
    void failActive(const ExactMissionTransferToken &token,
                    const QString &reason,
                    MAV_MISSION_RESULT result = MAV_MISSION_ERROR);
    void finishActive(const ExactMissionTransferToken &token);
    ExactMissionSnapshot commitCompletedSnapshot(
        const ExactMissionKey &key,
        const QVector<mavlink_mission_item_int_t> &items);
    void invalidateVehicle(
        const SwarmVehicleInstanceLease &vehicle,
        InvalidationReason reason);
    bool invalidateKey(const ExactMissionKey &key,
                       InvalidationReason reason);
    void invalidateAll(InvalidationReason reason);
    void watchCoordinator(MissionProtocolCoordinator *coordinator);
    void publishSnapshotEvents(QList<PendingSnapshotEvent> events);
    void armTimeout(const ExactMissionTransferToken &token);
    void disarmTimeout();
    qint64 nowMs() const;
    quint64 nextContentGeneration();
    quint64 nextObservationRevision();
    void assertServiceThread() const;

    QPointer<SwarmTelemetryRegistry> m_registry;
    QPointer<ExactLinkTransmitter> m_transmitter;
    RouteValidator m_routeValidator;
    CoordinatorResolver m_coordinatorResolver;
    Clock m_clock;
    mutable QElapsedTimer m_monotonicClock;
    mutable qint64 m_lastNowMs = 0;
    MissionTransferService m_transfer;
    QTimer m_timeoutTimer;
    QMetaObject::Connection m_timeoutConnection;
    quint64 m_timerToken = 0;
    QHash<ExactMissionKey, ExactMissionSnapshot> m_snapshots;
    QHash<ExactMissionKey, MissionProtocolCoordinator *>
        m_snapshotCoordinators;
    struct CoordinatorWatch
    {
        QPointer<MissionProtocolCoordinator> coordinator;
        QMetaObject::Connection leaseChanged;
        QMetaObject::Connection destroyed;
    };
    QList<CoordinatorWatch> m_coordinatorWatches;
    ActiveTransfer m_active;
    quint64 m_nextTransferId = 0;
    quint64 m_nextContentGeneration = 0;
    quint64 m_revision = 0;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    bool m_operationInFlight = false;
};

Q_DECLARE_METATYPE(ExactMissionKey)
Q_DECLARE_METATYPE(ExactMissionSnapshot)
Q_DECLARE_METATYPE(ExactMissionSnapshotLease)
Q_DECLARE_METATYPE(ExactMissionTransferToken)
Q_DECLARE_METATYPE(ExactMissionTransferResult)
Q_DECLARE_METATYPE(ExactMissionSnapshotService::StartResult)
Q_DECLARE_METATYPE(ExactMissionSnapshotService::InvalidationReason)

#endif // EXACTMISSIONSNAPSHOTSERVICE_H
