#include "ui/flightplanner/SurveyGridDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QWidget>

#include <cmath>

namespace {

constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kRadiansToDegrees = 57.295779513082320877;

SurveyGridCoordinate atMeters(double north, double east,
                              double latitude = 47.0,
                              double longitude = 8.0)
{
    SurveyGridCoordinate coordinate;
    coordinate.latitude = latitude
        + north * kRadiansToDegrees / kEarthRadiusMeters;
    coordinate.longitude = longitude
        + east * kRadiansToDegrees
            / (kEarthRadiusMeters * std::cos(latitude / kRadiansToDegrees));
    return coordinate;
}

QVector<SurveyGridCoordinate> rectangle()
{
    return {
        atMeters(0.0, 0.0),
        atMeters(0.0, 100.0),
        atMeters(100.0, 100.0),
        atMeters(100.0, 0.0),
    };
}

} // namespace

class SurveyGridDialogTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesMissionPlannerNamedControls();
    void generatesFromExternalPolygon();
    void invalidInputReportsStatusAndDoesNotAccept();
    void optionsAndExternalAnchorsRoundTrip();
    void missionOptionsRoundTrip();
    void altitudePresentationRoundTripsCanonicalMeters();
    void livePreviewRecalculatesWhileEditing();
    void editingInvalidatesPreviousResult();
};

void SurveyGridDialogTest::exposesMissionPlannerNamedControls()
{
    SurveyGridDialog dialog;

    QCOMPARE(dialog.objectName(), QStringLiteral("GridUI"));
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Survey (Grid)"));

    auto *altitude = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_altitude"));
    auto *distance = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_Distance"));
    auto *spacing = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_spacing"));
    auto *angle = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_angle"));
    auto *overshoot1 = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_overshoot"));
    auto *overshoot2 = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_overshoot2"));
    auto *leadin1 = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_leadin"));
    auto *leadin2 = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_leadin2"));
    auto *startFrom = dialog.findChild<QComboBox *>(
        QStringLiteral("CMB_startfrom"));
    auto *crossGrid = dialog.findChild<QCheckBox *>(
        QStringLiteral("chk_crossgrid"));
    auto *accept = dialog.findChild<QPushButton *>(
        QStringLiteral("BUT_Accept"));
    auto *preview = dialog.findChild<QWidget *>(QStringLiteral("map"));
    auto *triggerMode = dialog.findChild<QComboBox *>(
        QStringLiteral("CMB_triggermode"));
    auto *useSpeed = dialog.findChild<QCheckBox *>(
        QStringLiteral("CHK_usespeed"));
    auto *flyingSpeed = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_UpDownFlySpeed"));
    auto *splitCount = dialog.findChild<QSpinBox *>(
        QStringLiteral("NUM_split"));

    QVERIFY(altitude);
    QVERIFY(distance);
    QVERIFY(spacing);
    QVERIFY(angle);
    QVERIFY(overshoot1);
    QVERIFY(overshoot2);
    QVERIFY(leadin1);
    QVERIFY(leadin2);
    QVERIFY(startFrom);
    QVERIFY(crossGrid);
    QVERIFY(accept);
    QVERIFY(preview);
    QVERIFY(triggerMode);
    QVERIFY(useSpeed);
    QVERIFY(flyingSpeed);
    QVERIFY(splitCount);
    QCOMPARE(altitude->value(), 100.0);
    QCOMPARE(distance->value(), 50.0);
    QCOMPARE(spacing->value(), 30.0);
    QCOMPARE(startFrom->count(), 6);
    QCOMPARE(startFrom->itemText(0), QStringLiteral("Home"));
    QCOMPARE(startFrom->itemText(5), QStringLiteral("Point"));
    QVERIFY(overshoot1->minimum() < 0.0);
    QVERIFY(overshoot2->minimum() < 0.0);
    QVERIFY(leadin1->minimum() < 0.0);
    QVERIFY(leadin2->minimum() < 0.0);
    QVERIFY(!crossGrid->isChecked());
    QCOMPARE(triggerMode->count(), 5);
    QCOMPARE(triggerMode->itemText(0), QStringLiteral("None"));
    QCOMPARE(triggerMode->itemText(4), QStringLiteral("Set servo"));
    QVERIFY(!useSpeed->isChecked());
    QCOMPARE(flyingSpeed->value(), 5.0);
    QCOMPARE(splitCount->value(), 1);

    const char *missionDoubleNames[] = {
        "NUM_trigdist", "NUM_restoreSpeed", "NUM_takeoffalt",
        "TXT_headinghold", "NUM_copter_delay", "NUM_repttime",
    };
    for (const char *name : missionDoubleNames) {
        QVERIFY2(dialog.findChild<QDoubleSpinBox *>(
                     QString::fromLatin1(name)), name);
    }
    const char *missionIntegerNames[] = {
        "NUM_reptservo", "num_reptpwm", "num_setservolow",
        "num_setservohigh",
    };
    for (const char *name : missionIntegerNames) {
        QVERIFY2(dialog.findChild<QSpinBox *>(QString::fromLatin1(name)),
                 name);
    }
    const char *missionCheckNames[] = {
        "chk_stopstart", "CHK_toandland", "CHK_spline",
        "CHK_copter_headinghold",
    };
    for (const char *name : missionCheckNames) {
        QVERIFY2(dialog.findChild<QCheckBox *>(QString::fromLatin1(name)),
                 name);
    }
    auto *finishAction = dialog.findChild<QComboBox *>(
        QStringLiteral("CMB_finishaction"));
    QVERIFY(finishAction);
    QCOMPARE(finishAction->count(), 3);
    QCOMPARE(finishAction->itemText(1), QStringLiteral("RTL"));
    QCOMPARE(finishAction->itemText(2), QStringLiteral("Land"));
}

void SurveyGridDialogTest::generatesFromExternalPolygon()
{
    SurveyGridDialog dialog(rectangle());
    auto *distance = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_Distance"));
    auto *spacing = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_spacing"));
    auto *crossGrid = dialog.findChild<QCheckBox *>(
        QStringLiteral("chk_crossgrid"));
    QVERIFY(distance);
    QVERIFY(spacing);
    QVERIFY(crossGrid);

    distance->setValue(25.0);
    spacing->setValue(0.0);
    crossGrid->setChecked(true);
    QSignalSpy resultSpy(&dialog, &SurveyGridDialog::resultChanged);

    const SurveyGridResult generated = dialog.generate();

    QVERIFY2(generated.success, qPrintable(generated.error));
    QCOMPARE(generated.transects.size(), 8);
    QVERIFY(dialog.gridResult().success);
    QVERIFY(dialog.statusText().contains(QStringLiteral("Generated")));
    QCOMPARE(resultSpy.count(), 1);

    auto *accept = dialog.findChild<QPushButton *>(
        QStringLiteral("BUT_Accept"));
    QVERIFY(accept);
    QSignalSpy acceptedSpy(&dialog, &QDialog::accepted);
    accept->click();
    QCOMPARE(acceptedSpy.count(), 1);
    QCOMPARE(resultSpy.count(), 2);
}

void SurveyGridDialogTest::invalidInputReportsStatusAndDoesNotAccept()
{
    SurveyGridDialog dialog;
    auto *accept = dialog.findChild<QPushButton *>(
        QStringLiteral("BUT_Accept"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("LBL_status"));
    QVERIFY(accept);
    QVERIFY(status);
    QSignalSpy acceptedSpy(&dialog, &QDialog::accepted);

    accept->click();

    QCOMPARE(acceptedSpy.count(), 0);
    QVERIFY(!dialog.gridResult().success);
    QVERIFY(dialog.gridResult().error.contains(
        QStringLiteral("at least three"), Qt::CaseInsensitive));
    QVERIFY(status->text().contains(QStringLiteral("failed"),
                                    Qt::CaseInsensitive));
    QVERIFY(status->property("error").toBool());
}

void SurveyGridDialogTest::optionsAndExternalAnchorsRoundTrip()
{
    SurveyGridDialog dialog(rectangle());
    SurveyGridOptions expected;
    expected.altitudeMeters = 135.5;
    expected.distanceMeters = 27.25;
    expected.spacingMeters = 18.0;
    expected.angleDegrees = 42.0;
    expected.overshoot1Meters = -4.0;
    expected.overshoot2Meters = 9.0;
    expected.leadin1Meters = 12.0;
    expected.leadin2Meters = -3.0;
    expected.crossGrid = true;
    expected.startPosition = SurveyGridOptions::StartPosition::Point;
    expected.homeLocation = atMeters(-20.0, -20.0);
    expected.startPoint = atMeters(100.0, 100.0);

    dialog.setOptions(expected);
    const SurveyGridOptions actual = dialog.options();

    QCOMPARE(actual.altitudeMeters, expected.altitudeMeters);
    QCOMPARE(actual.distanceMeters, expected.distanceMeters);
    QCOMPARE(actual.spacingMeters, expected.spacingMeters);
    QCOMPARE(actual.angleDegrees, expected.angleDegrees);
    QCOMPARE(actual.overshoot1Meters, expected.overshoot1Meters);
    QCOMPARE(actual.overshoot2Meters, expected.overshoot2Meters);
    QCOMPARE(actual.leadin1Meters, expected.leadin1Meters);
    QCOMPARE(actual.leadin2Meters, expected.leadin2Meters);
    QCOMPARE(actual.crossGrid, expected.crossGrid);
    QCOMPARE(static_cast<int>(actual.startPosition),
             static_cast<int>(expected.startPosition));
    QCOMPARE(actual.homeLocation.latitude, expected.homeLocation.latitude);
    QCOMPARE(actual.homeLocation.longitude, expected.homeLocation.longitude);
    QCOMPARE(actual.startPoint.latitude, expected.startPoint.latitude);
    QCOMPARE(actual.startPoint.longitude, expected.startPoint.longitude);

    const SurveyGridCoordinate changedHome = atMeters(-50.0, 0.0);
    const SurveyGridCoordinate changedPoint = atMeters(0.0, 50.0);
    dialog.setHomeLocation(changedHome);
    dialog.setStartPoint(changedPoint);
    QCOMPARE(dialog.options().homeLocation.latitude, changedHome.latitude);
    QCOMPARE(dialog.options().startPoint.longitude, changedPoint.longitude);
}

void SurveyGridDialogTest::missionOptionsRoundTrip()
{
    SurveyGridDialog dialog(rectangle());
    SurveyMissionOptions expected;
    expected.useSpeed = true;
    expected.flyingSpeed = 12.5;
    expected.triggerMode = SurveyMissionOptions::TriggerMode::SetServo;
    expected.triggerDistance = 17.25;
    expected.stopTriggerAtStripEnds = true;
    expected.addTakeoff = true;
    expected.takeoffAltitude = 42.5;
    expected.finishAction = SurveyMissionOptions::FinishAction::Land;
    expected.useSplineWaypoints = true;
    expected.holdHeading = true;
    expected.heading = 271.25;
    expected.waypointDelay = 2.5;
    expected.servoNumber = 13;
    expected.servoPwm = 1780;
    expected.servoRepeatSeconds = 0.75;
    expected.servoLowPwm = 1050;
    expected.servoHighPwm = 1950;
    expected.splitCount = 3;
    expected.restoreSpeed = 4.25;

    dialog.setMissionOptions(expected);
    const SurveyMissionOptions actual = dialog.missionOptions();

    QCOMPARE(actual.useSpeed, expected.useSpeed);
    QCOMPARE(actual.flyingSpeed, expected.flyingSpeed);
    QCOMPARE(static_cast<int>(actual.triggerMode),
             static_cast<int>(expected.triggerMode));
    QCOMPARE(actual.triggerDistance, expected.triggerDistance);
    QCOMPARE(actual.stopTriggerAtStripEnds,
             expected.stopTriggerAtStripEnds);
    QCOMPARE(actual.addTakeoff, expected.addTakeoff);
    QCOMPARE(actual.takeoffAltitude, expected.takeoffAltitude);
    QCOMPARE(static_cast<int>(actual.finishAction),
             static_cast<int>(expected.finishAction));
    QCOMPARE(actual.useSplineWaypoints, expected.useSplineWaypoints);
    QCOMPARE(actual.holdHeading, expected.holdHeading);
    QCOMPARE(actual.heading, expected.heading);
    QCOMPARE(actual.waypointDelay, expected.waypointDelay);
    QCOMPARE(actual.servoNumber, expected.servoNumber);
    QCOMPARE(actual.servoPwm, expected.servoPwm);
    QCOMPARE(actual.servoRepeatSeconds, expected.servoRepeatSeconds);
    QCOMPARE(actual.servoLowPwm, expected.servoLowPwm);
    QCOMPARE(actual.servoHighPwm, expected.servoHighPwm);
    QCOMPARE(actual.splitCount, expected.splitCount);
    QCOMPARE(actual.restoreSpeed, expected.restoreSpeed);
}

void SurveyGridDialogTest::altitudePresentationRoundTripsCanonicalMeters()
{
    SurveyGridDialog dialog(rectangle());
    auto *altitude = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_altitude"));
    auto *takeoffAltitude = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_takeoffalt"));
    auto *altitudeLabel = dialog.findChild<QLabel *>(
        QStringLiteral("LBL_altitude"));
    auto *takeoffAltitudeLabel = dialog.findChild<QLabel *>(
        QStringLiteral("LBL_takeoffalt"));
    QVERIFY(altitude);
    QVERIFY(takeoffAltitude);
    QVERIFY(altitudeLabel);
    QVERIFY(takeoffAltitudeLabel);

    SurveyGridOptions gridOptions = dialog.options();
    gridOptions.altitudeMeters = 30.48;
    gridOptions.distanceMeters = 37.25;
    dialog.setOptions(gridOptions);
    SurveyMissionOptions missionOptions = dialog.missionOptions();
    missionOptions.takeoffAltitude = 30.48;
    missionOptions.triggerDistance = 17.25;
    dialog.setMissionOptions(missionOptions);
    QVERIFY(dialog.generate().success);
    const SurveyGridResult generated = dialog.gridResult();
    QSignalSpy parameterSpy(&dialog, &SurveyGridDialog::parametersChanged);
    QSignalSpy resultSpy(&dialog, &SurveyGridDialog::resultChanged);

    dialog.setAltitudePresentation(3.280839895013123,
                                   QStringLiteral("ft"));

    QCOMPARE(dialog.altitudeUnit(), QStringLiteral("ft"));
    QCOMPARE(dialog.altitudeMultiplier(), 3.280839895013123);
    QCOMPARE(altitude->value(), 100.0);
    QCOMPARE(takeoffAltitude->value(), 100.0);
    QCOMPARE(altitudeLabel->text(), QStringLiteral("Altitude (ft)"));
    QCOMPARE(takeoffAltitudeLabel->text(),
             QStringLiteral("Takeoff altitude (ft)"));
    QVERIFY(std::abs(dialog.options().altitudeMeters - 30.48) < 1e-9);
    QCOMPARE(dialog.options().distanceMeters, 37.25);
    QVERIFY(std::abs(dialog.missionOptions().takeoffAltitude - 30.48)
            < 1e-9);
    QCOMPARE(dialog.missionOptions().triggerDistance, 17.25);
    QCOMPARE(parameterSpy.count(), 0);
    QCOMPARE(resultSpy.count(), 0);
    QCOMPARE(dialog.gridResult().path.size(), generated.path.size());

    altitude->setValue(200.0);
    takeoffAltitude->setValue(150.0);
    QVERIFY(std::abs(dialog.options().altitudeMeters - 60.96) < 1e-9);
    QVERIFY(std::abs(dialog.missionOptions().takeoffAltitude - 45.72)
            < 1e-9);

    dialog.setAltitudePresentation(1.0, QStringLiteral("m"));
    QCOMPARE(altitude->value(), 61.0);
    QCOMPARE(takeoffAltitude->value(), 45.7);
    QVERIFY(std::abs(dialog.options().altitudeMeters - 60.96) < 1e-9);
    QVERIFY(std::abs(dialog.missionOptions().takeoffAltitude - 45.72)
            < 1e-9);
    QCOMPARE(altitudeLabel->text(), QStringLiteral("Altitude (m)"));
    QCOMPARE(takeoffAltitudeLabel->text(),
             QStringLiteral("Takeoff altitude (m)"));

    const QString tooltip = altitude->toolTip();
    dialog.setAltitudePresentation(0.0, QStringLiteral("invalid"));
    QCOMPARE(dialog.altitudeMultiplier(), 1.0);
    QCOMPARE(dialog.altitudeUnit(), QStringLiteral("m"));
    QCOMPARE(altitude->toolTip(), tooltip);
}

void SurveyGridDialogTest::livePreviewRecalculatesWhileEditing()
{
    SurveyGridDialog dialog(rectangle());
    auto *preview = dialog.findChild<QWidget *>(QStringLiteral("map"));
    auto *distance = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_Distance"));
    QVERIFY(preview);
    QVERIFY(distance);
    QCOMPARE(preview->property("boundaryPointCount").toInt(), 4);
    QVERIFY(preview->property("previewValid").toBool());
    const int initialGridPoints =
        preview->property("gridPointCount").toInt();
    QVERIFY(initialGridPoints > 0);
    QSignalSpy parameterSpy(&dialog, &SurveyGridDialog::parametersChanged);

    distance->setValue(20.0);

    QCOMPARE(parameterSpy.count(), 1);
    QVERIFY(preview->property("previewValid").toBool());
    QVERIFY(preview->property("gridPointCount").toInt()
            > initialGridPoints);
    QVERIFY(!dialog.gridResult().success);
}

void SurveyGridDialogTest::editingInvalidatesPreviousResult()
{
    SurveyGridDialog dialog(rectangle());
    QVERIFY(dialog.generate().success);
    auto *distance = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("NUM_Distance"));
    QVERIFY(distance);
    QSignalSpy parameterSpy(&dialog, &SurveyGridDialog::parametersChanged);

    distance->setValue(distance->value() + 1.0);

    QCOMPARE(parameterSpy.count(), 1);
    QVERIFY(!dialog.gridResult().success);
    QVERIFY(dialog.gridResult().path.isEmpty());
    QVERIFY(dialog.statusText().contains(QStringLiteral("Ready")));
}

QTEST_MAIN(SurveyGridDialogTest)
#include "test_surveygriddialog.moc"
