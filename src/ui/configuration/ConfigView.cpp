#include "ConfigView.h"

#include "AdvParameterList.h"
#include "ArduPlanePidConfig.h"
#include "ArduRoverPidConfig.h"
#include "BasicPidConfig.h"
#include "CopterPidConfig.h"
#include "ConfigPlannerAdvView.h"
#include "ConfigFriendlyParamsView.h"
#include "FlightModeConfig.h"
#include "GeoFenceConfig.h"
#include "LinkInterface.h"
#include "OsdConfig.h"
#include "QGCSettingsWidget.h"
#include "QGCUASParamManager.h"
#include "ArduPilotMegaMAV.h"
#include "UASInterface.h"
#include "UASManager.h"
#include "AppPaths.h"
#include "core/parameters/ParameterMetaDataRepository.h"
#include "core/parameters/ParameterMetaDataUpdater.h"
#include "logging.h"
#include "ui/BackstageView.h"

#include <QFrame>
#include <QDir>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace {
const QString kLastPageKey = QStringLiteral("config_lastpage");

const QString kFlightModes = QStringLiteral("ConfigFlightModesView");
const QString kStandardParams = QStringLiteral("ConfigFriendlyParamsView");
const QString kAdvancedParams = QStringLiteral("ConfigFriendlyParamsAdvView");
const QString kGeoFence = QStringLiteral("ConfigAC_FenceView");
const QString kBasicTuning = QStringLiteral("ConfigBasicTuningView");
const QString kPlaneTuning = QStringLiteral("ConfigArduplaneView");
const QString kRoverTuning = QStringLiteral("ConfigArduroverView");
const QString kExtendedTuning = QStringLiteral("ConfigExtendedTuningView");
const QString kOnboardOsd = QStringLiteral("ConfigOSDView");
const QString kFullParameterList = QStringLiteral("RawParamsView");
const QString kPlanner = QStringLiteral("ConfigPlannerView");
const QString kPlannerAdvanced = QStringLiteral("ConfigPlannerAdvView");

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
                                std::function<bool()> visibleWhen,
                                bool requiresConnection = true,
                                bool advanced = false,
                                bool allowsPartialParameters = false)
{
    BackstagePage definition;
    definition.id = id;
    definition.header = header;
    definition.visibleWhen = std::move(visibleWhen);
    definition.isAdvanced = advanced;
    definition.requiresConnection = requiresConnection;
    definition.allowsPartialParameters = allowsPartialParameters;
    definition.factory = [id](QWidget *parent) {
        return makePage<Page>(id, parent);
    };
    return definition;
}
}

ConfigView::ConfigView(QWidget *parent)
    : QWidget(parent),
      m_backstage(new BackstageView(this)),
      m_metadataRepository(new ParameterMetaDataRepository(
          AppPaths::resourcePath(QStringLiteral("files/ardupilotmega")),
          QDir(AppPaths::writableDataDirectory()).filePath(
              QStringLiteral("cache/parameter-metadata")))),
      m_metadataUpdater(new ParameterMetaDataUpdater(
          m_metadataRepository.get()))
{
    setObjectName(QStringLiteral("ConfigView"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_backstage);

    QSettings settings;
    m_advanced = settings.value(
        QStringLiteral("QGC_MAINWINDOW/ADVANCED_MODE"), false).toBool();
    m_preferredPageHeader = settings.value(kLastPageKey).toString();

    m_backstage->setAutomaticSelectionEnabled(false);
    buildPages();
    connect(m_backstage, &BackstageView::currentPageChanged,
            this, &ConfigView::currentPageChanged);
    connect(m_backstage, &BackstageView::stopLoadingRequested,
            this, &ConfigView::stopParameterLoading);
    connect(m_backstage, &BackstageView::retryLoadingRequested,
            this, &ConfigView::retryParameterLoading);
    connect(m_metadataUpdater.get(),
            &ParameterMetaDataUpdater::catalogUpdated,
            this, &ConfigView::parameterMetadataUpdated);
    connect(m_metadataUpdater.get(),
            &ParameterMetaDataUpdater::updateFailed,
            this, [this](ParameterFirmwareFamily family,
                         const QString &reason) {
        if (family == parameterFirmwareFamily()) {
            QLOG_WARN() << "Parameter metadata update failed:" << reason;
        }
    });
    connect(UASManager::instance(),
            QOverload<UASInterface *>::of(&UASManager::activeUASSet),
            this, &ConfigView::activeUASSet);
    activeUASSet(UASManager::instance()->getActiveUAS());
    m_backstage->restoreInitialPage(m_preferredPageHeader);
}

ConfigView::~ConfigView() = default;

void ConfigView::buildPages()
{
    const auto connected = [this]() { return m_connected; };
    const auto copter = [this]() {
        return m_connected && m_uas && m_uas->isMultirotor();
    };
    const auto plane = [this]() {
        return m_connected && m_uas && m_uas->isFixedWing();
    };
    const auto rover = [this]() {
        return m_connected && m_uas && m_uas->isGroundRover();
    };
    const auto always = []() { return true; };

    m_backstage->addPage(makeBackstagePage<FlightModeConfig>(
        kFlightModes, tr("Flight Modes"), connected));

    BackstagePage standardParameters;
    standardParameters.id = kStandardParams;
    standardParameters.header = tr("Standard Params");
    standardParameters.requiresConnection = true;
    standardParameters.visibleWhen = [this]() {
        return m_connected && friendlyParametersSupported();
    };
    standardParameters.factory = [this](QWidget *parent) {
        return createFriendlyParamsPage(false, parent);
    };
    m_backstage->addPage(standardParameters);

    BackstagePage advancedParameters;
    advancedParameters.id = kAdvancedParams;
    advancedParameters.header = tr("Advanced Params");
    advancedParameters.requiresConnection = true;
    advancedParameters.isAdvanced = true;
    advancedParameters.visibleWhen = [this]() {
        return m_connected && m_advanced && friendlyParametersSupported();
    };
    advancedParameters.factory = [this](QWidget *parent) {
        return createFriendlyParamsPage(true, parent);
    };
    m_backstage->addPage(advancedParameters);

    m_backstage->addPage(makeBackstagePage<GeoFenceConfig>(
        kGeoFence, tr("GeoFence"), copter));
    m_backstage->addPage(makeBackstagePage<BasicPidConfig>(
        kBasicTuning, tr("Basic Tuning"), copter));
    m_backstage->addPage(makeBackstagePage<ArduPlanePidConfig>(
        kPlaneTuning, tr("Basic Tuning (Plane)"), plane));
    m_backstage->addPage(makeBackstagePage<ArduRoverPidConfig>(
        kRoverTuning, tr("Basic Tuning (Rover)"), rover));
    m_backstage->addPage(makeBackstagePage<CopterPidConfig>(
        kExtendedTuning, tr("Extended Tuning"), copter));
    m_backstage->addPage(makeBackstagePage<OsdConfig>(
        kOnboardOsd, tr("Onboard OSD"), connected));

    m_backstage->addPage(makeBackstagePage<AdvParameterList>(
        kFullParameterList, tr("Full Parameter List"), always,
        false, false, true));

    BackstagePage planner;
    planner.id = kPlanner;
    planner.header = tr("Planner");
    planner.visibleWhen = always;
    planner.factory = [](QWidget *parent) {
        auto *settings = new QGCSettingsWidget(parent);
        return scrollablePage(settings, kPlanner, parent);
    };
    m_backstage->addPage(planner);

    m_backstage->addPage(makeBackstagePage<ConfigPlannerAdvView>(
        kPlannerAdvanced, tr("Planner (Advanced)"),
        [this]() { return m_advanced; }, false, true));
}

void ConfigView::advModeChanged(bool advanced)
{
    if (m_advanced == advanced) {
        return;
    }
    m_advanced = advanced;
    refreshPageVisibility();
    restorePreferredPage();
    emit advancedModeChanged(advanced);
}

void ConfigView::activeUASSet(UASInterface *uas)
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
    m_metadataUpdater->cancel();
    bindParameterManager(nullptr);

    m_uas = uas;
    m_parameterManager = nullptr;
    m_firmwareVersion.clear();
    m_officialFirmware = false;
    resetParameterProgress();

    if (m_uas) {
        connect(m_uas, &UASInterface::connected,
                this, &ConfigView::vehicleConnected);
        connect(m_uas, &UASInterface::disconnected,
                this, &ConfigView::vehicleDisconnected);
        connect(m_uas,
                QOverload<int, int, int, int, QString, QVariant>::of(
                    &UASInterface::parameterChanged),
                this, &ConfigView::parameterChanged);
        connect(m_uas, &UASInterface::parameterManagerChanged,
                this, &ConfigView::parameterManagerChanged);
        if (auto *apm = qobject_cast<ArduPilotMegaMAV *>(m_uas.data())) {
            connect(apm, &ArduPilotMegaMAV::versionDetected,
                    this, &ConfigView::firmwareVersionDetected);
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
        resetVehiclePages(true);
    }
    restorePreferredPage();
    if (auto *apm = qobject_cast<ArduPilotMegaMAV *>(m_uas.data())) {
        if (apm->getFirmwareVersion().isValid()) {
            firmwareVersionDetected(
                apm->getFirmwareVersion().versionString());
        }
    }
}

void ConfigView::vehicleConnected()
{
    syncConnectionState();
}

void ConfigView::vehicleDisconnected()
{
    m_metadataUpdater->cancel();
    m_firmwareVersion.clear();
    m_officialFirmware = false;
    syncConnectionState();
}

void ConfigView::parameterChanged(int uas, int component, int parameterCount,
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

void ConfigView::parameterListUpToDate(int component)
{
    Q_UNUSED(component)
    m_parametersReady = true;
    m_parameterLoadFailure.clear();
    m_parameterProgress = 100;
    refreshLoadingOverlay();
}

void ConfigView::parameterListReadyChanged(bool ready)
{
    m_parametersReady = ready;
    if (ready) {
        m_parameterLoadFailure.clear();
        m_parameterProgress = 100;
    }
    refreshLoadingOverlay();
}

void ConfigView::parameterListLoadFailed(const QString &reason)
{
    m_parametersReady = false;
    m_parameterLoadFailure = reason;
    refreshLoadingOverlay();
}

void ConfigView::parameterListLoadCanceled()
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

void ConfigView::parameterManagerChanged(QGCUASParamManager *manager)
{
    bindParameterManager(manager);
    resetParameterProgress();
    refreshPageVisibility();
    resetVehiclePages(false);
    restorePreferredPage();

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

void ConfigView::stopParameterLoading()
{
    if (m_parameterManager) {
        m_parameterManager->cancelParameterList();
    } else {
        parameterListLoadCanceled();
    }
}

void ConfigView::retryParameterLoading()
{
    resetParameterProgress();
    if (m_parameterManager) {
        m_parameterManager->requestParameterList();
    }
    refreshLoadingOverlay();
}

void ConfigView::currentPageChanged(const QString &pageId)
{
    if (!m_adjustingSelection && !pageId.isEmpty()) {
        m_preferredPageHeader = m_backstage->pageDefinition(pageId).header;
        QSettings settings;
        settings.setValue(kLastPageKey, m_preferredPageHeader);
    }
    refreshLoadingOverlay();
}

void ConfigView::firmwareVersionDetected(const QString &versionText)
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
    const bool identityChanged = normalized != m_firmwareVersion
        || official != m_officialFirmware;
    m_firmwareVersion = normalized;
    m_officialFirmware = official;
    if (identityChanged) {
        refreshFriendlyParameterPages();
        m_metadataUpdater->requestUpdate(
            parameterFirmwareFamily(), normalized, !official);
    }
}

void ConfigView::parameterMetadataUpdated(
    ParameterFirmwareFamily family, const QString &firmwareVersion)
{
    if (family != parameterFirmwareFamily()) {
        return;
    }
    if (!firmwareVersion.isEmpty()
        && (!m_officialFirmware || firmwareVersion != m_firmwareVersion)) {
        return;
    }
    refreshFriendlyParameterPages();
}

void ConfigView::refreshPageVisibility()
{
    m_backstage->refreshVisibility();
    refreshLoadingOverlay();
}

bool ConfigView::currentPageAllowsPartialParameters() const
{
    return m_backstage->pageDefinition(m_backstage->currentPageId())
        .allowsPartialParameters;
}

bool ConfigView::hasConnectedLink() const
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

void ConfigView::syncConnectionState()
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
    resetVehiclePages(false);
    restorePreferredPage();
}

void ConfigView::bindParameterManager(QGCUASParamManager *manager)
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
            this, &ConfigView::parameterListUpToDate);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListReadyChanged,
            this, &ConfigView::parameterListReadyChanged);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadFailed,
            this, &ConfigView::parameterListLoadFailed);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadCanceled,
            this, &ConfigView::parameterListLoadCanceled);
}

void ConfigView::resetVehiclePages(bool targetChanged)
{
    m_adjustingSelection = true;
    m_backstage->setAutomaticSelectionEnabled(false);
    for (const QString &pageId : m_backstage->pageIds()) {
        const BackstagePage definition = m_backstage->pageDefinition(pageId);
        if (definition.requiresConnection
            || (targetChanged && pageId == kFullParameterList)) {
            m_backstage->resetPage(pageId);
        }
    }
    m_backstage->setAutomaticSelectionEnabled(true);
    m_adjustingSelection = false;
}

void ConfigView::refreshFriendlyParameterPages()
{
    const ParameterFirmwareFamily family = parameterFirmwareFamily();
    const QString catalogVersion = m_officialFirmware
        ? m_firmwareVersion : QString();
    const ParameterMetaDataCatalog catalog = m_metadataRepository->catalog(
        family, catalogVersion);
    const bool enforceMetadataRanges =
        m_metadataRepository->catalogMatchesFirmwareVersion(
            family, catalogVersion);
    const QString unavailableMessage = catalog.isValid()
        ? QString()
        : tr("Parameter metadata is unavailable: %1")
              .arg(m_metadataRepository->errorString(
                  family, catalogVersion));

    const QStringList friendlyPages = {
        kStandardParams,
        kAdvancedParams
    };
    for (const QString &pageId : friendlyPages) {
        auto *page = qobject_cast<ConfigFriendlyParamsView *>(
            m_backstage->page(pageId));
        if (!page) {
            continue;
        }
        page->setCatalog(catalog, enforceMetadataRanges);
        page->setUnavailableMessage(unavailableMessage);
    }
}

void ConfigView::resetParameterProgress()
{
    m_parametersReady = false;
    m_parameterLoadFailure.clear();
    m_parameterProgress = -1;
    m_receivedParameterIds.clear();
    m_expectedParameterCounts.clear();
}

void ConfigView::restorePreferredPage()
{
    if (m_preferredPageHeader.isEmpty()) {
        return;
    }
    for (const QString &pageId : m_backstage->pageIds()) {
        if (m_backstage->isPageVisible(pageId)
            && m_backstage->pageDefinition(pageId).header
                == m_preferredPageHeader) {
            m_adjustingSelection = true;
            m_backstage->setCurrentPage(pageId);
            m_adjustingSelection = false;
            return;
        }
    }
}

QWidget *ConfigView::createFriendlyParamsPage(bool advanced, QWidget *parent)
{
    const ParameterFirmwareFamily family = parameterFirmwareFamily();
    const QString catalogVersion = m_officialFirmware
        ? m_firmwareVersion : QString();
    const ParameterMetaDataCatalog catalog = m_metadataRepository->catalog(
        family, catalogVersion);
    const bool enforceMetadataRanges =
        m_metadataRepository->catalogMatchesFirmwareVersion(
            family, catalogVersion);
    auto *page = new ConfigFriendlyParamsView(
        advanced, catalog, parent, enforceMetadataRanges);
    if (!catalog.isValid()) {
        page->setUnavailableMessage(
            tr("Parameter metadata is unavailable: %1")
                .arg(m_metadataRepository->errorString(
                    family, catalogVersion)));
    }
    page->setObjectName(advanced ? kAdvancedParams : kStandardParams);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);

    connect(page, &ConfigFriendlyParamsView::refreshRequested,
            page, [this](int componentId) {
        if (!m_connected || !m_parameterManager) {
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
        if (componentId != MAV_COMP_ID_AUTOPILOT1) {
            const QList<QString> names =
                m_parameterManager->getParameterNames(componentId);
            for (const QString &name : names) {
                m_parameterManager->requestParameterUpdate(componentId, name);
            }
            return;
        }
        retryParameterLoading();
    });
    connect(page, &ConfigFriendlyParamsView::writeRequested,
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
    return page;
}

QList<ConfigFriendlyParameterValue> ConfigView::parameterSnapshot(
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

ParameterFirmwareFamily ConfigView::parameterFirmwareFamily() const
{
    if (!m_uas
        || m_uas->getAutopilotType() != MAV_AUTOPILOT_ARDUPILOTMEGA) {
        return ParameterFirmwareFamily::Unknown;
    }
    switch (m_uas->getSystemType()) {
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

bool ConfigView::friendlyParametersSupported() const
{
    const ParameterFirmwareFamily family = parameterFirmwareFamily();
    return family == ParameterFirmwareFamily::ArduCopter
        || family == ParameterFirmwareFamily::ArduPlane
        || family == ParameterFirmwareFamily::Rover;
}

void ConfigView::refreshLoadingOverlay()
{
    const bool loading = m_connected && !m_parametersReady
        && !currentPageAllowsPartialParameters();
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
