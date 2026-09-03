#include "AntennaTrackerGeometry.h"

#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;

// cos(tracker latitude): the MP "shrinking factor for longitude going to
// poles direction". Clamped at 0 so a tracker on a pole never inflates a
// longitude difference into a negative or infinite scale.
double longitudeScale(double trackerLatitudeDegrees)
{
    const double latitude = std::fabs(trackerLatitudeDegrees);
    if (latitude >= 90.0) {
        return 0.0; // cos(90 deg) is 6e-17 in binary floating point, not 0
    }
    const double scale = std::cos(AntennaTrackerGeometry::DegreesToRadians(latitude));
    return scale > 0.0 ? scale : 0.0;
}

struct FlatOffsets
{
    double northMeters = 0.0; // vehicle north of tracker
    double eastMeters = 0.0;  // vehicle east of tracker, dateline-wrapped
};

FlatOffsets flatOffsets(const AntennaTrackerPosition &tracker,
                        const AntennaTrackerPosition &vehicle)
{
    FlatOffsets offsets;
    const double deltaLatitude = vehicle.latitude - tracker.latitude;
    const double deltaLongitude =
        AntennaTrackerGeometry::Wrap180(vehicle.longitude - tracker.longitude);
    offsets.northMeters = deltaLatitude * AntennaTrackerGeometry::MetersPerDegree;
    offsets.eastMeters = deltaLongitude * AntennaTrackerGeometry::MetersPerDegree *
                         longitudeScale(tracker.latitude);
    return offsets;
}
} // namespace

// --- positions --------------------------------------------------------------------------

bool AntennaTrackerPosition::isValid() const
{
    return std::isfinite(latitude) && std::isfinite(longitude) && std::isfinite(altitude) &&
           std::fabs(latitude) <= 90.0;
}

AntennaTrackerPosition AntennaTrackerPosition::normalised() const
{
    AntennaTrackerPosition result = *this;
    if (std::isfinite(longitude)) {
        result.longitude = AntennaTrackerGeometry::NormaliseLongitude(longitude);
    }
    return result;
}

// --- angles -----------------------------------------------------------------------------

double AntennaTrackerGeometry::DegreesToRadians(double degrees)
{
    return degrees * (kPi / 180.0);
}

double AntennaTrackerGeometry::RadiansToDegrees(double radians)
{
    return radians * (180.0 / kPi);
}

double AntennaTrackerGeometry::Wrap180(double degrees)
{
    if (degrees > 180.0) {
        return degrees - 360.0;
    }
    if (degrees < -180.0) {
        return degrees + 360.0;
    }
    return degrees;
}

double AntennaTrackerGeometry::Wrap360(double degrees)
{
    if (degrees < 0.0) {
        return degrees + 360.0;
    }
    if (degrees >= 360.0) {
        return degrees - 360.0;
    }
    return degrees;
}

double AntennaTrackerGeometry::NormaliseLongitude(double degrees)
{
    if (!std::isfinite(degrees)) {
        return degrees;
    }
    double result = std::fmod(degrees + 180.0, 360.0);
    if (result < 0.0) {
        result += 360.0;
    }
    return result - 180.0;
}

// --- MP CurrentState ----------------------------------------------------------------------

double AntennaTrackerGeometry::DistToHome(const AntennaTrackerPosition &tracker,
                                          const AntennaTrackerPosition &vehicle)
{
    if (!tracker.isValid() || !vehicle.isValid()) {
        return 0.0;
    }
    const FlatOffsets offsets = flatOffsets(tracker.normalised(), vehicle.normalised());
    // MP: sqrt(dstlat^2 + dstlon^2) with both offsets taken as absolute values.
    return std::sqrt(offsets.northMeters * offsets.northMeters +
                     offsets.eastMeters * offsets.eastMeters);
}

double AntennaTrackerGeometry::AZToMAV(const AntennaTrackerPosition &tracker,
                                       const AntennaTrackerPosition &vehicle)
{
    if (!tracker.isValid() || !vehicle.isValid()) {
        return 0.0;
    }
    const AntennaTrackerPosition from = tracker.normalised();
    const AntennaTrackerPosition to = vehicle.normalised();
    const FlatOffsets offsets = flatOffsets(from, to);
    if (offsets.northMeters == 0.0 && offsets.eastMeters == 0.0) {
        return 0.0; // MP: dist == 0 -> 0
    }
    // MP: bearing = 90 + atan2(dstlat, -dstlon) with dstlat = (trackerLat - lat)
    // * scaleLongUp and dstlon = trackerLng - lng. Scaling the latitude up by
    // 1 / cos is the same ratio as scaling the longitude down by cos (already in
    // the flat offsets), and 90 + atan2(-north, east) is the same angle as
    // atan2(east, north): no division can overflow at a pole and due north is
    // exactly 0 instead of a value that wraps to 359.999...
    return Wrap360(RadiansToDegrees(std::atan2(offsets.eastMeters, offsets.northMeters)));
}

double AntennaTrackerGeometry::ELToMAV(const AntennaTrackerPosition &tracker,
                                       const AntennaTrackerPosition &vehicle)
{
    if (!tracker.isValid() || !vehicle.isValid()) {
        return 0.0;
    }
    const double distance = DistToHome(tracker, vehicle);
    if (distance == 0.0) {
        return 0.0; // MP: dist == 0 -> 0 (even straight overhead)
    }
    const double altitudeDifference = vehicle.altitude - tracker.altitude;
    return RadiansToDegrees(std::atan(altitudeDifference / distance));
}

AntennaTrackerPointing AntennaTrackerGeometry::PointAt(const AntennaTrackerPosition &tracker,
                                                       const AntennaTrackerPosition &vehicle)
{
    AntennaTrackerPointing pointing;
    if (!tracker.isValid() || !vehicle.isValid()) {
        return pointing;
    }
    pointing.valid = true;
    pointing.horizontalDistance = DistToHome(tracker, vehicle);
    pointing.altitudeDifference = vehicle.altitude - tracker.altitude;
    pointing.coincident = pointing.horizontalDistance == 0.0;
    pointing.azimuth = pointing.coincident ? 0.0 : AZToMAV(tracker, vehicle);
    // atan2 keeps ELToMAV's value for any positive distance and yields the
    // honest +90 / -90 / 0 straight above, below or at the tracker.
    pointing.elevation =
        RadiansToDegrees(std::atan2(pointing.altitudeDifference, pointing.horizontalDistance));
    return pointing;
}

// --- WinForms PointLatLngAlt ------------------------------------------------------------------

double AntennaTrackerGeometry::GetBearing(const AntennaTrackerPosition &from,
                                          const AntennaTrackerPosition &to)
{
    if (!from.isValid() || !to.isValid()) {
        return 0.0;
    }
    const double latitude1 = DegreesToRadians(from.latitude);
    const double latitude2 = DegreesToRadians(to.latitude);
    const double longitudeDifference = DegreesToRadians(to.longitude - from.longitude);
    const double y = std::sin(longitudeDifference) * std::cos(latitude2);
    const double x = std::cos(latitude1) * std::sin(latitude2) -
                     std::sin(latitude1) * std::cos(latitude2) * std::cos(longitudeDifference);
    const double bearing = std::fmod(RadiansToDegrees(std::atan2(y, x)) + 360.0, 360.0);
    return bearing < 0.0 ? bearing + 360.0 : bearing;
}

double AntennaTrackerGeometry::GetDistance(const AntennaTrackerPosition &from,
                                           const AntennaTrackerPosition &to)
{
    if (!from.isValid() || !to.isValid()) {
        return 0.0;
    }
    const double latitude1 = DegreesToRadians(from.latitude);
    const double latitude2 = DegreesToRadians(to.latitude);
    const double deltaLatitude = latitude2 - latitude1;
    const double deltaLongitude = DegreesToRadians(to.longitude - from.longitude);
    double a = std::pow(std::sin(deltaLatitude / 2.0), 2.0) +
               std::cos(latitude1) * std::cos(latitude2) *
                   std::pow(std::sin(deltaLongitude / 2.0), 2.0);
    // Rounding can push a fraction past [0, 1] at (near-)antipodal points and
    // sqrt(1 - a) would then be NaN; clamp before both square roots.
    a = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return EarthRadiusMeters * c;
}
