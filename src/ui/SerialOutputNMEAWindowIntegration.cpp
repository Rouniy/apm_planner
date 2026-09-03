#include "SerialOutputNMEAWindow.h"

#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "comm/SerialLinkInterface.h"
#include "comm/VehicleTargetManager.h"

#include <QApplication>
#include <QFileInfo>
#include <QSerialPortInfo>

namespace
{
QString serialPortIdentity(const QString &portName)
{
    const QSerialPortInfo info(portName);
    QString candidate = info.systemLocation();
    if (candidate.isEmpty()) {
        candidate = portName;
    }
    const QString canonical = QFileInfo(candidate).canonicalFilePath();
    return canonical.isEmpty() ? candidate : canonical;
}

QString serialPortConflict(const QString &selection)
{
    if (NmeaOutputService::IsTcpHostSelection(selection)
        || NmeaOutputService::IsUdpHostSelection(selection)) {
        return QString();
    }
    LinkManager *manager = LinkManager::instance();
    const QString wanted = serialPortIdentity(selection);
    for (int linkId : manager->getLinks()) {
        if (!manager->getLinkConnected(linkId)) {
            continue;
        }
        auto *serial = qobject_cast<SerialLinkInterface *>(
            manager->getLink(linkId));
        if (serial && serialPortIdentity(serial->getPortName()) == wanted) {
            return SerialOutputNMEAWindow::tr(
                "Port %1 is already in use by an active vehicle link.")
                .arg(selection);
        }
    }
    return QString();
}
}

SerialOutputNMEAWindow::SerialOutputNMEAWindow(QWidget *owner)
    : SerialOutputNMEAWindow(DefaultDependencies(), owner)
{
    connectApplicationSignals();
}

SerialOutputNMEAWindow *SerialOutputNMEAWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new SerialOutputNMEAWindow(resolvedOwner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

SerialOutputNMEAWindow::Dependencies
SerialOutputNMEAWindow::DefaultDependencies()
{
    Dependencies dependencies;
    dependencies.outputFactory = NmeaOutputService::ProductionOutputFactory();
    dependencies.udpPortGuard = [](quint16 port, QString *reason) {
        LinkManager *manager = LinkManager::instance();
        if (manager && manager->isUdpPortInUse(port)) {
            if (reason) {
                *reason = SerialOutputNMEAWindow::tr(
                    "UDP port %1 is used by an active vehicle link; "
                    "choose TCP Host or disconnect that link.")
                    .arg(port);
            }
            return false;
        }
        return true;
    };
    dependencies.viewModel.enumeratePorts = []() {
        QStringList ports;
        for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
            ports.append(info.portName());
        }
        ports.removeDuplicates();
        return ports;
    };
    dependencies.viewModel.resolveSource = []() {
        NmeaOutputSource source;
        LinkManager *manager = LinkManager::instance();
        VehicleTargetManager *targets = manager
            ? manager->vehicleTargetManager() : nullptr;
        const VehicleTargetLease lease = targets
            ? targets->acquireTarget() : VehicleTargetLease();
        if (!manager || !lease.isValid()
            || !manager->getLinkConnected(lease.endpoint.linkId)) {
            return source;
        }
        source.endpoint = lease.endpoint;
        source.linkName = lease.endpoint.linkName;
        if (source.linkName.isEmpty()) {
            source.linkName = manager->getLinkShortName(
                source.endpoint.linkId);
        }
        return source;
    };
    dependencies.viewModel.validateSelection = serialPortConflict;
    return dependencies;
}

void SerialOutputNMEAWindow::connectApplicationSignals()
{
    LinkManager *manager = LinkManager::instance();
    if (!manager) {
        return;
    }
    // LinkInterface is observed only synchronously to extract its stable id;
    // no raw link pointer is retained by the window or service.
    connect(manager, &LinkManager::messageReceived, m_service,
            [this](LinkInterface *link, mavlink_message_t message) {
        if (link) {
            m_service->observeMessage(link->getId(), message);
        }
    });
    connect(manager, &LinkManager::linkRemoved,
            m_service, &NmeaOutputService::forgetLink);
    connect(manager, QOverload<int>::of(&LinkManager::linkChanged), this,
            [this, manager](int linkId) {
        if (m_service->endpoint().linkId == linkId
            && !manager->getLinkConnected(linkId)) {
            m_service->forgetLink(linkId);
        }
    });
}
