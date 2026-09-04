#ifndef SWARMFOLLOWPATHCORE_H
#define SWARMFOLLOWPATHCORE_H

#include "comm/SwarmTelemetryRegistry.h"

#include <QString>
#include <QVector>

struct SwarmFollowPathPoint
{
    double latitudeDegrees = 0.0;
    double longitudeDegrees = 0.0;
    double relativeAltitudeM = 0.0;
};

enum class SwarmFollowPathTrailUpdate
{
    Added,
    Unchanged,
    ResetAfterJump,
    InvalidPoint
};

/**
 * Bounded chronological leader trail used by Follow Path and, later, the
 * shared trail portion of Follow Leader.
 */
class SwarmFollowPathTrail final
{
public:
    static constexpr int MaximumPoints = 5000;
    static constexpr double MinimumSampleDistanceM = 0.1;
    static constexpr double MaximumSegmentDistanceM = 500.0;

    int count() const noexcept { return m_points.size(); }
    double lengthM() const noexcept { return m_lengthM; }
    void clear();

    SwarmFollowPathTrailUpdate record(const SwarmFollowPathPoint &point);
    bool pointBehind(double distanceM, SwarmFollowPathPoint *point) const;

    static bool isValidPoint(const SwarmFollowPathPoint &point) noexcept;

private:
    QVector<SwarmFollowPathPoint> m_points;
    double m_lengthM = 0.0;
};

struct SwarmFollowPathFollower
{
    SwarmVehicleInstanceLease lease;
    int order = 0;
};

struct SwarmFollowPathPlan
{
    SwarmVehicleInstanceLease leader;
    QVector<SwarmFollowPathFollower> followers;
    double separationM = 2.0;
};

struct SwarmFollowPathCommand
{
    SwarmVehicleInstanceLease follower;
    int order = 0;
    double distanceBehindM = 0.0;
    SwarmFollowPathPoint target;
};

struct SwarmFollowPathTick
{
    enum class State
    {
        Active,
        WaitingForTrail,
        Stopped
    };

    State state = State::Stopped;
    QVector<SwarmFollowPathCommand> commands;
    QString detail;
    double availableTrailM = 0.0;
    double requiredTrailM = 0.0;

    bool shouldContinue() const noexcept { return state != State::Stopped; }
    bool hasTargets() const noexcept
    {
        return state == State::Active && !commands.isEmpty();
    }
};

/** Pure exact-plan validation and path target generation for MP10 Follow Path. */
class SwarmFollowPathCore final
{
public:
    static constexpr int MaximumTelemetryAgeMs = 5000;
    static constexpr int MaximumOrder = 100;
    static constexpr double MinimumSeparationM = 1.0;
    static constexpr double MaximumSeparationM = 500.0;

    /**
     * Validates the complete exact group before mutating the trail. Every
     * follower target is resolved into a local vector before it is returned.
     */
    static SwarmFollowPathTick buildTick(
        const SwarmFollowPathPlan &plan,
        const QList<SwarmTelemetrySnapshot> &snapshots,
        qint64 nowMs,
        SwarmFollowPathTrail *trail,
        int maximumAgeMs = MaximumTelemetryAgeMs);

private:
    SwarmFollowPathCore() = delete;
};

#endif // SWARMFOLLOWPATHCORE_H
