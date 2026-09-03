#include "DeviceOperationsWindow.h"

#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "comm/VehicleTargetManager.h"
#include "configuration.h"

#include <QApplication>
#include <QPointer>

DeviceOperationsWindow::DeviceOperationsWindow(QWidget *owner)
    : DeviceOperationsWindow(DefaultDependencies(), owner)
{
    connectApplicationSignals();
}

DeviceOperationsWindow *DeviceOperationsWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new DeviceOperationsWindow(resolvedOwner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

DeviceOperationsWindow::Dependencies
DeviceOperationsWindow::DefaultDependencies()
{
    Dependencies dependencies;
    LinkManager *manager = LinkManager::instance();
    dependencies.targetManager = manager
        ? manager->vehicleTargetManager() : nullptr;
    dependencies.transmitter = manager
        ? manager->exactLinkTransmitter() : nullptr;
    dependencies.localSystemId = QGC::MavlinkID();
    dependencies.localComponentId = QGC::ComponentID();
    return dependencies;
}

void DeviceOperationsWindow::connectApplicationSignals()
{
    LinkManager *manager = LinkManager::instance();
    if (!manager || !m_service) {
        return;
    }
    connect(manager, &LinkManager::messageReceived, m_service,
            [service = QPointer<DeviceOperationService>(m_service)](
                LinkInterface *link, const mavlink_message_t &message) {
        if (service && link) {
            service->observeMessage(link->getId(), message);
        }
    });
    connect(manager, &LinkManager::linkRemoved,
            m_service, &DeviceOperationService::forgetLink);
    connect(manager, QOverload<int>::of(&LinkManager::linkChanged), m_service,
            [service = QPointer<DeviceOperationService>(m_service), manager](
                int linkId) {
        if (service && !manager->getLinkConnected(linkId)) {
            service->forgetLink(linkId);
        }
    });
}
