#ifndef ANTENNATRACKERGEOMETRY_H
#define ANTENNATRACKERGEOMETRY_H

#include <QtGlobal>

/*
 * Pure geometry of Mission Planner's antenna tracker: where must the tracker
 * point to see the vehicle. Mission Planner 10 computes this in
 * CurrentState.AZToMAV / ELToMAV / DistToHome from TrackerLocation and the
 * vehicle position; the same formulas live here as static functions with the
 * MP names, plus the great-circle helpers of PointLatLngAlt for cross-checks.
 *
 * Units and references:
 * - latitude / longitude: WGS-84 degrees. Any finite longitude is accepted and
 *   normalised to [-180, 180); latitude must be within [-90, 90].
 * - altitude: metres. Tracker and vehicle altitudes must share one datum; MP
 *   uses altitude above mean sea level (CurrentState.altasl and
 *   TrackerLocation.Alt). The module only uses their difference.
 * - horizontal distance: metres on the flat-earth (equirectangular) model of
 *   MP, 111319.5 m per degree, longitude shrunk by cos(tracker latitude).
 * - azimuth: degrees clockwise from true north, [0, 360).
 * - elevation: degrees above the tracker's horizontal plane, [-90, 90].
 *
 * Nothing here treats (0, 0) or a zero longitude as "no position" the way MP
 * does; validity is explicit (AntennaTrackerPosition::isValid) and the owner
 * keeps its own tracker-home state.
 */

struct AntennaTrackerPosition
{
    double latitude = 0.0;  // degrees
    double longitude = 0.0; // degrees
    double altitude = 0.0;  // metres, shared datum

    AntennaTrackerPosition() = default;
    AntennaTrackerPosition(double lat, double lng, double alt = 0.0)
        : latitude(lat), longitude(lng), altitude(alt)
    {
    }

    // Finite values, |latitude| <= 90 and a finite longitude.
    bool isValid() const;
    // The same position with the longitude normalised to [-180, 180).
    AntennaTrackerPosition normalised() const;
};

struct AntennaTrackerPointing
{
    bool valid = false;              // false for invalid input; every number is then 0
    bool coincident = false;         // zero horizontal distance: the azimuth is undefined (0)
    double azimuth = 0.0;            // degrees, [0, 360)
    double elevation = 0.0;          // degrees, [-90, 90]
    double horizontalDistance = 0.0; // metres (MP DistToHome with multiplierdist = 1)
    double altitudeDifference = 0.0; // metres, vehicle minus tracker
};

class AntennaTrackerGeometry
{
public:
    // MP CurrentState: metres per degree of latitude on the flat-earth model.
    static constexpr double MetersPerDegree = 111319.5;
    // WinForms PointLatLngAlt.GetDistance: mean earth radius in metres.
    static constexpr double EarthRadiusMeters = 6371000.0;

    static double DegreesToRadians(double degrees);
    static double RadiansToDegrees(double radians);
    // Single-step wraps like MP: into [-180, 180] / [0, 360).
    static double Wrap180(double degrees);
    static double Wrap360(double degrees);
    // Longitude normalisation for any finite input: [-180, 180).
    static double NormaliseLongitude(double degrees);

    // MP CurrentState.DistToHome: flat-earth horizontal distance in metres.
    // Longitude differences are wrapped through the dateline first (MP does
    // not); at a pole the longitude contributes nothing. 0 for invalid input.
    static double DistToHome(const AntennaTrackerPosition &tracker,
                             const AntennaTrackerPosition &vehicle);
    // MP CurrentState.AZToMAV: flat-earth bearing from the tracker to the
    // vehicle in [0, 360); 0 when the horizontal distance is 0 or the input is invalid.
    static double AZToMAV(const AntennaTrackerPosition &tracker,
                          const AntennaTrackerPosition &vehicle);
    // MP CurrentState.ELToMAV: atan(altitude difference / horizontal distance)
    // in degrees; 0 when the horizontal distance is 0 or the input is invalid.
    static double ELToMAV(const AntennaTrackerPosition &tracker,
                          const AntennaTrackerPosition &vehicle);

    // Everything at once. Unlike ELToMAV, a vehicle straight above or below a
    // valid tracker reports +90 / -90 with `coincident` set; the azimuth stays 0.
    static AntennaTrackerPointing PointAt(const AntennaTrackerPosition &tracker,
                                          const AntennaTrackerPosition &vehicle);

    // WinForms PointLatLngAlt great-circle references (spherical earth).
    static double GetBearing(const AntennaTrackerPosition &from,
                             const AntennaTrackerPosition &to); // degrees [0, 360)
    static double GetDistance(const AntennaTrackerPosition &from,
                              const AntennaTrackerPosition &to); // metres (haversine)
};

#endif // ANTENNATRACKERGEOMETRY_H
