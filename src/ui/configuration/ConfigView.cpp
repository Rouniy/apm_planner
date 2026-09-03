#include "ConfigView.h"

#include "ArduPlanePidConfig.h"
#include "ArduRoverPidConfig.h"
#include "BasicPidConfig.h"
#include "CopterPidConfig.h"
#include "ConfigPlannerAdvView.h"
#include "ConfigFriendlyParamsView.h"
#include "ConfigRawParams.h"
#include "ConfigUserDefinedView.h"
#include "FlightModeConfig.h"
#include "GeoFenceConfig.h"
#include "LinkInterface.h"
#include "LinkManager.h"
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
#include <QScopedValueRollback>
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
const QString kUserParams = QStringLiteral("ConfigUserDefinedView");
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

    BackstagePage userParameters;
    userParameters.id = kUserParams;
    userParameters.header = tr("User Params");
    userParameters.requiresConnection = true;
    userParameters.visibleWhen = connected;
    userParameters.factory = [this](QWidget *parent) {
        return createUserDefinedPage(parent);
    };
    m_backstage->addPage(userParameters);

    BackstagePage rawParameters;
    rawParameters.id = kFullParameterList;
    rawParameters.header = tr("Full Parameter List");
    rawParameters.requiresConnection = false;
    rawParameters.allowsPartialParameters = true;
    rawParameters.visibleWhen = always;
    rawParameters.factory = [this](QWidget *parent) {
        return createRawParamsPage(parent);
    };
    m_backstage->addPage(rawParameters);

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
        QGCUASParamManager *const sharedManager =
            LinkManager::instance()->parameterManager();
        if (m_parameterManager != sharedManager) {
            parameterManagerChanged(sharedManager);
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
        if (auto *apm = qobject_cast<ArduPilotMegaMAV *>(m_uas.data())) {
            connect(apm, &ArduPilotMegaMAV::versionDetected,
                    this, &ConfigView::firmwareVersionDetected);
        }
        bindParameterManager(LinkManager::instance()->parameterManager());
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

void ConfigView::parameterListUpToDate(int component)
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

void ConfigView::parameterListLoadStarted()
{
    if (sender() && sender() != m_parameterManager) {
        return;
    }
    m_parametersReady = false;
    m_parameterLoadFailure.clear();
    m_parameterLoadingCanceled = false;
    refreshLoadingOverlay();
}

void ConfigView::parameterListReadyChanged(bool ready)
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

void ConfigView::parameterListLoadFailed(const QString &reason)
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

void ConfigView::parameterListLoadCanceled()
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

void ConfigView::parameterManagerChanged(QGCUASParamManager *manager)
{
    const qulonglong revision = ++m_parameterTargetRevision;
    bindParameterManager(manager);
    resetParameterProgress();
    refreshPageVisibility();
    resetVehiclePages(false);
    // The raw page is intentionally available while disconnected, so it is
    // not covered by requiresConnection. Recreate it when the application-
    // owned facade changes to avoid retaining signal connections to an old
    // service instance.
    m_backstage->resetPage(kFullParameterList);
    restorePreferredPage();

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

void ConfigView::stopParameterLoading()
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

void ConfigView::retryParameterLoading()
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
    // Visibility changes are programmatic. If the selected route disappears,
    // BackstageView chooses a visible fallback synchronously, but that fallback
    // must not replace the user's saved route preference.
    QScopedValueRollback<bool> selectionGuard(m_adjustingSelection, true);
    m_backstage->refreshVisibility();
    if (auto *page = qobject_cast<ConfigRawParams *>(
            m_backstage->page(kFullParameterList))) {
        page->setConnected(m_connected);
    }
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
            &QGCUASParamManager::parameterTargetChanged,
            this, &ConfigView::parameterTargetChanged);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadStarted,
            this, &ConfigView::parameterListLoadStarted);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListReadyChanged,
            this, &ConfigView::parameterListReadyChanged);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadFailed,
            this, &ConfigView::parameterListLoadFailed);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListLoadCanceled,
            this, &ConfigView::parameterListLoadCanceled);
    connect(m_parameterManager,
            &QGCUASParamManager::parameterListProgressChanged,
            this, [this](int, int, int) { refreshLoadingOverlay(); });
}

void ConfigView::resetVehiclePages(bool targetChanged)
{
    const QString selectedPage = m_backstage->currentPageId();
    const bool automaticSelectionWasEnabled =
        m_backstage->automaticSelectionEnabled();
    QScopedValueRollback<bool> selectionGuard(m_adjustingSelection, true);
    m_backstage->setAutomaticSelectionEnabled(false);
    for (const QString &pageId : m_backstage->pageIds()) {
        const BackstagePage definition = m_backstage->pageDefinition(pageId);
        if (definition.requiresConnection
            || (targetChanged && pageId == kFullParameterList)) {
            m_backstage->resetPage(pageId);
        }
    }

    // Recreate the route the user was looking at before re-enabling automatic
    // fallback. This preserves a still-visible selection across a target reset.
    // If it became hidden, re-enabling selects the first visible concrete page.
    if (!selectedPage.isEmpty()
        && m_backstage->isPageVisible(selectedPage)) {
        m_backstage->setCurrentPage(selectedPage);
    }
    // During construction automatic selection is deliberately disabled until
    // restoreInitialPage() can honor the saved preference lazily. Preserve that
    // outer state instead of creating an earlier page factory here.
    m_backstage->setAutomaticSelectionEnabled(
        automaticSelectionWasEnabled);
}

void ConfigView::parameterTargetChanged()
{
    const QString currentPage = m_backstage->currentPageId();
    if (!currentPage.isEmpty()) {
        m_targetPageToRestore = currentPage;
    }
    const qulonglong revision = ++m_parameterTargetRevision;
    const QPointer<QGCUASParamManager> expectedManager(m_parameterManager);
    resetParameterProgress();
    resetVehiclePages(true);
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
        if (m_backstage->isPageVisible(pageToRestore)) {
            m_backstage->setCurrentPage(pageToRestore);
        }
    });
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
    if (auto *page = qobject_cast<ConfigUserDefinedView *>(
            m_backstage->page(kUserParams))) {
        page->setCatalog(catalog, enforceMetadataRanges);
    }
    if (auto *page = qobject_cast<ConfigRawParams *>(
            m_backstage->page(kFullParameterList))) {
        page->setCatalog(catalog, enforceMetadataRanges);
    }
}

void ConfigView::resetParameterProgress()
{
    m_parametersReady = false;
    m_parameterLoadFailure.clear();
    m_parameterLoadingCanceled = false;
    m_parameterRetryPending = false;
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
            QScopedValueRollback<bool> selectionGuard(
                m_adjustingSelection, true);
            m_backstage->setCurrentPage(pageId);
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
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigFriendlyParamsView::parameterChanged);
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

QWidget *ConfigView::createUserDefinedPage(QWidget *parent)
{
    const ParameterFirmwareFamily family = parameterFirmwareFamily();
    const QString catalogVersion = m_officialFirmware
        ? m_firmwareVersion : QString();
    const ParameterMetaDataCatalog catalog = m_metadataRepository->catalog(
        family, catalogVersion);
    const bool enforceMetadataRanges =
        m_metadataRepository->catalogMatchesFirmwareVersion(
            family, catalogVersion);
    auto *page = new ConfigUserDefinedView(
        catalog, parent, enforceMetadataRanges);
    page->setObjectName(kUserParams);
    page->setParameterSnapshot(
        parameterSnapshot(MAV_COMP_ID_AUTOPILOT1),
        MAV_COMP_ID_AUTOPILOT1);

    connect(page, &ConfigUserDefinedView::refreshRequested,
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
    connect(page, &ConfigUserDefinedView::writeRequested,
            page, [this, page](int componentId, const QString &name,
                              const QVariant &value) {
        if (!m_connected || !m_parameterManager) {
            page->parameterWriteFailed(componentId, name, value,
                                       tr("not connected"));
            return;
        }
        if (!m_parameterManager->getParameterNames(componentId)
                 .contains(name)) {
            page->parameterWriteFailed(componentId, name, value,
                                       tr("parameter unavailable"));
            return;
        }
        m_parameterManager->setParameter(componentId, name, value);
    });
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigUserDefinedView::parameterChanged);
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

QWidget *ConfigView::createRawParamsPage(QWidget *parent)
{
    const ParameterFirmwareFamily family = parameterFirmwareFamily();
    const QString catalogVersion = m_officialFirmware
        ? m_firmwareVersion : QString();
    const ParameterMetaDataCatalog catalog = m_metadataRepository->catalog(
        family, catalogVersion);
    const bool enforceMetadataRanges =
        m_metadataRepository->catalogMatchesFirmwareVersion(
            family, catalogVersion);
    auto *page = new ConfigRawParams(
        catalog, parent, enforceMetadataRanges);
    page->setConnected(m_connected);
    if (m_parameterManager && m_parameterManager->store()) {
        const ParameterSnapshot snapshot =
            m_parameterManager->store()->snapshot();
        page->setParameterSnapshot(
            snapshot.records(), snapshot.endpoint().componentId);

        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                page, &ConfigRawParams::parameterChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterWriteAcknowledged,
                page, &ConfigRawParams::parameterWriteAcknowledged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterWriteFailed,
                page, &ConfigRawParams::parameterWriteFailed);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterWriteCancelled,
                page, &ConfigRawParams::parameterWriteCancelled);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterBatchProgress,
                page, &ConfigRawParams::parameterBatchProgress);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterBatchCompleted,
                page, &ConfigRawParams::parameterBatchCompleted);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterTargetChanged,
                page, &ConfigRawParams::parameterTargetChanged);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListReadyChanged,
                page, [this, page](bool ready) {
            if (!ready || !m_parameterManager
                || !m_parameterManager->store()) {
                return;
            }
            const ParameterSnapshot refreshed =
                m_parameterManager->store()->snapshot();
            page->setParameterSnapshot(
                refreshed.records(), refreshed.endpoint().componentId);
        });
    }
    connect(page, &ConfigRawParams::refreshRequested,
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
        if (m_parameterManager->store()
            && componentId
                == m_parameterManager->store()->endpoint().componentId) {
            retryParameterLoading();
        }
    });
    connect(page, &ConfigRawParams::writeRequested,
            page, [this, page](int componentId,
                              const QVariantList &changes) {
        if (!m_connected || !m_parameterManager) {
            page->parameterWriteSubmissionFailed(tr("Not connected."));
            return;
        }
        const QPointer<ConfigRawParams> guard(page);
        const qulonglong batchId = m_parameterManager->writeParameters(
            componentId, changes);
        if (!guard) {
            return;
        }
        if (batchId == 0) {
            guard->parameterWriteSubmissionFailed(
                tr("The parameter batch was rejected for the selected target."));
            return;
        }
        guard->parameterBatchSubmitted(batchId, changes.size());
    });
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
