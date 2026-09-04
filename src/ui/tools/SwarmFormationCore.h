#ifndef SWARMFORMATIONCORE_H
#define SWARMFORMATIONCORE_H

#include "comm/SwarmTelemetryRegistry.h"

#include <QList>
#include <QString>
#include <QVector>

enum class SwarmVehicleFamily {
    Unsupported,
    Plane,
    Copter,
    Rover
};

struct SwarmFormationOffset
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct SwarmFormationTarget
{
    double latitudeDegrees = 0.0;
    double longitudeDegrees = 0.0;
    double relativeAltitudeM = 0.0;
    double velocityNorthMps = 0.0;
    double velocityEastMps = 0.0;
    double velocityDownMps = 0.0;
    double yawDegrees = 0.0;
};

struct SwarmFormationFollower
{
    SwarmVehicleInstanceLease lease;
    SwarmFormationOffset offset;
};

struct SwarmFormationPlan
{
    SwarmVehicleInstanceLease leader;
    QVector<SwarmFormationFollower> followers;
    bool alignYaw = true;
    bool aimGimbals = false;
};

struct SwarmFormationCommand
{
    static constexpr quint16 PositionVelocityTypeMask = 0x0DC0;

    SwarmVehicleInstanceLease follower;
    SwarmFormationTarget target;
    bool alignYaw = true;
    bool aimGimbal = false;
    bool issueYawAction = false;
    double yawErrorDegrees = 0.0;
};

struct SwarmFormationTick
{
    QVector<SwarmFormationCommand> commands;
    QString error;

    bool isValid() const noexcept
    {
        return error.isEmpty() && !commands.isEmpty();
    }
};

/** Pure MP10-compatible formation geometry and pre-send validation. */
class SwarmFormationCore final
{
public:
    static constexpr int MaximumTelemetryAgeMs = 5000;
    static constexpr double MaximumHorizontalOffsetM = 100000.0;
    static constexpr double MaximumVerticalOffsetM = 10000.0;

    static SwarmVehicleFamily family(
        const SwarmTelemetrySnapshot &snapshot);
    static bool supportsFormation(const SwarmTelemetrySnapshot &snapshot);
    static bool supportsPositionFollower(
        const SwarmTelemetrySnapshot &snapshot);
    static bool isPlane(const SwarmTelemetrySnapshot &snapshot);
    static bool isSafeOffset(const SwarmFormationOffset &offset) noexcept;
    static bool hasFreshPosition(const SwarmTelemetrySnapshot &snapshot,
                                 qint64 nowMs,
                                 int maximumAgeMs = MaximumTelemetryAgeMs);
    static bool hasFreshVelocity(const SwarmTelemetrySnapshot &snapshot,
                                 qint64 nowMs,
                                 int maximumAgeMs = MaximumTelemetryAgeMs);
    static bool hasFreshYaw(const SwarmTelemetrySnapshot &snapshot,
                            qint64 nowMs,
                            int maximumAgeMs = MaximumTelemetryAgeMs);

    static SwarmFormationTarget targetFromLeader(
        const SwarmTelemetrySnapshot &leader,
        const SwarmFormationOffset &offset);
    static SwarmFormationOffset offsetFromLeader(
        const SwarmTelemetrySnapshot &leader,
        const SwarmTelemetrySnapshot &follower);
    static bool tryOffsetFromLeader(
        const SwarmTelemetrySnapshot &leader,
        const SwarmTelemetrySnapshot &follower,
        qint64 nowMs,
        SwarmFormationOffset *offset,
        QString *error = nullptr,
        int maximumAgeMs = MaximumTelemetryAgeMs);

    /**
     * Builds the entire tick before any transport call. Snapshots must be the
     * result of one SwarmTelemetryRegistry::validateGroup() operation.
     */
    static SwarmFormationTick buildTick(
        const SwarmFormationPlan &plan,
        const QList<SwarmTelemetrySnapshot> &snapshots,
        qint64 nowMs,
        int maximumAgeMs = MaximumTelemetryAgeMs);

private:
    SwarmFormationCore() = delete;
};

#endif // SWARMFORMATIONCORE_H
