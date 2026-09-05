#include "SetupRouteRuntimeAudit.h"
#include "InspectorRuntimeAudit.h"
#include "LogDownloadRuntimeAudit.h"

#include "ui/BackstageView.h"
#include "ui/MainWindow.h"
#include "comm/LinkManager.h"
#include "comm/MAVLinkProtocol.h"
#include "configuration.h"
#include "ui/configuration/SetupView.h"
#include "ui/configuration/PlannerStartupUdpOptions.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAction>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QWidget>

namespace {

const QString kInstallFirmware = QStringLiteral("InstallFirmwareView");
const QString kMandatoryGroup = QStringLiteral("MandatoryHardwareGroup");
const QString kOptionalGroup = QStringLiteral("OptionalHardwareGroup");
const QString kAdvancedGroup = QStringLiteral("AdvancedGroup");
const QString kOpticalFlow = QStringLiteral("ConfigOptFlowView");
const QString kJoystick = QStringLiteral("ConfigJoystickView");

struct Mp10SetupRoute
{
    QString referenceId;
    QString groupId;
    QString qtPageId;
};

QList<Mp10SetupRoute> Mp10ReferenceRoutes()
{
    // Independent inventory transcribed from MP10 SetupViewModel.cs. Group
    // header rows are intentionally excluded. An empty Qt ID is an explicit,
    // reviewed port gap rather than a route the audit silently forgot.
    return {
        {QStringLiteral("InstallFirmwareViewModel"), {},
         QStringLiteral("InstallFirmwareView")},
        {QStringLiteral("ConfigFirmwareLegacyViewModel"), {}, {}},
        {QStringLiteral("ConfigSecureApViewModel"), {}, {}},
        {QStringLiteral("ConfigSecureViewModel"), {}, {}},

        {QStringLiteral("ConfigTradHeli4ViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigTradHeli4View")},
        {QStringLiteral("ConfigFrameClassTypeViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigFrameClassTypeView")},
        {QStringLiteral("ConfigFrameTypeViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigFrameTypeView")},
        {QStringLiteral("ConfigDefaultSettingsViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigDefaultSettingsView")},
        {QStringLiteral("ConfigAccelCalibrationViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigAccelCalibrationView")},
        {QStringLiteral("ConfigCompassViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigCompassView")},
        {QStringLiteral("ConfigCompassLegacyViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigCompassLegacyView")},
        {QStringLiteral("ConfigRadioInputViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigRadioInputView")},
        {QStringLiteral("ConfigRadioOutputViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigRadioOutputView")},
        {QStringLiteral("ConfigSerialViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigSerialView")},
        {QStringLiteral("ConfigESCCalibrationViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigESCCalibrationView")},
        {QStringLiteral("ConfigFlightModesViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigFlightModesView")},
        {QStringLiteral("ConfigFailSafeViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigFailSafeView")},
        {QStringLiteral("ConfigInitialParamsViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigInitialParamsView")},
        {QStringLiteral("ConfigHWIDViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigHWIDView")},
        {QStringLiteral("ConfigADSBViewModel"), kMandatoryGroup,
         QStringLiteral("ConfigADSBView")},

        {QStringLiteral("ConfigGpsInjectViewModel"), kOptionalGroup,
         QStringLiteral("ConfigGpsInjectView")},
        {QStringLiteral("ConfigCubeIDViewModel"), kOptionalGroup, {}},
        {QStringLiteral("SikRadioViewModel"), kOptionalGroup,
         QStringLiteral("SikRadioView")},
        {QStringLiteral("NvModemViewModel"), kOptionalGroup, {}},
        {QStringLiteral("ConfigGPSOrderViewModel"), kOptionalGroup,
         QStringLiteral("ConfigGPSOrderView")},
        {QStringLiteral("ConfigBatteryMonitoringViewModel"), kOptionalGroup,
         QStringLiteral("ConfigBatteryMonitoringView")},
        {QStringLiteral("ConfigBatteryMonitoring2ViewModel"), kOptionalGroup,
         QStringLiteral("ConfigBatteryMonitoring2View")},
        {QStringLiteral("ConfigDroneCanViewModel"), kOptionalGroup,
         QStringLiteral("ConfigDroneCanView")},
        {QStringLiteral("ConfigJoystickViewModel"), kOptionalGroup,
         kJoystick},
        {QStringLiteral("ConfigCompassMotViewModel"), kOptionalGroup,
         QStringLiteral("ConfigCompassMotView")},
        {QStringLiteral("ConfigRangeFinderViewModel"), kOptionalGroup,
         QStringLiteral("ConfigRangeFinderView")},
        {QStringLiteral("ConfigAirspeedViewModel"), kOptionalGroup,
         QStringLiteral("ConfigAirspeedView")},
        {QStringLiteral("ConfigPX4FlowViewModel"), kOptionalGroup, {}},
        {QStringLiteral("ConfigOptFlowViewModel"), kOptionalGroup,
         QStringLiteral("ConfigOptFlowView")},
        {QStringLiteral("ConfigHWOSDViewModel"), kOptionalGroup,
         QStringLiteral("ConfigHWOSDView")},
        {QStringLiteral("ConfigMountViewModel"), kOptionalGroup,
         QStringLiteral("ConfigMountView")},
        {QStringLiteral("ConfigMotorTestViewModel"), kOptionalGroup,
         QStringLiteral("ConfigMotorTestView")},
        {QStringLiteral("ConfigHWBTViewModel"), kOptionalGroup,
         QStringLiteral("ConfigHWBTView")},
        {QStringLiteral("ConfigParachuteViewModel"), kOptionalGroup,
         QStringLiteral("ConfigParachuteView")},
        {QStringLiteral("ConfigHWESP8266ViewModel"), kOptionalGroup,
         QStringLiteral("ConfigHWESP8266View")},
        {QStringLiteral("ConfigAntennaTrackerParamViewModel"), kOptionalGroup,
         {}},
        {QStringLiteral("ConfigFFTViewModel"), kOptionalGroup, {}},
        {QStringLiteral("ConfigAntennaTrackerViewModel"), kOptionalGroup,
         QStringLiteral("ConfigAntennaTrackerView")},
        {QStringLiteral("AntennaTrackerUIViewModel"), kOptionalGroup,
         QStringLiteral("AntennaTrackerUIView")},
        {QStringLiteral("ConfigHWCANViewModel"), kOptionalGroup,
         QStringLiteral("ConfigHWCANView")},
        {QStringLiteral("MavFTPUIViewModel"), kOptionalGroup,
         QStringLiteral("MavFTPUIView")},

        {QStringLiteral("ConfigAdvancedViewModel"), kAdvancedGroup,
         QStringLiteral("ConfigAdvancedView")},
        {QStringLiteral("ConfigElevationSourcesViewModel"), kAdvancedGroup,
         QStringLiteral("ConfigElevationSourcesView")},
        {QStringLiteral("ConfigDeveloperToolsViewModel"), kAdvancedGroup,
         QStringLiteral("ConfigDeveloperToolsView")},
        {QStringLiteral("ConfigMavCommandViewModel"), kAdvancedGroup,
         QStringLiteral("ConfigMavCommandView")},
        {QStringLiteral("ConfigTerminalViewModel"), kAdvancedGroup,
         QStringLiteral("ConfigTerminalView")},
        {QStringLiteral("ConfigOnboardReplViewModel"), kAdvancedGroup, {}},
        {QStringLiteral("ConfigScriptReplViewModel"), kAdvancedGroup, {}}
    };
}

QSet<QString> KnownMissingMp10Routes()
{
    return {
        QStringLiteral("ConfigFirmwareLegacyViewModel"),
        QStringLiteral("ConfigSecureApViewModel"),
        QStringLiteral("ConfigSecureViewModel"),
        QStringLiteral("ConfigCubeIDViewModel"),
        QStringLiteral("NvModemViewModel"),
        QStringLiteral("ConfigPX4FlowViewModel"),
        QStringLiteral("ConfigAntennaTrackerParamViewModel"),
        QStringLiteral("ConfigFFTViewModel"),
        QStringLiteral("ConfigOnboardReplViewModel"),
        QStringLiteral("ConfigScriptReplViewModel")
    };
}

QStringList ExpectedPageIds()
{
    QStringList result;
    for (const Mp10SetupRoute &route : Mp10ReferenceRoutes()) {
        if (!route.qtPageId.isEmpty()) {
            result.append(route.qtPageId);
        }
    }
    // Deliberately retained useful Qt extension, absent from MP10 SETUP.
    result.append(QStringLiteral("QmlPluginManagerView"));
    return result;
}

QStringList ExpectedNavigationOrder()
{
    QStringList result;
    QString currentGroup;
    for (const Mp10SetupRoute &route : Mp10ReferenceRoutes()) {
        if (!route.groupId.isEmpty() && route.groupId != currentGroup) {
            currentGroup = route.groupId;
            result.append(currentGroup);
        }
        if (!route.qtPageId.isEmpty()) {
            result.append(route.qtPageId);
        }
    }
    result.append(QStringLiteral("QmlPluginManagerView"));
    return result;
}

QStringList NavigationOrder(QWidget *navigationContent)
{
    QStringList result;
    if (!navigationContent || !navigationContent->layout()) {
        return result;
    }

    QLayout *const layout = navigationContent->layout();
    for (int index = 0; index < layout->count(); ++index) {
        QWidget *const widget = layout->itemAt(index)->widget();
        if (!widget) {
            continue;
        }
        const QString groupId = widget->property("groupId").toString();
        const QString pageId = widget->property("pageId").toString();
        if (!groupId.isEmpty()) {
            result.append(groupId);
        } else if (!pageId.isEmpty()) {
            result.append(pageId);
        }
    }
    return result;
}

int SemanticContentScore(QWidget *page)
{
    int score = 0;
    const QList<QWidget *> descendants = page->findChildren<QWidget *>();
    for (QWidget *const widget : descendants) {
        // isHidden() detects controls deliberately suppressed by page state but
        // does not reject descendants merely because the audit host is never
        // shown. Disabled offline controls are still truthful page content.
        if (!widget || widget->isHidden()) {
            continue;
        }
        if (const auto *label = qobject_cast<const QLabel *>(widget)) {
            if (!label->text().trimmed().isEmpty()) {
                ++score;
            }
            continue;
        }
        if (const auto *button =
                qobject_cast<const QAbstractButton *>(widget)) {
            if (!button->text().trimmed().isEmpty()) {
                ++score;
            }
            continue;
        }
        if (const auto *group = qobject_cast<const QGroupBox *>(widget)) {
            if (!group->title().trimmed().isEmpty()) {
                ++score;
            }
            continue;
        }
        if (qobject_cast<const QAbstractItemView *>(widget)
            || qobject_cast<const QLineEdit *>(widget)
            || qobject_cast<const QComboBox *>(widget)
            || qobject_cast<const QAbstractSpinBox *>(widget)
            || qobject_cast<const QAbstractSlider *>(widget)
            || qobject_cast<const QTextEdit *>(widget)
            || qobject_cast<const QPlainTextEdit *>(widget)
            || qobject_cast<const QTabWidget *>(widget)) {
            score += 2;
            continue;
        }
        if (qobject_cast<const QProgressBar *>(widget)) {
            ++score;
        }
    }
    return score;
}

class AuditResult final
{
public:
    void Expect(bool condition, const QString &message)
    {
        if (condition) {
            return;
        }
        ++m_failures;
        qCritical().noquote() << QStringLiteral("SETUP route audit: %1")
                                    .arg(message);
    }

    int exitCode() const { return m_failures == 0 ? 0 : 1; }
    int failures() const { return m_failures; }

private:
    int m_failures = 0;
};

void ConfigureIsolatedSettings()
{
    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.clear();
    settings.setValue(
        QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey), false);
    settings.beginGroup(QStringLiteral("AUTO_UPDATE"));
    settings.setValue(QStringLiteral("ENABLED"), false);
    settings.endGroup();
    settings.sync();
}

} // namespace

int RunSetupRouteRuntimeAudit()
{
    ConfigureIsolatedSettings();

    AuditResult result;
    result.Expect(RunLogDownloadRuntimeAudit() == 0,
                  QStringLiteral("production UDP/log route audit failed"));
    result.Expect(RunInspectorRuntimeAudit() == 0,
                  QStringLiteral("production Inspector route/ingress audit failed"));
    // The retained legacy Planner surface must not bypass Connection Options'
    // restart-only sender identity policy while exact services are running.
    const int runningId = QGC::MavlinkID();
    MAVLinkProtocol *const protocol = LinkManager::instance()->getProtocol();
    const int protocolId = protocol->systemId();
    QSettings identitySettings;
    const bool hadPendingId = identitySettings.contains(QStringLiteral("gcsid"));
    const QVariant pendingId = identitySettings.value(QStringLiteral("gcsid"));
    const int nextId = runningId == 23 ? 24 : 23;
    MainWindow::instance()->setGroundStationSystemId(nextId);
    result.Expect(identitySettings.value(QStringLiteral("gcsid")).toInt() == nextId,
                  QStringLiteral("legacy GCS id change was not persisted"));
    result.Expect(QGC::MavlinkID() == runningId && protocol->systemId() == protocolId,
                  QStringLiteral("legacy GCS id change mutated the running identity"));
    MainWindow::instance()->setGroundStationSystemId(0);
    result.Expect(identitySettings.value(QStringLiteral("gcsid")).toInt() == 255
                      && QGC::MavlinkID() == runningId
                      && protocol->systemId() == protocolId,
                  QStringLiteral("invalid legacy GCS id was not deferred as 255"));
    if (hadPendingId) identitySettings.setValue(QStringLiteral("gcsid"), pendingId);
    else identitySettings.remove(QStringLiteral("gcsid"));
    identitySettings.sync();
    const QList<Mp10SetupRoute> mp10Routes = Mp10ReferenceRoutes();
    const QSet<QString> knownMissing = KnownMissingMp10Routes();
    result.Expect(mp10Routes.size() == 53,
                  QStringLiteral("independent MP10 manifest is not 53 pages"));
    result.Expect(knownMissing.size() == 10,
                  QStringLiteral("current MP10 missing-route allowlist is not 10"));

    QSet<QString> referenceIds;
    QSet<QString> mappedQtIds;
    QSet<QString> observedMissing;
    for (const Mp10SetupRoute &route : mp10Routes) {
        result.Expect(!route.referenceId.isEmpty(),
                      QStringLiteral("MP10 manifest contains an empty ID"));
        result.Expect(!referenceIds.contains(route.referenceId),
                      QStringLiteral("duplicate MP10 route %1")
                          .arg(route.referenceId));
        referenceIds.insert(route.referenceId);
        if (route.qtPageId.isEmpty()) {
            observedMissing.insert(route.referenceId);
        } else {
            result.Expect(!mappedQtIds.contains(route.qtPageId),
                          QStringLiteral("duplicate Qt mapping %1")
                              .arg(route.qtPageId));
            mappedQtIds.insert(route.qtPageId);
        }
    }
    result.Expect(observedMissing == knownMissing,
                  QStringLiteral("MP10 missing-route allowlist mismatch\n"
                                 "expected: %1\nactual:   %2")
                      .arg(QStringList(knownMissing.values()).join(", "),
                           QStringList(observedMissing.values()).join(", ")));
    QSet<QString> baselineGaps = knownMissing;
    baselineGaps.insert(QStringLiteral("ConfigJoystickViewModel"));
    result.Expect(baselineGaps.size() == 11
                      && mappedQtIds.contains(kJoystick),
                  QStringLiteral("the audited 11-page baseline gap or its "
                                 "Joystick closure was lost"));

    QWidget host;
    int joystickLaunchCount = 0;
    SetupView setup(&host);
    BackstageView *const backstage = setup.findChild<BackstageView *>(
        QStringLiteral("BackstageView"));
    result.Expect(backstage != nullptr,
                  QStringLiteral("production BackstageView was not created"));
    if (!backstage) {
        return result.exitCode();
    }

    // Route activation can start network catalog loads or device workflows.
    // Blocking only Backstage lifecycle signals retains the production button,
    // lazy-factory, selection and stack code while preventing those side
    // effects. No control inside any created page is invoked by this audit.
    const QSignalBlocker lifecycleBlocker(backstage);

    const QStringList expectedPages = ExpectedPageIds();
    result.Expect(expectedPages.size() == 44,
                  QStringLiteral("the audited Qt inventory is not 44 pages"));
    result.Expect(backstage->pageIds() == expectedPages,
                  QStringLiteral("production page ID/order mismatch\nexpected: %1\nactual:   %2")
                      .arg(expectedPages.join(QStringLiteral(", ")),
                           backstage->pageIds().join(QStringLiteral(", "))));

    QWidget *const navigationContent = backstage->findChild<QWidget *>(
        QStringLiteral("backstageNavigationContent"));
    const QStringList navigationOrder = NavigationOrder(navigationContent);
    const QStringList expectedNavigation = ExpectedNavigationOrder();
    result.Expect(expectedNavigation.size() == 47,
                  QStringLiteral("the navigation baseline itself is not 47 entries"));
    result.Expect(navigationOrder == expectedNavigation,
                  QStringLiteral("production page/group order mismatch\nexpected: %1\nactual:   %2")
                      .arg(expectedNavigation.join(QStringLiteral(", ")),
                           navigationOrder.join(QStringLiteral(", "))));

    const QStringList groupIds = {
        kMandatoryGroup, kOptionalGroup, kAdvancedGroup
    };
    for (const QString &groupId : groupIds) {
        result.Expect(backstage->setGroupVisible(groupId, true),
                      QStringLiteral("missing group %1").arg(groupId));
        result.Expect(backstage->setGroupExpanded(groupId, true),
                      QStringLiteral("could not expand group %1").arg(groupId));
    }
    for (const QString &pageId : expectedPages) {
        result.Expect(bool(backstage->pageDefinition(pageId).factory),
                      QStringLiteral("route %1 has no production factory")
                          .arg(pageId));
        result.Expect(backstage->setPageVisible(pageId, true),
                      QStringLiteral("could not expose route %1 for audit")
                          .arg(pageId));
    }

    QStackedWidget *const stack = backstage->findChild<QStackedWidget *>(
        QStringLiteral("backstagePageStack"));
    result.Expect(stack != nullptr,
                  QStringLiteral("production page stack was not created"));
    if (!stack) {
        return result.exitCode();
    }

    QSet<QWidget *> createdPages;
    for (const QString &pageId : expectedPages) {
        QAbstractButton *const button =
            backstage->findChild<QAbstractButton *>(pageId);
        result.Expect(button != nullptr,
                      QStringLiteral("route %1 has no navigation button")
                          .arg(pageId));
        if (!button) {
            continue;
        }
        result.Expect(button->isEnabled(),
                      QStringLiteral("route button %1 is disabled")
                          .arg(pageId));
        button->click();

        QWidget *const page = backstage->page(pageId);
        result.Expect(backstage->currentPageId() == pageId,
                      QStringLiteral("click did not select route %1")
                          .arg(pageId));
        result.Expect(button->isChecked(),
                      QStringLiteral("selected button %1 is not checked")
                          .arg(pageId));
        result.Expect(page != nullptr,
                      QStringLiteral("factory for %1 returned null")
                          .arg(pageId));
        if (!page) {
            continue;
        }

        result.Expect(!createdPages.contains(page),
                      QStringLiteral("route %1 reused another route's widget")
                          .arg(pageId));
        createdPages.insert(page);
        result.Expect(page->property("pageId").toString() == pageId,
                      QStringLiteral("route %1 lost its pageId ownership")
                          .arg(pageId));
        result.Expect(page->parentWidget() == stack,
                      QStringLiteral("route %1 is not owned by the page stack")
                          .arg(pageId));
        result.Expect(stack->indexOf(page) >= 0,
                      QStringLiteral("route %1 is absent from the page stack")
                          .arg(pageId));
        result.Expect(stack->currentWidget() == page,
                      QStringLiteral("route %1 is not the displayed stack page")
                          .arg(pageId));

        const int semanticScore = SemanticContentScore(page);
        result.Expect(semanticScore >= 2,
                      QStringLiteral("route %1 is semantically blank (score %2)")
                          .arg(pageId)
                          .arg(semanticScore));
    }

    // Reproduce the production startup regression: the saved Advanced Tools
    // page can be restored before MainWindow builds its shared TOOLS QAction
    // catalogue. Both lazy pages must recover when actions become available.
    QPointer<QWidget> oldAdvanced(
        backstage->page(QStringLiteral("ConfigAdvancedView")));
    QPointer<QWidget> oldDeveloper(
        backstage->page(QStringLiteral("ConfigDeveloperToolsView")));
    QPointer<QWidget> oldJoystick(backstage->page(kJoystick));
    auto *joystickAction = new QAction(&host);
    joystickAction->setObjectName(QStringLiteral("actionJoystickSettings"));
    QObject::connect(joystickAction, &QAction::triggered,
                     &host, [&joystickLaunchCount]() {
        ++joystickLaunchCount;
    });
    const QStringList sharedActionNames = {
        QStringLiteral("actionMavlinkInspector"),
        QStringLiteral("actionMavlinkMirror"),
        QStringLiteral("actionNmeaOutput"),
        QStringLiteral("actionCotOutput"),
        QStringLiteral("actionFollowMe"),
        QStringLiteral("actionExternalGuided"),
        QStringLiteral("actionMovingBase"),
        QStringLiteral("actionMapTileCache"),
        QStringLiteral("actionDataFlashSpectrogram"),
        QStringLiteral("actionProximity"),
        QStringLiteral("actionMavlinkDeviceOperations"),
        QStringLiteral("actionTerrain3D"),
        QStringLiteral("actionOsdVideoOverlay")
    };
    for (const QString &objectName : sharedActionNames) {
        auto *action = new QAction(&host);
        action->setObjectName(objectName);
    }
    setup.applicationToolActionsReady();
    result.Expect(oldAdvanced.isNull(),
                  QStringLiteral("Advanced Tools retained its pre-action page"));
    result.Expect(oldDeveloper.isNull(),
                  QStringLiteral("Developer Tools retained its pre-action page"));
    result.Expect(oldJoystick.isNull(),
                  QStringLiteral("Joystick retained its pre-action launcher"));

    QAbstractButton *joystickNavigation =
        backstage->findChild<QAbstractButton *>(kJoystick);
    if (joystickNavigation) {
        joystickNavigation->click();
    }
    QWidget *const joystickPage = backstage->page(kJoystick);
    QAbstractButton *const joystickSettings = joystickPage
        ? joystickPage->findChild<QAbstractButton *>(
              QStringLiteral("JoystickSettingsButton"))
        : nullptr;
    result.Expect(joystickSettings && joystickSettings->isEnabled(),
                  QStringLiteral("Joystick route does not expose the shared "
                                 "settings action after action registration"));
    if (joystickSettings) {
        joystickSettings->click();
        result.Expect(joystickLaunchCount == 1,
                      QStringLiteral("Joystick route bypassed or failed to "
                                     "trigger MainWindow's shared action"));
    }

    const QStringList advancedButtons = {
        QStringLiteral("MAVLinkInspectorButton"),
        QStringLiteral("MavlinkMirrorButton"),
        QStringLiteral("NmeaButton"),
        QStringLiteral("CotTakButton"),
        QStringLiteral("FollowMeButton"),
        QStringLiteral("ExternalGuidedButton"),
        QStringLiteral("MovingBaseButton"),
        QStringLiteral("MapTileCacheButton"),
        QStringLiteral("SpectrogramButton"),
        QStringLiteral("ProximityButton")
    };
    QAbstractButton *advancedNavigation = backstage->findChild<QAbstractButton *>(
        QStringLiteral("ConfigAdvancedView"));
    if (advancedNavigation) {
        advancedNavigation->click();
    }
    QWidget *advancedPage = backstage->page(
        QStringLiteral("ConfigAdvancedView"));
    for (const QString &objectName : advancedButtons) {
        QAbstractButton *tool = advancedPage
            ? advancedPage->findChild<QAbstractButton *>(objectName) : nullptr;
        result.Expect(tool && tool->isEnabled(),
                      QStringLiteral("restored Advanced tool %1 is disabled")
                          .arg(objectName));
    }

    QAbstractButton *developerNavigation = backstage->findChild<QAbstractButton *>(
        QStringLiteral("ConfigDeveloperToolsView"));
    if (developerNavigation) {
        developerNavigation->click();
    }
    QWidget *developerPage = backstage->page(
        QStringLiteral("ConfigDeveloperToolsView"));
    const QStringList developerButtons = {
        QStringLiteral("MavlinkDeviceOperationsButton"),
        QStringLiteral("Terrain3dViewButton")
    };
    for (const QString &objectName : developerButtons) {
        QAbstractButton *tool = developerPage
            ? developerPage->findChild<QAbstractButton *>(objectName) : nullptr;
        result.Expect(tool && tool->isEnabled(),
                      QStringLiteral("restored Developer tool %1 is disabled")
                          .arg(objectName));
    }
#ifdef APM_HAS_QT_MULTIMEDIA
    QAbstractButton *osdVideo = developerPage
        ? developerPage->findChild<QAbstractButton *>(
              QStringLiteral("OsdVideoTelemetryOverlayButton"))
        : nullptr;
    result.Expect(osdVideo && osdVideo->isEnabled(),
                  QStringLiteral("restored Developer OSD Video is disabled"));
#endif

    // Optical Flow is intentionally the smallest retained legacy page and is
    // therefore the strongest regression probe for an accidentally blank
    // factory. Also exercise the production reset/fallback/recreate path.
    QAbstractButton *const opticalButton =
        backstage->findChild<QAbstractButton *>(kOpticalFlow);
    result.Expect(opticalButton != nullptr,
                  QStringLiteral("Optical Flow navigation button disappeared"));
    if (opticalButton) {
        opticalButton->click();
        QPointer<QWidget> oldOpticalPage(backstage->page(kOpticalFlow));
        result.Expect(!oldOpticalPage.isNull(),
                      QStringLiteral("Optical Flow page was not created"));
        result.Expect(backstage->resetPage(kOpticalFlow),
                      QStringLiteral("Optical Flow page reset failed"));
        result.Expect(oldOpticalPage.isNull(),
                      QStringLiteral("Optical Flow reset retained the old widget"));
        result.Expect(backstage->currentPageId() == kInstallFirmware,
                      QStringLiteral("reset did not select the first visible fallback"));
        opticalButton->click();
        QWidget *const recreated = backstage->page(kOpticalFlow);
        result.Expect(recreated != nullptr,
                      QStringLiteral("Optical Flow factory did not recreate"));
        if (recreated) {
            result.Expect(stack->currentWidget() == recreated,
                          QStringLiteral("recreated Optical Flow page is not current"));
            result.Expect(SemanticContentScore(recreated) >= 2,
                          QStringLiteral("recreated Optical Flow page is blank"));
        }
    }

    if (result.failures() == 0) {
        qInfo().noquote()
            << QStringLiteral("SETUP route audit: 53-page MP10 manifest, "
                              "44 Qt pages + 3 groups passed");
    }
    return result.exitCode();
}
