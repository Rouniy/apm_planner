#include "FollowMeWindow.h"

#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "comm/SerialLinkInterface.h"
#include "comm/VehicleTargetManager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
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
            return FollowMeWindow::tr(
                "Port %1 is already in use by an active vehicle link.")
                .arg(selection);
        }
    }
    return QString();
}
}

FollowMeWindow::FollowMeWindow(QWidget *owner)
    : FollowMeWindow(DefaultDependencies(), owner)
{
}

FollowMeWindow *FollowMeWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new FollowMeWindow(resolvedOwner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

FollowMeWindow::Dependencies FollowMeWindow::DefaultDependencies()
{
    Dependencies dependencies;
    LinkManager *manager = LinkManager::instance();
    dependencies.targetManager = manager
        ? manager->vehicleTargetManager() : nullptr;
    dependencies.guidedService = manager
        ? manager->guidedTargetService() : nullptr;
    dependencies.gpsInputFactory = [](QObject *parent) {
        return new FollowMeGpsInput(parent);
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
    dependencies.confirmStart = [](
        QWidget *parent, const QString &title, const QString &message) {
        QMessageBox dialog(QMessageBox::Warning, title, message,
                           QMessageBox::NoButton, parent);
        dialog.setTextFormat(Qt::PlainText);
        QPushButton *start = dialog.addButton(
            FollowMeWindow::tr("Start Follow Me"),
            QMessageBox::AcceptRole);
        QPushButton *cancel = dialog.addButton(QMessageBox::Cancel);
        dialog.setDefaultButton(cancel);
        dialog.setEscapeButton(cancel);
        dialog.exec();
        return dialog.clickedButton() == start;
    };
    return dependencies;
}
