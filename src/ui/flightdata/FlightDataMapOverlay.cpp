#include "FlightDataMapOverlay.h"

#include "ui/map/AbstractMapWidget.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>

namespace {
QString translated(const char *text)
{
    return QCoreApplication::translate("FlightDataMapOverlay", text);
}

QWidget *anchorWidget(QWidget *parent, int left, int top, int right, int bottom)
{
    auto *anchor = new QWidget(parent);
    anchor->setAttribute(Qt::WA_StyledBackground, false);
    auto *layout = new QHBoxLayout(anchor);
    layout->setContentsMargins(left, top, right, bottom);
    layout->setSpacing(0);
    return anchor;
}

QFrame *card(QWidget *parent, const QString &objectName)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(objectName);
    frame->setFrameShape(QFrame::NoFrame);
    frame->setStyleSheet(QStringLiteral(
        "QFrame#%1 {"
        " background-color: rgba(0, 0, 0, 153);"
        " border: 0px;"
        " border-radius: 4px;"
        "}"
        "QFrame#%1 QLabel, QFrame#%1 QCheckBox { color: white; }"
        "QFrame#%1 QProgressBar {"
        " background-color: #161B18;"
        " border: 1px solid #2A322D;"
        " border-radius: 3px;"
        "}"
        "QFrame#%1 QProgressBar::chunk {"
        " background-color: #34D399;"
        " border-radius: 2px;"
        "}").arg(objectName));
    return frame;
}
}

FlightDataMapOverlay::FlightDataMapOverlay(AbstractMapWidget *mapBackend,
                                           QGridLayout *mapLayout,
                                           QWidget *mapHost)
    : QObject(mapHost),
      m_mapBackend(mapBackend)
{
    Q_ASSERT(mapLayout);
    Q_ASSERT(mapHost);

    auto *controlsAnchor = anchorWidget(mapHost, 8, 8, 0, 0);
    auto *controls = card(controlsAnchor, QStringLiteral("FlightMapControls"));
    auto *controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(6, 3, 6, 3);
    controlsLayout->setSpacing(8);
    m_autoPanCheckBox = new QCheckBox(translated("Auto Pan"), controls);
    m_autoPanCheckBox->setObjectName(QStringLiteral("AutoPanCheckBox"));
    m_clearTrackButton = new QPushButton(translated("Clear Track"), controls);
    m_clearTrackButton->setObjectName(QStringLiteral("ClearTrackButton"));
    m_clearTrackButton->setProperty("compact", true);
    controlsLayout->addWidget(m_autoPanCheckBox);
    controlsLayout->addWidget(m_clearTrackButton);
    controlsAnchor->layout()->addWidget(controls);
    mapLayout->addWidget(controlsAnchor, 0, 0,
                         Qt::AlignTop | Qt::AlignLeft);

    auto *telemetryAnchor = anchorWidget(mapHost, 8, 0, 0, 8);
    auto *telemetry = card(telemetryAnchor,
                           QStringLiteral("FlightMapTelemetry"));
    auto *telemetryLayout = new QVBoxLayout(telemetry);
    telemetryLayout->setContentsMargins(6, 3, 6, 3);
    telemetryLayout->setSpacing(1);
    m_gpsLabel = new QLabel(telemetry);
    m_gpsLabel->setObjectName(QStringLiteral("FlightMapGpsText"));
    m_motionLabel = new QLabel(telemetry);
    m_motionLabel->setObjectName(QStringLiteral("FlightMapMotionText"));
    telemetryLayout->addWidget(m_gpsLabel);
    telemetryLayout->addWidget(m_motionLabel);
    telemetryAnchor->layout()->addWidget(telemetry);
    mapLayout->addWidget(telemetryAnchor, 0, 0,
                         Qt::AlignBottom | Qt::AlignLeft);

    auto *progressAnchor = anchorWidget(mapHost, 0, 0, 8, 8);
    auto *progress = card(progressAnchor,
                          QStringLiteral("FlightMissionProgress"));
    progress->setMinimumWidth(220);
    progress->setMaximumWidth(430);
    progress->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *progressLayout = new QVBoxLayout(progress);
    progressLayout->setContentsMargins(7, 4, 7, 4);
    progressLayout->setSpacing(2);
    m_missionLabel = new QLabel(progress);
    m_missionLabel->setObjectName(QStringLiteral("FlightMissionProgressText"));
    m_missionLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_missionProgress = new QProgressBar(progress);
    m_missionProgress->setObjectName(QStringLiteral("FlightMissionProgressBar"));
    m_missionProgress->setRange(0, 1000);
    m_missionProgress->setTextVisible(false);
    m_missionProgress->setFixedHeight(7);
    auto *trackLegend = new QLabel(translated("GPS track"), progress);
    trackLegend->setObjectName(QStringLiteral("FlightMapTrackLegend"));
    trackLegend->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont legendFont = trackLegend->font();
    legendFont.setPointSizeF(qMax(7.0, legendFont.pointSizeF() - 1.0));
    trackLegend->setFont(legendFont);
    progressLayout->addWidget(m_missionLabel);
    progressLayout->addWidget(m_missionProgress);
    progressLayout->addWidget(trackLegend);
    progressAnchor->layout()->addWidget(progress);
    mapLayout->addWidget(progressAnchor, 0, 0,
                         Qt::AlignBottom | Qt::AlignRight);

    const bool mapAvailable = !m_mapBackend.isNull();
    m_autoPanCheckBox->setEnabled(mapAvailable);
    m_clearTrackButton->setEnabled(mapAvailable);
    if (mapAvailable) {
        m_autoPanCheckBox->setChecked(m_mapBackend->FollowUAVEnabled());
    }
    connect(m_autoPanCheckBox, &QCheckBox::toggled, this,
            [this](bool enabled) {
        if (m_mapBackend) {
            m_mapBackend->SetFollowUAVEnabled(enabled);
        }
    });
    connect(m_clearTrackButton, &QPushButton::clicked, this, [this]() {
        if (m_mapBackend) {
            m_mapBackend->DeleteTrails();
        }
    });

    controlsAnchor->raise();
    telemetryAnchor->raise();
    progressAnchor->raise();
    setTelemetry(0.0, 0.0, 0.0, 0.0, 0.0,
                 translated("No mission loaded"), 0.0);
}

void FlightDataMapOverlay::setTelemetry(double satelliteCount,
                                        double gpsHdop,
                                        double groundSpeed,
                                        double windDirection,
                                        double windSpeed,
                                        const QString &missionProgressText,
                                        double missionProgress)
{
    m_gpsLabel->setText(translated("Sats: %1    HDOP: %2")
                            .arg(qRound(satelliteCount))
                            .arg(gpsHdop, 0, 'f', 1));
    m_motionLabel->setText(translated("GS: %1 m/s    Wind: %2\u00b0  %3 m/s")
                               .arg(groundSpeed, 0, 'f', 1)
                               .arg(windDirection, 0, 'f', 0)
                               .arg(windSpeed, 0, 'f', 1));
    m_missionLabel->setText(missionProgressText);
    m_missionLabel->setToolTip(missionProgressText);
    m_missionProgress->setValue(
        qBound(0, qRound(missionProgress * 10.0), 1000));
}
