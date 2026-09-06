#include "MicrodroneDownlinkWindow.h"

#include "comm/LinkManager.h"
#include "comm/MavlinkComponentRegistry.h"
#include "comm/SerialLinkInterface.h"
#include "comm/VehicleTargetManager.h"
#include <QApplication>
#include <QFileInfo>
#include <QSerialPortInfo>

namespace {
QString portIdentity(const QString &port)
{
    const QSerialPortInfo info(port);
    const QString path = info.systemLocation().isEmpty() ? port : info.systemLocation();
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? path : canonical;
}
}

MicrodroneDownlinkWindow::MicrodroneDownlinkWindow(QWidget *owner)
    : MicrodroneDownlinkWindow(DefaultDependencies(), owner)
{
    connectApplicationSignals();
    m_service->synchronizeSource();
}

MicrodroneDownlinkWindow *MicrodroneDownlinkWindow::OpenWindow(QWidget *owner)
{
    auto *window = new MicrodroneDownlinkWindow(owner ? owner : QApplication::activeWindow());
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

MicrodroneDownlinkWindow::Dependencies MicrodroneDownlinkWindow::DefaultDependencies()
{
    Dependencies dependencies;
    dependencies.service.outputFactory = MicrodroneDownlinkService::ProductionOutputFactory();
    dependencies.enumeratePorts = [] {
        QStringList ports;
        for (const auto &port : QSerialPortInfo::availablePorts()) ports.append(port.portName());
        ports.removeDuplicates();
        ports.sort();
        return ports;
    };
    dependencies.service.validatePort = [](const QString &port) {
        LinkManager *manager = LinkManager::instance();
        const QString wanted = portIdentity(port);
        for (int id : manager->getLinks()) {
            if (!manager->getLinkConnected(id)) continue;
            auto *serial = qobject_cast<SerialLinkInterface *>(manager->getLink(id));
            if (serial && portIdentity(serial->getPortName()) == wanted)
                return tr("Port %1 is already in use by an active vehicle link.").arg(port);
        }
        // Other secondary tools are protected by QSerialPort's exclusive open.
        return QString();
    };
    dependencies.service.resolveSource = [] {
        MicrodroneSource result;
        LinkManager *manager = LinkManager::instance();
        auto *targets = manager->vehicleTargetManager();
        auto *registry = manager->componentRegistry();
        if (!targets || !registry || !targets->isTargetGenerationSettled()) return result;
        const auto lease = targets->acquireTarget();
        if (!lease.isValid() || !manager->getLinkConnected(lease.endpoint.linkId)) return result;
        const quint64 epoch = manager->currentPhysicalLinkSession(lease.endpoint.linkId);
        for (const auto &instance : registry->components()) {
            if (instance.endpoint == lease.endpoint && instance.linkSessionEpoch == epoch
                && registry->validateLease(instance)) {
                result.selection = lease;
                result.instance = instance;
                result.linkName = manager->getLinkShortName(lease.endpoint.linkId);
                break;
            }
        }
        return result;
    };
    return dependencies;
}

void MicrodroneDownlinkWindow::connectApplicationSignals()
{
    LinkManager *manager = LinkManager::instance();
    auto *service = m_service;
    connect(manager, &LinkManager::mavlinkMessageObserved,
            service, [service](int linkId, quint64 epoch, const mavlink_message_t &message) {
        // Source transitions are delivered independently below. Do not copy
        // and scan the component registry for unrelated high-rate traffic.
        const auto source = service->source();
        const auto endpoint = source.selection.endpoint;
        if (source.isValid() && (endpoint.linkId != linkId
            || source.instance.linkSessionEpoch != epoch
            || endpoint.systemId != message.sysid || endpoint.componentId != message.compid)) return;
        service->observeMessage(linkId, epoch, message);
    });
    connect(manager, &LinkManager::physicalLinkSessionEnded,
            service, &MicrodroneDownlinkService::forgetLink);
    connect(manager, QOverload<int>::of(&LinkManager::linkChanged), service,
            [service] { service->synchronizeSource(); });
    connect(manager, &LinkManager::linkRemoved, service,
            [service] { service->synchronizeSource(); });
    auto *targets = manager->vehicleTargetManager();
    connect(targets, &VehicleTargetManager::targetGenerationChanged,
            service, &MicrodroneDownlinkService::synchronizeSource);
    connect(targets, &VehicleTargetManager::currentTargetChanged,
            service, &MicrodroneDownlinkService::synchronizeSource);
    auto *registry = manager->componentRegistry();
    connect(registry, &MavlinkComponentRegistry::componentRetired, service,
            [service](const MavlinkComponentInstanceLease &retired) {
        if (service->source().instance == retired)
            service->forgetLink(retired.endpoint.linkId, retired.linkSessionEpoch);
    });
    connect(registry, &MavlinkComponentRegistry::componentsChanged,
            service, &MicrodroneDownlinkService::synchronizeSource);
    connect(qApp, &QCoreApplication::aboutToQuit, service, &MicrodroneDownlinkService::stop);
}
