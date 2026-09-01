#include "SetupView.h"

#include "AccelCalibrationConfig.h"
#include "AirspeedConfig.h"
#include "ApmCustomFirmwareConfig.h"
#include "ArduPilotMegaMAV.h"
#include "BatteryMonitorConfig.h"
#include "CameraGimbalConfig.h"
#include "CompassConfig.h"
#include "ConfigSerialView.h"
#include "FailSafeConfig.h"
#include "FlightModeConfig.h"
#include "FrameTypeConfig.h"
#include "LinkInterface.h"
#include "OpticalFlowConfig.h"
#include "OsdConfig.h"
#include "QGCUASParamManager.h"
#include "Radio3DRConfig.h"
#include "RadioCalibrationConfig.h"
#include "RangeFinderConfig.h"
#include "UASInterface.h"
#include "UASManager.h"
#include "APMFirmwareVersion.h"
#include "AppPaths.h"
#include "core/parameters/ParameterMetaDataRepository.h"
#include "ui/BackstageView.h"

#include <QDir>
#include <QFrame>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const QString kInstallFirmware = QStringLiteral("InstallFirmwareView");
const QString kMandatoryGroup = QStringLiteral("MandatoryHardwareGroup");
const QString kFrameType = QStringLiteral("ConfigFrameClassTypeView");
const QString kAccelCalibration = QStringLiteral("ConfigAccelCalibrationView");
const QString kCompass = QStringLiteral("ConfigCompassView");
const QString kRadioInput = QStringLiteral("ConfigRadioInputView");
const QString kSerialPorts = QStringLiteral("ConfigSerialView");
const QString kFlightModes = QStringLiteral("ConfigFlightModesView");
const QString kFailSafe = QStringLiteral("ConfigFailSafeView");
const QString kOptionalGroup = QStringLiteral("OptionalHardwareGroup");
const QString kSikRadio = QStringLiteral("SikRadioView");
const QString kBatteryMonitor = QStringLiteral("ConfigBatteryMonitoringView");
const QString kRangeFinder = QStringLiteral("ConfigRangeFinderView");
const QString kAirspeed = QStringLiteral("ConfigAirspeedView");
const QString kOpticalFlow = QStringLiteral("ConfigOptFlowView");
const QString kOsd = QStringLiteral("ConfigHWOSDView");
const QString kCameraGimbal = QStringLiteral("ConfigMountView");

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
              QStringLiteral("cache/parameter-metadata"))))
{
    setObjectName(QStringLiteral("SetupView"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_backstage);

    buildPages();
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

SetupView::~SetupView() = default;

void SetupView::buildPages()
{
    m_backstage->addPage(makeBackstagePage<ApmCustomFirmwareConfig>(
        kInstallFirmware, tr("Install Firmware")));

    m_backstage->addGroup(tr(">> Mandatory Hardware"), kMandatoryGroup);
    m_backstage->addPage(makeBackstagePage<FrameTypeConfig>(
        kFrameType, tr("Frame Type"), true, true));
    m_backstage->addPage(makeBackstagePage<AccelCalibrationConfig>(
        kAccelCalibration, tr("Accel Calibration"), true, true));
    m_backstage->addPage(makeBackstagePage<CompassConfig>(
        kCompass, tr("Compass"), true, true));
    m_backstage->addPage(makeBackstagePage<RadioCalibrationConfig>(
        kRadioInput, tr("Radio Calibration"), true, true));
    BackstagePage serialPorts;
    serialPorts.id = kSerialPorts;
    serialPorts.header = tr("Serial Ports");
    serialPorts.isSub = true;
    serialPorts.requiresConnection = true;
    serialPorts.factory = [this](QWidget *parent) {
        return createSerialPage(parent);
    };
    m_backstage->addPage(serialPorts);
    m_backstage->addPage(makeBackstagePage<FlightModeConfig>(
        kFlightModes, tr("Flight Modes"), true, true));
    m_backstage->addPage(makeBackstagePage<FailSafeConfig>(
        kFailSafe, tr("FailSafe"), true, true));

    m_backstage->addGroup(tr(">> Optional Hardware"), kOptionalGroup);
    m_backstage->addPage(makeBackstagePage<Radio3DRConfig>(
        kSikRadio, tr("Sik Radio"), true));
    m_backstage->addPage(makeBackstagePage<BatteryMonitorConfig>(
        kBatteryMonitor, tr("Battery Monitor"), true, true));
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
        if (m_parameterManager != m_uas->getParamManager()) {
            parameterManagerChanged(m_uas->getParamManager());
        }
        syncConnectionState();
        return;
    }
    if (m_uas) {
        disconnect(m_uas, nullptr, this, nullptr);
    }
    bindParameterManager(nullptr);

    m_uas = uas;
    m_parameterManager = nullptr;
    m_firmwareVersion.clear();
    m_officialFirmware = false;
    resetParameterProgress();

    if (m_uas) {
        connect(m_uas, &UASInterface::connected,
                this, &SetupView::vehicleConnected);
        connect(m_uas, &UASInterface::disconnected,
                this, &SetupView::vehicleDisconnected);
        connect(m_uas,
                QOverload<int, int, int, int, QString, QVariant>::of(
                    &UASInterface::parameterChanged),
                this, &SetupView::parameterChanged);
        connect(m_uas, &UASInterface::parameterManagerChanged,
                this, &SetupView::parameterManagerChanged);
        if (auto *apm = qobject_cast<ArduPilotMegaMAV *>(m_uas.data())) {
            connect(apm, &ArduPilotMegaMAV::versionDetected,
                    this, &SetupView::firmwareVersionDetected);
        }
        bindParameterManager(m_uas->getParamManager());
        m_parametersReady = m_parameterManager
            && m_parameterManager->parameterListReady();
    }

    m_connected = hasConnectedLink();
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

void SetupView::parameterChanged(int uas, int component, int parameterCount,
                                 int parameterId, QString parameterName,
                                 QVariant value)
{
    Q_UNUSED(parameterName)
    Q_UNUSED(value)
    if (!m_connected || !m_uas || uas != m_uas->getUASID()
        || parameterId == UINT16_MAX || parameterCount <= 0) {
        return;
    }
    if (m_expectedParameterCounts.value(component) != parameterCount) {
        m_receivedParameterIds[component].clear();
        m_expectedParameterCounts[component] = parameterCount;
    }
    m_receivedParameterIds[component].insert(parameterId);
    int receivedTotal = 0;
    int expectedTotal = 0;
    for (auto iterator = m_expectedParameterCounts.constBegin();
         iterator != m_expectedParameterCounts.constEnd(); ++iterator) {
        expectedTotal += iterator.value();
        receivedTotal += qMin(iterator.value(),
                              m_receivedParameterIds.value(iterator.key()).size());
    }
    m_parameterProgress = expectedTotal > 0
        ? qBound(0, qRound(100.0 * receivedTotal / expectedTotal), 100)
        : -1;
    refreshLoadingOverlay();
}

void SetupView::parameterListUpToDate(int component)
{
    Q_UNUSED(component)
    m_parametersReady = true;
    m_parameterLoadFailure.clear();
    m_parameterProgress = 100;
    refreshLoadingOverlay();
}

void SetupView::parameterListReadyChanged(bool ready)
{
    m_parametersReady = ready;
    if (ready) {
        m_parameterLoadFailure.clear();
        m_parameterProgress = 100;
    }
    refreshLoadingOverlay();
}

void SetupView::parameterListLoadFailed(const QString &reason)
{
    m_parametersReady = false;
    m_parameterLoadFailure = reason;
    refreshLoadingOverlay();
}

void SetupView::parameterListLoadCanceled()
{
    m_parametersReady = false;
    int receivedTotal = 0;
    int expectedTotal = 0;
    for (auto iterator = m_expectedParameterCounts.constBegin();
         iterator != m_expectedParameterCounts.constEnd(); ++iterator) {
        expectedTotal += iterator.value();
        receivedTotal += qMin(iterator.value(),
                              m_receivedParameterIds.value(iterator.key()).size());
    }
    m_parameterLoadFailure = tr(
        "Parameter loading stopped at %1/%2. Retry Now is required before "
        "opening configuration pages.")
        .arg(receivedTotal)
        .arg(expectedTotal > 0 ? QString::number(expectedTotal)
                               : tr("unknown"));
    refreshLoadingOverlay();
}

void SetupView::parameterManagerChanged(QGCUASParamManager *manager)
{
    bindParameterManager(manager);
    resetParameterProgress();
    refreshPageVisibility();
    resetConnectionPages();

    const QPointer<QGCUASParamManager> expectedManager(manager);
    QTimer::singleShot(0, this, [this, expectedManager]() {
        if (!expectedManager || m_parameterManager != expectedManager) {
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
    if (m_parameterManager) {
        m_parameterManager->cancelParameterList();
    } else {
        parameterListLoadCanceled();
    }
}

void SetupView::retryParameterLoading()
{
    resetParameterProgress();
    if (m_parameterManager) {
        m_parameterManager->requestParameterList();
    }
    refreshLoadingOverlay();
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

    const bool selected = m_backstage->currentPageId() == kSerialPorts;
    m_backstage->resetPage(kSerialPorts);
    if (selected && m_connected
        && m_backstage->isPageVisible(kSerialPorts)) {
        m_backstage->setCurrentPage(kSerialPorts);
    }
}

void SetupView::refreshPageVisibility()
{
    const bool copter = m_connected && m_uas && m_uas->isMultirotor();

    m_backstage->setGroupVisible(kMandatoryGroup, m_connected);
    m_backstage->setPageVisible(kFrameType, copter);
    m_backstage->setPageVisible(kAccelCalibration, m_connected);
    m_backstage->setPageVisible(kCompass, m_connected);
    m_backstage->setPageVisible(kRadioInput, m_connected);
    m_backstage->setPageVisible(
        kSerialPorts,
        m_connected
            && firmwareFamily(m_uas) != ParameterFirmwareFamily::Unknown);
    m_backstage->setPageVisible(kFlightModes, m_connected);
    m_backstage->setPageVisible(kFailSafe, m_connected);

    m_backstage->setGroupVisible(kOptionalGroup, true);
    m_backstage->setPageVisible(kSikRadio, true);
    m_backstage->setPageVisible(kBatteryMonitor, m_connected);
    m_backstage->setPageVisible(kRangeFinder, m_connected);
    m_backstage->setPageVisible(kAirspeed, m_connected);
    m_backstage->setPageVisible(kOpticalFlow, m_connected);
    m_backstage->setPageVisible(kOsd, m_connected);
    m_backstage->setPageVisible(kCameraGimbal, m_connected);
    refreshLoadingOverlay();
}

bool SetupView::currentPageRequiresParameters() const
{
    const BackstagePage definition =
        m_backstage->pageDefinition(m_backstage->currentPageId());
    return definition.requiresConnection
        && !definition.allowsPartialParameters;
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
            &QGCUASParamManager::parameterListReadyChanged,
            this, &SetupView::parameterListReadyChanged);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadFailed,
            this, &SetupView::parameterListLoadFailed);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadCanceled,
            this, &SetupView::parameterListLoadCanceled);
}

void SetupView::resetConnectionPages()
{
    const QString selectedPage = m_backstage->currentPageId();
    for (const QString &pageId : m_backstage->pageIds()) {
        if (m_backstage->pageDefinition(pageId).requiresConnection) {
            m_backstage->resetPage(pageId);
        }
    }
    if (m_connected && m_backstage->isPageVisible(selectedPage)) {
        m_backstage->setCurrentPage(selectedPage);
    }
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
    if (m_uas) {
        const int expectedUasId = m_uas->getUASID();
        connect(m_uas,
                QOverload<int, int, QString, QVariant>::of(
                    &UASInterface::parameterChanged),
                page, [page, expectedUasId](int uasId, int componentId,
                                            const QString &name,
                                            const QVariant &value) {
            if (uasId == expectedUasId) {
                page->parameterChanged(componentId, name, value);
            }
        });
    }
    if (m_parameterManager) {
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
    m_parameterProgress = -1;
    m_receivedParameterIds.clear();
    m_expectedParameterCounts.clear();
}

void SetupView::refreshLoadingOverlay()
{
    const bool loading = m_connected && currentPageRequiresParameters()
        && !m_parametersReady;
    int receivedTotal = 0;
    int expectedTotal = 0;
    for (auto iterator = m_expectedParameterCounts.constBegin();
         iterator != m_expectedParameterCounts.constEnd(); ++iterator) {
        expectedTotal += iterator.value();
        receivedTotal += qMin(iterator.value(),
                              m_receivedParameterIds.value(iterator.key()).size());
    }
    const QString message = !m_parameterLoadFailure.isEmpty()
        ? m_parameterLoadFailure
        : (expectedTotal > 0
               ? tr("Loading parameters… %1/%2")
                     .arg(receivedTotal).arg(expectedTotal)
               : tr("Waiting for vehicle parameters…"));
    m_backstage->setLoading(loading, message, m_parameterProgress);
}
