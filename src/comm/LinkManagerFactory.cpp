#include "LinkManagerFactory.h"
#include "serialconnection.h"
#include "UDPLink.h"
#include "UDPClientLink.h"
#include "TCPLink.h"

#include <QPointer>


void LinkManagerFactory::connectLinkSignals(LinkInterface *link, LinkManager *lmgr)
{
    if (auto *udp = qobject_cast<UDPLink *>(link)) {
        const QPointer<UDPLink> guardedLink(udp);
        connect(udp, &UDPLink::datagramReceivedWithPeerRevision, lmgr,
                [lmgr, guardedLink](const QByteArray &bytes, quint64 revision) {
            if (guardedLink) {
                lmgr->receiveUdpDatagram(guardedLink, bytes, revision);
            }
        }, Qt::QueuedConnection);
    } else {
        const QPointer<LinkInterface> guardedLink(link);
        connect(link, &LinkInterface::bytesReceived, lmgr,
                [lmgr, guardedLink](LinkInterface *, const QByteArray &bytes) {
            if (guardedLink && lmgr->getLink(guardedLink->getId()) == guardedLink) {
                lmgr->getProtocol()->receiveBytes(guardedLink, bytes);
            }
        }, Qt::QueuedConnection);
    }
    // Queued lifecycle signals may outlive removal and deletion of the worker.
    // Never dereference the raw pointer copied into an old signal argument.
    const QPointer<LinkInterface> guardedLink(link);
    connect(link, QOverload<LinkInterface *>::of(&LinkInterface::connected),
            lmgr, [lmgr, guardedLink](LinkInterface *) {
        if (guardedLink && lmgr->getLink(guardedLink->getId()) == guardedLink) {
            lmgr->linkConnected(guardedLink);
        }
    });
    connect(link, QOverload<LinkInterface *>::of(&LinkInterface::disconnected),
            lmgr, [lmgr, guardedLink](LinkInterface *) {
        if (guardedLink && lmgr->getLink(guardedLink->getId()) == guardedLink) {
            lmgr->linkDisonnected(guardedLink);
        }
    });
    connect(link, &LinkInterface::error,
            lmgr, [lmgr, guardedLink](LinkInterface *, const QString &error) {
        if (guardedLink && lmgr->getLink(guardedLink->getId()) == guardedLink) {
            lmgr->linkErrorRec(guardedLink, error);
        }
    });
    connect(link, &LinkInterface::linkChanged,
            lmgr, [lmgr, guardedLink](LinkInterface *) {
        if (guardedLink && lmgr->getLink(guardedLink->getId()) == guardedLink) {
            lmgr->linkUpdated(guardedLink);
        }
    });
}

int LinkManagerFactory::addSerialConnection()
{
    LinkManager *lmgr = LinkManager::instance();
    if (lmgr->isShuttingDown()) {
        return -1;
    }
    SerialConnection *link = new SerialConnection();
    connectLinkSignals(link, lmgr);

    connect(link,SIGNAL(timeoutTriggered(LinkInterface*)),lmgr,SLOT(linkTimeoutTriggered(LinkInterface*)));

    const int id = link->getId();
    const QPointer<LinkInterface> guard(link);
    lmgr->addLink(link);
    return guard && lmgr->getLink(id) == guard.data() ? id : -1;
}

int LinkManagerFactory::addSerialConnection(QString port,int baud,
    const LinkManager::ConnectionProfile &profile)
{
    LinkManager *lmgr = LinkManager::instance();
    if (lmgr->isShuttingDown()) {
        return -1;
    }
    SerialConnection *link = new SerialConnection();
    connectLinkSignals(link, lmgr);

    connect(link,SIGNAL(timeoutTriggered(LinkInterface*)),lmgr,SLOT(linkTimeoutTriggered(LinkInterface*)));

    link->setPortName(port);
    link->setBaudRate(baud);

    const int id = link->getId();
    const QPointer<LinkInterface> guard(link);
    lmgr->addLink(link, profile);
    return guard && lmgr->getLink(id) == guard.data() ? id : -1;

}
int LinkManagerFactory::addUdpConnection(QHostAddress addr, int port,
                                         bool retryOnBindFailure,
                                         const LinkManager::ConnectionProfile &profile)
{
    LinkManager *lmgr = LinkManager::instance();
    if (lmgr->isShuttingDown()) {
        return -1;
    }
    UDPLink* link = new UDPLink(addr, port, retryOnBindFailure);
    connectLinkSignals(link, lmgr);
    if (!retryOnBindFailure) {
        const int linkId = link->getId();
        const QPointer<UDPLink> guardedLink(link);
        connect(link, &LinkInterface::communicationError, lmgr,
                [lmgr, guardedLink, linkId](const QString &, const QString &) {
            if (guardedLink && lmgr->getLink(linkId) == guardedLink.data()) {
                lmgr->removeLink(linkId);
            }
        }, Qt::QueuedConnection);
    }

    const int id = link->getId();
    const QPointer<LinkInterface> guard(link);
    lmgr->addLink(link, profile);
    if (!guard || lmgr->getLink(id) != guard.data()) return -1;
    lmgr->connectLink(id);
    return guard && lmgr->getLink(id) == guard.data() ? id : -1;

}

int LinkManagerFactory::addUdpClientConnection(QHostAddress addr,int port,
    const LinkManager::ConnectionProfile &profile)
{
    LinkManager *lmgr = LinkManager::instance();
    if (lmgr->isShuttingDown()) {
        return -1;
    }
    UDPClientLink* link = new UDPClientLink(addr,port);
    connectLinkSignals(link, lmgr);

    const int id = link->getId();
    const QPointer<LinkInterface> guard(link);
    lmgr->addLink(link, profile);
    return guard && lmgr->getLink(id) == guard.data() ? id : -1;
}

int LinkManagerFactory::addTcpConnection(QHostAddress addr, QString hostName, int port,bool asServer,
    const LinkManager::ConnectionProfile &profile)
{
    LinkManager *lmgr = LinkManager::instance();
    if (lmgr->isShuttingDown()) {
        return -1;
    }

    TCPLink *link = new TCPLink(addr, hostName, port, asServer);

    connectLinkSignals(link, lmgr);

    const int id = link->getId();
    const QPointer<LinkInterface> guard(link);
    lmgr->addLink(link, profile);
    if (!guard || lmgr->getLink(id) != guard.data()) return -1;
    if (asServer)
    {
        lmgr->connectLink(id);
    }
    return guard && lmgr->getLink(id) == guard.data() ? id : -1;
}
