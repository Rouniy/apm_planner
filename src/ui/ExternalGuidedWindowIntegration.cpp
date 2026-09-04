#include "ExternalGuidedWindow.h"

#include "comm/LinkManager.h"
#include "comm/VehicleTargetManager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>

ExternalGuidedWindow::ExternalGuidedWindow(QWidget *owner)
    : ExternalGuidedWindow(DefaultDependencies(), owner)
{
}

ExternalGuidedWindow *ExternalGuidedWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new ExternalGuidedWindow(resolvedOwner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

ExternalGuidedWindow::Dependencies
ExternalGuidedWindow::DefaultDependencies()
{
    Dependencies dependencies;
    LinkManager *manager = LinkManager::instance();
    dependencies.targetManager = manager
        ? manager->vehicleTargetManager() : nullptr;
    dependencies.guidedService = manager
        ? manager->guidedTargetService() : nullptr;
    dependencies.readFile = [](const QString &path) {
        return ExternalGuidedFile::read(path);
    };
    dependencies.chooseFile = [](QWidget *parent) {
        return QFileDialog::getOpenFileName(
            parent,
            ExternalGuidedWindow::tr(
                "Select External Guided target file"),
            QString(),
            ExternalGuidedWindow::tr(
                "GUIDED target (*.txt *.csv);;All files (*)"));
    };
    dependencies.confirmStart = [](
        QWidget *parent, const QString &title, const QString &message) {
        QMessageBox dialog(QMessageBox::Warning, title, message,
                           QMessageBox::NoButton, parent);
        dialog.setTextFormat(Qt::PlainText);
        QPushButton *start = dialog.addButton(
            ExternalGuidedWindow::tr("Start External Guided"),
            QMessageBox::AcceptRole);
        QPushButton *cancel = dialog.addButton(QMessageBox::Cancel);
        dialog.setDefaultButton(cancel);
        dialog.setEscapeButton(cancel);
        dialog.exec();
        return dialog.clickedButton() == start;
    };
    return dependencies;
}
