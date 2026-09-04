#ifndef SWARMWAYPOINTLEADERCORE_H
#define SWARMWAYPOINTLEADERCORE_H

#include "SwarmFollowPathCore.h"

#include <QByteArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

enum class SwarmWaypointLeaderMode
{
    Idle,
    Takeoff,
    FlyToGroundMaster,
    FollowGroundMaster,
    ReturnAlongMission,
    LandAltitude,
    Landing
};

struct SwarmWaypointLeaderSettings
{
    double separationM = 5.0;
    double leadM = 20.0;
    double offPathTriggerM = 10.0;
    double takeoffLandAltitudeSeparationM = 2.0;
    double navigationAccelerationMps2 = 1.0;
    bool vFormation = false;
    bool altitudeInterleave = false;
};

struct SwarmWaypointLeaderFollower
{
    SwarmVehicleInstanceLease lease;
    int order = 0;
};

/**
 * One immutable item from the air master's downloaded mission.
 *
 * `sequence` is the MAVState dictionary key used by MP10.  Coordinates retain
 * their wire representation so mission confirmation signatures do not depend
 * on a lossy presentation conversion.  Mission-path construction accepts only
 * GLOBAL_RELATIVE_ALT/INT navigation destinations.  An unsupported-frame
 * sequence-zero Home may contribute its horizontal coordinate only.
 */
struct SwarmWaypointLeaderMissionItem
{
    int sequence = 0;
    quint16 command = 0;
    int frame = 0;
    qint32 latitudeE7 = 0;
    qint32 longitudeE7 = 0;
    float relativeAltitudeM = 0.0F;
};

/** Mission data pinned to the exact air-master instance that supplied it. */
struct SwarmWaypointLeaderMissionSnapshot
{
    SwarmVehicleInstanceLease airMaster;
    MAV_MISSION_TYPE missionType = MAV_MISSION_TYPE_MISSION;
    quint64 contentGeneration = 0;
    QByteArray contentDigest;
    QVector<SwarmWaypointLeaderMissionItem> items;
};

struct SwarmWaypointLeaderProfilePoint
{
    double distanceM = 0.0;
    double relativeAltitudeM = 0.0;
};

/**
 * Compact MP10 WaypointLeader path.
 *
 * The original controller materializes every usable leg at 0.1 m.  The MP10
 * service preserves the same linear horizontal/altitude geometry as vertices;
 * this Qt value object follows that bounded representation.
 */
class SwarmWaypointLeaderMissionPath final
{
public:
    static constexpr double MaximumMissionSegmentM = 5000.0;

    static bool build(const SwarmWaypointLeaderMissionSnapshot &mission,
                      SwarmWaypointLeaderMissionPath *path,
                      QString *error = nullptr);
    static QString signatureOf(
        const QVector<SwarmWaypointLeaderMissionItem> &items);

    bool isValid() const noexcept { return m_vertices.size() >= 2; }
    QString signature() const { return m_signature; }
    double lengthM() const noexcept { return m_lengthM; }
    QVector<SwarmWaypointLeaderProfilePoint> profile() const;
    SwarmFollowPathPoint start() const;
    SwarmFollowPathPoint end() const;

    bool closest(const SwarmFollowPathPoint &location,
                 double *distanceAlongM,
                 double *distanceFromPathM) const;
    bool pointAt(double distanceM, SwarmFollowPathPoint *point) const;
    bool lineTargets(const SwarmFollowPathPoint &reference,
                     double leadM,
                     double separationM,
                     int count,
                     QVector<SwarmFollowPathPoint> *targets) const;
    bool vTargets(const SwarmFollowPathPoint &reference,
                  double leadM,
                  double separationM,
                  int count,
                  QVector<SwarmFollowPathPoint> *targets) const;

private:
    struct Vertex
    {
        double distanceM = 0.0;
        SwarmFollowPathPoint point;
    };

    QVector<Vertex> m_vertices;
    QString m_signature;
    double m_lengthM = 0.0;
};

struct SwarmWaypointLeaderPlan
{
    SwarmVehicleInstanceLease groundMaster;
    SwarmVehicleInstanceLease airMaster;
    QVector<SwarmWaypointLeaderFollower> followers;
    SwarmWaypointLeaderSettings settings;
    QString missionSignature;
    quint64 missionContentGeneration = 0;
    QByteArray missionContentDigest;
};

/** Telemetry and parameter-name capability from one exact group snapshot. */
struct SwarmWaypointLeaderVehicleState
{
    SwarmTelemetrySnapshot telemetry;
    QSet<QString> availableParameters;
};

struct SwarmWaypointLeaderVelocity
{
    double northMps = 0.0;
    double eastMps = 0.0;
    double downMps = 0.0;
};

/**
 * One deterministic external action.  The integration layer owns wire
 * serialization and ACK correlation; the core never calls transport APIs.
 */
struct SwarmWaypointLeaderCommandIntent
{
    enum class Kind
    {
        RequestPositionStream,
        SetParameter,
        SetModeGuided,
        Arm,
        Takeoff,
        PositionTarget,
        SetModeRtl
    };

    static constexpr quint16 PositionVelocityTypeMask = 0x0DC0;

    Kind kind = Kind::PositionTarget;
    SwarmVehicleInstanceLease lease;
    QString role;
    int order = 0;

    int streamRateHz = 0;
    QString parameterName;
    double parameterValue = 0.0;
    bool arm = true;
    double takeoffAltitudeM = 0.0;
    SwarmFollowPathPoint target;
    SwarmWaypointLeaderVelocity velocity;

    bool requiresVehicleAcknowledgement() const noexcept;
};

enum class SwarmWaypointLeaderBatchResult
{
    Succeeded,
    Rejected,
    Partial,
    Cancelled,
    OutcomeUncertain
};

struct SwarmWaypointLeaderTick
{
    enum class State
    {
        Active,
        WaitingForCommandCompletion,
        Cancelling,
        Stopped,
        Completed
    };

    State state = State::Stopped;
    SwarmWaypointLeaderMode mode = SwarmWaypointLeaderMode::Idle;
    QString status;
    quint64 batchId = 0;
    quint64 cancelBatchId = 0;
    bool urgent = false;
    // An urgent PositionTarget may accompany State::Cancelling. In that case
    // batchId/cancelBatchId both identify the pre-existing normal batch whose
    // terminal cancellation must still be reported through completeBatch();
    // dispatching the out-of-band urgent intents does not complete that batch.
    // Order is authoritative (for example GUIDED -> ARM -> TAKEOFF for each
    // vehicle).  The integration must stop the sequence on a terminal
    // failure and report one final result through completeBatch().
    QVector<SwarmWaypointLeaderCommandIntent> intents;

    bool shouldContinue() const noexcept
    {
        return state == State::Active
            || state == State::WaitingForCommandCompletion
            || state == State::Cancelling;
    }
    bool hasIntents() const noexcept
    {
        return !intents.isEmpty()
            && (urgent || (state == State::Active && batchId != 0));
    }
};

/**
 * Stateful, transport-free MP10 WaypointLeader controller.
 *
 * Every call validates the complete exact-instance group and confirmed
 * mission before deriving an all-or-nothing action batch.  A second batch is
 * withheld until `completeBatch()` records the terminal result of the first.
 * ACK-bearing intents therefore cannot be advanced optimistically by the UI.
 */
class SwarmWaypointLeaderCore final
{
public:
    static constexpr int MaximumTelemetryAgeMs = 5000;
    static constexpr int MaximumOrder = 20;
    static constexpr double MinimumSeparationM = 2.0;
    static constexpr double MaximumSeparationM = 500.0;
    static constexpr double MinimumLeadM = -500.0;
    static constexpr double MaximumLeadM = 5000.0;
    static constexpr double MinimumOffPathTriggerM = 1.0;
    static constexpr double MaximumOffPathTriggerM = 5000.0;
    static constexpr double MinimumAltitudeSeparationM = 1.0;
    static constexpr double MaximumAltitudeSeparationM = 100.0;
    static constexpr double MinimumNavigationAccelerationMps2 = 0.1;
    static constexpr double MaximumNavigationAccelerationMps2 = 100.0;
    static constexpr int PositionStreamRateHz = 5;
    static constexpr int ControlRateHz = 10;

    SwarmWaypointLeaderCore() = default;

    SwarmWaypointLeaderMode mode() const noexcept { return m_mode; }
    quint64 pendingBatchId() const noexcept
    {
        return m_pending ? m_pending->id : 0;
    }
    bool isStopped() const noexcept { return m_stopped; }

    void reset();
    bool requestMode(SwarmWaypointLeaderMode mode,
                     QString *error = nullptr);

    SwarmWaypointLeaderTick tick(
        const SwarmWaypointLeaderPlan &plan,
        const QList<SwarmWaypointLeaderVehicleState> &vehicles,
        const SwarmWaypointLeaderMissionSnapshot &mission,
        qint64 nowMs,
        int maximumAgeMs = MaximumTelemetryAgeMs);

    bool completeBatch(quint64 batchId,
                       SwarmWaypointLeaderBatchResult result,
                       const QString &detail = QString());

    static bool validateSettings(const SwarmWaypointLeaderSettings &settings,
                                 QString *error = nullptr);
    static QString modeName(SwarmWaypointLeaderMode mode);

private:
    struct TargetRecord
    {
        SwarmVehicleInstanceLease lease;
        SwarmFollowPathPoint target;
    };

    struct PendingBatch
    {
        quint64 id = 0;
        QString description;
        std::optional<SwarmWaypointLeaderMode> modeAfterSuccess;
        bool markStreamsRequested = false;
        bool markInitialParametersConfigured = false;
        bool markReturnParametersConfigured = false;
        bool markRtlIssued = false;
        QVector<SwarmVehicleInstanceLease> markTakeoffIssued;
        struct FlightStateBarrier
        {
            enum class Expected { Guided, Armed };
            SwarmVehicleInstanceLease lease;
            Expected expected = Expected::Guided;
            qint64 issuedAfterHeartbeatMs = -1;
        };
        QVector<FlightStateBarrier> markFlightStateBarriers;
        QVector<TargetRecord> targetUpdates;
    };

    struct ResolvedGroup
    {
        const SwarmWaypointLeaderVehicleState *ground = nullptr;
        const SwarmWaypointLeaderVehicleState *air = nullptr;
        QVector<const SwarmWaypointLeaderVehicleState *> flight;
        QVector<int> flightOrders;
        SwarmWaypointLeaderMissionPath path;
    };

    SwarmWaypointLeaderTick stop(const QString &status);
    SwarmWaypointLeaderTick active(const QString &status) const;
    SwarmWaypointLeaderTick urgent(
        const QString &status,
        QVector<SwarmWaypointLeaderCommandIntent> intents) const;
    SwarmWaypointLeaderTick issue(
        const QString &status,
        QVector<SwarmWaypointLeaderCommandIntent> intents,
        PendingBatch effect);

    bool resolve(const SwarmWaypointLeaderPlan &plan,
                 const QList<SwarmWaypointLeaderVehicleState> &vehicles,
                 const SwarmWaypointLeaderMissionSnapshot &mission,
                 qint64 nowMs,
                 int maximumAgeMs,
                 ResolvedGroup *group,
                 QString *error) const;
    bool sameCapturedPlan(const SwarmWaypointLeaderPlan &plan) const;
    void applyRequestedMode();

    SwarmWaypointLeaderTick initialize(
        const SwarmWaypointLeaderSettings &settings,
        const ResolvedGroup &group);
    SwarmWaypointLeaderTick takeoff(
        const SwarmWaypointLeaderSettings &settings,
        const ResolvedGroup &group);
    SwarmWaypointLeaderTick flyToGroundMaster(
        const SwarmWaypointLeaderSettings &settings,
        const ResolvedGroup &group);
    SwarmWaypointLeaderTick followGroundMaster(
        const SwarmWaypointLeaderSettings &settings,
        const ResolvedGroup &group);
    SwarmWaypointLeaderTick returnAlongMission(
        const SwarmWaypointLeaderSettings &settings,
        const ResolvedGroup &group);
    SwarmWaypointLeaderTick landAltitude(
        const SwarmWaypointLeaderSettings &settings,
        const ResolvedGroup &group);
    SwarmWaypointLeaderTick landing(const ResolvedGroup &group);
    std::optional<SwarmWaypointLeaderTick> collisionOverride(
        const SwarmWaypointLeaderSettings &settings,
        const ResolvedGroup &group,
        qint64 nowMs,
        int maximumAgeMs);

    static bool targets(const SwarmWaypointLeaderMissionPath &path,
                        const SwarmFollowPathPoint &reference,
                        double leadM,
                        const SwarmWaypointLeaderSettings &settings,
                        int count,
                        bool vFormation,
                        QVector<SwarmFollowPathPoint> *result);
    static QVector<SwarmWaypointLeaderCommandIntent> positionIntents(
        const QVector<const SwarmWaypointLeaderVehicleState *> &flight,
        const QVector<int> &flightOrders,
        const QVector<SwarmFollowPathPoint> &targets,
        const SwarmWaypointLeaderSettings &settings,
        const SwarmWaypointLeaderVelocity &velocity,
        QVector<TargetRecord> *baseTargetUpdates);
    static void appendNavigationAccelerationIntents(
        const QVector<const SwarmWaypointLeaderVehicleState *> &flight,
        const QVector<int> &flightOrders,
        double accelerationMps2,
        QVector<SwarmWaypointLeaderCommandIntent> *intents);

    bool takeoffWasIssued(const SwarmVehicleInstanceLease &lease) const;
    const PendingBatch::FlightStateBarrier *flightStateBarrier(
        const SwarmVehicleInstanceLease &lease) const;
    void setFlightStateBarrier(
        const PendingBatch::FlightStateBarrier &barrier);
    void clearFlightStateBarrier(const SwarmVehicleInstanceLease &lease);
    bool lastTarget(const SwarmVehicleInstanceLease &lease,
                    SwarmFollowPathPoint *target) const;
    void setLastTarget(const TargetRecord &record);
    void prepareLandingTargets(const ResolvedGroup &group);
    quint64 nextBatchId();

    SwarmWaypointLeaderMode m_mode = SwarmWaypointLeaderMode::Idle;
    std::optional<SwarmWaypointLeaderMode> m_requestedMode;
    std::optional<SwarmWaypointLeaderPlan> m_capturedPlan;
    std::optional<PendingBatch> m_pending;
    QVector<SwarmVehicleInstanceLease> m_takeoffIssued;
    QVector<PendingBatch::FlightStateBarrier> m_flightStateBarriers;
    QVector<TargetRecord> m_lastTargets;
    quint64 m_nextBatchId = 1;
    bool m_streamsRequested = false;
    bool m_initialParametersConfigured = false;
    bool m_returnParametersConfigured = false;
    bool m_rtlIssued = false;
    bool m_collisionCancellationPending = false;
    QString m_stopAfterPending;
    bool m_stopped = false;
    QString m_terminalStatus;
};

#endif // SWARMWAYPOINTLEADERCORE_H
