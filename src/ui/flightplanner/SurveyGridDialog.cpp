#include "SurveyGridDialog.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kDegreesToRadians = 0.017453292519943295769;
constexpr double kRadiansToDegrees = 57.295779513082320877;

QDoubleSpinBox *createNumber(QWidget *parent, const QString &objectName,
                             double minimum, double maximum, int decimals,
                             double step, double value)
{
    auto *number = new QDoubleSpinBox(parent);
    number->setObjectName(objectName);
    number->setRange(minimum, maximum);
    number->setDecimals(decimals);
    number->setSingleStep(step);
    number->setKeyboardTracking(false);
    number->setValue(value);
    return number;
}

QSpinBox *createInteger(QWidget *parent, const QString &objectName,
                        int minimum, int maximum, int value)
{
    auto *number = new QSpinBox(parent);
    number->setObjectName(objectName);
    number->setRange(minimum, maximum);
    number->setValue(value);
    number->setKeyboardTracking(false);
    return number;
}

double haversineDistanceSquared(const SurveyGridCoordinate &first,
                                const SurveyGridCoordinate &second)
{
    const double firstLatitude = first.latitude * kDegreesToRadians;
    const double secondLatitude = second.latitude * kDegreesToRadians;
    const double latitudeDelta = secondLatitude - firstLatitude;
    const double longitudeDelta =
        (second.longitude - first.longitude) * kDegreesToRadians;
    const double sinLatitude = std::sin(latitudeDelta * 0.5);
    const double sinLongitude = std::sin(longitudeDelta * 0.5);
    return sinLatitude * sinLatitude
        + std::cos(firstLatitude) * std::cos(secondLatitude)
            * sinLongitude * sinLongitude;
}

class SurveyGridPreviewWidget final : public QWidget
{
public:
    explicit SurveyGridPreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(220);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAutoFillBackground(true);
    }

    void setGeometryData(const QVector<SurveyGridCoordinate> &boundary,
                         const SurveyGridResult &grid)
    {
        m_boundary = boundary;
        m_grid = grid;
        setProperty("boundaryPointCount", m_boundary.size());
        setProperty("gridPointCount", m_grid.path.size());
        setProperty("previewValid", m_grid.success);
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), palette().color(QPalette::Base));

        QVector<SurveyGridCoordinate> coordinates = m_boundary;
        coordinates.reserve(coordinates.size() + m_grid.path.size());
        for (const SurveyGridPoint &point : m_grid.path) {
            coordinates.append(point.coordinate);
        }
        if (coordinates.isEmpty()) {
            painter.setPen(palette().color(QPalette::Disabled,
                                           QPalette::Text));
            painter.drawText(rect(), Qt::AlignCenter,
                             tr("Survey preview"));
            return;
        }

        const double referenceLongitude = coordinates.first().longitude;
        double latitudeSum = 0.0;
        int latitudeCount = 0;
        for (const SurveyGridCoordinate &coordinate : coordinates) {
            if (std::isfinite(coordinate.latitude)) {
                latitudeSum += coordinate.latitude;
                ++latitudeCount;
            }
        }
        const double meanLatitude = latitudeCount > 0
            ? latitudeSum / latitudeCount : 0.0;
        const double longitudeScale = std::max(
            0.01, std::abs(std::cos(meanLatitude * kDegreesToRadians)));

        auto project = [referenceLongitude, longitudeScale](
                           const SurveyGridCoordinate &coordinate) {
            double longitudeDelta = std::fmod(
                coordinate.longitude - referenceLongitude + 540.0, 360.0)
                - 180.0;
            return QPointF(longitudeDelta * longitudeScale,
                           -coordinate.latitude);
        };

        double minimumX = std::numeric_limits<double>::max();
        double maximumX = std::numeric_limits<double>::lowest();
        double minimumY = std::numeric_limits<double>::max();
        double maximumY = std::numeric_limits<double>::lowest();
        for (const SurveyGridCoordinate &coordinate : coordinates) {
            if (!std::isfinite(coordinate.latitude)
                || !std::isfinite(coordinate.longitude)) {
                continue;
            }
            const QPointF point = project(coordinate);
            minimumX = std::min(minimumX, point.x());
            maximumX = std::max(maximumX, point.x());
            minimumY = std::min(minimumY, point.y());
            maximumY = std::max(maximumY, point.y());
        }
        if (minimumX > maximumX || minimumY > maximumY) {
            return;
        }

        const QRectF viewport = QRectF(rect()).adjusted(14.0, 14.0,
                                                        -14.0, -14.0);
        const double geometryWidth = std::max(maximumX - minimumX, 1.0e-9);
        const double geometryHeight = std::max(maximumY - minimumY, 1.0e-9);
        const double scale = std::min(viewport.width() / geometryWidth,
                                      viewport.height() / geometryHeight);
        const QPointF geometryCenter((minimumX + maximumX) * 0.5,
                                     (minimumY + maximumY) * 0.5);
        const QPointF viewportCenter = viewport.center();
        auto mapPoint = [&project, geometryCenter, viewportCenter, scale](
                            const SurveyGridCoordinate &coordinate) {
            return viewportCenter
                + (project(coordinate) - geometryCenter) * scale;
        };

        QPolygonF boundaryPolygon;
        boundaryPolygon.reserve(m_boundary.size());
        for (const SurveyGridCoordinate &coordinate : m_boundary) {
            if (std::isfinite(coordinate.latitude)
                && std::isfinite(coordinate.longitude)) {
                boundaryPolygon.append(mapPoint(coordinate));
            }
        }
        if (boundaryPolygon.size() >= 2) {
            QColor boundaryFill(50, 135, 210, 38);
            painter.setBrush(boundaryFill);
            painter.setPen(QPen(QColor(45, 125, 205), 2.0));
            painter.drawPolygon(boundaryPolygon);
        }

        QPolygonF gridPath;
        gridPath.reserve(m_grid.path.size());
        for (const SurveyGridPoint &point : m_grid.path) {
            if (std::isfinite(point.coordinate.latitude)
                && std::isfinite(point.coordinate.longitude)) {
                gridPath.append(mapPoint(point.coordinate));
            }
        }
        if (gridPath.size() >= 2) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(225, 120, 35), 1.6));
            painter.drawPolyline(gridPath);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(225, 120, 35));
        for (const SurveyGridPoint &point : m_grid.path) {
            if (point.type == SurveyGridPointType::Photo) {
                painter.drawEllipse(mapPoint(point.coordinate), 1.8, 1.8);
            }
        }
    }

private:
    QVector<SurveyGridCoordinate> m_boundary;
    SurveyGridResult m_grid;
};

} // namespace

SurveyGridDialog::SurveyGridDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    invalidateResult();
}

SurveyGridDialog::SurveyGridDialog(
    const QVector<SurveyGridCoordinate> &polygon, QWidget *parent)
    : SurveyGridDialog(parent)
{
    setPolygon(polygon);
}

void SurveyGridDialog::setPolygon(
    const QVector<SurveyGridCoordinate> &polygon)
{
    m_polygon = polygon;
    m_boundaryPoints->setText(tr("%n boundary point(s)", nullptr,
                                 static_cast<int>(m_polygon.size())));

    if (!m_polygon.isEmpty()) {
        if (!m_homeWasExplicitlySet) {
            m_locationOptions.homeLocation = m_polygon.first();
        }
        if (!m_startPointWasExplicitlySet) {
            m_locationOptions.startPoint = m_polygon.first();
        }
    }
    applySuggestedAngle();
    invalidateResult();
}

const QVector<SurveyGridCoordinate> &SurveyGridDialog::polygon() const
{
    return m_polygon;
}

void SurveyGridDialog::setOptions(const SurveyGridOptions &newOptions)
{
    m_updatingControls = true;
    m_altitudeMeters = newOptions.altitudeMeters;
    m_altitude->setValue(
        m_altitudeMeters * m_altitudeMultiplier);
    m_distance->setValue(newOptions.distanceMeters);
    m_spacing->setValue(newOptions.spacingMeters);
    m_angle->setValue(newOptions.angleDegrees);
    m_overshoot1->setValue(newOptions.overshoot1Meters);
    m_overshoot2->setValue(newOptions.overshoot2Meters);
    m_leadin1->setValue(newOptions.leadin1Meters);
    m_leadin2->setValue(newOptions.leadin2Meters);
    const int startIndex = m_startFrom->findData(
        static_cast<int>(newOptions.startPosition));
    m_startFrom->setCurrentIndex(startIndex >= 0 ? startIndex : 0);
    m_crossGrid->setChecked(newOptions.crossGrid);
    m_updatingControls = false;

    m_locationOptions.homeLocation = newOptions.homeLocation;
    m_locationOptions.startPoint = newOptions.startPoint;
    m_homeWasExplicitlySet = true;
    m_startPointWasExplicitlySet = true;
    m_angleWasExplicitlySet = true;
    invalidateResult();
}

SurveyGridOptions SurveyGridDialog::options() const
{
    SurveyGridOptions current;
    current.altitudeMeters = m_altitudeMeters;
    current.distanceMeters = m_distance->value();
    current.spacingMeters = m_spacing->value();
    current.angleDegrees = m_angle->value();
    current.overshoot1Meters = m_overshoot1->value();
    current.overshoot2Meters = m_overshoot2->value();
    current.leadin1Meters = m_leadin1->value();
    current.leadin2Meters = m_leadin2->value();
    current.crossGrid = m_crossGrid->isChecked();
    current.startPosition = static_cast<SurveyGridOptions::StartPosition>(
        m_startFrom->currentData().toInt());
    current.homeLocation = m_locationOptions.homeLocation;
    current.startPoint = m_locationOptions.startPoint;
    return current;
}

void SurveyGridDialog::setMissionOptions(
    const SurveyMissionOptions &newOptions)
{
    m_updatingControls = true;
    m_takeoffAltitudeMeters = newOptions.takeoffAltitude;
    m_useSpeed->setChecked(newOptions.useSpeed);
    m_flyingSpeed->setValue(newOptions.flyingSpeed);
    const int triggerIndex = m_triggerMode->findData(
        static_cast<int>(newOptions.triggerMode));
    m_triggerMode->setCurrentIndex(triggerIndex >= 0 ? triggerIndex : 0);
    m_triggerDistance->setValue(newOptions.triggerDistance);
    m_stopTriggerAtStripEnds->setChecked(
        newOptions.stopTriggerAtStripEnds);
    m_addTakeoff->setChecked(newOptions.addTakeoff);
    m_takeoffAltitude->setValue(
        m_takeoffAltitudeMeters * m_altitudeMultiplier);
    const int finishIndex = m_finishAction->findData(
        static_cast<int>(newOptions.finishAction));
    m_finishAction->setCurrentIndex(finishIndex >= 0 ? finishIndex : 0);
    m_useSplineWaypoints->setChecked(newOptions.useSplineWaypoints);
    m_holdHeading->setChecked(newOptions.holdHeading);
    m_heading->setValue(newOptions.heading);
    m_waypointDelay->setValue(newOptions.waypointDelay);
    m_servoNumber->setValue(newOptions.servoNumber);
    m_servoPwm->setValue(newOptions.servoPwm);
    m_servoRepeatSeconds->setValue(newOptions.servoRepeatSeconds);
    m_servoLowPwm->setValue(newOptions.servoLowPwm);
    m_servoHighPwm->setValue(newOptions.servoHighPwm);
    m_splitCount->setValue(newOptions.splitCount);
    m_restoreSpeed->setValue(newOptions.restoreSpeed);
    m_updatingControls = false;

    updateMissionControlState();
    invalidateResult();
}

SurveyMissionOptions SurveyGridDialog::missionOptions() const
{
    SurveyMissionOptions current;
    current.useSpeed = m_useSpeed->isChecked();
    current.flyingSpeed = m_flyingSpeed->value();
    current.triggerMode = static_cast<SurveyMissionOptions::TriggerMode>(
        m_triggerMode->currentData().toInt());
    current.triggerDistance = m_triggerDistance->value();
    current.stopTriggerAtStripEnds =
        m_stopTriggerAtStripEnds->isChecked();
    current.addTakeoff = m_addTakeoff->isChecked();
    current.takeoffAltitude = m_takeoffAltitudeMeters;
    current.finishAction = static_cast<SurveyMissionOptions::FinishAction>(
        m_finishAction->currentData().toInt());
    current.useSplineWaypoints = m_useSplineWaypoints->isChecked();
    current.holdHeading = m_holdHeading->isChecked();
    current.heading = m_heading->value();
    current.waypointDelay = m_waypointDelay->value();
    current.servoNumber = m_servoNumber->value();
    current.servoPwm = m_servoPwm->value();
    current.servoRepeatSeconds = m_servoRepeatSeconds->value();
    current.servoLowPwm = m_servoLowPwm->value();
    current.servoHighPwm = m_servoHighPwm->value();
    current.splitCount = m_splitCount->value();
    current.restoreSpeed = m_restoreSpeed->value();
    return current;
}

void SurveyGridDialog::setAltitudePresentation(
        double multiplier, const QString &unit)
{
    const QString normalizedUnit = unit.trimmed();
    if (!std::isfinite(multiplier) || multiplier <= 0.0
        || normalizedUnit.isEmpty()
        || (m_altitudeMultiplier == multiplier
            && m_altitudeUnit == normalizedUnit)) {
        return;
    }

    const SurveyGridOptions canonicalGrid = options();
    const SurveyMissionOptions canonicalMission = missionOptions();
    m_altitudeMultiplier = multiplier;
    m_altitudeUnit = normalizedUnit;

    m_updatingControls = true;
    m_altitude->setRange(-100.0 * multiplier, 99999.0 * multiplier);
    m_altitude->setSingleStep(10.0 * multiplier);
    m_takeoffAltitude->setRange(0.0, 10000.0 * multiplier);
    m_takeoffAltitude->setSingleStep(multiplier);
    m_altitudeLabel->setText(tr("Altitude (%1)").arg(m_altitudeUnit));
    m_takeoffAltitudeLabel->setText(
        tr("Takeoff altitude (%1)").arg(m_altitudeUnit));
    m_altitude->setValue(
        canonicalGrid.altitudeMeters * m_altitudeMultiplier);
    m_takeoffAltitude->setValue(
        canonicalMission.takeoffAltitude * m_altitudeMultiplier);
    m_updatingControls = false;
}

double SurveyGridDialog::altitudeMultiplier() const
{
    return m_altitudeMultiplier;
}

QString SurveyGridDialog::altitudeUnit() const
{
    return m_altitudeUnit;
}

void SurveyGridDialog::setHomeLocation(
    const SurveyGridCoordinate &coordinate)
{
    m_locationOptions.homeLocation = coordinate;
    m_homeWasExplicitlySet = true;
    invalidateResult();
}

void SurveyGridDialog::setStartPoint(
    const SurveyGridCoordinate &coordinate)
{
    m_locationOptions.startPoint = coordinate;
    m_startPointWasExplicitlySet = true;
    invalidateResult();
}

SurveyGridResult SurveyGridDialog::generate()
{
    m_result = SurveyGridGenerator::CreateGrid(m_polygon, options());
    static_cast<SurveyGridPreviewWidget *>(m_preview)->setGeometryData(
        m_polygon, m_result);
    if (m_result.success) {
        int photoCount = 0;
        for (const SurveyGridPoint &point : m_result.path) {
            if (point.type == SurveyGridPointType::Photo) {
                ++photoCount;
            }
        }
        updateStatus(
            tr("Generated %1 point(s): %2 strip(s), %3 photo point(s).")
                .arg(m_result.path.size())
                .arg(m_result.transects.size())
                .arg(photoCount),
            false);
    } else {
        updateStatus(tr("Grid generation failed: %1").arg(m_result.error),
                     true);
    }
    emit resultChanged();
    return m_result;
}

const SurveyGridResult &SurveyGridDialog::gridResult() const
{
    return m_result;
}

QString SurveyGridDialog::statusText() const
{
    return m_status->text();
}

void SurveyGridDialog::invalidateResult()
{
    if (m_updatingControls) {
        return;
    }

    if (sender() == m_altitude) {
        m_altitudeMeters = m_altitude->value() / m_altitudeMultiplier;
    } else if (sender() == m_takeoffAltitude) {
        m_takeoffAltitudeMeters =
            m_takeoffAltitude->value() / m_altitudeMultiplier;
    }
    if (sender() == m_angle) {
        m_angleWasExplicitlySet = true;
    }
    m_result = SurveyGridResult();
    updateMissionControlState();
    updatePreview();
    if (m_polygon.size() < 3) {
        updateStatus(
            tr("Need at least 3 polygon points to generate a survey."), true);
    } else {
        updateStatus(tr("Ready to generate survey grid."), false);
    }
    emit parametersChanged();
    emit resultChanged();
}

void SurveyGridDialog::acceptGeneratedGrid()
{
    if (generate().success) {
        accept();
    }
}

void SurveyGridDialog::setupUi()
{
    setObjectName(QStringLiteral("GridUI"));
    setWindowTitle(tr("Survey (Grid)"));
    setModal(true);
    resize(760, 720);

    auto *mainLayout = new QVBoxLayout(this);
    auto *heading = new QLabel(tr("Survey (Grid)"), this);
    heading->setObjectName(QStringLiteral("LBL_title"));
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    mainLayout->addWidget(heading);

    m_boundaryPoints = new QLabel(this);
    m_boundaryPoints->setObjectName(QStringLiteral("LBL_boundary_points"));
    m_boundaryPoints->setText(tr("0 boundary point(s)"));
    mainLayout->addWidget(m_boundaryPoints);

    m_preview = new SurveyGridPreviewWidget(this);
    m_preview->setObjectName(QStringLiteral("map"));
    mainLayout->addWidget(m_preview, 1);

    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("tabControl1"));

    auto *simpleTab = new QWidget(tabs);
    simpleTab->setObjectName(QStringLiteral("tabSimple"));
    auto *simpleLayout = new QFormLayout(simpleTab);
    simpleLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_altitude = createNumber(simpleTab, QStringLiteral("NUM_altitude"),
                              -100.0, 99999.0, 1, 10.0, 100.0);
    m_angle = createNumber(simpleTab, QStringLiteral("NUM_angle"), 0.0,
                           360.0, 1, 1.0, 0.0);
    m_altitudeLabel = new QLabel(tr("Altitude (m)"), simpleTab);
    m_altitudeLabel->setObjectName(QStringLiteral("LBL_altitude"));
    simpleLayout->addRow(m_altitudeLabel, m_altitude);
    simpleLayout->addRow(tr("Angle (deg)"), m_angle);
    tabs->addTab(simpleTab, tr("Simple"));

    auto *missionTab = new QWidget(tabs);
    missionTab->setObjectName(QStringLiteral("tabMission"));
    auto *missionTabLayout = new QVBoxLayout(missionTab);
    missionTabLayout->setContentsMargins(0, 0, 0, 0);
    auto *missionScroll = new QScrollArea(missionTab);
    missionScroll->setObjectName(QStringLiteral("SCR_mission"));
    missionScroll->setWidgetResizable(true);
    auto *missionContents = new QWidget(missionScroll);
    auto *missionLayout = new QFormLayout(missionContents);
    missionLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_triggerMode = new QComboBox(missionContents);
    m_triggerMode->setObjectName(QStringLiteral("CMB_triggermode"));
    const struct {
        const char *name;
        SurveyMissionOptions::TriggerMode value;
    } triggerModes[] = {
        {"None", SurveyMissionOptions::TriggerMode::None},
        {"Distance", SurveyMissionOptions::TriggerMode::Distance},
        {"Digicam", SurveyMissionOptions::TriggerMode::Digicam},
        {"Repeat servo", SurveyMissionOptions::TriggerMode::RepeatServo},
        {"Set servo", SurveyMissionOptions::TriggerMode::SetServo},
    };
    for (const auto &triggerMode : triggerModes) {
        m_triggerMode->addItem(QString::fromLatin1(triggerMode.name),
                               static_cast<int>(triggerMode.value));
    }
    m_triggerDistance = createNumber(
        missionContents, QStringLiteral("NUM_trigdist"), 0.0, 10000.0,
        2, 1.0, 30.0);
    m_stopTriggerAtStripEnds = new QCheckBox(
        tr("Start/stop trigger at strip ends"), missionContents);
    m_stopTriggerAtStripEnds->setObjectName(
        QStringLiteral("chk_stopstart"));

    m_useSpeed = new QCheckBox(
        tr("Add speed command at survey start"), missionContents);
    m_useSpeed->setObjectName(QStringLiteral("CHK_usespeed"));
    m_flyingSpeed = createNumber(
        missionContents, QStringLiteral("NUM_UpDownFlySpeed"), 0.0,
        200.0, 2, 0.5, 5.0);
    m_restoreSpeed = createNumber(
        missionContents, QStringLiteral("NUM_restoreSpeed"), 0.0,
        200.0, 2, 0.5, 0.0);

    m_addTakeoff = new QCheckBox(tr("Add takeoff command"),
                                  missionContents);
    m_addTakeoff->setObjectName(QStringLiteral("CHK_toandland"));
    m_takeoffAltitude = createNumber(
        missionContents, QStringLiteral("NUM_takeoffalt"), 0.0, 10000.0,
        1, 1.0, 30.0);
    m_finishAction = new QComboBox(missionContents);
    m_finishAction->setObjectName(QStringLiteral("CMB_finishaction"));
    m_finishAction->addItem(
        tr("None"),
        static_cast<int>(SurveyMissionOptions::FinishAction::None));
    m_finishAction->addItem(
        tr("RTL"), static_cast<int>(
                       SurveyMissionOptions::FinishAction::ReturnToLaunch));
    m_finishAction->addItem(
        tr("Land"),
        static_cast<int>(SurveyMissionOptions::FinishAction::Land));

    m_useSplineWaypoints = new QCheckBox(
        tr("Use spline waypoints"), missionContents);
    m_useSplineWaypoints->setObjectName(QStringLiteral("CHK_spline"));
    m_holdHeading = new QCheckBox(tr("Hold heading"), missionContents);
    m_holdHeading->setObjectName(
        QStringLiteral("CHK_copter_headinghold"));
    m_heading = createNumber(
        missionContents, QStringLiteral("TXT_headinghold"), 0.0, 359.99,
        2, 1.0, 0.0);
    m_waypointDelay = createNumber(
        missionContents, QStringLiteral("NUM_copter_delay"), 0.0, 3600.0,
        2, 0.5, 0.0);

    m_servoNumber = createInteger(
        missionContents, QStringLiteral("NUM_reptservo"), 1, 16, 9);
    m_servoPwm = createInteger(
        missionContents, QStringLiteral("num_reptpwm"), 800, 2200, 1900);
    m_servoRepeatSeconds = createNumber(
        missionContents, QStringLiteral("NUM_repttime"), 0.0, 60.0,
        2, 0.1, 1.0);
    m_servoLowPwm = createInteger(
        missionContents, QStringLiteral("num_setservolow"), 800, 2200,
        1100);
    m_servoHighPwm = createInteger(
        missionContents, QStringLiteral("num_setservohigh"), 800, 2200,
        1900);
    m_splitCount = createInteger(
        missionContents, QStringLiteral("NUM_split"), 1, 100, 1);

    missionLayout->addRow(tr("Trigger mode"), m_triggerMode);
    missionLayout->addRow(tr("Trigger distance (m)"), m_triggerDistance);
    missionLayout->addRow(QString(), m_stopTriggerAtStripEnds);
    missionLayout->addRow(QString(), m_useSpeed);
    missionLayout->addRow(tr("Flying speed (m/s)"), m_flyingSpeed);
    missionLayout->addRow(tr("Restore speed (m/s)"), m_restoreSpeed);
    missionLayout->addRow(QString(), m_addTakeoff);
    m_takeoffAltitudeLabel = new QLabel(
        tr("Takeoff altitude (m)"), missionContents);
    m_takeoffAltitudeLabel->setObjectName(
        QStringLiteral("LBL_takeoffalt"));
    missionLayout->addRow(m_takeoffAltitudeLabel, m_takeoffAltitude);
    missionLayout->addRow(tr("Finish with"), m_finishAction);
    missionLayout->addRow(QString(), m_useSplineWaypoints);
    missionLayout->addRow(QString(), m_holdHeading);
    missionLayout->addRow(tr("Heading (deg)"), m_heading);
    missionLayout->addRow(tr("Waypoint delay (s)"), m_waypointDelay);
    missionLayout->addRow(tr("Servo number"), m_servoNumber);
    missionLayout->addRow(tr("Servo PWM"), m_servoPwm);
    missionLayout->addRow(tr("Servo repeat (s)"),
                          m_servoRepeatSeconds);
    missionLayout->addRow(tr("Servo low PWM"), m_servoLowPwm);
    missionLayout->addRow(tr("Servo high PWM"), m_servoHighPwm);
    missionLayout->addRow(tr("Split into flights"), m_splitCount);

    missionScroll->setWidget(missionContents);
    missionTabLayout->addWidget(missionScroll);
    tabs->addTab(missionTab, tr("Mission"));

    auto *optionsTab = new QWidget(tabs);
    optionsTab->setObjectName(QStringLiteral("tabGrid"));
    auto *optionsLayout = new QFormLayout(optionsTab);
    optionsLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_distance = createNumber(optionsTab, QStringLiteral("NUM_Distance"),
                              0.1, 9999.0, 2, 1.0, 50.0);
    m_spacing = createNumber(optionsTab, QStringLiteral("NUM_spacing"),
                             0.0, 5000.0, 1, 1.0, 30.0);
    m_overshoot1 = createNumber(optionsTab, QStringLiteral("NUM_overshoot"),
                                -999.0, 9999.0, 1, 1.0, 0.0);
    m_overshoot2 = createNumber(optionsTab,
                                QStringLiteral("NUM_overshoot2"), -999.0,
                                9999.0, 1, 1.0, 0.0);
    m_leadin1 = createNumber(optionsTab, QStringLiteral("NUM_leadin"),
                             -999.0, 9999.0, 1, 1.0, 0.0);
    m_leadin2 = createNumber(optionsTab, QStringLiteral("NUM_leadin2"),
                             -999.0, 9999.0, 1, 1.0, 0.0);

    auto *overshootEditor = new QWidget(optionsTab);
    overshootEditor->setObjectName(QStringLiteral("PAN_overshoot"));
    auto *overshootLayout = new QHBoxLayout(overshootEditor);
    overshootLayout->setContentsMargins(0, 0, 0, 0);
    overshootLayout->addWidget(m_overshoot1);
    overshootLayout->addWidget(m_overshoot2);

    auto *leadinEditor = new QWidget(optionsTab);
    leadinEditor->setObjectName(QStringLiteral("PAN_leadin"));
    auto *leadinLayout = new QHBoxLayout(leadinEditor);
    leadinLayout->setContentsMargins(0, 0, 0, 0);
    leadinLayout->addWidget(m_leadin1);
    leadinLayout->addWidget(m_leadin2);

    m_startFrom = new QComboBox(optionsTab);
    m_startFrom->setObjectName(QStringLiteral("CMB_startfrom"));
    const struct {
        const char *name;
        SurveyGridOptions::StartPosition value;
    } startPositions[] = {
        {"Home", SurveyGridOptions::StartPosition::Home},
        {"BottomLeft", SurveyGridOptions::StartPosition::BottomLeft},
        {"TopLeft", SurveyGridOptions::StartPosition::TopLeft},
        {"BottomRight", SurveyGridOptions::StartPosition::BottomRight},
        {"TopRight", SurveyGridOptions::StartPosition::TopRight},
        {"Point", SurveyGridOptions::StartPosition::Point},
    };
    for (const auto &startPosition : startPositions) {
        m_startFrom->addItem(QString::fromLatin1(startPosition.name),
                             static_cast<int>(startPosition.value));
    }

    m_crossGrid = new QCheckBox(tr("Cross Grid"), optionsTab);
    m_crossGrid->setObjectName(QStringLiteral("chk_crossgrid"));

    optionsLayout->addRow(tr("Distance between lines (m)"), m_distance);
    optionsLayout->addRow(tr("Photo spacing (m)"), m_spacing);
    optionsLayout->addRow(tr("Overshoot 1 / 2 (m)"), overshootEditor);
    optionsLayout->addRow(tr("Lead-in 1 / 2 (m)"), leadinEditor);
    optionsLayout->addRow(tr("Start From"), m_startFrom);
    optionsLayout->addRow(QString(), m_crossGrid);
    tabs->addTab(optionsTab, tr("Grid Options"));
    mainLayout->addWidget(tabs);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("LBL_status"));
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(m_status->fontMetrics().height() * 2);
    mainLayout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    buttons->setObjectName(QStringLiteral("buttonBox"));
    m_acceptButton = buttons->addButton(tr("Accept"),
                                        QDialogButtonBox::AcceptRole);
    m_acceptButton->setObjectName(QStringLiteral("BUT_Accept"));
    QPushButton *cancelButton = buttons->addButton(
        tr("Cancel"), QDialogButtonBox::RejectRole);
    cancelButton->setObjectName(QStringLiteral("BUT_Cancel"));
    mainLayout->addWidget(buttons);

    const QDoubleSpinBox *numbers[] = {
        m_altitude, m_distance, m_spacing, m_angle,
        m_overshoot1, m_overshoot2, m_leadin1, m_leadin2,
        m_triggerDistance, m_flyingSpeed,
        m_restoreSpeed, m_takeoffAltitude, m_heading, m_waypointDelay,
        m_servoRepeatSeconds,
    };
    for (const QDoubleSpinBox *number : numbers) {
        connect(number, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &SurveyGridDialog::invalidateResult);
    }
    connect(m_startFrom, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SurveyGridDialog::invalidateResult);
    connect(m_triggerMode,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SurveyGridDialog::invalidateResult);
    connect(m_finishAction,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SurveyGridDialog::invalidateResult);
    connect(m_crossGrid, &QCheckBox::toggled,
            this, &SurveyGridDialog::invalidateResult);
    const QCheckBox *missionChecks[] = {
        m_stopTriggerAtStripEnds, m_useSpeed, m_addTakeoff,
        m_useSplineWaypoints, m_holdHeading,
    };
    for (const QCheckBox *check : missionChecks) {
        connect(check, &QCheckBox::toggled,
                this, &SurveyGridDialog::invalidateResult);
    }
    const QSpinBox *missionIntegers[] = {
        m_servoNumber, m_servoPwm, m_servoLowPwm, m_servoHighPwm,
        m_splitCount,
    };
    for (const QSpinBox *number : missionIntegers) {
        connect(number, qOverload<int>(&QSpinBox::valueChanged),
                this, &SurveyGridDialog::invalidateResult);
    }
    connect(m_acceptButton, &QPushButton::clicked,
            this, &SurveyGridDialog::acceptGeneratedGrid);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
}

void SurveyGridDialog::updateStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setProperty("error", error);
    QPalette statusPalette = m_status->palette();
    statusPalette.setColor(QPalette::WindowText,
                           error ? QColor(190, 55, 55)
                                 : palette().color(QPalette::WindowText));
    m_status->setPalette(statusPalette);
}

void SurveyGridDialog::updatePreview()
{
    const SurveyGridResult previewResult = SurveyGridGenerator::CreateGrid(
        m_polygon, options());
    static_cast<SurveyGridPreviewWidget *>(m_preview)->setGeometryData(
        m_polygon, previewResult);
}

void SurveyGridDialog::updateMissionControlState()
{
    const auto triggerMode = static_cast<SurveyMissionOptions::TriggerMode>(
        m_triggerMode->currentData().toInt());
    const bool usesServo =
        triggerMode == SurveyMissionOptions::TriggerMode::RepeatServo
        || triggerMode == SurveyMissionOptions::TriggerMode::SetServo;

    m_flyingSpeed->setEnabled(m_useSpeed->isChecked());
    m_restoreSpeed->setEnabled(m_useSpeed->isChecked());
    m_triggerDistance->setEnabled(
        triggerMode == SurveyMissionOptions::TriggerMode::Distance);
    m_stopTriggerAtStripEnds->setEnabled(
        triggerMode == SurveyMissionOptions::TriggerMode::Distance
        || triggerMode == SurveyMissionOptions::TriggerMode::RepeatServo);
    m_takeoffAltitude->setEnabled(m_addTakeoff->isChecked());
    m_heading->setEnabled(m_holdHeading->isChecked());
    m_servoNumber->setEnabled(usesServo);
    m_servoPwm->setEnabled(
        triggerMode == SurveyMissionOptions::TriggerMode::RepeatServo);
    m_servoRepeatSeconds->setEnabled(
        triggerMode == SurveyMissionOptions::TriggerMode::RepeatServo);
    m_servoLowPwm->setEnabled(
        triggerMode == SurveyMissionOptions::TriggerMode::SetServo);
    m_servoHighPwm->setEnabled(
        triggerMode == SurveyMissionOptions::TriggerMode::SetServo);
}

void SurveyGridDialog::applySuggestedAngle()
{
    if (m_angleWasExplicitlySet || m_polygon.size() < 2) {
        return;
    }
    m_updatingControls = true;
    m_angle->setValue(longestSideBearing(m_polygon));
    m_updatingControls = false;
}

double SurveyGridDialog::longestSideBearing(
    const QVector<SurveyGridCoordinate> &polygon)
{
    if (polygon.size() < 2) {
        return 0.0;
    }

    double longestDistance = -1.0;
    double longestBearing = 0.0;
    SurveyGridCoordinate previous = polygon.last();
    for (const SurveyGridCoordinate &coordinate : polygon) {
        const double distance = haversineDistanceSquared(previous, coordinate);
        if (std::isfinite(distance) && distance > longestDistance) {
            const double firstLatitude = previous.latitude * kDegreesToRadians;
            const double secondLatitude = coordinate.latitude
                * kDegreesToRadians;
            const double longitudeDelta =
                (coordinate.longitude - previous.longitude)
                * kDegreesToRadians;
            const double y = std::sin(longitudeDelta)
                * std::cos(secondLatitude);
            const double x = std::cos(firstLatitude)
                    * std::sin(secondLatitude)
                - std::sin(firstLatitude) * std::cos(secondLatitude)
                    * std::cos(longitudeDelta);
            longestBearing = std::atan2(y, x) * kRadiansToDegrees;
            longestDistance = distance;
        }
        previous = coordinate;
    }
    longestBearing = std::fmod(longestBearing + 360.0, 360.0);
    return std::isfinite(longestBearing) ? longestBearing : 0.0;
}
