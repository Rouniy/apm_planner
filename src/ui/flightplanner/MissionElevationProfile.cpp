#include "MissionElevationProfile.h"

#include "QGCMAVLink.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double kEarthRadiusMeters = 6371000.0;
constexpr double kDegreesToRadians =
    3.14159265358979323846 / 180.0;

enum class AltitudeFrame {
    Absolute,
    Relative,
    Terrain,
};

struct RouteNode
{
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeMeters = 0.0;
    AltitudeFrame altitudeFrame = AltitudeFrame::Relative;
    QString label;
};

bool validCoordinate(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0;
}

AltitudeFrame altitudeFrameFor(quint8 frame)
{
    switch (frame) {
    case MAV_FRAME_GLOBAL:
    case MAV_FRAME_GLOBAL_INT:
        return AltitudeFrame::Absolute;
    case MAV_FRAME_GLOBAL_TERRAIN_ALT:
    case MAV_FRAME_GLOBAL_TERRAIN_ALT_INT:
        return AltitudeFrame::Terrain;
    case MAV_FRAME_GLOBAL_RELATIVE_ALT:
    case MAV_FRAME_GLOBAL_RELATIVE_ALT_INT:
    default:
        return AltitudeFrame::Relative;
    }
}

double wrappedLongitude(double longitude)
{
    while (longitude > 180.0) longitude -= 360.0;
    while (longitude < -180.0) longitude += 360.0;
    return longitude;
}

double interpolateLongitude(double from, double to, double fraction)
{
    double delta = to - from;
    if (delta > 180.0) delta -= 360.0;
    else if (delta < -180.0) delta += 360.0;
    return wrappedLongitude(from + delta * fraction);
}

double interpolate(double from, double to, double fraction)
{
    return from + (to - from) * fraction;
}

double quietNaN()
{
    return std::numeric_limits<double>::quiet_NaN();
}

double endpointPlannedAltitude(const RouteNode &node,
                               double terrainAltitude,
                               double homeReference)
{
    switch (node.altitudeFrame) {
    case AltitudeFrame::Absolute:
        return node.altitudeMeters;
    case AltitudeFrame::Relative:
        return std::isfinite(homeReference)
            ? homeReference + node.altitudeMeters : quietNaN();
    case AltitudeFrame::Terrain:
        return std::isfinite(terrainAltitude)
            ? terrainAltitude + node.altitudeMeters : quietNaN();
    }
    return quietNaN();
}
}

double MissionElevationProfile::DistanceMeters(
        double latitude1, double longitude1,
        double latitude2, double longitude2)
{
    if (!validCoordinate(latitude1, longitude1)
        || !validCoordinate(latitude2, longitude2)) {
        return 0.0;
    }
    const double deltaLatitude =
        (latitude2 - latitude1) * kDegreesToRadians;
    double deltaLongitudeDegrees = longitude2 - longitude1;
    if (deltaLongitudeDegrees > 180.0) deltaLongitudeDegrees -= 360.0;
    else if (deltaLongitudeDegrees < -180.0) {
        deltaLongitudeDegrees += 360.0;
    }
    const double deltaLongitude =
        deltaLongitudeDegrees * kDegreesToRadians;
    const double latitude1Radians = latitude1 * kDegreesToRadians;
    const double latitude2Radians = latitude2 * kDegreesToRadians;
    const double sinLatitude = std::sin(deltaLatitude * 0.5);
    const double sinLongitude = std::sin(deltaLongitude * 0.5);
    const double haversine = std::clamp(
        sinLatitude * sinLatitude
            + std::cos(latitude1Radians) * std::cos(latitude2Radians)
                * sinLongitude * sinLongitude,
        0.0, 1.0);
    return 2.0 * kEarthRadiusMeters * std::asin(std::sqrt(haversine));
}

bool MissionElevationProfile::CommandIsRoutePoint(quint16 command)
{
    // Mirrors the route assembled by Mission Planner's WPOverlay for the
    // elevation profile. ROI entries are removed before charting, while the
    // positioned landing/return-path markers remain part of the path.
    if (command == MAV_CMD_NAV_RETURN_TO_LAUNCH
        || command == MAV_CMD_NAV_CONTINUE_AND_CHANGE_ALT
        || command == MAV_CMD_NAV_GUIDED_ENABLE
        || command == MAV_CMD_NAV_DELAY
        || command == MAV_CMD_NAV_ROI
        || command == MAV_CMD_DO_SET_ROI) {
        return false;
    }
    return (command > 0 && command < MAV_CMD_NAV_LAST)
        || command == 188 // MAV_CMD_DO_RETURN_PATH_START (newer dialect)
        || command == MAV_CMD_DO_LAND_START;
}

MissionElevationProfileResult MissionElevationProfile::Build(
        const QVector<WpRowData> &missionRows,
        const MissionElevationHome &home,
        const TerrainProvider &terrainProvider,
        double requestedSampleSpacingMeters,
        int maximumSamples)
{
    MissionElevationProfileResult result;
    if (!std::isfinite(requestedSampleSpacingMeters)
        || requestedSampleSpacingMeters <= 0.0) {
        requestedSampleSpacingMeters = 10.0;
    }

    QVector<RouteNode> nodes;
    nodes.reserve(missionRows.size() + 1);
    if (home.valid && validCoordinate(home.latitude, home.longitude)
        && std::isfinite(home.altitudeAmslMeters)) {
        nodes.append({home.latitude, home.longitude,
                      home.altitudeAmslMeters,
                      AltitudeFrame::Absolute,
                      QStringLiteral("H")});
    }
    for (const WpRowData &row : missionRows) {
        if (!CommandIsRoutePoint(row.Command)
            || !WpRow::FrameHasGlobalLocation(row.Frame)
            || !validCoordinate(row.Lat, row.Lng)
            || (row.Lat == 0.0 && row.Lng == 0.0)
            || !std::isfinite(row.Alt)) {
            continue;
        }
        nodes.append({row.Lat, row.Lng, row.Alt,
                      altitudeFrameFor(row.Frame),
                      QString::number(row.Seq + 1)});
    }

    result.routePointCount = nodes.size();
    if (nodes.size() < 2) return result;

    QVector<double> legDistances;
    legDistances.reserve(nodes.size() - 1);
    for (int index = 1; index < nodes.size(); ++index) {
        const RouteNode &from = nodes.at(index - 1);
        const RouteNode &to = nodes.at(index);
        const double distance = DistanceMeters(
            from.latitude, from.longitude, to.latitude, to.longitude);
        legDistances.append(distance);
        result.totalDistanceMeters += distance;
    }

    maximumSamples = std::max(maximumSamples, nodes.size() + 1);
    const int legCount = legDistances.size();
    const int distanceSampleBudget = maximumSamples - 1 - legCount;
    double spacing = requestedSampleSpacingMeters;
    if (distanceSampleBudget > 0 && result.totalDistanceMeters > 0.0) {
        spacing = std::max(
            spacing, result.totalDistanceMeters / distanceSampleBudget);
    }
    result.sampleSpacingMeters = spacing;

    const auto sampleTerrain = [&result, &terrainProvider](
            double latitude, double longitude) {
        ++result.terrainSampleCount;
        double altitude = quietNaN();
        bool available = false;
        try {
            available = terrainProvider
                && terrainProvider(latitude, longitude, &altitude);
        } catch (...) {
            available = false;
        }
        if (!available || !std::isfinite(altitude)) {
            ++result.missingTerrainSampleCount;
            return quietNaN();
        }
        return altitude;
    };

    const RouteNode &firstNode = nodes.first();
    double firstTerrain = sampleTerrain(
        firstNode.latitude, firstNode.longitude);
    double homeReference = quietNaN();
    if (home.valid && std::isfinite(home.altitudeAmslMeters)) {
        homeReference = home.altitudeAmslMeters;
        result.homeReferenceAvailable = true;
    }
    double firstPlanned = endpointPlannedAltitude(
        firstNode, firstTerrain, homeReference);
    result.samples.append({0.0, firstNode.latitude, firstNode.longitude,
                           firstTerrain, firstPlanned});
    result.markers.append(
        {firstNode.label, 0.0, firstPlanned});

    double cumulativeDistance = 0.0;
    double startTerrain = firstTerrain;
    for (int index = 1; index < nodes.size(); ++index) {
        const RouteNode &from = nodes.at(index - 1);
        const RouteNode &to = nodes.at(index);
        const double legDistance = legDistances.at(index - 1);
        const int segments = std::max(
            1, static_cast<int>(std::ceil(legDistance / spacing)));
        const double endTerrain = sampleTerrain(to.latitude, to.longitude);
        const double startPlanned = endpointPlannedAltitude(
            from, startTerrain, homeReference);
        const double endPlanned = endpointPlannedAltitude(
            to, endTerrain, homeReference);

        double startClearance = quietNaN();
        if (to.altitudeFrame == AltitudeFrame::Terrain
            && std::isfinite(startTerrain)
            && std::isfinite(startPlanned)) {
            startClearance = startPlanned - startTerrain;
        }

        for (int segment = 1; segment <= segments; ++segment) {
            const double fraction =
                static_cast<double>(segment) / segments;
            const double latitude = interpolate(
                from.latitude, to.latitude, fraction);
            const double longitude = interpolateLongitude(
                from.longitude, to.longitude, fraction);
            const double terrain = segment == segments
                ? endTerrain : sampleTerrain(latitude, longitude);
            double planned = quietNaN();
            if (segment == segments) {
                planned = endPlanned;
            } else if (to.altitudeFrame == AltitudeFrame::Terrain
                && std::isfinite(terrain)
                && std::isfinite(startClearance)) {
                planned = terrain + interpolate(
                    startClearance, to.altitudeMeters, fraction);
            } else if (std::isfinite(startPlanned)
                       && std::isfinite(endPlanned)) {
                planned = interpolate(startPlanned, endPlanned, fraction);
            }
            result.samples.append({
                cumulativeDistance + legDistance * fraction,
                latitude, longitude, terrain, planned});
        }
        cumulativeDistance += legDistance;
        result.markers.append(
            {to.label, cumulativeDistance, endPlanned});
        startTerrain = endTerrain;
    }
    return result;
}
