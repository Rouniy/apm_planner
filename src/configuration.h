#ifndef QGC_CONFIGURATION_H
#define QGC_CONFIGURATION_H

#include "globalobject.h"
#include <QString>
#include <QDateTime>
#include <QDir>
#include <QCoreApplication>

/** @brief Polling interval in ms */
#define SERIAL_POLL_INTERVAL 100

/** @brief Heartbeat emission rate, in Hertz (times per second) */
#define MAVLINK_HEARTBEAT_DEFAULT_RATE 1
#define WITH_TEXT_TO_SPEECH 1

// Product identity and the namespace used by default-constructed QSettings.
#define QGC_APPLICATION_NAME "APM Planner 3.0"
#define QGC_APPLICATION_VERSION "3.0.0"
#define QGC_LEGACY_APPLICATION_NAME "APM Planner"
#define QGC_ORGANIZATION_NAME "ardupilot"
// Keep the established on-disk data root so an upgrade never strands logs,
// missions or parameter files when the visible product identity changes.
#define APP_DATA_DIRECTORY "/apmplanner2"
#define LOG_DIRECTORY "/dataflashLogs"
#define PARAMETER_DIRECTORY "/parameters"
#define MISSION_DIRECTORY "/missions"
#define MAVLINK_LOG_DIRECTORY "/tlogs"
#define MAVLINK_LOGFILE_EXT ".tlog"

#ifndef APP_TYPE
#define APP_TYPE stable // or "daily" for master branch builds
#endif

// May be overridden by release packaging once the signed 3.0 manifest service
// is deployed. The project-owned raw manifest keeps development checks from
// depending on the retired firmware-server root URL.
#ifndef APM_UPDATE_MANIFEST_URL
#define APM_UPDATE_MANIFEST_URL \
    "https://raw.githubusercontent.com/Rouniy/apm_planner/master/apm_planner_version.json"
#endif

#ifndef APP_PLATFORM

#ifdef Q_OS_MACX
#define APP_PLATFORM osx
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
// The published Linux desktop package and update manifest use the historical
// ubuntu64 identifier. Qt's processor macro is set reliably by every CMake
// build, unlike the old qmake-only Q_LINUX_64 define.
#define APP_PLATFORM ubuntu64
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_32)
#define APP_PLATFORM ubuntu32
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM_64)
#define APP_PLATFORM linuxarm64
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM)
#define APP_PLATFORM linuxarm32
#elif defined(Q_LINUX_64) && defined(Q_UBUNTU)
#define APP_PLATFORM ubuntu64
#elif defined(Q_LINUX_64) && defined(Q_ARCHLINUX)
#define APP_PLATFORM archlinux64
#elif defined(Q_LINUX_64) && defined(Q_FEDORA)
#define APP_PLATFORM fedora64
#elif defined(Q_LINUX_64)
#define APP_PLATFORM debian64
#elif defined(Q_OS_LINUX) && defined(Q_UBUNTU)
#define APP_PLATFORM ubuntu32
#elif defined(Q_LINUX_32) && defined(Q_ARCHLINUX)
#define APP_PLATFORM archlinux32
#elif defined(Q_LINUX_32) && defined(Q_FEDORA)
#define APP_PLATFORM fedora32
#elif defined(Q_OS_LINUX)
#define APP_PLATFORM debian32
#elif defined(Q_OPENBSD)
#define APP_PLATFORM OpenBSD
#else
#define APP_PLATFORM win
#endif

#endif

namespace QGC

{
const static QString APPNAME = "APMPLANNER3";
const static QString COMPANYNAME = "ARDUPILOT";
// Persistent QMainWindow state schema, not the product version. Keep it stable
// so migrated dock/window layouts remain restorable across the 3.0 upgrade.
const static int APPLICATIONVERSION = 2030;
// Mission Planner uses 255 for the ground station on a fresh profile.
const static quint8 defaultMavlinkSystemId = 255;

    inline void close(){
        GlobalObject* global = GlobalObject::sharedInstance();
        delete global;
        global = nullptr;
    }

    inline void loadSettings(){
        GlobalObject::sharedInstance()->loadSettings();
    }

    inline void saveSettings(){
        GlobalObject::sharedInstance()->saveSettings();
    }

    inline QString fileNameAsTime(){
        return GlobalObject::sharedInstance()->fileNameAsTime();
    }

    inline bool makeDirectory(const QString& dir){
        return GlobalObject::sharedInstance()->makeDirectory(dir);
    }

    inline QString appDataDirectory(){
        return GlobalObject::sharedInstance()->appDataDirectory();
    }

    inline void setAppDataDirectory(const QString& dir){
        GlobalObject::sharedInstance()->setAppDataDirectory(dir);
    }

    inline QString MAVLinkLogDirectory(){
        return GlobalObject::sharedInstance()->MAVLinkLogDirectory();
    }

    inline void setMAVLinkLogDirectory(const QString& dir){
        GlobalObject::sharedInstance()->setMAVLinkLogDirectory(dir);
    }

    inline QString logDirectory(){
        return GlobalObject::sharedInstance()->logDirectory();
    }

    inline void setLogDirectory(const QString& dir){
        GlobalObject::sharedInstance()->setLogDirectory(dir);
    }

    inline QString parameterDirectory(){
        return GlobalObject::sharedInstance()->parameterDirectory();
    }

    inline void setParameterDirectory(const QString& dir){
        GlobalObject::sharedInstance()->setParameterDirectory(dir);
    }

    inline QString missionDirectory(){
        return GlobalObject::sharedInstance()->missionDirectory();
    }

    inline void setMissionDirectory(const QString& dir){
        GlobalObject::sharedInstance()->setMissionDirectory(dir);
    }

    inline quint8 MavlinkID(){
        return GlobalObject::sharedInstance()->MavlinkID();
    }

    inline void setMavlinkID(const quint8 ID){
        GlobalObject::sharedInstance()->setMavlinkID(ID);
    }

    inline quint8 ComponentID(){
        return GlobalObject::sharedInstance()->ComponentID();
    }

    inline void setComponentID(const quint8 ID){
        GlobalObject::sharedInstance()->setComponentID(ID);
    }


    //Returns the absolute parth to the files, data, qml support directories
    //It could be in 1 of 2 places under Linux
    inline QString shareDirectory(){
        return GlobalObject::sharedInstance()->shareDirectory();
    }

    inline QRegExp paramSplitRegExp() {
        return QRegExp("\t|,|=");
    }

    inline QRegExp paramLineSplitRegExp() {
        return QRegExp("\r|\n");
    }

}

#endif // QGC_CONFIGURATION_H
