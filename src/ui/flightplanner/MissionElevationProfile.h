#ifndef MISSIONELEVATIONPROFILE_H
#define MISSIONELEVATIONPROFILE_H

#include "WpRow.h"

#include <QVector>

#include <functional>

struct MissionElevationHome
{
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeAmslMeters = 0.0;
};

struct MissionElevationSample
{
    double distanceMeters = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    double terrainAltitudeAmslMeters = 0.0;
    double plannedAltitudeAmslMeters = 0.0;
};

struct MissionElevationMarker
{
    QString label;
    double distanceMeters = 0.0;
    double plannedAltitudeAmslMeters = 0.0;
};

struct MissionElevationProfileResult
{
    QVector<MissionElevationSample> samples;
    QVector<MissionElevationMarker> markers;
    double totalDistanceMeters = 0.0;
    double sampleSpacingMeters = 10.0;
    int routePointCount = 0;
    int terrainSampleCount = 0;
    int missingTerrainSampleCount = 0;
    bool homeReferenceAvailable = false;

    bool hasRoute() const { return routePointCount >= 2; }
};

class MissionElevationProfile final
{
public:
    using TerrainProvider =
        std::function<bool(double latitude, double longitude,
                           double *altitudeAmslMeters)>;

    static MissionElevationProfileResult Build(
        const QVector<WpRowData> &missionRows,
        const MissionElevationHome &home,
        const TerrainProvider &terrainProvider,
        double requestedSampleSpacingMeters = 10.0,
        int maximumSamples = 20000);

    static double DistanceMeters(double latitude1, double longitude1,
                                 double latitude2, double longitude2);
    static bool CommandIsRoutePoint(quint16 command);
};

#endif // MISSIONELEVATIONPROFILE_H
