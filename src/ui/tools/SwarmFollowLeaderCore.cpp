#include "SwarmFollowLeaderCore.h"

#include "comm/SwarmFlightMode.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{

constexpr double EarthRadiusM = 6378137.0;
constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr double DegreesToRadians = Pi / 180.0;
constexpr double RadiansToDegrees = 180.0 / Pi;

bool validAge(qint64 observedMs, qint64 nowMs, int maximumAgeMs)
{
    return observedMs >= 0 && nowMs >= observedMs && maximumAgeMs > 0
        && nowMs - observedMs <= maximumAgeMs;
}

double normalizedLongitude(double degrees)
{
    double result = std::fmod(degrees + 540.0, 360.0);
    if (result < 0.0) {
        result += 360.0;
    }
    return result - 180.0;
}

std::pair<double, double> distanceAndBearing(
    const SwarmFollowPathPoint &from, const SwarmFollowPathPoint &to)
{
    const double fromLatitude = from.latitudeDegrees * DegreesToRadians;
    const double toLatitude = to.latitudeDegrees * DegreesToRadians;
    const double deltaLatitude =
        (to.latitudeDegrees - from.latitudeDegrees) * DegreesToRadians;
    const double deltaLongitude =
        (to.longitudeDegrees - from.longitudeDegrees) * DegreesToRadians;
    double haversine = std::sin(deltaLatitude / 2.0)
            * std::sin(deltaLatitude / 2.0)
        + std::cos(fromLatitude) * std::cos(toLatitude)
            * std::sin(deltaLongitude / 2.0)
            * std::sin(deltaLongitude / 2.0);
    haversine = std::max(0.0, std::min(1.0, haversine));
    const double distance = EarthRadiusM * 2.0 * std::atan2(
        std::sqrt(haversine), std::sqrt(1.0 - haversine));
    const double bearing = std::atan2(
        std::sin(deltaLongitude) * std::cos(toLatitude),
        std::cos(fromLatitude) * std::sin(toLatitude)
            - std::sin(fromLatitude) * std::cos(toLatitude)
                * std::cos(deltaLongitude));
    return {distance, bearing};
}

SwarmFollowPathPoint projected(const SwarmFollowPathPoint &from,
                               double bearingRadians,
                               double distanceM)
{
    if (distanceM <= std::numeric_limits<double>::epsilon()) {
        SwarmFollowPathPoint result = from;
        result.longitudeDegrees = normalizedLongitude(result.longitudeDegrees);
        return result;
    }
    const double angularDistance = distanceM / EarthRadiusM;
    const double latitude = from.latitudeDegrees * DegreesToRadians;
    const double longitude = from.longitudeDegrees * DegreesToRadians;
    const double targetLatitude = std::asin(
        std::sin(latitude) * std::cos(angularDistance)
        + std::cos(latitude) * std::sin(angularDistance)
            * std::cos(bearingRadians));
    const double targetLongitude = longitude + std::atan2(
        std::sin(bearingRadians) * std::sin(angularDistance)
            * std::cos(latitude),
        std::cos(angularDistance)
            - std::sin(latitude) * std::sin(targetLatitude));
    return {targetLatitude * RadiansToDegrees,
            normalizedLongitude(targetLongitude * RadiansToDegrees),
            from.relativeAltitudeM};
}

QString vehicleName(const SwarmVehicleInstanceLease &lease)
{
    const QString link = lease.endpoint.linkName.trimmed().isEmpty()
        ? QStringLiteral("Link %1").arg(lease.endpoint.linkId)
        : lease.endpoint.linkName.trimmed();
    return QStringLiteral("%1 — %2:%3")
        .arg(link)
        .arg(lease.endpoint.systemId)
        .arg(lease.endpoint.componentId);
}

bool sameEndpoint(const SwarmVehicleInstanceLease &left,
                  const SwarmVehicleInstanceLease &right)
{
    return left.endpoint.sameIdentity(right.endpoint);
}

const SwarmTelemetrySnapshot *snapshotFor(
    const QList<SwarmTelemetrySnapshot> &snapshots,
    const SwarmVehicleInstanceLease &lease,
    bool *replacementFound = nullptr)
{
    if (replacementFound) {
        *replacementFound = false;
    }
    for (const SwarmTelemetrySnapshot &snapshot : snapshots) {
        if (snapshot.lease.sameInstance(lease)) {
            return &snapshot;
        }
        if (replacementFound && sameEndpoint(snapshot.lease, lease)) {
            *replacementFound = true;
        }
    }
    return nullptr;
}

bool hasFreshHeartbeat(const SwarmTelemetrySnapshot &snapshot,
                       qint64 nowMs, int maximumAgeMs)
{
    return snapshot.heartbeatValid
        && validAge(snapshot.heartbeatObservedMs, nowMs, maximumAgeMs);
}

bool isLiveAutopilot(const SwarmTelemetrySnapshot &snapshot,
                     qint64 nowMs, int maximumAgeMs)
{
    return snapshot.lease.endpoint.componentId == MAV_COMP_ID_AUTOPILOT1
        && snapshot.autopilot != MAV_AUTOPILOT_INVALID
        && snapshot.vehicleType != MAV_TYPE_GCS
        && hasFreshHeartbeat(snapshot, nowMs, maximumAgeMs);
}

bool isLiveArduCopter(const SwarmTelemetrySnapshot &snapshot,
                      qint64 nowMs, int maximumAgeMs)
{
    return isLiveAutopilot(snapshot, nowMs, maximumAgeMs)
        && snapshot.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA
        && SwarmFlightMode::isCopter(snapshot.vehicleType);
}

bool hasFreshPosition(const SwarmTelemetrySnapshot &snapshot,
                      qint64 nowMs, int maximumAgeMs)
{
    return snapshot.positionValid
        && validAge(snapshot.positionObservedMs, nowMs, maximumAgeMs)
        && SwarmFollowPathTrail::isValidPoint(
            {snapshot.latitudeDegrees, snapshot.longitudeDegrees,
             snapshot.relativeAltitudeM});
}

bool hasFreshVelocity(const SwarmTelemetrySnapshot &snapshot,
                      qint64 nowMs, int maximumAgeMs)
{
    // GLOBAL_POSITION_INT supplies position and NED velocity atomically in
    // SwarmTelemetryRegistry, so both fields share positionObservedMs.
    return snapshot.velocityValid
        && validAge(snapshot.positionObservedMs, nowMs, maximumAgeMs)
        && std::isfinite(snapshot.velocityNorthMps)
        && std::isfinite(snapshot.velocityEastMps)
        && std::isfinite(snapshot.velocityDownMps);
}

SwarmFollowLeaderTick stopped(const QString &detail,
                              const SwarmFollowPathTrail *trail = nullptr,
                              double requiredTrailM = 0.0)
{
    SwarmFollowLeaderTick tick;
    tick.state = SwarmFollowLeaderTick::State::Stopped;
    tick.detail = detail;
    tick.availableTrailM = trail ? trail->lengthM() : 0.0;
    tick.requiredTrailM = requiredTrailM;
    return tick;
}

bool tryMissionTurnBearing(
    const SwarmFollowLeaderPlan &plan,
    const SwarmFollowPathPoint &groundPoint,
    double *bearingRadians)
{
    if (!bearingRadians || !plan.missionTurnHint) {
        return false;
    }
    const SwarmFollowLeaderMissionTurnHint &hint = *plan.missionTurnHint;
    if (!hint.groundMaster.sameInstance(plan.groundMaster)
        || !std::isfinite(hint.waypointDistanceM)
        || hint.waypointDistanceM < 0.0
        || hint.waypointDistanceM >= plan.settings.separationM * 1.5
        || !SwarmFollowPathTrail::isValidPoint(hint.currentWaypoint)
        || !SwarmFollowPathTrail::isValidPoint(hint.nextWaypoint)) {
        return false;
    }

    const std::pair<double, double> route = distanceAndBearing(
        hint.currentWaypoint, hint.nextWaypoint);
    if (!std::isfinite(route.first) || !std::isfinite(route.second)
        || route.first <= std::numeric_limits<double>::epsilon()) {
        return false;
    }
    const SwarmFollowPathPoint aim = projected(
        hint.currentWaypoint, route.second, plan.settings.separationM);
    const std::pair<double, double> aimGeometry = distanceAndBearing(
        groundPoint, aim);
    if (!std::isfinite(aimGeometry.first)
        || !std::isfinite(aimGeometry.second)
        || aimGeometry.first <= std::numeric_limits<double>::epsilon()) {
        return false;
    }
    *bearingRadians = aimGeometry.second;
    return true;
}

} // namespace

SwarmFollowLeaderTick SwarmFollowLeaderCore::buildTick(
    const SwarmFollowLeaderPlan &plan,
    const QList<SwarmTelemetrySnapshot> &snapshots,
    qint64 nowMs,
    SwarmFollowPathTrail *trail,
    int maximumAgeMs)
{
    if (!trail || nowMs < 0 || maximumAgeMs <= 0
        || !plan.groundMaster.isValid() || !plan.airMaster.isValid()) {
        return stopped(QStringLiteral(
            "Follow Leader plan, clock, trail, or master lease is invalid."),
            trail);
    }
    if (!std::isfinite(plan.settings.separationM)
        || plan.settings.separationM < MinimumSeparationM
        || plan.settings.separationM > MaximumSeparationM) {
        return stopped(QStringLiteral(
            "Follow Leader separation must be between 1 and 500 m."), trail);
    }
    if (!std::isfinite(plan.settings.leadM)
        || plan.settings.leadM < MinimumLeadM
        || plan.settings.leadM > MaximumLeadM) {
        return stopped(QStringLiteral(
            "Follow Leader lead must be finite and within +/-100000 m."), trail);
    }
    if (!std::isfinite(plan.settings.altitudeM)
        || plan.settings.altitudeM < MinimumAltitudeM
        || plan.settings.altitudeM > MaximumAltitudeM) {
        return stopped(QStringLiteral(
            "Follow Leader altitude must be between 1 and 10000 m."), trail);
    }
    if (plan.followers.size() > MaximumFollowers) {
        return stopped(QStringLiteral(
            "Follow Leader supports at most 21 follower positions."), trail);
    }
    if (sameEndpoint(plan.groundMaster, plan.airMaster)) {
        return stopped(QStringLiteral(
            "Follow Leader ground master and air master must be different vehicles."),
            trail);
    }

    QVector<SwarmFollowLeaderFollower> ordered = plan.followers;
    std::sort(ordered.begin(), ordered.end(),
              [](const SwarmFollowLeaderFollower &left,
                 const SwarmFollowLeaderFollower &right) {
        return left.order < right.order;
    });
    QSet<VehicleEndpoint> plannedEndpoints;
    plannedEndpoints.insert(plan.groundMaster.endpoint);
    plannedEndpoints.insert(plan.airMaster.endpoint);
    for (int index = 0; index < ordered.size(); ++index) {
        const SwarmFollowLeaderFollower &follower = ordered.at(index);
        if (!follower.lease.isValid()) {
            return stopped(QStringLiteral(
                "A Follow Leader follower lease is invalid."), trail);
        }
        if (follower.order != index + 1) {
            return stopped(QStringLiteral(
                "Follower order must be unique and contiguous from 1 through N."),
                trail);
        }
        if (plannedEndpoints.contains(follower.lease.endpoint)) {
            return stopped(QStringLiteral(
                "One exact vehicle endpoint has more than one Follow Leader role."),
                trail);
        }
        plannedEndpoints.insert(follower.lease.endpoint);
    }

    const int expectedSnapshots = ordered.size() + 2;
    if (snapshots.size() != expectedSnapshots) {
        return stopped(QStringLiteral(
            "The exact Follow Leader snapshot group does not match the plan."),
            trail);
    }
    QSet<VehicleEndpoint> snapshotEndpoints;
    for (const SwarmTelemetrySnapshot &snapshot : snapshots) {
        if (!snapshot.lease.isValid()
            || snapshotEndpoints.contains(snapshot.lease.endpoint)) {
            return stopped(QStringLiteral(
                "The Follow Leader snapshot group contains an invalid or duplicate endpoint."),
                trail);
        }
        snapshotEndpoints.insert(snapshot.lease.endpoint);
    }

    bool replacementFound = false;
    const SwarmTelemetrySnapshot *ground = snapshotFor(
        snapshots, plan.groundMaster, &replacementFound);
    if (!ground) {
        return stopped(replacementFound
            ? QStringLiteral(
                "The Follow Leader ground master was reconnected or replaced.")
            : QStringLiteral(
                "The Follow Leader ground master is missing from the exact snapshot group."),
            trail);
    }
    if (!isLiveAutopilot(*ground, nowMs, maximumAgeMs)) {
        return stopped(QStringLiteral(
            "The Follow Leader ground master is not a live autopilot."), trail);
    }
    if (!hasFreshPosition(*ground, nowMs, maximumAgeMs)) {
        return stopped(QStringLiteral(
            "The Follow Leader ground-master position is unavailable or stale."),
            trail);
    }
    if (!hasFreshVelocity(*ground, nowMs, maximumAgeMs)) {
        return stopped(QStringLiteral(
            "The Follow Leader ground-master velocity is unavailable or stale."),
            trail);
    }

    replacementFound = false;
    const SwarmTelemetrySnapshot *air = snapshotFor(
        snapshots, plan.airMaster, &replacementFound);
    if (!air) {
        return stopped(replacementFound
            ? QStringLiteral(
                "The Follow Leader air master was reconnected or replaced.")
            : QStringLiteral(
                "The Follow Leader air master is missing from the exact snapshot group."),
            trail);
    }
    if (!isLiveArduCopter(*air, nowMs, maximumAgeMs)) {
        return stopped(QStringLiteral(
            "The Follow Leader air master must be a live ArduCopter autopilot."),
            trail);
    }
    if (!hasFreshPosition(*air, nowMs, maximumAgeMs)) {
        return stopped(QStringLiteral(
            "The Follow Leader air-master position is unavailable or stale."),
            trail);
    }
    if (!SwarmFlightMode::isExactGuided(*air)) {
        return stopped(QStringLiteral(
            "Air master %1 is in %2, not the exact ArduCopter GUIDED mode.")
            .arg(vehicleName(plan.airMaster),
                 SwarmFlightMode::displayName(*air)), trail);
    }

    QVector<const SwarmTelemetrySnapshot *> followerSnapshots;
    followerSnapshots.reserve(ordered.size());
    for (const SwarmFollowLeaderFollower &planned : ordered) {
        replacementFound = false;
        const SwarmTelemetrySnapshot *follower = snapshotFor(
            snapshots, planned.lease, &replacementFound);
        if (!follower) {
            return stopped(replacementFound
                ? QStringLiteral(
                    "A Follow Leader follower was reconnected or replaced.")
                : QStringLiteral(
                    "A Follow Leader follower is missing from the exact snapshot group."),
                trail);
        }
        if (!isLiveArduCopter(*follower, nowMs, maximumAgeMs)) {
            return stopped(QStringLiteral(
                "Follower %1 must be a live ArduCopter autopilot.")
                .arg(vehicleName(planned.lease)), trail);
        }
        if (!hasFreshPosition(*follower, nowMs, maximumAgeMs)) {
            return stopped(QStringLiteral(
                "Follower %1 position is unavailable or stale.")
                .arg(vehicleName(planned.lease)), trail);
        }
        if (!SwarmFlightMode::isExactGuided(*follower)) {
            return stopped(QStringLiteral(
                "Follower %1 is in %2, not the exact ArduCopter GUIDED mode.")
                .arg(vehicleName(planned.lease),
                     SwarmFlightMode::displayName(*follower)), trail);
        }
        followerSnapshots.append(follower);
    }

    const SwarmFollowPathPoint groundPoint{
        ground->latitudeDegrees,
        ground->longitudeDegrees,
        ground->relativeAltitudeM};
    SwarmFollowPathTrail nextTrail = *trail;
    const SwarmFollowPathTrailUpdate trailUpdate = nextTrail.record(groundPoint);
    const double requiredTrailM = ordered.isEmpty()
        ? 0.0
        : static_cast<double>(ordered.constLast().order - 1)
            * plan.settings.separationM;
    if (trailUpdate == SwarmFollowPathTrailUpdate::InvalidPoint) {
        return stopped(QStringLiteral(
            "The Follow Leader ground-master position is invalid."), trail,
            requiredTrailM);
    }
    if (trailUpdate == SwarmFollowPathTrailUpdate::ResetAfterJump) {
        *trail = nextTrail;
        return stopped(QStringLiteral(
            "Follow Leader stopped because the ground-master position jumped by more than 500 m; the trail was reset."),
            trail, requiredTrailM);
    }

    struct FollowerTarget
    {
        SwarmVehicleInstanceLease lease;
        int order = 0;
        double distanceBehindM = 0.0;
        SwarmFollowPathPoint point;
    };
    QVector<FollowerTarget> followerTargets;
    followerTargets.reserve(ordered.size());
    for (int index = 0; index < ordered.size(); ++index) {
        const SwarmFollowLeaderFollower &planned = ordered.at(index);
        const double distanceBehindM =
            static_cast<double>(planned.order - 1)
            * plan.settings.separationM;
        SwarmFollowPathPoint target;
        // MP10 prepends the current ground-master location to its sampled
        // history. Preserve that exact order-1 target even when the shared
        // trail deliberately coalesces movement below 0.1 m.
        bool targetAvailable = true;
        if (planned.order == 1) {
            target = groundPoint;
        } else {
            targetAvailable = nextTrail.pointBehind(distanceBehindM, &target);
        }
        if (!targetAvailable) {
            *trail = nextTrail;
            SwarmFollowLeaderTick waiting;
            waiting.state = SwarmFollowLeaderTick::State::WaitingForTrail;
            waiting.detail = QStringLiteral(
                "Recording ground-master trail: %1 of %2 m required.")
                .arg(trail->lengthM(), 0, 'f', 1)
                .arg(requiredTrailM, 0, 'f', 1);
            waiting.availableTrailM = trail->lengthM();
            waiting.requiredTrailM = requiredTrailM;
            return waiting;
        }
        target.relativeAltitudeM += plan.settings.altitudeM;
        if (!SwarmFollowPathTrail::isValidPoint(target)) {
            return stopped(QStringLiteral(
                "A Follow Leader follower target is invalid or excessive."),
                trail, requiredTrailM);
        }
        followerTargets.append(
            {followerSnapshots.at(index)->lease, planned.order,
             distanceBehindM, target});
    }

    SwarmFollowLeaderVelocity groundVelocity{
        ground->velocityNorthMps,
        ground->velocityEastMps,
        ground->velocityDownMps};
    double bearingRadians = std::atan2(
        groundVelocity.eastMps, groundVelocity.northMps);
    SwarmFollowLeaderVelocity airVelocity = groundVelocity;
    if (tryMissionTurnBearing(plan, groundPoint, &bearingRadians)) {
        // MP10 turns the air-master feed-forward vector using CurrentState's
        // groundspeed rather than the N/E velocity magnitude. Fall back to
        // the atomically observed N/E vector when VFR_HUD is unavailable or
        // stale so an optional mission hint cannot introduce invalid data.
        const bool freshGroundSpeed = ground->vfrHudValid
            && validAge(ground->vfrHudObservedMs, nowMs, maximumAgeMs)
            && std::isfinite(ground->groundSpeedMps)
            && ground->groundSpeedMps >= 0.0;
        const double horizontalSpeed = freshGroundSpeed
            ? ground->groundSpeedMps
            : std::hypot(groundVelocity.northMps,
                         groundVelocity.eastMps);
        airVelocity.northMps = std::cos(bearingRadians) * horizontalSpeed;
        airVelocity.eastMps = std::sin(bearingRadians) * horizontalSpeed;
    }

    SwarmFollowPathPoint airTarget = projected(
        groundPoint, bearingRadians, plan.settings.separationM);
    airTarget.relativeAltitudeM = plan.settings.altitudeM;
    if (!SwarmFollowPathTrail::isValidPoint(airTarget)) {
        return stopped(QStringLiteral(
            "The Follow Leader air-master target is invalid."), trail,
            requiredTrailM);
    }

    QVector<SwarmFollowLeaderCommand> commands;
    commands.reserve(followerTargets.size() + 1);
    commands.append({air->lease,
                     QStringLiteral("Air master"),
                     0,
                     -plan.settings.separationM,
                     airTarget,
                     {airVelocity.northMps * 0.6,
                      airVelocity.eastMps * 0.6,
                      airVelocity.downMps * 0.6}});
    for (const FollowerTarget &target : followerTargets) {
        commands.append({target.lease,
                         QStringLiteral("Follower #%1").arg(target.order),
                         target.order,
                         target.distanceBehindM,
                         target.point,
                         {groundVelocity.northMps * 0.5,
                          groundVelocity.eastMps * 0.5,
                          groundVelocity.downMps * 0.5}});
    }

    *trail = nextTrail;
    SwarmFollowLeaderTick active;
    active.state = SwarmFollowLeaderTick::State::Active;
    active.commands = std::move(commands);
    active.detail = QStringLiteral(
        "Follow Leader active: %1 target(s) ready; trail %2 m.")
        .arg(active.commands.size()).arg(trail->lengthM(), 0, 'f', 1);
    active.availableTrailM = trail->lengthM();
    active.requiredTrailM = requiredTrailM;
    return active;
}
