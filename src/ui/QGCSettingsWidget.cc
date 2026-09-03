#include <QSettings>

#include "QGCSettingsWidget.h"
#include "MainWindow.h"
#include "ui_QGCSettingsWidget.h"

#include "LinkManager.h"
#include "GAudioOutput.h"
#include "ArduPilotMegaMAV.h"
#include "UASManager.h"
#include "map/CompiledMapBackends.h"
#include "map/MapWidgetFactory.h"
#include "configuration/DisplayViewProfile.h"
#include "configuration/PlannerStartupUdpOptions.h"

#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>

namespace {
int findCaseInsensitiveData(const QComboBox *combo, const QString &value)
{
    if (!combo) {
        return -1;
    }
    for (int index = 0; index < combo->count(); ++index) {
        if (combo->itemData(index).toString().compare(
                value.trimmed(), Qt::CaseInsensitive) == 0) {
            return index;
        }
    }
    return -1;
}
}

QGCSettingsWidget::QGCSettingsWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::QGCSettingsWidget)
{
    ui->setupUi(this);

    RegisterCompiledMapBackends();
    auto *mapGroup = new QGroupBox(tr("Aircraft Icon / Map"), ui->general);
    mapGroup->setObjectName(QStringLiteral("MapSettingsGroup"));
    auto *mapLayout = new QFormLayout(mapGroup);
    m_mapWidgetBackendComboBox = new QComboBox(mapGroup);
    m_mapWidgetBackendComboBox->setObjectName(
        QStringLiteral("MapWidgetBackendComboBox"));
    mapLayout->addRow(tr("Map Widget"), m_mapWidgetBackendComboBox);
    auto *mapNote = new QLabel(
        tr("Map widgets use the same provider and tile cache. "
           "Changes take effect after restart."), mapGroup);
    mapNote->setObjectName(QStringLiteral("MapWidgetBackendNote"));
    mapNote->setWordWrap(true);
    mapLayout->addRow(mapNote);
    m_mapWidgetBackendStatus = new QLabel(mapGroup);
    m_mapWidgetBackendStatus->setObjectName(
        QStringLiteral("MapWidgetBackendStatus"));
    m_mapWidgetBackendStatus->setWordWrap(true);
    mapLayout->addRow(m_mapWidgetBackendStatus);
    ui->gridLayout_3->removeItem(ui->verticalSpacer_2);
    ui->gridLayout_3->removeItem(ui->verticalSpacer);
    delete ui->verticalSpacer_2;
    delete ui->verticalSpacer;
    ui->verticalSpacer_2 = nullptr;
    ui->verticalSpacer = nullptr;
    ui->gridLayout_3->addWidget(mapGroup, 4, 0, 1, 3);

    auto *unitsGroup = new QGroupBox(tr("Units"), ui->general);
    unitsGroup->setObjectName(QStringLiteral("PlannerUnitsGroup"));
    auto *unitsLayout = new QFormLayout(unitsGroup);
    m_altitudeUnitsComboBox = new QComboBox(unitsGroup);
    m_altitudeUnitsComboBox->setObjectName(QStringLiteral("CMB_altunits"));
    m_altitudeUnitsComboBox->addItem(tr("Meters"),
                                     QStringLiteral("Meters"));
    m_altitudeUnitsComboBox->addItem(tr("Feet"), QStringLiteral("Feet"));
    const QString configuredAltitudeUnits = QSettings().value(
        QStringLiteral("altunits"), QStringLiteral("Meters")).toString();
    int altitudeUnitsIndex = findCaseInsensitiveData(
        m_altitudeUnitsComboBox, configuredAltitudeUnits);
    if (altitudeUnitsIndex < 0) altitudeUnitsIndex = 0;
    m_altitudeUnitsComboBox->setCurrentIndex(altitudeUnitsIndex);
    unitsLayout->addRow(tr("Alt Units"), m_altitudeUnitsComboBox);
    m_distanceUnitsComboBox = new QComboBox(unitsGroup);
    m_distanceUnitsComboBox->setObjectName(QStringLiteral("CMB_distunits"));
    m_distanceUnitsComboBox->addItem(tr("Meters"),
                                     QStringLiteral("Meters"));
    m_distanceUnitsComboBox->addItem(tr("Feet"), QStringLiteral("Feet"));
    const QString configuredDistanceUnits = QSettings().value(
        QStringLiteral("distunits"), QStringLiteral("Meters")).toString();
    int distanceUnitsIndex = findCaseInsensitiveData(
        m_distanceUnitsComboBox, configuredDistanceUnits);
    if (distanceUnitsIndex < 0) distanceUnitsIndex = 0;
    m_distanceUnitsComboBox->setCurrentIndex(distanceUnitsIndex);
    unitsLayout->addRow(tr("Dist Units"), m_distanceUnitsComboBox);
    m_displayLayoutComboBox = new QComboBox(unitsGroup);
    m_displayLayoutComboBox->setObjectName(
        QStringLiteral("CMB_displayview"));
    m_displayLayoutComboBox->addItem(
        tr("Basic"), static_cast<int>(DisplayViewPreset::Basic));
    m_displayLayoutComboBox->addItem(
        tr("Advanced"), static_cast<int>(DisplayViewPreset::Advanced));
    m_displayLayoutComboBox->addItem(
        tr("Custom"), static_cast<int>(DisplayViewPreset::Custom));
    m_displayLayoutLabel = new QLabel(tr("Layout"), unitsGroup);
    unitsLayout->addRow(m_displayLayoutLabel, m_displayLayoutComboBox);
    m_displayLayoutStatus = new QLabel(unitsGroup);
    m_displayLayoutStatus->setObjectName(
        QStringLiteral("DisplayLayoutStatus"));
    m_displayLayoutStatus->setWordWrap(true);
    unitsLayout->addRow(m_displayLayoutStatus);
    ui->gridLayout_3->addWidget(unitsGroup, 5, 0, 1, 3);
    auto *udpGroup = new QGroupBox(tr("Startup UDP Listeners"), ui->general);
    udpGroup->setObjectName(QStringLiteral("StartupUdpListenersGroup"));
    auto *udpLayout = new QFormLayout(udpGroup);
    QSettings startupSettings;
    const PlannerStartupUdpOptions startup =
        PlannerStartupUdpOptions::load(startupSettings);
    m_startupUdpEnabled = new QCheckBox(
        tr("Open MAVLink UDP listeners when APM Planner starts"), udpGroup);
    m_startupUdpEnabled->setObjectName(
        QStringLiteral("CHK_startup_udp_listeners"));
    m_startupUdpEnabled->setChecked(startup.enabled);
    udpLayout->addRow(m_startupUdpEnabled);
    m_startupUdpPrimaryPort = new QSpinBox(udpGroup);
    m_startupUdpPrimaryPort->setObjectName(
        QStringLiteral("NUM_startup_udp_primary_port"));
    m_startupUdpPrimaryPort->setRange(1, 65535);
    m_startupUdpPrimaryPort->setValue(startup.primaryPort);
    m_startupUdpPrimaryPort->setEnabled(startup.enabled);
    udpLayout->addRow(tr("Primary port"), m_startupUdpPrimaryPort);
    m_startupUdpAlternatePort = new QSpinBox(udpGroup);
    m_startupUdpAlternatePort->setObjectName(
        QStringLiteral("NUM_startup_udp_alternate_port"));
    m_startupUdpAlternatePort->setRange(1, 65535);
    m_startupUdpAlternatePort->setValue(startup.alternatePort);
    m_startupUdpAlternatePort->setEnabled(startup.enabled);
    udpLayout->addRow(tr("Alternate port"), m_startupUdpAlternatePort);
    m_startupUdpStatus = new QLabel(startup.configurationStatus(), udpGroup);
    m_startupUdpStatus->setObjectName(
        QStringLiteral("StartupUdpListenerStatus"));
    m_startupUdpStatus->setWordWrap(true);
    udpLayout->addRow(m_startupUdpStatus);
    auto *udpNote = new QLabel(
        PlannerStartupUdpOptions::restartNote(), udpGroup);
    udpNote->setObjectName(QStringLiteral("StartupUdpListenerNote"));
    udpNote->setWordWrap(true);
    udpLayout->addRow(udpNote);
    ui->gridLayout_3->addWidget(udpGroup, 6, 0, 1, 3);
    ui->gridLayout_3->setRowStretch(7, 1);

    populateMapWidgetBackends();
    connect(m_mapWidgetBackendComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &QGCSettingsWidget::mapWidgetBackendChanged);
    connect(m_altitudeUnitsComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &QGCSettingsWidget::altitudeUnitsIndexChanged);
    connect(m_distanceUnitsComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &QGCSettingsWidget::distanceUnitsIndexChanged);
    connect(m_displayLayoutComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &QGCSettingsWidget::displayLayoutIndexChanged);
    connect(m_startupUdpEnabled, &QCheckBox::toggled,
            this, &QGCSettingsWidget::saveStartupUdpOptions);
    connect(m_startupUdpPrimaryPort,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this, &QGCSettingsWidget::saveStartupUdpOptions);
    connect(m_startupUdpAlternatePort,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this, &QGCSettingsWidget::saveStartupUdpOptions);
    connect(MapWidgetFactory::instance(),
            &MapWidgetFactory::AvailableBackendsChanged,
            this, &QGCSettingsWidget::populateMapWidgetBackends);
    connect(MapWidgetFactory::instance(),
            &MapWidgetFactory::BackendChanged,
            this, [this](const QString &) {
                populateMapWidgetBackends();
            });
    connect(MapWidgetFactory::instance(),
            &MapWidgetFactory::StatusMessage,
            m_mapWidgetBackendStatus, &QLabel::setText);
    connect(DisplayViewProfileService::instance(),
            &DisplayViewProfileService::changed,
            this, &QGCSettingsWidget::syncDisplayLayout);
    syncDisplayLayout();

    // Add all protocols
    /*QList<ProtocolInterface*> protocols = LinkManager::instance()->getProtocols();
    foreach (ProtocolInterface* protocol, protocols) {
        MAVLinkProtocol* mavlink = dynamic_cast<MAVLinkProtocol*>(protocol);
        if (mavlink) {
            MAVLinkSettingsWidget* msettings = new MAVLinkSettingsWidget(mavlink, this);
            ui->tabWidget->addTab(msettings, "MAVLink");
        }
    }*/

}

void QGCSettingsWidget::altitudeUnitsIndexChanged(int index)
{
    if (!m_altitudeUnitsComboBox || index < 0) return;
    const QString units = m_altitudeUnitsComboBox->itemData(index).toString();
    if (units != QStringLiteral("Meters")
        && units != QStringLiteral("Feet")) {
        return;
    }
    QSettings().setValue(QStringLiteral("altunits"), units);
    emit altitudeUnitsChanged(units);
    MainWindow::instance()->setPlannerAltitudeUnits(units);
}

void QGCSettingsWidget::distanceUnitsIndexChanged(int index)
{
    if (!m_distanceUnitsComboBox || index < 0) return;
    const QString units = m_distanceUnitsComboBox->itemData(index).toString();
    if (units != QStringLiteral("Meters")
        && units != QStringLiteral("Feet")) {
        return;
    }
    QSettings().setValue(QStringLiteral("distunits"), units);
    emit distanceUnitsChanged(units);
    MainWindow::instance()->setPlannerDistanceUnits(units);
}

void QGCSettingsWidget::displayLayoutIndexChanged(int index)
{
    if (!m_displayLayoutComboBox || index < 0) {
        return;
    }
    const int rawPreset = m_displayLayoutComboBox->itemData(index).toInt();
    if (rawPreset < static_cast<int>(DisplayViewPreset::Basic)
        || rawPreset > static_cast<int>(DisplayViewPreset::Custom)) {
        syncDisplayLayout();
        return;
    }
    QString error;
    if (!DisplayViewProfileService::instance()->applyPreset(
            static_cast<DisplayViewPreset>(rawPreset), &error)) {
        syncDisplayLayout();
        m_displayLayoutStatus->setText(error);
    }
}

void QGCSettingsWidget::syncDisplayLayout()
{
    if (!m_displayLayoutComboBox || !m_displayLayoutStatus) {
        return;
    }
    const DisplayViewProfile profile =
        DisplayViewProfileService::instance()->current();
    const QSignalBlocker blocker(m_displayLayoutComboBox);
    const int index = m_displayLayoutComboBox->findData(
        static_cast<int>(profile.preset()));
    if (index >= 0) {
        m_displayLayoutComboBox->setCurrentIndex(index);
    }
    m_displayLayoutComboBox->setVisible(profile.displayPlannerLayout());
    m_displayLayoutLabel->setVisible(profile.displayPlannerLayout());
    m_displayLayoutStatus->setVisible(profile.displayPlannerLayout());
    m_displayLayoutStatus->setText(tr(
        "%1 layout is active. CONFIG and SETUP visibility now follows "
        "the shared Mission Planner profile.").arg(profile.presetName()));
}

void QGCSettingsWidget::saveStartupUdpOptions()
{
    if (!m_startupUdpEnabled || !m_startupUdpPrimaryPort
        || !m_startupUdpAlternatePort || !m_startupUdpStatus) {
        return;
    }
    PlannerStartupUdpOptions options;
    options.enabled = m_startupUdpEnabled->isChecked();
    options.primaryPort = m_startupUdpPrimaryPort->value();
    options.alternatePort = m_startupUdpAlternatePort->value();
    QSettings settings;
    options.save(settings);
    settings.sync();
    m_startupUdpPrimaryPort->setEnabled(options.enabled);
    m_startupUdpAlternatePort->setEnabled(options.enabled);
    if (settings.status() == QSettings::NoError) {
        m_startupUdpStatus->setText(options.configurationStatus());
    } else {
        m_startupUdpStatus->setText(tr(
            "Could not save the Startup UDP listener configuration."));
    }
}

void QGCSettingsWidget::populateMapWidgetBackends()
{
    if (!m_mapWidgetBackendComboBox) {
        return;
    }
    const QSignalBlocker blocker(m_mapWidgetBackendComboBox);
    m_mapWidgetBackendComboBox->clear();
    MapWidgetFactory *factory = MapWidgetFactory::instance();
    for (const MapWidgetBackendInfo &backend
         : factory->AvailableBackends()) {
        m_mapWidgetBackendComboBox->addItem(
            backend.displayName, backend.id);
    }
    int current = m_mapWidgetBackendComboBox->findData(
        factory->RequestedBackend());
    if (current < 0) {
        current = m_mapWidgetBackendComboBox->findData(
            factory->CurrentBackend());
    }
    if (current >= 0) {
        m_mapWidgetBackendComboBox->setCurrentIndex(current);
    }
    m_mapWidgetBackendComboBox->setEnabled(
        m_mapWidgetBackendComboBox->count() > 1);
    m_mapWidgetBackendComboBox->setToolTip(
        m_mapWidgetBackendComboBox->count() > 1
            ? tr("Select the map renderer used by DATA, PLAN and SIMULATION.")
            : tr("Only one map renderer is compiled into this build."));
    if (!factory->LastStatus().isEmpty()) {
        m_mapWidgetBackendStatus->setText(factory->LastStatus());
    } else {
        m_mapWidgetBackendStatus->clear();
    }
}

void QGCSettingsWidget::mapWidgetBackendChanged(int index)
{
    if (index < 0 || !m_mapWidgetBackendComboBox) {
        return;
    }
    MapWidgetFactory *factory = MapWidgetFactory::instance();
    const QString backendId =
        m_mapWidgetBackendComboBox->itemData(index).toString();
    if (!factory->SetBackend(backendId)) {
        populateMapWidgetBackends();
        return;
    }
    m_mapWidgetBackendStatus->setText(
        tr("Map widget backend will change to %1 after restart.")
            .arg(m_mapWidgetBackendComboBox->itemText(index)));
}

void QGCSettingsWidget::showEvent(QShowEvent *evt)
{
    Q_UNUSED(evt)

    if (!m_init)
    {
        m_init = true;
        // Audio preferences
        ui->audioMuteCheckBox->setChecked(GAudioOutput::instance()->isMuted());
        connect(ui->audioMuteCheckBox, SIGNAL(toggled(bool)), GAudioOutput::instance(), SLOT(mute(bool)));
        connect(GAudioOutput::instance(), SIGNAL(mutedChanged(bool)), ui->audioMuteCheckBox, SLOT(setChecked(bool)));

        // Reconnect
        ui->reconnectCheckBox->setChecked(MainWindow::instance()->autoReconnectEnabled());
        connect(ui->reconnectCheckBox, SIGNAL(clicked(bool)), MainWindow::instance(), SLOT(enableAutoReconnect(bool)));

        // Low power mode
        ui->lowPowerCheckBox->setChecked(MainWindow::instance()->lowPowerModeEnabled());
        connect(ui->lowPowerCheckBox, SIGNAL(clicked(bool)), MainWindow::instance(), SLOT(enableLowPowerMode(bool)));

        // Automatic use of system Proxies
        ui->autoProxyCheckBox->setChecked(MainWindow::instance()->autoProxyModeEnabled());
        connect(ui->autoProxyCheckBox, SIGNAL(clicked(bool)), MainWindow::instance(), SLOT(enableAutoProxyMode(bool)));
        connect(MainWindow::instance(), SIGNAL(autoProxyChanged(bool)), ui->autoProxyCheckBox, SLOT(setChecked(bool)));

        //Dock widget title bars
        ui->titleBarCheckBox->setChecked(MainWindow::instance()->dockWidgetTitleBarsEnabled());
        connect(ui->titleBarCheckBox,SIGNAL(clicked(bool)),MainWindow::instance(),SLOT(enableDockWidgetTitleBars(bool)));

        ui->heartbeatCheckBox->setChecked(MainWindow::instance()->heartbeatEnabled());
        connect(ui->heartbeatCheckBox,SIGNAL(clicked(bool)),MainWindow::instance(),SLOT(enableHeartbeat(bool)));

        ui->mavlinkLoggingCheckBox->setChecked(LinkManager::instance()->loggingEnabled());
        connect(ui->mavlinkLoggingCheckBox,SIGNAL(clicked(bool)),LinkManager::instance(),SLOT(enableLogging(bool)));

        ui->logDirEdit->setText(QGC::logDirectory());

        ui->appDataDirEdit->setText((QGC::appDataDirectory()));
        ui->paramDirEdit->setText(QGC::parameterDirectory());
        ui->mavlinkLogDirEdit->setText((QGC::MAVLinkLogDirectory()));
        ui->missionsDirEdit->setText((QGC::missionDirectory()));

        connect(ui->logDirSetButton, SIGNAL(clicked()), this, SLOT(setLogDir()));
        connect(ui->appDirSetButton, SIGNAL(clicked()), this, SLOT(setAppDataDir()));
        connect(ui->paramDirSetButton, SIGNAL(clicked()), this, SLOT(setParamDir()));
        connect(ui->mavlinkDirSetButton, SIGNAL(clicked()), this, SLOT(setMAVLinkLogDir()));
        connect(ui->missionsSetButton, SIGNAL(clicked()), this, SLOT(setMissionsDir()));

        // Style
        MainWindow::QGC_MAINWINDOW_STYLE style = (MainWindow::QGC_MAINWINDOW_STYLE)MainWindow::instance()->getStyle();
        switch (style) {
        case MainWindow::QGC_MAINWINDOW_STYLE_NATIVE:
            ui->nativeStyle->setChecked(true);
            break;
        case MainWindow::QGC_MAINWINDOW_STYLE_INDOOR:
            ui->indoorStyle->setChecked(true);
            break;
        case MainWindow::QGC_MAINWINDOW_STYLE_OUTDOOR:
            ui->outdoorStyle->setChecked(true);
            break;
        }
        connect(ui->nativeStyle, SIGNAL(clicked()), MainWindow::instance(), SLOT(loadNativeStyle()));
        connect(ui->indoorStyle, SIGNAL(clicked()), MainWindow::instance(), SLOT(loadIndoorStyle()));
        connect(ui->outdoorStyle, SIGNAL(clicked()), MainWindow::instance(), SLOT(loadOutdoorStyle()));

        connect(ui->extra1LineEdit, SIGNAL(editingFinished()), this, SLOT(ratesChanged()));
        connect(ui->extra2LineEdit, SIGNAL(editingFinished()), this, SLOT(ratesChanged()));
        connect(ui->extra3LineEdit, SIGNAL(editingFinished()), this, SLOT(ratesChanged()));
        connect(ui->positionLineEdit, SIGNAL(editingFinished()), this, SLOT(ratesChanged()));
        connect(ui->extStatusLineEdit, SIGNAL(editingFinished()), this, SLOT(ratesChanged()));
        connect(ui->rcChannelDataLineEdit, SIGNAL(editingFinished()), this, SLOT(ratesChanged()));
        connect(ui->rawSensorLineEdit, SIGNAL(editingFinished()), this, SLOT(ratesChanged()));

        ui->MavlinkspinBox->setValue(QGC::MavlinkID());
        connect(ui->MavlinkspinBox, SIGNAL(valueChanged(int)), this, SLOT(mavIdChanged(int)));

        ui->ComponentspinBox->setValue(QGC::ComponentID());
        connect(ui->ComponentspinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &QGCSettingsWidget::componentIdChanged);
        // Ahgrl sowas muss auch für componentID

        connect(UASManager::instance(),SIGNAL(activeUASSet(UASInterface*)),this,SLOT(setActiveUAS(UASInterface*)));
        setActiveUAS(UASManager::instance()->getActiveUAS());

        setDataRateLineEdits();

        QSettings settings;
        settings.beginGroup("AUTO_UPDATE");
        if (settings.value("RELEASE_TYPE", "stable").toString().trimmed()
                .compare(QStringLiteral("beta"), Qt::CaseInsensitive) == 0) {
            ui->enableBetaReleaseCheckBox->setChecked(true);
        }
        settings.endGroup();
        connect(ui->enableBetaReleaseCheckBox, SIGNAL(clicked(bool)), this, SLOT(setBetaRelease(bool)));

        ui->hideDonateButtonCheckBox->setChecked(settings.value("USER_DONATED", false).toBool());
        connect(ui->hideDonateButtonCheckBox, SIGNAL(clicked(bool)), this, SLOT(setHideDonateButton(bool)));
    }
}

QGCSettingsWidget::~QGCSettingsWidget()
{
    delete ui;
}

void QGCSettingsWidget::setLogDir()
{
    QFileDialog dlg(this, "Set log output directory");
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setDirectory(QGC::logDirectory());

    if(dlg.exec() == QDialog::Accepted) {
        QDir dir = dlg.directory();
        QString name = dir.absolutePath();
        QGC::setLogDirectory(name);
        ui->logDirEdit->setText(name);
    }
}

void QGCSettingsWidget::setMAVLinkLogDir()
{
    QFileDialog dlg(this, "Set tlog output directory");
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setDirectory(QGC::MAVLinkLogDirectory());

    if(dlg.exec() == QDialog::Accepted) {
        QDir dir = dlg.directory();
        QString name = dir.absolutePath();
        QGC::setMAVLinkLogDirectory(name);
        ui->mavlinkLogDirEdit->setText(name);
    }
}

void QGCSettingsWidget::setParamDir()
{
    QFileDialog dlg(this, "Set parameters directory");
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setDirectory(QGC::parameterDirectory());

    if(dlg.exec() == QDialog::Accepted) {
        QDir dir = dlg.directory();
        QString name = dir.absolutePath();
        QGC::setParameterDirectory(name);
        ui->paramDirEdit->setText(name);
    }
}

void QGCSettingsWidget::setAppDataDir()
{
    QFileDialog dlg(this, "Set application data directory");
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setDirectory(QGC::appDataDirectory());

    if(dlg.exec() ==  QDialog::Accepted) {
        QDir dir = dlg.directory();
        QString name = dir.absolutePath();
        QGC::setAppDataDirectory(name);
        ui->appDataDirEdit->setText(name);
    }
}

void QGCSettingsWidget::setMissionsDir()
{
    QFileDialog dlg(this, "Set missions directory");
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setDirectory(QGC::missionDirectory());

    if(dlg.exec() ==  QDialog::Accepted) {
        QDir dir = dlg.directory();
        QString name = dir.absolutePath();
        QGC::setMissionDirectory(name);
        ui->missionsDirEdit->setText(name);
    }
}

void QGCSettingsWidget::setActiveUAS(UASInterface *uas)
{
    if (m_uas){
        m_uas = NULL;
    }

    if (uas != NULL){
        m_uas = uas;
    }
}

void QGCSettingsWidget::setDataRateLineEdits()
{
    QSettings settings;
    settings.beginGroup("DATA_RATES");
    ui->extStatusLineEdit->setText(settings.value("EXT_SYS_STATUS",2).toString());
    ui->positionLineEdit->setText(settings.value("POSITION",3).toString());
    ui->extra1LineEdit->setText(settings.value("EXTRA1",10).toString());
    ui->extra2LineEdit->setText(settings.value("EXTRA2",10).toString());
    ui->extra3LineEdit->setText(settings.value("EXTRA3",2).toString());

    ui->rawSensorLineEdit->setText(settings.value("RAW_SENSOR_DATA",2).toString());
    ui->rcChannelDataLineEdit->setText(settings.value("RC_CHANNEL_DATA",2).toString());
    settings.endGroup();
}

void QGCSettingsWidget::ratesChanged()
{
    QSettings settings;
    settings.beginGroup("DATA_RATES");
    bool ok;
    int conversion = ui->extStatusLineEdit->text().toInt(&ok);
    if (ok){
        settings.setValue("EXT_SYS_STATUS",conversion);
    }

    conversion = ui->positionLineEdit->text().toInt(&ok);
    if (ok){
        settings.setValue("POSITION",conversion);
    }

    conversion = ui->extra1LineEdit->text().toInt(&ok);
    if (ok){
        settings.setValue("EXTRA1", conversion);
    }

    conversion = ui->extra2LineEdit->text().toInt(&ok);
    if (ok){
        settings.setValue("EXTRA2", conversion);
    }

    conversion = ui->extra3LineEdit->text().toInt(&ok);
    if (ok){
        settings.setValue("EXTRA3", conversion);
    }

    conversion = ui->rawSensorLineEdit->text().toInt(&ok);
    if (ok){
        settings.setValue("RAW_SENSOR_DATA", conversion);
    }

    conversion = ui->rcChannelDataLineEdit->text().toInt(&ok);
    if (ok){
        settings.setValue("RC_CHANNEL_DATA", conversion);
    }
    settings.endGroup();
    settings.sync();

    setDataRateLineEdits();

    if (m_uas) {
        ArduPilotMegaMAV *mav =
            dynamic_cast<ArduPilotMegaMAV *>(m_uas.data());
        if (mav != NULL){
            mav->RequestAllDataStreams();
        }
    }
}

void QGCSettingsWidget::mavIdChanged(int id)
{
    MainWindow::instance()->setGroundStationSystemId(id);
}

void QGCSettingsWidget::componentIdChanged(int id)
{
    quint8 localID = static_cast<quint8>(id);
    QGC::setComponentID(localID);
}

void QGCSettingsWidget::setBetaRelease(bool state)
{
    QString type;
    QSettings settings;
    settings.beginGroup("AUTO_UPDATE");
    if (state == true){
        type = "beta";
    } else {
        type = "stable";
    }
    settings.setValue("RELEASE_TYPE", type);
    settings.sync();
}

void QGCSettingsWidget::setHideDonateButton(bool state)
{
    QSettings settings;
    settings.setValue("USER_DONATED", state);
    settings.sync();
}
