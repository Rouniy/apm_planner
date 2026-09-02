#include "SetupView.h"

#include "AccelCalibrationConfig.h"
#include "AirspeedConfig.h"
#include "ApmCustomFirmwareConfig.h"
#include "ArduPilotMegaMAV.h"
#include "BatteryMonitorConfig.h"
#include "CameraGimbalConfig.h"
#include "ConfigAdvancedView.h"
#include "ConfigADSBView.h"
#include "ConfigBatteryMonitoring2View.h"
#include "ConfigDefaultSettingsView.h"
#include "CompassConfig.h"
#include "ConfigDroneCanView.h"
#include "ConfigDeveloperToolsView.h"
#include "ConfigElevationSourcesView.h"
#include "ConfigESCCalibrationView.h"
#include "ConfigGpsInjectView.h"
#include "ConfigGPSOrderView.h"
#include "ConfigHWCANView.h"
#include "ConfigHWIDView.h"
#include "ConfigHWBTSerialService.h"
#include "ConfigHWBTView.h"
#include "ConfigHWESP8266View.h"
#include "ConfigInitialParamsView.h"
#include "ConfigMavCommandView.h"
#include "ConfigMotorTestView.h"
#include "ConfigParachuteView.h"
#include "ConfigRadioOutputView.h"
#include "ConfigSerialView.h"
#include "ConfigRawParams.h"
#include "FrameDefaultCatalogService.h"
#include "comm/AdsbIdentificationClient.h"
#include "comm/DroneCanGetNodeInfoClient.h"
#include "comm/DroneCanGetSetClient.h"
#include "comm/DroneCanMavlinkTransport.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/Esp8266ParameterClient.h"
#include "comm/VehicleTargetManager.h"
#include "FailSafeConfig.h"
#include "FlightModeConfig.h"
#include "FrameTypeConfig.h"
#include "LinkInterface.h"
#include "LinkManager.h"
#include "SerialLinkInterface.h"
#include "TerminalConsole.h"
#include "OpticalFlowConfig.h"
#include "OsdConfig.h"
#include "QGCUASParamManager.h"
#include "QGCCore.h"
#include "Radio3DRConfig.h"
#include "QmlPluginManagerView.h"
#include "RadioCalibrationConfig.h"
#include "RangeFinderConfig.h"
#include "UASInterface.h"
#include "UASManager.h"
#include "APMFirmwareVersion.h"
#include "AppPaths.h"
#include "core/parameters/ParameterMetaDataRepository.h"
#include "ui/BackstageView.h"

#include <QDir>
#include <QDateTime>
#include <QFrame>
#include <QFile>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const QString kInstallFirmware = QStringLiteral("InstallFirmwareView");
const QString kMandatoryGroup = QStringLiteral("MandatoryHardwareGroup");
const QString kFrameType = QStringLiteral("ConfigFrameClassTypeView");
const QString kDefaultSettings = QStringLiteral("ConfigDefaultSettingsView");
const QString kAccelCalibration = QStringLiteral("ConfigAccelCalibrationView");
const QString kCompass = QStringLiteral("ConfigCompassView");
const QString kRadioInput = QStringLiteral("ConfigRadioInputView");
const QString kRadioOutput = QStringLiteral("ConfigRadioOutputView");
const QString kSerialPorts = QStringLiteral("ConfigSerialView");
const QString kEscCalibration = QStringLiteral("ConfigESCCalibrationView");
const QString kFlightModes = QStringLiteral("ConfigFlightModesView");
const QString kFailSafe = QStringLiteral("ConfigFailSafeView");
const QString kInitialParams = QStringLiteral("ConfigInitialParamsView");
const QString kHWID = QStringLiteral("ConfigHWIDView");
const QString kADSB = QStringLiteral("ConfigADSBView");
const QString kOptionalGroup = QStringLiteral("OptionalHardwareGroup");
const QString kSikRadio = QStringLiteral("SikRadioView");
const QString kGPSInject = QStringLiteral("ConfigGpsInjectView");
const QString kGPSOrder = QStringLiteral("ConfigGPSOrderView");
const QString kDroneCAN = QStringLiteral("ConfigDroneCanView");
const QString kHWCAN = QStringLiteral("ConfigHWCANView");
const QString kBatteryMonitor = QStringLiteral("ConfigBatteryMonitoringView");
const QString kBatteryMonitor2 = QStringLiteral("ConfigBatteryMonitoring2View");
const QString kRangeFinder = QStringLiteral("ConfigRangeFinderView");
const QString kAirspeed = QStringLiteral("ConfigAirspeedView");
const QString kOpticalFlow = QStringLiteral("ConfigOptFlowView");
const QString kOsd = QStringLiteral("ConfigHWOSDView");
const QString kCameraGimbal = QStringLiteral("ConfigMountView");
const QString kMotorTest = QStringLiteral("ConfigMotorTestView");
const QString kBluetoothSetup = QStringLiteral("ConfigHWBTView");
const QString kParachute = QStringLiteral("ConfigParachuteView");
const QString kESP8266 = QStringLiteral("ConfigHWESP8266View");
const QString kAdvancedGroup = QStringLiteral("AdvancedGroup");
const QString kAdvancedTools = QStringLiteral("ConfigAdvancedView");
const QString kElevationSources = QStringLiteral("ConfigElevationSourcesView");
const QString kDeveloperTools = QStringLiteral("ConfigDeveloperToolsView");
const QString kMissionCommandList = QStringLiteral("ConfigMavCommandView");
const QString kTerminal = QStringLiteral("ConfigTerminalView");
const QString kQmlPlugins = QStringLiteral("QmlPluginManagerView");

QWidget *scrollablePage(QWidget *content, const QString &objectName,
                        QWidget *parent)
{
    content->setObjectName(objectName + QStringLiteral("Content"));
    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName(objectName);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
}

template<typename Page>
QWidget *makePage(const QString &objectName, QWidget *parent)
{
    return scrollablePage(new Page, objectName, parent);
}

template<typename Page>
BackstagePage makeBackstagePage(const QString &id, const QString &header,
                                bool subPage = false,
                                bool requiresConnection = false)
{
    BackstagePage definition;
    definition.id = id;
    definition.header = header;
    definition.isSub = subPage;
    definition.requiresConnection = requiresConnection;
    definition.factory = [id](QWidget *parent) {
        return makePage<Page>(id, parent);
    };
    return definition;
}

ParameterFirmwareFamily firmwareFamily(UASInterface *uas)
{
    if (!uas
        || uas->getAutopilotType() != MAV_AUTOPILOT_ARDUPILOTMEGA) {
        return ParameterFirmwareFamily::Unknown;
    }
    switch (uas->getSystemType()) {
    case MAV_TYPE_FIXED_WING:
    case MAV_TYPE_VTOL_DUOROTOR:
    case MAV_TYPE_VTOL_QUADROTOR:
    case MAV_TYPE_VTOL_TILTROTOR:
    case MAV_TYPE_VTOL_RESERVED2:
    case MAV_TYPE_VTOL_RESERVED3:
    case MAV_TYPE_VTOL_RESERVED4:
    case MAV_TYPE_VTOL_RESERVED5:
        return ParameterFirmwareFamily::ArduPlane;
    case MAV_TYPE_GROUND_ROVER:
    case MAV_TYPE_SURFACE_BOAT:
        return ParameterFirmwareFamily::Rover;
    case MAV_TYPE_TRICOPTER:
    case MAV_TYPE_QUADROTOR:
    case MAV_TYPE_COAXIAL:
    case MAV_TYPE_HELICOPTER:
    case MAV_TYPE_HEXAROTOR:
    case MAV_TYPE_OCTOROTOR:
    case MAV_TYPE_DODECAROTOR:
    case MAV_TYPE_DECAROTOR:
        return ParameterFirmwareFamily::ArduCopter;
    case MAV_TYPE_SUBMARINE:
        return ParameterFirmwareFamily::ArduSub;
    case MAV_TYPE_ANTENNA_TRACKER:
        return ParameterFirmwareFamily::AntennaTracker;
    default:
        return ParameterFirmwareFamily::Unknown;
    }
}
}

SetupView::SetupView(QWidget *parent)
    : QWidget(parent),
      m_backstage(new BackstageView(this)),
      m_metadataRepository(new ParameterMetaDataRepository(
          AppPaths::resourcePath(QStringLiteral("files/ardupilotmega")),
          QDir(AppPaths::writableDataDirectory()).filePath(
              QStringLiteral("cache/parameter-metadata")))),
      m_frameDefaultCatalogService(new FrameDefaultCatalogService(this))
{
    setObjectName(QStringLiteral("SetupView"));
    m_droneCanBroker =
        UASManager::instance()->droneCanForwardingBroker();
    m_droneCanTransport =
        UASManager::instance()->droneCanMavlinkTransport();
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_backstage);

    QSettings settings;
    settings.setFallbacksEnabled(false);
    m_advanced = settings.value(
        QStringLiteral("QGC_MAINWINDOW/ADVANCED_MODE"), false).toBool();

    buildPages();
    connect(m_backstage, &BackstageView::pageActivated,
            this, [](const QString &id, QWidget *page) {
        if (id == kDefaultSettings) {
            if (auto *defaults =
                    qobject_cast<ConfigDefaultSettingsView *>(page)) {
                defaults->activate();
            }
        } else if (id == kADSB) {
            if (auto *adsb = qobject_cast<ConfigADSBView *>(page)) {
                adsb->activate();
            }
        } else if (id == kESP8266) {
            if (auto *esp = qobject_cast<ConfigHWESP8266View *>(page)) {
                esp->activate();
            }
        }
    });
    connect(m_backstage, &BackstageView::pageDeactivated,
            this, [](const QString &id, QWidget *page) {
        if (id == kDefaultSettings) {
            if (auto *defaults =
                    qobject_cast<ConfigDefaultSettingsView *>(page)) {
                defaults->deactivate();
            }
        } else if (id == kADSB) {
            if (auto *adsb = qobject_cast<ConfigADSBView *>(page)) {
                adsb->deactivate();
            }
        } else if (id == kESP8266) {
            if (auto *esp = qobject_cast<ConfigHWESP8266View *>(page)) {
                esp->deactivate();
            }
        }
    });
    connect(m_backstage, &BackstageView::currentPageChanged,
            this, [this]() { refreshLoadingOverlay(); });
    connect(m_backstage, &BackstageView::stopLoadingRequested,
            this, &SetupView::stopParameterLoading);
    connect(m_backstage, &BackstageView::retryLoadingRequested,
            this, &SetupView::retryParameterLoading);
    connect(UASManager::instance(),
            QOverload<UASInterface *>::of(&UASManager::activeUASSet),
            this, &SetupView::activeUASSet);
    activeUASSet(UASManager::instance()->getActiveUAS());
}

SetupView::~SetupView()
{
    // Destroy pages with active transports while Setup's transport pointers
    // are still alive, so they can send their final stop commands safely.
    m_backstage->resetPage(kDroneCAN);
    m_backstage->resetPage(kMotorTest);
    m_backstage->resetPage(kDefaultSettings);
    m_backstage->resetPage(kADSB);
    m_backstage->resetPage(kESP8266);
}

void SetupView::buildPages()
{
    m_backstage->addPage(makeBackstagePage<ApmCustomFirmwareConfig>(
        kInstallFirmware, tr("Install Firmware")));

    m_backstage->addGroup(tr(">> Mandatory Hardware"), kMandatoryGroup);
    m_backstage->addPage(makeBackstagePage<FrameTypeConfig>(
        kFrameType, tr("Frame Type"), true, true));
    BackstagePage defaultSettings;
    defaultSettings.id = kDefaultSettings;
    defaultSettings.header = tr("Default Settings");
    defaultSettings.isSub = true;
    defaultSettings.requiresConnection = true;
    defaultSettings.allowsPartialParameters = false;
    defaultSettings.factory = [this](QWidget *parent) {
        return createDefaultSettingsPage(parent);
    };
    m_backstage->addPage(defaultSettings);
    m_backstage->addPage(makeBackstagePage<AccelCalibrationConfig>(
        kAccelCalibration, tr("Accel Calibration"), true, true));
    m_backstage->addPage(makeBackstagePage<CompassConfig>(
        kCompass, tr("Compass"), true, true));
    m_backstage->addPage(makeBackstagePage<RadioCalibrationConfig>(
        kRadioInput, tr("Radio Calibration"), true, true));
    BackstagePage radioOutput;
    radioOutput.id = kRadioOutput;
    radioOutput.header = tr("Servo Output");
    radioOutput.isSub = true;
    radioOutput.requiresConnection = true;
    radioOutput.factory = [this](QWidget *parent) {
        return createRadioOutputPage(parent);
    };
    m_backstage->addPage(radioOutput);
    BackstagePage serialPorts;
    serialPorts.id = kSerialPorts;
    serialPorts.header = tr("Serial Ports");
    serialPorts.isSub = true;
    serialPorts.requiresConnection = true;
    serialPorts.factory = [this](QWidget *parent) {
        return createSerialPage(parent);
    };
    m_backstage->addPage(serialPorts);
    BackstagePage escCalibration;
    escCalibration.id = kEscCalibration;
    escCalibration.header = tr("ESC Calibration");
    escCalibration.isSub = true;
    escCalibration.requiresConnection = true;
    escCalibration.factory = [this](QWidget *parent) {
        return createEscCalibrationPage(parent);
    };
    m_backstage->addPage(escCalibration);
    m_backstage->addPage(makeBackstagePage<FlightModeConfig>(
        kFlightModes, tr("Flight Modes"), true, true));
    m_backstage->addPage(makeBackstagePage<FailSafeConfig>(
        kFailSafe, tr("FailSafe"), true, true));
    BackstagePage initialParams;
    initialParams.id = kInitialParams;
    initialParams.header = tr("Initial Parameters");
    initialParams.isSub = true;
    initialParams.requiresConnection = true;
    initialParams.factory = [this](QWidget *parent) {
        return createInitialParamsPage(parent);
    };
    m_backstage->addPage(initialParams);
    BackstagePage hwId;
    hwId.id = kHWID;
    hwId.header = tr("HW ID");
    hwId.isSub = true;
    hwId.requiresConnection = true;
    hwId.allowsPartialParameters = false;
    hwId.factory = [this](QWidget *parent) {
        return createHWIDPage(parent);
    };
    m_backstage->addPage(hwId);
    BackstagePage adsb;
    adsb.id = kADSB;
    adsb.header = tr("ADSB");
    adsb.isSub = true;
    adsb.requiresConnection = true;
    adsb.allowsPartialParameters = false;
    adsb.factory = [this](QWidget *parent) {
        return createADSBPage(parent);
    };
    m_backstage->addPage(adsb);

    m_backstage->addGroup(tr(">> Optional Hardware"), kOptionalGroup);
    BackstagePage gpsInject;
    gpsInject.id = kGPSInject;
    gpsInject.header = tr("RTK/GPS Inject");
    gpsInject.isSub = true;
    gpsInject.requiresConnection = false;
    gpsInject.allowsPartialParameters = true;
    gpsInject.factory = [this](QWidget *parent) {
        return createGpsInjectPage(parent);
    };
    m_backstage->addPage(gpsInject);
    m_backstage->addPage(makeBackstagePage<Radio3DRConfig>(
        kSikRadio, tr("Sik Radio"), true));
    BackstagePage gpsOrder;
    gpsOrder.id = kGPSOrder;
    gpsOrder.header = tr("CAN GPS Order");
    gpsOrder.isSub = true;
    gpsOrder.requiresConnection = true;
    gpsOrder.factory = [this](QWidget *parent) {
        return createGPSOrderPage(parent);
    };
    m_backstage->addPage(gpsOrder);
    m_backstage->addPage(makeBackstagePage<BatteryMonitorConfig>(
        kBatteryMonitor, tr("Battery Monitor"), true, true));
    BackstagePage batteryMonitor2;
    batteryMonitor2.id = kBatteryMonitor2;
    batteryMonitor2.header = tr("Battery Monitor 2");
    batteryMonitor2.isSub = true;
    batteryMonitor2.requiresConnection = true;
    batteryMonitor2.factory = [this](QWidget *parent) {
        return createBatteryMonitoring2Page(parent);
    };
    m_backstage->addPage(batteryMonitor2);
    m_backstage->addPage(makeBackstagePage<RangeFinderConfig>(
        kRangeFinder, tr("Range Finder"), true, true));
    m_backstage->addPage(makeBackstagePage<AirspeedConfig>(
        kAirspeed, tr("Airspeed"), true, true));
    m_backstage->addPage(makeBackstagePage<OpticalFlowConfig>(
        kOpticalFlow, tr("Optical Flow"), true, true));
    m_backstage->addPage(makeBackstagePage<OsdConfig>(
        kOsd, tr("OSD"), true, true));
    m_backstage->addPage(makeBackstagePage<CameraGimbalConfig>(
        kCameraGimbal, tr("Camera Gimbal"), true, true));
    BackstagePage motorTest;
    motorTest.id = kMotorTest;
    motorTest.header = tr("Motor Test");
    motorTest.isSub = true;
    motorTest.requiresConnection = true;
    motorTest.factory = [this](QWidget *parent) {
        return createMotorTestPage(parent);
    };
    m_backstage->addPage(motorTest);
    BackstagePage bluetoothSetup;
    bluetoothSetup.id = kBluetoothSetup;
    bluetoothSetup.header = tr("Bluetooth Setup");
    bluetoothSetup.isSub = true;
    bluetoothSetup.requiresConnection = false;
    bluetoothSetup.factory = [this](QWidget *parent) {
        return createBluetoothSetupPage(parent);
    };
    m_backstage->addPage(bluetoothSetup);
    BackstagePage parachute;
    parachute.id = kParachute;
    parachute.header = tr("Parachute");
    parachute.isSub = true;
    parachute.requiresConnection = true;
    parachute.factory = [this](QWidget *parent) {
        return createParachutePage(parent);
    };
    m_backstage->addPage(parachute);
    BackstagePage esp8266;
    esp8266.id = kESP8266;
    esp8266.header = tr("ESP8266 Setup");
    esp8266.isSub = true;
    esp8266.requiresConnection = true;
    // The bridge owns a separate component-240 parameter list, so it must not
    // be covered by the autopilot parameter-loading overlay.
    esp8266.allowsPartialParameters = true;
    esp8266.factory = [this](QWidget *parent) {
        return createESP8266Page(parent);
    };
    m_backstage->addPage(esp8266);
    BackstagePage droneCan;
    droneCan.id = kDroneCAN;
    droneCan.header = tr("DroneCAN/UAVCAN");
    droneCan.isSub = true;
    droneCan.requiresConnection = false;
    droneCan.allowsPartialParameters = true;
    droneCan.factory = [this](QWidget *parent) {
        return createDroneCanPage(parent);
    };
    m_backstage->addPage(droneCan);
    BackstagePage hwCan;
    hwCan.id = kHWCAN;
    hwCan.header = tr("HW CAN");
    hwCan.isSub = true;
    hwCan.requiresConnection = true;
    hwCan.factory = [this](QWidget *parent) {
        return createHWCANPage(parent);
    };
    m_backstage->addPage(hwCan);

    m_backstage->addGroup(tr(">> Advanced"), kAdvancedGroup);
    BackstagePage advancedTools;
    advancedTools.id = kAdvancedTools;
    advancedTools.header = tr("Advanced Tools");
    advancedTools.isSub = true;
    advancedTools.isAdvanced = true;
    advancedTools.allowsPartialParameters = true;
    advancedTools.factory = [this](QWidget *parent) {
        return new ConfigAdvancedView(window(), parent);
    };
    m_backstage->addPage(advancedTools);
    m_backstage->addPage(configElevationSourcesBackstagePage());
    BackstagePage developerTools;
    developerTools.id = kDeveloperTools;
    developerTools.header = tr("Developer Tools");
    developerTools.isSub = true;
    developerTools.isAdvanced = true;
    developerTools.allowsPartialParameters = true;
    developerTools.factory = [](QWidget *parent) {
        return new ConfigDeveloperToolsView(parent);
    };
    m_backstage->addPage(developerTools);
    m_backstage->addPage(configMavCommandBackstagePage());
    BackstagePage terminal;
    terminal.id = kTerminal;
    terminal.header = tr("Terminal");
    terminal.isSub = true;
    terminal.isAdvanced = true;
    terminal.allowsPartialParameters = true;
    terminal.factory = [](QWidget *parent) {
        auto *page = new TerminalConsole(parent);
        page->setObjectName(QStringLiteral("ConfigTerminalView"));
        return page;
    };
    m_backstage->addPage(terminal);
    BackstagePage qmlPlugins;
    qmlPlugins.id = kQmlPlugins;
    qmlPlugins.header = tr("QML Plugins");
    qmlPlugins.isSub = true;
    qmlPlugins.isAdvanced = true;
    qmlPlugins.allowsPartialParameters = true;
    qmlPlugins.factory = [](QWidget *parent) {
        auto *core = qobject_cast<QGCCore *>(QCoreApplication::instance());
        return new QmlPluginManagerView(
            core ? core->qmlPluginManager() : nullptr, parent);
    };
    m_backstage->addPage(qmlPlugins);
}

void SetupView::advModeChanged(bool advanced)
{
    if (m_advanced == advanced) {
        return;
    }
    m_advanced = advanced;
    refreshPageVisibility();
    emit advancedModeChanged(advanced);
}

void SetupView::activeUASSet(UASInterface *uas)
{
    const bool targetChanged = m_uas != uas;
    if (!targetChanged && m_uas) {
        QGCUASParamManager *const sharedManager =
            LinkManager::instance()->parameterManager();
        if (m_parameterManager != sharedManager) {
            parameterManagerChanged(sharedManager);
        }
        syncConnectionState();
        return;
    }
    if (targetChanged && m_uas) {
        // Let active transport pages stop while their original UAS is still
        // the active target.
        m_backstage->resetPage(kDroneCAN);
        m_backstage->resetPage(kMotorTest);
        if (m_droneCanTransport
            && m_droneCanTransport->pinnedUas() == m_uas) {
            m_droneCanTransport->unbindEndpoint(true);
        }
    }
    if (m_uas) {
        disconnect(m_uas, nullptr, this, nullptr);
    }
    if (m_gpsInjectPage) {
        m_gpsInjectPage->viewModel()->SetVehiclePosition(
            0.0, 0.0, 0.0, false);
    }
    bindParameterManager(nullptr);

    m_uas = uas;
    m_droneCanLastPrimaryLink = nullptr;
    m_parameterManager = nullptr;
    m_firmwareVersion.clear();
    m_officialFirmware = false;
    resetParameterProgress();

    if (m_uas) {
        connect(m_uas, &UASInterface::connected,
                this, &SetupView::vehicleConnected);
        connect(m_uas, &UASInterface::disconnected,
                this, &SetupView::vehicleDisconnected);
        connect(m_uas, &UASInterface::globalPositionChanged,
                this, [this](UASInterface *source, double latitude,
                             double longitude, double altitude,
                             quint64 timestamp) {
            Q_UNUSED(timestamp)
            if (!m_gpsInjectPage || source != m_uas) {
                return;
            }
            m_gpsInjectPage->viewModel()->SetVehiclePosition(
                latitude, longitude, altitude,
                source->globalPositionKnown());
        });
        const int expectedUasId = m_uas->getUASID();
        connect(m_uas, &UASInterface::mavlinkMessageRecieved,
                this, [this, expectedUasId](
                          LinkInterface *link, mavlink_message_t message) {
            if (m_uas && m_uas->getUASID() == expectedUasId
                && link && link->isConnected()
                && message.sysid == expectedUasId
                && message.compid == MAV_COMP_ID_AUTOPILOT1) {
                m_droneCanLastPrimaryLink = link;
            }
        });
        if (auto *apm = qobject_cast<ArduPilotMegaMAV *>(m_uas.data())) {
            connect(apm, &ArduPilotMegaMAV::versionDetected,
                    this, &SetupView::firmwareVersionDetected);
        }
        bindParameterManager(LinkManager::instance()->parameterManager());
        m_parametersReady = m_parameterManager
            && m_parameterManager->parameterListReady();
    }

    m_connected = hasConnectedLink();
    emit connectionStateChanged(m_connected);
    if (m_parameterManager) {
        if (!m_connected) {
            m_parameterManager->cancelParameterList();
            m_parametersReady = false;
        } else if (!m_parametersReady
                   && !m_parameterManager->parameterListInProgress()) {
            m_parameterManager->requestParameterList();
        }
    }
    refreshPageVisibility();
    if (targetChanged) {
        resetConnectionPages();
    }
    if (auto *apm = qobject_cast<ArduPilotMegaMAV *>(m_uas.data())) {
        if (apm->getFirmwareVersion().isValid()) {
            firmwareVersionDetected(
                apm->getFirmwareVersion().versionString());
        }
    }
}

void SetupView::vehicleConnected()
{
    syncConnectionState();
}

void SetupView::vehicleDisconnected()
{
    syncConnectionState();
}

void SetupView::parameterListUpToDate(int component)
{
    Q_UNUSED(component)
    if (sender() && sender() != m_parameterManager) {
        return;
    }
    m_parametersReady = true;
    m_parameterLoadFailure.clear();
    m_parameterLoadingCanceled = false;
    m_parameterRetryPending = false;
    refreshLoadingOverlay();
}

void SetupView::parameterListLoadStarted()
{
    if (sender() && sender() != m_parameterManager) {
        return;
    }
    m_parametersReady = false;
    m_parameterLoadFailure.clear();
    m_parameterLoadingCanceled = false;
    refreshLoadingOverlay();
}

void SetupView::parameterListReadyChanged(bool ready)
{
    if (sender() && sender() != m_parameterManager) {
        return;
    }
    m_parametersReady = ready;
    if (ready) {
        m_parameterLoadFailure.clear();
        m_parameterLoadingCanceled = false;
        m_parameterRetryPending = false;
    }
    refreshLoadingOverlay();
}

void SetupView::parameterListLoadFailed(const QString &reason)
{
    if (sender() && sender() != m_parameterManager) {
        return;
    }
    m_parametersReady = false;
    m_parameterLoadFailure = reason;
    m_parameterLoadingCanceled = false;
    m_parameterRetryPending = false;
    refreshLoadingOverlay();
}

void SetupView::parameterListLoadCanceled()
{
    if (sender() && sender() != m_parameterManager) {
        return;
    }
    m_parametersReady = false;
    m_parameterLoadFailure.clear();
    m_parameterLoadingCanceled = true;
    m_parameterRetryPending = false;
    refreshLoadingOverlay();
}

void SetupView::parameterManagerChanged(QGCUASParamManager *manager)
{
    const qulonglong revision = ++m_parameterTargetRevision;
    bindParameterManager(manager);
    resetParameterProgress();
    refreshPageVisibility();
    resetConnectionPages();

    const QPointer<QGCUASParamManager> expectedManager(manager);
    QTimer::singleShot(0, this, [this, expectedManager, revision]() {
        if (revision != m_parameterTargetRevision
            || !expectedManager || m_parameterManager != expectedManager) {
            return;
        }
        m_parametersReady = expectedManager->parameterListReady();
        if (m_connected && !m_parametersReady
            && !expectedManager->parameterListInProgress()) {
            expectedManager->requestParameterList();
        }
        refreshLoadingOverlay();
    });
}

void SetupView::stopParameterLoading()
{
    if (!m_connected) {
        return;
    }
    m_backstage->setParameterLoadingStopping();
    if (m_parameterManager) {
        m_parameterManager->cancelParameterList();
    } else {
        m_parametersReady = false;
        m_parameterLoadFailure.clear();
        m_parameterLoadingCanceled = true;
        m_parameterRetryPending = false;
        refreshLoadingOverlay();
    }
}

void SetupView::retryParameterLoading()
{
    if (!m_connected || !m_parameterManager || m_parameterRetryPending) {
        return;
    }
    resetParameterProgress();
    m_parameterRetryPending = true;
    m_parameterManager->requestParameterList();
    refreshLoadingOverlay();
    m_backstage->setParameterLoadingRequesting();
}

void SetupView::firmwareVersionDetected(const QString &versionText)
{
    Q_UNUSED(versionText)
    auto *apm = qobject_cast<ArduPilotMegaMAV *>(m_uas.data());
    if (!apm) {
        return;
    }
    const APMFirmwareVersion firmware = apm->getFirmwareVersion();
    if (!firmware.isValid()) {
        return;
    }
    const QString normalized = QStringLiteral("%1.%2.%3")
        .arg(firmware.majorNumber())
        .arg(firmware.minorNumber())
        .arg(firmware.patchNumber());
    const bool official = firmware.isOfficial();
    if (normalized == m_firmwareVersion
        && official == m_officialFirmware) {
        return;
    }
    m_firmwareVersion = normalized;
    m_officialFirmware = official;

    const QString selectedPage = m_backstage->currentPageId();
    m_backstage->resetPage(kEscCalibration);
    m_backstage->resetPage(kDefaultSettings);
    m_backstage->resetPage(kMotorTest);
    m_backstage->resetPage(kRadioOutput);
    m_backstage->resetPage(kSerialPorts);
    m_backstage->resetPage(kInitialParams);
    m_backstage->resetPage(kParachute);
    m_backstage->resetPage(kGPSOrder);
    m_backstage->resetPage(kHWCAN);
    m_backstage->resetPage(kADSB);
    if (m_connected && m_backstage->isPageVisible(selectedPage)) {
        m_backstage->setCurrentPage(selectedPage);
    }
}

void SetupView::refreshPageVisibility()
{
    const bool copter = m_connected
        && firmwareFamily(m_uas) == ParameterFirmwareFamily::ArduCopter;

    m_backstage->setGroupVisible(kMandatoryGroup, m_connected);
    m_backstage->setPageVisible(kFrameType, copter);
    m_backstage->setPageVisible(kDefaultSettings, copter);
    m_backstage->setPageVisible(kAccelCalibration, m_connected);
    m_backstage->setPageVisible(kCompass, m_connected);
    m_backstage->setPageVisible(kRadioInput, m_connected);
    m_backstage->setPageVisible(kRadioOutput, m_connected);
    m_backstage->setPageVisible(
        kSerialPorts,
        m_connected
            && firmwareFamily(m_uas) != ParameterFirmwareFamily::Unknown);
    m_backstage->setPageVisible(kEscCalibration, m_connected);
    m_backstage->setPageVisible(kFlightModes, m_connected);
    m_backstage->setPageVisible(kFailSafe, m_connected);
    const ParameterFirmwareFamily family = firmwareFamily(m_uas);
    m_backstage->setPageVisible(
        kInitialParams,
        m_connected
            && (family == ParameterFirmwareFamily::ArduCopter
                || family == ParameterFirmwareFamily::ArduPlane));
    m_backstage->setPageVisible(kHWID, m_connected);
    m_backstage->setPageVisible(kADSB, m_connected);

    m_backstage->setGroupVisible(kOptionalGroup, true);
    m_backstage->setPageVisible(kGPSInject, true);
    m_backstage->setPageVisible(kSikRadio, true);
    m_backstage->setPageVisible(kGPSOrder, m_connected);
    m_backstage->setPageVisible(kBatteryMonitor, m_connected);
    m_backstage->setPageVisible(kBatteryMonitor2, m_connected);
    m_backstage->setPageVisible(kRangeFinder, m_connected);
    m_backstage->setPageVisible(kAirspeed, m_connected);
    m_backstage->setPageVisible(kOpticalFlow, m_connected);
    m_backstage->setPageVisible(kOsd, m_connected);
    m_backstage->setPageVisible(kCameraGimbal, m_connected);
    m_backstage->setPageVisible(
        kMotorTest,
        m_connected
            && firmwareFamily(m_uas) != ParameterFirmwareFamily::ArduSub);
    m_backstage->setPageVisible(kBluetoothSetup, true);
    m_backstage->setPageVisible(kParachute, m_connected);
    m_backstage->setPageVisible(kESP8266, m_connected);
    m_backstage->setPageVisible(kDroneCAN, true);
    m_backstage->setPageVisible(kHWCAN, m_connected);

    m_backstage->setGroupVisible(kAdvancedGroup, m_advanced);
    m_backstage->setPageVisible(kAdvancedTools, m_advanced);
    m_backstage->setPageVisible(kElevationSources, m_advanced);
    m_backstage->setPageVisible(kDeveloperTools, m_advanced);
    m_backstage->setPageVisible(kMissionCommandList, m_advanced);
    m_backstage->setPageVisible(kTerminal, m_advanced);
    m_backstage->setPageVisible(kQmlPlugins, m_advanced);
    refreshLoadingOverlay();
}

bool SetupView::currentPageAllowsPartialParameters() const
{
    const BackstagePage definition =
        m_backstage->pageDefinition(m_backstage->currentPageId());
    return definition.allowsPartialParameters;
}

bool SetupView::hasConnectedLink() const
{
    if (!m_uas || !m_uas->getLinks()) {
        return false;
    }
    for (LinkInterface *link : *m_uas->getLinks()) {
        if (link && link->isConnected()) {
            return true;
        }
    }
    return false;
}

void SetupView::syncConnectionState()
{
    const bool connected = hasConnectedLink();
    if (connected == m_connected) {
        return;
    }
    m_connected = connected;
    emit connectionStateChanged(m_connected);
    resetParameterProgress();
    if (m_parameterManager) {
        if (!m_connected) {
            m_parameterManager->cancelParameterList();
        } else {
            m_parametersReady = m_parameterManager->parameterListReady();
            if (!m_parametersReady
                && !m_parameterManager->parameterListInProgress()) {
                m_parameterManager->requestParameterList();
            }
        }
    }
    refreshPageVisibility();
    resetConnectionPages();
}

void SetupView::bindParameterManager(QGCUASParamManager *manager)
{
    if (m_parameterManager == manager) {
        return;
    }
    if (m_parameterManager) {
        disconnect(m_parameterManager, nullptr, this, nullptr);
    }
    m_parameterManager = manager;
    if (!m_parameterManager) {
        return;
    }
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListUpToDate,
            this, &SetupView::parameterListUpToDate);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterTargetChanged,
            this, &SetupView::parameterTargetChanged);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadStarted,
            this, &SetupView::parameterListLoadStarted);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListReadyChanged,
            this, &SetupView::parameterListReadyChanged);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadFailed,
            this, &SetupView::parameterListLoadFailed);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadCanceled,
            this, &SetupView::parameterListLoadCanceled);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListProgressChanged,
            this, [this](int, int, int) { refreshLoadingOverlay(); });
}

void SetupView::parameterTargetChanged()
{
    const QString currentPage = m_backstage->currentPageId();
    if (!currentPage.isEmpty()) {
        m_targetPageToRestore = currentPage;
    }
    const qulonglong revision = ++m_parameterTargetRevision;
    const QPointer<QGCUASParamManager> expectedManager(m_parameterManager);
    resetParameterProgress();
    resetConnectionPages(false);
    QTimer::singleShot(0, this, [this, expectedManager, revision]() {
        if (revision != m_parameterTargetRevision
            || !expectedManager || m_parameterManager != expectedManager) {
            return;
        }
        if (expectedManager) {
            m_parametersReady = expectedManager->parameterListReady();
            if (m_connected && !m_parametersReady
                && !expectedManager->parameterListInProgress()) {
                expectedManager->requestParameterList();
            }
        }
        refreshPageVisibility();
        refreshLoadingOverlay();
        const QString pageToRestore = m_targetPageToRestore;
        m_targetPageToRestore.clear();
        if (m_connected && m_backstage->isPageVisible(pageToRestore)) {
            m_backstage->setCurrentPage(pageToRestore);
        }
    });
}

void SetupView::resetConnectionPages(bool restoreSelection)
{
    const QString selectedPage = m_backstage->currentPageId();
    m_backstage->setAutomaticSelectionEnabled(false);
    for (const QString &pageId : m_backstage->pageIds()) {
        if (m_backstage->pageDefinition(pageId).requiresConnection) {
            m_backstage->resetPage(pageId);
        }
    }
    m_backstage->setAutomaticSelectionEnabled(true);
    if (restoreSelection && m_connected
        && m_backstage->isPageVisible(selectedPage)) {
        m_backstage->setCurrentPage(selectedPage);
    }
}

QWidget *SetupView::createDefaultSettingsPage(QWidget *parent)
{
    const ParameterFirmwareFamily family = firmwareFamily(m_uas);
    const QString catalogVersion = m_officialFirmware
        ? m_firmwareVersion : QString();
    const ParameterMetaDataCatalog catalog = m_metadataRepository->catalog(
        family, catalogVersion);
    const bool enforceMetadataRanges =
        m_metadataRepository->catalogMatchesFirmwareVersion(
            family, catalogVersion);

    auto *page = new ConfigDefaultSettingsView(
        m_frameDefaultCatalogService, catalog, parent,
        enforceMetadataRanges);
    page->setConnected(m_connected);
    ConfigRawParams *const rawParams = page->rawParams();
    const QPointer<QGCUASParamManager> expectedManager(m_parameterManager);

    if (expectedManager && expectedManager->store()) {
        const ParameterSnapshot snapshot = expectedManager->store()->snapshot();
        page->setParameterSnapshot(
            snapshot.records(), snapshot.endpoint().componentId);

        connect(expectedManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                rawParams, &ConfigRawParams::parameterChanged);
        connect(expectedManager,
                &QGCUASParamManager::parameterWriteAcknowledged,
                rawParams, &ConfigRawParams::parameterWriteAcknowledged);
        connect(expectedManager,
                &QGCUASParamManager::parameterWriteFailed,
                rawParams, &ConfigRawParams::parameterWriteFailed);
        connect(expectedManager,
                &QGCUASParamManager::parameterWriteCancelled,
                rawParams, &ConfigRawParams::parameterWriteCancelled);
        connect(expectedManager,
                &QGCUASParamManager::parameterBatchProgress,
                rawParams, &ConfigRawParams::parameterBatchProgress);
        connect(expectedManager,
                &QGCUASParamManager::parameterBatchCompleted,
                rawParams, &ConfigRawParams::parameterBatchCompleted);
        connect(expectedManager,
                &QGCUASParamManager::parameterTargetChanged,
                page, &ConfigDefaultSettingsView::parameterTargetChanged);
        connect(expectedManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page, expectedManager](bool ready) {
            if (!ready || !expectedManager
                || m_parameterManager != expectedManager
                || !expectedManager->store()) {
                return;
            }
            const ParameterSnapshot refreshed =
                expectedManager->store()->snapshot();
            page->setParameterSnapshot(
                refreshed.records(), refreshed.endpoint().componentId);
        });
    }

    connect(page, &ConfigDefaultSettingsView::refreshRequested,
            page, [this, expectedManager](int componentId) {
        if (!m_connected || !expectedManager
            || m_parameterManager != expectedManager) {
            return;
        }
        if (m_uas && m_uas->isArmed()
            && QMessageBox::question(
                   this, tr("Refresh Params"),
                   tr("The vehicle is armed. Refreshing the complete parameter "
                      "list can consume telemetry bandwidth. Continue?"),
                   QMessageBox::Yes | QMessageBox::No,
                   QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        if (expectedManager->store()
            && componentId
                == expectedManager->store()->endpoint().componentId) {
            retryParameterLoading();
        }
    });
    connect(page, &ConfigDefaultSettingsView::writeRequested,
            page, [this, page, expectedManager](
                      int componentId, const QVariantList &changes) {
        ConfigRawParams *const raw = page->rawParams();
        if (!m_connected || !expectedManager
            || m_parameterManager != expectedManager) {
            raw->parameterWriteSubmissionFailed(tr("Not connected."));
            return;
        }
        const QPointer<ConfigDefaultSettingsView> guard(page);
        const qulonglong batchId = expectedManager->writeParameters(
            componentId, changes);
        if (!guard) {
            return;
        }
        if (batchId == 0) {
            guard->rawParams()->parameterWriteSubmissionFailed(
                tr("The parameter batch was rejected for the selected target."));
            return;
        }
        guard->rawParams()->parameterBatchSubmitted(
            batchId, changes.size());
    });
    return page;
}

QWidget *SetupView::createHWIDPage(QWidget *parent)
{
    auto *page = new ConfigHWIDView(parent);
    const QPointer<QGCUASParamManager> expectedManager(m_parameterManager);
    if (expectedManager && expectedManager->store()) {
        const ParameterSnapshot snapshot = expectedManager->store()->snapshot();
        page->setParameterSnapshot(
            snapshot.records(), snapshot.endpoint().componentId);
        connect(expectedManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page, expectedManager](bool ready) {
            if (!ready || !expectedManager
                || m_parameterManager != expectedManager
                || !expectedManager->store()) {
                return;
            }
            const ParameterSnapshot refreshed =
                expectedManager->store()->snapshot();
            page->setParameterSnapshot(
                refreshed.records(), refreshed.endpoint().componentId);
        });
    }
    connect(page, &ConfigHWIDView::refreshRequested,
            page, [this, page, expectedManager](int componentId) {
        if (!m_connected || !expectedManager
            || m_parameterManager != expectedManager
            || !expectedManager->store()) {
            return;
        }
        const ParameterSnapshot refreshed =
            expectedManager->store()->snapshot();
        page->setParameterSnapshot(refreshed.records(), componentId);
    });
    return page;
}

QWidget *SetupView::createADSBPage(QWidget *parent)
{
    const ParameterFirmwareFamily family = firmwareFamily(m_uas);
    const QString catalogVersion = m_officialFirmware
        ? m_firmwareVersion : QString();
    ParameterMetaDataCatalog catalog = m_metadataRepository->catalog(
        family, catalogVersion);
    if (!catalog.isValid()
        && family != ParameterFirmwareFamily::Unknown
        && family != ParameterFirmwareFamily::ArduCopter) {
        catalog = m_metadataRepository->catalog(
            ParameterFirmwareFamily::ArduCopter);
    }
    const bool enforceMetadataRanges =
        m_metadataRepository->catalogMatchesFirmwareVersion(
            family, catalogVersion);

    LinkManager *const links = LinkManager::instance();
    VehicleTargetManager *const targets = links
        ? links->vehicleTargetManager() : nullptr;
    ExactLinkTransmitter *const transmitter = links
        ? links->exactLinkTransmitter() : nullptr;
    const VehicleTargetLease expectedTarget = targets
        ? targets->acquireTarget() : VehicleTargetLease{};
    const int expectedComponent = expectedTarget.isValid()
        ? expectedTarget.endpoint.componentId : MAV_COMP_ID_AUTOPILOT1;

    AdsbIdentificationClient *client = nullptr;
    if (targets && transmitter) {
        client = new AdsbIdentificationClient(targets, transmitter);
        client->bind(expectedTarget);
    }
    auto *page = new ConfigADSBView(
        client, catalog, parent, enforceMetadataRanges);
    if (client) {
        client->setParent(page);
    }

    const QPointer<UASInterface> expectedUas(m_uas);
    const QPointer<QGCUASParamManager> expectedManager(m_parameterManager);
    const QPointer<LinkInterface> expectedLink(
        expectedTarget.isValid() && links
            ? links->getLink(expectedTarget.endpoint.linkId) : nullptr);
    const auto targetIsCurrent =
        [this, targets, expectedTarget, expectedUas,
         expectedManager, expectedLink]() {
        return expectedTarget.isValid() && targets && expectedUas
            && expectedManager && expectedLink
            && m_uas == expectedUas
            && m_parameterManager == expectedManager
            && expectedUas->getUASID()
                == expectedTarget.endpoint.systemId
            && targets->isCurrentTarget(
                expectedTarget.endpoint.linkId,
                expectedTarget.endpoint.systemId,
                expectedTarget.endpoint.componentId,
                expectedTarget.generation);
    };
    const auto syncConnected =
        [page, targetIsCurrent, expectedLink]() {
        page->setConnected(
            targetIsCurrent() && expectedLink
            && expectedLink->isConnected());
    };

    if (expectedManager && expectedManager->store()
        && expectedTarget.isValid()) {
        const ParameterSnapshot snapshot =
            expectedManager->store()->snapshot(expectedTarget.endpoint);
        page->setParameterSnapshot(
            snapshot.records(), expectedComponent);
    }
    syncConnected();

    connect(this, &SetupView::connectionStateChanged,
            page, [syncConnected](bool) { syncConnected(); });
    if (expectedLink) {
        connect(expectedLink,
                QOverload<bool>::of(&LinkInterface::connected),
                page, [syncConnected](bool) { syncConnected(); });
    }

    connect(page, &ConfigADSBView::refreshRequested,
            page, [this, page, targetIsCurrent, expectedManager](int) {
        if (!m_connected || !targetIsCurrent() || !expectedManager
            || m_parameterManager != expectedManager) {
            page->parameterWriteSubmissionFailed(
                tr("not connected to the selected target"));
            return;
        }
        expectedManager->requestParameterList();
    });

    connect(page, &ConfigADSBView::writeRequested,
            page,
            [this, page, targetIsCurrent, expectedTarget,
             expectedManager, expectedLink](
                int componentId, const QString &name,
                const QVariant &value) {
        const auto reject = [page, componentId, name, value](
                                const QString &reason) {
            page->parameterWriteFailed(
                componentId, name, value, reason);
        };
        if (!m_connected || !targetIsCurrent()
            || !expectedManager || !expectedLink
            || !expectedLink->isConnected()
            || componentId != expectedTarget.endpoint.componentId) {
            reject(tr("not connected to the selected target"));
            return;
        }
        const QVariantList changes{QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("value"), value}
        }};
        if (expectedManager->writeParameters(
                componentId, changes) == 0) {
            reject(tr("write was rejected for the selected target"));
        }
    });

    connect(page, &ConfigADSBView::writeParamsRequested,
            page,
            [this, page, targetIsCurrent, expectedTarget,
             expectedManager, expectedLink](
                int componentId, const QVariantList &changes) {
        if (!m_connected || !targetIsCurrent()
            || !expectedManager || !expectedLink
            || !expectedLink->isConnected()
            || componentId != expectedTarget.endpoint.componentId) {
            page->parameterWriteSubmissionFailed(
                tr("not connected to the selected target"));
            return;
        }
        const QPointer<ConfigADSBView> guard(page);
        const qulonglong batchId = expectedManager->writeParameters(
            componentId, changes);
        if (!guard) {
            return;
        }
        if (batchId == 0) {
            guard->parameterWriteSubmissionFailed(
                tr("write was rejected for the selected target"));
            return;
        }
        guard->parameterBatchSubmitted(componentId, batchId);
    });

    if (client && expectedUas) {
        connect(expectedUas, &UASInterface::mavlinkMessageRecieved,
                page,
                [client, targetIsCurrent, expectedTarget, expectedLink](
                    LinkInterface *incomingLink,
                    const mavlink_message_t &message) {
            if (targetIsCurrent() && expectedLink
                && incomingLink == expectedLink
                && incomingLink->getId()
                    == expectedTarget.endpoint.linkId) {
                client->observeMessage(incomingLink->getId(), message);
            }
        });
    }

    if (expectedManager) {
        connect(expectedManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigADSBView::parameterChanged);
        connect(expectedManager,
                &QGCUASParamManager::parameterWriteFailed,
                page,
                [page, targetIsCurrent, expectedComponent](
                    qulonglong, qulonglong, int componentId,
                    const QString &name, int, const QString &reason) {
            if (targetIsCurrent() && componentId == expectedComponent) {
                page->parameterWriteFailed(
                    componentId, name, reason);
            }
        });
        connect(expectedManager,
                &QGCUASParamManager::parameterWriteCancelled,
                page,
                [page, targetIsCurrent, expectedComponent](
                    qulonglong, qulonglong, int componentId,
                    const QString &name) {
            if (targetIsCurrent() && componentId == expectedComponent) {
                page->parameterWriteFailed(
                    componentId, name, tr("write cancelled"));
            }
        });
        connect(expectedManager,
                &QGCUASParamManager::parameterBatchCompleted,
                page, &ConfigADSBView::parameterBatchCompleted);
        connect(expectedManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page,
                [page, targetIsCurrent, expectedManager,
                 expectedTarget, expectedComponent](bool ready) {
            if (!ready || !targetIsCurrent() || !expectedManager
                || !expectedManager->store()) {
                return;
            }
            const ParameterSnapshot snapshot =
                expectedManager->store()->snapshot(expectedTarget.endpoint);
            page->setParameterSnapshot(
                snapshot.records(), expectedComponent);
        });
    }

    return page;
}

QWidget *SetupView::createMotorTestPage(QWidget *parent)
{
    const ParameterFirmwareFamily family = firmwareFamily(m_uas);
    ParameterMetaDataCatalog catalog =
        m_metadataRepository->catalog(
            family,
            m_officialFirmware ? m_firmwareVersion : QString());
    if (!catalog.isValid()
        && family != ParameterFirmwareFamily::Unknown
        && family != ParameterFirmwareFamily::ArduCopter) {
        catalog = m_metadataRepository->catalog(
            ParameterFirmwareFamily::ArduCopter);
    }

    auto *page = new ConfigMotorTestView(catalog, parent);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);
    if (m_uas) {
        page->setVehicleType(m_uas->getSystemType());
    }
    QFile layoutFile(AppPaths::resourcePath(
        QStringLiteral("files/APMotorLayout.json")));
    if (!layoutFile.open(QIODevice::ReadOnly)) {
        layoutFile.setFileName(
            QStringLiteral(":/files/APMotorLayout.json"));
        layoutFile.open(QIODevice::ReadOnly);
    }
    if (layoutFile.isOpen()) {
        page->setMotorLayoutJson(layoutFile.readAll());
    }
    page->setConnected(m_connected);
    page->setArmed(m_uas && m_uas->isArmed());

    const int expectedUasId = m_uas ? m_uas->getUASID() : -1;
    connect(page, &ConfigMotorTestView::motorTestRequested,
            page, [this, page, expectedUasId](
                      int componentId, int motor, int throttleType,
                      int throttle, int durationSec, int motorCount,
                      int testOrder) {
        const bool stopCommand = throttle == 0 && durationSec == 0;
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId) {
            page->commandSendFailed(componentId, tr("not connected"));
            return;
        }
        if (!stopCommand && m_uas->isArmed()) {
            page->commandSendFailed(componentId, tr("vehicle armed"));
            return;
        }
        LinkInterface *commandLink = nullptr;
        if (m_uas->getLinks()) {
            for (LinkInterface *link : *m_uas->getLinks()) {
                if (link && link->isConnected()) {
                    commandLink = link;
                    break;
                }
            }
        }
        if (!m_uas->executeCommandOnLink(
                commandLink, MAV_CMD_DO_MOTOR_TEST, 0,
                static_cast<float>(motor),
                static_cast<float>(throttleType),
                static_cast<float>(throttle),
                static_cast<float>(durationSec),
                static_cast<float>(motorCount),
                static_cast<float>(testOrder),
                0.0f, componentId)) {
            page->commandSendFailed(componentId, tr("no connected link"));
        }
    });
    connect(page, &ConfigMotorTestView::writeRequested,
            page, [this, page, expectedUasId](
                      int componentId, const QString &name,
                      const QVariant &value) {
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId
            || !m_parameterManager) {
            page->parameterWriteFailed(componentId, name,
                                       tr("not connected"));
            return;
        }
        if (m_uas->isArmed()) {
            page->parameterWriteFailed(componentId, name,
                                       tr("vehicle armed"));
            return;
        }
        if (!m_parameterManager->getParameterNames(componentId)
                 .contains(name)) {
            page->parameterWriteFailed(componentId, name,
                                       tr("parameter unavailable"));
            return;
        }
        QVariant currentValue;
        if (m_parameterManager->getParameterValue(
                componentId, name, currentValue)
            && qAbs(currentValue.toDouble() - value.toDouble())
                <= 1.0e-6) {
            m_parameterManager->requestParameterUpdate(componentId, name);
            return;
        }
        m_parameterManager->setParameter(componentId, name, value);
    });
    if (m_uas) {
        connect(m_uas, &UASInterface::commandAckReceived,
                page, [this, page, expectedUasId](
                          int uasId, int componentId, int command,
                          int result, int progress, int resultParam2,
                          int targetSystem, int targetComponent) {
            Q_UNUSED(progress)
            Q_UNUSED(resultParam2)
            if (uasId == expectedUasId && m_uas
                && (targetSystem == 0
                    || targetSystem == m_uas->getSystemId())
                && (targetComponent == 0
                    || targetComponent == m_uas->getComponentId())) {
                page->commandAckReceived(componentId, command, result);
            }
        });
        connect(m_uas, QOverload<bool>::of(&UASInterface::armingChanged),
                page, &ConfigMotorTestView::setArmed);
        connect(m_uas, &UASInterface::connected,
                page, [this, page, expectedUasId]() {
            page->setConnected(
                m_connected && m_uas
                && m_uas->getUASID() == expectedUasId);
        });
        connect(m_uas, &UASInterface::disconnected,
                page, [this, page, expectedUasId]() {
            page->setConnected(
                m_connected && m_uas
                && m_uas->getUASID() == expectedUasId);
        });
    }
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigMotorTestView::parameterChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page, expectedUasId](bool ready) {
            if (ready && m_connected && m_uas
                && m_uas->getUASID() == expectedUasId
                && m_parameterManager && !page->viewModel()->Busy()) {
                page->setParameterSnapshot(
                    parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
                    MAV_COMP_ID_AUTOPILOT1);
            }
        });
    }
    return page;
}

QWidget *SetupView::createBluetoothSetupPage(QWidget *parent)
{
    auto *page = new ConfigHWBTView(parent);
    auto *service = new ConfigHWBTSerialService(page);

    QString preferredPort;
    if (m_uas && m_uas->getLinks()) {
        for (LinkInterface *link : *m_uas->getLinks()) {
            auto *serial = qobject_cast<SerialLinkInterface *>(link);
            if (serial) {
                preferredPort = serial->getPortName();
                break;
            }
        }
    }
    page->setPorts(ConfigHWBTSerialService::availablePorts(),
                   preferredPort);
    page->setMainLinkConnected(m_connected);
    connect(this, &SetupView::connectionStateChanged,
            page, &ConfigHWBTView::setMainLinkConnected);
    connect(page, &ConfigHWBTView::programRequested,
            service, &ConfigHWBTSerialService::program);
    connect(page, &ConfigHWBTView::cancelRequested,
            service, &ConfigHWBTSerialService::cancel);
    connect(page, &ConfigHWBTView::refreshPortsRequested,
            page, [page]() {
        page->setPorts(
            ConfigHWBTSerialService::availablePorts(),
            page->viewModel()->SelectedPort());
    });
    connect(service, &ConfigHWBTSerialService::progress,
            page, &ConfigHWBTView::operationProgress);
    connect(service, &ConfigHWBTSerialService::finished,
            page, &ConfigHWBTView::operationFinished);
    return page;
}

QWidget *SetupView::createParachutePage(QWidget *parent)
{
    ParameterFirmwareFamily family = firmwareFamily(m_uas);
    ParameterMetaDataCatalog catalog =
        m_metadataRepository->catalog(
            family,
            m_officialFirmware ? m_firmwareVersion : QString());
    if (!catalog.isValid()
        && family != ParameterFirmwareFamily::Unknown
        && family != ParameterFirmwareFamily::ArduCopter) {
        catalog = m_metadataRepository->catalog(
            ParameterFirmwareFamily::ArduCopter);
    }

    auto *page = new ConfigParachuteView(catalog, parent);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);
    page->setConnected(m_connected);
    page->setArmed(m_uas && m_uas->isArmed());

    const int expectedUasId = m_uas ? m_uas->getUASID() : -1;
    connect(page, &ConfigParachuteView::writeRequested,
            page, [this, page, expectedUasId](
                      int componentId, const QString &name,
                      const QVariant &value) {
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId
            || !m_parameterManager) {
            page->parameterWriteFailed(componentId, name,
                                       tr("not connected"));
            return;
        }
        if (m_uas->isArmed()) {
            page->parameterWriteFailed(componentId, name,
                                       tr("vehicle armed"));
            return;
        }
        if (!m_parameterManager->getParameterNames(componentId)
                 .contains(name)) {
            page->parameterWriteFailed(componentId, name,
                                       tr("parameter unavailable"));
            return;
        }
        QVariant currentValue;
        if (m_parameterManager->getParameterValue(
                componentId, name, currentValue)
            && qAbs(currentValue.toDouble() - value.toDouble())
                <= 1.0e-6) {
            // The parameter manager drops unchanged writes. Request an
            // explicit vehicle echo so the strict assignment sequence can
            // still prove every step before continuing.
            m_parameterManager->requestParameterUpdate(componentId, name);
            return;
        }
        m_parameterManager->setParameter(componentId, name, value);
    });
    connect(page, &ConfigParachuteView::refreshRequested,
            page, [this, page, expectedUasId](int componentId) {
        Q_UNUSED(componentId)
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId
            || !m_parameterManager) {
            page->refreshFailed(tr("not connected"));
            return;
        }
        if (m_parameterManager->parameterListInProgress()) {
            page->refreshFailed(tr("parameter refresh already in progress"));
            return;
        }
        m_parameterManager->requestParameterList();
    });
    if (m_uas) {
        connect(m_uas, &UASInterface::connected,
                page, [this, page, expectedUasId]() {
            page->setConnected(
                m_connected && m_uas
                && m_uas->getUASID() == expectedUasId);
        });
        connect(m_uas, &UASInterface::disconnected,
                page, [this, page, expectedUasId]() {
            page->setConnected(
                m_connected && m_uas
                && m_uas->getUASID() == expectedUasId);
        });
        connect(m_uas, QOverload<bool>::of(&UASInterface::armingChanged),
                page, &ConfigParachuteView::setArmed);
    }
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigParachuteView::parameterChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page, expectedUasId](bool ready) {
            if (ready && m_connected && m_uas
                && m_uas->getUASID() == expectedUasId
                && m_parameterManager
                && !page->viewModel()->HasPendingWrites()) {
                page->setParameterSnapshot(
                    parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
                    MAV_COMP_ID_AUTOPILOT1);
            }
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListLoadFailed,
                page, &ConfigParachuteView::refreshFailed);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListLoadCanceled,
                page, &ConfigParachuteView::refreshCanceled);
    }
    return page;
}

QWidget *SetupView::createESP8266Page(QWidget *parent)
{
    LinkManager *const links = LinkManager::instance();
    VehicleTargetManager *const targets = links
        ? links->vehicleTargetManager() : nullptr;
    ExactLinkTransmitter *const transmitter = links
        ? links->exactLinkTransmitter() : nullptr;
    const VehicleTargetLease expectedTarget = targets
        ? targets->acquireTarget() : VehicleTargetLease{};

    Esp8266ParameterClient *client = nullptr;
    if (targets && transmitter) {
        client = new Esp8266ParameterClient(targets, transmitter);
        client->bind(expectedTarget);
    }
    auto *page = new ConfigHWESP8266View(client, parent);
    if (client) {
        client->setParent(page);
    }

    const QPointer<UASInterface> expectedUas(m_uas);
    const QPointer<LinkInterface> expectedLink(
        expectedTarget.isValid() && links
            ? links->getLink(expectedTarget.endpoint.linkId) : nullptr);
    const auto targetIsCurrent =
        [this, targets, expectedTarget, expectedUas, expectedLink]() {
        return m_connected && expectedTarget.isValid() && targets
            && expectedUas && expectedLink
            && m_uas == expectedUas
            && expectedUas->getUASID()
                == expectedTarget.endpoint.systemId
            && targets->isCurrentTarget(
                expectedTarget.endpoint.linkId,
                expectedTarget.endpoint.systemId,
                expectedTarget.endpoint.componentId,
                expectedTarget.generation);
    };
    const auto syncConnected =
        [page, targetIsCurrent, expectedLink]() {
        page->setConnected(
            targetIsCurrent() && expectedLink
            && expectedLink->isConnected());
    };
    syncConnected();

    connect(this, &SetupView::connectionStateChanged,
            page, [syncConnected](bool) { syncConnected(); });
    if (expectedLink) {
        connect(expectedLink,
                QOverload<bool>::of(&LinkInterface::connected),
                page, [syncConnected](bool) { syncConnected(); });
    }
    if (client && expectedUas) {
        connect(expectedUas, &UASInterface::mavlinkMessageRecieved,
                page,
                [client, targetIsCurrent, expectedTarget, expectedLink](
                    LinkInterface *incomingLink,
                    const mavlink_message_t &message) {
            if (targetIsCurrent() && expectedLink
                && incomingLink == expectedLink
                && incomingLink->getId()
                    == expectedTarget.endpoint.linkId
                && message.sysid
                    == expectedTarget.endpoint.systemId) {
                client->observeMessage(incomingLink->getId(), message);
            }
        });
    }
    return page;
}

QWidget *SetupView::createGpsInjectPage(QWidget *parent)
{
    auto *page = new ConfigGpsInjectView;
    m_gpsInjectPage = page;
    connect(page, &QObject::destroyed, this, [this, page]() {
        if (m_gpsInjectPage == page) {
            m_gpsInjectPage = nullptr;
        }
    });

    ConfigGpsInjectViewModel *model = page->viewModel();
    if (m_uas) {
        model->SetVehiclePosition(
            m_uas->getLatitude(), m_uas->getLongitude(),
            m_uas->getAltitudeAMSL(), m_uas->globalPositionKnown());
    }
    connect(model, &ConfigGpsInjectViewModel::rtcmDataReady,
            page, [this, model](const QByteArray &frame) {
        if (frame.isEmpty()) {
            return;
        }
        const QPointer<UASInterface> target = m_uas;
        const bool accepted = target && target->injectGpsData(frame);
        model->ReportInjectionResult(
            frame.size(), accepted,
            accepted ? QString()
                     : target ? tr("no connected vehicle link")
                              : tr("no active vehicle"));
    }, Qt::QueuedConnection);

    const auto receiverUnavailable = [this, model](const QString &receiver) {
        model->SetReceiverStatus(tr(
            "%1 receiver auto-configuration is not implemented yet; "
            "RTCM injection remains available.").arg(receiver));
    };
    connect(model, &ConfigGpsInjectViewModel::ubloxConfigureRequested,
            page, [receiverUnavailable](bool) {
        receiverUnavailable(QStringLiteral("UBlox M8P/F9P"));
    }, Qt::QueuedConnection);
    connect(model, &ConfigGpsInjectViewModel::ubloxSurveyInRequested,
            page, [receiverUnavailable](int, double, bool) {
        receiverUnavailable(QStringLiteral("UBlox M8P/F9P"));
    }, Qt::QueuedConnection);
    connect(model, &ConfigGpsInjectViewModel::ubloxBasePositionRequested,
            page, [receiverUnavailable](double, double, double, int, double) {
        receiverUnavailable(QStringLiteral("UBlox M8P/F9P"));
    }, Qt::QueuedConnection);
    connect(model, &ConfigGpsInjectViewModel::septentrioConfigureRequested,
            page, [receiverUnavailable]() {
        receiverUnavailable(QStringLiteral("Septentrio"));
    }, Qt::QueuedConnection);
    connect(model, &ConfigGpsInjectViewModel::septentrioPositionRequested,
            page, [receiverUnavailable](bool, double, double, double) {
        receiverUnavailable(QStringLiteral("Septentrio"));
    }, Qt::QueuedConnection);
    connect(model, &ConfigGpsInjectViewModel::septentrioRtcmRequested,
            page, [receiverUnavailable](const QString &, double,
                                        bool, bool, bool, bool) {
        receiverUnavailable(QStringLiteral("Septentrio"));
    }, Qt::QueuedConnection);

    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName(QStringLiteral("ConfigGpsInjectPage"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    return scroll;
}

QWidget *SetupView::createGPSOrderPage(QWidget *parent)
{
    ParameterFirmwareFamily family = firmwareFamily(m_uas);
    ParameterMetaDataCatalog catalog =
        m_metadataRepository->catalog(
            family,
            m_officialFirmware ? m_firmwareVersion : QString());
    if (!catalog.isValid()
        && family != ParameterFirmwareFamily::Unknown
        && family != ParameterFirmwareFamily::ArduCopter) {
        catalog = m_metadataRepository->catalog(
            ParameterFirmwareFamily::ArduCopter);
    }

    auto *page = new ConfigGPSOrderView(catalog, parent);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);
    page->setConnected(m_connected);
    page->setArmed(m_uas && m_uas->isArmed());

    const int expectedUasId = m_uas ? m_uas->getUASID() : -1;
    connect(page, &ConfigGPSOrderView::writeRequested,
            page, [this, page, expectedUasId](
                      int componentId, const QString &name,
                      const QVariant &value) {
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId
            || !m_parameterManager) {
            page->parameterWriteFailed(componentId, name,
                                       tr("not connected"));
            return;
        }
        if (!m_parameterManager->getParameterNames(componentId)
                 .contains(name)) {
            page->parameterWriteFailed(componentId, name,
                                       tr("parameter unavailable"));
            return;
        }
        QVariant currentValue;
        if (m_parameterManager->getParameterValue(
                componentId, name, currentValue)
            && currentValue.toDouble() == value.toDouble()) {
            // Preserve the strict write/echo contract even when the requested
            // assignment already matches the parameter manager's cache.
            m_parameterManager->requestParameterUpdate(componentId, name);
            return;
        }
        m_parameterManager->setParameter(componentId, name, value);
    });
    connect(page, &ConfigGPSOrderView::refreshRequested,
            page, [this, page, expectedUasId](int componentId) {
        Q_UNUSED(componentId)
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId
            || !m_parameterManager) {
            page->refreshFailed(tr("not connected"));
            return;
        }
        if (m_parameterManager->parameterListInProgress()) {
            page->refreshFailed(tr("parameter refresh already in progress"));
            return;
        }
        m_parameterManager->requestParameterList();
    });
    if (m_uas) {
        connect(m_uas, &UASInterface::connected,
                page, [this, page, expectedUasId]() {
            page->setConnected(
                m_connected && m_uas
                && m_uas->getUASID() == expectedUasId);
        });
        connect(m_uas, &UASInterface::disconnected,
                page, [this, page, expectedUasId]() {
            page->setConnected(
                m_connected && m_uas
                && m_uas->getUASID() == expectedUasId);
        });
        connect(m_uas, QOverload<bool>::of(&UASInterface::armingChanged),
                page, &ConfigGPSOrderView::setArmed);
    }
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigGPSOrderView::parameterChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page, expectedUasId](bool ready) {
            if (ready && m_connected && m_uas
                && m_uas->getUASID() == expectedUasId
                && m_parameterManager
                && !page->viewModel()->HasPendingWrites()) {
                page->setParameterSnapshot(
                    parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
                    MAV_COMP_ID_AUTOPILOT1);
            }
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListLoadFailed,
                page, &ConfigGPSOrderView::refreshFailed);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListLoadCanceled,
                page, &ConfigGPSOrderView::refreshCanceled);
    }
    return page;
}

QWidget *SetupView::createBatteryMonitoring2Page(QWidget *parent)
{
    ParameterFirmwareFamily family = firmwareFamily(m_uas);
    ParameterMetaDataCatalog catalog =
        m_metadataRepository->catalog(
            family,
            m_officialFirmware ? m_firmwareVersion : QString());
    if (!catalog.isValid()
        && family != ParameterFirmwareFamily::Unknown
        && family != ParameterFirmwareFamily::ArduCopter) {
        catalog = m_metadataRepository->catalog(
            ParameterFirmwareFamily::ArduCopter);
    }
    LinkManager *const links = LinkManager::instance();
    VehicleTargetManager *const targets = links->vehicleTargetManager();
    const VehicleTargetLease expectedTarget = targets
        ? targets->acquireTarget() : VehicleTargetLease{};
    const int expectedComponent = expectedTarget.isValid()
        ? expectedTarget.endpoint.componentId : MAV_COMP_ID_AUTOPILOT1;
    auto *page = new ConfigBatteryMonitoring2View(catalog);
    page->setParameterSnapshot(
        parameterSnapshot(expectedComponent), expectedComponent);
    const QPointer<UASInterface> expectedUas(m_uas);
    const QPointer<QGCUASParamManager> expectedManager(m_parameterManager);
    const QPointer<LinkInterface> expectedLink(
        expectedTarget.isValid()
            ? links->getLink(expectedTarget.endpoint.linkId) : nullptr);

    const auto targetIsCurrent =
        [this, targets, expectedTarget, expectedUas,
         expectedManager, expectedLink]() {
        return expectedTarget.isValid() && targets && expectedUas
            && expectedManager && expectedLink
            && m_uas == expectedUas
            && m_parameterManager == expectedManager
            && expectedUas->getUASID()
                == expectedTarget.endpoint.systemId
            && targets->isCurrentTarget(
                expectedTarget.endpoint.linkId,
                expectedTarget.endpoint.systemId,
                expectedTarget.endpoint.componentId,
                expectedTarget.generation);
    };
    const auto syncConnected =
        [page, targetIsCurrent, expectedLink]() {
        page->setConnected(
            targetIsCurrent() && expectedLink
            && expectedLink->isConnected());
    };
    syncConnected();

    connect(this, &SetupView::connectionStateChanged,
            page, [syncConnected](bool) { syncConnected(); });
    if (expectedLink) {
        connect(expectedLink,
                QOverload<bool>::of(&LinkInterface::connected),
                page, [syncConnected](bool) { syncConnected(); });
    }

    connect(page, &ConfigBatteryMonitoring2View::writeRequested,
            page,
            [this, page, targetIsCurrent, expectedTarget,
             expectedManager, expectedLink](
                int componentId, const QString &name,
                const QVariant &value) {
        const auto reject = [page, componentId, name](
                                const QString &reason) {
            page->parameterWriteSubmissionFailed(
                componentId, name, reason);
        };
        if (!m_connected || !targetIsCurrent()
            || !expectedLink || !expectedLink->isConnected()
            || componentId != expectedTarget.endpoint.componentId) {
            reject(tr("not connected to the selected target"));
            return;
        }
        if (!expectedManager->getParameterNames(componentId)
                 .contains(name)) {
            reject(tr("parameter unavailable"));
            return;
        }
        const QVariantList changes{QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("value"), value}
        }};
        const QPointer<ConfigBatteryMonitoring2View> guard(page);
        const qulonglong batchId = expectedManager->writeParameters(
            componentId, changes);
        if (!guard) {
            return;
        }
        if (batchId == 0) {
            guard->parameterWriteSubmissionFailed(
                componentId, name,
                tr("write was rejected for the selected target"));
            return;
        }
        guard->parameterBatchSubmitted(componentId, name, batchId);
    });

    if (expectedUas) {
        connect(expectedUas, &UASInterface::mavlinkMessageRecieved,
                page,
                [page, targetIsCurrent, expectedTarget, expectedLink](
                    LinkInterface *incomingLink,
                    const mavlink_message_t &message) {
            if (targetIsCurrent() && expectedLink
                && incomingLink == expectedLink
                && incomingLink->getId()
                    == expectedTarget.endpoint.linkId
                && message.sysid
                    == expectedTarget.endpoint.systemId) {
                page->observeMavlinkMessage(message);
            }
        });
    }

    if (expectedManager) {
        connect(expectedManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigBatteryMonitoring2View::parameterChanged);
        connect(expectedManager,
                &QGCUASParamManager::parameterWriteAcknowledged,
                page,
                &ConfigBatteryMonitoring2View::parameterWriteAcknowledged);
        connect(expectedManager,
                &QGCUASParamManager::parameterWriteFailed,
                page, &ConfigBatteryMonitoring2View::parameterWriteFailed);
        connect(expectedManager,
                &QGCUASParamManager::parameterWriteCancelled,
                page,
                &ConfigBatteryMonitoring2View::parameterWriteCancelled);
        connect(expectedManager,
                &QGCUASParamManager::parameterBatchCompleted,
                page,
                &ConfigBatteryMonitoring2View::parameterBatchCompleted);
        connect(expectedManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page,
                [this, page, targetIsCurrent, expectedComponent](bool ready) {
            if (ready && targetIsCurrent()
                && !page->viewModel()->HasPendingWrite()) {
                page->setParameterSnapshot(
                    parameterSnapshot(expectedComponent),
                    expectedComponent);
            }
        });
    }

    return scrollablePage(page, kBatteryMonitor2, parent);
}

LinkInterface *SetupView::selectDroneCanLink() const
{
    LinkInterface *lastPrimary = m_droneCanLastPrimaryLink.data();
    if (lastPrimary && lastPrimary->isConnected()) {
        return lastPrimary;
    }
    if (!m_uas || !m_uas->getLinks()) {
        return nullptr;
    }
    for (LinkInterface *link : *m_uas->getLinks()) {
        if (link && link->isConnected()) {
            return link;
        }
    }
    return nullptr;
}

bool SetupView::bindDroneCanTransport()
{
    if (!m_droneCanBroker || !m_droneCanTransport || !m_uas) {
        return false;
    }
    if (m_droneCanBroker->owns(m_droneCanLease)) {
        LinkInterface *const pinnedLink = m_droneCanTransport->pinnedLink();
        if (m_droneCanTransport->pinnedUas() == m_uas && pinnedLink
            && pinnedLink->isConnected()
            && m_droneCanLease.sessionGeneration
                == m_droneCanTransport->generation()) {
            // Never migrate a live lease just because another link most
            // recently delivered an autopilot packet. A bus can only move
            // after the old lease explicitly stops forwarding.
            return true;
        }
        m_droneCanTransport->unbindEndpoint(
            true, m_droneCanLease.sessionGeneration);
        return false;
    }
    LinkInterface *link = selectDroneCanLink();
    if (!link || !link->isConnected()) {
        return false;
    }
    return m_droneCanTransport->bindEndpoint(
        m_uas, link, QDateTime::currentMSecsSinceEpoch()) != 0;
}

QWidget *SetupView::createDroneCanPage(QWidget *parent)
{
    auto *page = new ConfigDroneCanView(parent);
    auto *nodeInfoClient = new DroneCanGetNodeInfoClient(page);
    auto *getSetClient = new DroneCanGetSetClient(page);
    auto *nodeInfoTimer = new QTimer(nodeInfoClient);
    nodeInfoTimer->setObjectName(
        QStringLiteral("droneCanGetNodeInfoTimer"));
    nodeInfoTimer->setInterval(100);
    connect(nodeInfoTimer, &QTimer::timeout, nodeInfoClient,
            [nodeInfoClient]() {
        nodeInfoClient->tick(QDateTime::currentMSecsSinceEpoch());
    });
    auto *getSetTimer = new QTimer(getSetClient);
    getSetTimer->setObjectName(QStringLiteral("droneCanGetSetTimer"));
    getSetTimer->setInterval(100);
    connect(getSetTimer, &QTimer::timeout, getSetClient,
            [getSetClient]() {
        getSetClient->tick(QDateTime::currentMSecsSinceEpoch());
    });
    m_droneCanPage = page;
    m_droneCanLease = {};
    page->setVehicleConnected(m_connected);
    page->setVehicleArmed(m_uas && m_uas->isArmed());

    const auto bindNodeInfoSession =
        [this, nodeInfoClient]() -> bool {
        if (!m_droneCanBroker
            || !m_droneCanBroker->confirmed(m_droneCanLease)) {
            return false;
        }
        return nodeInfoClient->bindSession({
            m_droneCanLease.sessionGeneration,
            m_droneCanLease.busIndex,
            127
        });
    };
    const auto bindGetSetSession =
        [this, getSetClient]() -> bool {
        if (!m_droneCanBroker
            || !m_droneCanBroker->confirmed(m_droneCanLease)) {
            return false;
        }
        return getSetClient->bindSession({
            m_droneCanLease.sessionGeneration,
            m_droneCanLease.busIndex,
            127
        });
    };
    const auto requestNodeInfo =
        [bindNodeInfoSession, nodeInfoClient, nodeInfoTimer](
            int nodeId, bool preferCanFd) {
        if (bindNodeInfoSession()
            && nodeInfoClient->requestNodeInfo(
                nodeId, preferCanFd,
                QDateTime::currentMSecsSinceEpoch())) {
            nodeInfoTimer->start();
        }
    };
    connect(page->viewModel(),
            &ConfigDroneCanViewModel::nodeInfoRequested,
            page, requestNodeInfo);
    connect(page->viewModel(),
            &ConfigDroneCanViewModel::parameterReadRequested,
            page, [page, bindGetSetSession, getSetClient, getSetTimer](
                      int nodeId, quint16 index, bool preferCanFd) {
        if (!bindGetSetSession()) {
            page->viewModel()->parameterRequestFailed(
                nodeId, tr("DroneCAN forwarding session is not ready"));
            return;
        }
        if (getSetClient->getByIndex(
                nodeId, index, preferCanFd,
                QDateTime::currentMSecsSinceEpoch())) {
            getSetTimer->start();
        } else if (page->viewModel()->IsReadingParameters()) {
            page->viewModel()->parameterRequestFailed(
                nodeId, tr("unable to start the GetSet request"));
        }
    });
    connect(page->viewModel(),
            &ConfigDroneCanViewModel::parameterReadCancelRequested,
            page, [getSetClient, getSetTimer]() {
        getSetTimer->stop();
        getSetClient->cancelPendingRequest();
    });
    connect(page->viewModel(),
            &ConfigDroneCanViewModel::discoveryEpochReset,
            page, [nodeInfoClient, nodeInfoTimer,
                   getSetClient, getSetTimer]() {
        nodeInfoTimer->stop();
        getSetTimer->stop();
        // Refresh remains in the same broker generation. Preserve the
        // transfer-ID sequence so late responses from the old discovery
        // epoch cannot match a newly issued request.
        nodeInfoClient->clearPendingRequests();
        getSetClient->clearPendingRequest();
    });
    connect(nodeInfoClient,
            &DroneCanGetNodeInfoClient::transmitRequested,
            page, [this](quint32 canId, const QByteArray &data,
                         bool canFd) {
        if (m_droneCanBroker) {
            m_droneCanBroker->transmitFrame(
                m_droneCanLease, canId, data, canFd);
        }
    });
    connect(nodeInfoClient,
            &DroneCanGetNodeInfoClient::nodeInfoReceived,
            page->viewModel(),
            &ConfigDroneCanViewModel::observeNodeInfo);
    connect(nodeInfoClient,
            &DroneCanGetNodeInfoClient::requestFailed,
            page->viewModel(),
            &ConfigDroneCanViewModel::nodeInfoRequestFailed);
    connect(nodeInfoClient,
            &DroneCanGetNodeInfoClient::nodeInfoReceived,
            nodeInfoTimer, [nodeInfoClient, nodeInfoTimer]() {
        if (nodeInfoClient->pendingRequestCount() == 0) {
            nodeInfoTimer->stop();
        }
    });
    connect(getSetClient,
            &DroneCanGetSetClient::transmitRequested,
            page, [this, getSetClient](quint32 canId,
                                      const QByteArray &data,
                                      bool canFd) {
        if (!m_droneCanBroker
            || !m_droneCanBroker->transmitFrame(
                m_droneCanLease, canId, data, canFd)) {
            const QString reason = m_droneCanBroker
                    && !m_droneCanBroker->lastError().isEmpty()
                ? m_droneCanBroker->lastError()
                : tr("DroneCAN frame transport is unavailable");
            // This callback is synchronous. cancelPendingRequest() also
            // prevents DroneCanGetSetClient from sending the rest of a
            // multi-frame request after the broker invalidates its lease.
            getSetClient->cancelPendingRequest(reason);
        }
    });
    connect(getSetClient, &DroneCanGetSetClient::parameterReceived,
            page->viewModel(),
            &ConfigDroneCanViewModel::observeParameter);
    connect(getSetClient, &DroneCanGetSetClient::requestFailed,
            page->viewModel(),
            &ConfigDroneCanViewModel::parameterRequestFailed);
    connect(getSetClient, &DroneCanGetSetClient::requestCancelled,
            page->viewModel(),
            &ConfigDroneCanViewModel::parameterRequestCancelled);
    connect(getSetClient, &DroneCanGetSetClient::parameterReceived,
            getSetTimer, [getSetClient, getSetTimer]() {
        if (!getSetClient->hasPendingRequest()) {
            getSetTimer->stop();
        }
    });
    connect(getSetClient, &DroneCanGetSetClient::requestFailed,
            getSetTimer, [getSetTimer]() { getSetTimer->stop(); });
    connect(getSetClient, &DroneCanGetSetClient::requestCancelled,
            getSetTimer, [getSetTimer]() { getSetTimer->stop(); });
    connect(nodeInfoClient,
            &DroneCanGetNodeInfoClient::requestFailed,
            nodeInfoTimer, [nodeInfoClient, nodeInfoTimer]() {
        if (nodeInfoClient->pendingRequestCount() == 0) {
            nodeInfoTimer->stop();
        }
    });

    const int expectedUasId = m_uas ? m_uas->getUASID() : -1;
    connect(page, &ConfigDroneCanView::canForwardingRequested,
            page, [this, page, expectedUasId, bindNodeInfoSession,
                   bindGetSetSession, nodeInfoClient, nodeInfoTimer,
                   getSetClient, getSetTimer](
                      int componentId, int bus, bool enable) {
        if (!enable) {
            nodeInfoTimer->stop();
            getSetTimer->stop();
            nodeInfoClient->resetSession();
            getSetClient->resetSession();
            if (m_droneCanBroker
                && m_droneCanBroker->owns(m_droneCanLease)) {
                m_droneCanBroker->release(
                    m_droneCanLease,
                    QDateTime::currentMSecsSinceEpoch());
            }
            m_droneCanLease = {};
            return;
        }
        if (!m_uas || m_uas->getUASID() != expectedUasId
            || componentId != MAV_COMP_ID_AUTOPILOT1
            || bus < 1 || bus > 2) {
            if (enable) {
                page->forwardingSendFailed(tr("vehicle target changed"));
            }
            return;
        }
        if (!bindDroneCanTransport()) {
            page->forwardingSendFailed(
                m_droneCanBroker && !m_droneCanBroker->lastError().isEmpty()
                    ? m_droneCanBroker->lastError()
                    : tr("no connected link"));
            return;
        }
        if (m_droneCanBroker->owns(m_droneCanLease)) {
            m_droneCanBroker->tick(QDateTime::currentMSecsSinceEpoch());
            return;
        }
        m_droneCanLease = m_droneCanBroker->acquire(
            page, bus - 1, QDateTime::currentMSecsSinceEpoch());
        if (!m_droneCanBroker->owns(m_droneCanLease)) {
            m_droneCanLease = {};
            page->forwardingSendFailed(
                m_droneCanBroker->lastError().isEmpty()
                    ? tr("unable to acquire the DroneCAN transport")
                    : m_droneCanBroker->lastError());
        } else if (m_droneCanBroker->confirmed(m_droneCanLease)) {
            bindNodeInfoSession();
            bindGetSetSession();
            page->forwardingAckReceived(0);
        }
    });
    connect(m_droneCanBroker, &DroneCanForwardingBroker::stateChanged,
            page, [this, page, bindNodeInfoSession, bindGetSetSession,
                   requestNodeInfo, nodeInfoClient, nodeInfoTimer,
                   getSetClient, getSetTimer]() {
        if (!m_droneCanLease.isValid()) {
            nodeInfoTimer->stop();
            getSetTimer->stop();
            nodeInfoClient->resetSession();
            getSetClient->resetSession();
            return;
        }
        if (!m_droneCanBroker->owns(m_droneCanLease)) {
            nodeInfoTimer->stop();
            getSetTimer->stop();
            nodeInfoClient->resetSession();
            getSetClient->resetSession();
            m_droneCanLease = {};
            if (page->viewModel()->IsConnected()) {
                page->forwardingSendFailed(
                    m_droneCanBroker->lastError().isEmpty()
                        ? tr("DroneCAN transport was closed")
                        : m_droneCanBroker->lastError());
            }
            return;
        }
        if (m_droneCanBroker->confirmed(m_droneCanLease)) {
            bindNodeInfoSession();
            bindGetSetSession();
            if (page->viewModel()->IsBusy()) {
                page->forwardingAckReceived(0);
            }
            for (const DroneCanNode &node : page->viewModel()->Nodes()) {
                if (node.name == QStringLiteral("—")) {
                    requestNodeInfo(node.id, false);
                }
            }
        }
    });
    connect(m_droneCanBroker, &DroneCanForwardingBroker::frameAccepted,
            page, [this, page, nodeInfoClient, getSetClient](
                      quint64 sessionGeneration, int busIndex,
                      quint32 canId, const QByteArray &data,
                      bool canFd, qint64 nowMs) {
        if (m_droneCanBroker->owns(m_droneCanLease)
            && m_droneCanLease.sessionGeneration == sessionGeneration) {
            nodeInfoClient->acceptFrame(
                sessionGeneration, busIndex, canId, data, canFd, nowMs);
            getSetClient->acceptFrame(
                sessionGeneration, busIndex, canId, data, canFd, nowMs);
            page->canFrameReceived(busIndex, canId, data, canFd, nowMs);
        }
    });
    connect(this, &SetupView::connectionStateChanged,
            page, &ConfigDroneCanView::setVehicleConnected);
    if (m_uas) {
        connect(m_uas, QOverload<bool>::of(&UASInterface::armingChanged),
                page, &ConfigDroneCanView::setVehicleArmed);
    }
    connect(page, &QObject::destroyed, this, [this, page]() {
        if (m_droneCanPage.data() == page) {
            m_droneCanPage = nullptr;
            m_droneCanLease = {};
        }
    });

    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName(QStringLiteral("ConfigDroneCanPage"));
    scroll->setWidgetResizable(true);
    scroll->setWidget(page);
    return scroll;
}

QWidget *SetupView::createHWCANPage(QWidget *parent)
{
    ParameterFirmwareFamily family = firmwareFamily(m_uas);
    ParameterMetaDataCatalog catalog =
        m_metadataRepository->catalog(
            family,
            m_officialFirmware ? m_firmwareVersion : QString());
    if (!catalog.isValid()
        && family != ParameterFirmwareFamily::Unknown
        && family != ParameterFirmwareFamily::ArduCopter) {
        catalog = m_metadataRepository->catalog(
            ParameterFirmwareFamily::ArduCopter);
    }

    auto *page = new ConfigHWCANView(catalog, parent);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);
    page->setConnected(m_connected);
    page->setArmed(m_uas && m_uas->isArmed());

    const int expectedUasId = m_uas ? m_uas->getUASID() : -1;
    connect(page, &ConfigHWCANView::writeRequested,
            page, [this, page, expectedUasId](
                      int componentId, const QString &name,
                      const QVariant &value) {
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId
            || !m_parameterManager) {
            page->parameterWriteFailed(componentId, name,
                                       tr("not connected"));
            return;
        }
        if (m_uas->isArmed()) {
            page->parameterWriteFailed(componentId, name,
                                       tr("vehicle armed"));
            return;
        }
        if (!m_parameterManager->getParameterNames(componentId)
                 .contains(name)) {
            page->parameterWriteFailed(componentId, name,
                                       tr("parameter unavailable"));
            return;
        }
        QVariant currentValue;
        if (m_parameterManager->getParameterValue(
                componentId, name, currentValue)
            && qAbs(currentValue.toDouble() - value.toDouble())
                <= 1.0e-6) {
            m_parameterManager->requestParameterUpdate(componentId, name);
            return;
        }
        m_parameterManager->setParameter(componentId, name, value);
    });
    connect(page, &ConfigHWCANView::commandRequested,
            page, [this, page, expectedUasId](
                      int componentId, int command,
                      float param1, float param2) {
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId) {
            page->commandSendFailed(componentId, command,
                                    tr("not connected"));
            return;
        }
        if (m_uas->isArmed()) {
            page->commandSendFailed(componentId, command,
                                    tr("vehicle armed"));
            return;
        }
        LinkInterface *commandLink = nullptr;
        if (m_uas->getLinks()) {
            for (LinkInterface *link : *m_uas->getLinks()) {
                if (link && link->isConnected()) {
                    commandLink = link;
                    break;
                }
            }
        }
        if (!m_uas->executeCommandOnLink(
                commandLink, static_cast<MAV_CMD>(command), 0,
                param1, param2, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                componentId)) {
            page->commandSendFailed(componentId, command,
                                    tr("no connected link"));
        }
    });
    if (m_uas) {
        connect(m_uas, &UASInterface::commandAckReceived,
                page, [this, page, expectedUasId](
                          int uasId, int componentId, int command,
                          int result, int progress, int resultParam2,
                          int targetSystem, int targetComponent) {
            Q_UNUSED(progress)
            Q_UNUSED(resultParam2)
            if (uasId == expectedUasId && m_uas
                && (targetSystem == 0
                    || targetSystem == m_uas->getSystemId())
                && (targetComponent == 0
                    || targetComponent == m_uas->getComponentId())) {
                page->commandAckReceived(componentId, command, result);
            }
        });
        connect(m_uas, &UASInterface::connected,
                page, [this, page, expectedUasId]() {
            page->setConnected(
                m_connected && m_uas
                && m_uas->getUASID() == expectedUasId);
        });
        connect(m_uas, &UASInterface::disconnected,
                page, [this, page, expectedUasId]() {
            page->setConnected(
                m_connected && m_uas
                && m_uas->getUASID() == expectedUasId);
        });
        connect(m_uas, QOverload<bool>::of(&UASInterface::armingChanged),
                page, &ConfigHWCANView::setArmed);
    }
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigHWCANView::parameterChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page, expectedUasId](bool ready) {
            if (ready && m_connected && m_uas
                && m_uas->getUASID() == expectedUasId
                && m_parameterManager && !page->viewModel()->Busy()) {
                page->setParameterSnapshot(
                    parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
                    MAV_COMP_ID_AUTOPILOT1);
            }
        });
    }

    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName(QStringLiteral("ConfigHWCANPage"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    return scroll;
}

QWidget *SetupView::createEscCalibrationPage(QWidget *parent)
{
    ParameterFirmwareFamily family = firmwareFamily(m_uas);
    ParameterMetaDataCatalog catalog =
        m_metadataRepository->catalog(
            family,
            m_officialFirmware ? m_firmwareVersion : QString());
    if (!catalog.isValid()
        && family != ParameterFirmwareFamily::Unknown
        && family != ParameterFirmwareFamily::ArduCopter) {
        catalog = m_metadataRepository->catalog(
            ParameterFirmwareFamily::ArduCopter);
    }

    auto *page = new ConfigESCCalibrationView(catalog, parent);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);
    page->setConnected(m_connected);
    page->setArmed(m_uas && m_uas->isArmed());

    const int expectedUasId = m_uas ? m_uas->getUASID() : -1;
    connect(page, &ConfigESCCalibrationView::writeRequested,
            page, [this, page, expectedUasId](
                      int componentId, const QString &name,
                      const QVariant &value) {
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId
            || !m_parameterManager) {
            page->parameterWriteFailed(componentId, name,
                                       tr("not connected"));
            return;
        }
        if (m_uas->isArmed()) {
            page->parameterWriteFailed(componentId, name,
                                       tr("vehicle armed"));
            return;
        }
        if (!m_parameterManager->getParameterNames(componentId)
                 .contains(name)) {
            page->parameterWriteFailed(componentId, name,
                                       tr("parameter unavailable"));
            return;
        }
        if (name == QLatin1String("ESC_CALIBRATION")) {
            QVariant currentValue;
            if (m_parameterManager->getParameterValue(
                    componentId, name, currentValue)
                && currentValue.toInt() == value.toInt()) {
                // QGCParamWidget intentionally drops unchanged writes. Ask
                // the vehicle for a fresh value so the model still requires
                // a live echo before declaring calibration successful.
                m_parameterManager->requestParameterUpdate(
                    componentId, name);
                return;
            }
        }
        m_parameterManager->setParameter(componentId, name, value);
    });
    connect(page, &ConfigESCCalibrationView::refreshRequested,
            page, [this, page, expectedUasId](int componentId) {
        Q_UNUSED(componentId)
        if (!m_connected || !m_uas
            || m_uas->getUASID() != expectedUasId
            || !m_parameterManager) {
            page->refreshFailed(tr("not connected"));
            return;
        }
        if (m_parameterManager->parameterListInProgress()) {
            page->refreshFailed(tr("parameter refresh already in progress"));
            return;
        }
        m_parameterManager->requestParameterList();
    });
    if (m_uas) {
        connect(m_uas, QOverload<bool>::of(&UASInterface::armingChanged),
                page, &ConfigESCCalibrationView::setArmed);
        connect(m_uas, &UASInterface::connected,
                page, [page]() { page->setConnected(true); });
        connect(m_uas, &UASInterface::disconnected,
                page, [page]() { page->setConnected(false); });
    }
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigESCCalibrationView::parameterChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page, expectedUasId](bool ready) {
            if (ready && m_connected && m_uas
                && m_uas->getUASID() == expectedUasId
                && m_parameterManager
                && !page->viewModel()->HasPendingWrites()) {
                page->setParameterSnapshot(
                    parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
                    MAV_COMP_ID_AUTOPILOT1);
            }
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListLoadFailed,
                page, &ConfigESCCalibrationView::refreshFailed);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListLoadCanceled,
                page, &ConfigESCCalibrationView::refreshCanceled);
    }
    return page;
}

QWidget *SetupView::createRadioOutputPage(QWidget *parent)
{
    ParameterFirmwareFamily family = firmwareFamily(m_uas);
    ParameterMetaDataCatalog catalog =
        m_metadataRepository->catalog(
            family,
            m_officialFirmware ? m_firmwareVersion : QString());
    if (!catalog.isValid()
        && family != ParameterFirmwareFamily::Unknown
        && family != ParameterFirmwareFamily::ArduCopter) {
        catalog = m_metadataRepository->catalog(
            ParameterFirmwareFamily::ArduCopter);
    }

    auto *page = new ConfigRadioOutputView(catalog, parent);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);
    connect(page, &ConfigRadioOutputView::writeRequested,
            page, [this, page](int componentId, const QString &name,
                               const QVariant &value) {
        if (!m_connected || !m_parameterManager) {
            page->parameterWriteFailed(componentId, name,
                                       tr("not connected"));
            return;
        }
        if (!m_parameterManager->getParameterNames(componentId)
                 .contains(name)) {
            page->parameterWriteFailed(componentId, name,
                                       tr("parameter unavailable"));
            return;
        }
        m_parameterManager->setParameter(componentId, name, value);
    });
    if (m_uas) {
        connect(m_uas, &UASInterface::servoOutputChanged,
                page, &ConfigRadioOutputView::servoOutputChanged);
    }
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigRadioOutputView::parameterChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page](bool ready) {
            if (ready && m_uas && m_parameterManager) {
                page->setParameterSnapshot(
                    parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
                    MAV_COMP_ID_AUTOPILOT1);
            }
        });
    }
    return page;
}

QWidget *SetupView::createSerialPage(QWidget *parent)
{
    ParameterFirmwareFamily family = firmwareFamily(m_uas);
    ParameterMetaDataCatalog catalog =
        m_metadataRepository->catalog(
            family,
            m_officialFirmware ? m_firmwareVersion : QString());
    if (!catalog.isValid()
        && family != ParameterFirmwareFamily::Unknown
        && family != ParameterFirmwareFamily::ArduCopter) {
        catalog = m_metadataRepository->catalog(
            ParameterFirmwareFamily::ArduCopter);
    }

    auto *page = new ConfigSerialView(catalog);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);
    connect(page, &ConfigSerialView::writeRequested,
            page, [this, page](int componentId, const QString &name,
                               const QVariant &value) {
        if (!m_connected || !m_parameterManager) {
            page->parameterWriteFailed(componentId, name,
                                       tr("not connected"));
            return;
        }
        if (!m_parameterManager->getParameterNames(componentId)
                 .contains(name)) {
            page->parameterWriteFailed(componentId, name,
                                       tr("parameter unavailable"));
            return;
        }
        m_parameterManager->setParameter(componentId, name, value);
    });
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigSerialView::parameterChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page](bool ready) {
            if (ready && m_uas && m_parameterManager) {
                page->setParameterSnapshot(
                    parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
                    MAV_COMP_ID_AUTOPILOT1);
            }
        });
    }

    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName(kSerialPorts);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    return scroll;
}

QWidget *SetupView::createInitialParamsPage(QWidget *parent)
{
    const ParameterFirmwareFamily family = firmwareFamily(m_uas);
    int firmwareMajor = 0;
    if (auto *apm = qobject_cast<ArduPilotMegaMAV *>(m_uas.data())) {
        if (apm->getFirmwareVersion().isValid()) {
            firmwareMajor = apm->getFirmwareVersion().majorNumber();
        }
    }

    auto *page = new ConfigInitialParamsView;
    page->setVehicleContext(
        family == ParameterFirmwareFamily::ArduPlane, firmwareMajor);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);
    connect(page, &ConfigInitialParamsView::writeToFcRequested,
            page, [this, page]() {
        const QStringList available = m_parameterManager
            ? m_parameterManager->getParameterNames(
                  MAV_COMP_ID_AUTOPILOT1)
            : QStringList();
        page->WriteToFc(available,
                        m_connected && m_parameterManager);
    });
    connect(page, &ConfigInitialParamsView::writeRequested,
            page, [this, page](int componentId, const QString &name,
                               const QVariant &value) {
        if (!m_connected || !m_parameterManager) {
            page->parameterWriteFailed(componentId, name,
                                       tr("not connected"));
            return;
        }
        if (!m_parameterManager->getParameterNames(componentId)
                 .contains(name)) {
            page->parameterWriteFailed(componentId, name,
                                       tr("parameter unavailable"));
            return;
        }
        m_parameterManager->setParameter(componentId, name, value);
    });
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigInitialParamsView::parameterChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page](bool ready) {
            if (ready && m_uas && m_parameterManager
                && !page->viewModel()->Writing()) {
                page->setParameterSnapshot(
                    parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
                    MAV_COMP_ID_AUTOPILOT1);
            }
        });
    }

    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName(kInitialParams);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    return scroll;
}

QList<ConfigFriendlyParameterValue> SetupView::parameterSnapshot(
    int componentId) const
{
    QList<ConfigFriendlyParameterValue> result;
    if (!m_parameterManager) {
        return result;
    }
    const QList<QString> names =
        m_parameterManager->getParameterNames(componentId);
    for (const QString &name : names) {
        QVariant value;
        if (m_parameterManager->getParameterValue(componentId, name, value)) {
            result.append({componentId, name, value});
        }
    }
    return result;
}

void SetupView::resetParameterProgress()
{
    m_parametersReady = false;
    m_parameterLoadFailure.clear();
    m_parameterLoadingCanceled = false;
    m_parameterRetryPending = false;
}

void SetupView::refreshLoadingOverlay()
{
    const bool loading = BackstageView::shouldShowParameterLoading(
        m_connected, m_parametersReady,
        currentPageAllowsPartialParameters());
    int receivedTotal = 0;
    int expectedTotal = 0;
    if (m_parameterManager) {
        expectedTotal = m_parameterManager->parameterListReportedCount();
        receivedTotal = m_parameterManager->parameterListReceivedCount();
    }
    m_backstage->setParameterLoadingState(
        loading, receivedTotal, expectedTotal,
        m_parameterLoadingCanceled, m_parameterLoadFailure);
}
