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
 *   @brief Definition of UDP connection (server) for unmanned vehicles
 *   @author Lorenz Meier <mavteam@student.ethz.ch>
 *
 */
#include <QtGlobal>

#include <QTimer>
#include <QList>
#include <QMutexLocker>
#include <QtNetwork/qhostaddress.h>
#include <iostream>
#include <QHostInfo>

#include <limits>

#include "logging.h"
#include "UDPLink.h"
#include "LinkManager.h"
#include "QGC.h"

namespace
{
QHostAddress resolvedIpv4Address(const QHostInfo &info)
{
    QHostAddress result;
    for (const QHostAddress &candidate : info.addresses()) {
        // Preserve the historical IPv4-only peer behavior.
        if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
            result = candidate;
        }
    }
    return result;
}
}


UDPLink::UDPLink(QHostAddress host, quint16 port, bool retryOnBindFailure) :
    socket(NULL),
    connectState(false),
    _shouldRestartConnection(false),
    _retryOnBindFailure(retryOnBindFailure),
    _running(false)
{
    this->host = host;
    this->port = port;
    // Set unique ID and add link to the list of links
    this->id = getNextLinkId();
    this->name = tr("UDP Link (port:%1)").arg(this->port);
    emit nameChanged(this->name);
    QLOG_INFO() << "UDP Created " << name;
}

UDPLink::~UDPLink()
{
    // Tell the thread to exit
    m_peerState.advanceRevision();
    _running = false;

    // Wait for it to exit
    wait();
}

/**
 * @brief Runs the thread
 *
 **/
void UDPLink::run()
{
    forever {
        if (isInterruptionRequested()) {
            _running = false;
            delete socket;
            socket = nullptr;
            connectState = false;
            emit disconnected();
            emit connected(false);
            emit disconnected(this);
            QLOG_INFO() << "UDPLink:" << "Terminating thread";
            break;
        }
        if (!isConnected() && !host.isNull() && port != 0 ) {
            if (!hardwareConnect() && !_retryOnBindFailure) {
                delete socket;
                socket = nullptr;
                _running = false;
                break;
            }
            msleep(50);
            continue;
        }

        if (_shouldRestartConnection) {
            _shouldRestartConnection = false;
            hardwareConnect();
            msleep(50);
            continue;
        }

        if (!socket) {
            msleep(50);
            continue;
        }

        bool loop = socket->hasPendingDatagrams();
        if(loop) {
            readBytes();
        }

        //-- Loop right away if busy
        if((_dequeBytes() || loop) && _running)
            continue;

        if (!_running) {
            delete socket;
            socket = nullptr;
            connectState = false;
            emit disconnected();
            emit connected(false);
            emit disconnected(this);
            QLOG_INFO() << "UDPLink:" << "Terminando a thread:";
            break;
        }

        //-- Settle down (it gets here if there is nothing to read or write)
        msleep(50);
    }
}

void UDPLink::setAddress(QHostAddress host)
{
    if (this->host == host) {
        return;
    }
    this->host = host;
    m_peerState.advanceRevision();
    emit linkChanged(this);
    _shouldRestartConnection = true;
}

void UDPLink::setPort(int port)
{
    if (this->port == port) {
        return;
    }
    this->port = port;
    m_peerState.advanceRevision();
    this->name = tr("UDP Link (port:%1)").arg(this->port);
    emit nameChanged(this->name);
    emit linkChanged(this);
    _shouldRestartConnection = true;
}

/**
 * @param host Hostname in standard formatting, e.g. localhost:14551 or 192.168.1.1:14551
 */
void UDPLink::addHost(const QString& host)
{
    QLOG_INFO() << "UDP:" << "ADDING HOST:" << host;
    QString hostName = host.trimmed();
    quint16 requestedPort = port;
    if (hostName.contains(QLatin1Char(':'))) {
        const QStringList parts = hostName.split(QLatin1Char(':'));
        bool portOk = false;
        const uint parsedPort = parts.last().toUInt(&portOk);
        if (!portOk || parsedPort == 0
                || parsedPort > std::numeric_limits<quint16>::max()) {
            return;
        }
        hostName = parts.first().trimmed();
        requestedPort = static_cast<quint16>(parsedPort);
        QLOG_DEBUG() << "HOST:" << hostName;
    }

    // QHostInfo::fromName may block; resolution deliberately stays outside
    // the peer-state lock.
    const QHostInfo info = QHostInfo::fromName(hostName);
    if (info.error() != QHostInfo::NoError) {
        return;
    }
    const QHostAddress address = resolvedIpv4Address(info);
    const UdpPeerUpdate update =
            m_peerState.upsertPeer(address, requestedPort);
    if (!update.accepted || !update.changed) {
        return;
    }
    QLOG_DEBUG() << "Address:" << address.toString();
    emit linkChanged(this);
    _shouldRestartConnection = true;
}

void UDPLink::removeHost(const QString& hostname)
{
    QString host = hostname;
    if (host.contains(":")) host = host.split(":").first();
    host = host.trimmed();
    // DNS resolution is intentionally outside the peer-state lock.
    const QHostInfo info = QHostInfo::fromName(host);
    if (info.error() != QHostInfo::NoError) {
        return;
    }
    const QHostAddress address = resolvedIpv4Address(info);
    const UdpPeerUpdate update = m_peerState.removePeer(address);
    if (!update.accepted || !update.changed) {
        return;
    }
    emit linkChanged(this);
    _shouldRestartConnection = true;
}

void UDPLink::writeBytes(const char* data, qint64 size)
{
    if (!data || size <= 0) {
        return;
    }
    m_peerState.enqueueLatest(QByteArray(data, size));
}

bool UDPLink::enqueueForPeerRevision(const QByteArray &bytes,
                                     quint64 expectedRevision)
{
    return m_peerState.enqueueForRevision(bytes, expectedRevision);
}

bool UDPLink::_dequeBytes()
{
    const std::optional<UdpPeerDatagram> datagram =
            m_peerState.takeNextCurrent();
    if (datagram) {
        _sendBytes(*datagram);
    }
    return m_peerState.hasQueuedDatagrams();
}

void UDPLink::_sendBytes(const UdpPeerDatagram &datagram)
{
    // The destinations are the immutable enqueue-time snapshot. A peer change
    // can only make the envelope stale; it can never redirect these bytes.
    for (int h = 0; h < datagram.peers.hosts.size(); h++)
    {
        const QHostAddress currentHost = datagram.peers.hosts.at(h);
        const quint16 currentPort = datagram.peers.ports.at(h);
//#define UDPLINK_DEBUG
#ifdef UDPLINK_DEBUG
        QString bytes;
        QString ascii;
        for (int i=0; i<datagram.bytes.size(); i++)
        {
            unsigned char v = datagram.bytes.at(i);
            bytes.append(QString().sprintf("%02x ", v));
            if (datagram.bytes.at(i) > 31 && datagram.bytes.at(i) < 127)
            {
                ascii.append(datagram.bytes.at(i));
            }
            else
            {
                ascii.append(219);
            }
        }
        QLOG_TRACE() << "Sent" << datagram.bytes.size() << "bytes to" << currentHost.toString() << ":" << currentPort << "data:";
        QLOG_TRACE() << bytes;
        QLOG_TRACE() << "ASCII:" << ascii;
#endif
        if (!socket) {
            return;
        }
        socket->writeDatagram(datagram.bytes, currentHost, currentPort);

        // Log the amount and time written out for future data rate calculations.
        QMutexLocker dataRateLocker(&dataRateMutex);
        logDataRateToBuffer(outDataWriteAmounts, outDataWriteTimes,
                            &outDataIndex, datagram.bytes.size(),
                            QDateTime::currentMSecsSinceEpoch());
    }
}

/**
 * @brief Read a number of bytes from the interface.
 *
 * @param data Pointer to the data byte array to write the bytes to
 * @param maxLength The maximum number of bytes to write
 **/
void UDPLink::readBytes()
{
    while (socket->hasPendingDatagrams())
    {
        QByteArray datagram;
        datagram.resize(socket->pendingDatagramSize());

        QHostAddress sender;
        quint16 senderPort;
        socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        // Learn/replace the sender before publishing bytes. Both the peer list
        // and the revision stamped onto this datagram come from one lock scope.
        const UdpPeerUpdate peer =
                m_peerState.upsertPeer(sender, senderPort);

        // Log this data reception for this timestep
        {
            QMutexLocker dataRateLocker(&dataRateMutex);
            logDataRateToBuffer(inDataWriteAmounts, inDataWriteTimes,
                                &inDataIndex, datagram.length(),
                                QDateTime::currentMSecsSinceEpoch());
        }

        if (!peer.accepted) {
            continue;
        }

        // The strict consumer uses the revision-bearing signal. Keep the
        // inherited signal for legacy non-protocol observers.
        emit datagramReceivedWithPeerRevision(datagram, peer.revision);
        emit bytesReceived(this, datagram);

#ifdef UDPLINK_DEBUG
        // Echo data for debugging purposes
        std::cerr << __FILE__ << __LINE__ << "Received datagram:" << std::endl;
//        int i;
//        for (i=0; i<s; i++)
//        {
//            unsigned int v=data[i];
//            fprintf(stderr,"%02x ", v);
//        }
//        std::cerr << std::endl;
#endif

        if(!_running)
            break;
    }
}


/**
 * @brief Get the number of bytes to read.
 *
 * @return The number of bytes to read
 **/
qint64 UDPLink::bytesAvailable()
{
    return socket->pendingDatagramSize();
}

/**
 * @brief Disconnect the connection.
 *
 * @return True if connection has been disconnected, false if connection couldn't be disconnected.
 **/
bool UDPLink::disconnect()
{
    QLOG_INFO() << "UDP disconnect";
    m_peerState.advanceRevision();
    _running = false;
    return true;
}

/**
 * @brief Connect the connection.
 *
 * @return True if connection has been established, false if connection couldn't be established.
 **/
bool UDPLink::connect()
{
    QLOG_INFO() << "UDPLink::UDP connect " << host << ":" << port;
    if (isRunning()) {
        return true;
    }
    if (!m_peerState.advanceRevision()) {
        return false;
    }
    start(NormalPriority);
    return true;
}

bool UDPLink::hardwareConnect(void)
{
    delete socket;
    QHostAddress host = QHostAddress::AnyIPv4;
    socket = new QUdpSocket();
    socket->setProxy(QNetworkProxy::NoProxy);
    connectState = socket->bind(host, port, QAbstractSocket::ReuseAddressHint);
    if (connectState) {
        emit connected();
        emit connected(true);
        emit connected(this);
    } else {
        emit disconnected();
        emit connected(false);
        emit communicationError("UDP Link Error", "Error binding UDP port");
    }
    _running = true;
    return connectState;
}

/**
 * @brief Check if connection is active.
 *
 * @return True if link is connected, false otherwise.
 **/
bool UDPLink::isConnected() const
{
    return connectState;
}

int UDPLink::getId() const
{
    return id;
}

QString UDPLink::getName() const
{
    return name;
}

QString UDPLink::getShortName() const
{
    return QString("UDP Link");
}

QString UDPLink::getDetail() const
{
    return QString::number(port);
}

UDPLink::PeerSnapshot UDPLink::peerSnapshot() const
{
    return m_peerState.snapshot();
}

QList<QHostAddress> UDPLink::getHosts() const
{
    return peerSnapshot().hosts;
}

QList<quint16> UDPLink::getPorts() const
{
    return peerSnapshot().ports;
}

void UDPLink::setName(QString name)
{
    this->name = name;
    emit nameChanged(this->name);
    emit linkChanged(this);
}


qint64 UDPLink::getConnectionSpeed() const
{
    return 54000000; // 54 Mbit
}

qint64 UDPLink::getCurrentInDataRate() const
{
    return 0;
}

qint64 UDPLink::getCurrentOutDataRate() const
{
    return 0;
}
