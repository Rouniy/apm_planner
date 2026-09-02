#include "SurveyMissionBuilder.h"

#include "QGCMAVLink.h"

#include <QObject>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {

QString tagFor(const SurveyGridPoint &point)
{
    return SurveyGridGenerator::PointTag(point.type);
}

bool sameLocation(const SurveyGridCoordinate &left,
                  const SurveyGridCoordinate &right)
{
    return std::abs(left.latitude - right.latitude) < 1.0e-9
        && std::abs(left.longitude - right.longitude) < 1.0e-9
        && std::abs(left.altitude - right.altitude) < 1.0e-6;
}

double normalizedHeading(double heading)
{
    heading = std::fmod(heading, 360.0);
    return heading < 0.0 ? heading + 360.0 : heading;
}

WpRowData command(quint16 id)
{
    WpRowData row;
    row.Command = id;
    row.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    return row;
}

WpRowData navigationCommand(const SurveyGridPoint &point,
                            const SurveyMissionOptions &options)
{
    WpRowData row = command(
        options.waypointDelay <= 0.0 && options.useSplineWaypoints
                && (point.type == SurveyGridPointType::StripStart
                    || point.type == SurveyGridPointType::SurveyStart)
            ? MAV_CMD_NAV_SPLINE_WAYPOINT
            : MAV_CMD_NAV_WAYPOINT);
    row.Lat = point.coordinate.latitude;
    row.Lng = point.coordinate.longitude;
    row.Alt = point.coordinate.altitude;
    row.P1 = std::max(0.0, options.waypointDelay);
    row.Tag = tagFor(point);
    return row;
}

WpRowData triggerDistanceCommand(double distance)
{
    WpRowData row = command(MAV_CMD_DO_SET_CAM_TRIGG_DIST);
    row.P1 = std::max(0.0, distance);
    row.P3 = 1.0;
    return row;
}

WpRowData repeatServoCommand(const SurveyMissionOptions &options,
                             int repetitions)
{
    WpRowData row = command(MAV_CMD_DO_REPEAT_SERVO);
    row.P1 = std::clamp(options.servoNumber, 1, 16);
    row.P2 = std::clamp(options.servoPwm, 800, 2200);
    row.P3 = std::max(0, repetitions);
    row.P4 = std::max(0.0, options.servoRepeatSeconds);
    return row;
}

WpRowData setServoCommand(int servoNumber, int pwm)
{
    WpRowData row = command(MAV_CMD_DO_SET_SERVO);
    row.P1 = std::clamp(servoNumber, 1, 16);
    row.P2 = std::clamp(pwm, 800, 2200);
    return row;
}

SurveyMissionPlan buildSegment(const QVector<SurveyGridPoint> &grid,
                               const SurveyGridCoordinate &home,
                               const SurveyMissionOptions &options)
{
    SurveyMissionPlan result;
    if (grid.isEmpty()) {
        result.success = true;
        result.segmentCount = 0;
        return result;
    }

    if (options.addTakeoff) {
        WpRowData takeoff = command(MAV_CMD_NAV_TAKEOFF);
        takeoff.Alt = std::max(0.0, options.takeoffAltitude);
        takeoff.P1 = 20.0;
        result.commands.append(takeoff);
    }

    if (options.useSpeed && options.flyingSpeed > 0.0) {
        WpRowData speed = command(MAV_CMD_DO_CHANGE_SPEED);
        speed.P2 = options.flyingSpeed;
        result.commands.append(speed);
    }

    bool distanceTriggerStarted = false;
    bool hasLastNavigationPoint = false;
    SurveyGridCoordinate lastNavigationPoint;
    auto addNavigation = [&](const SurveyGridPoint &point) {
        if (hasLastNavigationPoint
            && sameLocation(lastNavigationPoint, point.coordinate)) {
            return;
        }
        if (options.holdHeading) {
            WpRowData yaw = command(MAV_CMD_CONDITION_YAW);
            yaw.P1 = normalizedHeading(options.heading);
            result.commands.append(yaw);
        }
        result.commands.append(navigationCommand(point, options));
        lastNavigationPoint = point.coordinate;
        hasLastNavigationPoint = true;
        ++result.navigationCount;
    };

    addNavigation(grid.first());
    for (int index = 1; index < grid.size(); ++index) {
        const SurveyGridPoint &point = grid.at(index);
        const QString tag = tagFor(point);

        if (tag == QStringLiteral("M")) {
            if (options.triggerMode
                    == SurveyMissionOptions::TriggerMode::Digicam) {
                addNavigation(point);
                WpRowData camera = command(MAV_CMD_DO_DIGICAM_CONTROL);
                camera.Lat = 1.0;
                camera.P1 = 1.0;
                result.commands.append(camera);
                ++result.cameraCommandCount;
            } else if (options.triggerMode
                           == SurveyMissionOptions::TriggerMode::RepeatServo
                       && !options.stopTriggerAtStripEnds) {
                addNavigation(point);
                result.commands.append(repeatServoCommand(options, 1));
                ++result.cameraCommandCount;
            }
            continue;
        }

        if (tag == QStringLiteral("S") || tag == QStringLiteral("E")) {
            addNavigation(point);
        }

        switch (options.triggerMode) {
        case SurveyMissionOptions::TriggerMode::Distance:
            if (options.stopTriggerAtStripEnds) {
                if (tag == QStringLiteral("SM")) {
                    addNavigation(point);
                    result.commands.append(
                        triggerDistanceCommand(options.triggerDistance));
                    ++result.cameraCommandCount;
                } else if (tag == QStringLiteral("ME")) {
                    addNavigation(point);
                    result.commands.append(triggerDistanceCommand(0.0));
                    ++result.cameraCommandCount;
                }
            } else if (!distanceTriggerStarted) {
                result.commands.append(
                    triggerDistanceCommand(options.triggerDistance));
                ++result.cameraCommandCount;
                distanceTriggerStarted = true;
            } else if (tag == QStringLiteral("ME")) {
                addNavigation(point);
            }
            break;
        case SurveyMissionOptions::TriggerMode::RepeatServo:
            if (options.stopTriggerAtStripEnds) {
                if (tag == QStringLiteral("SM")) {
                    addNavigation(point);
                    result.commands.append(repeatServoCommand(options, 999));
                    ++result.cameraCommandCount;
                } else if (tag == QStringLiteral("ME")) {
                    addNavigation(point);
                    result.commands.append(repeatServoCommand(options, 0));
                    ++result.cameraCommandCount;
                }
            }
            break;
        case SurveyMissionOptions::TriggerMode::SetServo:
            if (tag == QStringLiteral("SM")) {
                addNavigation(point);
                result.commands.append(setServoCommand(
                    options.servoNumber, options.servoLowPwm));
                ++result.cameraCommandCount;
            } else if (tag == QStringLiteral("ME")) {
                addNavigation(point);
                result.commands.append(setServoCommand(
                    options.servoNumber, options.servoHighPwm));
                ++result.cameraCommandCount;
            }
            break;
        case SurveyMissionOptions::TriggerMode::None:
        case SurveyMissionOptions::TriggerMode::Digicam:
            break;
        }
    }

    if (options.triggerMode == SurveyMissionOptions::TriggerMode::Distance) {
        result.commands.append(triggerDistanceCommand(0.0));
        ++result.cameraCommandCount;
    }
    if (options.useSpeed && options.restoreSpeed > 0.0) {
        WpRowData speed = command(MAV_CMD_DO_CHANGE_SPEED);
        speed.P2 = options.restoreSpeed;
        result.commands.append(speed);
    }
    if (options.finishAction
            == SurveyMissionOptions::FinishAction::ReturnToLaunch) {
        result.commands.append(command(MAV_CMD_NAV_RETURN_TO_LAUNCH));
    } else if (options.finishAction
                   == SurveyMissionOptions::FinishAction::Land) {
        WpRowData land = command(MAV_CMD_NAV_LAND);
        land.Lat = home.latitude;
        land.Lng = home.longitude;
        result.commands.append(land);
    }

    result.success = true;
    return result;
}

} // namespace

QVector<QPair<int, int>> SurveyMissionBuilder::SplitRanges(
        const QVector<SurveyGridPoint> &grid, int splitCount, QString *error)
{
    if (error) error->clear();
    QVector<QPair<int, int>> ranges;
    if (splitCount < 1) {
        if (error) *error = QObject::tr("Split count must be at least one.");
        return ranges;
    }
    if (grid.isEmpty()) return ranges;

    QVector<QPair<int, int>> strips;
    for (int index = 0; index < grid.size();) {
        if (tagFor(grid.at(index)) != QStringLiteral("S")) {
            ++index;
            continue;
        }
        const int start = index;
        ++index;
        while (index < grid.size()
               && tagFor(grid.at(index)) != QStringLiteral("E")
               && tagFor(grid.at(index)) != QStringLiteral("S")) {
            ++index;
        }
        if (index >= grid.size()
            || tagFor(grid.at(index)) != QStringLiteral("E")) {
            if (error) {
                *error = QObject::tr(
                    "The generated grid contains an incomplete survey strip.");
            }
            return {};
        }
        strips.append(qMakePair(start, index + 1));
        ++index;
    }
    if (strips.size() < splitCount) {
        if (error) {
            *error = QObject::tr(
                "Split count exceeds the available survey strips.");
        }
        return {};
    }

    ranges.reserve(splitCount);
    for (int split = 0; split < splitCount; ++split) {
        const int firstStrip = split * strips.size() / splitCount;
        const int lastStripExclusive =
            (split + 1) * strips.size() / splitCount;
        ranges.append(qMakePair(
            strips.at(firstStrip).first,
            strips.at(lastStripExclusive - 1).second));
    }
    return ranges;
}

SurveyMissionPlan SurveyMissionBuilder::Build(
        const QVector<SurveyGridPoint> &grid,
        const SurveyGridCoordinate &home,
        const SurveyMissionOptions &options)
{
    SurveyMissionPlan result;
    if (options.splitCount < 1) {
        result.error = QObject::tr("Split count must be at least one.");
        return result;
    }
    if (grid.isEmpty()) {
        result.success = true;
        result.segmentCount = 0;
        return result;
    }
    if (options.splitCount == 1) {
        return buildSegment(grid, home, options);
    }
    if (!options.addTakeoff
        || options.finishAction == SurveyMissionOptions::FinishAction::None) {
        result.error = QObject::tr(
            "Split missions require takeoff and an RTL or Land finish for every flight.");
        return result;
    }

    QString splitError;
    const QVector<QPair<int, int>> ranges =
        SplitRanges(grid, options.splitCount, &splitError);
    if (!splitError.isEmpty()) {
        result.error = splitError;
        return result;
    }

    QVector<int> starts;
    starts.reserve(ranges.size());
    SurveyMissionOptions segmentOptions = options;
    segmentOptions.splitCount = 1;
    for (const QPair<int, int> &range : ranges) {
        starts.append(result.commands.size());
        const SurveyMissionPlan segment = buildSegment(
            grid.mid(range.first, range.second - range.first),
            home, segmentOptions);
        if (!segment.success) {
            result.error = segment.error;
            return result;
        }
        result.commands += segment.commands;
        result.navigationCount += segment.navigationCount;
        result.cameraCommandCount += segment.cameraCommandCount;
    }

    QVector<WpRowData> jumps;
    jumps.reserve(starts.size());
    for (int start : starts) {
        WpRowData jump = command(MAV_CMD_DO_JUMP);
        jump.P1 = start + starts.size() + 1;
        jump.P2 = 1.0;
        jumps.append(jump);
    }
    result.commands = jumps + result.commands;
    result.segmentCount = starts.size();
    result.jumpTargetsAreRelative = true;
    result.success = true;
    return result;
}
