#ifndef FLIGHTPLANNERROUTEMETRICS_H
#define FLIGHTPLANNERROUTEMETRICS_H

#include <QVector>

/**
 * Backend-neutral route geometry used by the PLAN table and map backends.
 *
 * Callers decide which mission commands form the flown route and expose that
 * decision through RoutePoint::included. This keeps MAVLink and widget policy
 * out of the geometry helper while preserving an input index for each table
 * row.
 */
class FlightPlannerRouteMetrics final
{
public:
    struct Coordinate
    {
        double latitude = 0.0;
        double longitude = 0.0;
        double altitude = 0.0;

        bool IsValid() const;
    };

    struct RoutePoint
    {
        Coordinate coordinate;
        bool included = true;
        bool loiterTurnsCommand = false;
        double loiterTurns = 0.0;
        double commandLoiterRadiusMeters = 0.0;
    };

    struct Options
    {
        double configuredLoiterRadiusMeters = 0.0;
        // Mission Planner uses the configured radius for Plane/Rover when P3
        // is zero. ArduCopter uses no fallback, so its caller passes false.
        bool useConfiguredLoiterRadiusWhenCommandRadiusIsZero = true;
    };

    struct Leg
    {
        // Index in the RoutePoint input vector. A fromPointIndex of -1 means
        // that this leg starts at Home.
        int fromPointIndex = -1;
        int toPointIndex = -1;
        bool valid = false;
        bool startsAtHome = false;
        double horizontalDistanceMeters = 0.0;
        double distanceMeters = 0.0;
        double bearingDegrees = 0.0;
        double gradientPercent = 0.0;
        double angleDegrees = 0.0;
        double additionalDistanceMeters = 0.0;
    };

    struct Result
    {
        // One entry per input point, allowing a caller to update table rows
        // without rebuilding an index map. Excluded/invalid/first-without-Home
        // points have valid == false.
        QVector<Leg> legs;
        int includedPointCount = 0;

        // Totals below include the Home -> first point leg when Home is valid.
        // totalDistanceMeters also includes loiter-turn additional distance;
        // totalHorizontalDistanceMeters is the direct-leg horizontal sum.
        double totalHorizontalDistanceMeters = 0.0;
        double totalDistanceMeters = 0.0;
        double additionalDistanceMeters = 0.0;

        // Mission Planner's TotalDist excludes Home -> first point but retains
        // loiter-turn distance, including loiter attached to the first point.
        double missionDistanceMeters = 0.0;
        double lastLegDistanceMeters = 0.0;
        double lastToHomeDistanceMeters = 0.0;
    };

    static Result Calculate(const Coordinate &home, bool homeValid,
                            const QVector<RoutePoint> &points);
    static Result Calculate(const Coordinate &home, bool homeValid,
                            const QVector<RoutePoint> &points,
                            const Options &options);
    static double HorizontalDistanceMeters(const Coordinate &from,
                                           const Coordinate &to);
    static double BearingDegrees(const Coordinate &from,
                                 const Coordinate &to);
    static double AdditionalLoiterDistanceMeters(const RoutePoint &point,
                                                 const Options &options);
};

#endif
