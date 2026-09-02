#ifndef SURVEYMISSIONBUILDER_H
#define SURVEYMISSIONBUILDER_H

#include "SurveyGridGenerator.h"
#include "WpRow.h"

#include <QString>
#include <QPair>
#include <QVector>

struct SurveyMissionOptions
{
    enum class TriggerMode
    {
        None,
        Distance,
        Digicam,
        RepeatServo,
        SetServo
    };

    enum class FinishAction
    {
        None,
        ReturnToLaunch,
        Land
    };

    bool useSpeed = false;
    double flyingSpeed = 5.0;
    TriggerMode triggerMode = TriggerMode::None;
    double triggerDistance = 30.0;
    bool stopTriggerAtStripEnds = false;
    bool addTakeoff = false;
    double takeoffAltitude = 30.0;
    FinishAction finishAction = FinishAction::None;
    bool useSplineWaypoints = false;
    bool holdHeading = false;
    double heading = 0.0;
    double waypointDelay = 0.0;
    int servoNumber = 9;
    int servoPwm = 1900;
    double servoRepeatSeconds = 1.0;
    int servoLowPwm = 1100;
    int servoHighPwm = 1900;
    int splitCount = 1;
    double restoreSpeed = 0.0;
};

struct SurveyMissionPlan
{
    bool success = false;
    QString error;
    QVector<WpRowData> commands;
    int navigationCount = 0;
    int cameraCommandCount = 0;
    int segmentCount = 1;
    bool jumpTargetsAreRelative = false;
};

// Converts the tagged Mission Planner grid path (S/SM/M/ME/E) into the same
// navigation, camera and flight-control mission commands used by MP10.
class SurveyMissionBuilder final
{
public:
    static SurveyMissionPlan Build(
        const QVector<SurveyGridPoint> &grid,
        const SurveyGridCoordinate &home,
        const SurveyMissionOptions &options = SurveyMissionOptions());

    static QVector<QPair<int, int>> SplitRanges(
        const QVector<SurveyGridPoint> &grid, int splitCount,
        QString *error = nullptr);

private:
    SurveyMissionBuilder() = delete;
};

#endif // SURVEYMISSIONBUILDER_H
