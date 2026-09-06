#ifndef LINKMANAGERFACTORY_H
#define LINKMANAGERFACTORY_H

#include "LinkManager.h"
#include <QObject>
#include <QHostAddress>

class LinkManagerFactory : public QObject
{
//    Q_OBJECT
public:
//    explicit LinkManagerFactory(QObject *parent = 0);
//    ~LinkManagerFactory();

    // Serial Links
    static int addSerialConnection(QString port,int baud,
        const LinkManager::ConnectionProfile &profile = {});
    static int addSerialConnection();

    // IP Links
    static int addUdpConnection(QHostAddress addr, int port,
                                bool retryOnBindFailure = true,
                                const LinkManager::ConnectionProfile &profile = {});
    static int addUdpClientConnection(QHostAddress addr,int port,
        const LinkManager::ConnectionProfile &profile = {});
    static int addTcpConnection(QHostAddress addr, QString hostName, int port, bool asServer,
        const LinkManager::ConnectionProfile &profile = {});

private:
#ifdef APM_SETUP_ROUTE_RUNTIME_AUDIT
    friend int RunSigningTransportRuntimeAudit();
    friend int RunDeveloperVehicleToolRuntimeAudit();
    friend int RunGuidedNavigationRuntimeAudit();
#endif
    static void connectLinkSignals(LinkInterface *link, LinkManager *lmgr);
};

#endif // LINKMANAGERFACTORY_H
