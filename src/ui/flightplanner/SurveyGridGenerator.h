#ifndef SURVEYGRIDGENERATOR_H
#define SURVEYGRIDGENERATOR_H

#include <QString>
#include <QVector>

struct SurveyGridCoordinate
{
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
};

enum class SurveyGridPointType
{
    StripStart,
    SurveyStart,
    Photo,
    SurveyEnd,
    StripEnd
};

struct SurveyGridPoint
{
    SurveyGridCoordinate coordinate;
    SurveyGridPointType type = SurveyGridPointType::SurveyStart;
};

struct SurveyGridTransect
{
    QVector<SurveyGridPoint> points;
    int passIndex = 0;
    int laneIndex = 0;
};

struct SurveyGridOptions
{
    enum class StartPosition
    {
        Home,
        BottomLeft,
        TopLeft,
        BottomRight,
        TopRight,
        Point
    };

    double altitudeMeters = 100.0;

    // Mission Planner names these values Distance and Spacing respectively.
    double distanceMeters = 50.0;
    double spacingMeters = 30.0;
    double angleDegrees = 0.0;
    double overshoot1Meters = 0.0;
    double overshoot2Meters = 0.0;
    // Mission Planner applies independent lead-ins to the two alternating
    // strip directions. Positive values extend the approach outside the
    // polygon; negative values start both S and SM inside the strip.
    double leadin1Meters = 0.0;
    double leadin2Meters = 0.0;
    bool crossGrid = false;

    StartPosition startPosition = StartPosition::Home;
    SurveyGridCoordinate homeLocation;
    SurveyGridCoordinate startPoint;
};

struct SurveyGridResult
{
    bool success = false;
    QString error;
    QVector<SurveyGridTransect> transects;
    QVector<SurveyGridPoint> path;
};

// Geometry-only implementation of Mission Planner's Survey (Grid). It does not
// create MAVLink mission items and has no dependency on a map widget.
class SurveyGridGenerator final
{
public:
    using StartPosition = SurveyGridOptions::StartPosition;

    static SurveyGridResult CreateGrid(
        const QVector<SurveyGridCoordinate> &polygon,
        const SurveyGridOptions &options = SurveyGridOptions());

    static QString PointTag(SurveyGridPointType type);
};

#endif // SURVEYGRIDGENERATOR_H
