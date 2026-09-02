#include "FlightPlannerActionPanel.h"

#include "FlightPlannerViewModel.h"
#include "FenceRallyController.h"
#include "SurveyGridDialog.h"
#include "SurveyMissionBuilder.h"
#include "ui/map/MapTileSourceFactory.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace {
QPushButton *disabledButton(const QString &text, const QString &objectName,
                            const QString &reason, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(objectName);
    button->setEnabled(false);
    button->setToolTip(reason);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return button;
}

QFrame *separator(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

QLabel *fieldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return label;
}
}

FlightPlannerActionPanel::FlightPlannerActionPanel(
    FlightPlannerViewModel *viewModel, QWidget *parent)
    : QWidget(parent)
    , m_viewModel(viewModel)
{
    m_fenceRallyController = new MissionPlanner::FenceRallyController(
        viewModel, this);
    setObjectName(QStringLiteral("ActionPanel"));
    setMinimumWidth(168);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *scroller = new QScrollArea(this);
    scroller->setObjectName(QStringLiteral("ActionScroller"));
    scroller->setWidgetResizable(true);
    scroller->setFrameShape(QFrame::NoFrame);
    scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rootLayout->addWidget(scroller);

    auto *items = new QWidget(scroller);
    items->setObjectName(QStringLiteral("ActionItemsPanel"));
    auto *itemsLayout = new QVBoxLayout(items);
    itemsLayout->setContentsMargins(6, 6, 6, 6);
    itemsLayout->setSpacing(6);
    itemsLayout->setAlignment(Qt::AlignTop);
    scroller->setWidget(items);

    auto *grid = new QCheckBox(tr("Grid"), items);
    grid->setObjectName(QStringLiteral("ShowGridCheckBox"));
    grid->setEnabled(false);
    grid->setToolTip(tr("Planner graticule rendering is not implemented yet."));
    itemsLayout->addWidget(grid);

    itemsLayout->addWidget(disabledButton(
        tr("View KML"), QStringLiteral("ViewKmlButton"),
        tr("Mission KML preview is not implemented yet."), items));

    m_mapTypeCombo = new QComboBox(items);
    m_mapTypeCombo->setObjectName(QStringLiteral("MapTypeComboBox"));
    m_mapTypeCombo->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_mapTypeCombo->setMinimumContentsLength(12);
    m_mapTypeCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    const QStringList mapTypeNames = core::MapType::TypesList();
    for (const QString &name : mapTypeNames) {
        const core::MapType::Types type = core::MapType::TypeByStr(name);
        if (MapTileSourceFactory::IsKnownMapType(type)) {
            m_mapTypeCombo->addItem(name, static_cast<int>(type));
        }
    }
    itemsLayout->addWidget(m_mapTypeCombo);

    m_statusLabel = new QLabel(items);
    m_statusLabel->setObjectName(QStringLiteral("Status"));
    m_statusLabel->setWordWrap(true);
    itemsLayout->addWidget(m_statusLabel);

    auto *ogcRow = new QWidget(items);
    auto *ogcLayout = new QHBoxLayout(ogcRow);
    ogcLayout->setContentsMargins(0, 0, 0, 0);
    ogcLayout->setSpacing(6);
    ogcLayout->addWidget(disabledButton(
        tr("WMS…"), QStringLiteral("WmsButton"),
        tr("WMS map sources are not implemented yet."), ogcRow));
    ogcLayout->addWidget(disabledButton(
        tr("WMTS…"), QStringLiteral("WmtsButton"),
        tr("WMTS map sources are not implemented yet."), ogcRow));
    itemsLayout->addWidget(ogcRow);

    itemsLayout->addWidget(disabledButton(
        tr("Inject Custom Map"), QStringLiteral("InjectCustomMapButton"),
        tr("Custom URL tile sources are not implemented yet."), items));

    auto *polygonTitle = fieldLabel(tr("Polygon"), items);
    polygonTitle->setObjectName(QStringLiteral("PolygonLabel"));
    QFont polygonTitleFont = polygonTitle->font();
    polygonTitleFont.setBold(true);
    polygonTitle->setFont(polygonTitleFont);
    itemsLayout->addWidget(polygonTitle);

    m_polygonDrawButton = new QPushButton(tr("Draw Polygon"), items);
    m_polygonDrawButton->setObjectName(QStringLiteral("DrawPolygonButton"));
    m_polygonDrawButton->setCheckable(true);
    itemsLayout->addWidget(m_polygonDrawButton);

    m_polygonClearButton = new QPushButton(tr("Clear Polygon"), items);
    m_polygonClearButton->setObjectName(QStringLiteral("ClearPolygonButton"));
    itemsLayout->addWidget(m_polygonClearButton);

    m_polygonFromWaypointsButton = new QPushButton(
            tr("From Current Waypoints"), items);
    m_polygonFromWaypointsButton->setObjectName(
            QStringLiteral("BuildPolygonFromWaypointsButton"));
    itemsLayout->addWidget(m_polygonFromWaypointsButton);

    auto *polygonFileRow = new QWidget(items);
    auto *polygonFileLayout = new QHBoxLayout(polygonFileRow);
    polygonFileLayout->setContentsMargins(0, 0, 0, 0);
    polygonFileLayout->setSpacing(6);
    m_polygonLoadButton = new QPushButton(tr("Load .poly"), polygonFileRow);
    m_polygonLoadButton->setObjectName(QStringLiteral("LoadPolygonButton"));
    m_polygonSaveButton = new QPushButton(tr("Save .poly"), polygonFileRow);
    m_polygonSaveButton->setObjectName(QStringLiteral("SavePolygonButton"));
    polygonFileLayout->addWidget(m_polygonLoadButton);
    polygonFileLayout->addWidget(m_polygonSaveButton);
    itemsLayout->addWidget(polygonFileRow);

    auto *polygonOffsetRow = new QWidget(items);
    auto *polygonOffsetLayout = new QHBoxLayout(polygonOffsetRow);
    polygonOffsetLayout->setContentsMargins(0, 0, 0, 0);
    polygonOffsetLayout->setSpacing(6);
    m_polygonOffset = new QDoubleSpinBox(polygonOffsetRow);
    m_polygonOffset->setObjectName(QStringLiteral("PolygonOffsetMeters"));
    m_polygonOffset->setRange(-100000.0, 100000.0);
    m_polygonOffset->setDecimals(2);
    m_polygonOffset->setValue(5.0);
    m_polygonOffset->setSuffix(tr(" m"));
    m_polygonOffset->setKeyboardTracking(false);
    m_polygonOffsetButton = new QPushButton(tr("Offset"), polygonOffsetRow);
    m_polygonOffsetButton->setObjectName(
            QStringLiteral("OffsetDrawnPolygonButton"));
    polygonOffsetLayout->addWidget(m_polygonOffset, 1);
    polygonOffsetLayout->addWidget(m_polygonOffsetButton);
    itemsLayout->addWidget(polygonOffsetRow);

    m_polygonAreaLabel = new QLabel(items);
    m_polygonAreaLabel->setObjectName(QStringLiteral("PolygonArea"));
    m_polygonAreaLabel->setWordWrap(true);
    itemsLayout->addWidget(m_polygonAreaLabel);

    m_fenceInclusionButton = new QPushButton(
            tr("Inclusion Fence from Polygon"), items);
    m_fenceInclusionButton->setObjectName(
            QStringLiteral("FenceInclusionFromPolygonButton"));
    itemsLayout->addWidget(m_fenceInclusionButton);
    m_fenceExclusionButton = new QPushButton(
            tr("Exclusion Fence from Polygon"), items);
    m_fenceExclusionButton->setObjectName(
            QStringLiteral("FenceExclusionFromPolygonButton"));
    itemsLayout->addWidget(m_fenceExclusionButton);

    m_surveyGridButton = new QPushButton(tr("Survey (Grid)"), items);
    m_surveyGridButton->setObjectName(QStringLiteral("SurveyGridButton"));
    m_surveyGridButton->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_surveyGridButton->setEnabled(!m_viewModel.isNull());
    itemsLayout->addWidget(m_surveyGridButton);
    itemsLayout->addWidget(separator(items));

    m_loadButton = new QPushButton(tr("Load File"), items);
    m_loadButton->setObjectName(QStringLiteral("LoadFileButton"));
    m_appendButton = new QPushButton(tr("Load + Append"), items);
    m_appendButton->setObjectName(QStringLiteral("LoadAndAppendButton"));
    m_saveButton = new QPushButton(tr("Save File"), items);
    m_saveButton->setObjectName(QStringLiteral("SaveFileButton"));
    for (QPushButton *button : {m_loadButton, m_appendButton, m_saveButton}) {
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setEnabled(!m_viewModel.isNull());
        itemsLayout->addWidget(button);
    }
    itemsLayout->addWidget(separator(items));

    itemsLayout->addWidget(fieldLabel(tr("Mission Type"), items));
    m_missionTypeCombo = new QComboBox(items);
    m_missionTypeCombo->setObjectName(QStringLiteral("MissionTypeComboBox"));
    m_missionTypeCombo->addItems({QStringLiteral("Mission"),
                                  QStringLiteral("Fence"),
                                  QStringLiteral("Rally")});
    m_missionTypeCombo->setEnabled(!m_viewModel.isNull());
    itemsLayout->addWidget(m_missionTypeCombo);

    m_useMavFtp = new QCheckBox(tr("Use MAVFTP"), items);
    m_useMavFtp->setObjectName(QStringLiteral("UseMissionMavFtp"));
    m_useMavFtp->setToolTip(tr(
        "Fast MAVFTP Mission/Fence/Rally transfer is not implemented yet."));
    m_useMavFtp->setEnabled(false);
    itemsLayout->addWidget(m_useMavFtp);

    m_readButton = new QPushButton(tr("Read"), items);
    m_readButton->setObjectName(QStringLiteral("ReadWaypointsButton"));
    m_readButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    itemsLayout->addWidget(m_readButton);

    m_writeButton = new QPushButton(tr("Write"), items);
    m_writeButton->setObjectName(QStringLiteral("WriteWaypointsButton"));
    m_writeButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    itemsLayout->addWidget(m_writeButton);

    itemsLayout->addWidget(disabledButton(
        tr("Write Fast"), QStringLiteral("WriteWaypointsFastButton"),
        tr("Fast MAVFTP mission upload is not implemented yet."), items));

    m_cancelButton = new QPushButton(tr("Cancel"), items);
    m_cancelButton->setObjectName(QStringLiteral("CancelTransferButton"));
    m_cancelButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    itemsLayout->addWidget(m_cancelButton);

    m_transferProgress = new QProgressBar(items);
    m_transferProgress->setObjectName(
            QStringLiteral("MissionTransferProgress"));
    m_transferProgress->setRange(0, 100);
    m_transferProgress->setValue(0);
    m_transferProgress->setTextVisible(true);
    itemsLayout->addWidget(m_transferProgress);
    itemsLayout->addWidget(separator(items));

    auto *homeTitle = fieldLabel(tr("Home Location"), items);
    homeTitle->setObjectName(QStringLiteral("HomeLocationLabel"));
    QFont titleFont = homeTitle->font();
    titleFont.setBold(true);
    homeTitle->setFont(titleFont);
    itemsLayout->addWidget(homeTitle);

    const auto makeCoordinateEditor = [items](const QString &objectName,
                                               double minimum,
                                               double maximum,
                                               int decimals) {
        auto *editor = new QDoubleSpinBox(items);
        editor->setObjectName(objectName);
        editor->setRange(minimum, maximum);
        editor->setDecimals(decimals);
        editor->setKeyboardTracking(false);
        editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return editor;
    };

    itemsLayout->addWidget(fieldLabel(tr("Lat"), items));
    m_homeLat = makeCoordinateEditor(QStringLiteral("HomeLat"), -90.0, 90.0, 7);
    itemsLayout->addWidget(m_homeLat);
    itemsLayout->addWidget(fieldLabel(tr("Long"), items));
    m_homeLng = makeCoordinateEditor(QStringLiteral("HomeLng"), -180.0, 180.0, 7);
    itemsLayout->addWidget(m_homeLng);
    itemsLayout->addWidget(fieldLabel(tr("ASL"), items));
    m_homeAlt = makeCoordinateEditor(QStringLiteral("HomeAltDisplay"),
                                     -10000.0, 100000.0, 2);
    itemsLayout->addWidget(m_homeAlt);

    m_setHomeButton = new QPushButton(tr("Set from Vehicle"), items);
    m_setHomeButton->setObjectName(QStringLiteral("SetHomeFromVehicleButton"));
    m_setHomeButton->setEnabled(!m_viewModel.isNull());
    m_setHomeButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    itemsLayout->addWidget(m_setHomeButton);

    if (m_viewModel) {
        syncMissionType(m_viewModel->MissionType());
        syncStatus(m_viewModel->Status());
        m_useMavFtp->setChecked(m_viewModel->UseMavFtp());
        m_homeLat->setValue(m_viewModel->HomeLat());
        m_homeLng->setValue(m_viewModel->HomeLng());
        syncAltitudePresentation();
        syncTransferState();

        connect(m_loadButton, &QPushButton::clicked,
                this, [this]() { loadFile(false); });
        connect(m_appendButton, &QPushButton::clicked,
                this, [this]() { loadFile(true); });
        connect(m_saveButton, &QPushButton::clicked,
                this, &FlightPlannerActionPanel::saveFile);
        connect(m_surveyGridButton, &QPushButton::clicked,
                this, &FlightPlannerActionPanel::openSurveyGrid);
        connect(m_polygonDrawButton, &QPushButton::toggled,
                m_viewModel, &FlightPlannerViewModel::setPolygonDrawMode);
        connect(m_polygonClearButton, &QPushButton::clicked,
                m_viewModel, &FlightPlannerViewModel::ClearPolygon);
        connect(m_polygonFromWaypointsButton, &QPushButton::clicked,
                m_viewModel,
                &FlightPlannerViewModel::BuildPolygonFromWaypoints);
        connect(m_polygonLoadButton, &QPushButton::clicked,
                this, &FlightPlannerActionPanel::loadPolygon);
        connect(m_polygonSaveButton, &QPushButton::clicked,
                this, &FlightPlannerActionPanel::savePolygon);
        connect(m_polygonOffsetButton, &QPushButton::clicked,
                this, &FlightPlannerActionPanel::offsetPolygon);
        connect(m_polygonOffset,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { syncPolygonState(); });
        connect(m_fenceInclusionButton, &QPushButton::clicked,
                this, [this]() {
                    m_viewModel->AddDrawnPolygonToFence(true);
                });
        connect(m_fenceExclusionButton, &QPushButton::clicked,
                this, [this]() {
                    m_viewModel->AddDrawnPolygonToFence(false);
                });
        connect(m_setHomeButton, &QPushButton::clicked,
                m_viewModel,
                qOverload<>(&FlightPlannerViewModel::SetHomeFromVehicle));
        connect(m_readButton, &QPushButton::clicked,
                m_viewModel, &FlightPlannerViewModel::ReadWaypoints);
        connect(m_writeButton, &QPushButton::clicked,
                m_viewModel, &FlightPlannerViewModel::WriteWaypoints);
        connect(m_cancelButton, &QPushButton::clicked,
                m_viewModel, &FlightPlannerViewModel::CancelTransfer);
        connect(m_missionTypeCombo, &QComboBox::currentTextChanged,
                m_viewModel, &FlightPlannerViewModel::setMissionType);
        connect(m_useMavFtp, &QCheckBox::toggled,
                m_viewModel, &FlightPlannerViewModel::setUseMavFtp);
        connect(m_homeLat,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                m_viewModel, &FlightPlannerViewModel::setHomeLat);
        connect(m_homeLng,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                m_viewModel, &FlightPlannerViewModel::setHomeLng);
        connect(m_homeAlt,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                m_viewModel, &FlightPlannerViewModel::setHomeAltDisplay);

        connect(m_viewModel, &FlightPlannerViewModel::missionTypeChanged,
                this, &FlightPlannerActionPanel::syncMissionType);
        connect(m_viewModel->Waypoints(),
                &FlightPlannerMissionModel::rowsChanged,
                this, [this](FlightPlannerMissionModel::MissionStore) {
                    syncTransferState();
                });
        connect(m_viewModel->DrawnPolygon(),
                &FlightPlannerPolygonModel::DrawnPolygonChanged,
                this, &FlightPlannerActionPanel::syncTransferState);
        connect(m_viewModel,
                &FlightPlannerViewModel::polygonDrawModeChanged,
                this, [this](bool enabled) {
                    const QSignalBlocker blocker(m_polygonDrawButton);
                    m_polygonDrawButton->setChecked(enabled);
                });
        connect(m_viewModel, &FlightPlannerViewModel::homeValidChanged,
                this, [this](bool) { syncSaveAvailability(); });
        connect(m_viewModel, &FlightPlannerViewModel::statusChanged,
                this, &FlightPlannerActionPanel::syncStatus);
        connect(m_viewModel, &FlightPlannerViewModel::transferBusyChanged,
                this, [this](bool) { syncTransferState(); });
        connect(m_viewModel, &FlightPlannerViewModel::transferProgressChanged,
                this, [this](int progress) {
                    m_transferProgress->setValue(progress);
                });
        connect(m_viewModel,
                &FlightPlannerViewModel::transferAvailabilityChanged,
                this, &FlightPlannerActionPanel::syncTransferState);
        connect(m_viewModel,
                &FlightPlannerViewModel::vehicleHomeProviderChanged,
                this, [this](bool) { syncTransferState(); });
        connect(m_viewModel, &FlightPlannerViewModel::useMavFtpChanged,
                this, [this](bool enabled) {
                    const QSignalBlocker blocker(m_useMavFtp);
                    m_useMavFtp->setChecked(enabled);
                });
        connect(m_viewModel, &FlightPlannerViewModel::homeLatChanged,
                this, [this](double value) {
                    const QSignalBlocker blocker(m_homeLat);
                    m_homeLat->setValue(value);
                });
        connect(m_viewModel, &FlightPlannerViewModel::homeLngChanged,
                this, [this](double value) {
                    const QSignalBlocker blocker(m_homeLng);
                    m_homeLng->setValue(value);
                });
        connect(m_viewModel, &FlightPlannerViewModel::homeAltDisplayChanged,
                this, [this](double value) {
                    const QSignalBlocker blocker(m_homeAlt);
                    m_homeAlt->setValue(value);
                });
        connect(m_viewModel, &FlightPlannerViewModel::altUnitsChanged,
                this, [this](const QString &) {
                    syncAltitudePresentation();
                });
        connect(m_viewModel, &QObject::destroyed, this, [this]() {
            for (QPushButton *button : {m_loadButton, m_appendButton,
                                        m_saveButton, m_setHomeButton,
                                        m_readButton, m_writeButton,
                                        m_cancelButton, m_surveyGridButton,
                                        m_polygonDrawButton,
                                        m_polygonClearButton,
                                        m_polygonFromWaypointsButton,
                                        m_polygonLoadButton,
                                        m_polygonSaveButton,
                                        m_polygonOffsetButton,
                                        m_fenceInclusionButton,
                                        m_fenceExclusionButton}) {
                button->setEnabled(false);
            }
            m_missionTypeCombo->setEnabled(false);
            m_useMavFtp->setEnabled(false);
            m_homeLat->setEnabled(false);
            m_homeLng->setEnabled(false);
            m_homeAlt->setEnabled(false);
            m_transferProgress->setEnabled(false);
            m_polygonOffset->setEnabled(false);
            syncStatus(tr("Flight planner model is unavailable."));
        });
    } else {
        syncStatus(tr("Flight planner model is unavailable."));
        m_homeLat->setEnabled(false);
        m_homeLng->setEnabled(false);
        m_homeAlt->setEnabled(false);
        m_readButton->setEnabled(false);
        m_writeButton->setEnabled(false);
        m_cancelButton->setEnabled(false);
        m_surveyGridButton->setEnabled(false);
        for (QPushButton *button : {m_polygonDrawButton,
                                    m_polygonClearButton,
                                    m_polygonFromWaypointsButton,
                                    m_polygonLoadButton,
                                    m_polygonSaveButton,
                                    m_polygonOffsetButton,
                                    m_fenceInclusionButton,
                                    m_fenceExclusionButton}) {
            button->setEnabled(false);
        }
        m_polygonOffset->setEnabled(false);
        m_transferProgress->setEnabled(false);
    }

    MapTileSourceFactory *mapFactory = MapTileSourceFactory::instance();
    syncMapType(static_cast<int>(mapFactory->CurrentMapType()));
    connect(m_mapTypeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, mapFactory](int index) {
                if (index < 0) return;
                const auto requested = static_cast<core::MapType::Types>(
                    m_mapTypeCombo->itemData(index).toInt());
                if (!mapFactory->SetMapType(requested)
                    && !mapFactory->LastStatus().isEmpty()) {
                    syncStatus(mapFactory->LastStatus());
                }
                syncMapType(static_cast<int>(mapFactory->CurrentMapType()));
            });
    connect(mapFactory, &MapTileSourceFactory::MapTypeChanged,
            this, [this](core::MapType::Types type) {
                syncMapType(static_cast<int>(type));
            });
    connect(mapFactory, &MapTileSourceFactory::StatusMessage,
            this, &FlightPlannerActionPanel::syncStatus);
}

void FlightPlannerActionPanel::loadFile(bool append)
{
    if (!m_viewModel) return;
    const QString type = m_viewModel->MissionType();
    QString filter;
    if (type == QStringLiteral("Fence")) {
        filter = tr("QGC Plan (*.plan);;Fence Files (*.fen);;All Files (*)");
    } else if (type == QStringLiteral("Rally")) {
        filter = tr("QGC Plan (*.plan);;Rally Files (*.ral);;All Files (*)");
    } else {
        filter = tr("QGC Plan (*.plan);;Waypoint Files (*.waypoints *.txt);;All Files (*)");
    }
    const QString path = QFileDialog::getOpenFileName(
        this, append ? tr("Load and Append %1").arg(type)
                     : tr("Load %1").arg(type),
        QString(), filter);
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().compare(
            QStringLiteral("plan"), Qt::CaseInsensitive) == 0) {
        m_viewModel->LoadPlanFile(path, append);
    } else if (type == QStringLiteral("Fence")) {
        m_fenceRallyController->LoadLegacyFence(path, append);
    } else if (type == QStringLiteral("Rally")) {
        m_fenceRallyController->LoadLegacyRally(path, append);
    } else if (append) {
        m_viewModel->LoadAndAppend(path);
    } else {
        m_viewModel->LoadFile(path);
    }
}

void FlightPlannerActionPanel::saveFile()
{
    if (!m_viewModel) return;
    const QString type = m_viewModel->MissionType();
    QString extension;
    QString filter;
    if (type == QStringLiteral("Fence")) {
        extension = QStringLiteral("plan");
        filter = tr("QGC Plan (*.plan);;Fence Files (*.fen);;All Files (*)");
    } else if (type == QStringLiteral("Rally")) {
        extension = QStringLiteral("plan");
        filter = tr("QGC Plan (*.plan);;Rally Files (*.ral);;All Files (*)");
    } else {
        extension = QStringLiteral("plan");
        filter = tr("QGC Plan (*.plan);;Waypoint Files (*.waypoints *.txt);;All Files (*)");
    }
    const QString suggested = QStringLiteral("%1.%2")
        .arg(type.toLower(), extension);
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save %1").arg(type), suggested, filter);
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().compare(
            QStringLiteral("plan"), Qt::CaseInsensitive) == 0) {
        m_viewModel->SavePlanFile(path);
    } else if (type == QStringLiteral("Fence")) {
        m_fenceRallyController->SaveLegacyFence(path);
    } else if (type == QStringLiteral("Rally")) {
        m_fenceRallyController->SaveLegacyRally(path);
    } else {
        m_viewModel->SaveFile(path);
    }
}

void FlightPlannerActionPanel::loadPolygon()
{
    if (!m_viewModel || m_viewModel->TransferBusy()) return;
    const QString path = QFileDialog::getOpenFileName(
            this, tr("Load Polygon"), QString(),
            tr("Polygon Files (*.poly);;All Files (*)"));
    if (!path.isEmpty())
        m_viewModel->LoadPolygon(path);
}

void FlightPlannerActionPanel::savePolygon()
{
    if (!m_viewModel || m_viewModel->TransferBusy()) return;
    const QString path = QFileDialog::getSaveFileName(
            this, tr("Save Polygon"), QStringLiteral("polygon.poly"),
            tr("Polygon Files (*.poly);;All Files (*)"));
    if (!path.isEmpty())
        m_viewModel->SavePolygon(path);
}

void FlightPlannerActionPanel::offsetPolygon()
{
    if (!m_viewModel || m_viewModel->TransferBusy()) return;
    m_viewModel->OffsetDrawnPolygon(m_polygonOffset->value());
}

void FlightPlannerActionPanel::openSurveyGrid()
{
    if (!m_viewModel || m_viewModel->TransferBusy()) return;
    const QVector<SurveyGridCoordinate> polygon =
        m_viewModel->SurveyBoundary(m_viewModel->DefaultAltitude());
    if (polygon.size() < 3) {
        m_viewModel->setStatus(
            tr("Draw at least 3 mission points to outline the survey area."));
        return;
    }

    SurveyGridDialog dialog(polygon, this);
    dialog.setAltitudePresentation(
        m_viewModel->AltitudeMultiplier(), m_viewModel->AltUnit());
    SurveyGridCoordinate home = polygon.first();
    if (m_viewModel->HomeValid()) {
        home = {m_viewModel->HomeLat(), m_viewModel->HomeLng(),
                m_viewModel->HomeAlt()};
    }
    dialog.setHomeLocation(home);
    if (dialog.exec() != QDialog::Accepted) return;

    const SurveyMissionPlan plan = SurveyMissionBuilder::Build(
        dialog.gridResult().path, home, dialog.missionOptions());
    m_viewModel->AppendSurveyPlan(plan);
}

void FlightPlannerActionPanel::syncMissionType(const QString &type)
{
    const QSignalBlocker blocker(m_missionTypeCombo);
    const int index = m_missionTypeCombo->findText(type, Qt::MatchFixedString);
    if (index >= 0) {
        m_missionTypeCombo->setCurrentIndex(index);
    }
    syncTransferState();
}

void FlightPlannerActionPanel::syncSaveAvailability()
{
    const bool hasModel = !m_viewModel.isNull();
    const bool idle = hasModel && !m_viewModel->TransferBusy();
    const bool missionStore = hasModel
        && m_viewModel->MissionType() == QStringLiteral("Mission");
    const bool homeValid = hasModel && m_viewModel->HomeValid();
    m_saveButton->setEnabled(idle && (!missionStore || homeValid));
    if (missionStore && !homeValid) {
        m_saveButton->setToolTip(
            tr("Set a valid Home location before saving a WPL mission."));
    } else {
        m_saveButton->setToolTip(QString());
    }
}

void FlightPlannerActionPanel::syncMapType(int type)
{
    const QSignalBlocker blocker(m_mapTypeCombo);
    const int index = m_mapTypeCombo->findData(type);
    if (index >= 0) {
        m_mapTypeCombo->setCurrentIndex(index);
    }
}

void FlightPlannerActionPanel::syncStatus(const QString &status)
{
    m_statusLabel->setText(tr("Status: %1").arg(status));
    m_statusLabel->setToolTip(status);
}

void FlightPlannerActionPanel::syncAltitudePresentation()
{
    if (!m_viewModel || !m_homeAlt) return;
    const double multiplier = m_viewModel->AltitudeMultiplier();
    const QSignalBlocker blocker(m_homeAlt);
    m_homeAlt->setRange(-10000.0 * multiplier, 100000.0 * multiplier);
    m_homeAlt->setSuffix(
        QStringLiteral(" %1").arg(m_viewModel->AltUnit()));
    m_homeAlt->setValue(m_viewModel->HomeAltDisplay());
}

void FlightPlannerActionPanel::syncTransferState()
{
    const bool hasModel = !m_viewModel.isNull();
    const bool busy = hasModel && m_viewModel->TransferBusy();
    const bool canRead = hasModel && m_viewModel->CanReadWaypoints();
    const bool canWrite = hasModel && m_viewModel->CanWriteWaypoints();

    m_readButton->setEnabled(canRead);
    m_writeButton->setEnabled(canWrite);
    m_cancelButton->setEnabled(
            hasModel && m_viewModel->CanCancelTransfer());
    m_transferProgress->setEnabled(hasModel);
    m_transferProgress->setValue(
            hasModel ? m_viewModel->TransferProgress() : 0);

    const QString unavailableTip = busy
            ? tr("Wait for the active mission transfer to finish.")
            : tr("Connect and select a vehicle first.");
    m_readButton->setToolTip(canRead ? QString() : unavailableTip);
    m_writeButton->setToolTip(canWrite ? QString() : unavailableTip);
    m_cancelButton->setToolTip(
            hasModel && m_viewModel->CanCancelTransfer()
                ? tr("Cancel the active mission transfer.")
                : tr("No flight-plan transfer is active."));

    m_missionTypeCombo->setEnabled(hasModel && !busy);
    m_loadButton->setEnabled(hasModel && !busy);
    m_appendButton->setEnabled(hasModel && !busy);
    const bool surveyAvailable = hasModel && !busy
        && m_viewModel->MissionType() == QStringLiteral("Mission")
        && m_viewModel->SurveyBoundary(
                m_viewModel->DefaultAltitude()).size() >= 3;
    m_surveyGridButton->setEnabled(surveyAvailable);
    m_surveyGridButton->setToolTip(surveyAvailable ? QString()
        : tr("Draw a valid polygon or create at least 3 mission waypoints "
             "to define the survey boundary."));
    m_setHomeButton->setEnabled(
            hasModel && !busy && m_viewModel->CanSetHomeFromVehicle());
    m_setHomeButton->setToolTip(
            hasModel && m_viewModel->CanSetHomeFromVehicle()
                ? QString()
                : tr("Connect and select a vehicle first."));
    m_homeLat->setEnabled(hasModel && !busy);
    m_homeLng->setEnabled(hasModel && !busy);
    m_homeAlt->setEnabled(hasModel && !busy);
    syncPolygonState();
    syncSaveAvailability();
}

void FlightPlannerActionPanel::syncPolygonState()
{
    const bool hasModel = !m_viewModel.isNull();
    const bool idle = hasModel && !m_viewModel->TransferBusy();
    const FlightPlannerPolygonModel *polygon = hasModel
            ? m_viewModel->DrawnPolygon() : nullptr;
    const int count = polygon ? polygon->Count() : 0;
    const bool valid = polygon && polygon->IsValid();

    m_polygonDrawButton->setEnabled(idle);
    {
        const QSignalBlocker blocker(m_polygonDrawButton);
        m_polygonDrawButton->setChecked(
                hasModel && m_viewModel->PolygonDrawMode());
    }
    m_polygonClearButton->setEnabled(idle && count > 0);
    m_polygonFromWaypointsButton->setEnabled(idle && hasModel
            && m_viewModel->Waypoints()->storeRowCount(
                    FlightPlannerMissionModel::MissionStore::Mission) >= 3);
    m_polygonLoadButton->setEnabled(idle);
    m_polygonSaveButton->setEnabled(idle && valid);
    m_polygonOffset->setEnabled(idle && valid);
    m_polygonOffsetButton->setEnabled(idle && valid
            && m_polygonOffset->value() != 0.0);
    m_fenceInclusionButton->setEnabled(idle && valid);
    m_fenceExclusionButton->setEnabled(idle && valid);

    if (valid) {
        const double area = polygon->PolygonArea();
        m_polygonAreaLabel->setText(
                tr("%1 vertices — %2 m² (%3 ha)")
                .arg(count)
                .arg(QString::number(area, 'f', 0))
                .arg(QString::number(area / 10000.0, 'f', 2)));
    } else {
        m_polygonAreaLabel->setText(
                tr("%1 of 3 required vertices").arg(count));
    }
}
