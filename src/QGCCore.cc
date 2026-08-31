/*=====================================================================

QGroundControl Open Source Ground Control Station

(c) 2009, 2010 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>

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
 *   @brief Implementation of class QGCCore
 *
 *   @author Lorenz Meier <mavteam@student.ethz.ch>
 *
 */

#include "QGCCore.h"
#include "AppSettingsMigration.h"
#include "logging.h"
#include "configuration.h"
#include "QGC.h"
#include "MainWindow.h"
#include "GAudioOutput.h"

#ifdef OPAL_RT
#include "OpalLink.h"
#endif
#include "UDPLink.h"
#include "MAVLinkSimulationLink.h"

#include <QFile>
#include <QFlags>
#include <QThread>
#include <QSplashScreen>
#include <QPixmap>
#include <QDesktopWidget>
#include <QPainter>
#include <QStyleFactory>
#include <QAction>

/**
 * @brief Constructor for the main application.
 *
 * This constructor initializes and starts the whole application. It takes standard
 * command-line parameters
 *
 * @param argc The number of command-line parameters
 * @param argv The string array of parameters
 **/


QGCCore::QGCCore(int &argc, char* argv[]) : QApplication(argc, argv)
{
    // Set settings format
    QSettings::setDefaultFormat(QSettings::IniFormat);

    // Establish the 3.0 identity before any component creates a default QSettings.
    this->setOrganizationName(QLatin1String(QGC_ORGANIZATION_NAME));
    this->setOrganizationDomain("org.ardupilot");
    this->setApplicationName(QGC_APPLICATION_NAME);
    this->setApplicationDisplayName(QGC_APPLICATION_NAME);
    this->setApplicationVersion(QGC_APPLICATION_VERSION);

    // The application name is part of the QSettings namespace. Copy the complete
    // legacy profile once, without replacing values already written by 3.0. This
    // preserves custom data, log, mission and parameter paths as well as all other
    // preferences before the first settings read.
    AppSettingsMigration::migrateLegacyUserSettings(
                QLatin1String(QGC_ORGANIZATION_NAME),
                QLatin1String(QGC_LEGACY_APPLICATION_NAME),
                QLatin1String(QGC_APPLICATION_NAME));

    m_mouseWheelFilter = new QGCMouseWheelEventFilter(this);

    connect(this, SIGNAL(aboutToQuit()), this, SLOT(aboutToQuit()));
}

void QGCCore::aboutToQuit()
{
    LinkManager::instance()->shutdown();
}

void QGCCore::initialize()
{
    QLOG_INFO() << "QGCCore::initialize()";
    QLOG_INFO() << "Current Build Info";
    QLOG_INFO() << "Git Hash:" << define2string(GIT_HASH);
    QLOG_INFO() << "Git Commit:" << define2string(GIT_COMMIT);
    QLOG_INFO() << "APPLICATION_NAME:" << define2string(QGC_APPLICATION_NAME);
    QLOG_INFO() << "APPLICATION_VERSION:" << define2string(QGC_APPLICATION_VERSION);
    QLOG_INFO() << "APP_PLATFORM:" << define2string(APP_PLATFORM);
    QLOG_INFO() << "APP_TYPE:" << define2string(APP_TYPE);


    // Record the running version without discarding user preferences. Version
    // changes are normal upgrades and must never clear the settings store.
    QSettings settings;

    // Show user an upgrade message if QGC got upgraded (see code below, after splash screen)
    bool upgraded = false;
    QString lastApplicationVersion;
    if (settings.contains("QGC_APPLICATION_VERSION"))
    {
        const QString qgcVersion = settings.value("QGC_APPLICATION_VERSION").toString();
        if (qgcVersion != QGC_APPLICATION_VERSION)
        {
            lastApplicationVersion = qgcVersion;
            settings.setValue("QGC_APPLICATION_VERSION", QGC_APPLICATION_VERSION);
            upgraded = true;
        }
    }
    else
    {
        settings.setValue("QGC_APPLICATION_VERSION", QGC_APPLICATION_VERSION);
    }
    settings.sync();


    // Show splash screen
    // Use the version-neutral logo; the previous splash visibly branded the
    // application as 2.0 even after the executable identity had changed.
    QPixmap splashImage(":/files/images/apm_planner_logo_splash.png");
    QSplashScreen* splashScreen = new QSplashScreen(splashImage);
    // Delete splash screen after mainWindow was displayed
    splashScreen->setAttribute(Qt::WA_DeleteOnClose);
    splashScreen->show();
    processEvents();
    splashScreen->showMessage(tr("Loading application fonts"), Qt::AlignLeft | Qt::AlignBottom, QColor(62, 93, 141));

    // Exit main application when last window is closed
    connect(this, SIGNAL(lastWindowClosed()), this, SLOT(quit()));

    // Load application font
    QFontDatabase fontDatabase;
    const QString fontFileName(":/general/vera.ttf"); ///< Font file is part of the QRC file and compiled into the app
    //const QString fontFamilyName = "Bitstream Vera Sans";
    if(!QFile::exists(fontFileName)) printf("ERROR! font file: %s DOES NOT EXIST!\n", fontFileName.toStdString().c_str());
    fontDatabase.addApplicationFont(fontFileName);
    // Avoid Using setFont(). In the Qt docu you can read the following:
    //     "Warning: Do not use this function in conjunction with Qt Style Sheets."
    // setFont(fontDatabase.font(fontFamilyName, "Roman", 12));

    // Start the comm link manager
    splashScreen->showMessage(tr("Starting Communication Links"), Qt::AlignLeft | Qt::AlignBottom, QColor(62, 93, 141));
    startLinkManager();

    // Start the UAS Manager
    splashScreen->showMessage(tr("Starting UAS Manager"), Qt::AlignLeft | Qt::AlignBottom, QColor(62, 93, 141));
    startUASManager();

    // Start the user interface
    splashScreen->showMessage(tr("Starting User Interface"), Qt::AlignLeft | Qt::AlignBottom, QColor(62, 93, 141));
    // Start UI

    // Connect links
    // to make sure that all components are initialized when the
    // first messages arrive
    //UDPLink* udpLink = new UDPLink(QHostAddress::Any, 14550);
    //MainWindow::instance()->addLink(udpLink);
    // Listen on Multicast-Address 239.255.77.77, Port 14550
    //QHostAddress * multicast_udp = new QHostAddress("239.255.77.77");
    //UDPLink* udpLink = new UDPLink(*multicast_udp, 14550);

#ifdef OPAL_RT
    // Add OpalRT Link, but do not connect
    OpalLink* opalLink = new OpalLink();
    MainWindow::instance()->addLink(opalLink);
#endif
#ifdef SIMULATION_LINK
    MAVLinkSimulationLink* simulationLink = new MAVLinkSimulationLink(":/demo-log.txt");
    simulationLink->disconnect();
#endif

    mainWindow = MainWindow::instance();

    // Remove splash screen
    splashScreen->finish(mainWindow);

    if (upgraded) mainWindow->showInfoMessage(
                tr("Upgrade Complete"),
                tr("APM Planner 3.0 has been upgraded from version %1 to version %2. Your user preferences have been retained.")
                .arg(lastApplicationVersion).arg(QGC_APPLICATION_VERSION));

}

/**
 * @brief Destructor for the groundstation. It destroys all loaded instances.
 *
 **/
QGCCore::~QGCCore()
{
    QGC::saveSettings();
    QGC::close();
    // Delete singletons
    // First systems
    delete UASManager::instance();
    // then links

    // Finally the main window
    //delete MainWindow::instance();
    //The main window now autodeletes on close.
}

/**
 * @brief Start the link managing component.
 *
 * The link manager keeps track of all communication links and provides the global
 * packet queue. It is the main communication hub
 **/
void QGCCore::startLinkManager()
{
    QLOG_INFO() << "Start Link Manager";
    LinkManager::instance();
}

/**
 * @brief Start the Unmanned Air System Manager
 *
 **/
void QGCCore::startUASManager()
{
    QLOG_INFO() << "Start UAS Manager";
    // Load UAS plugins
    QDir pluginsDir(qApp->applicationDirPath());

#if defined(Q_OS_WIN)
    if (pluginsDir.dirName().toLower() == "debug" || pluginsDir.dirName().toLower() == "release")
        pluginsDir.cdUp();
#elif defined(Q_OS_LINUX)
    if (pluginsDir.dirName().toLower() == "debug" || pluginsDir.dirName().toLower() == "release")
        pluginsDir.cdUp();
#elif defined(Q_OS_MAC)
    if (pluginsDir.dirName() == "MacOS") {
        pluginsDir.cdUp();
        pluginsDir.cdUp();
        pluginsDir.cdUp();
    }
#endif
    pluginsDir.cd("plugins");

    UASManager::instance();

    // Load plugins

    QStringList pluginFileNames;

    foreach (const QString& fileName, pluginsDir.entryList(QDir::Files)) {
        QPluginLoader loader(pluginsDir.absoluteFilePath(fileName));
        QObject *plugin = loader.instance();
        if (plugin) {
            //populateMenus(plugin);
            pluginFileNames += fileName;
            //printf(QString("Loaded plugin from " + fileName + "\n").toStdString().c_str());
        }
    }
}
