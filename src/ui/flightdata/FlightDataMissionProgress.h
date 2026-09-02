#ifndef FLIGHTDATAMISSIONPROGRESS_H
#define FLIGHTDATAMISSIONPROGRESS_H

#include <QVector>

struct FlightDataMissionPoint
{
    int sequence = 0;
    double latitude = 0.0;
    double longitude = 0.0;
};

struct FlightDataMissionProgress
{
    int itemCount = 0;
    double totalDistanceMeters = 0.0;
    double travelledDistanceMeters = 0.0;
    double percent = 0.0;
};

class FlightDataMissionProgressCalculator final
{
public:
    static FlightDataMissionProgress Calculate(
        double homeLatitude,
        double homeLongitude,
        const QVector<FlightDataMissionPoint> &missionPoints,
        int currentWaypoint,
        double distanceToCurrentWaypoint);
};

#endif // FLIGHTDATAMISSIONPROGRESS_H
