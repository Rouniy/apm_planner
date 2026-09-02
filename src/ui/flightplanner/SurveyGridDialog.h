#ifndef SURVEYGRIDDIALOG_H
#define SURVEYGRIDDIALOG_H

#include "SurveyGridGenerator.h"
#include "SurveyMissionBuilder.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QWidget;

// An editor for Mission Planner's Survey (Grid) geometry and mission options.
// The owning Flight Planner supplies the boundary and consumes gridResult()
// after explicit generation; the dialog maintains its own live preview.
class SurveyGridDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SurveyGridDialog(QWidget *parent = nullptr);
    explicit SurveyGridDialog(
        const QVector<SurveyGridCoordinate> &polygon,
        QWidget *parent = nullptr);

    void setPolygon(const QVector<SurveyGridCoordinate> &polygon);
    const QVector<SurveyGridCoordinate> &polygon() const;

    void setOptions(const SurveyGridOptions &options);
    SurveyGridOptions options() const;

    void setMissionOptions(const SurveyMissionOptions &options);
    SurveyMissionOptions missionOptions() const;

    // All SurveyGridOptions/SurveyMissionOptions values remain canonical SI.
    // This policy only controls the altitude editors shown to the user.
    void setAltitudePresentation(double multiplier, const QString &unit);
    double altitudeMultiplier() const;
    QString altitudeUnit() const;

    void setHomeLocation(const SurveyGridCoordinate &coordinate);
    void setStartPoint(const SurveyGridCoordinate &coordinate);

    // Generates without closing the dialog. The Accept button calls this and
    // closes the dialog only when generation succeeds.
    SurveyGridResult generate();
    const SurveyGridResult &gridResult() const;
    QString statusText() const;

signals:
    void parametersChanged();
    void resultChanged();

private slots:
    void invalidateResult();
    void acceptGeneratedGrid();

private:
    void setupUi();
    void updateStatus(const QString &text, bool error);
    void updatePreview();
    void updateMissionControlState();
    void applySuggestedAngle();

    static double longestSideBearing(
        const QVector<SurveyGridCoordinate> &polygon);

    QVector<SurveyGridCoordinate> m_polygon;
    SurveyGridOptions m_locationOptions;
    SurveyGridResult m_result;

    QDoubleSpinBox *m_altitude = nullptr;
    QLabel *m_altitudeLabel = nullptr;
    QDoubleSpinBox *m_distance = nullptr;
    QDoubleSpinBox *m_spacing = nullptr;
    QDoubleSpinBox *m_angle = nullptr;
    QDoubleSpinBox *m_overshoot1 = nullptr;
    QDoubleSpinBox *m_overshoot2 = nullptr;
    QDoubleSpinBox *m_leadin1 = nullptr;
    QDoubleSpinBox *m_leadin2 = nullptr;
    QComboBox *m_startFrom = nullptr;
    QCheckBox *m_crossGrid = nullptr;

    QComboBox *m_triggerMode = nullptr;
    QDoubleSpinBox *m_triggerDistance = nullptr;
    QCheckBox *m_stopTriggerAtStripEnds = nullptr;
    QDoubleSpinBox *m_flyingSpeed = nullptr;
    QCheckBox *m_useSpeed = nullptr;
    QDoubleSpinBox *m_restoreSpeed = nullptr;
    QCheckBox *m_addTakeoff = nullptr;
    QDoubleSpinBox *m_takeoffAltitude = nullptr;
    QLabel *m_takeoffAltitudeLabel = nullptr;
    QComboBox *m_finishAction = nullptr;
    QCheckBox *m_useSplineWaypoints = nullptr;
    QCheckBox *m_holdHeading = nullptr;
    QDoubleSpinBox *m_heading = nullptr;
    QDoubleSpinBox *m_waypointDelay = nullptr;
    QSpinBox *m_servoNumber = nullptr;
    QSpinBox *m_servoPwm = nullptr;
    QDoubleSpinBox *m_servoRepeatSeconds = nullptr;
    QSpinBox *m_servoLowPwm = nullptr;
    QSpinBox *m_servoHighPwm = nullptr;
    QSpinBox *m_splitCount = nullptr;

    QWidget *m_preview = nullptr;
    QLabel *m_boundaryPoints = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_acceptButton = nullptr;

    bool m_updatingControls = false;
    bool m_angleWasExplicitlySet = false;
    bool m_homeWasExplicitlySet = false;
    bool m_startPointWasExplicitlySet = false;
    double m_altitudeMeters = 100.0;
    double m_takeoffAltitudeMeters = 30.0;
    double m_altitudeMultiplier = 1.0;
    QString m_altitudeUnit = QStringLiteral("m");
};

#endif // SURVEYGRIDDIALOG_H
