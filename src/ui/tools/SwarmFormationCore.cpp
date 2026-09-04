#include "SwarmFormationCore.h"

#include <QHash>
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
    return observedMs >= 0 && nowMs >= observedMs
        && maximumAgeMs > 0 && nowMs - observedMs <= maximumAgeMs;
}

double wrap180(double degrees)
{
    while (degrees > 180.0) {
        degrees -= 360.0;
    }
    while (degrees < -180.0) {
        degrees += 360.0;
    }
    return degrees;
}

double yawDegrees(const SwarmTelemetrySnapshot &snapshot)
{
    double degrees = snapshot.yawRadians * RadiansToDegrees;
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0) {
        degrees += 360.0;
    }
    return degrees;
}

bool finiteTarget(const SwarmFormationTarget &target)
{
    return std::isfinite(target.latitudeDegrees)
        && std::isfinite(target.longitudeDegrees)
        && std::isfinite(target.relativeAltitudeM)
        && std::isfinite(target.velocityNorthMps)
        && std::isfinite(target.velocityEastMps)
        && std::isfinite(target.velocityDownMps)
        && std::isfinite(target.yawDegrees)
        && target.latitudeDegrees >= -90.0
        && target.latitudeDegrees <= 90.0
        && target.longitudeDegrees >= -180.0
        && target.longitudeDegrees <= 180.0;
}

double normalizeLongitude(double longitudeDegrees)
{
    double result = std::fmod(longitudeDegrees + 540.0, 360.0);
    if (result < 0.0) {
        result += 360.0;
    }
    return result - 180.0;
}

std::pair<double, double> project(double latitudeDegrees,
                                  double longitudeDegrees,
                                  double bearingRadians,
                                  double distanceM)
{
    if (distanceM <= std::numeric_limits<double>::epsilon()) {
        return {latitudeDegrees, normalizeLongitude(longitudeDegrees)};
    }
    const double angularDistance = distanceM / EarthRadiusM;
    const double latitude = latitudeDegrees * DegreesToRadians;
    const double longitude = longitudeDegrees * DegreesToRadians;
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
            normalizeLongitude(targetLongitude * RadiansToDegrees)};
}

std::pair<double, double> distanceAndBearing(
    double fromLatitudeDegrees, double fromLongitudeDegrees,
    double toLatitudeDegrees, double toLongitudeDegrees)
{
    const double fromLatitude = fromLatitudeDegrees * DegreesToRadians;
    const double toLatitude = toLatitudeDegrees * DegreesToRadians;
    const double deltaLatitude =
        (toLatitudeDegrees - fromLatitudeDegrees) * DegreesToRadians;
    const double deltaLongitude =
        (toLongitudeDegrees - fromLongitudeDegrees) * DegreesToRadians;
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

const SwarmTelemetrySnapshot *findSnapshot(
    const QList<SwarmTelemetrySnapshot> &snapshots,
    const SwarmVehicleInstanceLease &lease)
{
    for (const SwarmTelemetrySnapshot &snapshot : snapshots) {
        if (snapshot.lease.sameInstance(lease)) {
            return &snapshot;
        }
    }
    return nullptr;
}

bool isCopterType(int type)
{
    switch (type) {
    case MAV_TYPE_QUADROTOR:
    case MAV_TYPE_COAXIAL:
    case MAV_TYPE_HELICOPTER:
    case MAV_TYPE_HEXAROTOR:
    case MAV_TYPE_OCTOROTOR:
    case MAV_TYPE_TRICOPTER:
    case MAV_TYPE_DODECAROTOR:
    case MAV_TYPE_DECAROTOR:
        return true;
    default:
        return false;
    }
}

bool isRoverType(int type)
{
    return type == MAV_TYPE_GROUND_ROVER || type == MAV_TYPE_SURFACE_BOAT;
}

} // namespace

bool SwarmFormationCore::supportsFormation(
    const SwarmTelemetrySnapshot &snapshot)
{
    return family(snapshot) != SwarmVehicleFamily::Unsupported;
}

SwarmVehicleFamily SwarmFormationCore::family(
    const SwarmTelemetrySnapshot &snapshot)
{
    if (!snapshot.heartbeatValid
        || snapshot.autopilot != MAV_AUTOPILOT_ARDUPILOTMEGA) {
        return SwarmVehicleFamily::Unsupported;
    }
    if (snapshot.vehicleType == MAV_TYPE_FIXED_WING) {
        return SwarmVehicleFamily::Plane;
    }
    if (isCopterType(snapshot.vehicleType)) {
        return SwarmVehicleFamily::Copter;
    }
    if (isRoverType(snapshot.vehicleType)) {
        return SwarmVehicleFamily::Rover;
    }
    return SwarmVehicleFamily::Unsupported;
}

bool SwarmFormationCore::supportsPositionFollower(
    const SwarmTelemetrySnapshot &snapshot)
{
    const SwarmVehicleFamily type = family(snapshot);
    return type == SwarmVehicleFamily::Copter
        || type == SwarmVehicleFamily::Rover;
}

bool SwarmFormationCore::isPlane(const SwarmTelemetrySnapshot &snapshot)
{
    return family(snapshot) == SwarmVehicleFamily::Plane;
}

bool SwarmFormationCore::isSafeOffset(
    const SwarmFormationOffset &offset) noexcept
{
    return std::isfinite(offset.x) && std::isfinite(offset.y)
        && std::isfinite(offset.z)
        && std::abs(offset.x) <= MaximumHorizontalOffsetM
        && std::abs(offset.y) <= MaximumHorizontalOffsetM
        && std::abs(offset.z) <= MaximumVerticalOffsetM;
}

bool SwarmFormationCore::hasFreshPosition(
    const SwarmTelemetrySnapshot &snapshot, qint64 nowMs, int maximumAgeMs)
{
    return snapshot.positionValid
        && validAge(snapshot.positionObservedMs, nowMs, maximumAgeMs)
        && std::isfinite(snapshot.latitudeDegrees)
        && std::isfinite(snapshot.longitudeDegrees)
        && std::isfinite(snapshot.relativeAltitudeM)
        && snapshot.latitudeDegrees >= -90.0
        && snapshot.latitudeDegrees <= 90.0
        && snapshot.longitudeDegrees >= -180.0
        && snapshot.longitudeDegrees <= 180.0
        && (std::abs(snapshot.latitudeDegrees)
            > std::numeric_limits<double>::epsilon()
            || std::abs(snapshot.longitudeDegrees)
            > std::numeric_limits<double>::epsilon());
}

bool SwarmFormationCore::hasFreshVelocity(
    const SwarmTelemetrySnapshot &snapshot, qint64 nowMs, int maximumAgeMs)
{
    return snapshot.velocityValid
        && validAge(snapshot.positionObservedMs, nowMs, maximumAgeMs)
        && std::isfinite(snapshot.velocityNorthMps)
        && std::isfinite(snapshot.velocityEastMps)
        && std::isfinite(snapshot.velocityDownMps);
}

bool SwarmFormationCore::hasFreshYaw(
    const SwarmTelemetrySnapshot &snapshot, qint64 nowMs, int maximumAgeMs)
{
    return snapshot.attitudeValid
        && validAge(snapshot.attitudeObservedMs, nowMs, maximumAgeMs)
        && std::isfinite(snapshot.yawRadians);
}

SwarmFormationTarget SwarmFormationCore::targetFromLeader(
    const SwarmTelemetrySnapshot &leader,
    const SwarmFormationOffset &offset)
{
    SwarmFormationTarget target;
    const double leaderYawDegrees = yawDegrees(leader);
    const double heading = -leaderYawDegrees * DegreesToRadians;
    const double east = offset.x * std::cos(heading)
        - offset.y * std::sin(heading);
    const double north = offset.x * std::sin(heading)
        + offset.y * std::cos(heading);
    const double distance = std::sqrt(east * east + north * north);
    const double bearing = std::atan2(east, north);
    const std::pair<double, double> coordinate = project(
        leader.latitudeDegrees, leader.longitudeDegrees, bearing, distance);
    target.latitudeDegrees = coordinate.first;
    target.longitudeDegrees = coordinate.second;
    target.relativeAltitudeM = leader.relativeAltitudeM + offset.z;
    target.velocityNorthMps = leader.velocityNorthMps;
    target.velocityEastMps = leader.velocityEastMps;
    target.velocityDownMps = leader.velocityDownMps;
    target.yawDegrees = leaderYawDegrees;
    return target;
}

SwarmFormationOffset SwarmFormationCore::offsetFromLeader(
    const SwarmTelemetrySnapshot &leader,
    const SwarmTelemetrySnapshot &follower)
{
    const std::pair<double, double> geometry = distanceAndBearing(
        leader.latitudeDegrees, leader.longitudeDegrees,
        follower.latitudeDegrees, follower.longitudeDegrees);
    const double east = geometry.first * std::sin(geometry.second);
    const double north = geometry.first * std::cos(geometry.second);
    const double heading = -yawDegrees(leader) * DegreesToRadians;
    SwarmFormationOffset result;
    result.x = east * std::cos(heading) + north * std::sin(heading);
    result.y = -east * std::sin(heading) + north * std::cos(heading);
    result.z = follower.relativeAltitudeM - leader.relativeAltitudeM;
    return result;
}

bool SwarmFormationCore::tryOffsetFromLeader(
    const SwarmTelemetrySnapshot &leader,
    const SwarmTelemetrySnapshot &follower,
    qint64 nowMs, SwarmFormationOffset *offset,
    QString *error, int maximumAgeMs)
{
    if (error) {
        error->clear();
    }
    if (!offset) {
        if (error) {
            *error = QStringLiteral("The output offset is unavailable.");
        }
        return false;
    }
    if (!hasFreshPosition(leader, nowMs, maximumAgeMs)
        || !hasFreshYaw(leader, nowMs, maximumAgeMs)) {
        if (error) {
            *error = QStringLiteral(
                "Leader position or attitude yaw is unavailable or stale.");
        }
        return false;
    }
    if (!hasFreshPosition(follower, nowMs, maximumAgeMs)) {
        if (error) {
            *error = QStringLiteral(
                "Follower position is unavailable or stale.");
        }
        return false;
    }
    const SwarmFormationOffset calculated =
        offsetFromLeader(leader, follower);
    if (!isSafeOffset(calculated)) {
        if (error) {
            *error = QStringLiteral("The captured offset is invalid or excessive.");
        }
        return false;
    }
    *offset = calculated;
    return true;
}

SwarmFormationTick SwarmFormationCore::buildTick(
    const SwarmFormationPlan &plan,
    const QList<SwarmTelemetrySnapshot> &snapshots,
    qint64 nowMs, int maximumAgeMs)
{
    SwarmFormationTick result;
    if (!plan.leader.isValid()) {
        result.error = QStringLiteral("Formation stopped: leader lease is invalid.");
        return result;
    }
    if (nowMs < 0 || maximumAgeMs <= 0) {
        result.error = QStringLiteral("Formation stopped: the telemetry clock is invalid.");
        return result;
    }
    if (plan.followers.isEmpty()) {
        result.error = QStringLiteral("Formation stopped: no followers are selected.");
        return result;
    }
    if (plan.followers.size()
        > SwarmTelemetryRegistry::MaximumVehicleEndpoints - 1) {
        result.error = QStringLiteral("Formation stopped: too many followers.");
        return result;
    }
    QSet<VehicleEndpoint> plannedEndpoints;
    plannedEndpoints.insert(plan.leader.endpoint);
    for (const SwarmFormationFollower &planned : plan.followers) {
        if (!planned.lease.isValid()) {
            result.error = QStringLiteral(
                "Formation stopped: a follower lease is invalid.");
            return result;
        }
        if (plannedEndpoints.contains(planned.lease.endpoint)) {
            result.error = planned.lease.sameInstance(plan.leader)
                ? QStringLiteral(
                    "Formation stopped: leader was also selected as a follower.")
                : QStringLiteral(
                    "Formation stopped: a vehicle endpoint is assigned twice.");
            return result;
        }
        if (!isSafeOffset(planned.offset)) {
            result.error = QStringLiteral(
                "Formation stopped: follower %1 has an invalid or excessive offset.")
                .arg(vehicleName(planned.lease));
            return result;
        }
        plannedEndpoints.insert(planned.lease.endpoint);
    }
    if (snapshots.size() != plan.followers.size() + 1) {
        result.error = QStringLiteral(
            "Formation stopped: the validated group does not match the plan.");
        return result;
    }
    for (int index = 0; index < snapshots.size(); ++index) {
        const SwarmTelemetrySnapshot &snapshot = snapshots.at(index);
        bool duplicate = false;
        for (int previous = 0; previous < index; ++previous) {
            if (snapshot.lease.sameInstance(
                    snapshots.at(previous).lease)) {
                duplicate = true;
                break;
            }
        }
        if (!snapshot.lease.isValid() || duplicate) {
            result.error = QStringLiteral(
                "Formation stopped: the validated group contains an invalid or duplicate instance.");
            return result;
        }
    }
    const SwarmTelemetrySnapshot *leader = findSnapshot(
        snapshots, plan.leader);
    if (!leader) {
        result.error = QStringLiteral(
            "Formation stopped: leader disappeared or was reconnected.");
        return result;
    }
    if (!supportsFormation(*leader)) {
        result.error = QStringLiteral(
            "Formation stopped: leader firmware or vehicle type is unsupported.");
        return result;
    }
    if (!leader->heartbeatValid
        || !validAge(leader->heartbeatObservedMs, nowMs, maximumAgeMs)) {
        result.error = QStringLiteral(
            "Formation stopped: leader heartbeat is unavailable or stale.");
        return result;
    }
    if (!hasFreshPosition(*leader, nowMs, maximumAgeMs)) {
        result.error = QStringLiteral(
            "Formation stopped: leader position is unavailable or stale.");
        return result;
    }
    if (!hasFreshVelocity(*leader, nowMs, maximumAgeMs)) {
        result.error = QStringLiteral(
            "Formation stopped: leader velocity is unavailable or stale.");
        return result;
    }
    if (!hasFreshYaw(*leader, nowMs, maximumAgeMs)) {
        result.error = QStringLiteral(
            "Formation stopped: leader attitude yaw is unavailable or stale.");
        return result;
    }

    result.commands.reserve(plan.followers.size());
    for (const SwarmFormationFollower &planned : plan.followers) {
        const SwarmTelemetrySnapshot *follower = findSnapshot(
            snapshots, planned.lease);
        if (!follower) {
            result.error = QStringLiteral(
                "Formation stopped: follower %1 disappeared or was reconnected.")
                .arg(vehicleName(planned.lease));
            result.commands.clear();
            return result;
        }
        if (!supportsPositionFollower(*follower)) {
            result.error = isPlane(*follower)
                ? QStringLiteral(
                    "Formation stopped: Plane attitude/PID control is not yet enabled in this build.")
                : QStringLiteral(
                    "Formation stopped: follower %1 has an unsupported firmware or vehicle type.")
                    .arg(vehicleName(planned.lease));
            result.commands.clear();
            return result;
        }
        if (!follower->heartbeatValid
            || !validAge(follower->heartbeatObservedMs,
                         nowMs, maximumAgeMs)) {
            result.error = QStringLiteral(
                "Formation stopped: follower %1 heartbeat is unavailable or stale.")
                .arg(vehicleName(planned.lease));
            result.commands.clear();
            return result;
        }
        SwarmFormationCommand command;
        command.follower = planned.lease;
        command.target = targetFromLeader(*leader, planned.offset);
        command.alignYaw = plan.alignYaw;
        command.aimGimbal = plan.alignYaw && plan.aimGimbals;
        if (plan.alignYaw) {
            if (!hasFreshYaw(*follower, nowMs, maximumAgeMs)) {
                result.error = QStringLiteral(
                    "Formation stopped: follower %1 attitude yaw is unavailable or stale.")
                    .arg(vehicleName(planned.lease));
                result.commands.clear();
                return result;
            }
            command.yawErrorDegrees = wrap180(
                yawDegrees(*follower) - command.target.yawDegrees);
            command.issueYawAction =
                std::abs(command.yawErrorDegrees) > 3.0;
        }
        if (!finiteTarget(command.target)) {
            result.error = QStringLiteral(
                "Formation stopped: follower %1 target is not finite.")
                .arg(vehicleName(planned.lease));
            result.commands.clear();
            return result;
        }
        result.commands.append(command);
    }
    return result;
}
