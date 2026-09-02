#include "logging.h"
#include "configuration.h"
#include "globalobject.h"
#include "AppPaths.h"
#include "mavlink.h"
#include <QSettings>
#include <QDateTime>
#include <QDir>

GlobalObject* GlobalObject::sharedInstance()
{
    static GlobalObject* _globalInstance = nullptr;
    if (_globalInstance) {
        return _globalInstance;
    }
    // Create the global object
    _globalInstance = new GlobalObject();
    return _globalInstance;
}

GlobalObject::GlobalObject()
{
    loadSettings();
}

GlobalObject::~GlobalObject()
{
}

void GlobalObject::loadSettings()
{
    QSettings settings;
    const uint loadedMavlinkId = settings.value(
        QStringLiteral("gcsid"), defaultMavlinkID()).toUInt();
    settings.beginGroup("GLOBAL_SETTINGS");
    m_appDataDirectory = settings.value("APP_DATA_DIRECTORY", defaultAppDataDirectory()).toString();
    m_logDirectory = settings.value("LOG_DIRECTORY", defaultLogDirectory()).toString();
    m_MAVLinklogDirectory = settings.value("MAVLINK_LOG_DIRECTORY", defaultMAVLinkLogDirectory()).toString();
    m_parameterDirectory = settings.value("PARAMETER_DIRECTORY", defaultParameterDirectory()).toString();
    m_missionDirectory = settings.value("MISSION_DIRECTORY", defaultMissionDirectory()).toString();
    m_mavlinkID = static_cast<quint8>(
        loadedMavlinkId >= 1 && loadedMavlinkId <= 255
            ? loadedMavlinkId
            : defaultMavlinkID());
    m_componentID = static_cast<quint8>(settings.value("COMPONENT_ID", defaultComponentID()).toUInt());

    settings.endGroup();
}

void GlobalObject::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("gcsid"), m_mavlinkID);
    settings.beginGroup("GLOBAL_SETTINGS");
    settings.setValue("APP_DATA_DIRECTORY", m_appDataDirectory);
    settings.setValue("LOG_DIRECTORY", m_logDirectory);
    settings.setValue("MAVLINK_LOG_DIRECTORY", m_MAVLinklogDirectory);
    QLOG_DEBUG() << "save tlog dir to:" << m_MAVLinklogDirectory;
    settings.setValue("PARAMETER_DIRECTORY", m_parameterDirectory);
    settings.setValue("MISSION_DIRECTORY", m_missionDirectory);
    settings.setValue("COMPONENT_ID", m_componentID);

    settings.sync();
}

QString GlobalObject::fileNameAsTime()
{
    QDateTime timeNow;
    timeNow = timeNow.currentDateTime();
    return "/" + timeNow.toString("yyyy-MM-dd hh-mm-ss") + MAVLINK_LOGFILE_EXT;
}

bool GlobalObject::makeDirectory(const QString& dir)
{
    return AppPaths::ensureDirectory(dir);
}

//
// App Data Directory
//

QString GlobalObject::defaultAppDataDirectory()
{
    return AppPaths::writableDataDirectory();
}

QString GlobalObject::appDataDirectory()
{
    makeDirectory(m_appDataDirectory);
    return m_appDataDirectory;
}

void GlobalObject::setAppDataDirectory(const QString &dir)
{
    QLOG_DEBUG() << "Set app dir to:" << dir;
    m_appDataDirectory = dir;
}

//
// Log Data Directory
//

QString GlobalObject::defaultLogDirectory()
{
    return QDir(defaultAppDataDirectory()).filePath(QStringLiteral("dataflashLogs"));
}

QString GlobalObject::logDirectory()
{
    makeDirectory(m_logDirectory);
    return m_logDirectory;
}

void GlobalObject::setLogDirectory(const QString &dir)
{
    QLOG_DEBUG() << "Set dataflash dir to:" << dir;
    m_logDirectory = dir;
}

//
// MAVLink Log Data Directory
//

QString GlobalObject::defaultMAVLinkLogDirectory()
{
    return QDir(defaultAppDataDirectory()).filePath(QStringLiteral("tlogs"));
}

QString GlobalObject::MAVLinkLogDirectory()
{
    makeDirectory(m_MAVLinklogDirectory);
    return m_MAVLinklogDirectory;
}

void GlobalObject::setMAVLinkLogDirectory(const QString &dir)
{
    QLOG_DEBUG() << "Set tlog dir to:" << dir;
    m_MAVLinklogDirectory = dir;
}

//
// Parameter Data Directory
//

QString GlobalObject::defaultParameterDirectory()
{
    return QDir(defaultAppDataDirectory()).filePath(QStringLiteral("parameters"));
}

QString GlobalObject::parameterDirectory()
{
    makeDirectory(m_parameterDirectory);
    return m_parameterDirectory;
}

void GlobalObject::setParameterDirectory(const QString &dir)
{
    QLOG_DEBUG() << "Set param dir to:" << dir;
    m_parameterDirectory = dir;
}

//
// Parameter Data Directory
//

QString GlobalObject::defaultMissionDirectory()
{
    return QDir(defaultAppDataDirectory()).filePath(QStringLiteral("missions"));
}

QString GlobalObject::missionDirectory()
{
    makeDirectory(m_missionDirectory);
    return m_missionDirectory;
}

void GlobalObject::setMissionDirectory(const QString &dir)
{
    QLOG_DEBUG() << "Set mission dir to:" << dir;
    m_missionDirectory = dir;
}

//
// Mavlink ID of APM PLanner
//

quint8 GlobalObject::defaultMavlinkID()
{
    return QGC::defaultMavlinkSystemId;
}

quint8 GlobalObject::MavlinkID()
{
    return m_mavlinkID;
}

void GlobalObject::setMavlinkID(const quint8 mavlinkID)
{
    m_mavlinkID = mavlinkID;
}

//
// Component ID of APM Planner when uploading Waypoints via mavlink
//

quint8 GlobalObject::defaultComponentID()
{
    return MAV_COMP_ID_MISSIONPLANNER;
}

quint8 GlobalObject::ComponentID()
{
    return m_componentID;
}

void GlobalObject::setComponentID(const quint8 componentID)
{
    m_componentID = componentID;
}


//
// Share Directory
//

QString GlobalObject::shareDirectory()
{
    return AppPaths::resourceRoot();
}
