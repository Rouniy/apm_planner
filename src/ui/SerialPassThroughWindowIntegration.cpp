#include "SerialPassThroughWindow.h"

#include "comm/LinkManager.h"
#include "comm/MAVLinkProtocol.h"
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
    if (MavlinkMirrorOutput::IsTcpHostSelection(selection)
        || MavlinkMirrorOutput::IsUdpHostSelection(selection)) {
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
            return SerialPassThroughWindow::tr(
                "Port %1 is already in use by an active vehicle link.")
                .arg(selection);
        }
    }
    return QString();
}
}

SerialPassThroughWindow::SerialPassThroughWindow(QWidget *owner)
    : SerialPassThroughWindow(DefaultDependencies(), owner)
{
    connectApplicationSignals();
}

SerialPassThroughWindow *SerialPassThroughWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new SerialPassThroughWindow(resolvedOwner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

SerialPassThroughWindow::Dependencies
SerialPassThroughWindow::DefaultDependencies()
{
    Dependencies dependencies;
    dependencies.rawWriter = [](int linkId, const QByteArray &bytes) {
        LinkManager *manager = LinkManager::instance();
        return manager && !manager->isShuttingDown()
            && manager->writeRawBytes(linkId, bytes);
    };
    dependencies.udpPortGuard = [](quint16 port, QString *reason) {
        LinkManager *manager = LinkManager::instance();
        if (manager && manager->isUdpPortInUse(port)) {
            if (reason) {
                *reason = SerialPassThroughWindow::tr(
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
        MavlinkMirrorSource source;
        LinkManager *manager = LinkManager::instance();
        VehicleTargetManager *targets = manager
            ? manager->vehicleTargetManager() : nullptr;
        const VehicleTargetLease lease = targets
            ? targets->acquireTarget() : VehicleTargetLease();
        if (!manager || !lease.isValid()
            || !manager->getLinkConnected(lease.endpoint.linkId)) {
            return source;
        }
        source.linkId = lease.endpoint.linkId;
        source.linkName = lease.endpoint.linkName;
        if (source.linkName.isEmpty()) {
            source.linkName = manager->getLinkShortName(source.linkId);
        }
        return source;
    };
    dependencies.viewModel.validateSelection = serialPortConflict;
    return dependencies;
}

void SerialPassThroughWindow::connectApplicationSignals()
{
    LinkManager *manager = LinkManager::instance();
    if (!manager) {
        return;
    }
    if (MAVLinkProtocol *protocol = manager->getProtocol()) {
        connect(protocol, &MAVLinkProtocol::frameReceived,
                m_service, &MavlinkMirrorService::observeFrame);
    }
    connect(manager, &LinkManager::linkRemoved,
            m_service, &MavlinkMirrorService::forgetLink);
    connect(manager, QOverload<int>::of(&LinkManager::linkChanged), this,
            [this, manager](int linkId) {
                if (m_service->linkId() == linkId
                    && !manager->getLinkConnected(linkId)) {
                    m_service->forgetLink(linkId);
                    m_viewModel->refreshStatus();
                }
            });
}
