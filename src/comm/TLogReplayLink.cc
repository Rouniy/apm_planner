#include "TLogReplayLink.h"
#include "UASManager.h"
#include "UASObject.h"
#include "ArduPilotMegaMAV.h"
#include "LinkManager.h"
#include "MainWindow.h"

#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QMetaObject>

TLogReplayLink::TLogReplayLink(QObject *parent) :
    LinkInterface(),
    m_toBeDeleted(false),
    m_threadRun(false),
    m_speedVar(50),
    m_posVar(0),
    m_pause(false),
    m_mavlinkDecoder(new MAVLinkDecoder()),
    m_ownsMavlinkDecoder(true)
{
    Q_UNUSED(parent);
    qRegisterMetaType<mavlink_message_t>("mavlink_message_t");
}

TLogReplayLink::~TLogReplayLink()
{
    disconnect();
    if (m_ownsMavlinkDecoder) {
        delete m_mavlinkDecoder;
    }
    m_mavlinkDecoder = nullptr;
}
int TLogReplayLink::getId() const
{
    return 1;
}
QString TLogReplayLink::getName() const
{
    return "AP2SimulationLink";
}

QString TLogReplayLink::getShortName() const
{
    return "AP2SimLink";
}

QString TLogReplayLink::getDetail() const
{
    return "sim";
}
void TLogReplayLink::requestReset()
{

}
bool TLogReplayLink::isConnected() const
{
    return false;
}
qint64 TLogReplayLink::getConnectionSpeed() const
{
    return 115200;
}

bool TLogReplayLink::connect()
{
    start();
    return true;
}
bool TLogReplayLink::disconnect()
{
    stop();
    requestInterruption();
    if (QThread::currentThread() != this) {
        wait();
    }
    return true;
}
qint64 TLogReplayLink::bytesAvailable()
{
    return 0;
}
void TLogReplayLink::writeBytes(const char *bytes, qint64 length)
{
    Q_UNUSED(bytes);
    Q_UNUSED(length);
}
void TLogReplayLink::readBytes()
{

}
void TLogReplayLink::setSpeed(int speed)
{
    m_variableAccessMutex.lock();
    m_speedVar = speed;
    m_variableAccessMutex.unlock();
}
void TLogReplayLink::setPosition(qint64 pos)
{
    m_variableAccessMutex.lock();
    m_posVar = pos;
    m_variableAccessMutex.unlock();
}

void TLogReplayLink::play()
{
    m_pause = false;
}

void TLogReplayLink::pause()
{
    m_pause = true;
}
bool TLogReplayLink::isPaused()
{
    return m_pause;
}
void TLogReplayLink::setMavlinkDecoder(MAVLinkDecoder *decoder)
{
    if (decoder == m_mavlinkDecoder) {
        return;
    }
    if (m_ownsMavlinkDecoder) {
        delete m_mavlinkDecoder;
    }
    if (decoder) {
        m_mavlinkDecoder = decoder;
        m_ownsMavlinkDecoder = false;
    } else {
        m_mavlinkDecoder = new MAVLinkDecoder();
        m_ownsMavlinkDecoder = true;
    }
}
void TLogReplayLink::setMavlinkInspector(QGCMAVLinkInspector *inspector)
{
    QObject::disconnect(m_inspectorConnection);
    m_inspectorConnection = QMetaObject::Connection();
    if (inspector) {
        m_inspectorConnection = QObject::connect(
            this, &TLogReplayLink::inspectorMessage,
            inspector, &QGCMAVLinkInspector::receiveMessage,
            Qt::QueuedConnection);
    }
}

void TLogReplayLink::run()
{
    m_pause = false;
    m_threadRun = true;
    emit connected(this);
    emit connected(true);
    emit connected();
    QFile file(m_logFile);
    file.open(QIODevice::ReadOnly);
    int bytesize = 0;
    qint64 msecs = QDateTime::currentMSecsSinceEpoch();
    QMetaObject::invokeMethod(LinkManager::instance(), []() {
        if (!LinkManager::instance()->isShuttingDown()) {
            MainWindow::instance()->toolBar().disableConnectWidget(true);
            MainWindow::instance()->toolBar().overrideDisableConnectWidget(true);
        }
    }, Qt::QueuedConnection);
    int privSpeedVar = 100;
    mavlink_message_t message;
    mavlink_status_t status;
    QByteArray timebuf;
    bool firsttime = true;
    int delay = 0;
    qint64 lastLogTime = 0;
    qint64 lastPcTime = 0;
    bool nexttime = false;
    qint64 privatepos = 0;
    UASInterface *replayUas = nullptr;
    int replaySystemId = -1;
    while (!file.atEnd() && m_threadRun && !isInterruptionRequested())
    {
        if (QDateTime::currentMSecsSinceEpoch() - msecs > 1000)
        {
            msecs = QDateTime::currentMSecsSinceEpoch();
            m_variableAccessMutex.lock();

            //m_speedVar is a value, between 25 and 175. These are speed percentages.
            privSpeedVar = m_speedVar; //50 - ((m_speedVar) / 4);
            if (privatepos != m_posVar)
            {
                privatepos = m_posVar;
                if (privatepos > 0 && privatepos < 100)
                {
                    file.seek((privatepos / 100.0) * file.size());
                }
            }
            m_variableAccessMutex.unlock();
        }
        emit logProgress(file.pos(),file.size());
        QByteArray bytes = file.read(128);
        bytesize+=128;

        for (int i=0;i<bytes.size();i++)
        {
            unsigned int decodeState = mavlink_parse_char(14, (uint8_t)(bytes[i]), &message, &status);
            if (decodeState != 1)
            {
                //Not a mavlink byte!
                if (nexttime)
                {
                    timebuf.append(bytes[i]);
                }
                if (timebuf.size() == 8)
                {
                    nexttime = false;

                    //Should be a timestamp for the next packet.
                    quint64 logmsecs = quint64(static_cast<unsigned char>(timebuf.at(0))) << 56;
                    logmsecs += quint64(static_cast<unsigned char>(timebuf.at(1))) << 48;
                    logmsecs += quint64(static_cast<unsigned char>(timebuf.at(2))) << 40;
                    logmsecs += quint64(static_cast<unsigned char>(timebuf.at(3))) << 32;
                    logmsecs += quint64(static_cast<unsigned char>(timebuf.at(4))) << 24;
                    logmsecs += quint64(static_cast<unsigned char>(timebuf.at(5))) << 16;
                    logmsecs += quint64(static_cast<unsigned char>(timebuf.at(6))) << 8;
                    logmsecs += quint64(static_cast<unsigned char>(timebuf.at(7))) << 0;

                    timebuf.clear();

                    if (firsttime)
                    {
                        firsttime = false;
                        lastLogTime = logmsecs;
                        lastPcTime = QDateTime::currentMSecsSinceEpoch();
                    }
                    else
                    {
                        //Difference in time between the last time we read a timestamp, and this time
                        qint64 pcdiff = QDateTime::currentMSecsSinceEpoch() - lastPcTime;
                        lastPcTime = QDateTime::currentMSecsSinceEpoch();

                        //Difference in time between the last timestamp we fired off, and this one
                        qint64 logdiff = logmsecs - lastLogTime;
                        lastLogTime = logmsecs;
                        logdiff /= 1000;

                        if (logdiff < pcdiff)
                        {
                            //The next mavlink packet was fired faster than our loop is running, send it immediatly
                            //Fire immediatly
                            delay = 0;
                        }
                        else
                        {
                            //The next mavlink packet was sent logdiff-pcdiff millseconds after the current time
                            delay = logdiff-pcdiff;

                        }
                    }

                }
            }
            else if (decodeState == 1)
            {
                nexttime = true;
                //Good decode
                if (message.sysid == QGC::MavlinkID())
                {
                    //GCS packet, ignore it
                }
                else
                {
                    UASInterface* uas = UASManager::instance()->getUASForId(message.sysid);
                    if (!uas && message.msgid == MAVLINK_MSG_ID_HEARTBEAT)
                    {
                        mavlink_heartbeat_t heartbeat;
                        // Reset version field to 0
                        heartbeat.mavlink_version = 0;
                        mavlink_msg_heartbeat_decode(&message, &heartbeat);


                        // Create a new UAS object
                        if (heartbeat.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA)
                        {
                            ArduPilotMegaMAV* mav = new ArduPilotMegaMAV(0, message.sysid);
                            mav->setSystemType((int)heartbeat.type);
                            uas = mav;
                            replayUas = mav;
                            replaySystemId = message.sysid;
                            // Make UAS aware that this link can be used to communicate with the actual robot
                            uas->addLink(this);
                            UASObject *obj = new UASObject();
                            LinkManager::instance()->addSimObject(message.sysid,obj);

                            // Now add UAS to "official" list, which makes the whole application aware of it
                            UASManager::instance()->addUAS(uas);

                        }
                    }
                    else if (uas)
                    {
                        if (delay > 0 && delay < 10000)
                        {
                            //Split the delay into 100msec chunks, to allow for canceling.
                            int realdelay = delay / ((double)privSpeedVar / 100.0);
                            int repeat = realdelay / 100;
                            for (int i=0;i<repeat;i++)
                            {
                                msleep(100);
                                if (!m_threadRun)
                                {
                                    // Stop promptly. Common cleanup below is
                                    // queued back to the UI thread.
                                    break;
                                }
                            }
                            if (!m_threadRun || isInterruptionRequested()) {
                                break;
                            }
                            msleep(realdelay - repeat * 100);
                            //msleep(delay / ((double)privSpeedVar / 100.0));
                        }
                        else
                        {
                            msleep(1);
                        }
                        uas->receiveMessage(this,message);
                        if (UASObject *object = LinkManager::instance()->getUasObject(message.sysid)) {
                            object->messageReceived(this,message);
                        }
                        m_mavlinkDecoder->receiveMessage(this,message);
                        emit inspectorMessage(this, message);
                    }
                    else
                    {
                        //no UAS, and not a heartbeat
                    }
                }

            }
        }
        while (m_pause && m_threadRun && !isInterruptionRequested())
        {
            msleep(100);
        }
    }
    if (m_threadRun)
    {
        m_toBeDeleted = true;
    }
    LinkManager *lm = LinkManager::instance();
    QMetaObject::invokeMethod(lm, [lm, replayUas, replaySystemId]() {
        UASManager *uasManager = UASManager::instance();
        if (lm->isShuttingDown() || uasManager->isShuttingDown()) {
            return;
        }
        if (replaySystemId >= 0) {
            lm->removeSimObject(static_cast<uint8_t>(replaySystemId));
        }
        // Treat the captured pointer only as an identity token until the UI
        // thread confirms that UASManager still owns the replay vehicle.
        if (replayUas && uasManager->getUASList().contains(replayUas)) {
            uasManager->removeUAS(replayUas);
        }
        MainWindow::instance()->toolBar().overrideDisableConnectWidget(false);
        MainWindow::instance()->toolBar().disableConnectWidget(false);
    }, Qt::QueuedConnection);
    emit disconnected(this);
    emit disconnected();
    emit connected(false);
}

void TLogReplayLink::setLog(QString logfile)
{
    m_logFile = logfile;
}
void TLogReplayLink::stop()
{
    m_toBeDeleted = false;
    m_threadRun = false;
}

bool TLogReplayLink::toBeDeleted()
{
    return m_toBeDeleted.load();
}
