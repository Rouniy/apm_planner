#include "SwarmFollowPathCore.h"

#include "comm/SwarmFlightMode.h"
#include "SwarmFormationCore.h"

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

bool freshPosition(const SwarmTelemetrySnapshot &snapshot,
                   qint64 nowMs, int maximumAgeMs)
{
    return snapshot.positionValid && snapshot.positionObservedMs >= 0
        && nowMs >= snapshot.positionObservedMs && maximumAgeMs > 0
        && nowMs - snapshot.positionObservedMs <= maximumAgeMs
        && SwarmFollowPathTrail::isValidPoint(
            {snapshot.latitudeDegrees, snapshot.longitudeDegrees,
             snapshot.relativeAltitudeM});
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

SwarmFollowPathTick stopped(const QString &detail,
                            const SwarmFollowPathTrail *trail = nullptr,
                            double requiredTrailM = 0.0)
{
    SwarmFollowPathTick tick;
    tick.state = SwarmFollowPathTick::State::Stopped;
    tick.detail = detail;
    tick.availableTrailM = trail ? trail->lengthM() : 0.0;
    tick.requiredTrailM = requiredTrailM;
    return tick;
}

} // namespace

void SwarmFollowPathTrail::clear()
{
    m_points.clear();
    m_lengthM = 0.0;
}

SwarmFollowPathTrailUpdate SwarmFollowPathTrail::record(
    const SwarmFollowPathPoint &point)
{
    if (!isValidPoint(point)) {
        return SwarmFollowPathTrailUpdate::InvalidPoint;
    }
    if (m_points.isEmpty()) {
        m_points.append(point);
        return SwarmFollowPathTrailUpdate::Added;
    }

    const SwarmFollowPathPoint previous = m_points.constLast();
    const double segment = distanceAndBearing(previous, point).first;
    if (!std::isfinite(segment) || segment > MaximumSegmentDistanceM) {
        clear();
        m_points.append(point);
        return SwarmFollowPathTrailUpdate::ResetAfterJump;
    }
    if (segment < MinimumSampleDistanceM) {
        // Retain the last sampled horizontal point so small movements
        // accumulate, but keep its current relative altitude.
        m_points.last().relativeAltitudeM = point.relativeAltitudeM;
        return SwarmFollowPathTrailUpdate::Unchanged;
    }

    m_points.append(point);
    m_lengthM += segment;
    while (m_points.size() > MaximumPoints) {
        m_lengthM -= distanceAndBearing(m_points.at(0), m_points.at(1)).first;
        m_points.removeFirst();
    }
    m_lengthM = std::max(0.0, m_lengthM);
    return SwarmFollowPathTrailUpdate::Added;
}

bool SwarmFollowPathTrail::pointBehind(
    double distanceM, SwarmFollowPathPoint *point) const
{
    if (point) {
        *point = SwarmFollowPathPoint();
    }
    if (!point || !std::isfinite(distanceM) || distanceM < 0.0
        || m_points.isEmpty()) {
        return false;
    }
    if (distanceM <= std::numeric_limits<double>::epsilon()) {
        *point = m_points.constLast();
        return true;
    }

    double remaining = distanceM;
    for (int index = m_points.size() - 1; index > 0; --index) {
        const SwarmFollowPathPoint &newer = m_points.at(index);
        const SwarmFollowPathPoint &older = m_points.at(index - 1);
        const std::pair<double, double> segment =
            distanceAndBearing(newer, older);
        if (!std::isfinite(segment.first)
            || segment.first <= std::numeric_limits<double>::epsilon()) {
            continue;
        }
        if (remaining <= segment.first) {
            const double ratio = std::max(
                0.0, std::min(1.0, remaining / segment.first));
            *point = projected(newer, segment.second, remaining);
            point->relativeAltitudeM = newer.relativeAltitudeM
                + (older.relativeAltitudeM - newer.relativeAltitudeM) * ratio;
            return isValidPoint(*point);
        }
        remaining -= segment.first;
    }
    return false;
}

bool SwarmFollowPathTrail::isValidPoint(
    const SwarmFollowPathPoint &point) noexcept
{
    if (!std::isfinite(point.latitudeDegrees)
        || !std::isfinite(point.longitudeDegrees)
        || !std::isfinite(point.relativeAltitudeM)
        || point.latitudeDegrees < -90.0 || point.latitudeDegrees > 90.0
        || point.longitudeDegrees < -180.0 || point.longitudeDegrees > 180.0
        || std::abs(point.relativeAltitudeM) > 100000.0) {
        return false;
    }
    const qint64 latitudeE7 = std::llround(point.latitudeDegrees * 1.0e7);
    const qint64 longitudeE7 = std::llround(point.longitudeDegrees * 1.0e7);
    return latitudeE7 != 0 || longitudeE7 != 0;
}

SwarmFollowPathTick SwarmFollowPathCore::buildTick(
    const SwarmFollowPathPlan &plan,
    const QList<SwarmTelemetrySnapshot> &snapshots,
    qint64 nowMs,
    SwarmFollowPathTrail *trail,
    int maximumAgeMs)
{
    if (!trail || nowMs < 0 || maximumAgeMs <= 0
        || !plan.leader.isValid()) {
        return stopped(QStringLiteral(
            "Follow Path plan, clock, or leader lease is invalid."), trail);
    }
    if (!std::isfinite(plan.separationM)
        || plan.separationM < MinimumSeparationM
        || plan.separationM > MaximumSeparationM) {
        return stopped(QStringLiteral(
            "Follow Path separation must be between 1 and 500 m."), trail);
    }
    if (plan.followers.isEmpty()) {
        return stopped(QStringLiteral(
            "Explicitly enable at least one Follow Path follower."), trail);
    }
    if (plan.followers.size()
        >= SwarmTelemetryRegistry::MaximumVehicleEndpoints) {
        return stopped(QStringLiteral(
            "Follow Path exceeds the exact swarm vehicle limit."), trail);
    }

    QVector<SwarmFollowPathFollower> ordered = plan.followers;
    std::sort(ordered.begin(), ordered.end(),
              [](const SwarmFollowPathFollower &left,
                 const SwarmFollowPathFollower &right) {
        return left.order < right.order;
    });
    for (int index = 0; index < ordered.size(); ++index) {
        const SwarmFollowPathFollower &follower = ordered.at(index);
        if (!follower.lease.isValid()) {
            return stopped(QStringLiteral(
                "A Follow Path follower lease is invalid."), trail);
        }
        if (follower.order < 1 || follower.order > MaximumOrder
            || (index > 0 && ordered.at(index - 1).order == follower.order)) {
            return stopped(QStringLiteral(
                "Follower order must be unique and between 1 and 100."), trail);
        }
        if (sameEndpoint(plan.leader, follower.lease)) {
            return stopped(QStringLiteral(
                "The leader was also selected as a follower."), trail);
        }
        for (int previous = 0; previous < index; ++previous) {
            if (sameEndpoint(follower.lease, ordered.at(previous).lease)) {
                return stopped(QStringLiteral(
                    "One exact vehicle endpoint has more than one follower order."),
                    trail);
            }
        }
    }

    const int expectedSnapshots = ordered.size() + 1;
    if (snapshots.size() != expectedSnapshots) {
        return stopped(QStringLiteral(
            "The exact Follow Path snapshot group does not match the plan."),
            trail);
    }
    for (int index = 0; index < snapshots.size(); ++index) {
        const SwarmVehicleInstanceLease &lease = snapshots.at(index).lease;
        if (!lease.isValid()) {
            return stopped(QStringLiteral(
                "The Follow Path snapshot group contains an invalid lease."),
                trail);
        }
        for (int previous = 0; previous < index; ++previous) {
            if (sameEndpoint(lease, snapshots.at(previous).lease)) {
                return stopped(QStringLiteral(
                    "The Follow Path snapshot group contains a duplicate endpoint."),
                    trail);
            }
        }
    }

    bool replacementFound = false;
    const SwarmTelemetrySnapshot *leader = snapshotFor(
        snapshots, plan.leader, &replacementFound);
    if (!leader) {
        return stopped(replacementFound
            ? QStringLiteral(
                "The Follow Path leader was reconnected or replaced.")
            : QStringLiteral(
                "The Follow Path leader is missing from the exact snapshot group."),
            trail);
    }
    if (!SwarmFormationCore::supportsFormation(*leader)) {
        return stopped(QStringLiteral(
            "The Follow Path leader is not a supported ArduPilot Plane, Copter, or Rover."),
            trail);
    }
    if (!freshPosition(*leader, nowMs, maximumAgeMs)) {
        return stopped(QStringLiteral(
            "The Follow Path leader position is unavailable or stale."), trail);
    }

    QVector<const SwarmTelemetrySnapshot *> followerSnapshots;
    followerSnapshots.reserve(ordered.size());
    for (const SwarmFollowPathFollower &planned : ordered) {
        replacementFound = false;
        const SwarmTelemetrySnapshot *follower = snapshotFor(
            snapshots, planned.lease, &replacementFound);
        if (!follower) {
            return stopped(replacementFound
                ? QStringLiteral(
                    "A Follow Path follower was reconnected or replaced.")
                : QStringLiteral(
                    "A Follow Path follower is missing from the exact snapshot group."),
                trail);
        }
        const SwarmVehicleFamily family = SwarmFormationCore::family(*follower);
        if (family == SwarmVehicleFamily::Plane) {
            return stopped(QStringLiteral(
                "ArduPlane Follow Path followers remain disabled until the exact guided-waypoint sender is ported."),
                trail);
        }
        if (!SwarmFormationCore::supportsPositionFollower(*follower)) {
            return stopped(QStringLiteral(
                "Follow Path followers must be supported ArduPilot Copter or Rover vehicles."),
                trail);
        }
        if (!freshPosition(*follower, nowMs, maximumAgeMs)) {
            return stopped(QStringLiteral(
                "Follower %1 position is unavailable or stale.")
                .arg(vehicleName(planned.lease)), trail);
        }
        if (!SwarmFlightMode::isExactGuided(*follower)) {
            return stopped(QStringLiteral(
                "Follower %1 is in %2, not the exact ArduPilot GUIDED mode.")
                .arg(vehicleName(planned.lease),
                     SwarmFlightMode::displayName(*follower)), trail);
        }
        followerSnapshots.append(follower);
    }

    const SwarmFollowPathPoint leaderPoint{
        leader->latitudeDegrees,
        leader->longitudeDegrees,
        leader->relativeAltitudeM};
    const SwarmFollowPathTrailUpdate update = trail->record(leaderPoint);
    const double requiredTrailM =
        static_cast<double>(ordered.constLast().order) * plan.separationM;
    if (update == SwarmFollowPathTrailUpdate::InvalidPoint) {
        return stopped(QStringLiteral(
            "The Follow Path leader position is invalid."), trail,
            requiredTrailM);
    }
    if (update == SwarmFollowPathTrailUpdate::ResetAfterJump) {
        return stopped(QStringLiteral(
            "Follow Path stopped because the leader position jumped by more than 500 m; the trail was reset."),
            trail, requiredTrailM);
    }

    QVector<SwarmFollowPathCommand> commands;
    commands.reserve(ordered.size());
    for (int index = 0; index < ordered.size(); ++index) {
        const SwarmFollowPathFollower &planned = ordered.at(index);
        const double distanceBehindM =
            static_cast<double>(planned.order) * plan.separationM;
        SwarmFollowPathPoint target;
        if (!trail->pointBehind(distanceBehindM, &target)) {
            SwarmFollowPathTick waiting;
            waiting.state = SwarmFollowPathTick::State::WaitingForTrail;
            waiting.detail = QStringLiteral(
                "Recording leader trail: %1 of %2 m required.")
                .arg(trail->lengthM(), 0, 'f', 1)
                .arg(requiredTrailM, 0, 'f', 1);
            waiting.availableTrailM = trail->lengthM();
            waiting.requiredTrailM = requiredTrailM;
            return waiting;
        }
        commands.append({followerSnapshots.at(index)->lease,
                         planned.order, distanceBehindM, target});
    }

    SwarmFollowPathTick active;
    active.state = SwarmFollowPathTick::State::Active;
    active.commands = commands;
    active.detail = QStringLiteral(
        "Follow Path active: %1 target(s) ready; trail %2 m.")
        .arg(commands.size()).arg(trail->lengthM(), 0, 'f', 1);
    active.availableTrailM = trail->lengthM();
    active.requiredTrailM = requiredTrailM;
    return active;
}
