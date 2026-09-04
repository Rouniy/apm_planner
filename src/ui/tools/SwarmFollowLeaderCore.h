#ifndef SWARMFOLLOWLEADERCORE_H
#define SWARMFOLLOWLEADERCORE_H

#include "SwarmFollowPathCore.h"

#include <QList>
#include <QString>
#include <QVector>

#include <optional>

struct SwarmFollowLeaderVelocity
{
    double northMps = 0.0;
    double eastMps = 0.0;
    double downMps = 0.0;
};

struct SwarmFollowLeaderSettings
{
    double separationM = 5.0;
    double leadM = 20.0;
    double altitudeM = 10.0;
};

struct SwarmFollowLeaderFollower
{
    SwarmVehicleInstanceLease lease;
    int order = 0;
};

/**
 * Optional mission context for MP10's near-waypoint turn correction.
 *
 * The lease makes the otherwise transport-free hint exact: a hint captured
 * for a previous ground-master instance cannot steer a replacement instance.
 */
struct SwarmFollowLeaderMissionTurnHint
{
    SwarmVehicleInstanceLease groundMaster;
    double waypointDistanceM = 0.0;
    SwarmFollowPathPoint currentWaypoint;
    SwarmFollowPathPoint nextWaypoint;
};

struct SwarmFollowLeaderPlan
{
    SwarmVehicleInstanceLease groundMaster;
    SwarmVehicleInstanceLease airMaster;
    QVector<SwarmFollowLeaderFollower> followers;
    SwarmFollowLeaderSettings settings;
    std::optional<SwarmFollowLeaderMissionTurnHint> missionTurnHint;
};

struct SwarmFollowLeaderCommand
{
    SwarmVehicleInstanceLease lease;
    QString role;
    int order = 0;
    double distanceBehindM = 0.0;
    SwarmFollowPathPoint target;
    SwarmFollowLeaderVelocity velocity;
};

struct SwarmFollowLeaderTick
{
    enum class State
    {
        Active,
        WaitingForTrail,
        Stopped
    };

    State state = State::Stopped;
    QVector<SwarmFollowLeaderCommand> commands;
    QString detail;
    double availableTrailM = 0.0;
    double requiredTrailM = 0.0;

    bool shouldContinue() const noexcept { return state != State::Stopped; }
    bool hasTargets() const noexcept
    {
        return state == State::Active && !commands.isEmpty();
    }
};

/** Pure exact-plan validation and target generation for MP10 Follow Leader. */
class SwarmFollowLeaderCore final
{
public:
    static constexpr int MaximumTelemetryAgeMs = 5000;
    static constexpr double MinimumSeparationM = 1.0;
    static constexpr double MaximumSeparationM = 500.0;
    static constexpr double MinimumLeadM = -100000.0;
    static constexpr double MaximumLeadM = 100000.0;
    static constexpr double MinimumAltitudeM = 1.0;
    static constexpr double MaximumAltitudeM = 10000.0;
    static constexpr int MaximumFollowers = 21;

    /**
     * Validates one complete exact snapshot group and constructs the complete
     * command batch before returning it to the transport layer.
     */
    static SwarmFollowLeaderTick buildTick(
        const SwarmFollowLeaderPlan &plan,
        const QList<SwarmTelemetrySnapshot> &snapshots,
        qint64 nowMs,
        SwarmFollowPathTrail *trail,
        int maximumAgeMs = MaximumTelemetryAgeMs);

private:
    SwarmFollowLeaderCore() = delete;
};

#endif // SWARMFOLLOWLEADERCORE_H
