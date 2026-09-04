#include "ConfigPlannerView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
QComboBox *linearUnitsCombo(QWidget *parent, const QString &objectName)
{
    auto *combo = new QComboBox(parent);
    combo->setObjectName(objectName);
    combo->addItem(QObject::tr("Meters"), QStringLiteral("Meters"));
    combo->addItem(QObject::tr("Feet"), QStringLiteral("Feet"));
    return combo;
}

void setComboData(QComboBox *combo, const QVariant &value)
{
    const QSignalBlocker blocker(combo);
    const int index = combo->findData(value);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
}
}

ConfigPlannerView::ConfigPlannerView(ConfigPlannerViewModel *viewModel,
                                     QWidget *parent)
    : QWidget(parent), m_viewModel(viewModel)
{
    setObjectName(QStringLiteral("ConfigPlannerView"));
    if (!m_viewModel) {
        m_viewModel = ConfigPlannerViewModel::instance();
    }
    setStyleSheet(QStringLiteral(
        "QLabel#plannerSettingsTitle { font-size: 16px; font-weight: bold; }"
        "QGroupBox[plannerSection=\"true\"] { font-weight: bold;"
        " margin-top: 10px; padding-top: 8px; }"
        "QLabel[portingUnavailable=\"true\"] { font-weight: normal; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);
    auto *title = new QLabel(tr("Planner Settings"), this);
    title->setObjectName(QStringLiteral("plannerSettingsTitle"));
    root->addWidget(title);

    QGroupBox *display = addSection(
        root, 0, QStringLiteral("PlannerDisplaySection"));
    auto *displayForm = new QFormLayout(display);
    m_distanceUnits = linearUnitsCombo(
        display, QStringLiteral("CMB_distunits"));
    displayForm->addRow(tr("Dist Units"), m_distanceUnits);
    m_altitudeUnits = linearUnitsCombo(
        display, QStringLiteral("CMB_altunits"));
    displayForm->addRow(tr("Alt Units"), m_altitudeUnits);
    m_layout = new QComboBox(display);
    m_layout->setObjectName(QStringLiteral("CMB_displayview"));
    m_layout->addItem(tr("Basic"),
                      static_cast<int>(DisplayViewPreset::Basic));
    m_layout->addItem(tr("Advanced"),
                      static_cast<int>(DisplayViewPreset::Advanced));
    m_layout->addItem(tr("Custom"),
                      static_cast<int>(DisplayViewPreset::Custom));
    m_layoutLabel = new QLabel(tr("Theme / Layout"), display);
    displayForm->addRow(m_layoutLabel, m_layout);
    m_layoutStatus = new QLabel(display);
    m_layoutStatus->setObjectName(QStringLiteral("DisplayLayoutStatus"));
    m_layoutStatus->setWordWrap(true);
    displayForm->addRow(m_layoutStatus);
    m_hudOverlay = new QCheckBox(tr("Enable HUD Overlay"), display);
    m_hudOverlay->setObjectName(QStringLiteral("CHK_hudshow"));
    displayForm->addRow(tr("HUD / OSD"), m_hudOverlay);
    auto *displayMissing = new QLabel(
        tr("UI language, speed units, OSD color and message severity do not "
           "yet have complete Qt consumers."), display);
    displayMissing->setObjectName(QStringLiteral("DisplayPendingNote"));
    displayMissing->setProperty("portingUnavailable", true);
    displayMissing->setWordWrap(true);
    displayForm->addRow(displayMissing);

    QGroupBox *speech = addSection(
        root, 1, QStringLiteral("PlannerSpeechSection"));
    auto *speechLayout = new QVBoxLayout(speech);
    auto *speechControls = new QWidget(speech);
    auto *speechControlsLayout = new QHBoxLayout(speechControls);
    speechControlsLayout->setContentsMargins(0, 0, 0, 0);
    m_speechEnabled = new QCheckBox(tr("Enable Speech"), speechControls);
    m_speechEnabled->setObjectName(QStringLiteral("CHK_speechenable"));
    speechControlsLayout->addWidget(m_speechEnabled);
    m_speechTest = new QPushButton(tr("Test Speech"), speechControls);
    m_speechTest->setObjectName(QStringLiteral("SpeechTest"));
    speechControlsLayout->addWidget(m_speechTest);
    speechControlsLayout->addStretch(1);
    speechLayout->addWidget(speechControls);
    m_speechBackendStatus = new QLabel(speech);
    m_speechBackendStatus->setObjectName(
        QStringLiteral("SpeechBackendStatus"));
    m_speechBackendStatus->setWordWrap(true);
    speechLayout->addWidget(m_speechBackendStatus);
    m_audioMute = new QCheckBox(
        tr("Mute all audio output (useful legacy Qt control)"), speech);
    m_audioMute->setObjectName(QStringLiteral("CHK_audioMute"));
    speechLayout->addWidget(m_audioMute);
    addUnavailableNote(
        speechLayout,
        tr("Enable Speech controls all current Qt vehicle announcements. "
           "MP10 speech levels, per-event switches/templates, Armed Only and "
           "Vario remain unavailable until their complete consumers are ported."),
        QStringLiteral("SpeechPendingNote"));

    QGroupBox *shortcuts = addSection(
        root, 2, QStringLiteral("PlannerFlightShortcutsSection"));
    auto *shortcutsLayout = new QVBoxLayout(shortcuts);
    addUnavailableNote(
        shortcutsLayout,
        tr("Global Alt-key flight commands are unavailable: the required "
           "exact-target confirmations and safety service are not ported."),
        QStringLiteral("FlightShortcutsPendingNote"));

    QGroupBox *waypoints = addSection(
        root, 3, QStringLiteral("PlannerWaypointsConnectSection"));
    auto *waypointLayout = new QVBoxLayout(waypoints);
    m_heartbeat = new QCheckBox(
        tr("Send GCS heartbeats (useful legacy Qt control)"), waypoints);
    m_heartbeat->setObjectName(QStringLiteral("CHK_GCSheartbeat"));
    waypointLayout->addWidget(m_heartbeat);
    addUnavailableNote(
        waypointLayout,
        tr("MP10 waypoint-on-connect, map rotation, USB reset, ESP32 RTS "
           "reset and no-RC policies do not yet have Qt consumers."),
        QStringLiteral("WaypointsConnectPendingNote"));

    QGroupBox *startup = addSection(
        root, 4, QStringLiteral("PlannerStartupUdpSection"));
    auto *startupForm = new QFormLayout(startup);
    m_startupUdpEnabled = new QCheckBox(
        tr("Open MAVLink UDP listeners when APM Planner starts"), startup);
    m_startupUdpEnabled->setObjectName(
        QStringLiteral("CHK_startup_udp_listeners"));
    startupForm->addRow(m_startupUdpEnabled);
    m_startupUdpPrimary = new QSpinBox(startup);
    m_startupUdpPrimary->setObjectName(
        QStringLiteral("NUM_startup_udp_primary_port"));
    m_startupUdpPrimary->setRange(1, 65535);
    startupForm->addRow(tr("Primary UDP port"), m_startupUdpPrimary);
    m_startupUdpAlternate = new QSpinBox(startup);
    m_startupUdpAlternate->setObjectName(
        QStringLiteral("NUM_startup_udp_alternate_port"));
    m_startupUdpAlternate->setRange(1, 65535);
    startupForm->addRow(tr("Alternate UDP port"), m_startupUdpAlternate);
    m_startupUdpStatus = new QLabel(startup);
    m_startupUdpStatus->setObjectName(
        QStringLiteral("StartupUdpListenerStatus"));
    m_startupUdpStatus->setWordWrap(true);
    startupForm->addRow(m_startupUdpStatus);
    auto *startupNote = new QLabel(
        PlannerStartupUdpOptions::restartNote(), startup);
    startupNote->setObjectName(QStringLiteral("StartupUdpListenerNote"));
    startupNote->setWordWrap(true);
    startupForm->addRow(startupNote);

    QGroupBox *telemetry = addSection(
        root, 5, QStringLiteral("PlannerTelemetryRatesSection"));
    auto *telemetryLayout = new QVBoxLayout(telemetry);
    addUnavailableNote(
        telemetryLayout,
        tr("The five MP10 grouped stream rates and target-safe parameter "
           "refresh are not ported. The different seven-rate APM Planner "
           "editor remains available in Legacy options."),
        QStringLiteral("TelemetryRatesPendingNote"));
    auto *legacyTelemetry = new QPushButton(
        tr("Open Legacy APM Planner options…"), telemetry);
    legacyTelemetry->setObjectName(
        QStringLiteral("OpenLegacyTelemetryOptions"));
    telemetryLayout->addWidget(legacyTelemetry, 0, Qt::AlignLeft);

    QGroupBox *map = addSection(
        root, 6, QStringLiteral("PlannerAircraftMapSection"));
    auto *mapForm = new QFormLayout(map);
    m_mapBackend = new QComboBox(map);
    m_mapBackend->setObjectName(
        QStringLiteral("MapWidgetBackendComboBox"));
    mapForm->addRow(tr("Map renderer (useful Qt extension)"), m_mapBackend);
    m_mapBackendStatus = new QLabel(map);
    m_mapBackendStatus->setObjectName(
        QStringLiteral("MapWidgetBackendStatus"));
    m_mapBackendStatus->setWordWrap(true);
    mapForm->addRow(m_mapBackendStatus);
    auto *mapMissing = new QLabel(
        tr("MP10 aircraft vectors, tooltips, overlays, cache mode and "
           "external ADS-B controls remain unavailable."), map);
    mapMissing->setObjectName(QStringLiteral("AircraftMapPendingNote"));
    mapMissing->setProperty("portingUnavailable", true);
    mapMissing->setWordWrap(true);
    mapForm->addRow(mapMissing);

    QGroupBox *logs = addSection(
        root, 7, QStringLiteral("PlannerLogsSection"));
    auto *logsForm = new QFormLayout(logs);
    m_mavlinkLogging = new QCheckBox(
        tr("Enable MAVLink telemetry logging"), logs);
    m_mavlinkLogging->setObjectName(
        QStringLiteral("CHK_mavlink_logging"));
    logsForm->addRow(m_mavlinkLogging);
    auto directoryRow = [logs](QLineEdit **edit, QPushButton **button,
                               const QString &editName,
                               const QString &buttonName) {
        auto *row = new QWidget(logs);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        *edit = new QLineEdit(row);
        (*edit)->setObjectName(editName);
        (*edit)->setReadOnly(true);
        *button = new QPushButton(QObject::tr("Browse…"), row);
        (*button)->setObjectName(buttonName);
        layout->addWidget(*edit, 1);
        layout->addWidget(*button);
        return row;
    };
    QPushButton *dataFlashBrowse = nullptr;
    logsForm->addRow(
        tr("DataFlash log directory"),
        directoryRow(&m_dataFlashLogDirectory, &dataFlashBrowse,
                     QStringLiteral("DataFlashLogDirectory"),
                     QStringLiteral("BrowseDataFlashLogDirectory")));
    QPushButton *tlogBrowse = nullptr;
    logsForm->addRow(
        tr("Tlog directory"),
        directoryRow(&m_tlogDirectory, &tlogBrowse,
                     QStringLiteral("TlogDirectory"),
                     QStringLiteral("BrowseTlogDirectory")));

    QGroupBox *advanced = addSection(
        root, 8, QStringLiteral("PlannerAdvancedSection"));
    auto *advancedLayout = new QVBoxLayout(advanced);
    m_betaUpdates = new QCheckBox(
        tr("Use beta update channel (useful legacy Qt control)"), advanced);
    m_betaUpdates->setObjectName(QStringLiteral("CHK_beta_updates"));
    advancedLayout->addWidget(m_betaUpdates);
    m_betaStatus = new QLabel(advanced);
    m_betaStatus->setObjectName(QStringLiteral("BetaUpdateStatus"));
    m_betaStatus->setProperty("portingUnavailable", true);
    m_betaStatus->setWordWrap(true);
    advancedLayout->addWidget(m_betaStatus);
    m_autoProxy = new QCheckBox(
        tr("Use system network proxy (useful legacy Qt control)"), advanced);
    m_autoProxy->setObjectName(QStringLiteral("CHK_auto_proxy"));
    advancedLayout->addWidget(m_autoProxy);
    addUnavailableNote(
        advancedLayout,
        tr("MP10 password protection, auto parameter commit, background "
           "loads and slow-machine policies are not enforced by Qt yet."),
        QStringLiteral("AdvancedPendingNote"));
    auto *legacyOptions = new QPushButton(
        tr("Open Legacy APM Planner options…"), advanced);
    legacyOptions->setObjectName(QStringLiteral("OpenLegacyPlannerOptions"));
    advancedLayout->addWidget(legacyOptions, 0, Qt::AlignLeft);

    root->addStretch(1);

    connect(m_viewModel, &ConfigPlannerViewModel::stateChanged,
            this, &ConfigPlannerView::syncFromModel);
    connect(m_viewModel, &QObject::destroyed, this, [this]() {
        setEnabled(false);
    });
    connect(m_viewModel, &ConfigPlannerViewModel::altitudeUnitsChanged,
            this, &ConfigPlannerView::altitudeUnitsChanged);
    connect(m_viewModel, &ConfigPlannerViewModel::distanceUnitsChanged,
            this, &ConfigPlannerView::distanceUnitsChanged);
    connect(m_distanceUnits,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (m_viewModel && index >= 0) {
            m_viewModel->setDistanceUnits(
                m_distanceUnits->itemData(index).toString());
        }
    });
    connect(m_altitudeUnits,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (m_viewModel && index >= 0) {
            m_viewModel->setAltitudeUnits(
                m_altitudeUnits->itemData(index).toString());
        }
    });
    connect(m_layout, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (!m_viewModel || index < 0) return;
        if (!m_viewModel->setDisplayPreset(static_cast<DisplayViewPreset>(
                m_layout->itemData(index).toInt()))) {
            syncFromModel();
            m_layoutStatus->setText(m_viewModel->lastError());
        }
    });
    connect(m_hudOverlay, &QCheckBox::toggled,
            this, [this](bool enabled) {
        if (!m_viewModel) return;
        if (!m_viewModel->setHudOverlayEnabled(enabled)) {
            syncFromModel();
        }
    });
    connect(m_speechEnabled, &QCheckBox::toggled,
            this, [this](bool enabled) {
        if (!m_viewModel) return;
        if (!m_viewModel->setSpeechEnabled(enabled)) {
            syncFromModel();
            m_speechBackendStatus->setText(m_viewModel->lastError());
        }
    });
    connect(m_speechTest, &QPushButton::clicked,
            this, &ConfigPlannerView::speechTestRequested);
    connect(m_startupUdpEnabled, &QCheckBox::toggled,
            this, &ConfigPlannerView::saveStartupUdpOptions);
    connect(m_startupUdpPrimary,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ConfigPlannerView::saveStartupUdpOptions);
    connect(m_startupUdpAlternate,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ConfigPlannerView::saveStartupUdpOptions);
    connect(m_betaUpdates, &QCheckBox::toggled,
            this, [this](bool enabled) {
        if (!m_viewModel) return;
        if (!m_viewModel->setBetaUpdatesEnabled(enabled)) {
            const QString error = m_viewModel->lastError();
            syncFromModel();
            m_betaStatus->setText(error);
            return;
        }
        m_betaStatus->clear();
    });
    connect(m_audioMute, &QCheckBox::toggled,
            this, &ConfigPlannerView::audioMuteChanged);
    connect(m_heartbeat, &QCheckBox::toggled,
            this, &ConfigPlannerView::heartbeatChanged);
    connect(m_mavlinkLogging, &QCheckBox::toggled,
            this, &ConfigPlannerView::mavlinkLoggingChanged);
    connect(m_autoProxy, &QCheckBox::toggled,
            this, &ConfigPlannerView::autoProxyChanged);
    connect(m_mapBackend,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index >= 0) {
            emit mapBackendRequested(
                m_mapBackend->itemData(index).toString());
        }
    });
    connect(dataFlashBrowse, &QPushButton::clicked,
            this, &ConfigPlannerView::chooseDataFlashLogDirectory);
    connect(tlogBrowse, &QPushButton::clicked,
            this, &ConfigPlannerView::chooseTlogDirectory);
    connect(legacyTelemetry, &QPushButton::clicked,
            this, &ConfigPlannerView::legacyTelemetryOptionsRequested);
    connect(legacyOptions, &QPushButton::clicked,
            this, &ConfigPlannerView::legacyOptionsRequested);

    syncFromModel();
}

QGroupBox *ConfigPlannerView::addSection(QVBoxLayout *layout, int index,
                                         const QString &objectName)
{
    const QStringList titles = ConfigPlannerViewModel::sectionTitles();
    auto *section = new QGroupBox(titles.value(index), this);
    section->setObjectName(objectName);
    section->setProperty("plannerSection", true);
    section->setProperty("sectionIndex", index);
    layout->addWidget(section);
    return section;
}

QLabel *ConfigPlannerView::addUnavailableNote(
    QVBoxLayout *layout, const QString &text, const QString &objectName)
{
    auto *note = new QLabel(text, layout->parentWidget());
    note->setObjectName(objectName);
    note->setProperty("portingUnavailable", true);
    note->setWordWrap(true);
    layout->addWidget(note);
    return note;
}

void ConfigPlannerView::syncFromModel()
{
    if (!m_viewModel) return;
    setComboData(m_altitudeUnits, m_viewModel->altitudeUnits());
    setComboData(m_distanceUnits, m_viewModel->distanceUnits());
    const DisplayViewProfile profile = m_viewModel->displayProfile();
    setComboData(m_layout, static_cast<int>(profile.preset()));
    const bool layoutVisible = profile.displayPlannerLayout();
    m_layoutLabel->setVisible(layoutVisible);
    m_layout->setVisible(layoutVisible);
    m_layoutStatus->setVisible(layoutVisible);
    m_layoutStatus->setText(tr(
        "%1 layout is active. CONFIG and SETUP use this shared profile.")
                                    .arg(profile.presetName()));

    const PlannerStartupUdpOptions startup =
        m_viewModel->startupUdpOptions();
    {
        const QSignalBlocker enabledBlocker(m_startupUdpEnabled);
        const QSignalBlocker primaryBlocker(m_startupUdpPrimary);
        const QSignalBlocker alternateBlocker(m_startupUdpAlternate);
        m_startupUdpEnabled->setChecked(startup.enabled);
        m_startupUdpPrimary->setValue(startup.primaryPort);
        m_startupUdpAlternate->setValue(startup.alternatePort);
    }
    m_startupUdpPrimary->setEnabled(startup.enabled);
    m_startupUdpAlternate->setEnabled(startup.enabled);
    m_startupUdpStatus->setText(startup.configurationStatus());
    {
        const QSignalBlocker blocker(m_betaUpdates);
        m_betaUpdates->setChecked(m_viewModel->betaUpdatesEnabled());
    }
    {
        const QSignalBlocker blocker(m_hudOverlay);
        m_hudOverlay->setChecked(m_viewModel->hudOverlayEnabled());
    }
    {
        const QSignalBlocker blocker(m_speechEnabled);
        m_speechEnabled->setChecked(m_viewModel->speechEnabled());
    }
}

void ConfigPlannerView::saveStartupUdpOptions()
{
    if (!m_viewModel) return;
    PlannerStartupUdpOptions options;
    options.enabled = m_startupUdpEnabled->isChecked();
    options.primaryPort = m_startupUdpPrimary->value();
    options.alternatePort = m_startupUdpAlternate->value();
    if (!m_viewModel->setStartupUdpOptions(options)) {
        m_startupUdpStatus->setText(m_viewModel->lastError());
        return;
    }
    m_startupUdpPrimary->setEnabled(options.enabled);
    m_startupUdpAlternate->setEnabled(options.enabled);
}

void ConfigPlannerView::reloadSettings()
{
    if (m_viewModel) {
        m_viewModel->reload();
    }
}

void ConfigPlannerView::setMapBackends(
    const QList<ConfigPlannerMapBackend> &backends,
    const QString &selectedId, const QString &status)
{
    const QSignalBlocker blocker(m_mapBackend);
    m_mapBackend->clear();
    for (const ConfigPlannerMapBackend &backend : backends) {
        m_mapBackend->addItem(backend.displayName, backend.id);
    }
    const int selected = m_mapBackend->findData(selectedId);
    if (selected >= 0) {
        m_mapBackend->setCurrentIndex(selected);
    }
    m_mapBackend->setEnabled(m_mapBackend->count() > 1);
    setMapBackendStatus(status);
}

void ConfigPlannerView::setMapBackendStatus(const QString &status)
{
    m_mapBackendStatus->setText(status);
}

void ConfigPlannerView::setAudioMuted(bool muted)
{
    const QSignalBlocker blocker(m_audioMute);
    m_audioMute->setChecked(muted);
}

void ConfigPlannerView::setHeartbeatEnabled(bool enabled)
{
    const QSignalBlocker blocker(m_heartbeat);
    m_heartbeat->setChecked(enabled);
}

void ConfigPlannerView::setMavlinkLoggingEnabled(bool enabled)
{
    const QSignalBlocker blocker(m_mavlinkLogging);
    m_mavlinkLogging->setChecked(enabled);
}

void ConfigPlannerView::setAutoProxyEnabled(bool enabled)
{
    const QSignalBlocker blocker(m_autoProxy);
    m_autoProxy->setChecked(enabled);
}

void ConfigPlannerView::setLogDirectories(
    const QString &dataFlashDirectory, const QString &tlogDirectory)
{
    m_dataFlashLogDirectory->setText(dataFlashDirectory);
    m_tlogDirectory->setText(tlogDirectory);
}

void ConfigPlannerView::setSpeechBackendStatus(const QString &status)
{
    m_speechBackendStatus->setText(status);
}

void ConfigPlannerView::chooseDataFlashLogDirectory()
{
    const QString selected = QFileDialog::getExistingDirectory(
        this, tr("Set DataFlash log directory"),
        m_dataFlashLogDirectory->text());
    if (!selected.isEmpty()) {
        emit dataFlashLogDirectorySelected(selected);
    }
}

void ConfigPlannerView::chooseTlogDirectory()
{
    const QString selected = QFileDialog::getExistingDirectory(
        this, tr("Set tlog directory"), m_tlogDirectory->text());
    if (!selected.isEmpty()) {
        emit tlogDirectorySelected(selected);
    }
}
