#include "SwarmWaypointLeaderCore.h"

#include "comm/SwarmFlightMode.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <algorithm>
#include <cmath>
#include <cstring>
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

double wrappedRadians(double radians)
{
    while (radians > Pi) {
        radians -= 2.0 * Pi;
    }
    while (radians < -Pi) {
        radians += 2.0 * Pi;
    }
    return radians;
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

SwarmFollowPathPoint pointOf(const SwarmWaypointLeaderVehicleState &vehicle)
{
    return {vehicle.telemetry.latitudeDegrees,
            vehicle.telemetry.longitudeDegrees,
            vehicle.telemetry.relativeAltitudeM};
}

bool sameEndpoint(const SwarmVehicleInstanceLease &left,
                  const SwarmVehicleInstanceLease &right)
{
    return left.endpoint.sameIdentity(right.endpoint);
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

QString roleForIndex(int index, const QVector<int> &orders)
{
    return index == 0
        ? QStringLiteral("Air master")
        : QStringLiteral("Follower #%1").arg(orders.value(index));
}

bool liveAutopilot(const SwarmTelemetrySnapshot &snapshot,
                   qint64 nowMs, int maximumAgeMs)
{
    return snapshot.lease.endpoint.componentId == MAV_COMP_ID_AUTOPILOT1
        && snapshot.heartbeatValid
        && validAge(snapshot.heartbeatObservedMs, nowMs, maximumAgeMs)
        && snapshot.autopilot != MAV_AUTOPILOT_INVALID
        && snapshot.vehicleType != MAV_TYPE_GCS;
}

bool liveArduCopter(const SwarmTelemetrySnapshot &snapshot,
                    qint64 nowMs, int maximumAgeMs)
{
    return liveAutopilot(snapshot, nowMs, maximumAgeMs)
        && snapshot.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA
        && SwarmFlightMode::isCopter(snapshot.vehicleType);
}

bool freshPosition(const SwarmTelemetrySnapshot &snapshot,
                   qint64 nowMs, int maximumAgeMs)
{
    return snapshot.positionValid
        && validAge(snapshot.positionObservedMs, nowMs, maximumAgeMs)
        && SwarmFollowPathTrail::isValidPoint(
            {snapshot.latitudeDegrees, snapshot.longitudeDegrees,
             snapshot.relativeAltitudeM});
}

bool freshVelocity(const SwarmTelemetrySnapshot &snapshot,
                   qint64 nowMs, int maximumAgeMs)
{
    return snapshot.velocityValid
        && validAge(snapshot.positionObservedMs, nowMs, maximumAgeMs)
        && std::isfinite(snapshot.velocityNorthMps)
        && std::isfinite(snapshot.velocityEastMps)
        && std::isfinite(snapshot.velocityDownMps);
}

const SwarmWaypointLeaderVehicleState *stateFor(
    const QList<SwarmWaypointLeaderVehicleState> &vehicles,
    const SwarmVehicleInstanceLease &lease,
    bool *replacementFound = nullptr)
{
    if (replacementFound) {
        *replacementFound = false;
    }
    for (const SwarmWaypointLeaderVehicleState &vehicle : vehicles) {
        if (vehicle.telemetry.lease.sameInstance(lease)) {
            return &vehicle;
        }
        if (replacementFound
            && sameEndpoint(vehicle.telemetry.lease, lease)) {
            *replacementFound = true;
        }
    }
    return nullptr;
}

bool isExactRtl(const SwarmTelemetrySnapshot &snapshot)
{
    return snapshot.heartbeatValid
        && snapshot.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA
        && SwarmFlightMode::isCopter(snapshot.vehicleType)
        && (snapshot.baseMode & MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) != 0
        && snapshot.customMode == 6U;
}

double horizontalDistance(const SwarmFollowPathPoint &left,
                          const SwarmFollowPathPoint &right)
{
    return distanceAndBearing(left, right).first;
}

bool finiteInRange(double value, double minimum, double maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool isRelativeAltitudeFrame(int frame)
{
    return frame == MAV_FRAME_GLOBAL_RELATIVE_ALT
        || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
}

void clearError(QString *error)
{
    if (error) {
        error->clear();
    }
}

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

qint32 floatBits(float value)
{
    static_assert(sizeof(float) == sizeof(qint32),
                  "Waypoint mission altitude requires IEEE-754 float bits");
    qint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

QString batchFailureName(SwarmWaypointLeaderBatchResult result)
{
    switch (result) {
    case SwarmWaypointLeaderBatchResult::Succeeded:
        return QStringLiteral("succeeded");
    case SwarmWaypointLeaderBatchResult::Rejected:
        return QStringLiteral("was rejected");
    case SwarmWaypointLeaderBatchResult::Partial:
        return QStringLiteral("was only partially applied");
    case SwarmWaypointLeaderBatchResult::Cancelled:
        return QStringLiteral("was cancelled");
    case SwarmWaypointLeaderBatchResult::OutcomeUncertain:
        return QStringLiteral("has an uncertain outcome");
    }
    return QStringLiteral("failed");
}

} // namespace

bool SwarmWaypointLeaderCommandIntent::requiresVehicleAcknowledgement()
    const noexcept
{
    return kind == Kind::SetParameter || kind == Kind::SetModeGuided
        || kind == Kind::Arm || kind == Kind::Takeoff
        || kind == Kind::SetModeRtl;
}

QString SwarmWaypointLeaderMissionPath::signatureOf(
    const QVector<SwarmWaypointLeaderMissionItem> &items)
{
    QVector<SwarmWaypointLeaderMissionItem> ordered = items;
    std::sort(ordered.begin(), ordered.end(),
              [](const SwarmWaypointLeaderMissionItem &left,
                 const SwarmWaypointLeaderMissionItem &right) {
        return left.sequence < right.sequence;
    });

    QByteArray input;
    input.reserve(ordered.size() * 64);
    for (const SwarmWaypointLeaderMissionItem &item : ordered) {
        input.append(QByteArray::number(item.sequence));
        input.append(':');
        input.append(QByteArray::number(item.command));
        input.append(':');
        input.append(QByteArray::number(item.frame));
        input.append(':');
        input.append(QByteArray::number(item.latitudeE7));
        input.append(':');
        input.append(QByteArray::number(item.longitudeE7));
        input.append(':');
        input.append(QByteArray::number(floatBits(item.relativeAltitudeM)));
        input.append(';');
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(input, QCryptographicHash::Sha256)
            .toHex().toUpper());
}

bool SwarmWaypointLeaderMissionPath::build(
    const SwarmWaypointLeaderMissionSnapshot &mission,
    SwarmWaypointLeaderMissionPath *path, QString *error)
{
    clearError(error);
    if (path) {
        *path = SwarmWaypointLeaderMissionPath();
    }
    if (!path) {
        return fail(error, QStringLiteral(
            "The Waypoint Leader mission-path output is unavailable."));
    }
    if (!mission.airMaster.isValid()) {
        return fail(error, QStringLiteral(
            "The air-master mission lease is invalid."));
    }
    if (mission.items.size() < 2) {
        return fail(error, QStringLiteral(
            "The air master has no downloaded waypoint mission."));
    }

    QVector<SwarmWaypointLeaderMissionItem> ordered = mission.items;
    std::sort(ordered.begin(), ordered.end(),
              [](const SwarmWaypointLeaderMissionItem &left,
                 const SwarmWaypointLeaderMissionItem &right) {
        return left.sequence < right.sequence;
    });
    for (int index = 1; index < ordered.size(); ++index) {
        if (ordered.at(index - 1).sequence == ordered.at(index).sequence) {
            return fail(error, QStringLiteral(
                "The air-master mission contains a duplicate sequence."));
        }
    }

    QVector<Vertex> vertices;
    double cumulativeM = 0.0;
    SwarmWaypointLeaderMissionItem previous = ordered.constFirst();
    bool previousIsHorizontalOriginOnly =
        !isRelativeAltitudeFrame(previous.frame);
    if (previousIsHorizontalOriginOnly && previous.sequence != 0) {
        return fail(error, QStringLiteral(
            "The air-master mission starts at sequence %1 in unsupported frame %2; only sequence 0 may provide a horizontal origin.")
            .arg(previous.sequence)
            .arg(previous.frame));
    }
    for (int index = 1; index < ordered.size(); ++index) {
        const SwarmWaypointLeaderMissionItem item = ordered.at(index);
        if (item.command != MAV_CMD_NAV_WAYPOINT
            && item.command != MAV_CMD_NAV_SPLINE_WAYPOINT) {
            continue;
        }
        if (!isRelativeAltitudeFrame(item.frame)) {
            return fail(error, QStringLiteral(
                "The air-master mission navigation destination at sequence %1 uses unsupported frame %2; only GLOBAL_RELATIVE_ALT and GLOBAL_RELATIVE_ALT_INT are accepted.")
                .arg(item.sequence)
                .arg(item.frame));
        }

        SwarmFollowPathPoint startPoint{
            previous.latitudeE7 / 1.0e7,
            previous.longitudeE7 / 1.0e7,
            previous.relativeAltitudeM};
        SwarmFollowPathPoint destination{
            item.latitudeE7 / 1.0e7,
            item.longitudeE7 / 1.0e7,
            item.relativeAltitudeM};
        if (previousIsHorizontalOriginOnly) {
            startPoint.relativeAltitudeM = destination.relativeAltitudeM;
        }
        if (!SwarmFollowPathTrail::isValidPoint(startPoint)
            || !SwarmFollowPathTrail::isValidPoint(destination)) {
            return fail(error, QStringLiteral(
                "The air-master mission contains an invalid waypoint coordinate or altitude."));
        }

        const double segmentM = horizontalDistance(startPoint, destination);
        if (!std::isfinite(segmentM)) {
            return fail(error, QStringLiteral(
                "The air-master mission contains an invalid waypoint leg."));
        }
        if (segmentM <= SwarmFollowPathTrail::MinimumSampleDistanceM) {
            previous = item;
            previousIsHorizontalOriginOnly = false;
            continue;
        }
        if (segmentM > MaximumMissionSegmentM) {
            return fail(error, QStringLiteral(
                "The air-master mission leg ending at sequence %1 exceeds the %2 m maximum.")
                .arg(item.sequence)
                .arg(MaximumMissionSegmentM, 0, 'f', 0));
        }

        if (vertices.isEmpty()) {
            vertices.append({0.0, startPoint});
        } else if (horizontalDistance(vertices.constLast().point, startPoint)
                   > 0.5) {
            return fail(error, QStringLiteral(
                "The air-master mission has a discontinuous waypoint path."));
        }
        cumulativeM += segmentM;
        vertices.append({cumulativeM, destination});
        previous = item;
        previousIsHorizontalOriginOnly = false;
    }

    if (vertices.size() < 2
        || cumulativeM <= SwarmFollowPathTrail::MinimumSampleDistanceM) {
        return fail(error, QStringLiteral(
            "The air-master mission contains no usable waypoint legs."));
    }

    path->m_vertices = std::move(vertices);
    path->m_lengthM = cumulativeM;
    path->m_signature = signatureOf(ordered);
    return true;
}

QVector<SwarmWaypointLeaderProfilePoint>
SwarmWaypointLeaderMissionPath::profile() const
{
    QVector<SwarmWaypointLeaderProfilePoint> result;
    result.reserve(m_vertices.size());
    for (const Vertex &vertex : m_vertices) {
        result.append({vertex.distanceM, vertex.point.relativeAltitudeM});
    }
    return result;
}

SwarmFollowPathPoint SwarmWaypointLeaderMissionPath::start() const
{
    return m_vertices.isEmpty() ? SwarmFollowPathPoint()
                                : m_vertices.constFirst().point;
}

SwarmFollowPathPoint SwarmWaypointLeaderMissionPath::end() const
{
    return m_vertices.isEmpty() ? SwarmFollowPathPoint()
                                : m_vertices.constLast().point;
}

bool SwarmWaypointLeaderMissionPath::closest(
    const SwarmFollowPathPoint &location,
    double *distanceAlongM, double *distanceFromPathM) const
{
    if (distanceAlongM) {
        *distanceAlongM = 0.0;
    }
    if (distanceFromPathM) {
        *distanceFromPathM = std::numeric_limits<double>::infinity();
    }
    if (!distanceAlongM || !distanceFromPathM || !isValid()
        || !SwarmFollowPathTrail::isValidPoint(location)) {
        return false;
    }

    for (int index = 1; index < m_vertices.size(); ++index) {
        const Vertex &startVertex = m_vertices.at(index - 1);
        const Vertex &endVertex = m_vertices.at(index);
        const double segmentLengthM =
            endVertex.distanceM - startVertex.distanceM;
        const std::pair<double, double> toLocation = distanceAndBearing(
            startVertex.point, location);
        const std::pair<double, double> segment = distanceAndBearing(
            startVertex.point, endVertex.point);
        const double alongM = std::max(0.0, std::min(segmentLengthM,
            std::cos(wrappedRadians(toLocation.second - segment.second))
                * toLocation.first));
        const SwarmFollowPathPoint projectedPoint = projected(
            startVertex.point, segment.second, alongM);
        const double crossTrackM =
            horizontalDistance(projectedPoint, location);
        if (crossTrackM < *distanceFromPathM) {
            *distanceFromPathM = crossTrackM;
            *distanceAlongM = startVertex.distanceM + alongM;
        }
    }
    return std::isfinite(*distanceFromPathM);
}

bool SwarmWaypointLeaderMissionPath::pointAt(
    double distanceM, SwarmFollowPathPoint *point) const
{
    if (point) {
        *point = SwarmFollowPathPoint();
    }
    if (!point || !isValid() || !std::isfinite(distanceM)
        || distanceM < 0.0 || distanceM > m_lengthM) {
        return false;
    }
    if (distanceM <= std::numeric_limits<double>::epsilon()) {
        *point = start();
        return true;
    }
    for (int index = 1; index < m_vertices.size(); ++index) {
        const Vertex &startVertex = m_vertices.at(index - 1);
        const Vertex &endVertex = m_vertices.at(index);
        if (distanceM > endVertex.distanceM
            && index < m_vertices.size() - 1) {
            continue;
        }
        const double segmentLengthM =
            endVertex.distanceM - startVertex.distanceM;
        const double offsetM = std::max(0.0, std::min(
            segmentLengthM, distanceM - startVertex.distanceM));
        const double ratio = segmentLengthM
                <= std::numeric_limits<double>::epsilon()
            ? 0.0 : offsetM / segmentLengthM;
        const double bearing = distanceAndBearing(
            startVertex.point, endVertex.point).second;
        *point = projected(startVertex.point, bearing, offsetM);
        point->relativeAltitudeM = startVertex.point.relativeAltitudeM
            + (endVertex.point.relativeAltitudeM
               - startVertex.point.relativeAltitudeM) * ratio;
        return SwarmFollowPathTrail::isValidPoint(*point);
    }
    return false;
}

bool SwarmWaypointLeaderMissionPath::lineTargets(
    const SwarmFollowPathPoint &reference, double leadM,
    double separationM, int count,
    QVector<SwarmFollowPathPoint> *targets) const
{
    if (targets) {
        targets->clear();
    }
    double referenceDistanceM = 0.0;
    double offPathM = 0.0;
    if (!targets || count <= 0 || !std::isfinite(leadM)
        || !std::isfinite(separationM) || separationM <= 0.0
        || !closest(reference, &referenceDistanceM, &offPathM)) {
        return false;
    }
    targets->reserve(count);
    for (int index = 0; index < count; ++index) {
        SwarmFollowPathPoint target;
        const double distanceM = referenceDistanceM + leadM
            - static_cast<double>(index) * separationM;
        if (!pointAt(distanceM, &target)) {
            targets->clear();
            return false;
        }
        targets->append(target);
    }
    return true;
}

bool SwarmWaypointLeaderMissionPath::vTargets(
    const SwarmFollowPathPoint &reference, double leadM,
    double separationM, int count,
    QVector<SwarmFollowPathPoint> *targets) const
{
    if (targets) {
        targets->clear();
    }
    double referenceDistanceM = 0.0;
    double offPathM = 0.0;
    if (!targets || count <= 0 || !std::isfinite(leadM)
        || !std::isfinite(separationM) || separationM <= 0.0
        || !closest(reference, &referenceDistanceM, &offPathM)) {
        return false;
    }
    SwarmFollowPathPoint front;
    if (!pointAt(referenceDistanceM + leadM, &front)) {
        return false;
    }
    targets->reserve(count);
    targets->append(front);
    for (int index = 1; index < count; ++index) {
        const int rank = (index + 1) / 2;
        SwarmFollowPathPoint center;
        if (!pointAt(referenceDistanceM + leadM
                     - static_cast<double>(rank) * separationM,
                     &center)) {
            targets->clear();
            return false;
        }
        const double forwardBearing =
            distanceAndBearing(center, front).second;
        const double lateralBearing = forwardBearing
            + (index % 2 == 1 ? Pi / 2.0 : -Pi / 2.0);
        SwarmFollowPathPoint lateral = projected(
            center, lateralBearing,
            separationM / 2.0 * static_cast<double>(rank));
        lateral.relativeAltitudeM = center.relativeAltitudeM;
        targets->append(lateral);
    }
    return true;
}

QString SwarmWaypointLeaderCore::modeName(SwarmWaypointLeaderMode mode)
{
    switch (mode) {
    case SwarmWaypointLeaderMode::Idle:
        return QStringLiteral("Idle");
    case SwarmWaypointLeaderMode::Takeoff:
        return QStringLiteral("Takeoff");
    case SwarmWaypointLeaderMode::FlyToGroundMaster:
        return QStringLiteral("FlyToGroundMaster");
    case SwarmWaypointLeaderMode::FollowGroundMaster:
        return QStringLiteral("FollowGroundMaster");
    case SwarmWaypointLeaderMode::ReturnAlongMission:
        return QStringLiteral("ReturnAlongMission");
    case SwarmWaypointLeaderMode::LandAltitude:
        return QStringLiteral("LandAltitude");
    case SwarmWaypointLeaderMode::Landing:
        return QStringLiteral("Landing");
    }
    return QStringLiteral("Invalid");
}

bool SwarmWaypointLeaderCore::validateSettings(
    const SwarmWaypointLeaderSettings &settings, QString *error)
{
    clearError(error);
    if (!finiteInRange(settings.separationM,
                       MinimumSeparationM, MaximumSeparationM)) {
        return fail(error, QStringLiteral(
            "Waypoint Leader separation must be between 2 and 500 m."));
    }
    if (!finiteInRange(settings.leadM, MinimumLeadM, MaximumLeadM)) {
        return fail(error, QStringLiteral(
            "Waypoint Leader lead must be between -500 and 5000 m."));
    }
    if (!finiteInRange(settings.offPathTriggerM,
                       MinimumOffPathTriggerM,
                       MaximumOffPathTriggerM)) {
        return fail(error, QStringLiteral(
            "Waypoint Leader off-path trigger must be between 1 and 5000 m."));
    }
    if (!finiteInRange(settings.takeoffLandAltitudeSeparationM,
                       MinimumAltitudeSeparationM,
                       MaximumAltitudeSeparationM)) {
        return fail(error, QStringLiteral(
            "Waypoint Leader altitude separation must be between 1 and 100 m."));
    }
    if (!finiteInRange(settings.navigationAccelerationMps2,
                       MinimumNavigationAccelerationMps2,
                       MaximumNavigationAccelerationMps2)) {
        return fail(error, QStringLiteral(
            "Waypoint Leader navigation acceleration must be between 0.1 and 100 m/s²."));
    }
    return true;
}

void SwarmWaypointLeaderCore::reset()
{
    if (m_pending) {
        m_requestedMode.reset();
        m_collisionCancellationPending = false;
        m_stopAfterPending = QStringLiteral(
            "Waypoint Leader stopped after its pending command batch was cancelled for reset.");
        return;
    }
    m_mode = SwarmWaypointLeaderMode::Idle;
    m_requestedMode.reset();
    m_capturedPlan.reset();
    m_pending.reset();
    m_takeoffIssued.clear();
    m_flightStateBarriers.clear();
    m_lastTargets.clear();
    m_nextBatchId = 1;
    m_streamsRequested = false;
    m_initialParametersConfigured = false;
    m_returnParametersConfigured = false;
    m_rtlIssued = false;
    m_collisionCancellationPending = false;
    m_stopAfterPending.clear();
    m_stopped = false;
    m_terminalStatus.clear();
}

bool SwarmWaypointLeaderCore::requestMode(
    SwarmWaypointLeaderMode mode, QString *error)
{
    clearError(error);
    if (mode != SwarmWaypointLeaderMode::Idle
        && mode != SwarmWaypointLeaderMode::ReturnAlongMission
        && mode != SwarmWaypointLeaderMode::LandAltitude) {
        return fail(error, QStringLiteral(
            "Only Idle, ReturnAlongMission, or LandAltitude can be requested."));
    }
    if (m_stopped) {
        return fail(error, QStringLiteral(
            "Waypoint Leader is stopped; create or reset the run first."));
    }
    m_requestedMode = mode;
    return true;
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::stop(
    const QString &status)
{
    m_requestedMode.reset();
    if (m_pending) {
        m_stopAfterPending = status;
        m_collisionCancellationPending = false;
        SwarmWaypointLeaderTick result;
        result.state = SwarmWaypointLeaderTick::State::Cancelling;
        result.mode = m_mode;
        result.status = QStringLiteral(
            "%1 Cancelling the pending command batch first.").arg(status);
        result.batchId = m_pending->id;
        result.cancelBatchId = m_pending->id;
        return result;
    }
    m_stopped = true;
    m_terminalStatus = status;
    SwarmWaypointLeaderTick result;
    result.state = SwarmWaypointLeaderTick::State::Stopped;
    result.mode = m_mode;
    result.status = status;
    return result;
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::active(
    const QString &status) const
{
    SwarmWaypointLeaderTick result;
    result.state = SwarmWaypointLeaderTick::State::Active;
    result.mode = m_mode;
    result.status = status;
    return result;
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::urgent(
    const QString &status,
    QVector<SwarmWaypointLeaderCommandIntent> intents) const
{
    SwarmWaypointLeaderTick result;
    result.state = SwarmWaypointLeaderTick::State::Active;
    result.mode = m_mode;
    result.status = status;
    result.urgent = true;
    result.intents = std::move(intents);
    return result;
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::issue(
    const QString &status,
    QVector<SwarmWaypointLeaderCommandIntent> intents,
    PendingBatch effect)
{
    if (intents.isEmpty()) {
        return active(status);
    }
    effect.id = nextBatchId();
    m_pending = effect;

    SwarmWaypointLeaderTick result;
    result.state = SwarmWaypointLeaderTick::State::Active;
    result.mode = m_mode;
    result.status = status;
    result.batchId = effect.id;
    result.intents = std::move(intents);
    return result;
}

quint64 SwarmWaypointLeaderCore::nextBatchId()
{
    quint64 result = m_nextBatchId++;
    if (result == 0) {
        result = m_nextBatchId++;
    }
    if (m_nextBatchId == 0) {
        m_nextBatchId = 1;
    }
    return result;
}

bool SwarmWaypointLeaderCore::completeBatch(
    quint64 batchId, SwarmWaypointLeaderBatchResult result,
    const QString &detail)
{
    if (!m_pending || batchId == 0 || m_pending->id != batchId) {
        return false;
    }
    const PendingBatch completed = *m_pending;
    m_pending.reset();

    if (!m_stopAfterPending.isEmpty()) {
        m_stopped = true;
        m_requestedMode.reset();
        m_collisionCancellationPending = false;
        m_terminalStatus = m_stopAfterPending;
        m_stopAfterPending.clear();
        return true;
    }

    if (m_collisionCancellationPending
        && result == SwarmWaypointLeaderBatchResult::Cancelled) {
        m_collisionCancellationPending = false;
        return true;
    }
    m_collisionCancellationPending = false;

    if (result != SwarmWaypointLeaderBatchResult::Succeeded) {
        m_stopped = true;
        m_requestedMode.reset();
        m_terminalStatus = QStringLiteral(
            "Waypoint Leader stopped because %1 %2.")
            .arg(completed.description, batchFailureName(result));
        if (!detail.trimmed().isEmpty()) {
            m_terminalStatus += QStringLiteral(" %1").arg(detail.trimmed());
        }
        return true;
    }

    if (completed.markStreamsRequested) {
        m_streamsRequested = true;
    }
    if (completed.markInitialParametersConfigured) {
        m_initialParametersConfigured = true;
    }
    if (completed.markReturnParametersConfigured) {
        m_returnParametersConfigured = true;
    }
    if (completed.markRtlIssued) {
        m_rtlIssued = true;
    }
    for (const SwarmVehicleInstanceLease &lease :
         completed.markTakeoffIssued) {
        if (!takeoffWasIssued(lease)) {
            m_takeoffIssued.append(lease);
        }
    }
    for (const PendingBatch::FlightStateBarrier &barrier :
         completed.markFlightStateBarriers) {
        setFlightStateBarrier(barrier);
    }
    for (const TargetRecord &record : completed.targetUpdates) {
        setLastTarget(record);
    }
    if (completed.modeAfterSuccess) {
        m_mode = *completed.modeAfterSuccess;
    }
    return true;
}

bool SwarmWaypointLeaderCore::sameCapturedPlan(
    const SwarmWaypointLeaderPlan &plan) const
{
    if (!m_capturedPlan) {
        return false;
    }
    const SwarmWaypointLeaderPlan &captured = *m_capturedPlan;
    if (!captured.groundMaster.sameInstance(plan.groundMaster)
        || !captured.airMaster.sameInstance(plan.airMaster)
        || captured.missionSignature != plan.missionSignature
        || captured.missionContentGeneration
            != plan.missionContentGeneration
        || captured.missionContentDigest != plan.missionContentDigest
        || captured.settings.separationM != plan.settings.separationM
        || captured.settings.leadM != plan.settings.leadM
        || captured.settings.offPathTriggerM
            != plan.settings.offPathTriggerM
        || captured.settings.takeoffLandAltitudeSeparationM
            != plan.settings.takeoffLandAltitudeSeparationM
        || captured.settings.navigationAccelerationMps2
            != plan.settings.navigationAccelerationMps2
        || captured.settings.vFormation != plan.settings.vFormation
        || captured.settings.altitudeInterleave
            != plan.settings.altitudeInterleave
        || captured.followers.size() != plan.followers.size()) {
        return false;
    }

    QVector<SwarmWaypointLeaderFollower> expected = captured.followers;
    QVector<SwarmWaypointLeaderFollower> actual = plan.followers;
    const auto byOrder = [](const SwarmWaypointLeaderFollower &left,
                            const SwarmWaypointLeaderFollower &right) {
        return left.order < right.order;
    };
    std::sort(expected.begin(), expected.end(), byOrder);
    std::sort(actual.begin(), actual.end(), byOrder);
    for (int index = 0; index < expected.size(); ++index) {
        if (expected.at(index).order != actual.at(index).order
            || !expected.at(index).lease.sameInstance(
                actual.at(index).lease)) {
            return false;
        }
    }
    return true;
}

bool SwarmWaypointLeaderCore::resolve(
    const SwarmWaypointLeaderPlan &plan,
    const QList<SwarmWaypointLeaderVehicleState> &vehicles,
    const SwarmWaypointLeaderMissionSnapshot &mission,
    qint64 nowMs, int maximumAgeMs,
    ResolvedGroup *group, QString *error) const
{
    clearError(error);
    if (group) {
        *group = ResolvedGroup();
    }
    if (!group || nowMs < 0 || maximumAgeMs <= 0) {
        return fail(error, QStringLiteral(
            "the exact telemetry clock or group output is invalid."));
    }
    if (!validateSettings(plan.settings, error)) {
        return false;
    }
    if (!plan.groundMaster.isValid() || !plan.airMaster.isValid()) {
        return fail(error, QStringLiteral("a master lease is invalid."));
    }
    if (sameEndpoint(plan.groundMaster, plan.airMaster)) {
        return fail(error, QStringLiteral(
            "ground master and air master must be different vehicles."));
    }
    if (plan.followers.size() > MaximumOrder) {
        return fail(error, QStringLiteral(
            "more than 20 Waypoint Leader followers were selected."));
    }

    QVector<SwarmWaypointLeaderFollower> orderedFollowers = plan.followers;
    std::sort(orderedFollowers.begin(), orderedFollowers.end(),
              [](const SwarmWaypointLeaderFollower &left,
                 const SwarmWaypointLeaderFollower &right) {
        return left.order < right.order;
    });
    QSet<VehicleEndpoint> endpoints;
    endpoints.insert(plan.groundMaster.endpoint);
    endpoints.insert(plan.airMaster.endpoint);
    int previousOrder = -1;
    for (const SwarmWaypointLeaderFollower &follower : orderedFollowers) {
        if (!follower.lease.isValid()) {
            return fail(error, QStringLiteral("a follower lease is invalid."));
        }
        if (follower.order < 1 || follower.order > MaximumOrder
            || follower.order == previousOrder) {
            return fail(error, QStringLiteral(
                "follower order must be unique and between 1 and 20."));
        }
        if (endpoints.contains(follower.lease.endpoint)) {
            return fail(error, QStringLiteral(
                "a master was also selected as follower, or a follower is duplicated."));
        }
        endpoints.insert(follower.lease.endpoint);
        previousOrder = follower.order;
    }

    if (vehicles.size() != orderedFollowers.size() + 2) {
        return fail(error, QStringLiteral(
            "the exact Waypoint Leader snapshot group does not match the plan."));
    }
    QSet<VehicleEndpoint> snapshotEndpoints;
    for (const SwarmWaypointLeaderVehicleState &vehicle : vehicles) {
        if (!vehicle.telemetry.lease.isValid()
            || snapshotEndpoints.contains(vehicle.telemetry.lease.endpoint)) {
            return fail(error, QStringLiteral(
                "the exact snapshot group contains an invalid or duplicate endpoint."));
        }
        snapshotEndpoints.insert(vehicle.telemetry.lease.endpoint);
    }

    bool replacement = false;
    group->ground = stateFor(vehicles, plan.groundMaster, &replacement);
    if (!group->ground) {
        return fail(error, replacement
            ? QStringLiteral("the ground master was reconnected or replaced.")
            : QStringLiteral("the ground master is missing from the exact group."));
    }
    if (!liveAutopilot(group->ground->telemetry, nowMs, maximumAgeMs)) {
        return fail(error, QStringLiteral(
            "the ground master is not a live autopilot."));
    }
    if (!freshPosition(group->ground->telemetry, nowMs, maximumAgeMs)) {
        return fail(error, QStringLiteral(
            "the ground-master position is unavailable or stale."));
    }
    if (!freshVelocity(group->ground->telemetry, nowMs, maximumAgeMs)) {
        return fail(error, QStringLiteral(
            "the ground-master velocity is unavailable or stale."));
    }

    replacement = false;
    group->air = stateFor(vehicles, plan.airMaster, &replacement);
    if (!group->air) {
        return fail(error, replacement
            ? QStringLiteral("the air master was reconnected or replaced.")
            : QStringLiteral("the air master is missing from the exact group."));
    }
    if (!liveArduCopter(group->air->telemetry, nowMs, maximumAgeMs)) {
        return fail(error, QStringLiteral(
            "the air master must be a live ArduCopter autopilot."));
    }
    if (!freshPosition(group->air->telemetry, nowMs, maximumAgeMs)) {
        return fail(error, QStringLiteral(
            "the air-master position is unavailable or stale."));
    }
    if (!freshVelocity(group->air->telemetry, nowMs, maximumAgeMs)) {
        return fail(error, QStringLiteral(
            "the air-master velocity is unavailable or stale."));
    }

    if (mission.missionType != MAV_MISSION_TYPE_MISSION) {
        return fail(error, QStringLiteral(
            "the confirmed Waypoint Leader snapshot is not the main mission type."));
    }
    if (mission.contentGeneration == 0
        || mission.contentDigest.isEmpty()
        || plan.missionContentGeneration == 0
        || plan.missionContentDigest.isEmpty()
        || mission.contentGeneration != plan.missionContentGeneration
        || mission.contentDigest != plan.missionContentDigest) {
        return fail(error, QStringLiteral(
            "the exact mission-cache lease is missing, stale, or does not match the plan."));
    }
    if (!mission.airMaster.sameInstance(plan.airMaster)) {
        return fail(error, sameEndpoint(mission.airMaster, plan.airMaster)
            ? QStringLiteral(
                "the mission belongs to a replaced air-master instance.")
            : QStringLiteral(
                "the mission does not belong to the selected air master."));
    }
    if (!SwarmWaypointLeaderMissionPath::build(
            mission, &group->path, error)) {
        return false;
    }
    if (group->path.signature() != plan.missionSignature) {
        return fail(error, QStringLiteral(
            "the air-master mission changed after the plan was confirmed."));
    }

    group->flight.reserve(orderedFollowers.size() + 1);
    group->flightOrders.reserve(orderedFollowers.size() + 1);
    group->flight.append(group->air);
    group->flightOrders.append(0);
    for (const SwarmWaypointLeaderFollower &follower : orderedFollowers) {
        replacement = false;
        const SwarmWaypointLeaderVehicleState *state = stateFor(
            vehicles, follower.lease, &replacement);
        if (!state) {
            return fail(error, replacement
                ? QStringLiteral("a follower was reconnected or replaced.")
                : QStringLiteral("a follower is missing from the exact group."));
        }
        if (!liveArduCopter(state->telemetry, nowMs, maximumAgeMs)) {
            return fail(error, QStringLiteral(
                "follower %1 must be a live ArduCopter autopilot.")
                .arg(vehicleName(follower.lease)));
        }
        if (!freshPosition(state->telemetry, nowMs, maximumAgeMs)) {
            return fail(error, QStringLiteral(
                "follower %1 position is unavailable or stale.")
                .arg(vehicleName(follower.lease)));
        }
        if (!freshVelocity(state->telemetry, nowMs, maximumAgeMs)) {
            return fail(error, QStringLiteral(
                "follower %1 velocity is unavailable or stale.")
                .arg(vehicleName(follower.lease)));
        }
        group->flight.append(state);
        group->flightOrders.append(follower.order);
    }

    QVector<SwarmFollowPathPoint> staging;
    const double takeoffLeadM =
        static_cast<double>(group->flight.size() + 1)
        * plan.settings.separationM;
    if (!group->path.lineTargets(group->path.start(), takeoffLeadM,
            plan.settings.separationM, group->flight.size(), &staging)) {
        return fail(error, QStringLiteral(
            "the air-master mission is too short to stage all selected air vehicles."));
    }
    return true;
}

void SwarmWaypointLeaderCore::applyRequestedMode()
{
    if (!m_requestedMode) {
        return;
    }
    m_mode = *m_requestedMode;
    m_requestedMode.reset();
    if (m_mode == SwarmWaypointLeaderMode::Idle) {
        m_initialParametersConfigured = false;
        m_takeoffIssued.clear();
        m_flightStateBarriers.clear();
        m_lastTargets.clear();
        m_returnParametersConfigured = false;
        m_rtlIssued = false;
    } else if (m_mode == SwarmWaypointLeaderMode::ReturnAlongMission) {
        m_returnParametersConfigured = false;
    } else if (m_mode == SwarmWaypointLeaderMode::LandAltitude) {
        m_rtlIssued = false;
    }
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::tick(
    const SwarmWaypointLeaderPlan &plan,
    const QList<SwarmWaypointLeaderVehicleState> &vehicles,
    const SwarmWaypointLeaderMissionSnapshot &mission,
    qint64 nowMs, int maximumAgeMs)
{
    if (m_pending && !m_stopAfterPending.isEmpty()) {
        SwarmWaypointLeaderTick cancelling;
        cancelling.state = SwarmWaypointLeaderTick::State::Cancelling;
        cancelling.mode = m_mode;
        cancelling.status = QStringLiteral(
            "%1 Cancelling the pending command batch first.")
            .arg(m_stopAfterPending);
        cancelling.batchId = m_pending->id;
        cancelling.cancelBatchId = m_pending->id;
        return cancelling;
    }
    if (m_stopped) {
        SwarmWaypointLeaderTick terminal;
        terminal.state = SwarmWaypointLeaderTick::State::Stopped;
        terminal.mode = m_mode;
        terminal.status = m_terminalStatus;
        return terminal;
    }

    ResolvedGroup group;
    QString error;
    if (!resolve(plan, vehicles, mission, nowMs, maximumAgeMs,
                 &group, &error)) {
        return stop(QStringLiteral("Waypoint Leader stopped: %1").arg(error));
    }
    if (!m_capturedPlan) {
        m_capturedPlan = plan;
    } else if (!sameCapturedPlan(plan)) {
        return stop(QStringLiteral(
            "Waypoint Leader stopped because the confirmed plan changed."));
    }

    if (!m_pending) {
        applyRequestedMode();
    }
    const bool requiresExactGuided =
        m_mode == SwarmWaypointLeaderMode::FlyToGroundMaster
        || m_mode == SwarmWaypointLeaderMode::FollowGroundMaster
        || m_mode == SwarmWaypointLeaderMode::ReturnAlongMission
        || m_mode == SwarmWaypointLeaderMode::LandAltitude;
    const bool collisionControlEnabled =
        m_mode != SwarmWaypointLeaderMode::Landing;
    for (const SwarmWaypointLeaderVehicleState *vehicle : group.flight) {
        if ((requiresExactGuided || (collisionControlEnabled
                                     && vehicle->telemetry.armed))
            && !SwarmFlightMode::isExactGuided(vehicle->telemetry)) {
            return stop(QStringLiteral(
                "Waypoint Leader stopped: an air vehicle left exact GUIDED mode; position-target collision control is unavailable."));
        }
    }
    if (collisionControlEnabled) {
        if (const std::optional<SwarmWaypointLeaderTick> collision =
                collisionOverride(plan.settings, group, nowMs, maximumAgeMs)) {
            SwarmWaypointLeaderTick result = *collision;
            if (m_pending) {
                m_collisionCancellationPending = true;
                result.state = SwarmWaypointLeaderTick::State::Cancelling;
                result.batchId = m_pending->id;
                result.cancelBatchId = m_pending->id;
                result.status += QStringLiteral(
                    " Cancelling the normal command batch while the urgent target is sent.");
            }
            return result;
        }
    }

    if (m_pending) {
        SwarmWaypointLeaderTick waiting;
        waiting.state =
            SwarmWaypointLeaderTick::State::WaitingForCommandCompletion;
        waiting.mode = m_mode;
        waiting.status = QStringLiteral(
            "Waiting for terminal completion of %1.")
            .arg(m_pending->description);
        waiting.batchId = m_pending->id;
        return waiting;
    }

    switch (m_mode) {
    case SwarmWaypointLeaderMode::Idle:
        return initialize(plan.settings, group);
    case SwarmWaypointLeaderMode::Takeoff:
        return takeoff(plan.settings, group);
    case SwarmWaypointLeaderMode::FlyToGroundMaster:
        return flyToGroundMaster(plan.settings, group);
    case SwarmWaypointLeaderMode::FollowGroundMaster:
        return followGroundMaster(plan.settings, group);
    case SwarmWaypointLeaderMode::ReturnAlongMission:
        return returnAlongMission(plan.settings, group);
    case SwarmWaypointLeaderMode::LandAltitude:
        return landAltitude(plan.settings, group);
    case SwarmWaypointLeaderMode::Landing:
        return landing(group);
    }
    return stop(QStringLiteral("Waypoint Leader stopped: invalid state."));
}

void SwarmWaypointLeaderCore::appendNavigationAccelerationIntents(
    const QVector<const SwarmWaypointLeaderVehicleState *> &flight,
    const QVector<int> &flightOrders,
    double accelerationMps2,
    QVector<SwarmWaypointLeaderCommandIntent> *intents)
{
    if (!intents) {
        return;
    }
    for (int index = 0; index < flight.size(); ++index) {
        const SwarmWaypointLeaderVehicleState &vehicle = *flight.at(index);
        SwarmWaypointLeaderCommandIntent intent;
        intent.kind = SwarmWaypointLeaderCommandIntent::Kind::SetParameter;
        intent.lease = vehicle.telemetry.lease;
        intent.role = roleForIndex(index, flightOrders);
        intent.order = flightOrders.value(index);
        if (vehicle.availableParameters.contains(
                QStringLiteral("WPNAV_ACCEL"))) {
            intent.parameterName = QStringLiteral("WPNAV_ACCEL");
            intent.parameterValue = accelerationMps2 * 100.0;
        } else if (vehicle.availableParameters.contains(
                       QStringLiteral("WP_ACC"))) {
            intent.parameterName = QStringLiteral("WP_ACC");
            intent.parameterValue = accelerationMps2;
        } else {
            continue;
        }
        intents->append(intent);
    }
}

QVector<SwarmWaypointLeaderCommandIntent>
SwarmWaypointLeaderCore::positionIntents(
    const QVector<const SwarmWaypointLeaderVehicleState *> &flight,
    const QVector<int> &flightOrders,
    const QVector<SwarmFollowPathPoint> &targetPoints,
    const SwarmWaypointLeaderSettings &settings,
    const SwarmWaypointLeaderVelocity &velocity,
    QVector<TargetRecord> *baseTargetUpdates)
{
    QVector<SwarmWaypointLeaderCommandIntent> intents;
    if (baseTargetUpdates) {
        baseTargetUpdates->clear();
    }
    if (flight.size() != targetPoints.size()
        || flightOrders.size() != flight.size()) {
        return intents;
    }
    intents.reserve(flight.size());
    if (baseTargetUpdates) {
        baseTargetUpdates->reserve(flight.size());
    }
    for (int index = 0; index < flight.size(); ++index) {
        const SwarmWaypointLeaderVehicleState &vehicle = *flight.at(index);
        const SwarmFollowPathPoint target = targetPoints.at(index);
        SwarmWaypointLeaderCommandIntent intent;
        intent.kind = SwarmWaypointLeaderCommandIntent::Kind::PositionTarget;
        intent.lease = vehicle.telemetry.lease;
        intent.role = roleForIndex(index, flightOrders);
        intent.order = flightOrders.at(index);
        intent.target = target;
        intent.velocity = velocity;
        intents.append(intent);

        if (baseTargetUpdates) {
            SwarmFollowPathPoint baseTarget = target;
            if (settings.altitudeInterleave) {
                baseTarget.relativeAltitudeM -=
                    settings.takeoffLandAltitudeSeparationM * (index % 2);
            }
            baseTargetUpdates->append(
                {vehicle.telemetry.lease, baseTarget});
        }
    }
    return intents;
}

bool SwarmWaypointLeaderCore::targets(
    const SwarmWaypointLeaderMissionPath &path,
    const SwarmFollowPathPoint &reference,
    double leadM,
    const SwarmWaypointLeaderSettings &settings,
    int count,
    bool vFormation,
    QVector<SwarmFollowPathPoint> *result)
{
    const bool found = vFormation
        ? path.vTargets(reference, leadM, settings.separationM,
                        count, result)
        : path.lineTargets(reference, leadM, settings.separationM,
                           count, result);
    if (!found || !settings.altitudeInterleave) {
        return found;
    }
    for (int index = 0; index < result->size(); ++index) {
        (*result)[index].relativeAltitudeM +=
            settings.takeoffLandAltitudeSeparationM * (index % 2);
        if (!SwarmFollowPathTrail::isValidPoint(result->at(index))) {
            result->clear();
            return false;
        }
    }
    return true;
}

bool SwarmWaypointLeaderCore::takeoffWasIssued(
    const SwarmVehicleInstanceLease &lease) const
{
    return std::any_of(m_takeoffIssued.cbegin(), m_takeoffIssued.cend(),
                       [&lease](const SwarmVehicleInstanceLease &issued) {
        return issued.sameInstance(lease);
    });
}

const SwarmWaypointLeaderCore::PendingBatch::FlightStateBarrier *
SwarmWaypointLeaderCore::flightStateBarrier(
    const SwarmVehicleInstanceLease &lease) const
{
    const auto found = std::find_if(
        m_flightStateBarriers.cbegin(), m_flightStateBarriers.cend(),
        [&lease](const PendingBatch::FlightStateBarrier &barrier) {
            return barrier.lease.sameInstance(lease);
        });
    return found == m_flightStateBarriers.cend() ? nullptr : &*found;
}

void SwarmWaypointLeaderCore::setFlightStateBarrier(
    const PendingBatch::FlightStateBarrier &barrier)
{
    clearFlightStateBarrier(barrier.lease);
    m_flightStateBarriers.append(barrier);
}

void SwarmWaypointLeaderCore::clearFlightStateBarrier(
    const SwarmVehicleInstanceLease &lease)
{
    m_flightStateBarriers.erase(
        std::remove_if(
            m_flightStateBarriers.begin(), m_flightStateBarriers.end(),
            [&lease](const PendingBatch::FlightStateBarrier &barrier) {
                return barrier.lease.sameInstance(lease);
            }),
        m_flightStateBarriers.end());
}

bool SwarmWaypointLeaderCore::lastTarget(
    const SwarmVehicleInstanceLease &lease,
    SwarmFollowPathPoint *target) const
{
    if (target) {
        *target = SwarmFollowPathPoint();
    }
    if (!target) {
        return false;
    }
    for (const TargetRecord &record : m_lastTargets) {
        if (record.lease.sameInstance(lease)) {
            *target = record.target;
            return true;
        }
    }
    return false;
}

void SwarmWaypointLeaderCore::setLastTarget(const TargetRecord &record)
{
    for (TargetRecord &existing : m_lastTargets) {
        if (existing.lease.sameInstance(record.lease)) {
            existing.target = record.target;
            return;
        }
    }
    m_lastTargets.append(record);
}

void SwarmWaypointLeaderCore::prepareLandingTargets(
    const ResolvedGroup &group)
{
    for (const SwarmWaypointLeaderVehicleState *vehicle : group.flight) {
        SwarmFollowPathPoint ignored;
        if (!lastTarget(vehicle->telemetry.lease, &ignored)) {
            setLastTarget({vehicle->telemetry.lease, pointOf(*vehicle)});
        }
    }
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::initialize(
    const SwarmWaypointLeaderSettings &settings,
    const ResolvedGroup &group)
{
    Q_UNUSED(settings);
    QVector<SwarmWaypointLeaderCommandIntent> intents;
    PendingBatch effect;
    effect.description = QStringLiteral(
        "Waypoint Leader initialization");
    effect.modeAfterSuccess = SwarmWaypointLeaderMode::Takeoff;

    if (!m_streamsRequested) {
        SwarmWaypointLeaderCommandIntent groundStream;
        groundStream.kind =
            SwarmWaypointLeaderCommandIntent::Kind::RequestPositionStream;
        groundStream.lease = group.ground->telemetry.lease;
        groundStream.role = QStringLiteral("Ground master");
        groundStream.streamRateHz = PositionStreamRateHz;
        intents.append(groundStream);
        for (int index = 0; index < group.flight.size(); ++index) {
            SwarmWaypointLeaderCommandIntent stream;
            stream.kind = SwarmWaypointLeaderCommandIntent::Kind::
                RequestPositionStream;
            stream.lease = group.flight.at(index)->telemetry.lease;
            stream.role = roleForIndex(index, group.flightOrders);
            stream.order = group.flightOrders.at(index);
            stream.streamRateHz = PositionStreamRateHz;
            intents.append(stream);
        }
        effect.markStreamsRequested = true;
    }

    if (!m_initialParametersConfigured) {
        for (int index = 0; index < group.flight.size(); ++index) {
            const SwarmWaypointLeaderVehicleState &vehicle =
                *group.flight.at(index);
            const QString role = roleForIndex(index, group.flightOrders);
            const int order = group.flightOrders.at(index);

            SwarmWaypointLeaderCommandIntent rtl;
            rtl.kind = SwarmWaypointLeaderCommandIntent::Kind::SetParameter;
            rtl.lease = vehicle.telemetry.lease;
            rtl.role = role;
            rtl.order = order;
            rtl.parameterValue = 0.0;
            if (vehicle.availableParameters.contains(
                    QStringLiteral("RTL_ALT"))) {
                rtl.parameterName = QStringLiteral("RTL_ALT");
                intents.append(rtl);
            } else if (vehicle.availableParameters.contains(
                           QStringLiteral("RTL_ALT_M"))) {
                rtl.parameterName = QStringLiteral("RTL_ALT_M");
                intents.append(rtl);
            }

            SwarmWaypointLeaderCommandIntent acceleration;
            acceleration.kind =
                SwarmWaypointLeaderCommandIntent::Kind::SetParameter;
            acceleration.lease = vehicle.telemetry.lease;
            acceleration.role = role;
            acceleration.order = order;
            if (vehicle.availableParameters.contains(
                    QStringLiteral("WPNAV_ACCEL"))) {
                acceleration.parameterName =
                    QStringLiteral("WPNAV_ACCEL");
                acceleration.parameterValue = 100.0;
                intents.append(acceleration);
            } else if (vehicle.availableParameters.contains(
                           QStringLiteral("WP_ACC"))) {
                acceleration.parameterName = QStringLiteral("WP_ACC");
                acceleration.parameterValue = 1.0;
                intents.append(acceleration);
            }
        }
        effect.markInitialParametersConfigured = true;
    }

    m_takeoffIssued.clear();
    m_flightStateBarriers.clear();
    m_lastTargets.clear();
    m_returnParametersConfigured = false;
    m_rtlIssued = false;
    if (intents.isEmpty()) {
        m_streamsRequested = true;
        m_initialParametersConfigured = true;
        m_mode = SwarmWaypointLeaderMode::Takeoff;
        return active(QStringLiteral(
            "Waypoint Leader initialized; beginning staged takeoff."));
    }
    return issue(QStringLiteral(
        "Initializing Waypoint Leader and requesting 5 Hz position streams."),
        std::move(intents), std::move(effect));
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::takeoff(
    const SwarmWaypointLeaderSettings &settings,
    const ResolvedGroup &group)
{
    QVector<SwarmFollowPathPoint> positions;
    const double leadM = static_cast<double>(group.flight.size() + 1)
        * settings.separationM;
    if (!group.path.lineTargets(group.path.start(), leadM,
            settings.separationM, group.flight.size(), &positions)) {
        return stop(QStringLiteral(
            "Waypoint Leader stopped: mission no longer fits takeoff staging."));
    }

    QVector<SwarmWaypointLeaderCommandIntent> intents;
    PendingBatch effect;
    effect.description = QStringLiteral("the staged-takeoff command batch");
    bool allAtAltitude = true;
    bool allAtTarget = true;
    bool waitingForFreshHeartbeat = false;
    int positioned = 0;
    for (int index = 0; index < group.flight.size(); ++index) {
        const SwarmWaypointLeaderVehicleState &vehicle =
            *group.flight.at(index);
        SwarmFollowPathPoint target = positions.at(index);
        target.relativeAltitudeM +=
            settings.takeoffLandAltitudeSeparationM * (index % 3);
        if (!SwarmFollowPathTrail::isValidPoint(target)) {
            return stop(QStringLiteral(
                "Waypoint Leader stopped: a staged-takeoff target is invalid."));
        }
        effect.targetUpdates.append({vehicle.telemetry.lease, target});
        const QString role = roleForIndex(index, group.flightOrders);
        const int order = group.flightOrders.at(index);

        if (const PendingBatch::FlightStateBarrier *barrier =
                flightStateBarrier(vehicle.telemetry.lease)) {
            allAtAltitude = false;
            allAtTarget = false;
            if (vehicle.telemetry.heartbeatObservedMs
                <= barrier->issuedAfterHeartbeatMs) {
                waitingForFreshHeartbeat = true;
                continue;
            }
            const bool reached =
                barrier->expected
                    == PendingBatch::FlightStateBarrier::Expected::Guided
                ? SwarmFlightMode::isExactGuided(vehicle.telemetry)
                : vehicle.telemetry.armed;
            clearFlightStateBarrier(vehicle.telemetry.lease);
            if (!reached) {
                return stop(QStringLiteral(
                    "Waypoint Leader stopped: a fresh heartbeat did not confirm the acknowledged flight-state command."));
            }
        }

        if (!SwarmFlightMode::isExactGuided(vehicle.telemetry)) {
            SwarmWaypointLeaderCommandIntent guided;
            guided.kind =
                SwarmWaypointLeaderCommandIntent::Kind::SetModeGuided;
            guided.lease = vehicle.telemetry.lease;
            guided.role = role;
            guided.order = order;
            intents.append(guided);
            effect.markFlightStateBarriers.append(
                {vehicle.telemetry.lease,
                 PendingBatch::FlightStateBarrier::Expected::Guided,
                 vehicle.telemetry.heartbeatObservedMs});
            allAtAltitude = false;
            allAtTarget = false;
            continue;
        }
        if (!vehicle.telemetry.armed) {
            SwarmWaypointLeaderCommandIntent arm;
            arm.kind = SwarmWaypointLeaderCommandIntent::Kind::Arm;
            arm.lease = vehicle.telemetry.lease;
            arm.role = role;
            arm.order = order;
            arm.arm = true;
            intents.append(arm);
            effect.markFlightStateBarriers.append(
                {vehicle.telemetry.lease,
                 PendingBatch::FlightStateBarrier::Expected::Armed,
                 vehicle.telemetry.heartbeatObservedMs});
            allAtAltitude = false;
            allAtTarget = false;
            continue;
        }

        if (vehicle.telemetry.relativeAltitudeM
            < target.relativeAltitudeM - 0.5) {
            allAtAltitude = false;
            allAtTarget = false;
            if (!takeoffWasIssued(vehicle.telemetry.lease)) {
                SwarmWaypointLeaderCommandIntent takeoffIntent;
                takeoffIntent.kind =
                    SwarmWaypointLeaderCommandIntent::Kind::Takeoff;
                takeoffIntent.lease = vehicle.telemetry.lease;
                takeoffIntent.role = role;
                takeoffIntent.order = order;
                takeoffIntent.takeoffAltitudeM = target.relativeAltitudeM;
                intents.append(takeoffIntent);
                effect.markTakeoffIssued.append(vehicle.telemetry.lease);
            }
            continue;
        }

        SwarmWaypointLeaderCommandIntent position;
        position.kind =
            SwarmWaypointLeaderCommandIntent::Kind::PositionTarget;
        position.lease = vehicle.telemetry.lease;
        position.role = role;
        position.order = order;
        position.target = target;
        intents.append(position);
        ++positioned;
        if (horizontalDistance(pointOf(vehicle), target)
            > settings.separationM) {
            allAtTarget = false;
        }
    }

    if (allAtAltitude && allAtTarget) {
        effect.modeAfterSuccess =
            SwarmWaypointLeaderMode::FlyToGroundMaster;
    }
    if (intents.isEmpty()) {
        for (const TargetRecord &target : effect.targetUpdates) {
            setLastTarget(target);
        }
        if (effect.modeAfterSuccess) {
            m_mode = *effect.modeAfterSuccess;
            return active(QStringLiteral(
                "Staged takeoff complete; flying toward the ground master."));
        }
        return active(waitingForFreshHeartbeat
            ? QStringLiteral(
                "Staged takeoff is waiting for fresh heartbeat confirmation before the next flight-state command.")
            : QStringLiteral(
                "Staged takeoff is waiting for vehicle altitude telemetry."));
    }
    const QString status = allAtAltitude && allAtTarget
        ? QStringLiteral(
            "Staged takeoff targets are complete; awaiting the terminal batch before advancing.")
        : waitingForFreshHeartbeat
        ? QStringLiteral(
            "Staged takeoff is waiting for fresh heartbeat confirmation before the next flight-state command.")
        : QStringLiteral("Staged takeoff: %1/%2 vehicle(s) at target altitude.")
            .arg(positioned).arg(group.flight.size());
    return issue(status, std::move(intents), std::move(effect));
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::flyToGroundMaster(
    const SwarmWaypointLeaderSettings &settings,
    const ResolvedGroup &group)
{
    QVector<SwarmFollowPathPoint> targetPoints;
    if (!targets(group.path, pointOf(*group.air), settings.separationM,
                 settings, group.flight.size(), false, &targetPoints)) {
        if (horizontalDistance(pointOf(*group.air), group.path.end())
            < settings.separationM) {
            m_mode = SwarmWaypointLeaderMode::ReturnAlongMission;
            m_returnParametersConfigured = false;
            return active(QStringLiteral(
                "Air master reached the mission end before the ground master; returning."));
        }
        return active(QStringLiteral(
            "Waiting for enough mission path around the air master."));
    }

    QVector<TargetRecord> updates;
    QVector<SwarmWaypointLeaderCommandIntent> intents = positionIntents(
        group.flight, group.flightOrders, targetPoints, settings,
        SwarmWaypointLeaderVelocity(), &updates);
    PendingBatch effect;
    effect.description = QStringLiteral(
        "the fly-to-ground-master command batch");
    effect.targetUpdates = updates;

    QVector<SwarmFollowPathPoint> followTargets;
    if (targets(group.path, pointOf(*group.ground), settings.leadM,
                settings, group.flight.size(), settings.vFormation,
                &followTargets)
        && horizontalDistance(pointOf(*group.air), followTargets.constFirst())
            < settings.separationM) {
        appendNavigationAccelerationIntents(
            group.flight, group.flightOrders,
            settings.navigationAccelerationMps2, &intents);
        effect.modeAfterSuccess =
            SwarmWaypointLeaderMode::FollowGroundMaster;
    }

    return issue(effect.modeAfterSuccess
            ? QStringLiteral(
                "Formation reached the ground master; awaiting follow-mode command completion.")
            : QStringLiteral("Flying toward ground master: %1 target(s) ready.")
                .arg(group.flight.size()),
        std::move(intents), std::move(effect));
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::followGroundMaster(
    const SwarmWaypointLeaderSettings &settings,
    const ResolvedGroup &group)
{
    double alongM = 0.0;
    double offPathM = 0.0;
    if (!group.path.closest(pointOf(*group.ground), &alongM, &offPathM)) {
        return stop(QStringLiteral(
            "Waypoint Leader stopped: ground-master path distance is unavailable."));
    }
    if (offPathM > settings.offPathTriggerM) {
        m_mode = SwarmWaypointLeaderMode::ReturnAlongMission;
        m_returnParametersConfigured = false;
        return active(QStringLiteral(
            "Ground master is %1 m off path; switching to return mode.")
            .arg(offPathM, 0, 'f', 1));
    }

    QVector<SwarmFollowPathPoint> targetPoints;
    if (!targets(group.path, pointOf(*group.ground), settings.leadM,
                 settings, group.flight.size(), settings.vFormation,
                 &targetPoints)) {
        return active(QStringLiteral(
            "Waiting for complete follow targets within the mission path."));
    }
    const SwarmWaypointLeaderVelocity velocity{
        group.ground->telemetry.velocityNorthMps / 3.0,
        group.ground->telemetry.velocityEastMps / 3.0,
        group.ground->telemetry.velocityDownMps / 3.0};
    QVector<TargetRecord> updates;
    QVector<SwarmWaypointLeaderCommandIntent> intents = positionIntents(
        group.flight, group.flightOrders, targetPoints, settings,
        velocity, &updates);
    PendingBatch effect;
    effect.description = QStringLiteral("the follow command batch");
    effect.targetUpdates = updates;
    return issue(QStringLiteral(
        "Following ground master: %1 target(s); off path %2 m.")
            .arg(group.flight.size()).arg(offPathM, 0, 'f', 1),
        std::move(intents), std::move(effect));
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::returnAlongMission(
    const SwarmWaypointLeaderSettings &settings,
    const ResolvedGroup &group)
{
    QVector<SwarmWaypointLeaderCommandIntent> intents;
    PendingBatch effect;
    effect.description = QStringLiteral("the return-along-mission batch");
    if (!m_returnParametersConfigured) {
        appendNavigationAccelerationIntents(
            group.flight, group.flightOrders, 1.0, &intents);
        effect.markReturnParametersConfigured = true;
        if (intents.isEmpty()) {
            m_returnParametersConfigured = true;
        }
    }

    QVector<SwarmFollowPathPoint> targetPoints;
    if (!targets(group.path, pointOf(*group.air), settings.separationM,
                 settings, group.flight.size(), false, &targetPoints)) {
        if (horizontalDistance(pointOf(*group.air), group.path.end())
            < settings.separationM) {
            prepareLandingTargets(group);
            if (intents.isEmpty()) {
                m_mode = SwarmWaypointLeaderMode::LandAltitude;
                return active(QStringLiteral(
                    "Mission end reached; establishing separated RTL altitudes."));
            }
            effect.modeAfterSuccess =
                SwarmWaypointLeaderMode::LandAltitude;
            return issue(QStringLiteral(
                "Mission end reached; awaiting return-parameter completion before separated altitudes."),
                std::move(intents), std::move(effect));
        }
        if (intents.isEmpty()) {
            return active(QStringLiteral(
                "Returning along mission; waiting for complete formation targets."));
        }
        return issue(QStringLiteral(
            "Configuring return acceleration while complete formation targets are unavailable."),
            std::move(intents), std::move(effect));
    }

    QVector<TargetRecord> updates;
    QVector<SwarmWaypointLeaderCommandIntent> position = positionIntents(
        group.flight, group.flightOrders, targetPoints, settings,
        SwarmWaypointLeaderVelocity(), &updates);
    intents += position;
    effect.targetUpdates = updates;
    return issue(QStringLiteral(
        "Returning along mission: %1 target(s) ready.")
            .arg(group.flight.size()),
        std::move(intents), std::move(effect));
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::landAltitude(
    const SwarmWaypointLeaderSettings &settings,
    const ResolvedGroup &group)
{
    prepareLandingTargets(group);
    QVector<SwarmWaypointLeaderCommandIntent> intents;
    bool allReady = true;
    for (int index = 0; index < group.flight.size(); ++index) {
        const SwarmWaypointLeaderVehicleState &vehicle =
            *group.flight.at(index);
        SwarmFollowPathPoint baseTarget;
        if (!lastTarget(vehicle.telemetry.lease, &baseTarget)) {
            return stop(QStringLiteral(
                "Waypoint Leader stopped: a landing base target is unavailable."));
        }
        SwarmFollowPathPoint target = baseTarget;
        target.relativeAltitudeM +=
            settings.takeoffLandAltitudeSeparationM * index;
        if (!SwarmFollowPathTrail::isValidPoint(target)) {
            return stop(QStringLiteral(
                "Waypoint Leader stopped: a separated landing target is invalid."));
        }
        SwarmWaypointLeaderCommandIntent position;
        position.kind =
            SwarmWaypointLeaderCommandIntent::Kind::PositionTarget;
        position.lease = vehicle.telemetry.lease;
        position.role = roleForIndex(index, group.flightOrders);
        position.order = group.flightOrders.at(index);
        position.target = target;
        intents.append(position);
        if (vehicle.telemetry.armed
            && vehicle.telemetry.relativeAltitudeM
                < target.relativeAltitudeM - 0.5) {
            allReady = false;
        }
    }

    PendingBatch effect;
    effect.description = QStringLiteral(
        "the separated-altitude landing batch");
    if (allReady && !m_rtlIssued) {
        for (int index = 0; index < group.flight.size(); ++index) {
            SwarmWaypointLeaderCommandIntent rtl;
            rtl.kind = SwarmWaypointLeaderCommandIntent::Kind::SetModeRtl;
            rtl.lease = group.flight.at(index)->telemetry.lease;
            rtl.role = roleForIndex(index, group.flightOrders);
            rtl.order = group.flightOrders.at(index);
            intents.append(rtl);
        }
        effect.markRtlIssued = true;
        effect.modeAfterSuccess = SwarmWaypointLeaderMode::Landing;
    } else if (allReady) {
        m_mode = SwarmWaypointLeaderMode::Landing;
    }
    return issue(allReady
            ? QStringLiteral(
                "All air vehicles reached separated altitudes; awaiting RTL completion.")
            : QStringLiteral(
                "Establishing separated RTL altitudes before landing."),
        std::move(intents), std::move(effect));
}

SwarmWaypointLeaderTick SwarmWaypointLeaderCore::landing(
    const ResolvedGroup &group)
{
    int armedCount = 0;
    int firstNonRtl = -1;
    for (int index = 0; index < group.flight.size(); ++index) {
        const SwarmTelemetrySnapshot &telemetry =
            group.flight.at(index)->telemetry;
        if (!telemetry.armed) {
            continue;
        }
        ++armedCount;
        if (firstNonRtl < 0 && !isExactRtl(telemetry)) {
            firstNonRtl = index;
        }
    }
    if (armedCount == 0) {
        m_stopped = true;
        m_terminalStatus = QStringLiteral(
            "Waypoint Leader completed: all commanded air vehicles are disarmed.");
        SwarmWaypointLeaderTick completed;
        completed.state = SwarmWaypointLeaderTick::State::Completed;
        completed.mode = m_mode;
        completed.status = m_terminalStatus;
        return completed;
    }

    if (firstNonRtl >= 0) {
        SwarmWaypointLeaderCommandIntent rtl;
        rtl.kind = SwarmWaypointLeaderCommandIntent::Kind::SetModeRtl;
        rtl.lease = group.flight.at(firstNonRtl)->telemetry.lease;
        rtl.role = roleForIndex(firstNonRtl, group.flightOrders);
        rtl.order = group.flightOrders.at(firstNonRtl);
        PendingBatch effect;
        effect.description = QStringLiteral("the landing RTL correction");
        return issue(QStringLiteral(
            "RTL landing in progress: %1 air vehicle(s) still armed.")
                .arg(armedCount), {rtl}, std::move(effect));
    }
    return active(QStringLiteral(
        "RTL landing in progress: %1 air vehicle(s) still armed.")
        .arg(armedCount));
}

std::optional<SwarmWaypointLeaderTick>
SwarmWaypointLeaderCore::collisionOverride(
    const SwarmWaypointLeaderSettings &settings,
    const ResolvedGroup &group,
    qint64 nowMs,
    int maximumAgeMs)
{
    QVector<int> armed;
    for (int index = 0; index < group.flight.size(); ++index) {
        if (group.flight.at(index)->telemetry.armed) {
            armed.append(index);
        }
    }

    const auto positionIntent = [&group](
        int index, const SwarmFollowPathPoint &target) {
        SwarmWaypointLeaderCommandIntent intent;
        intent.kind =
            SwarmWaypointLeaderCommandIntent::Kind::PositionTarget;
        intent.lease = group.flight.at(index)->telemetry.lease;
        intent.role = roleForIndex(index, group.flightOrders);
        intent.order = group.flightOrders.at(index);
        intent.target = target;
        return intent;
    };
    const auto projectedOneSecond = [](const SwarmTelemetrySnapshot &vehicle) {
        const double distanceM = std::hypot(
            vehicle.velocityNorthMps, vehicle.velocityEastMps);
        const double bearing = std::atan2(
            vehicle.velocityEastMps, vehicle.velocityNorthMps);
        SwarmFollowPathPoint result = projected(
            {vehicle.latitudeDegrees, vehicle.longitudeDegrees,
             vehicle.relativeAltitudeM}, bearing, distanceM);
        result.relativeAltitudeM = vehicle.relativeAltitudeM
            - vehicle.velocityDownMps;
        return result;
    };
    const auto headingDifference = [](const SwarmTelemetrySnapshot &left,
                                      const SwarmTelemetrySnapshot &right) {
        const double leftHeading = std::atan2(
            left.velocityEastMps, left.velocityNorthMps) * RadiansToDegrees;
        const double rightHeading = std::atan2(
            right.velocityEastMps, right.velocityNorthMps) * RadiansToDegrees;
        double difference = std::fmod(
            std::abs(leftHeading - rightHeading), 360.0);
        if (difference < 0.0) {
            difference += 360.0;
        }
        return difference > 180.0 ? 360.0 - difference : difference;
    };

    for (int first = 0; first < armed.size(); ++first) {
        for (int second = first + 1; second < armed.size(); ++second) {
            const int leftIndex = armed.at(first);
            const int rightIndex = armed.at(second);
            const SwarmTelemetrySnapshot &left =
                group.flight.at(leftIndex)->telemetry;
            const SwarmTelemetrySnapshot &right =
                group.flight.at(rightIndex)->telemetry;
            const SwarmFollowPathPoint leftPoint =
                pointOf(*group.flight.at(leftIndex));
            const SwarmFollowPathPoint rightPoint =
                pointOf(*group.flight.at(rightIndex));
            const double altitudeDifference = std::abs(
                left.relativeAltitudeM - right.relativeAltitudeM);
            if (horizontalDistance(leftPoint, rightPoint)
                    < settings.separationM / 2.0
                && altitudeDifference < 1.0) {
                const int climbIndex = left.relativeAltitudeM
                        > right.relativeAltitudeM
                    ? leftIndex : rightIndex;
                SwarmFollowPathPoint target =
                    pointOf(*group.flight.at(climbIndex));
                target.relativeAltitudeM +=
                    settings.takeoffLandAltitudeSeparationM;
                if (!SwarmFollowPathTrail::isValidPoint(target)) {
                    return stop(QStringLiteral(
                        "Waypoint Leader stopped: collision-separation target is invalid."));
                }
                return urgent(QStringLiteral(
                    "Collision separation override active; normal progression is paused."),
                    {positionIntent(climbIndex, target)});
            }

            const SwarmFollowPathPoint leftProjected =
                projectedOneSecond(left);
            const SwarmFollowPathPoint rightProjected =
                projectedOneSecond(right);
            if (horizontalDistance(leftProjected, rightProjected)
                    >= settings.separationM / 2.0
                || altitudeDifference >= 1.0) {
                continue;
            }

            const double difference = headingDifference(left, right);
            const bool freshGroundSpeed = left.vfrHudValid
                && validAge(left.vfrHudObservedMs, nowMs, maximumAgeMs)
                && std::isfinite(left.groundSpeedMps);
            if (difference < 45.0 && freshGroundSpeed
                && left.groundSpeedMps > 0.5) {
                return urgent(QStringLiteral(
                    "Collision separation override active; normal progression is paused."),
                    {positionIntent(leftIndex, leftPoint)});
            }
            if (difference > 135.0) {
                SwarmFollowPathPoint leftClimb = leftPoint;
                leftClimb.relativeAltitudeM +=
                    settings.takeoffLandAltitudeSeparationM;
                if (!SwarmFollowPathTrail::isValidPoint(leftClimb)) {
                    return stop(QStringLiteral(
                        "Waypoint Leader stopped: collision-avoidance target is invalid."));
                }
                return urgent(QStringLiteral(
                    "Collision separation override active; normal progression is paused."),
                    {positionIntent(leftIndex, leftClimb),
                     positionIntent(rightIndex, rightPoint)});
            }
        }
    }
    return std::nullopt;
}
