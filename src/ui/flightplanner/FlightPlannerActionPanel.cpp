#include "FlightPlannerActionPanel.h"

#include "FlightPlannerViewModel.h"
#include "UASInterface.h"
#include "UASManager.h"
#include "ui/map/MapTileSourceFactory.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
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
    itemsLayout->addWidget(disabledButton(
        tr("Survey (Grid)"), QStringLiteral("SurveyGridButton"),
        tr("Survey grid generation is not implemented yet."), items));
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

    itemsLayout->addWidget(disabledButton(
        tr("Read"), QStringLiteral("ReadWaypointsButton"),
        tr("Typed Mission/Fence/Rally download is not implemented yet."), items));
    itemsLayout->addWidget(disabledButton(
        tr("Write"), QStringLiteral("WriteWaypointsButton"),
        tr("Typed Mission/Fence/Rally upload is not implemented yet."), items));
    itemsLayout->addWidget(disabledButton(
        tr("Write Fast"), QStringLiteral("WriteWaypointsFastButton"),
        tr("Fast MAVFTP mission upload is not implemented yet."), items));
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
        m_homeAlt->setValue(m_viewModel->HomeAlt());

        connect(m_loadButton, &QPushButton::clicked,
                this, [this]() { loadFile(false); });
        connect(m_appendButton, &QPushButton::clicked,
                this, [this]() { loadFile(true); });
        connect(m_saveButton, &QPushButton::clicked,
                this, &FlightPlannerActionPanel::saveFile);
        connect(m_setHomeButton, &QPushButton::clicked,
                this, &FlightPlannerActionPanel::setHomeFromVehicle);
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
                m_viewModel, &FlightPlannerViewModel::setHomeAlt);

        connect(m_viewModel, &FlightPlannerViewModel::missionTypeChanged,
                this, &FlightPlannerActionPanel::syncMissionType);
        connect(m_viewModel, &FlightPlannerViewModel::homeValidChanged,
                this, [this](bool) { syncSaveAvailability(); });
        connect(m_viewModel, &FlightPlannerViewModel::statusChanged,
                this, &FlightPlannerActionPanel::syncStatus);
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
        connect(m_viewModel, &FlightPlannerViewModel::homeAltChanged,
                this, [this](double value) {
                    const QSignalBlocker blocker(m_homeAlt);
                    m_homeAlt->setValue(value);
                });
        connect(m_viewModel, &QObject::destroyed, this, [this]() {
            for (QPushButton *button : {m_loadButton, m_appendButton,
                                        m_saveButton, m_setHomeButton}) {
                button->setEnabled(false);
            }
            m_missionTypeCombo->setEnabled(false);
            m_useMavFtp->setEnabled(false);
            m_homeLat->setEnabled(false);
            m_homeLng->setEnabled(false);
            m_homeAlt->setEnabled(false);
            syncStatus(tr("Flight planner model is unavailable."));
        });
    } else {
        syncStatus(tr("Flight planner model is unavailable."));
        m_homeLat->setEnabled(false);
        m_homeLng->setEnabled(false);
        m_homeAlt->setEnabled(false);
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
    const QString path = QFileDialog::getOpenFileName(
        this, append ? tr("Load and Append Mission") : tr("Load Mission"),
        QString(), tr("Waypoint Files (*.waypoints *.txt);;All Files (*)"));
    if (path.isEmpty()) return;
    if (append) {
        m_viewModel->LoadAndAppend(path);
    } else {
        m_viewModel->LoadFile(path);
    }
}

void FlightPlannerActionPanel::saveFile()
{
    if (!m_viewModel) return;
    const QString suggested = QStringLiteral("%1.waypoints")
        .arg(m_viewModel->MissionType().toLower());
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Mission"), suggested,
        tr("Waypoint Files (*.waypoints *.txt);;All Files (*)"));
    if (!path.isEmpty()) {
        m_viewModel->SaveFile(path);
    }
}

void FlightPlannerActionPanel::setHomeFromVehicle()
{
    if (!m_viewModel) return;
    UASInterface *vehicle = UASManager::instance()->getActiveUAS();
    if (!vehicle) {
        QMessageBox::information(this, tr("Home Location"),
                                 tr("Connect and select a vehicle first."));
        return;
    }
    m_viewModel->SetHomeFromVehicle(vehicle->getLatitude(),
                                    vehicle->getLongitude(),
                                    vehicle->getAltitudeAMSL());
}

void FlightPlannerActionPanel::syncMissionType(const QString &type)
{
    const QSignalBlocker blocker(m_missionTypeCombo);
    const int index = m_missionTypeCombo->findText(type, Qt::MatchFixedString);
    if (index >= 0) {
        m_missionTypeCombo->setCurrentIndex(index);
    }
    syncSaveAvailability();
}

void FlightPlannerActionPanel::syncSaveAvailability()
{
    const bool hasModel = !m_viewModel.isNull();
    const bool missionFileSupported = hasModel
        && m_viewModel->MissionType() == QStringLiteral("Mission");
    const bool homeValid = hasModel && m_viewModel->HomeValid();
    m_saveButton->setEnabled(missionFileSupported && homeValid);
    if (!missionFileSupported) {
        m_saveButton->setToolTip(
            tr("Fence and Rally file codecs are not implemented yet."));
    } else if (!homeValid) {
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
