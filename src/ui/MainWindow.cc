/*=====================================================================

QGroundControl Open Source Ground Control Station

(c) 2009 - 2013 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>

This file is part of the QGROUNDCONTROL project

    QGROUNDCONTROL is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    QGROUNDCONTROL is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with QGROUNDCONTROL. If not, see <http://www.gnu.org/licenses/>.

======================================================================*/

/**
 * @file
 *   @brief Implementation of class MainWindow
 *   @author Lorenz Meier <mail@qgroundcontrol.org>
 */

#include "MainWindow.h"
#include "logging.h"
#include "dockwidgettitlebareventfilter.h"
#include "QGC.h"
#include "CommConfigurationWindow.h"
#include "GAudioOutput.h"
#include "services/SpeechAnnouncer.h"
#include "QGCToolWidget.h"
#include "QGCMAVLinkLogPlayer.h"
#include "QGCSettingsWidget.h"
#include "QGCTabbedInfoView.h"
#include "QGCMAVLinkLogPlayer.h"
#include "QGCMAVLinkInspector.h"
#include "MAVLinkInspectorWindow.h"
#include "LinkStatsWindow.h"
#include "MissionPlannerToolsMenu.h"
#include "QGCMapTool.h"
#include "QGCStatusBar.h"
#include "QGCWaypointListMulti.h"
#include "ParameterInterface.h"
#include "submainwindow.h"
#include "UASControlWidget.h"
#include "UAS.h"
#include "QGCUASParamManager.h"
#include "UASListWidget.h"
#include "PrimaryFlightDisplayQML.h"
#include "MissionElevationDisplay.h"
#include "EKFMonitor.h"
#include "VibrationMonitor.h"
#include "UASInfoWidget.h"
#include "HSIDisplay.h"
#include "PrimaryFlightDisplay.h"
#include "PrimaryFlightDisplayQML.h"
#include "ObjectDetectionView.h"
#include "WatchdogControl.h"

#include "FlightDataView.h"
#include "flightdata/FlightDataViewModel.h"
#include "flightdata/HudControl.h"
#include "flightdata/ProximityWindow.h"
#include "map/MapCacheView.h"
#include "map/AbstractMapWidget.h"
#include "FlightPlannerView.h"
#include "flightplanner/FlightPlannerActionPanel.h"
#include "flightplanner/FlightPlannerMeasurement.h"
#include "flightplanner/MissionElevationProfile.h"
#include "flightplanner/FlightPlannerViewModel.h"
#include "flightplanner/FlightPlannerPrefetchController.h"
#include "flightplanner/FlightPlannerWaypointPanel.h"
#include "configuration/ElevationSourceService.h"
#include "HelpView.h"
#include "docking/DockableView.h"
#include "MainWindowHeader.h"
#include "ConnectionOptionsWindow.h"
#include "ConfigView.h"
#include "SetupView.h"
#include "configuration/QmlPluginManagerView.h"
#include "TerminalConsole.h"
#include "AP2DataPlot2D.h"
#include "uas/LogDownloadDialog.h"
#include "QGCCore.h"
#include "LinkManager.h"
#include "LinkManagerFactory.h"
#include "comm/VehicleTargetManager.h"

#ifdef QGC_OSG_ENABLED
#include "Q3DWidgetFactory.h"
#endif

#include "AboutDialog.h"


#include <QSettings>
#include <QApplication>
#include <QDockWidget>
#include <QDialog>
#include <QInputDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QNetworkInterface>
#include <QMessageBox>
#include <QScreen>
#include <QShortcut>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>


#include <QTimer>
#include <QHostInfo>
#include <QSplashScreen>
#include <QGCHilLink.h>
#include <QGCHilConfiguration.h>
#include <QGCHilFlightGearConfiguration.h>

namespace {
MainWindow *s_mainWindowInstance = nullptr;

QString canonicalPlannerLinearUnits(const QString &value)
{
    const QString candidate = value.trimmed();
    if (candidate.compare(QStringLiteral("Meters"), Qt::CaseInsensitive)
        == 0) {
        return QStringLiteral("Meters");
    }
    if (candidate.compare(QStringLiteral("Feet"), Qt::CaseInsensitive)
        == 0) {
        return QStringLiteral("Feet");
    }
    return {};
}

// WinForms ToolStripMenuItem supports a command and a child drop-down on the
// same row. QMenu normally turns such an action into a submenu-only item, so
// preserve Mission Planner's split behavior: the row body triggers Insert Wp,
// while its indicator (and ordinary hover) opens At Current Position.
class MenuSplitActionFilter final : public QObject
{
public:
    MenuSplitActionFilter(QMenu *menu, QAction *action)
        : QObject(menu), m_menu(menu), m_action(action)
    {
        if (m_menu) m_menu->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_menu || !m_action
            || event->type() != QEvent::MouseButtonRelease) {
            return QObject::eventFilter(watched, event);
        }
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() != Qt::LeftButton)
            return QObject::eventFilter(watched, event);

        const QRect geometry = m_menu->actionGeometry(m_action);
        if (!geometry.contains(mouseEvent->pos()))
            return QObject::eventFilter(watched, event);
        const int indicatorWidth = qMax(
            22, m_menu->style()->pixelMetric(
                    QStyle::PM_MenuButtonIndicator, nullptr, m_menu) + 10);
        const bool indicatorHit = m_menu->layoutDirection() == Qt::RightToLeft
            ? mouseEvent->pos().x() < geometry.left() + indicatorWidth
            : mouseEvent->pos().x() > geometry.right() - indicatorWidth;
        if (indicatorHit)
            return QObject::eventFilter(watched, event);

        const QPointer<QAction> action(m_action);
        if (QMenu *submenu = m_action->menu()) submenu->close();
        m_menu->close();
        QTimer::singleShot(0, m_menu, [action]() {
            if (action) action->trigger();
        });
        return true;
    }

private:
    QPointer<QMenu> m_menu;
    QPointer<QAction> m_action;
};
}

LogWindowSingleton &LogWindowSingleton::instance()
{
   static LogWindowSingleton instance;
   return instance;
}

void LogWindowSingleton::write(const QString &message)
{
   if (!m_debugPtr.isNull())
   {
      if (!m_outPutBuffer.empty())
      {
         foreach (QString string, m_outPutBuffer)
         {
            m_debugPtr->write(string);
         }
         m_outPutBuffer.clear();
         m_startupBuffering = false;
      }
      m_debugPtr->write(message);
   }
   else if (m_startupBuffering)
   {
      m_outPutBuffer.append(message);
   }
}

void LogWindowSingleton::setDebugOutput(DebugOutput::Ptr outputPtr)
{
   m_debugPtr = outputPtr;
}

void LogWindowSingleton::removeDebugOutput()
{
   m_debugPtr.clear();
}


MainWindow* MainWindow::instance()
{
    // This singleton impl. is NOT thread safe. Fortunately we do not need
    // thread safety as the first call is always done @ application start.
    if (s_mainWindowInstance == nullptr)
    {
        new MainWindow();
    }
    return s_mainWindowInstance;
}

// inline function definitions

int MainWindow::getStyle()
{
    return currentStyle;
}

bool MainWindow::autoReconnectEnabled()
{
    return autoReconnect;
}

bool MainWindow::dockWidgetTitleBarsEnabled()
{
    return dockWidgetTitleBarEnabled;
}


bool MainWindow::lowPowerModeEnabled()
{
    return lowPowerMode;
}

bool MainWindow::autoProxyModeEnabled()
{
    return autoProxyMode;
}

/**
* Create new mainwindow. The constructor instantiates all parts of the user
* interface. It does NOT show the mainwindow. To display it, call the show()
* method.
*
* @see QMainWindow::show()
**/
MainWindow::MainWindow(QWidget *parent):
    QMainWindow(parent),
    aboutToCloseFlag(false),
    changingViewsFlag(false),
    centerStackActionGroup(new QActionGroup(this)),
    styleFileName(QCoreApplication::applicationDirPath() + "/style-outdoor.css"),
    m_heartbeatEnabled(true),
    m_dialog(nullptr),
    m_terminalDialog(NULL)
{
    // Some child widgets consult MainWindow::instance() from showEvent while
    // this constructor is still assembling the central stack. Publish the
    // singleton before creating those children to prevent recursive windows.
    Q_ASSERT(s_mainWindowInstance == nullptr
             || s_mainWindowInstance == this);
    s_mainWindowInstance = this;

    QLOG_DEBUG() << "Creating MainWindow";
    setAttribute(Qt::WA_DeleteOnClose);
    hide();

    loadSettings();
    enableDockWidgetTitleBars(dockWidgetTitleBarEnabled);

    loadStyle(currentStyle);


    // Setup user interface
    ui.setupUi(this);
    hide();

    ui.actionAdvanced_Mode->setChecked(isAdvancedMode);
    ui.actionSimulate->setVisible(false);

    // We only need this menu if we have more than one system
    //    ui.menuConnected_Systems->setEnabled(false);

    // Set dock options
    setDockOptions(AnimatedDocks | AllowTabbedDocks | AllowNestedDocks);

    configureWindowName();

    // Setup corners
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);

    // Setup UI state machines
    centerStackActionGroup->setExclusive(true);

    auto *applicationShell = new QWidget(this);
    auto *applicationShellLayout = new QVBoxLayout(applicationShell);
    applicationShellLayout->setContentsMargins(0, 0, 0, 0);
    applicationShellLayout->setSpacing(0);
    m_mainWindowHeader = new MainWindowHeader(applicationShell);
    centerStack = new QStackedWidget(applicationShell);
    centerStack->setObjectName(QStringLiteral("mainScreenStack"));
    applicationShellLayout->addWidget(m_mainWindowHeader);
    applicationShellLayout->addWidget(centerStack, 1);
    setCentralWidget(applicationShell);

    helpViewAction = new QAction(tr("Help"), this);
    helpViewAction->setObjectName(QStringLiteral("actionHelpView"));
    helpViewAction->setCheckable(true);
    ui.menuPerspectives->addAction(helpViewAction);

    // Match the primary Mission Planner 10 navigation shortcuts. More
    // tool-specific shortcuts are enabled with the corresponding ported tools.
    ui.actionFlightView->setShortcut(QKeySequence(Qt::Key_F2));
    ui.actionMissionView->setShortcut(QKeySequence(Qt::Key_F3));
    ui.actionSoftwareConfig->setShortcut(QKeySequence(Qt::Key_F4));
    auto *connectionShortcut = new QAction(this);
    connectionShortcut->setObjectName(QStringLiteral("actionToggleConnection"));
    connectionShortcut->setShortcut(QKeySequence(Qt::Key_F12));
    addAction(connectionShortcut);
    connect(connectionShortcut, &QAction::triggered,
            m_mainWindowHeader, &MainWindowHeader::toggleConnection);

    auto *refreshParametersShortcut = new QAction(this);
    refreshParametersShortcut->setObjectName(
        QStringLiteral("actionRefreshFullParameterList"));
    refreshParametersShortcut->setShortcut(QKeySequence(Qt::Key_F5));
    addAction(refreshParametersShortcut);
    connect(refreshParametersShortcut, &QAction::triggered, this, [this]() {
        UASInterface *uas = UASManager::instance()->getActiveUAS();
        if (uas) {
            uas->requestParameters();
        } else {
            showStatusMessage(tr("Connect a vehicle before refreshing parameters."));
        }
    });

    auto *saveParametersShortcut = new QAction(this);
    saveParametersShortcut->setObjectName(
        QStringLiteral("actionSaveParametersToEeprom"));
    saveParametersShortcut->setShortcut(QKeySequence(QStringLiteral("Ctrl+Y")));
    addAction(saveParametersShortcut);
    connect(saveParametersShortcut, &QAction::triggered, this, [this]() {
        UASInterface *uas = UASManager::instance()->getActiveUAS();
        if (uas) {
            uas->writeParametersToStorage();
        } else {
            showStatusMessage(tr("Connect a vehicle before saving parameters."));
        }
    });

    m_mainWindowHeader->setNavigationActions(ui.actionFlightView,
                                                  ui.actionMissionView,
                                                  ui.actionHardwareConfig,
                                                  ui.actionSoftwareConfig,
                                                  ui.actionSimulation_View,
                                                  helpViewAction);
    m_mainWindowHeader->setToolsMenu(ui.menuTools);
    // Proximity is an MP10 Advanced SETUP action rather than a top-level
    // TOOLS item. Keep one shared action source for ConfigAdvancedView without
    // mixing it into the application-tools inventory.
    auto *proximityAction = new QAction(tr("Proximity"), this);
    proximityAction->setObjectName(QStringLiteral("actionProximity"));
    connect(proximityAction, &QAction::triggered, this, [this]() {
        UASManager *manager = UASManager::instance();
        ProximityWindow *window = ProximityWindow::OpenWindow(this);
        connect(manager,
                QOverload<UASInterface *>::of(&UASManager::activeUASSet),
                window, &ProximityWindow::setActiveUAS);
        connect(manager, &UASManager::UASDeleted, window,
                [window](UASInterface *uas) {
            if (window->activeUAS() == uas) {
                window->setActiveUAS(nullptr);
            }
        });
        // Subscribe before sampling. A vehicle switch immediately before the
        // snapshot is then represented either by the signal or by this value,
        // never lost in the open-window interval.
        window->setActiveUAS(manager->silentGetActiveUAS());
    });
    connect(m_mainWindowHeader, &MainWindowHeader::fullScreenRequested,
            ui.actionFullscreen, &QAction::trigger);
    connect(m_mainWindowHeader, &MainWindowHeader::configureLinkRequested,
            this, [this](int linkId) { configLink(linkId); });
    ui.menuBar->hide();

    // Mission Planner 10 shell contract: 1280x800 initial geometry with a
    // 1120x720 usable minimum on all desktop platforms.
    setMinimumSize(1120, 720);

    customStatusBar = new QGCStatusBar(this);
    setStatusBar(customStatusBar);
    statusBar()->setSizeGripEnabled(true);
    statusBar()->hide();


    buildCommonWidgets();
    connectCommonWidgets();

    // Create actions
    connectCommonActions();

    buildMissionPlannerToolsMenu();

    // Populate link menu
    QList<int> links = LinkManager::instance()->getLinks();
    for (int i=0;i<links.size();i++)
    {
        addLink(links.at(i));
    }

    connect(LinkManager::instance(), SIGNAL(newLink(int)), this, SLOT(addLink(int)), Qt::QueuedConnection);
    connect(LinkManager::instance(),SIGNAL(linkError(int,QString)),this,SLOT(linkError(int,QString)));

    connect(ui.actionTerminalConsole, SIGNAL(triggered()), this, SLOT(showTerminalConsole()));

    // Connect user interface devices
    joystickWidget = 0;
    joystick = new JoystickInput();

#ifdef MOUSE_ENABLED_WIN

    mouseInput = new Mouse3DInput(this);
    mouse = new Mouse6dofInput(mouseInput);
#endif //MOUSE_ENABLED_WIN

#if MOUSE_ENABLED_LINUX

    mouse = new Mouse6dofInput(this);
    connect(this, SIGNAL(x11EventOccured(XEvent*)), mouse, SLOT(handleX11Event(XEvent*)));
#endif //MOUSE_ENABLED_LINUX

    // Set low power mode
    enableLowPowerMode(lowPowerMode);

    // Set Automatic use of system Proxies
    enableAutoProxyMode(autoProxyMode);

    // Initialize window state
    windowStateVal = windowState();

    // Restore the window setup
    loadViewState();

    // Restore the window position and size
    if (settings.contains(getWindowGeometryKey()))
    {
        // Restore the window geometry
        restoreGeometry(settings.value(getWindowGeometryKey()).toByteArray());
    }
    else
    {
        // Adjust the size
        QScreen *pScreen = QGuiApplication::primaryScreen();
        const QRect rect = pScreen->availableGeometry();
        resize(qMin(1280, rect.width()), qMin(800, rect.height()));
    }
    show();

    connect(&windowNameUpdateTimer, SIGNAL(timeout()), this, SLOT(configureWindowName()));
    windowNameUpdateTimer.start(15000);

    ui.actionDeveloper_Credits->setVisible(false);
    ui.actionOnline_Documentation->setVisible(false);
    ui.actionProject_Roadmap_2->setVisible(false);
    show();


    //Disable firmware update and unconnected view buttons, as they aren't required for the moment.
    ui.actionFirmwareUpdateView->setVisible(false);
    ui.actionUnconnectedView->setVisible(false);

    // Keep the established modal flow for the menu action and background
    // notifications, while the Mission Planner Help page receives scoped
    // inline completion for its explicit stable/beta checks.
    connect(&m_autoUpdateCheck, SIGNAL(updateAvailable(QString,QString,QString,QString)),
            this, SLOT(showAutoUpdateDownloadDialog(QString,QString,QString,QString)));
    connect(&m_autoUpdateCheck, SIGNAL(noUpdateAvailable()),
            this, SLOT(showNoUpdateAvailDialog()));
    connect(helpView, &HelpView::checkForUpdatesRequested, this, [this]() {
        if (!m_autoUpdateCheck.checkForUpdates(AutoUpdateCheck::Stable,
                                               AutoUpdateCheck::Inline)) {
            helpView->setUpdateCheckInProgress(
                false, tr("An update check is already in progress."));
        }
    });
    connect(helpView, &HelpView::checkForBetaUpdatesRequested, this, [this]() {
        if (!m_autoUpdateCheck.checkForUpdates(AutoUpdateCheck::Beta,
                                               AutoUpdateCheck::Inline)) {
            helpView->setUpdateCheckInProgress(
                false, tr("An update check is already in progress."));
        }
    });
    connect(&m_autoUpdateCheck, &AutoUpdateCheck::checkNoUpdate,
            this, [this](AutoUpdateCheck::ReleaseChannel channel,
                         AutoUpdateCheck::Presentation presentation) {
                if (presentation != AutoUpdateCheck::Inline) {
                    return;
                }
                helpView->setUpdateCheckInProgress(
                    false, channel == AutoUpdateCheck::Beta
                        ? tr("No new beta update available.")
                        : tr("No new update available."));
            });
    connect(&m_autoUpdateCheck, &AutoUpdateCheck::checkFailed,
            this, [this](AutoUpdateCheck::ReleaseChannel channel,
                         AutoUpdateCheck::Presentation presentation,
                         const QString &reason) {
                if (presentation != AutoUpdateCheck::Inline) {
                    if (presentation == AutoUpdateCheck::Modal) {
                        QMessageBox::warning(
                            this, tr("Update Check"),
                            tr("Update check failed: %1").arg(reason));
                    }
                    return;
                }
                helpView->setUpdateCheckInProgress(
                    false, channel == AutoUpdateCheck::Beta
                        ? tr("Beta update check failed: %1").arg(reason)
                        : tr("Update check failed: %1").arg(reason));
            });
    connect(&m_autoUpdateCheck, &AutoUpdateCheck::checkAvailable,
            this, [this](AutoUpdateCheck::ReleaseChannel,
                         AutoUpdateCheck::Presentation presentation,
                         const QString &version, const QString &releaseType,
                         const QString &url, const QString &name) {
                if (presentation != AutoUpdateCheck::Inline) {
                    return;
                }
                helpView->setUpdateCheckInProgress(
                    false, tr("Version %1 is available.").arg(version));
                showAutoUpdateDownloadDialog(version, releaseType, url, name);
            });
    if (m_autoUpdateCheck.isUpdateEnabled()) {
        QTimer::singleShot(5000, &m_autoUpdateCheck, SLOT(autoUpdateCheck()));
    }

}

MainWindow::~MainWindow()
{
    // Logging remains active while child widgets are being destroyed. Detach
    // the GUI sink first so destructor messages cannot append to a QTextEdit
    // which is itself already in QObject teardown.
    LogWindowSingleton::instance().removeDebugOutput();
    debugOutput.clear();

    // Inspector windows contain receivers for both the live LinkManager stream
    // and the log-player replay relay. Destroy every receiver while both
    // publishers are still alive; QObject then removes all subscriptions
    // synchronously and no destroyed-lambda needs to touch a raw logPlayer.
    closeMavlinkInspectorWindows();

    closeTerminalConsole();

    if (joystickWidget)
    {
        QLOG_DEBUG() << "Delete JoystickWidget";

        delete joystickWidget;
        joystickWidget = NULL;
    }
    if (joystick)
    {
        joystick->shutdown();
        joystick->wait(5000);
        delete joystick;
        joystick = NULL;
    }

    // All communication dialogs and embedded widgets have this window as
    // their QObject parent. Let QObject destroy each child exactly once.
    commsWidgetList.clear();
}

void MainWindow::buildMissionPlannerToolsMenu()
{
    MissionPlannerToolsMenu::HandlerMap handlers;
    handlers.insert(QStringLiteral("actionDeveloperTools"),
                    [this]() { showDeveloperTools(); });
    handlers.insert(QStringLiteral("actionPluginManager"),
                    [this]() { showPluginManager(); });
    handlers.insert(QStringLiteral("actionMavlinkInspector"),
                    [this]() { showMavlinkInspector(); });
    handlers.insert(QStringLiteral("actionMapTileCache"),
                    [this]() { MapCacheView::OpenWindow(this); });
    handlers.insert(QStringLiteral("actionLinkStatistics"),
                    [this]() { LinkStatsWindow::OpenWindow(this); });
    handlers.insert(QStringLiteral("actionConnectionOptions"),
                    [this]() { showConnectionOptions(); });
    handlers.insert(QStringLiteral("actionDownloadLogs"),
                    [this]() { showLogDownload(); });

    MissionPlannerToolsMenu::Populate(ui.menuTools, this, handlers);
    // The native menu bar is hidden. Register every MP10 shortcut directly on
    // MainWindow so it remains active when the header auto-hides.
    addActions(ui.menuTools->actions());
    ui.menuTools->menuAction()->setVisible(true);
    m_mainWindowHeader->setConnectionOptionsAction(
        findChild<QAction *>(QStringLiteral("actionConnectionOptions")));
}

void MainWindow::disableTLogReplayBar()
{
    statusBar()->hide();
}

void MainWindow::enableTLogReplayBar()
{
    statusBar()->show();
}

void MainWindow::loadTlogMenuClicked()
{
    //QString fileName = QFileDialog::getOpenFileName(this, tr("Specify MAVLink log file name to replay"), QGC::MAVLinkLogDirectory(), tr("MAVLink Telemetry log (*.tlog)"));
    //if (fileName == "")
    //{
        //No file selected/cancel clicked
        return;
    //}
    //statusBar()->show();
    //customStatusBar->logPlayer()->loadLog(fileName);
}

void MainWindow::resizeEvent(QResizeEvent * event)
{
    QMainWindow::resizeEvent(event);
}

QString MainWindow::getWindowStateKey()
{
    if (UASManager::instance()->getActiveUAS())
    {
        return QString::number(currentView)+"_windowstate_" + UASManager::instance()->getActiveUAS()->getAutopilotTypeName();
    }
    else
        return QString::number(currentView)+"_windowstate";
}

QString MainWindow::getWindowGeometryKey()
{
    //return QString::number(currentView)+"_geometry";
    return "_geometry";
}

void MainWindow::buildCustomWidget()
{
    // Create custom widgets
    QList<QGCToolWidget*> widgets = QGCToolWidget::createWidgetsFromSettings(this);

    if (widgets.size() > 0)
    {
        ui.menuTools->addSeparator();
    }

    for(int i = 0; i < widgets.size(); ++i)
    {
        // Check if this widget already has a parent, do not create it in this case
        QGCToolWidget* tool = widgets.at(i);
        QDockWidget* dock = dynamic_cast<QDockWidget*>(tool->parentWidget());
        if (!dock)
        {
            QSettings settings;
            settings.beginGroup("QGC_MAINWINDOW");

            /*QDockWidget* dock = new QDockWidget(tool->windowTitle(), this);
            dock->setObjectName(tool->objectName()+"_DOCK");
            dock->setWidget(tool);
            connect(tool, SIGNAL(destroyed()), dock, SLOT(deleteLater()));
            QAction* showAction = new QAction(widgets.at(i)->windowTitle(), this);
            showAction->setCheckable(true);
            connect(showAction, SIGNAL(triggered(bool)), dock, SLOT(setVisible(bool)));
            connect(dock, SIGNAL(visibilityChanged(bool)), showAction, SLOT(setChecked(bool)));
            widgets.at(i)->setMainMenuAction(showAction);
            ui.menuTools->addAction(showAction);*/

            // Load dock widget location (default is bottom)
            Qt::DockWidgetArea location = static_cast <Qt::DockWidgetArea>(tool->getDockWidgetArea(currentView));

            //addDockWidget(location, dock);
            //dock->hide();
            int view = settings.value(QString("TOOL_PARENT_") + tool->objectName(),-1).toInt();
            //settings.setValue(QString("TOOL_PARENT_") + "UNNAMED_TOOL_" + QString::number(ui.menuTools->actions().size()),currentView);
            settings.endGroup();

            QDockWidget* dock;

            switch (view)
            {
            case VIEW_ENGINEER:
                dock = createDockWidget(engineeringView,tool,tool->getTitle(),tool->objectName(),(VIEW_SECTIONS)view,location);
                break;
            case VIEW_FLIGHT:
                dock = createDockWidget(pilotView,tool,tool->getTitle(),tool->objectName(),(VIEW_SECTIONS)view,location);
                break;
            case VIEW_SIMULATION:
                dock = createDockWidget(simView,tool,tool->getTitle(),tool->objectName(),(VIEW_SECTIONS)view,location);
                break;
            case VIEW_MISSION:
                dock = createDockWidget(plannerView,tool,tool->getTitle(),tool->objectName(),(VIEW_SECTIONS)view,location);
                break;
            case VIEW_MAVLINK:
                dock = createDockWidget(mavlinkView,tool,tool->getTitle(),tool->objectName(),(VIEW_SECTIONS)view,location);
                break;
            default:
                dock = createDockWidget(centerStack->currentWidget(),tool,tool->getTitle(),tool->objectName(),(VIEW_SECTIONS)view,location);
                break;
            }

            // XXX temporary "fix"
            if (dock) {
                dock->hide();
            }

            //createDockWidget(0,tool,tool->getTitle(),tool->objectName(),view,location);
        }
    }
}

MainWindowHeader& MainWindow::toolBar()
{
    return *m_mainWindowHeader;
}

void MainWindow::buildCommonWidgets()
{
    //TODO:  move protocol outside UI
    //mavlink     = new MAVLinkProtocol();
   // connect(mavlink, SIGNAL(protocolStatusMessage(QString,QString)), this, SLOT(showCriticalMessage(QString,QString)), Qt::QueuedConnection);
    connect(LinkManager::instance(),SIGNAL(protocolStatusMessage(QString,QString)),this,SLOT(showCriticalMessage(QString,QString)));
    // Add generic MAVLink decoder
    //mavlinkDecoder = new MAVLinkDecoder(mavlink, this);

    // Log player
    logPlayer = new QGCMAVLinkLogPlayer(customStatusBar);
    //logPlayer->setMavlinkDecoder(mavlinkDecoder);
    connect(logPlayer,SIGNAL(logFinished()),statusBar(),SLOT(hide()));
    customStatusBar->setLogPlayer(logPlayer);

    // Center widgets
    if (!plannerView)
    {
        plannerView = new FlightPlannerView(this);
        connect(plannerView, &DockableView::layoutRestoreRejected,
                this, [this](const QString &reason) {
                    QLOG_WARN() << "FlightPlannerView layout reset:" << reason;
                    showStatusMessage(tr("PLAN layout was reset: %1").arg(reason));
                });
        plannerViewModel = new FlightPlannerViewModel(plannerView);
        ElevationSourceService *elevationService =
            ElevationSourceService::instance();
        plannerViewModel->setTerrainAltitudeProvider(
            [elevationService](double latitude, double longitude,
                               double *altitudeAmslMeters) {
                return elevationService
                    && elevationService->sampleAltitude(
                        latitude, longitude, altitudeAmslMeters);
            });
        connect(elevationService,
                &ElevationSourceService::srtmTileAvailable,
                plannerViewModel,
                [this](const QString &tileName) {
            plannerViewModel->setStatus(
                tr("Terrain tile %1 is ready; repeat the altitude-verified "
                   "operation to apply it.").arg(tileName));
        });
        connect(elevationService,
                &ElevationSourceService::srtmDownloadFailed,
                plannerViewModel,
                [this](const QString &tileName, const QString &error) {
            plannerViewModel->setStatus(
                tr("Terrain tile %1 could not be downloaded: %2")
                    .arg(tileName, error));
        });
        plannerViewModel->setWpRadius(
            settings.value(QStringLiteral("FlightPlanner/TXT_WPRad"),
                           plannerViewModel->WpRadius()).toDouble());
        plannerViewModel->setLoiterRadius(
            settings.value(QStringLiteral("FlightPlanner/TXT_loiterrad"),
                           plannerViewModel->LoiterRadius()).toDouble());
        plannerViewModel->setDefaultAltitude(
            settings.value(QStringLiteral("FlightPlanner/TXT_DefaultAlt"),
                           plannerViewModel->DefaultAltitude()).toDouble());
        plannerViewModel->setDefaultFrame(
            settings.value(QStringLiteral("FlightPlanner/CMB_altmode"),
                           plannerViewModel->DefaultFrame()).toString());
        plannerViewModel->setAltWarn(
            settings.value(QStringLiteral("FlightPlanner/TXT_altwarn"),
                           plannerViewModel->AltWarn()).toDouble());
        plannerViewModel->setSplineDefault(
            settings.value(QStringLiteral("FlightPlanner/CHK_splinedefault"),
                           plannerViewModel->SplineDefault()).toBool());
        plannerViewModel->setVerifyHeight(
            settings.value(QStringLiteral("FlightPlanner/CHK_verifyheight"),
                           plannerViewModel->VerifyHeight()).toBool());
        plannerViewModel->setAltUnits(
            settings.value(QStringLiteral("altunits"),
                           QStringLiteral("Meters")).toString());
        plannerViewModel->setDistUnits(
            settings.value(QStringLiteral("distunits"),
                           QStringLiteral("Meters")).toString());
        connect(plannerViewModel, &FlightPlannerViewModel::wpRadiusChanged,
                this, [this](double value) {
            settings.setValue(QStringLiteral("FlightPlanner/TXT_WPRad"), value);
        });
        connect(plannerViewModel, &FlightPlannerViewModel::loiterRadiusChanged,
                this, [this](double value) {
            settings.setValue(
                QStringLiteral("FlightPlanner/TXT_loiterrad"), value);
        });
        connect(plannerViewModel,
                &FlightPlannerViewModel::defaultAltitudeChanged,
                this, [this](double value) {
            settings.setValue(
                QStringLiteral("FlightPlanner/TXT_DefaultAlt"), value);
        });
        connect(plannerViewModel, &FlightPlannerViewModel::defaultFrameChanged,
                this, [this](const QString &value) {
            settings.setValue(
                QStringLiteral("FlightPlanner/CMB_altmode"), value);
        });
        connect(plannerViewModel, &FlightPlannerViewModel::altWarnChanged,
                this, [this](double value) {
            settings.setValue(
                QStringLiteral("FlightPlanner/TXT_altwarn"), value);
        });
        connect(plannerViewModel, &FlightPlannerViewModel::splineDefaultChanged,
                this, [this](bool enabled) {
            settings.setValue(
                QStringLiteral("FlightPlanner/CHK_splinedefault"), enabled);
        });
        connect(plannerViewModel, &FlightPlannerViewModel::verifyHeightChanged,
                this, [this](bool enabled) {
            settings.setValue(
                QStringLiteral("FlightPlanner/CHK_verifyheight"), enabled);
        });
        connect(plannerViewModel, &FlightPlannerViewModel::altUnitsChanged,
                this, [this](const QString &value) {
            settings.setValue(QStringLiteral("altunits"), value);
            emit plannerAltitudeUnitsChanged(value);
        });
        connect(plannerViewModel, &FlightPlannerViewModel::distUnitsChanged,
                this, [this](const QString &value) {
            settings.setValue(QStringLiteral("distunits"), value);
            emit plannerDistanceUnitsChanged(value);
        });
        plannerMapTool = new QGCMapTool(
            MapWidgetRole::FlightPlanner, plannerView);
        plannerMapTool->setObjectName(QStringLiteral("PlannerMap"));
        plannerMapTool->mapWidget()->SetMissionPlanningEnabled(true);
        plannerView->setMapWidget(plannerMapTool);
        addToCentralStackedWidget(plannerView, VIEW_MISSION, "Maps");
    }

    //pilotView (aka Flight or Mission View)
    if (!pilotView)
    {
        pilotView = new FlightDataView(this);
        connect(pilotView, &DockableView::layoutRestoreRejected,
                this, [this](const QString &reason) {
                    QLOG_WARN() << "FlightDataView layout reset:" << reason;
                    showStatusMessage(tr("DATA layout was reset: %1").arg(reason));
                });
        addToCentralStackedWidget(pilotView, VIEW_FLIGHT, "Pilot");
    }

    if (!configView)
    {
        configView = new SubMainWindow(this);
        configView->setObjectName("VIEW_HARDWARE_CONFIG");
        hardwareSetupView = new SetupView(this);
        configView->setCentralWidget(hardwareSetupView);
        addToCentralStackedWidget(configView,VIEW_HARDWARE_CONFIG, tr("Hardware"));
        connect(ui.actionAdvanced_Mode, SIGNAL(toggled(bool)),
                hardwareSetupView, SLOT(advModeChanged(bool)));
        hardwareSetupView->advModeChanged(isAdvancedMode);
    }

    if (!softwareConfigView)
    {
        softwareConfigView = new SubMainWindow(this);
        softwareConfigView->setObjectName("VIEW_SOFTWARE_CONFIG");
        ConfigView *configPage = new ConfigView(this);
        softwareConfigView->setCentralWidget(configPage);
        addToCentralStackedWidget(softwareConfigView, VIEW_SOFTWARE_CONFIG, tr("Software"));
        connect(ui.actionAdvanced_Mode, SIGNAL(toggled(bool)), configPage, SLOT(advModeChanged(bool)));
    }

    if (!helpView)
    {
        helpView = new HelpView(this);
        addToCentralStackedWidget(helpView, VIEW_HELP, tr("Help"));
    }

     AP2DataPlot2D *plot = NULL;
    if (!engineeringView)
    {
        engineeringView = new SubMainWindow(this);
        engineeringView->setObjectName("VIEW_ENGINEER");
        //engineeringView->setCentralWidget(new QGCDataPlot2D(this));
        plot = new AP2DataPlot2D(this);
        connect(logPlayer,SIGNAL(logLoaded()),plot,SLOT(clearGraph()));
        engineeringView->setCentralWidget(plot);

        addToCentralStackedWidget(engineeringView, VIEW_ENGINEER, tr("Logfile Plot"));
    }

    if (!mavlinkView)
    {
        //mavlinkView = new SubMainWindow(this);
        //mavlinkView->setObjectName("VIEW_MAVLINK");
        //mavlinkView->setCentralWidget(new XMLCommProtocolWidget(this));
        //addToCentralStackedWidget(mavlinkView, VIEW_MAVLINK, tr("Mavlink Generator"));
    }

    if (!simView)
    {
        simView = new SubMainWindow(this);
        simView->setObjectName("VIEW_SIMULATOR");
        simView->setCentralWidget(new QGCMapTool(
            MapWidgetRole::Simulation, this));
        addToCentralStackedWidget(simView, VIEW_SIMULATION, tr("Simulation View"));
    }

    if (!debugOutput)
    {
       debugOutput = DebugOutput::Ptr(new DebugOutput);
       LogWindowSingleton::instance().setDebugOutput(debugOutput);
    }

    // Dock widgets
    QAction* tempAction = ui.menuTools->addAction(tr("Control"));
    tempAction->setCheckable(true);
    connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));

    createDockWidget(simView,new UASControlWidget(this),tr("Control"),"UNMANNED_SYSTEM_CONTROL_DOCKWIDGET",VIEW_SIMULATION,Qt::LeftDockWidgetArea);
    auto *plannerActionPanel = new FlightPlannerActionPanel(
        plannerViewModel, plannerView);
    if (plannerView->setActionPanel(plannerActionPanel)) {
        plannerActionPanel->show();
        registerDockablePanel(plannerView, VIEW_MISSION,
                              FlightPlannerView::actionPanelId(),
                              tr("Actions"), plannerActionPanel);
    }
    auto *plannerWaypointPanel = new FlightPlannerWaypointPanel(
        plannerViewModel, plannerView);
    if (plannerView->setWaypointPanel(plannerWaypointPanel)) {
        registerDockablePanel(plannerView, VIEW_MISSION,
                              FlightPlannerView::waypointPanelId(),
                              tr("Waypoints"), plannerWaypointPanel);
    }

    AbstractMapWidget *plannerMap = plannerMapTool->mapWidget();
    plannerWaypointPanel->setZoomRange(
        plannerMap->MinZoom(), plannerMap->MaxZoom());
    plannerWaypointPanel->setZoomLevel(plannerMap->CurrentZoomLevel());
    connect(plannerWaypointPanel,
            &FlightPlannerWaypointPanel::zoomLevelRequested,
            plannerMap, [plannerMap](int level) {
        plannerMap->SetZoom(level);
    });
    connect(plannerMap, &AbstractMapWidget::ZoomChanged,
            plannerWaypointPanel,
            &FlightPlannerWaypointPanel::setZoomLevel);
    auto *plannerPrefetchController = new FlightPlannerPrefetchController(
        plannerMap, plannerViewModel, plannerView);

    // Mission Planner-compatible map actions live above the concrete map
    // backend. Each backend only reports the clicked geographic coordinate;
    // the shared ViewModel owns validation, undo and mission mutation.
    auto *plannerContextMenu = new QMenu(plannerView);
    plannerContextMenu->setObjectName(QStringLiteral("contextMenuStrip1"));
    plannerContextMenu->setProperty("plannerWaypointSequence", -1);
    const auto addPlannerAction = [plannerContextMenu](
            const QString &text, const char *objectName) {
        QAction *action = plannerContextMenu->addAction(text);
        action->setObjectName(QString::fromLatin1(objectName));
        return action;
    };
    QAction *deleteWaypointAction = addPlannerAction(
        tr("Delete WP"), "deleteWPToolStripMenuItem");
    auto *insertWaypointMenu = plannerContextMenu->addMenu(tr("Insert Wp"));
    insertWaypointMenu->setObjectName(
        QStringLiteral("insertWpToolStripMenuItem"));
    QAction *insertWaypointAction = insertWaypointMenu->menuAction();
    insertWaypointAction->setObjectName(
        QStringLiteral("insertWpToolStripMenuItem"));
    QAction *currentPositionAction = insertWaypointMenu->addAction(
        tr("At Current Position"));
    currentPositionAction->setObjectName(
        QStringLiteral("currentPositionToolStripMenuItem"));
    new MenuSplitActionFilter(plannerContextMenu, insertWaypointAction);
    QAction *insertSplineWaypointAction = addPlannerAction(
        tr("Insert Spline WP"), "insertSplineWPToolStripMenuItem");
    auto *loiterMenu = plannerContextMenu->addMenu(tr("Loiter"));
    loiterMenu->setObjectName(QStringLiteral("loiterToolStripMenuItem"));
    loiterMenu->menuAction()->setObjectName(
        QStringLiteral("loiterToolStripMenuItem"));
    QAction *loiterForeverAction = loiterMenu->addAction(tr("Forever"));
    loiterForeverAction->setObjectName(
        QStringLiteral("loiterForeverToolStripMenuItem"));
    QAction *loiterTimeAction = loiterMenu->addAction(tr("Time"));
    loiterTimeAction->setObjectName(
        QStringLiteral("loitertimeToolStripMenuItem"));
    QAction *loiterCirclesAction = loiterMenu->addAction(tr("Circles"));
    loiterCirclesAction->setObjectName(
        QStringLiteral("loitercirclesToolStripMenuItem"));
    auto *jumpMenu = plannerContextMenu->addMenu(tr("Jump"));
    jumpMenu->setObjectName(QStringLiteral("jumpToolStripMenuItem"));
    jumpMenu->menuAction()->setObjectName(
        QStringLiteral("jumpToolStripMenuItem"));
    QAction *jumpStartAction = jumpMenu->addAction(tr("Start"));
    jumpStartAction->setObjectName(
        QStringLiteral("jumpstartToolStripMenuItem"));
    QAction *jumpWaypointAction = jumpMenu->addAction(tr("WP #"));
    jumpWaypointAction->setObjectName(
        QStringLiteral("jumpwPToolStripMenuItem"));
    QAction *rtlAction = addPlannerAction(
        tr("RTL"), "rTLToolStripMenuItem");
    QAction *landAction = addPlannerAction(
        tr("Land"), "landToolStripMenuItem");
    QAction *takeoffAction = addPlannerAction(
        tr("Takeoff"), "takeoffToolStripMenuItem");
    QAction *roiAction = addPlannerAction(
        tr("DO_SET_ROI"), "setROIToolStripMenuItem");
    QAction *clearMissionAction = addPlannerAction(
        tr("Clear Mission"), "clearMissionToolStripMenuItem");
    plannerContextMenu->addSeparator();
    QAction *reverseWaypointsAction = addPlannerAction(
        tr("Reverse WPs"), "reverseWPsToolStripMenuItem");
    QAction *modifyAltitudeAction = addPlannerAction(
        tr("Modify Alt"), "modifyAltToolStripMenuItem");
    QAction *elevationGraphAction = addPlannerAction(
        tr("Elevation Graph"), "elevationGraphToolStripMenuItem");
    plannerContextMenu->addSeparator();
    QAction *undoAction = addPlannerAction(
        tr("Undo"), "undoToolStripMenuItem");
    auto *mapToolMenu = plannerContextMenu->addMenu(tr("Map Tool"));
    mapToolMenu->setObjectName(QStringLiteral("mapToolToolStripMenuItem"));
    mapToolMenu->menuAction()->setObjectName(
        QStringLiteral("mapToolToolStripMenuItem"));
    QAction *measureAction = mapToolMenu->addAction(
        tr("Measure Distance"));
    measureAction->setObjectName(QStringLiteral("ContextMeasure"));
    if (QAction *action =
            plannerPrefetchController->PrefetchVisibleAreaAction()) {
        mapToolMenu->addAction(action);
    }
    if (QAction *action =
            plannerPrefetchController->PrefetchWaypointPathAction()) {
        mapToolMenu->addAction(action);
    }
    mapToolMenu->menuAction()->setEnabled(!mapToolMenu->isEmpty());
    QAction *setHomeAction = addPlannerAction(
        tr("Set Home Here"), "setHomeHereToolStripMenuItem");
    QAction *setFenceReturnAction = addPlannerAction(
        tr("Set Return Location"), "setReturnLocationToolStripMenuItem");
    setFenceReturnAction->setVisible(false);

    const QList<QAction *> missionContextActions{
        deleteWaypointAction, insertWaypointAction,
        insertSplineWaypointAction, loiterMenu->menuAction(),
        jumpMenu->menuAction(), rtlAction, landAction, takeoffAction,
        roiAction, clearMissionAction, reverseWaypointsAction,
        modifyAltitudeAction, elevationGraphAction,
    };
    const auto contextLatitude = [plannerContextMenu]() {
        return plannerContextMenu->property("plannerLatitude").toDouble();
    };
    const auto contextLongitude = [plannerContextMenu]() {
        return plannerContextMenu->property("plannerLongitude").toDouble();
    };
    const auto contextWaypointSequence = [plannerContextMenu]() {
        return plannerContextMenu->property(
            "plannerWaypointSequence").toInt();
    };
    connect(insertWaypointAction, &QAction::triggered, plannerView,
            [this, plannerWaypointPanel,
             contextLatitude, contextLongitude]() {
        const int count = plannerViewModel->Waypoints()->storeRowCount(
            FlightPlannerMissionModel::MissionStore::Mission);
        const int selected = plannerWaypointPanel->selectedWaypoint();
        const int suggested = selected >= 0 ? selected + 1 : count;
        bool accepted = false;
        const int index = QInputDialog::getInt(
            plannerView, tr("Insert WP"), tr("Insert WP after wp#"),
            suggested, 0, count, 1, &accepted);
        if (!accepted) return;
        plannerViewModel->InsertRegularWaypointAt(
            index, contextLatitude(), contextLongitude(),
            plannerViewModel->DefaultAltitude());
    });
    connect(insertSplineWaypointAction, &QAction::triggered, plannerView,
            [this, plannerWaypointPanel,
             contextLatitude, contextLongitude]() {
        const int count = plannerViewModel->Waypoints()->storeRowCount(
            FlightPlannerMissionModel::MissionStore::Mission);
        const int selected = plannerWaypointPanel->selectedWaypoint();
        const int suggested = selected >= 0 ? selected + 1 : count;
        bool accepted = false;
        const int index = QInputDialog::getInt(
            plannerView, tr("Insert WP"), tr("Insert WP after wp#"),
            suggested, 0, count, 1, &accepted);
        if (!accepted) return;
        plannerViewModel->InsertSplineWaypointAt(
            index, contextLatitude(), contextLongitude(),
            plannerViewModel->DefaultAltitude());
    });
    connect(currentPositionAction, &QAction::triggered, plannerViewModel,
            &FlightPlannerViewModel::AddWaypointAtCurrentPosition);
    connect(deleteWaypointAction, &QAction::triggered, plannerView,
            [this, contextWaypointSequence]() {
        const int sequence = contextWaypointSequence();
        if (sequence >= 0) plannerViewModel->DeleteWaypoint(sequence);
    });
    connect(setHomeAction, &QAction::triggered, plannerView,
            [this, contextLatitude, contextLongitude]() {
        plannerViewModel->SetHome(contextLatitude(), contextLongitude());
    });
    connect(setFenceReturnAction, &QAction::triggered, plannerView,
            [this, contextLatitude, contextLongitude]() {
        plannerViewModel->SetFenceReturn(
            contextLatitude(), contextLongitude());
    });
    connect(takeoffAction, &QAction::triggered, plannerView,
            [this, contextLatitude, contextLongitude]() {
        plannerViewModel->AddTakeoff(
            contextLatitude(), contextLongitude(),
            plannerViewModel->DefaultAltitude());
    });
    connect(landAction, &QAction::triggered, plannerView,
            [this, contextLatitude, contextLongitude]() {
        plannerViewModel->AddLand(contextLatitude(), contextLongitude());
    });
    connect(rtlAction, &QAction::triggered, plannerView,
            [this]() { plannerViewModel->AddRtl(); });
    connect(roiAction, &QAction::triggered, plannerView,
            [this, contextLatitude, contextLongitude]() {
        plannerViewModel->AddRoi(contextLatitude(), contextLongitude());
    });
    connect(loiterForeverAction, &QAction::triggered, plannerView,
            [this, contextLatitude, contextLongitude]() {
        plannerViewModel->AddLoiterForever(
            contextLatitude(), contextLongitude());
    });
    connect(loiterTimeAction, &QAction::triggered, plannerView,
            [this, contextLatitude, contextLongitude]() {
        bool accepted = false;
        const double seconds = QInputDialog::getDouble(
            plannerView, tr("Loiter Time"), tr("Loiter Time"),
            5.0, 0.0, 86400.0, 2, &accepted);
        if (accepted) {
            plannerViewModel->AddLoiterTime(
                contextLatitude(), contextLongitude(), seconds);
        }
    });
    connect(loiterCirclesAction, &QAction::triggered, plannerView,
            [this, contextLatitude, contextLongitude]() {
        bool accepted = false;
        const double turns = QInputDialog::getDouble(
            plannerView, tr("Loiter Turns"), tr("Loiter Turns"),
            3.0, 0.01, 100000.0, 2, &accepted);
        if (accepted) {
            plannerViewModel->AddLoiterTurns(
                contextLatitude(), contextLongitude(), turns);
        }
    });
    connect(jumpStartAction, &QAction::triggered, plannerView,
            [this]() {
        bool accepted = false;
        const int repeats = QInputDialog::getInt(
            plannerView, tr("Jump repeat"),
            tr("Number of times to Repeat"), 5, -1, 32767, 1,
            &accepted);
        if (accepted) plannerViewModel->AddJump(1, repeats);
    });
    connect(jumpWaypointAction, &QAction::triggered, plannerView,
            [this]() {
        const int count = plannerViewModel->Waypoints()->storeRowCount(
            FlightPlannerMissionModel::MissionStore::Mission);
        bool accepted = false;
        const int target = QInputDialog::getInt(
            plannerView, tr("WP No"), tr("Jump to WP no?"),
            1, 1, count + 1, 1, &accepted);
        if (!accepted) return;
        const int repeats = QInputDialog::getInt(
            plannerView, tr("Jump repeat"),
            tr("Number of times to Repeat"), 5, -1, 32767, 1,
            &accepted);
        if (accepted) plannerViewModel->AddJump(target, repeats);
    });
    connect(clearMissionAction, &QAction::triggered, plannerViewModel,
            &FlightPlannerViewModel::ClearWaypoints);
    connect(reverseWaypointsAction, &QAction::triggered, plannerView,
            [this]() { plannerViewModel->ReverseWaypoints(); });
    connect(modifyAltitudeAction, &QAction::triggered, plannerView,
            [this]() {
        bool accepted = false;
        const QString expression = QInputDialog::getText(
            plannerView, tr("Modify Alt"),
            tr("Enter +value (%1) or *factor for all mission altitudes:")
                .arg(plannerViewModel->AltUnit()),
            QLineEdit::Normal, QStringLiteral("+0"), &accepted);
        if (accepted) plannerViewModel->ModifyAllAlt(expression);
    });
    connect(elevationGraphAction, &QAction::triggered,
            this, &MainWindow::showMissionElevation);
    const auto measurement = std::make_shared<FlightPlannerMeasurement>();
    connect(measureAction, &QAction::triggered, plannerView,
            [this, plannerMap, measurement,
             contextLatitude, contextLongitude]() {
        FlightPlannerMeasurement::Result completed;
        const FlightPlannerMeasurement::Step step = measurement->AddPoint(
            contextLatitude(), contextLongitude(), &completed);
        if (step == FlightPlannerMeasurement::Step::Rejected) return;

        if (step == FlightPlannerMeasurement::Step::Started) {
            const FlightPlannerMeasurement::Point start = measurement->Start();
            plannerMap->SetPlannerMeasurement({
                {start.latitude, start.longitude, 0.0},
            });
            QMessageBox::information(
                plannerView, tr("Measure Dist"),
                tr("You can now pan/zoom around.\n"
                   "Click this option again to get the distance."));
            return;
        }

        plannerMap->SetPlannerMeasurement({
            {completed.start.latitude, completed.start.longitude, 0.0},
            {completed.end.latitude, completed.end.longitude, 0.0},
        });
        const QString distance = FlightPlannerMeasurement::FormatDistance(
            completed.distanceMeters,
            plannerViewModel->DistanceMultiplier(),
            plannerViewModel->DistanceUnit());
        QMessageBox::information(
            plannerView, tr("Measure Dist"),
            tr("Distance: %1 AZ: %2")
                .arg(distance)
                .arg(completed.bearingDegrees, 0, 'f', 0));
        plannerMap->SetPlannerMeasurement({});
    });
    connect(undoAction, &QAction::triggered, plannerView,
            [this]() { plannerViewModel->Undo(); });
    connect(plannerContextMenu, &QMenu::aboutToShow, plannerView,
            [this, plannerContextMenu, missionContextActions, setHomeAction,
             setFenceReturnAction,
             deleteWaypointAction, reverseWaypointsAction,
             elevationGraphAction, undoAction]() {
        const bool editable = !plannerViewModel->TransferBusy();
        const bool mission = plannerViewModel->MissionType()
                == QStringLiteral("Mission");
        const bool fence = plannerViewModel->MissionType()
                == QStringLiteral("Fence");
        for (QAction *action : missionContextActions) {
            action->setVisible(mission);
            action->setEnabled(editable);
        }
        setHomeAction->setEnabled(editable);
        setFenceReturnAction->setVisible(fence);
        setFenceReturnAction->setEnabled(editable && fence);
        const int missionCount = plannerViewModel->Waypoints()->storeRowCount(
            FlightPlannerMissionModel::MissionStore::Mission);
        const int waypointSequence = plannerContextMenu->property(
            "plannerWaypointSequence").toInt();
        deleteWaypointAction->setEnabled(
            editable && mission && waypointSequence >= 0
            && waypointSequence < missionCount);
        reverseWaypointsAction->setEnabled(editable && missionCount > 1);
        int routePointCount = plannerViewModel->HomeValid() ? 1 : 0;
        const QVector<WpRowData> missionRows =
            plannerViewModel->Waypoints()->rows(
                FlightPlannerMissionModel::MissionStore::Mission);
        for (const WpRowData &row : missionRows) {
            if (MissionElevationProfile::CommandIsRoutePoint(row.Command)
                && WpRow::FrameHasGlobalLocation(row.Frame)
                && (row.Lat != 0.0 || row.Lng != 0.0)) {
                ++routePointCount;
            }
        }
        elevationGraphAction->setEnabled(mission && routePointCount >= 2);
        undoAction->setEnabled(editable && plannerViewModel->CanUndo());
    });
    connect(plannerMap, &AbstractMapWidget::PlannerContextMenuRequested,
            plannerView,
            [plannerContextMenu](double latitude, double longitude,
                                 const QPoint &globalPosition,
                                 int waypointSequence) {
        plannerContextMenu->setProperty("plannerLatitude", latitude);
        plannerContextMenu->setProperty("plannerLongitude", longitude);
        plannerContextMenu->setProperty(
            "plannerWaypointSequence", waypointSequence);
        plannerContextMenu->popup(globalPosition);
    });
    auto *plannerUndoShortcut = new QShortcut(QKeySequence::Undo, plannerView);
    plannerUndoShortcut->setObjectName(QStringLiteral("PlannerUndoShortcut"));
    plannerUndoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(plannerUndoShortcut, &QShortcut::activated, plannerView,
            [this]() { plannerViewModel->Undo(); });

    const auto refreshPlannerMap = [this, plannerMap]() {
        if (!plannerViewModel || !plannerMap) return;
        FlightPlannerMissionModel *model = plannerViewModel->Waypoints();
        const auto store = model->missionStore();
        plannerMap->SetPlannerRows(model->rows(store), store);
    };
    const auto refreshPlannerHome = [this, plannerMap]() {
        if (!plannerViewModel || !plannerMap) return;
        if (plannerViewModel->HomeValid()) {
            plannerMap->SetPlannerHome(plannerViewModel->HomeLat(),
                                       plannerViewModel->HomeLng(),
                                       plannerViewModel->HomeAlt());
        } else {
            plannerMap->ClearPlannerHome();
        }
    };
    const auto refreshPlannerNavigation = [this, plannerMap]() {
        if (!plannerViewModel || !plannerMap) return;
        plannerMap->SetPlannerNavigationParameters(
            plannerViewModel->NavigationParameters());
    };
    const auto refreshPlannerAltitudePresentation = [this, plannerMap]() {
        if (!plannerViewModel || !plannerMap) return;
        plannerMap->SetPlannerAltitudePresentation(
            plannerViewModel->AltitudeMultiplier(),
            plannerViewModel->AltUnit());
    };
    const auto refreshPlannerPolygon = [this, plannerMap]() {
        if (!plannerViewModel || !plannerMap) return;
        QVector<MapCoordinate> points;
        const QVector<SurveyGridCoordinate> &polygon =
            plannerViewModel->DrawnPolygon()->DrawnPolygon();
        points.reserve(polygon.size());
        for (const SurveyGridCoordinate &point : polygon) {
            points.append({point.latitude, point.longitude, point.altitude});
        }
        plannerMap->SetPlannerDrawnPolygon(points);
    };
    connect(plannerViewModel->Waypoints(),
            &FlightPlannerMissionModel::rowsChanged, plannerView,
            [refreshPlannerMap](FlightPlannerMissionModel::MissionStore) {
                refreshPlannerMap();
            });
    connect(plannerViewModel,
            &FlightPlannerViewModel::missionTypeChanged, plannerView,
            [refreshPlannerMap](const QString &) { refreshPlannerMap(); });
    connect(plannerViewModel, &FlightPlannerViewModel::homeLatChanged,
            plannerView, [refreshPlannerHome](double) { refreshPlannerHome(); });
    connect(plannerViewModel, &FlightPlannerViewModel::homeLngChanged,
            plannerView, [refreshPlannerHome](double) { refreshPlannerHome(); });
    connect(plannerViewModel, &FlightPlannerViewModel::homeAltChanged,
            plannerView, [refreshPlannerHome](double) { refreshPlannerHome(); });
    connect(plannerViewModel, &FlightPlannerViewModel::homeValidChanged,
            plannerView, [refreshPlannerHome](bool) { refreshPlannerHome(); });
    connect(plannerViewModel,
            &FlightPlannerViewModel::plannerNavigationChanged,
            plannerView, refreshPlannerNavigation);
    connect(plannerViewModel,
            &FlightPlannerViewModel::altitudePresentationChanged,
            plannerView, refreshPlannerAltitudePresentation);
    connect(plannerViewModel->DrawnPolygon(),
            &FlightPlannerPolygonModel::DrawnPolygonChanged,
            plannerView, refreshPlannerPolygon);
    connect(plannerMap, &AbstractMapWidget::PlannerCoordinateRequested,
            plannerViewModel,
            [this](double latitude, double longitude) {
                if (!plannerViewModel) return;
                if (plannerViewModel->PolygonDrawMode()) {
                    plannerViewModel->AddPolygonPoint(latitude, longitude);
                } else {
                    plannerViewModel->AddWaypointAt(latitude, longitude);
                }
            });
    connect(plannerMap, &AbstractMapWidget::PlannerWaypointMoved,
            plannerViewModel,
            [this](int sequence, double latitude, double longitude) {
                if (!plannerViewModel) return;
                plannerViewModel->MoveWaypoint(
                    sequence, latitude, longitude);
            }, Qt::QueuedConnection);
    connect(plannerWaypointPanel,
            &FlightPlannerWaypointPanel::selectedWaypointChanged,
            plannerMap, &AbstractMapWidget::SetPlannerSelection);
    refreshPlannerAltitudePresentation();
    refreshPlannerHome();
    refreshPlannerNavigation();
    refreshPlannerPolygon();
    refreshPlannerMap();

    QAction *missionElevationAction =
        ui.menuTools->addAction(tr("Mission Elevation"));
    missionElevationAction->setObjectName(
        QStringLiteral("missionElevationToolStripMenuItem"));
    connect(missionElevationAction, &QAction::triggered,
            this, &MainWindow::showMissionElevation);

    createDockWidget(simView,new QGCWaypointListMulti(this),tr("Mission Plan"),"WAYPOINT_LIST_DOCKWIDGET",VIEW_SIMULATION,Qt::BottomDockWidgetArea);
    createDockWidget(simView,new ParameterInterface(this),tr("Parameters"),"PARAMETER_INTERFACE_DOCKWIDGET",VIEW_SIMULATION,Qt::RightDockWidgetArea);


    /*{ //Status details disabled until such a point that we can ensure it's completly operational
        QAction* tempAction = ui.menuTools->addAction(tr("Status Details"));
        menuToDockNameMap[tempAction] = "UAS_STATUS_DETAILS_DOCKWIDGET";
        tempAction->setCheckable(true);
        connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
    }*/
    //Horizontal situation disabled until such a point that we can ensure it's completly operational
    //createDockWidget(simView,new HSIDisplay(this),tr("Horizontal Situation"),"HORIZONTAL_SITUATION_INDICATOR_DOCKWIDGET",VIEW_SIMULATION,Qt::BottomDockWidgetArea);

    {
        QAction* tempAction = ui.menuTools->addAction(tr("Flight Display"));
        tempAction->setCheckable(true);
        connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
        menuToDockNameMap[tempAction] = "HEAD_DOWN_DISPLAY_1_DOCKWIDGET";
    }

    /*{ //Actuator status disabled until such a point that we can ensure it's completly operational
        QAction* tempAction = ui.menuTools->addAction(tr("Actuator Status"));
        tempAction->setCheckable(true);
        connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
        menuToDockNameMap[tempAction] = "HEAD_DOWN_DISPLAY_2_DOCKWIDGET";
    }*/

    /*{ //Radio Control disabled until such a point that we can ensure it's completly operational
	QAction* tempAction = ui.menuTools->addAction(tr("Radio Control"));
        tempAction->setCheckable(true);
	connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
    }*/

    //HUD disabled until such a point that we can ensure it's completly operational
    //createDockWidget(engineeringView,new HUD(320,240,this),tr("Video Downlink"),"HEAD_UP_DISPLAY_DOCKWIDGET",VIEW_ENGINEER,Qt::RightDockWidgetArea,this->width()/1.5);

#ifndef PFD_QML
    createDockWidget(simView,new PrimaryFlightDisplay(320,240,this),tr("Primary Flight Display"),
                     "PRIMARY_FLIGHT_DISPLAY_DOCKWIDGET",VIEW_SIMULATION,Qt::RightDockWidgetArea);

    { //This is required since we don't show the new PFD in full yet
        QAction* tempAction = ui.menuTools->addAction(tr("Primary Flight Display (2)"));
        tempAction->setCheckable(true);
        connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
        menuToDockNameMap[tempAction] = "PRIMARY_FLIGHT_DISPLAY_QML_DOCKWIDGET";
    }
#else
    createDockWidget(simView,new PrimaryFlightDisplayQML(this),tr("Primary Flight Display"),
                     "PRIMARY_FLIGHT_DISPLAY_QML_DOCKWIDGET",VIEW_SIMULATION,Qt::RightDockWidgetArea);

    { //This is required since we don't show the old PFD in any view
        QAction* tempAction = ui.menuTools->addAction(tr("Primary Flight Display (old)"));
        tempAction->setCheckable(true);
        connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
        menuToDockNameMap[tempAction] = "PRIMARY_FLIGHT_DISPLAY_DOCKWIDGET";
    }
#endif

    // Mission Planner 10 names and layout: HudHost/FdTabs form the 2* left
    // column and FdMap forms the 3* right column. Adding them in this order
    // keeps that layout identical in the embedded Qt dock host.
    auto *pilotHudHost = new QWidget(this);
    auto *pilotHudLayout = new QVBoxLayout(pilotHudHost);
    pilotHudLayout->setObjectName(QStringLiteral("HudHostLayout"));
    pilotHudLayout->setContentsMargins(0, 0, 0, 0);
    pilotHudLayout->setSpacing(0);
    auto *pilotHud = new HudControl(pilotHudHost);
    pilotHud->setObjectName(QStringLiteral("Hud"));
    pilotHudLayout->addWidget(pilotHud);
    auto *flightDataViewModel = new FlightDataViewModel(pilotHud);
    flightDataViewModel->setObjectName(QStringLiteral("FlightDataViewModel"));
    flightDataViewModel->attachHud(pilotHud);
    auto *speechAnnouncer = new SpeechAnnouncer(
        flightDataViewModel, flightDataViewModel);
    speechAnnouncer->setObjectName(QStringLiteral("SpeechAnnouncer"));
    if (pilotView->setHudWidget(pilotHudHost)) {
        registerDockablePanel(pilotView, VIEW_FLIGHT,
                              FlightDataView::hudPanelId(),
                              tr("HUD"), pilotHudHost);
    }
    auto *pilotMap = new QGCMapTool(this);
    pilotMap->setFlightDataViewModel(flightDataViewModel);
    if (pilotView->setMapWidget(pilotMap)) {
        registerDockablePanel(pilotView, VIEW_FLIGHT,
                              FlightDataView::mapPanelId(),
                              tr("Map"), pilotMap);
    }

    { // Adds the Vibration Monitor Tool
        QAction* tempAction = ui.menuTools->addAction(tr("Vibration Monitor"));
        tempAction->setCheckable(true);
        connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
        menuToDockNameMap[tempAction] = "VIBRATION_MONITOR_DOCKWIDGET";
    }

    { // Adds the EKF Monitor Tool
        QAction* tempAction = ui.menuTools->addAction(tr("EKF Monitor"));
        tempAction->setCheckable(true);
        connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
        menuToDockNameMap[tempAction] = "EKF_MONITOR_DOCKWIDGET";
    }

    QGCTabbedInfoView *infoview = new QGCTabbedInfoView(this);
    connect(infoview, &QGCTabbedInfoView::clearTrackRequested,
            pilotMap, [pilotMap]() {
        if (AbstractMapWidget *const map = pilotMap->mapWidget()) {
            map->DeleteTrails();
        }
    });
    connect(infoview, &QGCTabbedInfoView::joystickSetupRequested,
            this, &MainWindow::configure);
    infoview->setFlightDataViewModel(flightDataViewModel);
    infoview->addSource(mavlinkDecoder);
    if (pilotView->setInfoView(infoview)) {
        registerDockablePanel(pilotView, VIEW_FLIGHT,
                              FlightDataView::infoPanelId(),
                              tr("Info View"), infoview);
    }

    //connect(ui.actionLoad_tlog,SIGNAL(triggered()),this,SLOT(loadTlogMenuClicked()));

    // Custom widgets, added last to all menus and layouts
    buildCustomWidget();



#ifdef QGC_OSG_ENABLE
    if (q3DWidget)
    {
        q3DWidget = Q3DWidgetFactory::get("PIXHAWK", this);
        q3DWidget->setObjectName("VIEW_3DWIDGET");

        addToCentralStackedWidget(q3DWidget, VIEW_3DWIDGET, tr("Local 3D"));
    }
#endif

#if defined(GOOGLE_EARTH_VIEW)/*(defined _MSC_VER) | (defined Q_OS_MAC)*/
    if (!earthWidget)
    {
        earthWidget = new QGCGoogleEarthView(this);
        addToCentralStackedWidget(earthWidget,VIEW_GOOGLEEARTH, tr("Google Earth"));
    }
#endif
}

void MainWindow::addTool(SubMainWindow *parent,VIEW_SECTIONS view,QDockWidget* widget, const QString& title, Qt::DockWidgetArea area)
{
    QList<QAction*> actionlist = ui.menuTools->actions();
    bool found = false;
    QAction *targetAction;
    for (int i=0;i<actionlist.size();i++)
    {
        if (actionlist[i]->text() == title)
        {
            found = true;
            targetAction = actionlist[i];
        }
    }
    if (!found)
    {
        // Legacy docks retain their internal toggle action for layout/state
        // synchronization, but MP10's top-level TOOLS menu contains only
        // application tools and must not grow when a link/UAS appears.
        QAction* tempAction = new QAction(title, this);
        tempAction->setCheckable(true);
        menuToDockNameMap[tempAction] = widget->objectName();
        if (!centralWidgetToDockWidgetsMap.contains(view))
        {
            centralWidgetToDockWidgetsMap[view] = QMap<QString,QWidget*>();
        }
        centralWidgetToDockWidgetsMap[view][widget->objectName()]= widget;
        connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
        connect(widget, SIGNAL(visibilityChanged(bool)), tempAction, SLOT(setChecked(bool)));
        tempAction->setChecked(widget->isVisible());
        menuToDockNameMapByView[view][tempAction] = widget->objectName();
    }
    else
    {
        if (!menuToDockNameMap.contains(targetAction))
        {
            menuToDockNameMap[targetAction] = widget->objectName();
            //menuToDockNameMap[targetAction] = title;
        }
        if (!centralWidgetToDockWidgetsMap.contains(view))
        {
            centralWidgetToDockWidgetsMap[view] = QMap<QString,QWidget*>();
        }
        centralWidgetToDockWidgetsMap[view][widget->objectName()]= widget;
        connect(widget, SIGNAL(visibilityChanged(bool)), targetAction, SLOT(setChecked(bool)));
        menuToDockNameMapByView[view][targetAction] = widget->objectName();
    }
    parent->addDockWidget(area,widget);
}

void MainWindow::registerDockablePanel(DockableView *parent,
                                       VIEW_SECTIONS view,
                                       const QString &panelId,
                                       const QString &title,
                                       QWidget *content)
{
    if (!parent || panelId.isEmpty() || !content) {
        return;
    }

    QAction *menuAction = nullptr;
    const QList<QAction *> actions = ui.menuTools->actions();
    for (QAction *action : actions) {
        if (action->text() == title) {
            menuAction = action;
            break;
        }
    }
    if (!menuAction) {
        // DATA/PLAN panels are controlled by DockableView, not by MP10 TOOLS.
        menuAction = new QAction(title, this);
        menuAction->setCheckable(true);
        connect(menuAction, SIGNAL(triggered(bool)), this, SLOT(showTool(bool)));
    }

    menuToDockNameMap[menuAction] = panelId;
    menuToDockNameMapByView[view][menuAction] = panelId;
    centralWidgetToDockWidgetsMap[view][panelId] = content;

    if (QAction *toggleAction = parent->panelToggleAction(panelId)) {
        menuAction->setChecked(parent->isPanelOpen(panelId));
        connect(toggleAction, &QAction::toggled,
                menuAction, &QAction::setChecked, Qt::UniqueConnection);
    }
}

QDockWidget* MainWindow::createDockWidget(QWidget *parent,QWidget *child,QString title,QString objectname,VIEW_SECTIONS view,Qt::DockWidgetArea area,int minwidth,int minheight)
{
    //if (child->objectName() == "")
    //{
    child->setObjectName(objectname);
    //}

    if (auto *dockableView = qobject_cast<DockableView *>(parent)) {
        DockableView::PanelLocation location = DockableView::PanelLocation::Left;
        switch (area) {
        case Qt::RightDockWidgetArea:
            location = DockableView::PanelLocation::Right;
            break;
        case Qt::TopDockWidgetArea:
            location = DockableView::PanelLocation::Top;
            break;
        case Qt::BottomDockWidgetArea:
            location = DockableView::PanelLocation::Bottom;
            break;
        case Qt::LeftDockWidgetArea:
        default:
            location = DockableView::PanelLocation::Left;
            break;
        }
        const QSize preferredSize = (minwidth > 0 || minheight > 0)
            ? QSize(minwidth, minheight) : QSize();
        if (!dockableView->addPanel(objectname, title, child, location,
                                    QString(), preferredSize)) {
            QLOG_WARN() << "Could not add panel" << objectname
                        << "to" << dockableView->viewId();
            child->deleteLater();
            return nullptr;
        }
        registerDockablePanel(dockableView, view, objectname, title, child);
        return nullptr;
    }

    QDockWidget *widget = new QDockWidget(title,this);
    if (!isAdvancedMode)
    {
        if (dockWidgetTitleBarEnabled)
        {
            dockToTitleBarMap[widget] = widget->titleBarWidget();
            QLabel *label = new QLabel(this);
            label->setText(title);
            widget->setTitleBarWidget(label);
            label->installEventFilter(new DockWidgetTitleBarEventFilter());
        }
        else
        {
            dockToTitleBarMap[widget] = widget->titleBarWidget();
            widget->setTitleBarWidget(new QWidget(this));
        }
    }
    else
    {
        QLabel *label = new QLabel(this);
        label->setText(title);
        dockToTitleBarMap[widget] = label;
        label->installEventFilter(new DockWidgetTitleBarEventFilter());
        label->hide();
    }
    widget->setObjectName(child->objectName());
    widget->setWidget(child);
    if (minheight != 0 || minwidth != 0)
    {
        widget->setMinimumHeight(minheight);
        widget->setMinimumWidth(minwidth);
    }
    addTool(qobject_cast<SubMainWindow*>(parent),view,widget,title,area);

    return widget;
}
void MainWindow::loadDockWidget(QString name)
{
    if (centralWidgetToDockWidgetsMap[currentView].contains(name))
    {
        return;
    }
    if (name.startsWith("HIL_CONFIG"))
    {
        //It's a HIL widget.
        showHILConfigurationWidget(UASManager::instance()->getActiveUAS());
    }
    else if (name == "UNMANNED_SYSTEM_CONTROL_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new UASControlWidget(this),tr("Control"),"UNMANNED_SYSTEM_CONTROL_DOCKWIDGET",currentView,Qt::LeftDockWidgetArea);
    }
    else if (name == "UNMANNED_SYSTEM_LIST_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new UASListWidget(this),tr("Unmanned Systems"),"UNMANNED_SYSTEM_LIST_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "WAYPOINT_LIST_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new QGCWaypointListMulti(this),tr("Mission Plan"),"WAYPOINT_LIST_DOCKWIDGET",currentView,Qt::BottomDockWidgetArea);
    }
    else if (name == "VIBRATION_MONITOR_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new VibrationMonitor(this),tr("Vibration Monitor"),"VIBRATION_MONITOR_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "EKF_MONITOR_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new EKFMonitor(this),tr("EKF Monitor"),"EKF_MONITOR_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "PARAMETER_INTERFACE_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new ParameterInterface(this),tr("Parameters"),"PARAMETER_INTERFACE_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "UAS_STATUS_DETAILS_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new UASInfoWidget(this),tr("Status Details"),"UAS_STATUS_DETAILS_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "HORIZONTAL_SITUATION_INDICATOR_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new HSIDisplay(this),tr("Horizontal Situation"),"HORIZONTAL_SITUATION_INDICATOR_DOCKWIDGET",currentView,Qt::BottomDockWidgetArea);
    }
    else if (name == "HEAD_DOWN_DISPLAY_1_DOCKWIDGET")
    {
        //FIXME: memory of acceptList will never be freed again
        QStringList* acceptList = new QStringList();
        acceptList->append("-3.3,ATTITUDE.roll,rad,+3.3,s");
        acceptList->append("-3.3,ATTITUDE.pitch,deg,+3.3,s");
        acceptList->append("-3.3,ATTITUDE.yaw,deg,+3.3,s");
        HDDisplay *hddisplay = new HDDisplay(acceptList,"Flight Display",this);
        hddisplay->addSource(mavlinkDecoder);
        createDockWidget(centerStack->currentWidget(),hddisplay,tr("Flight Display"),"HEAD_DOWN_DISPLAY_1_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "HEAD_DOWN_DISPLAY_2_DOCKWIDGET")
    {
        //FIXME: memory of acceptList2 will never be freed again
        QStringList* acceptList2 = new QStringList();
        acceptList2->append("0,RAW_PRESSURE.pres_abs,hPa,65500");
        HDDisplay *hddisplay = new HDDisplay(acceptList2,"Actuator Status",this);
        hddisplay->addSource(mavlinkDecoder);
        createDockWidget(centerStack->currentWidget(),hddisplay,tr("Actuator Status"),"HEAD_DOWN_DISPLAY_2_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "Radio Control")
    {
        QLOG_DEBUG() << "Error loading window:" << name << "Unknown window type";
        //createDockWidget(centerStack->currentWidget(),hddisplay,tr("Actuator Status"),"HEADS_DOWN_DISPLAY_2_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "PRIMARY_FLIGHT_DISPLAY_DOCKWIDGET")
    {
        // createDockWidget(centerStack->currentWidget(),new HUD(320,240,this),tr("Head Up Display"),"PRIMARY_FLIGHT_DISPLAY_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
        createDockWidget(centerStack->currentWidget(),new PrimaryFlightDisplay(320,240,this),tr("Primary Flight Display"),"HEAD_UP_DISPLAY_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "PRIMARY_FLIGHT_DISPLAY_QML_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new PrimaryFlightDisplayQML(this),tr("Primary Flight Display QML"),"HEAD_UP_DISPLAY_DOCKWIDGET",currentView,Qt::RightDockWidgetArea);
    }
    else if (name == "UAS_INFO_QUICKVIEW_DOCKWIDGET")
    {
        createDockWidget(centerStack->currentWidget(),new UASQuickView(this),tr("Quick View"),"UAS_INFO_QUICKVIEW_DOCKWIDGET",currentView,Qt::LeftDockWidgetArea);
    }
    else
    {
        if (customWidgetNameToFilenameMap.contains(name))
        {
            loadCustomWidget(customWidgetNameToFilenameMap[name],currentView);
            //customWidgetNameToFilenameMap.remove(name);
        }
        else
        {
            QLOG_DEBUG() << "Error loading window:" << name;
        }
    }
}

void MainWindow::showTool(bool show)
{
    //Called when a menu item is clicked on, regardless of view.

    QAction* act = qobject_cast<QAction *>(sender());
    QString name;
    if (menuToDockNameMapByView.value(currentView).contains(act)) {
        name = menuToDockNameMapByView.value(currentView).value(act);
    } else if (menuToDockNameMap.contains(act)) {
        name = menuToDockNameMap.value(act);
    }
    if (!name.isEmpty())
    {
        if (centralWidgetToDockWidgetsMap.contains(currentView))
        {
            if (centralWidgetToDockWidgetsMap[currentView].contains(name))
            {
                if (auto *dockableView = qobject_cast<DockableView *>(
                        centerStack->currentWidget())) {
                    if (!dockableView->setPanelVisible(name, show)) {
                        // Core DATA/PLAN surfaces must never become completely
                        // empty. Reflect a rejected attempt to hide the final
                        // panel back in the Tools menu action.
                        act->setChecked(!show);
                    }
                    return;
                }
                if (show)
                {
                    centralWidgetToDockWidgetsMap[currentView][name]->show();
                }
                else
                {
                    centralWidgetToDockWidgetsMap[currentView][name]->hide();
                }
            }
            else if (show)
            {
                loadDockWidget(name);
            }
        }
    }
    //QWidget* widget = qVariantValue<QWidget *>(act->data());
    //widget->setVisible(show);
}
/*void addToolByName(QString name,SubMainWindow parent,const QString& title, Qt::DockWidgetArea area)
{
    if (name == "Control")
    {
        QDockWidget *widget = new QDockWidget(tr("Control"),this);
        dockToTitleBarMap[widget] = widget->titleBarWidget();
        widget->setObjectName("UNMANNED_SYSTEM_CONTROL_DOCKWIDGET");
        widget->setWidget(new UASControlWidget(this));
        addTool(parent,VIEW_SIMULATION,widget,tr("Control"),area);
    }
}*/
void MainWindow::addToCentralStackedWidget(QWidget* widget, VIEW_SECTIONS viewSection, const QString& title)
{
    Q_UNUSED(title);
    Q_ASSERT(widget->objectName().length() != 0);

    // Check if this widget already has been added
    if (centerStack->indexOf(widget) == -1)
    {
        centerStack->addWidget(widget);
        centralWidgetToDockWidgetsMap[viewSection] = QMap<QString,QWidget*>();
    }
}


void MainWindow::showCentralWidget()
{
    QAction* act = qobject_cast<QAction *>(sender());
    QWidget* widget = act->data().value<QWidget*>();
    centerStack->setCurrentWidget(widget);
}

void MainWindow::showHILConfigurationWidget(UASInterface* uas)
{
    // Add simulation configuration widget
    UAS* mav = dynamic_cast<UAS*>(uas);

    if (mav && !hilDocks.contains(mav->getUASID()))
    {
        //QGCToolWidget* tool = new QGCToolWidget("Unnamed Tool " + QString::number(ui.menuTools->actions().size()));
        //createDockWidget(centerStack->currentWidget(),tool,"Unnamed Tool " + QString::number(ui.menuTools->actions().size()),"UNNAMED_TOOL_" + QString::number(ui.menuTools->actions().size())+"DOCK",currentView,Qt::BottomDockWidgetArea);

        QGCHilConfiguration* hconf = new QGCHilConfiguration(mav, this);
        QString hilDockName = tr("HIL Config %1").arg(uas->getUASName());
        QDockWidget* hilDock = createDockWidget(simView, hconf,hilDockName, hilDockName.toUpper().replace(" ", "_"),VIEW_SIMULATION,Qt::LeftDockWidgetArea);
        hilDocks.insert(mav->getUASID(), hilDock);

        //        if (currentView != VIEW_SIMULATION)
        //            hilDock->hide();
        //        else
        //            hilDock->show();
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (isVisible()) storeViewState();
    aboutToCloseFlag = true;
    closeMavlinkInspectorWindows();
    if (logPlayer) {
        logPlayer->shutdown();
    }
    // Top-level child dialogs are not closed by QWidget parent teardown until
    // the MainWindow destructor runs. Queue this modeless plot first so its
    // QCustomPlot-owned shared objects are released while the event loop and
    // terrain services are still alive.
    if (plannerElevationDialog) {
        plannerElevationDialog->close();
        plannerElevationDialog->deleteLater();
        plannerElevationDialog = nullptr;
    }
    storeSettings();
    //mavlink->storeSettings();
    UASManager::instance()->storeSettings();
    QMainWindow::closeEvent(event);
    if (event->isAccepted()) {
        // Closing the primary window is an explicit application-exit request.
        // Do not rely solely on lastWindowClosed(): modeless connection and
        // transport windows can outlive their native X11 window long enough to
        // leave the event loop running (and links receiving data) invisibly.
        QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
    }
}

/**
 * Connect the signals and slots of the common window widgets
 */
void MainWindow::connectCommonWidgets()
{
    /*if (infoDockWidget && infoDockWidget->widget())
    {
        connect(mavlink, SIGNAL(receiveLossChanged(int, float)),
                infoDockWidget->widget(), SLOT(updateSendLoss(int, float)));
    }*/


}

void MainWindow::createCustomWidget()
{
    //void MainWindow::createDockWidget(QWidget *parent,QWidget *child,QString title,QString objectname,VIEW_SECTIONS view,Qt::DockWidgetArea area,int minwidth,int minheight)
    //QDockWidget* dock = new QDockWidget("Unnamed Tool", this);

    if (QGCToolWidget::instances()->size() < 2)
    {
        // This is the first widget
        ui.menuTools->addSeparator();
    }
    QGCToolWidget* tool = new QGCToolWidget("Unnamed Tool " + QString::number(ui.menuTools->actions().size()));
    createDockWidget(centerStack->currentWidget(),tool,"Unnamed Tool " + QString::number(ui.menuTools->actions().size()),"UNNAMED_TOOL_" + QString::number(ui.menuTools->actions().size())+"DOCK",currentView,Qt::BottomDockWidgetArea);
    //tool->setObjectName("UNNAMED_TOOL_" + QString::number(ui.menuTools->actions().size()));
    QSettings settings;
    settings.beginGroup("QGC_MAINWINDOW");
    settings.setValue(QString("TOOL_PARENT_") + tool->objectName(),currentView);
    settings.endGroup();



    //connect(tool, SIGNAL(destroyed()), dock, SLOT(deleteLater()));
    //dock->setWidget(tool);

    //QAction* showAction = new QAction(tool->getTitle(), this);
    //showAction->setCheckable(true);
    //connect(dock, SIGNAL(visibilityChanged(bool)), showAction, SLOT(setChecked(bool)));
    //connect(showAction, SIGNAL(triggered(bool)), dock, SLOT(setVisible(bool)));
    //tool->setMainMenuAction(showAction);
    //ui.menuTools->addAction(showAction);
    //this->addDockWidget(Qt::BottomDockWidgetArea, dock);
    //dock->setVisible(true);
}

void MainWindow::loadCustomWidget()
{
    QString widgetFileExtension(".qgw");
    QString fileName = QFileDialog::getOpenFileName(this, tr("Specify Widget File Name"),
                                                    QGC::appDataDirectory(),
                                                    tr("QGroundControl Widget (*%1);;").arg(widgetFileExtension));
    if (fileName != "") loadCustomWidget(fileName);
}
void MainWindow::loadCustomWidget(const QString& fileName, int view)
{
    QGCToolWidget* tool = new QGCToolWidget("", this);
    if (tool->loadSettings(fileName, true))
    {
        QLOG_DEBUG() << "Loading custom tool:" << tool->getTitle() << tool->objectName();
        switch ((VIEW_SECTIONS)view)
        {
        case VIEW_ENGINEER:
            createDockWidget(engineeringView,tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
            break;
        case VIEW_FLIGHT:
            createDockWidget(pilotView,tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
            break;
        case VIEW_SIMULATION:
            createDockWidget(simView,tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
            break;
        case VIEW_MISSION:
            createDockWidget(plannerView,tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
            break;
        default:
        {
            //Delete tool, create menu item to tie it to.
            customWidgetNameToFilenameMap[tool->objectName()+"DOCK"] = fileName;
            QAction* tempAction = new QAction(tool->getTitle(), this);
            menuToDockNameMap[tempAction] = tool->objectName()+"DOCK";
            tempAction->setCheckable(true);
            connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
            tool->deleteLater();
            //createDockWidget(centerStack->currentWidget(),tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
        }
            break;
        }
    }
    else
    {
        return;
    }
}

void MainWindow::loadCustomWidget(const QString& fileName, bool singleinstance)
{
    QGCToolWidget* tool = new QGCToolWidget("", this);
    if (tool->loadSettings(fileName, true) || !singleinstance)
    {
        QLOG_DEBUG() << "Loading custom tool:" << tool->getTitle() << tool->objectName();
        QSettings settings;
        settings.beginGroup("QGC_MAINWINDOW");
        //settings.setValue(QString("TOOL_PARENT_") + "UNNAMED_TOOL_" + QString::number(ui.menuTools->actions().size()),currentView);

        int view = settings.value(QString("TOOL_PARENT_") + tool->objectName(),-1).toInt();
        switch (view)
        {
        case VIEW_ENGINEER:
            createDockWidget(engineeringView,tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
            break;
        case VIEW_FLIGHT:
            createDockWidget(pilotView,tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
            break;
        case VIEW_SIMULATION:
            createDockWidget(simView,tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
            break;
        case VIEW_MISSION:
            createDockWidget(plannerView,tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
            break;
        default:
        {
            //Delete tool, create menu item to tie it to.
            customWidgetNameToFilenameMap[tool->objectName()+"DOCK"] = fileName;
            QAction* tempAction = new QAction(tool->getTitle(), this);
            menuToDockNameMap[tempAction] = tool->objectName()+"DOCK";
            tempAction->setCheckable(true);
            connect(tempAction,SIGNAL(triggered(bool)),this, SLOT(showTool(bool)));
            tool->deleteLater();
            //createDockWidget(centerStack->currentWidget(),tool,tool->getTitle(),tool->objectName()+"DOCK",(VIEW_SECTIONS)view,Qt::LeftDockWidgetArea);
        }
            break;
        }


        settings.endGroup();
        // Add widget to UI
        /*QDockWidget* dock = new QDockWidget(tool->getTitle(), this);
        connect(tool, SIGNAL(destroyed()), dock, SLOT(deleteLater()));
        dock->setWidget(tool);
        tool->setParent(dock);

        QAction* showAction = new QAction(tool->getTitle(), this);
        showAction->setCheckable(true);
        connect(dock, SIGNAL(visibilityChanged(bool)), showAction, SLOT(setChecked(bool)));
        connect(showAction, SIGNAL(triggered(bool)), dock, SLOT(setVisible(bool)));
        tool->setMainMenuAction(showAction);
        ui.menuTools->addAction(showAction);
        this->addDockWidget(Qt::BottomDockWidgetArea, dock);
        dock->hide();*/
    }
    else
    {
        return;
    }
}

void MainWindow::loadCustomWidgetsFromDefaults(const QString& systemType, const QString& autopilotType)
{
    QString defaultsDir = QGC::shareDirectory() + "/files/" + autopilotType.toLower() + "/widgets/";
    QString platformDir = QGC::shareDirectory() + "/files/" + autopilotType.toLower() + "/" + systemType.toLower() + "/widgets/";

    QDir widgets(defaultsDir);
    QStringList files = widgets.entryList();
    QDir platformWidgets(platformDir);
    files.append(platformWidgets.entryList());

    if (files.count() == 0)
    {
        QLOG_DEBUG() << "No default custom widgets for system " << systemType << "autopilot" << autopilotType << " found";
        QLOG_DEBUG() << "Tried with path: " << defaultsDir;
        showStatusMessage(tr("Did not find any custom widgets in %1").arg(defaultsDir));
    }

    // Load all custom widgets found in the AP folder
    for(int i = 0; i < files.count(); ++i)
    {
        QString file = files[i];
        if (file.endsWith(".qgw"))
        {
            // Will only be loaded if not already a custom widget with
            // the same name is present
            loadCustomWidget(defaultsDir+"/"+file, true);
            showStatusMessage(tr("Loaded custom widget %1").arg(defaultsDir+"/"+file));
        }
    }
}

void MainWindow::loadSettings()
{
    QSettings settings;
    const bool heartbeat = settings.value(
        QStringLiteral("CHK_GCSheartbeat"), true).toBool();
    settings.beginGroup("QGC_MAINWINDOW");
    autoReconnect = settings.value("AUTO_RECONNECT",false).toBool();
    currentStyle = (QGC_MAINWINDOW_STYLE)settings.value("CURRENT_STYLE", QGC_MAINWINDOW_STYLE_OUTDOOR).toInt();
    currentView= static_cast<VIEW_SECTIONS>(settings.value("CURRENT_VIEW", VIEW_FLIGHT).toInt());
    lowPowerMode = settings.value("LOW_POWER_MODE", false).toBool();
    autoProxyMode = settings.value("AUTO_PROXY_MODE", false).toBool();
    dockWidgetTitleBarEnabled = settings.value("DOCK_WIDGET_TITLEBARS", true).toBool();
    isAdvancedMode = settings.value("ADVANCED_MODE", false).toBool();
    enableHeartbeat(heartbeat);
    settings.endGroup();
}

void MainWindow::storeSettings()
{
    QSettings settings;
    settings.beginGroup("QGC_MAINWINDOW");
    settings.setValue("AUTO_RECONNECT", autoReconnect);
    settings.setValue("CURRENT_STYLE", currentStyle);
    settings.setValue("LOW_POWER_MODE", lowPowerMode);
    settings.setValue("AUTO_PROXY_MODE", autoProxyMode);
    settings.setValue("ADVANCED_MODE", isAdvancedMode);
    settings.endGroup();

    if (!aboutToCloseFlag && isVisible())
    {
        settings.setValue(getWindowGeometryKey(), saveGeometry());
        // Save the last current view in any case
        settings.setValue("CURRENT_VIEW", currentView);
        // Save the current window state, but only if a system is connected (else no real number of widgets would be present))
        if (UASManager::instance()->getUASList().length() > 0) settings.setValue(getWindowStateKey(), saveState(QGC::applicationVersion()));
        // Save the current view only if a UAS is connected
        if (UASManager::instance()->getUASList().length() > 0) settings.setValue("CURRENT_VIEW_WITH_UAS_CONNECTED", currentView);
        // Save the current power mode
    }
    settings.sync();
}

void MainWindow::configureWindowName()
{
    QList<QHostAddress> hostAddresses = QNetworkInterface::allAddresses();
    QString windowname = qApp->applicationDisplayName() + " " + qApp->applicationVersion();
    bool prevAddr = false;

    windowname.append(" (" + QHostInfo::localHostName() + ": ");

    for (int i = 0; i < hostAddresses.size(); i++)
    {
        // Exclude loopback IPv4 and all IPv6 addresses
        if (hostAddresses.at(i) != QHostAddress("127.0.0.1") && !hostAddresses.at(i).toString().contains(":"))
        {
            if(prevAddr) windowname.append("/");
            windowname.append(hostAddresses.at(i).toString());
            prevAddr = true;
        }
    }

    windowname.append(")");

    setWindowTitle(windowname);

#ifndef Q_WS_MAC
    //qApp->setWindowIcon(QIcon(":/core/images/qtcreator_logo_128.png"));
#endif
}

void MainWindow::startVideoCapture()
{
    QString format = "bmp";
    QString initialPath = QGC::appDataDirectory();

    QString screenFileName = QFileDialog::getSaveFileName(this, tr("Save As"),
                                                          initialPath,
                                                          tr("%1 Files (*.%2);;All Files (*)")
                                                          .arg(format.toUpper())
                                                          .arg(format));
    delete videoTimer;
    videoTimer = new QTimer(this);
    //videoTimer->setInterval(40);
    //connect(videoTimer, SIGNAL(timeout()), this, SLOT(saveScreen()));
    //videoTimer->stop();
}

void MainWindow::stopVideoCapture()
{
    videoTimer->stop();

    // TODO Convert raw images to PNG
}

void MainWindow::saveScreen()
{
    QList<QScreen *> screens = QGuiApplication::screens();
    if(!screens.empty())
    {
        QPixmap window = screens[0]->grabWindow(this->winId());
        QString format = "bmp";

        if (!screenFileName.isEmpty())
        {
            window.save(screenFileName, format.toLatin1());
        }
    }
}

void MainWindow::enableDockWidgetTitleBars(bool enabled)
{
    dockWidgetTitleBarEnabled = enabled;
    QSettings settings;
    settings.beginGroup("QGC_MAINWINDOW");
    settings.setValue("DOCK_WIDGET_TITLEBARS",dockWidgetTitleBarEnabled);
    settings.endGroup();
    settings.sync();
    if (!isAdvancedMode)
    {
        if (enabled)
        {
            for (QMap<QDockWidget*,QWidget*>::const_iterator i=dockToTitleBarMap.constBegin();i!=dockToTitleBarMap.constEnd();i++)
            {
                QLabel *label = new QLabel(this);
                label->setText(i.key()->windowTitle());
                i.key()->setTitleBarWidget(label);
                //label->setEnabled(false);
                label->installEventFilter(new DockWidgetTitleBarEventFilter());
            }
        }
        else
        {
            for (QMap<QDockWidget*,QWidget*>::const_iterator i=dockToTitleBarMap.constBegin();i!=dockToTitleBarMap.constEnd();i++)
            {
                i.key()->setTitleBarWidget(new QWidget(this));
            }
        }
    }
}

void MainWindow::enableAutoReconnect(bool enabled)
{
    autoReconnect = enabled;
}

void MainWindow::enableAutoProxyMode(bool enabled)
{
    if (enabled)
    {
        QLOG_INFO() << "NETWORK_PROXY:" << "Attempting to enable System Network Proxies";
        QNetworkProxyFactory::setUseSystemConfiguration(true);

        // Check for proxy used for well known external URL
        QNetworkProxyQuery npq(QUrl("http://www.google.com"));
        QList<QNetworkProxy> listOfProxies = QNetworkProxyFactory::systemProxyForQuery(npq);

        if (listOfProxies.size() &&
                QNetworkProxy::NoProxy != listOfProxies[0].type())
        {
            QLOG_INFO() << "NETWORK_PROXY:" << "System Proxies in use for external urls";
            autoProxyMode = enabled;
        }
        else
        {
            QLOG_ERROR() << "NETWORK_PROXY:" << "No System Proxies found in environment";
            QNetworkProxyFactory::setUseSystemConfiguration(false);
        }
    }
    else
    {
        QLOG_INFO() << "NETWORK_PROXY:" << "Disabling System Network Proxies";
        QNetworkProxyFactory::setUseSystemConfiguration(false);

        autoProxyMode = enabled;
    }

    // Ensure the checkbox is in-sync with the current value
    emit autoProxyChanged(autoProxyMode);
}

void MainWindow::loadNativeStyle()
{
    loadStyle(QGC_MAINWINDOW_STYLE_NATIVE);
}

void MainWindow::loadIndoorStyle()
{
    loadStyle(QGC_MAINWINDOW_STYLE_INDOOR);
}

void MainWindow::loadOutdoorStyle()
{
    loadStyle(QGC_MAINWINDOW_STYLE_OUTDOOR);
}

void MainWindow::loadStyle(QGC_MAINWINDOW_STYLE style)
{
    switch (style) {
    case QGC_MAINWINDOW_STYLE_NATIVE: {
        // Native mode means setting no style
        // so if we were already in native mode
        // take no action
        // Only if a style was set, remove it.
        if (style != currentStyle) {
            qApp->setStyleSheet("QMainWindow::separator { background: rgb(0, 0, 0); width: 5px; height: 5px;}");
            //qApp->setStyleSheet("");
            showInfoMessage(tr("Please restart APM Planner"), tr("Please restart APM Planner to switch to the fully native look and feel. The cross-platform Fusion base style remains active until restart."));
        }
    }
        break;
    case QGC_MAINWINDOW_STYLE_INDOOR:
        qApp->setStyle(QStringLiteral("Fusion"));
        styleFileName = ":files/styles/style-indoor.css";
        reloadStylesheet();
        break;
    case QGC_MAINWINDOW_STYLE_OUTDOOR:
        // Fusion is available on every supported Qt desktop platform. The
        // Emerald stylesheet can therefore produce the same metrics and
        // palette on Linux, Windows and macOS instead of inheriting native
        // style differences.
        qApp->setStyle(QStringLiteral("Fusion"));
        styleFileName = ":files/styles/style-outdoor.css";
        reloadStylesheet();
        break;
    }
    currentStyle = style;
}

void MainWindow::selectStylesheet()
{
    // Let user select style sheet
    QFileDialog *dialog = new QFileDialog(this,tr("Specify stylesheet"), styleFileName, tr("CSS Stylesheet (*.css);;"));
    dialog->setFileMode(QFileDialog::ExistingFile);
    connect(dialog,SIGNAL(accepted()),this,SLOT(selectStylesheetDialogAccepted()));
    dialog->show();
}
void MainWindow::selectStylesheetDialogAccepted()
{
    QFileDialog *dialog = qobject_cast<QFileDialog*>(sender());
    if (!dialog)
    {
        return;
    }
    if (dialog->selectedFiles().size() == 0)
    {
        //No file selected/cancel clicked
        return;
    }
    QString tmpfilename = dialog->selectedFiles().at(0);

    if (!tmpfilename.endsWith(".css"))
    {
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Information);
        msgBox.setText(tr("QGroundControl did lot load a new style"));
        msgBox.setInformativeText(tr("No suitable .css file selected. Please select a valid .css file."));
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setDefaultButton(QMessageBox::Ok);
        msgBox.exec();
        return;
    }
    styleFileName = tmpfilename;

    // Load style sheet
    reloadStylesheet();
}

void MainWindow::reloadStylesheet()
{
    // Load style sheet
    QScopedPointer<QFile> styleSheet(new QFile(styleFileName));
    if (!styleSheet->exists())
    {
        styleSheet.reset(new QFile(":files/styles/style-outdoor.css"));
    }
    if (styleSheet->open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QString style = QString(styleSheet->readAll());
        style.replace("ICONDIR", QGC::shareDirectory() + "/files/styles/");
        qApp->setStyleSheet(style);
    }
    else
    {
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Information);
        msgBox.setText(tr("QGroundControl did lot load a new style"));
        msgBox.setInformativeText(tr("Stylesheet file %1 was not readable").arg(styleFileName));
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setDefaultButton(QMessageBox::Ok);
        msgBox.exec();
    }
}

/**
 * The status message will be overwritten if a new message is posted to this function
 *
 * @param status message text
 * @param timeout how long the status should be displayed
 */
void MainWindow::showStatusMessage(const QString& status, int timeout)
{
    statusBar()->showMessage(status, timeout);
}

/**
 * The status message will be overwritten if a new message is posted to this function.
 * it will be automatically hidden after 5 seconds.
 *
 * @param status message text
 */
void MainWindow::showStatusMessage(const QString& status)
{
    statusBar()->showMessage(status, 20000);
}

void MainWindow::showCriticalMessage(const QString& title, const QString& message)
{
//    QMessageBox msgBox(this);
    //QMessageBox::information(this,title,message);
    qDebug() << "Critical message:" << title << message;
//    msgBox.setIcon(QMessageBox::Critical);
//    msgBox.setText(title);
//    msgBox.setInformativeText(message);
//    msgBox.setStandardButtons(QMessageBox::Ok);
//    msgBox.setDefaultButton(QMessageBox::Ok);
//    msgBox.show();
}

void MainWindow::showInfoMessage(const QString& title, const QString& message)
{
    QMessageBox msgBox(this);
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setText(title);
    msgBox.setInformativeText(message);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.setDefaultButton(QMessageBox::Ok);
    msgBox.exec();
}

/**
* @brief Create all actions associated to the main window
*
**/
void MainWindow::connectCommonActions()
{
    // Bind together the perspective actions
    QActionGroup* perspectives = new QActionGroup(ui.menuPerspectives);
    perspectives->addAction(ui.actionEngineersView);
    perspectives->addAction(ui.actionMavlinkView);
    perspectives->addAction(ui.actionFlightView);
    perspectives->addAction(ui.actionSimulation_View);
    perspectives->addAction(ui.actionMissionView);
    //perspectives->addAction(ui.actionConfiguration_2);
    perspectives->addAction(ui.actionHardwareConfig);
    perspectives->addAction(ui.actionSoftwareConfig);
    perspectives->addAction(helpViewAction);
    //perspectives->addAction(ui.actionFirmwareUpdateView);
    perspectives->addAction(ui.actionTerminalView);
    //perspectives->addAction(ui.actionUnconnectedView);
    perspectives->setExclusive(true);

    // Mark the right one as selected
    if (currentView == VIEW_ENGINEER)
    {
        ui.actionEngineersView->setChecked(true);
        ui.actionEngineersView->activate(QAction::Trigger);
    }
    if (currentView == VIEW_MAVLINK)
    {
        ui.actionMavlinkView->setChecked(true);
        ui.actionMavlinkView->activate(QAction::Trigger);
    }
    if (currentView == VIEW_FLIGHT)
    {
        ui.actionFlightView->setChecked(true);
        ui.actionFlightView->activate(QAction::Trigger);
    }
    if (currentView == VIEW_SIMULATION)
    {
        ui.actionSimulation_View->setChecked(true);
        ui.actionSimulation_View->activate(QAction::Trigger);
    }
    if (currentView == VIEW_MISSION)
    {
        ui.actionMissionView->setChecked(true);
        ui.actionMissionView->activate(QAction::Trigger);
    }
    if (currentView == VIEW_HARDWARE_CONFIG)
    {
        ui.actionHardwareConfig->setChecked(true);
        ui.actionHardwareConfig->activate(QAction::Trigger);
    }
    if (currentView == VIEW_SOFTWARE_CONFIG)
    {
        ui.actionSoftwareConfig->setChecked(true);
        ui.actionSoftwareConfig->activate(QAction::Trigger);
    }
    if (currentView == VIEW_HELP)
    {
        helpViewAction->setChecked(true);
        helpViewAction->activate(QAction::Trigger);
    }
    if (currentView == VIEW_FIRMWAREUPDATE)
    {
        ui.actionFirmwareUpdateView->setChecked(true);
        ui.actionFirmwareUpdateView->activate(QAction::Trigger);
    }
    if (currentView == VIEW_TERMINAL)
    {
        ui.actionTerminalView->setChecked(true);
        ui.actionTerminalView->activate(QAction::Trigger);
    }
    if (currentView == VIEW_UNCONNECTED)
    {
        ui.actionUnconnectedView->setChecked(true);
        ui.actionUnconnectedView->activate(QAction::Trigger);
    }

    // The UAS actions are not enabled without connection to system
    ui.actionLiftoff->setEnabled(false);
    ui.actionLand->setEnabled(false);
    ui.actionEmergency_Kill->setEnabled(false);
    ui.actionEmergency_Land->setEnabled(false);
    ui.actionShutdownMAV->setEnabled(false);

    // About
    connect(ui.actionAbout_APM_Planner_2_0, SIGNAL(triggered()), this, SLOT(showAbout()));

    // Check for updates
    connect(ui.actionCheck_For_Updates, SIGNAL(triggered()), &m_autoUpdateCheck, SLOT(forcedAutoUpdateCheck()));

    // Connect actions from ui
    //connect(ui.actionAdd_Link, SIGNAL(triggered()), this, SLOT(addLink()));
    ui.actionSerial->setData(LinkInterface::SERIAL_LINK);
    ui.actionTCP->setData(LinkInterface::TCP_LINK);
    ui.actionUDP->setData(LinkInterface::UDP_LINK);
    ui.actionUDPClient->setData(LinkInterface::UDP_CLIENT_LINK);
    connect(ui.actionSerial,SIGNAL(triggered()),this,SLOT(addLink()));
    connect(ui.actionTCP,SIGNAL(triggered()),this,SLOT(addLink()));
    connect(ui.actionUDP,SIGNAL(triggered()),this,SLOT(addLink()));
    connect(ui.actionUDPClient,SIGNAL(triggered()),this,SLOT(addLink()));
    connect(ui.actionAdvanced_Mode,SIGNAL(triggered(bool)),this,SLOT(setAdvancedMode(bool)));

    // Connect internal actions
    connect(UASManager::instance(), SIGNAL(UASCreated(UASInterface*)), this, SLOT(UASCreated(UASInterface*)));
    connect(UASManager::instance(), SIGNAL(activeUASSet(UASInterface*)), this, SLOT(setActiveUAS(UASInterface*)));
    connect(UASManager::instance(), SIGNAL(UASDeleted(UASInterface*)), this, SLOT(UASDeleted(UASInterface*)));
    if (UASInterface *initialUas =
            UASManager::instance()->silentGetActiveUAS()) {
        bindPlannerVehicle(initialUas);
    }

    // Unmanned System controls
    connect(ui.actionLiftoff, SIGNAL(triggered()), UASManager::instance(), SLOT(launchActiveUAS()));
    connect(ui.actionLand, SIGNAL(triggered()), UASManager::instance(), SLOT(returnActiveUAS()));
    connect(ui.actionEmergency_Land, SIGNAL(triggered()), UASManager::instance(), SLOT(stopActiveUAS()));
    connect(ui.actionEmergency_Kill, SIGNAL(triggered()), UASManager::instance(), SLOT(killActiveUAS()));
    connect(ui.actionShutdownMAV, SIGNAL(triggered()), UASManager::instance(), SLOT(shutdownActiveUAS()));
    connect(ui.actionConfiguration, SIGNAL(triggered()), UASManager::instance(), SLOT(configureActiveUAS()));

    // Views actions
    connect(ui.actionFlightView, SIGNAL(triggered()), this, SLOT(loadPilotView()));
    connect(ui.actionSimulation_View, SIGNAL(triggered()), this, SLOT(loadSimulationView()));
    connect(ui.actionEngineersView, SIGNAL(triggered()), this, SLOT(loadEngineerView()));
    connect(ui.actionMissionView, SIGNAL(triggered()), this, SLOT(loadOperatorView()));
    connect(ui.actionUnconnectedView, SIGNAL(triggered()), this, SLOT(loadUnconnectedView()));
    connect(ui.actionHardwareConfig,SIGNAL(triggered()),this,SLOT(loadHardwareConfigView()));
    connect(ui.actionSoftwareConfig,SIGNAL(triggered()),this,SLOT(loadSoftwareConfigView()));
    connect(helpViewAction, &QAction::triggered, this, &MainWindow::loadHelpView);
    connect(ui.actionTerminalView,SIGNAL(triggered()),this,SLOT(loadTerminalView()));

    connect(ui.actionFirmwareUpdateView, SIGNAL(triggered()), this, SLOT(loadFirmwareUpdateView()));
    connect(ui.actionMavlinkView, SIGNAL(triggered()), this, SLOT(loadMAVLinkView()));

    connect(ui.actionReloadStylesheet, SIGNAL(triggered()), this, SLOT(reloadStylesheet()));
    connect(ui.actionSelectStylesheet, SIGNAL(triggered()), this, SLOT(selectStylesheet()));

    // Help Actions
    connect(ui.actionOnline_Documentation, SIGNAL(triggered()), this, SLOT(showHelp()));
    connect(ui.actionDeveloper_Credits, SIGNAL(triggered()), this, SLOT(showCredits()));
    connect(ui.actionProject_Roadmap_2, SIGNAL(triggered()), this, SLOT(showRoadMap()));

    // Custom widget actions
    connect(ui.actionNewCustomWidget, SIGNAL(triggered()), this, SLOT(createCustomWidget()));
    connect(ui.actionLoadCustomWidgetFile, SIGNAL(triggered()), this, SLOT(loadCustomWidget()));

    // Audio output
    ui.actionMuteAudioOutput->setChecked(GAudioOutput::instance()->isMuted());
    connect(GAudioOutput::instance(), SIGNAL(mutedChanged(bool)), ui.actionMuteAudioOutput, SLOT(setChecked(bool)));
    connect(ui.actionMuteAudioOutput, SIGNAL(triggered(bool)), GAudioOutput::instance(), SLOT(mute(bool)));

    // User interaction
    // NOTE: Joystick thread is not started and
    // configuration widget is not instantiated
    // unless it is actually used
    // so no ressources spend on this.

    // Configuration
    // Joystick
    connect(ui.actionJoystickSettings, SIGNAL(triggered()), this, SLOT(configure()));
    // Application Settings
    connect(ui.actionSettings, SIGNAL(triggered()), this, SLOT(showSettings()));

    if (isAdvancedMode)
    {
        ui.menuPerspectives->menuAction()->setVisible(true);
        ui.menuNetwork->menuAction()->setVisible(true);
    }
    else
    {
        ui.menuPerspectives->menuAction()->setVisible(false);
        ui.menuNetwork->menuAction()->setVisible(false);
    }
    // MP10 exposes TOOLS independently of the legacy advanced-mode toggle.
    ui.menuTools->menuAction()->setVisible(true);

    connect(ui.actionDebug_Console,SIGNAL(triggered()),debugOutput.data(),SLOT(show()));
    connect(ui.actionSimulate, SIGNAL(triggered(bool)), this, SLOT(simulateLink(bool)));

    //Disable simulation view until we ensure it's operational.
    ui.actionSimulate->setVisible(false);
    ui.actionSimulationView->setVisible(false);
}

void MainWindow::showHelp()
{
    if(!QDesktopServices::openUrl(QUrl("http://qgroundcontrol.org/users/start")))
    {
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Critical);
        msgBox.setText("Could not open help in browser");
        msgBox.setInformativeText("To get to the online help, please open http://qgroundcontrol.org/user_guide in a browser.");
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setDefaultButton(QMessageBox::Ok);
        msgBox.exec();
    }
}

void MainWindow::showCredits()
{
    if(!QDesktopServices::openUrl(QUrl("http://qgroundcontrol.org/credits")))
    {
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Critical);
        msgBox.setText("Could not open credits in browser");
        msgBox.setInformativeText("To get to the online help, please open http://qgroundcontrol.org/credits in a browser.");
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setDefaultButton(QMessageBox::Ok);
        msgBox.exec();
    }
}

void MainWindow::showRoadMap()
{
    if(!QDesktopServices::openUrl(QUrl("http://qgroundcontrol.org/dev/roadmap")))
    {
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Critical);
        msgBox.setText("Could not open roadmap in browser");
        msgBox.setInformativeText("To get to the online help, please open http://qgroundcontrol.org/roadmap in a browser.");
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setDefaultButton(QMessageBox::Ok);
        msgBox.exec();
    }
}

void MainWindow::configure()
{
    if (!joystickWidget)
    {
        if (!joystick->isRunning())
        {
            joystick->start();
        }
        joystickWidget = new JoystickWidget(joystick);
    }
    joystickWidget->show();
}

void MainWindow::showSettings()
{
    if (settingsDialog) {
        settingsDialog->show();
        settingsDialog->raise();
        settingsDialog->activateWindow();
        return;
    }
    settingsDialog = new QDialog(this);
    settingsDialog->setAttribute(Qt::WA_DeleteOnClose);
    settingsDialog->setWindowTitle(tr("APM Planner 3.0 Settings"));
    auto *layout = new QVBoxLayout(settingsDialog);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *settingsWidget = new QGCSettingsWidget(settingsDialog);
    layout->addWidget(settingsWidget);
    settingsDialog->resize(1014, 839);
    settingsDialog->show();
}

void MainWindow::showMissionElevation()
{
    if (plannerElevationDialog) {
        plannerElevationDialog->show();
        plannerElevationDialog->raise();
        plannerElevationDialog->activateWindow();
        return;
    }
    if (!plannerViewModel) {
        showStatusMessage(tr("PLAN is not ready for an elevation graph."));
        return;
    }

    auto *display = new MissionElevationDisplay(
        plannerViewModel, ElevationSourceService::instance());
    if (!display->HasUsableMission()) {
        display->deleteLater();
        showStatusMessage(tr(
            "Need at least two route points (HOME + waypoint or two waypoints) for an elevation graph."));
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("ElevationGraphWindow"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Elevation Graph — APM Planner"));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(display);
    dialog->resize(820, 420);
    plannerElevationDialog = dialog;
    connect(dialog, &QObject::destroyed, this, [this]() {
        plannerElevationDialog = nullptr;
    });
    dialog->show();
}

void MainWindow::showDeveloperTools()
{
    if (!isAdvancedMode) {
        setAdvancedMode(true);
    }
    loadHardwareConfigView();
    if (!hardwareSetupView || !hardwareSetupView->showDeveloperTools()) {
        showStatusMessage(tr("Developer Tools are not available."));
    }
}

void MainWindow::showPluginManager()
{
    auto *core = qobject_cast<QGCCore *>(QCoreApplication::instance());
    auto *window = new QWidget(this, Qt::Window);
    window->setObjectName(QStringLiteral("PluginManagerWindow"));
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowModality(Qt::NonModal);
    window->setWindowTitle(tr("Plugin Manager"));
    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QmlPluginManagerView(
        core ? core->qmlPluginManager() : nullptr, window));
    window->resize(900, 600);
    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::showLogDownload()
{
    auto *dialog = new LogDownloadDialog(this);
    dialog->setObjectName(QStringLiteral("LogDownloadWindow"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

QString MainWindow::plannerAltitudeUnits() const
{
    if (plannerViewModel) return plannerViewModel->AltUnits();
    const QString configured = canonicalPlannerLinearUnits(
        settings.value(QStringLiteral("altunits"),
                       QStringLiteral("Meters")).toString());
    return configured.isEmpty() ? QStringLiteral("Meters") : configured;
}

QString MainWindow::plannerDistanceUnits() const
{
    if (plannerViewModel) return plannerViewModel->DistUnits();
    const QString configured = canonicalPlannerLinearUnits(
        settings.value(QStringLiteral("distunits"),
                       QStringLiteral("Meters")).toString());
    return configured.isEmpty() ? QStringLiteral("Meters") : configured;
}

void MainWindow::setPlannerAltitudeUnits(const QString &units)
{
    const QString canonical = canonicalPlannerLinearUnits(units);
    if (canonical.isEmpty()) return;
    if (plannerViewModel) {
        plannerViewModel->setAltUnits(canonical);
        return;
    }
    if (plannerAltitudeUnits() == canonical) return;
    settings.setValue(QStringLiteral("altunits"), canonical);
    emit plannerAltitudeUnitsChanged(canonical);
}

void MainWindow::setPlannerDistanceUnits(const QString &units)
{
    const QString canonical = canonicalPlannerLinearUnits(units);
    if (canonical.isEmpty()) return;
    if (plannerViewModel) {
        plannerViewModel->setDistUnits(canonical);
        return;
    }
    if (plannerDistanceUnits() == canonical) return;
    settings.setValue(QStringLiteral("distunits"), canonical);
    emit plannerDistanceUnitsChanged(canonical);
}



bool MainWindow::configLink(int linkid)
{
    // Go searching for this link's configuration window
    QList<QAction*> actions = ui.menuNetwork->actions();

    bool found(false);

    //const int32_t& linkIndex(LinkManager::instance()->getLinks().indexOf(linkid));
    //const int32_t& linkID(LinkManager::instance()->getLinks()[linkIndex]->getId());

    foreach (QAction* action, actions)
    {
        if (action->data().toInt() == linkid)
        { // LinkManager::instance()->getLinks().indexOf(link)
            found = true;
            action->trigger(); // Show the Link Config Dialog
        }
    }

    return found;
}
void MainWindow::addLink()
{
    QAction *send = qobject_cast<QAction*>(sender());
    if (!send)
    {
        return;
    }
    int newid = 0;
    if (send->data() == LinkInterface::SERIAL_LINK)
    {
        newid = LinkManagerFactory::addSerialConnection();
    }
    else if (send->data() == LinkInterface::TCP_LINK)
    {
        newid = LinkManagerFactory::addTcpConnection(QHostAddress::LocalHost, "", 5760, false);
    }
    else if (send->data() == LinkInterface::UDP_LINK)
    {
        newid = LinkManagerFactory::addUdpConnection(QHostAddress::LocalHost,14550);
    }
    else if (send->data() == LinkInterface::UDP_CLIENT_LINK)
    {
        newid = LinkManagerFactory::addUdpClientConnection(QHostAddress("192.168.4.1"),14550);
    }
    addLink(newid);
    for (int i=0;i<ui.menuNetwork->actions().size();i++)
    {
        if (ui.menuNetwork->actions().at(i)->data().toInt() == newid)
        {
            //Link already exists!
            ui.menuNetwork->actions().at(i)->trigger();
            return;
        }
    }

    //CommConfigurationWindow *commWidget = new CommConfigurationWindow()
}

void MainWindow::addLink(int linkid)
{
    for (int i=0;i<ui.menuNetwork->actions().size();i++)
    {
        if (ui.menuNetwork->actions().at(i)->data().toInt() == linkid)
        {
            //Link already exists!
            return;
        }
    }
    CommConfigurationWindow* commWidget = new CommConfigurationWindow(linkid, 0, this);
    commsWidgetList.append(commWidget);
    connect(commWidget,SIGNAL(destroyed(QObject*)),this,SLOT(commsWidgetDestroyed(QObject*)));
    QAction* action = commWidget->getAction();
    action->setData(linkid);
    ui.menuNetwork->addAction(action);

    // Error handling
    //connect(link, SIGNAL(communicationError(QString,QString)), this, SLOT(showCriticalMessage(QString,QString)), Qt::QueuedConnection);
}

void MainWindow::linkError(int linkid,QString errorstring)
{
    Q_UNUSED(linkid)

    QWidget* parent = QApplication::activeWindow();
    if (!parent) {
        parent = this;
    }
    QMessageBox::information(parent,"Link Error",errorstring);
}

void MainWindow::simulateLink(bool simulate) {
    if (!simulationLink.isNull())
        simulationLink = new MAVLinkSimulationLink(":/demo-log.txt");
    simulationLink->connectLink(simulate);
}

void MainWindow::commsWidgetDestroyed(QObject *obj)
{
    if (commsWidgetList.contains(obj))
    {
        commsWidgetList.removeOne(obj);
    }
}

void MainWindow::bindPlannerVehicle(UASInterface *uas)
{
    if (!plannerViewModel)
        return;

    if (plannerParameterVehicle) {
        disconnect(plannerParameterVehicle.data(), nullptr,
                   plannerViewModel.data(), nullptr);
    }
    if (QGCUASParamManager *manager =
            LinkManager::instance()->parameterManager()) {
        disconnect(manager, nullptr, plannerViewModel.data(), nullptr);
    }
    plannerParameterVehicle = uas;
    plannerViewModel->setVehicleParameterAccess({}, {});

    UAS *vehicle = qobject_cast<UAS *>(uas);
    plannerViewModel->setMissionTransferController(
            vehicle ? vehicle->missionTransferController() : nullptr);
    plannerViewModel->setVehicleType(uas ? uas->getSystemType() : -1);

    if (!uas) {
        plannerViewModel->setVehicleHomeProvider({});
        plannerViewModel->setVehiclePositionProvider({});
        return;
    }

    const QPointer<UASInterface> activeVehicle(uas);
    const auto parameterComponent = [activeVehicle](
            const QString &name, double *value) -> int {
        if (!activeVehicle || !value) {
            return -1;
        }
        LinkManager *const links = LinkManager::instance();
        VehicleTargetManager *const targets = links->vehicleTargetManager();
        QGCUASParamManager *const manager = links->parameterManager();
        const VehicleTargetLease target = targets
            ? targets->acquireTarget() : VehicleTargetLease{};
        if (!manager || !target.isValid()
            || target.endpoint.systemId != activeVehicle->getUASID()) {
            return -1;
        }
        QList<int> components = manager->getComponentIds();
        if (components.removeOne(1)) {
            components.prepend(1);
        }
        for (int component : components) {
            QVariant parameter;
            if (!manager->getParameterValue(component, name, parameter)) {
                continue;
            }
            bool ok = false;
            const double converted = parameter.toDouble(&ok);
            if (ok && std::isfinite(converted)) {
                *value = converted;
                return component;
            }
        }
        return -1;
    };
    plannerViewModel->setVehicleParameterAccess(
        [parameterComponent](const QString &name, double *value) {
            return parameterComponent(name, value) >= 0;
        },
        [activeVehicle, parameterComponent](const QString &name,
                                             double value) {
            double previousValue = 0.0;
            const int component = parameterComponent(name, &previousValue);
            if (!activeVehicle || component < 0) {
                return false;
            }
            QGCUASParamManager *const manager =
                LinkManager::instance()->parameterManager();
            if (!manager) {
                return false;
            }
            return manager->writeParameters(
                component,
                QVariantList{QVariantMap{
                    {QStringLiteral("name"), name},
                    {QStringLiteral("value"), value}
                }}) != 0;
        });
    QGCUASParamManager *const manager =
        LinkManager::instance()->parameterManager();
    VehicleTargetManager *const targets =
        LinkManager::instance()->vehicleTargetManager();
    if (manager) {
        connect(manager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                plannerViewModel,
                [this, activeVehicle, targets](
                    int component, const QString &name,
                    const QVariant &value) {
            const VehicleTargetLease target = targets
                ? targets->acquireTarget() : VehicleTargetLease{};
            if (plannerViewModel && activeVehicle && target.isValid()
                && target.endpoint.systemId == activeVehicle->getUASID()
                && target.endpoint.componentId == component) {
                plannerViewModel->UpdateVehicleParameter(name, value);
            }
        });
    }
    connect(uas, &UASInterface::parameterManagerChanged,
            plannerViewModel, [this](QGCUASParamManager *) {
        if (plannerViewModel) {
            plannerViewModel->RefreshVehicleParameters();
        }
    });
    plannerViewModel->setVehiclePositionProvider(
            [activeVehicle](double *latitude, double *longitude,
                            double *altitudeRelative) {
        if (!activeVehicle || !latitude || !longitude || !altitudeRelative
            || !activeVehicle->globalPositionKnown()) {
            return false;
        }
        *latitude = activeVehicle->getLatitude();
        *longitude = activeVehicle->getLongitude();
        *altitudeRelative = activeVehicle->getAltitudeRelative();
        return std::isfinite(*latitude)
                && std::isfinite(*longitude)
                && std::isfinite(*altitudeRelative);
    });
    plannerViewModel->setVehicleHomeProvider(
            [activeVehicle](double *latitude, double *longitude,
                            double *altitudeAsl) {
        if (!activeVehicle || !latitude || !longitude || !altitudeAsl
            || !activeVehicle->globalPositionKnown()) {
            return false;
        }
        *latitude = activeVehicle->getLatitude();
        *longitude = activeVehicle->getLongitude();
        *altitudeAsl = activeVehicle->getAltitudeAMSL();
        return std::isfinite(*latitude)
                && std::isfinite(*longitude)
                && std::isfinite(*altitudeAsl);
    });
}

void MainWindow::setActiveUAS(UASInterface* uas)
{
    bindPlannerVehicle(uas);

    // Enable and rename menu
    //    ui.menuUnmanned_System->setTitle(uas->getUASName());
    //    if (!ui.menuUnmanned_System->isEnabled()) ui.menuUnmanned_System->setEnabled(true);
    if (settings.contains(getWindowStateKey()))
    {
        if (SubMainWindow *win = qobject_cast<SubMainWindow *>(
                centerStack->currentWidget())) {
            win->restoreState(settings.value(getWindowStateKey()).toByteArray(),
                              QGC::applicationVersion());
        }
    }
    if (auto *dockableView = qobject_cast<DockableView *>(
            centerStack->currentWidget())) {
        restoreDockableLayout(dockableView);
    }

}

void MainWindow::UASSpecsChanged(int uas)
{
    UASInterface* activeUAS = UASManager::instance()->getActiveUAS();
    if (activeUAS)
    {
        if (activeUAS->getUASID() == uas)
        {
            //            ui.menuUnmanned_System->setTitle(activeUAS->getUASName());
        }
    }
    else
    {
        // Last system deleted
        //        ui.menuUnmanned_System->setTitle(tr("No System"));
        //        ui.menuUnmanned_System->setEnabled(false);
    }
}

void MainWindow::UASCreated(UASInterface* uas)
{

        // The pilot, operator and engineer views were not available on startup, enable them now
    ui.actionFlightView->setEnabled(true);
    ui.actionMissionView->setEnabled(true);
    ui.actionEngineersView->setEnabled(true);
    // The UAS actions are not enabled without connection to system
    ui.actionLiftoff->setEnabled(true);
    ui.actionLand->setEnabled(true);
    ui.actionEmergency_Kill->setEnabled(true);
    ui.actionEmergency_Land->setEnabled(true);
    ui.actionShutdownMAV->setEnabled(true);

    QIcon icon;
    // Set matching icon
    switch (uas->getSystemType())
    {
    case MAV_TYPE_GENERIC:
        icon = QIcon(":files/images/mavs/generic.svg");
        break;
    case MAV_TYPE_FIXED_WING:
        icon = QIcon(":files/images/mavs/fixed-wing.svg");
        break;
    case MAV_TYPE_QUADROTOR:
        icon = QIcon(":files/images/mavs/quadrotor.svg");
        break;
    case MAV_TYPE_COAXIAL:
        icon = QIcon(":files/images/mavs/coaxial.svg");
        break;
    case MAV_TYPE_HELICOPTER:
        icon = QIcon(":files/images/mavs/helicopter.svg");
        break;
    case MAV_TYPE_ANTENNA_TRACKER:
        icon = QIcon(":files/images/mavs/antenna-tracker.svg");
        break;
    case MAV_TYPE_GCS:
        icon = QIcon(":files/images/mavs/groundstation.svg");
        break;
    case MAV_TYPE_AIRSHIP:
        icon = QIcon(":files/images/mavs/airship.svg");
        break;
    case MAV_TYPE_FREE_BALLOON:
        icon = QIcon(":files/images/mavs/free-balloon.svg");
        break;
    case MAV_TYPE_ROCKET:
        icon = QIcon(":files/images/mavs/rocket.svg");
        break;
    case MAV_TYPE_GROUND_ROVER:
        icon = QIcon(":files/images/mavs/ground-rover.svg");
        break;
    case MAV_TYPE_SURFACE_BOAT:
        icon = QIcon(":files/images/mavs/surface-boat.svg");
        break;
    case MAV_TYPE_SUBMARINE:
        icon = QIcon(":files/images/mavs/submarine.svg");
        break;
    case MAV_TYPE_HEXAROTOR:
        icon = QIcon(":files/images/mavs/hexarotor.svg");
        break;
    case MAV_TYPE_OCTOROTOR:
        icon = QIcon(":files/images/mavs/octorotor.svg");
        break;
    case MAV_TYPE_TRICOPTER:
        icon = QIcon(":files/images/mavs/tricopter.svg");
        break;
    case MAV_TYPE_FLAPPING_WING:
        icon = QIcon(":files/images/mavs/flapping-wing.svg");
        break;
    case MAV_TYPE_KITE:
        icon = QIcon(":files/images/mavs/kite.svg");
        break;
    default:
        icon = QIcon(":files/images/mavs/unknown.svg");
        break;
    }

    // XXX The multi-UAS selection menu has been disabled for now,
    // its redundant with right-clicking the UAS in the list.
    // this code piece might be removed later if this is the final
    // conclusion (May 2013)
    //        QAction* uasAction = new QAction(icon, tr("Select %1 for control").arg(uas->getUASName()), ui.menuConnected_Systems);
    //        connect(uas, SIGNAL(systemRemoved()), uasAction, SLOT(deleteLater()));
    //        connect(uasAction, SIGNAL(triggered()), uas, SLOT(setSelected()));
    //        ui.menuConnected_Systems->addAction(uasAction);


    connect(uas, SIGNAL(systemSpecsChanged(int)), this, SLOT(UASSpecsChanged(int)));

    // HIL
    showHILConfigurationWidget(uas);


    // MP10 configuration pages replace the legacy .qgw parameter docks.
    // Loading them here also appended view-dependent actions back into TOOLS
    // after its exact application-tool inventory had been built.


    if (uas->getAutopilotType() == MAV_AUTOPILOT_PX4)
    {
        // Dock widgets
        if (!detectionDockWidget)
        {
            detectionDockWidget = new QDockWidget(tr("Object Recognition"), this);
            detectionDockWidget->setWidget( new ObjectDetectionView("/files/images/patterns", this) );
            detectionDockWidget->setObjectName("OBJECT_DETECTION_DOCK_WIDGET");
            //addTool(detectionDockWidget, tr("Object Recognition"), Qt::RightDockWidgetArea);
        }

        if (!watchdogControlDockWidget)
        {
            watchdogControlDockWidget = new QDockWidget(tr("Process Control"), this);
            watchdogControlDockWidget->setWidget( new WatchdogControl(this) );
            watchdogControlDockWidget->setObjectName("WATCHDOG_CONTROL_DOCKWIDGET");
            //addTool(watchdogControlDockWidget, tr("Process Control"), Qt::BottomDockWidgetArea);
        }
    }

    // Change the view only if this is the first UAS

    // If this is the first connected UAS, it is both created as well as
    // the currently active UAS
    if (UASManager::instance()->getUASList().size() == 1)
    {
        // Load last view if setting is present
        if (settings.contains("CURRENT_VIEW_WITH_UAS_CONNECTED"))
        {
            /*int view = settings.value("CURRENT_VIEW_WITH_UAS_CONNECTED").toInt();
                switch (view)
                {
                case VIEW_ENGINEER:
                    loadEngineerView();
                    break;
                case VIEW_MAVLINK:
                    loadMAVLinkView();
                    break;
                case VIEW_FIRMWAREUPDATE:
                    loadFirmwareUpdateView();
                    break;
                case VIEW_FLIGHT:
                    loadPilotView();
                    break;
                case VIEW_SIMULATION:
                    loadSimulationView();
                    break;
                case VIEW_UNCONNECTED:
                    loadUnconnectedView();
                    break;
                case VIEW_MISSION:
                default:
                    loadOperatorView();
                    break;
                }*/
        }
        else
        {
            // loadOperatorView();
        }
    }

    //}

    //    if (!ui.menuConnected_Systems->isEnabled()) ui.menuConnected_Systems->setEnabled(true);
    //    if (!ui.menuUnmanned_System->isEnabled()) ui.menuUnmanned_System->setEnabled(true);

    // Reload view state in case new widgets were added
    loadViewState();
}

void MainWindow::UASDeleted(UASInterface* uas)
{
    Q_UNUSED(uas);
    // activeUASSet is normally emitted first, but reinject the manager state
    // here as a lifetime guard for removals performed by legacy link paths.
    bindPlannerVehicle(UASManager::instance()->silentGetActiveUAS());
    if (UASManager::instance()->getUASList().count() == 0)
    {
        // Last system deleted
        //        ui.menuUnmanned_System->setTitle(tr("No System"));
        //        ui.menuUnmanned_System->setEnabled(false);
    }
}

/**
 * Stores the current view state
 */
void MainWindow::storeViewState()
{
    if (!aboutToCloseFlag)
    {
        QWidget *currentWidget = centerStack->currentWidget();
        if (auto *dockableView = qobject_cast<DockableView *>(currentWidget)) {
            const QByteArray layout = dockableView->saveLayout();
            if (!layout.isEmpty()) {
                settings.setValue(getWindowStateKey()
                                      + QStringLiteral("_DOCK_LAYOUT_V1"),
                                  layout);
            }
        } else if (SubMainWindow *win = qobject_cast<SubMainWindow *>(currentWidget)) {
            const QList<QDockWidget *> widgets = win->findChildren<QDockWidget *>();
            QStringList widgetNames;
            for (QDockWidget *widget : widgets) {
                widgetNames.append(widget->objectName());
            }
            settings.setValue(getWindowStateKey() + QStringLiteral("WIDGETS"),
                              widgetNames.join(QLatin1Char(',')));
            settings.setValue(getWindowStateKey(),
                              win->saveState(QGC::applicationVersion()));
        }
        settings.setValue(getWindowStateKey()+"CENTER_WIDGET", centerStack->currentIndex());
        // Although we want save the state of the window, we do not want to change the top-leve state (minimized, maximized, etc)
        // therefore this state is stored here and restored after applying the rest of the settings in the new
        // perspective.
        windowStateVal = this->windowState();
        settings.setValue(getWindowGeometryKey(), saveGeometry());
    }
}

void MainWindow::loadViewState()
{
    // Restore center stack state
    const int index = settings.value(getWindowStateKey()+"CENTER_WIDGET", -1).toInt();
    // The offline plot view is usually the consequence of a logging run, always show the realtime view first
    if (centerStack->indexOf(engineeringView) == index)
    {
        // Rewrite to realtime plot
        //index = centerStack->indexOf(linechartWidget);
    }

    // A perspective has one canonical central page. Persisted numeric stack
    // indices become stale whenever pages are added or reordered, and trusting
    // them can select a blank, unrelated widget for DATA or PLAN.
    QWidget *expectedWidget = nullptr;
    switch (currentView)
    {
        case VIEW_HARDWARE_CONFIG:
            expectedWidget = configView;
            break;
        case VIEW_SOFTWARE_CONFIG:
            expectedWidget = softwareConfigView;
            break;
        case VIEW_HELP:
            expectedWidget = helpView;
            break;
        case VIEW_ENGINEER:
            expectedWidget = engineeringView;
            break;
        case VIEW_FLIGHT:
            expectedWidget = pilotView;
            break;
        case VIEW_MAVLINK:
            expectedWidget = mavlinkView;
            break;
        case VIEW_MISSION:
            expectedWidget = plannerView;
            break;
        case VIEW_SIMULATION:
            expectedWidget = simView;
            break;
        case VIEW_TERMINAL:
            expectedWidget = terminalView;
            break;
        case VIEW_UNCONNECTED:
        case VIEW_FULL:
        default:
            break;
    }

    if (expectedWidget && centerStack->indexOf(expectedWidget) >= 0) {
        centerStack->setCurrentWidget(expectedWidget);
    } else if (index >= 0 && index < centerStack->count()) {
        centerStack->setCurrentIndex(index);
    } else {
        if (detectionDockWidget) detectionDockWidget->hide();
        if (watchdogControlDockWidget) watchdogControlDockWidget->hide();
        if (controlDockWidget) controlDockWidget->hide();
        if (listDockWidget) listDockWidget->show();
    }

    QWidget *currentWidget = centerStack->currentWidget();
    if (auto *dockableView = qobject_cast<DockableView *>(currentWidget)) {
        restoreDockableLayout(dockableView);
    } else if (SubMainWindow *win = qobject_cast<SubMainWindow *>(currentWidget)) {
        // Legacy Qt docking remains isolated to views that have not been ported yet.
        if (settings.contains(getWindowStateKey() + QStringLiteral("WIDGETS"))) {
            const QStringList widgetNames = settings.value(
                getWindowStateKey() + QStringLiteral("WIDGETS"))
                                                .toString()
                                                .split(QLatin1Char(','),
                                                       Qt::SkipEmptyParts);
            for (const QString &widgetName : widgetNames) {
                QLOG_DEBUG() << "Loading widget:" << widgetName;
                loadDockWidget(widgetName);
            }
        }
        if (settings.contains(getWindowStateKey())) {
            win->restoreState(settings.value(getWindowStateKey()).toByteArray(),
                              QGC::applicationVersion());
        }
    }
}

void MainWindow::restoreDockableLayout(DockableView *view)
{
    if (!view) {
        return;
    }
    const QString layoutKey = getWindowStateKey()
        + QStringLiteral("_DOCK_LAYOUT_V1");
    if (!settings.contains(layoutKey)) {
        return;
    }
    if (!view->restoreLayout(settings.value(layoutKey).toByteArray())) {
        // Do not retry a corrupt/incompatible state on every activation. The
        // view has already restored all core panels before reporting failure.
        settings.remove(layoutKey);
        settings.sync();
    }
}
void MainWindow::setAdvancedMode(bool mode)
{
    isAdvancedMode = mode;
    ui.actionAdvanced_Mode->setChecked(mode);
    ui.menuPerspectives->menuAction()->setVisible(mode);
    ui.menuTools->menuAction()->setVisible(true);
    ui.menuNetwork->menuAction()->setVisible(mode);

    for (QMap<QDockWidget*,QWidget*>::const_iterator i=dockToTitleBarMap.constBegin();
         i!=dockToTitleBarMap.constEnd();i++)
    {
        QWidget *widget = i.key()->titleBarWidget();
        i.key()->setTitleBarWidget(i.value());
        dockToTitleBarMap[i.key()] = widget;
    }
}

void MainWindow::loadEngineerView()
{
    if (currentView != VIEW_ENGINEER)
    {
        storeViewState();
        currentView = VIEW_ENGINEER;
        ui.actionEngineersView->setChecked(true);
        loadViewState();
    }
}

void MainWindow::loadOperatorView()
{
    if (currentView != VIEW_MISSION)
    {
        storeViewState();
        currentView = VIEW_MISSION;
    }
    ui.actionMissionView->setChecked(true);
    loadViewState();
}
void MainWindow::loadHardwareConfigView()
{
    if (currentView != VIEW_HARDWARE_CONFIG)
    {
        storeViewState();
        currentView = VIEW_HARDWARE_CONFIG;
        ui.actionHardwareConfig->setChecked(true);
        loadViewState();
    }
}

void MainWindow::loadSoftwareConfigView()
{
    if (currentView != VIEW_SOFTWARE_CONFIG)
    {
        storeViewState();
        currentView = VIEW_SOFTWARE_CONFIG;
        ui.actionSoftwareConfig->setChecked(true);
        loadViewState();
    }
}

void MainWindow::loadHelpView()
{
    if (currentView != VIEW_HELP)
    {
        storeViewState();
        currentView = VIEW_HELP;
        helpViewAction->setChecked(true);
        loadViewState();
    }
}

void MainWindow::loadTerminalView()
{
    if (currentView != VIEW_TERMINAL)
    {
        storeViewState();
        currentView = VIEW_TERMINAL;
        ui.actionTerminalView->setChecked(true);
        loadViewState();
    }
}


void MainWindow::loadUnconnectedView()
{
    if (currentView != VIEW_UNCONNECTED)
    {
        storeViewState();
        currentView = VIEW_UNCONNECTED;
        ui.actionUnconnectedView->setChecked(true);
        loadViewState();
    }
}

void MainWindow::loadPilotView()
{
    if (currentView != VIEW_FLIGHT)
    {
        storeViewState();
        currentView = VIEW_FLIGHT;
    }
    ui.actionFlightView->setChecked(true);
    loadViewState();
}

void MainWindow::loadSimulationView()
{
    if (currentView != VIEW_SIMULATION)
    {
        storeViewState();
        currentView = VIEW_SIMULATION;
        ui.actionSimulation_View->setChecked(true);
        loadViewState();
    }
}

void MainWindow::loadMAVLinkView()
{
    if (currentView != VIEW_MAVLINK)
    {
        storeViewState();
        currentView = VIEW_MAVLINK;
        ui.actionMavlinkView->setChecked(true);
        loadViewState();
    }
}

void MainWindow::loadFirmwareUpdateView()
{
    if (currentView != VIEW_FIRMWAREUPDATE)
    {
        storeViewState();
        currentView = VIEW_FIRMWAREUPDATE;
        ui.actionFirmwareUpdateView->setChecked(true);
        loadViewState();
    }
}

//void MainWindow::loadDataView(QString fileName)
//{
//    // Plot is now selected, now load data from file
//    if (dataView)
//    {
//        //dataView->setCentralWidget(new QGCDataPlot2D(this));
//        QGCDataPlot2D *plot = qobject_cast<QGCDataPlot2D*>(dataView->centralWidget());
//        if (plot)
//        {
//            plot->loadFile(fileName);
//        }
//    }
//    /*QStackedWidget *centerStack = dynamic_cast<QStackedWidget*>(centralWidget());
//    if (centerStack)
//    {
//        centerStack->setCurrentWidget(dataView);
//        dataplotWidget->loadFile(fileName);
//    }*/
//}


QList<QAction*> MainWindow::listLinkMenuActions(void)
{
    return ui.menuNetwork->actions();
}

#ifdef MOUSE_ENABLED_LINUX
bool MainWindow::x11Event(XEvent *event)
{
    emit x11EventOccured(event);
    //QLOG_DEBUG() << "XEvent occured...";
    return false;
}
#endif // MOUSE_ENABLED_LINUX

void MainWindow::showAbout()
{
    AboutDialog* dialog = new AboutDialog(this);
    dialog->exec();
    dialog->hide();
    delete dialog;
    dialog = NULL;
}

void MainWindow::showAutoUpdateDownloadDialog(QString version, QString releaseType, QString url, QString name)
{
    QLOG_DEBUG() << "Update Available! Show Update Dialog";
    QLOG_DEBUG() << "Ver:" << version << "type:" << releaseType;

    if (m_dialog) {
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }
    auto *dialog = new AutoUpdateDialog(version, name, url, this);
    m_dialog = dialog;
    const AutoUpdateCheck::ReleaseChannel channel =
        AutoUpdateCheck::releaseChannelFromString(releaseType);
    connect(dialog, &AutoUpdateDialog::autoUpdateCancelled,
            this, [this, dialog, channel](const QString &skippedVersion) {
                m_autoUpdateCheck.setSkippedVersion(channel, skippedVersion);
                dialog->deleteLater();
                if (m_dialog == dialog) {
                    m_dialog = nullptr;
                }
            });
    connect(dialog, &QDialog::finished, this, [this, dialog]() {
        if (m_dialog == dialog) {
            m_dialog = nullptr;
        }
    });
    connect(dialog, &QObject::destroyed, this, [this, dialog]() {
        if (m_dialog == dialog) {
            m_dialog = nullptr;
        }
    });
    dialog->show();
}

void MainWindow::showNoUpdateAvailDialog()
{
    QMessageBox::information(this,"Update Check", "No new update available!",QMessageBox::Ok);
}
void MainWindow::enableHeartbeat(bool enabled)
{
    QSettings settings;
    settings.setValue(QStringLiteral("CHK_GCSheartbeat"), enabled);
    settings.sync();

    if (m_heartbeatEnabled != enabled)
    {
        m_heartbeatEnabled = enabled;
        for (int i=0;i<UASManager::instance()->getUASList().size();i++)
        {
            UASManager::instance()->getUASList().at(i)->setHeartbeatEnabled(enabled);
        }
        storeSettings();
    }
}

void MainWindow::setGroundStationSystemId(int systemId)
{
    if (systemId < 1 || systemId > 255) {
        systemId = 255;
    }

    QGC::setMavlinkID(static_cast<quint8>(systemId));
    QSettings settings;
    settings.setValue(QStringLiteral("gcsid"), systemId);
    settings.sync();

    if (MAVLinkProtocol *protocol = LinkManager::instance()->getProtocol()) {
        protocol->setSystemId(static_cast<quint8>(systemId));
    }
    const QList<UASInterface *> systems = UASManager::instance()->getUASList();
    for (UASInterface *uas : systems) {
        uas->setGroundStationSystemId(systemId);
    }
}

void MainWindow::showConnectionOptions()
{
    ConnectionOptionsWindow::OpenWindow(this);
}

void MainWindow::openAdditionalConnection(const QString &connection, int baud)
{
    int linkId = -1;
    if (connection == QStringLiteral("TCP")) {
        bool accepted = false;
        const QString addressText = QInputDialog::getText(
            this, tr("TCP Connection"), tr("Remote IP address"),
            QLineEdit::Normal, QStringLiteral("127.0.0.1"), &accepted);
        if (!accepted) {
            return;
        }
        QHostAddress address;
        if (!address.setAddress(addressText.trimmed())) {
            showCriticalMessage(tr("TCP Connection"),
                                tr("Enter a valid IPv4 or IPv6 address."));
            return;
        }
        const int port = QInputDialog::getInt(
            this, tr("TCP Connection"), tr("Remote port"), 5760,
            1, 65535, 1, &accepted);
        if (!accepted) {
            return;
        }
        linkId = LinkManagerFactory::addTcpConnection(
            address, addressText.trimmed(), port, false);
    } else if (connection == QStringLiteral("UDP")) {
        bool accepted = false;
        const int port = QInputDialog::getInt(
            this, tr("UDP Connection"), tr("Local listen port"), 14550,
            1, 65535, 1, &accepted);
        if (!accepted) {
            return;
        }
        linkId = LinkManagerFactory::addUdpConnection(
            QHostAddress::Any, port);
    } else if (connection == QStringLiteral("UDPCl")) {
        bool accepted = false;
        const QString addressText = QInputDialog::getText(
            this, tr("UDP Client Connection"), tr("Remote IP address"),
            QLineEdit::Normal, QStringLiteral("127.0.0.1"), &accepted);
        if (!accepted) {
            return;
        }
        QHostAddress address;
        if (!address.setAddress(addressText.trimmed())) {
            showCriticalMessage(tr("UDP Client Connection"),
                                tr("Enter a valid IPv4 or IPv6 address."));
            return;
        }
        const int port = QInputDialog::getInt(
            this, tr("UDP Client Connection"), tr("Remote port"), 14550,
            1, 65535, 1, &accepted);
        if (!accepted) {
            return;
        }
        linkId = LinkManagerFactory::addUdpClientConnection(address, port);
    } else if (connection == QStringLiteral("WS")) {
        showInfoMessage(
            tr("WebSocket Connection"),
            tr("WebSocket transport is not available in this build yet."));
        return;
    } else if (!connection.trimmed().isEmpty()) {
        linkId = LinkManagerFactory::addSerialConnection(
            connection.trimmed(), baud);
    }

    if (linkId < 0) {
        showCriticalMessage(tr("Connections"),
                            tr("The selected connection could not be created."));
        return;
    }
    if (!LinkManager::instance()->getLinkConnected(linkId)
        && !LinkManager::instance()->connectLink(linkId)) {
        showCriticalMessage(
            tr("Connections"),
            tr("Could not open %1.").arg(connection));
        return;
    }
    showStatusMessage(tr("Additional connection opened: %1")
                          .arg(LinkManager::instance()->getLinkDetail(linkId)));
}

void MainWindow::showTerminalConsole()
{
    if(m_terminalDialog == NULL){
        m_terminalDialog = new QDialog(NULL);
        TerminalConsole *terminalConsole = new TerminalConsole(this);
        QVBoxLayout* vLayout = new QVBoxLayout(m_terminalDialog);
        vLayout->setMargin(0);
        vLayout->addWidget(terminalConsole);
        m_terminalDialog->resize(640,325);
        m_terminalDialog->show();
        connect(m_terminalDialog, SIGNAL(finished(int)), this, SLOT(closeTerminalConsole()));
    }

    if (m_terminalDialog){
        m_terminalDialog->raise();
    }
}

void MainWindow::showMavlinkInspector()
{
    pruneMavlinkInspectorWindows();

    auto *inspector = new QGCMAVLinkInspector;
    auto *window = new MAVLinkInspectorWindow(inspector, this);
    m_mavlinkInspectorWindows.append(window);
    connect(window, &QObject::destroyed,
            this, &MainWindow::pruneMavlinkInspectorWindows);

    if (logPlayer) {
        logPlayer->addMavlinkInspector(inspector);
    }

    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::closeMavlinkInspectorWindows()
{
    const QList<QPointer<MAVLinkInspectorWindow>> windows =
        m_mavlinkInspectorWindows;
    m_mavlinkInspectorWindows.clear();

    for (const QPointer<MAVLinkInspectorWindow> &window : windows) {
        // Direct deletion is deliberate during shutdown: WA_DeleteOnClose
        // alone would leave replay receivers alive until the event loop gets
        // another deferred-delete pass.
        delete window.data();
    }
}

void MainWindow::pruneMavlinkInspectorWindows()
{
    m_mavlinkInspectorWindows.erase(
        std::remove_if(
            m_mavlinkInspectorWindows.begin(),
            m_mavlinkInspectorWindows.end(),
            [](const QPointer<MAVLinkInspectorWindow> &window) {
                return window.isNull();
            }),
        m_mavlinkInspectorWindows.end());
}

void MainWindow::closeTerminalConsole()
{
    if (m_terminalDialog){
        m_terminalDialog->close();
        m_terminalDialog->deleteLater();
        m_terminalDialog = NULL;
    }
}
