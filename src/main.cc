/*=====================================================================

QGroundControl Open Source Ground Control Station

(c) 2009 - 2011 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>

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
 *   @brief Main executable
 *   @author Lorenz Meier <mavteam@student.ethz.ch>
 *
 */


#include "MainWindow.h"
#include "QGCCore.h"
#include "configuration.h"
#include "logging.h"

#include <iostream>

#include <QApplication>
#include <QFile>
#include <QMutexLocker>
#include <QTextStream>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#endif

#ifdef APM_SETUP_ROUTE_RUNTIME_AUDIT
#include "SetupRouteRuntimeAudit.h"
#include "SigningTransportRuntimeAudit.h"

#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstring>
#include <memory>
#endif

/* SDL does ugly things to main() */
#ifdef main
#undef main
#endif

// Create the base logging category as defined in logging.h
Q_LOGGING_CATEGORY(apmGeneral, "apm.general");

// Install a message handler so you do not need
// the MSFT debug tools installed to se
// qDebug(), qWarning(), qCritical and qAbort
#ifdef Q_OS_WIN
void msgHandler( QtMsgType type, const char* msg )
{
    const char symbols[] = { 'I', 'E', '!', 'X' };
    QString output = QString("[%1] %2").arg( symbols[type] ).arg( msg );
    std::cerr << output.toStdString() << std::endl;
    if( type == QtFatalMsg ) abort();
}
#endif

// Path for file logging
static QString sLogPath;

static void setUtf8Encoding(QTextStream &stream)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif
}

// Message handler for logging provides console and file output
// The handler itself has to be reentrant and threadsafe!
void loggingMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    // The message handler has to be thread safe
    static QMutex mutex;
    QMutexLocker localLoc(&mutex);

    static QFile logFile;
    static QTextStream logStream;

    if (!logFile.isOpen() && !sLogPath.isEmpty()) {
        logFile.setFileName(sLogPath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            logStream.setDevice(&logFile);
            setUtf8Encoding(logStream);
        }
    }

    QString outMessage(qFormatLogMessage(type, context, message));  // just format only once
    if (logFile.isOpen())
    {
        logStream << outMessage << '\n';
        logStream.flush();
    }

    LogWindowSingleton::instance().write(outMessage);   // log to debug window

    fprintf(stderr, "%s\n", qPrintable(outMessage));    // log to console
}


/**
 * @brief Starts the application
 *
 * @param argc Number of commandline arguments
 * @param argv Commandline arguments
 * @return exit code, 0 for normal exit and !=0 for error cases
 */
int main(int argc, char *argv[])
{
#ifdef APM_SETUP_ROUTE_RUNTIME_AUDIT
    bool setupRouteAuditRequested = false;
    bool signingTransportAuditRequested = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--setup-route-audit") == 0) {
            setupRouteAuditRequested = true;
        }
        if (std::strcmp(argv[index], "--signing-transport-audit") == 0)
            signingTransportAuditRequested = true;
    }

    // Construct before application singletons so their static destructors
    // release signing/file locks before the isolated directory is removed.
    static std::unique_ptr<QTemporaryDir> setupRouteAuditSettings;
    if (setupRouteAuditRequested || signingTransportAuditRequested) {
        // The audit constructs production pages but must not observe or mutate
        // the operator's settings and writable application-data directories.
        QStandardPaths::setTestModeEnabled(true);
        setupRouteAuditSettings.reset(new QTemporaryDir);
        if (!setupRouteAuditSettings->isValid()) {
            std::cerr << "SETUP route audit: temporary settings directory failed"
                      << std::endl;
            return 2;
        }
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           setupRouteAuditSettings->path());
        // Every production audit uses fresh app data too: the local Signing
        // dialog must never create/unlock a vault from an earlier test/user run.
        qputenv("APM_PLANNER_HOME", setupRouteAuditSettings->path().toUtf8());
    }
#endif

// install the message handler
#ifdef Q_OS_WIN
    //qInstallMsgHandler( msgHandler );
#endif

#ifdef Q_OS_LINUX
    // Part of a fix for "#646 Primary Flight Display acts as a CPU hog..." wich consumed lots
    // of cpu cylcles when APM-Plannner is used on machines with Intel Graphics.
    // The complete fix needs the environment variable "QSG_RENDER_LOOP=threaded" to be set before
    // APM-Planner is started in order to work correctly.
    // Be aware that setting only the environment variable seems to fix the problem, but without
    // this code change the application could crash or hang after a while.
    // see https://forum.qt.io/topic/68721/high-cpu-usage/4
    // MUST be called before construction of QApplication - in our case QGCCore.
    QCoreApplication::setAttribute(Qt::AA_X11InitThreads);
#endif

    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

    // Init application
    QGCCore core(argc, argv);

#ifdef APM_SETUP_ROUTE_RUNTIME_AUDIT
    if (signingTransportAuditRequested) return RunSigningTransportRuntimeAudit();
    if (setupRouteAuditRequested) {
        return RunSetupRouteRuntimeAudit();
    }
#endif

    // Init logging
    // create filename and path for logfile like "apmlog_20160529.txt"
    // one logfile for every day. Size is not limited
    QString logFileName("apmlog_");
    logFileName.append(QDateTime::currentDateTime().toString("yyyyMMdd"));
    logFileName.append(".txt");
    sLogPath = QString(QDir(QGC::appDataDirectory()).filePath(logFileName));

    // Just keep the 5 recent logfiles and delete the rest.
    QDir logDirectory(QGC::appDataDirectory(), "apmlog*", QDir::Name, QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot);
    QStringList logFileList(logDirectory.entryList());
    // As the file list is sorted we can delete index 0 cause its the oldest one
    while(logFileList.size() > 5)
    {
        logDirectory.remove(logFileList.at(0));
        logFileList.pop_front();
    }

    // Add sperator for better orientation in Logfiles
    QFile logFile(sLogPath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        QTextStream logStream(&logFile);
        setUtf8Encoding(logStream);
        logStream << "\n\n**************************************************\n\n";
        logStream.flush();
    }

    // set up logging pattern
#if QT_VERSION < QT_VERSION_CHECK(5, 5, 0)
    // QT < 5.5.x does not support qInfo() logging macro and no info-formatting too
    QString logPattern("[%{time yyyyMMdd h:mm:ss.zzz} %{if-debug}DEBUG%{endif}%{if-warning}WARN %{endif}%{if-critical}ERROR%{endif}%{if-fatal}FATAL%{endif}] - %{message}");
#else
    QString logPattern("[%{time yyyyMMdd h:mm:ss.zzz} %{if-debug}DEBUG%{endif}%{if-info}INFO %{endif}%{if-warning}WARN %{endif}%{if-critical}ERROR%{endif}%{if-fatal}FATAL%{endif}] - %{message}");
#endif

    qSetMessagePattern(logPattern);

    // install the message handler for logging
    qInstallMessageHandler(loggingMessageHandler);

    // Setup output for logging category
    QLoggingCategory::setFilterRules(QStringLiteral("apm.general.debug=true"));

    // start the application
    core.initialize();
    return core.exec();
}
