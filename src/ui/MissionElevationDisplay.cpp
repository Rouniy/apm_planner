#include "MissionElevationDisplay.h"

#include "configuration/ElevationSourceService.h"
#include "flightplanner/FlightPlannerMissionModel.h"
#include "flightplanner/FlightPlannerViewModel.h"
#include "qcustomplot.h"
#include "ui_MissionElevationDisplay.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int PlannedPathGraphId = 0;
constexpr int DemGraphId = 1;
constexpr int DisplaySampleLimit = 4096;

double displayOrNaN(double canonicalMeters, double multiplier)
{
    return std::isfinite(canonicalMeters)
        ? canonicalMeters * multiplier
        : std::numeric_limits<double>::quiet_NaN();
}
}

MissionElevationDisplay::MissionElevationDisplay(
        FlightPlannerViewModel *plannerViewModel,
        ElevationSourceService *elevationSource,
        QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::MissionElevationDisplay)
    , m_plannerViewModel(plannerViewModel)
    , m_elevationSource(elevationSource)
{
    ui->setupUi(this);
    setObjectName(QStringLiteral("MissionElevationDisplay"));
    setMinimumSize(640, 320);
    ui->resolutionLabel->setWordWrap(true);
    ui->resolutionLabel->setFixedHeight(24);

    QCustomPlot *plot = ui->customPlot;
    plot->plotLayout()->insertRow(0);
    auto *title = new QCPTextElement(
        plot, tr("Elevation above ground"),
        QFont(font().family(), 12, QFont::Bold));
    title->setTextColor(Qt::white);
    plot->plotLayout()->addElement(
        0, 0,
        title);
    const QColor chartBackground(38, 39, 40);
    plot->setBackground(QBrush(chartBackground));
    plot->axisRect()->setBackground(QBrush(chartBackground));
    plot->xAxis->setBasePen(QPen(Qt::white));
    plot->xAxis->setTickPen(QPen(Qt::white));
    plot->xAxis->setSubTickPen(QPen(Qt::white));
    plot->xAxis->setTickLabelColor(Qt::white);
    plot->xAxis->setLabelColor(Qt::white);
    plot->yAxis->setBasePen(QPen(Qt::white));
    plot->yAxis->setTickPen(QPen(Qt::white));
    plot->yAxis->setSubTickPen(QPen(Qt::white));
    plot->yAxis->setTickLabelColor(QColor(220, 40, 40));
    plot->yAxis->setLabelColor(QColor(220, 40, 40));
    plot->addGraph();
    plot->graph(PlannedPathGraphId)->setName(tr("Planned Path"));
    plot->graph(PlannedPathGraphId)->setPen(QPen(QColor(220, 40, 40), 2));
    plot->addGraph();
    plot->graph(DemGraphId)->setName(tr("DEM"));
    plot->graph(DemGraphId)->setPen(QPen(QColor(50, 120, 220), 2));
    plot->legend->setVisible(true);
    plot->legend->setBrush(QBrush(chartBackground));
    plot->legend->setBorderPen(QPen(Qt::white));
    plot->legend->setTextColor(Qt::white);
    plot->xAxis->grid()->setVisible(true);
    plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(150);
    connect(&m_refreshTimer, &QTimer::timeout,
            this, &MissionElevationDisplay::RebuildProfile);

    if (m_plannerViewModel) {
        FlightPlannerMissionModel *model = m_plannerViewModel->Waypoints();
        connect(model, &FlightPlannerMissionModel::rowsChanged,
                this, [this](FlightPlannerMissionModel::MissionStore store) {
            if (store == FlightPlannerMissionModel::MissionStore::Mission) {
                scheduleProfileRebuild();
            }
        });
        connect(m_plannerViewModel,
                &FlightPlannerViewModel::homeLatChanged,
                this, [this](double) { scheduleProfileRebuild(); });
        connect(m_plannerViewModel,
                &FlightPlannerViewModel::homeLngChanged,
                this, [this](double) { scheduleProfileRebuild(); });
        connect(m_plannerViewModel,
                &FlightPlannerViewModel::homeAltChanged,
                this, [this](double) { scheduleProfileRebuild(); });
        connect(m_plannerViewModel,
                &FlightPlannerViewModel::homeValidChanged,
                this, [this](bool) { scheduleProfileRebuild(); });
        connect(m_plannerViewModel,
                &FlightPlannerViewModel::altitudePresentationChanged,
                this, &MissionElevationDisplay::renderProfile);
        connect(m_plannerViewModel,
                &FlightPlannerViewModel::distancePresentationChanged,
                this, &MissionElevationDisplay::renderProfile);
        connect(m_plannerViewModel, &QObject::destroyed, this, [this]() {
            m_refreshTimer.stop();
            m_profile = {};
            renderProfile();
        });
    }
    if (m_elevationSource) {
        connect(m_elevationSource,
                &ElevationSourceService::srtmTileAvailable,
                this, [this](const QString &) {
            scheduleProfileRebuild();
        });
        connect(m_elevationSource,
                &ElevationSourceService::nativeRastersChanged,
                this, &MissionElevationDisplay::scheduleProfileRebuild);
    }
    RebuildProfile();
}

MissionElevationDisplay::~MissionElevationDisplay()
{
    m_refreshTimer.stop();
    delete ui;
}

const MissionElevationProfileResult &MissionElevationDisplay::Profile() const
{
    return m_profile;
}

bool MissionElevationDisplay::HasUsableMission() const
{
    return m_profile.hasRoute();
}

void MissionElevationDisplay::scheduleProfileRebuild()
{
    if (!m_refreshTimer.isActive()) m_refreshTimer.start();
}

void MissionElevationDisplay::RebuildProfile()
{
    m_refreshTimer.stop();
    if (!m_plannerViewModel) {
        m_profile = {};
    } else {
        m_profile = m_plannerViewModel->BuildElevationProfile(
            10.0, DisplaySampleLimit);
    }
    renderProfile();
    emit profileChanged();
}

void MissionElevationDisplay::renderProfile()
{
    QCustomPlot *plot = ui->customPlot;
    plot->clearItems();
    const double altitudeMultiplier = m_plannerViewModel
        ? m_plannerViewModel->AltitudeMultiplier() : 1.0;
    const double distanceMultiplier = m_plannerViewModel
        ? m_plannerViewModel->DistanceMultiplier() : 1.0;
    const QString altitudeUnit = m_plannerViewModel
        ? m_plannerViewModel->AltUnit() : QStringLiteral("m");
    const QString distanceUnit = m_plannerViewModel
        ? m_plannerViewModel->DistanceUnit() : QStringLiteral("m");
    plot->xAxis->setLabel(
        tr("Distance (%1)").arg(distanceUnit));
    plot->yAxis->setLabel(
        tr("Elevation (%1)").arg(altitudeUnit));

    QVector<double> distances;
    QVector<double> terrain;
    QVector<double> planned;
    distances.reserve(m_profile.samples.size());
    terrain.reserve(m_profile.samples.size());
    planned.reserve(m_profile.samples.size());
    for (const MissionElevationSample &sample : m_profile.samples) {
        distances.append(sample.distanceMeters * distanceMultiplier);
        terrain.append(displayOrNaN(
            sample.terrainAltitudeAmslMeters, altitudeMultiplier));
        planned.append(displayOrNaN(
            sample.plannedAltitudeAmslMeters, altitudeMultiplier));
    }
    plot->graph(PlannedPathGraphId)->setData(distances, planned);
    plot->graph(DemGraphId)->setData(distances, terrain);

    for (const MissionElevationMarker &marker : m_profile.markers) {
        if (!std::isfinite(marker.plannedAltitudeAmslMeters)) continue;
        auto *text = new QCPItemText(plot);
        text->setText(marker.label);
        text->setColor(Qt::white);
        text->setRotation(90.0);
        text->setPen(Qt::NoPen);
        text->setBrush(Qt::NoBrush);
        text->setClipToAxisRect(false);
        text->position->setType(QCPItemPosition::ptPlotCoords);
        text->position->setAxes(plot->xAxis, plot->yAxis);
        text->position->setCoords(
            marker.distanceMeters * distanceMultiplier,
            marker.plannedAltitudeAmslMeters * altitudeMultiplier);
        text->setPositionAlignment(
            (marker.distanceMeters <= 0.0
                 ? Qt::AlignLeft : Qt::AlignRight)
            | Qt::AlignVCenter);
    }

    if (!m_profile.hasRoute()) {
        plot->xAxis->setRange(0.0, 50.0 * distanceMultiplier);
        plot->yAxis->setRange(0.0, 25.0 * altitudeMultiplier);
        ui->resolutionLabel->setText(
            tr("Need at least two route points (HOME + waypoint or two waypoints)."));
    } else {
        plot->rescaleAxes();
        QCPRange yRange = plot->yAxis->range();
        if (std::isfinite(yRange.lower) && std::isfinite(yRange.upper)) {
            const double span = std::max(1.0, yRange.size());
            plot->yAxis->setRange(
                yRange.lower - span * 0.1,
                yRange.upper + span * 0.1);
        }
        if (m_profile.missingTerrainSampleCount > 0) {
            ui->resolutionLabel->setText(
                tr("DEM missing for %1 of %2 samples; gaps are left unknown.")
                    .arg(m_profile.missingTerrainSampleCount)
                    .arg(m_profile.terrainSampleCount));
        } else {
            ui->resolutionLabel->setText(
                tr("%1 samples · spacing ≤ %2 %3")
                    .arg(m_profile.samples.size())
                    .arg(m_profile.sampleSpacingMeters
                             * distanceMultiplier, 0, 'f', 1)
                    .arg(distanceUnit));
        }
    }
    plot->replot();
}
