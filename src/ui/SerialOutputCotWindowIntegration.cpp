#include "SerialOutputCotWindow.h"

#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "comm/SerialLinkInterface.h"
#include "comm/VehicleTargetManager.h"

#include <QApplication>
#include <QFileInfo>
#include <QSerialPortInfo>
#include <QSettings>

namespace {

const char SettingsGroup[] = "MissionPlanner/Tools/CotOutput";

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
QString validateTransportSelection(
    const CotOutputTransport::Settings &settings)
{
    LinkManager *manager = LinkManager::instance();
    if (!manager) {
        return QString();
    }
    if (settings.mode == CotOutputTransport::Mode::UdpHost
        && manager->isUdpPortInUse(settings.port)) {
        return SerialOutputCotWindow::tr(
            "UDP port %1 is used by an active vehicle link; choose another port or transport.")
            .arg(settings.port);
    }
    if (settings.mode != CotOutputTransport::Mode::Serial) {
        return QString();
    }

    const QString wanted = serialPortIdentity(settings.serialPort);
    for (int linkId : manager->getLinks()) {
        if (!manager->getLinkConnected(linkId)) {
            continue;
        }
        auto *serial = qobject_cast<SerialLinkInterface *>(
            manager->getLink(linkId));
        if (serial && serialPortIdentity(serial->getPortName()) == wanted) {
            return SerialOutputCotWindow::tr(
                "Port %1 is already in use by an active vehicle link.")
                .arg(settings.serialPort);
        }
    }
    return QString();
}

CotOutputSource currentSource()
{
    CotOutputSource source;
    LinkManager *manager = LinkManager::instance();
    VehicleTargetManager *targets = manager
        ? manager->vehicleTargetManager() : nullptr;
    const VehicleTargetLease lease = targets
        ? targets->acquireTarget() : VehicleTargetLease();
    if (!manager || !targets || !lease.isValid()
        || !manager->getLinkConnected(lease.endpoint.linkId)) {
        return source;
    }

    source.linkId = lease.endpoint.linkId;
    source.linkName = lease.endpoint.linkName;
    if (source.linkName.isEmpty()) {
        source.linkName = manager->getLinkShortName(source.linkId);
    }
    for (const VehicleEndpoint &endpoint : targets->endpoints()) {
        if (endpoint.linkId == source.linkId) {
            source.endpoints.append(endpoint);
        }
    }
    if (!source.endpoints.contains(lease.endpoint)) {
        source.endpoints.append(lease.endpoint);
    }
    return source;
}

} // namespace

SerialOutputCotWindow::SerialOutputCotWindow(QWidget *owner)
    : SerialOutputCotWindow(DefaultDependencies(), owner)
{
    connectApplicationSignals();
}

SerialOutputCotWindow *SerialOutputCotWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new SerialOutputCotWindow(resolvedOwner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

SerialOutputCotWindow::Dependencies
SerialOutputCotWindow::DefaultDependencies()
{
    Dependencies dependencies;
    dependencies.transportFactory =
        CotOutputService::ProductionTransportFactory();
    dependencies.sender = CotOutputService::ProductionSender();
    dependencies.viewModel.enumeratePorts = []() {
        QStringList ports;
        for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
            ports.append(info.portName());
        }
        ports.removeDuplicates();
        return ports;
    };
    dependencies.viewModel.resolveSource = currentSource;
    dependencies.viewModel.validateSelection = validateTransportSelection;
    dependencies.viewModel.loadSettings = []() {
        QVariantMap values;
        QSettings settings;
        settings.beginGroup(QString::fromLatin1(SettingsGroup));
        for (const QString &key : settings.childKeys()) {
            values.insert(key, settings.value(key));
        }
        settings.endGroup();
        return values;
    };
    dependencies.viewModel.saveSettings = [](
        const QVariantMap &values, QString *error) {
        QSettings settings;
        settings.beginGroup(QString::fromLatin1(SettingsGroup));
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            settings.setValue(it.key(), it.value());
        }
        settings.endGroup();
        settings.sync();
        if (settings.status() == QSettings::NoError) {
            if (error) {
                error->clear();
            }
            return true;
        }
        if (error) {
            *error = SerialOutputCotWindow::tr(
                "the application settings store rejected the write");
        }
        return false;
    };
    return dependencies;
}

void SerialOutputCotWindow::connectApplicationSignals()
{
    LinkManager *manager = LinkManager::instance();
    if (!manager) {
        return;
    }
    VehicleTargetManager *targets = manager->vehicleTargetManager();
    if (targets) {
        m_viewModel->updateAvailableEndpoints(targets->endpoints());
        connect(targets, &VehicleTargetManager::endpointsChanged,
                this, [this, targets]() {
            m_viewModel->updateAvailableEndpoints(targets->endpoints());
        });
    }

    // LinkInterface is observed only synchronously to extract its stable id;
    // neither the window nor the service retains a raw link pointer.
    connect(manager, &LinkManager::messageReceived, m_service,
            [this](LinkInterface *link, mavlink_message_t message) {
        if (link) {
            m_service->observeMessage(link->getId(), message);
        }
    });
    connect(manager, &LinkManager::linkRemoved,
            m_service, &CotOutputService::forgetLink);
    connect(manager, QOverload<int>::of(&LinkManager::linkChanged), this,
            [this, manager](int linkId) {
        if (m_service->linkId() == linkId
            && !manager->getLinkConnected(linkId)) {
            m_service->forgetLink(linkId);
        }
    });
}
