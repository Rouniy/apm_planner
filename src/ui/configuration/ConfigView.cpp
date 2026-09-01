#include "ConfigView.h"

#include "AdvParameterList.h"
#include "ArduPlanePidConfig.h"
#include "ArduRoverPidConfig.h"
#include "BasicPidConfig.h"
#include "CopterPidConfig.h"
#include "FlightModeConfig.h"
#include "GeoFenceConfig.h"
#include "LinkInterface.h"
#include "OsdConfig.h"
#include "QGCSettingsWidget.h"
#include "QGCUASParamManager.h"
#include "UASInterface.h"
#include "UASManager.h"
#include "ui/BackstageView.h"

#include <QFrame>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include <utility>

namespace {
const QString kLastPageKey = QStringLiteral("config_lastpage");

const QString kFlightModes = QStringLiteral("ConfigFlightModesView");
const QString kGeoFence = QStringLiteral("ConfigAC_FenceView");
const QString kBasicTuning = QStringLiteral("ConfigBasicTuningView");
const QString kPlaneTuning = QStringLiteral("ConfigArduplaneView");
const QString kRoverTuning = QStringLiteral("ConfigArduroverView");
const QString kExtendedTuning = QStringLiteral("ConfigExtendedTuningView");
const QString kOnboardOsd = QStringLiteral("ConfigOSDView");
const QString kFullParameterList = QStringLiteral("RawParamsView");
const QString kPlanner = QStringLiteral("ConfigPlannerView");

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
      m_backstage(new BackstageView(this))
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
    connect(UASManager::instance(),
            QOverload<UASInterface *>::of(&UASManager::activeUASSet),
            this, &ConfigView::activeUASSet);
    activeUASSet(UASManager::instance()->getActiveUAS());
    m_backstage->restoreInitialPage(m_preferredPageHeader);
}

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
        syncConnectionState();
        return;
    }
    if (m_uas) {
        disconnect(m_uas, nullptr, this, nullptr);
    }
    if (m_parameterManager) {
        disconnect(m_parameterManager, nullptr, this, nullptr);
    }

    m_uas = uas;
    m_parameterManager = nullptr;
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
        m_parameterManager = m_uas->getParamManager();
        if (m_parameterManager) {
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
            m_parametersReady = m_parameterManager->parameterListReady();
        }
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
}

void ConfigView::vehicleConnected()
{
    syncConnectionState();
}

void ConfigView::vehicleDisconnected()
{
    syncConnectionState();
}

void ConfigView::parameterChanged(int uas, int component, int parameterCount,
                                  int parameterId, QString parameterName,
                                  QVariant value)
{
    Q_UNUSED(uas)
    Q_UNUSED(parameterName)
    Q_UNUSED(value)
    if (!m_connected || parameterId == UINT16_MAX || parameterCount <= 0) {
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
