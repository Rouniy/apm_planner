#include "MovingBaseWindow.h"

#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "comm/SerialLinkInterface.h"
#include "comm/VehicleTargetManager.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSerialPortInfo>
#include <QSettings>
#include <QStandardPaths>

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
    LinkManager *manager = LinkManager::instance();
    if (!manager) {
        return QString();
    }
    const QString wanted = serialPortIdentity(selection);
    for (int linkId : manager->getLinks()) {
        if (!manager->getLinkConnected(linkId)) {
            continue;
        }
        auto *serial = qobject_cast<SerialLinkInterface *>(
            manager->getLink(linkId));
        if (serial && serialPortIdentity(serial->getPortName()) == wanted) {
            return MovingBaseWindow::tr(
                "Port %1 is already in use by an active vehicle link.")
                .arg(selection);
        }
    }
    return QString();
}
}

MovingBaseWindow::MovingBaseWindow(QWidget *owner)
    : MovingBaseWindow(DefaultDependencies(), owner)
{
}

MovingBaseWindow *MovingBaseWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new MovingBaseWindow(resolvedOwner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

MovingBaseWindow::Dependencies MovingBaseWindow::DefaultDependencies()
{
    Dependencies dependencies;
    LinkManager *manager = LinkManager::instance();
    dependencies.targetManager = manager
        ? manager->vehicleTargetManager() : nullptr;
    dependencies.service = manager
        ? manager->movingBaseService() : nullptr;
    dependencies.transportFactory = [](
        const MovingBaseInputTransport::Settings &settings,
        QObject *parent) {
        return new MovingBaseInputTransport(settings, parent);
    };
    dependencies.enumeratePorts = []() {
        QStringList ports;
        for (const QSerialPortInfo &info
             : QSerialPortInfo::availablePorts()) {
            ports.append(info.portName());
        }
        ports.removeDuplicates();
        return ports;
    };
    dependencies.validateSerial = serialPortConflict;
    dependencies.validateUdpHostPort = [](quint16 port) {
        LinkManager *currentManager = LinkManager::instance();
        if (currentManager && currentManager->isUdpPortInUse(port)) {
            return MovingBaseWindow::tr(
                "UDP port %1 is used by an active vehicle link; "
                "choose another port or transport.")
                .arg(port);
        }
        return QString();
    };
    dependencies.readSetting = [](
        const QString &key, const QVariant &fallback) {
        QSettings settings;
        return settings.value(key, fallback);
    };
    dependencies.writeSettings = [](
        const QVariantMap &values, QString *error) {
        QSettings settings;
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            settings.setValue(it.key(), it.value());
        }
        settings.sync();
        if (settings.status() == QSettings::NoError) {
            return true;
        }
        if (error) {
            *error = MovingBaseWindow::tr(
                "the settings backend returned error %1")
                .arg(static_cast<int>(settings.status()));
        }
        return false;
    };
    const QString dataPath = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    dependencies.rawLogPath = QDir(dataPath).filePath(
        QStringLiteral("MovingBase.txt"));
    return dependencies;
}
