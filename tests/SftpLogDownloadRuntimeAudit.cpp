#include "SftpLogDownloadRuntimeAudit.h"
#include "ui/MainWindow.h"
#include "ui/SftpLogDownloadWindow.h"
#include "ui/configuration/ConfigDeveloperToolsView.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QThread>
#include <QDebug>

namespace {
template<class T> T *find(QObject *owner, const char *name) { return owner ? owner->findChild<T *>(QString::fromLatin1(name)) : nullptr; }
template<class T> T *visible(QObject *owner, const char *name) {
    if (owner) for (auto *item : owner->findChildren<T *>(QString::fromLatin1(name))) if (item->isVisible()) return item;
    return nullptr;
}
bool wait(const std::function<bool()> &ready, int timeout = 8000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeout) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1); }
    return ready();
}
bool write(const QString &path, const QByteArray &bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size(); }
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
}

int RunSftpLogDownloadRuntimeAudit()
{
    int failures = 0;
    const auto check = [&](bool good, const char *why) { if (!good) { ++failures; qCritical() << "SFTP runtime:" << why; } };
    auto *main = MainWindow::instance();
    auto *route = find<QAction>(main, "actionDeveloperTools");
    auto *shared = find<QAction>(main, "actionSftpLogDownload");
    check(route && shared, "production actions missing"); if (!route || !shared) return 1;
    route->trigger(); QApplication::processEvents();
    auto *page = main->findChild<ConfigDeveloperToolsView *>();
    auto *button = find<QPushButton>(page, "DownloadDataFlashSftpButton");
    check(page && page->ActionCount() == 32 && page->ImplementedActionCount() == 32
          && button && button->isEnabled(), "SFTP unavailable offline or Developer inventory changed");
    if (!button) return 1;
    button->click(); QApplication::processEvents();
    QPointer<SftpLogDownloadWindow> window(main->findChild<SftpLogDownloadWindow *>());
    check(window && window->isVisible() && window->isWindow() && window->windowModality() == Qt::NonModal,
          "action did not open real modeless window"); if (!window) return 1;
    shared->trigger(); check(main->findChildren<SftpLogDownloadWindow *>().size() == 1, "duplicate shared window");
    check(find<QLineEdit>(window, "SftpPassword")->echoMode() == QLineEdit::Password, "password is not masked");
    check(find<QTableWidget>(window, "RemoteLogGrid")->columnCount() == 4, "reference grid columns missing");
    const QString python = qEnvironmentVariable("APM_SFTP_AUDIT_PYTHON");
    const QString screenshot = qEnvironmentVariable("APM_SFTP_AUDIT_SCREENSHOT");
    if (!python.isEmpty()) {
        QTemporaryDir remote, destination;
        QByteArray bin(89, '\0'); bin[0] = char(0xa3); bin[1] = char(0x95); bin[2] = char(128);
        // A real data definition and record, not just FMT's self-definition
        // (the retained parser correctly refuses the latter as an empty log).
        bin[3] = char(130); bin[4] = char(4); bin.replace(5, 3, "TST"); bin[9] = 'B';
        bin.replace(25, 5, "Value"); bin += QByteArray::fromHex("a3958201");
        check(write(remote.filePath("fixture.bin"), bin), "server fixture write failed");
        check(write(destination.filePath("fixture.bin"), "keep this file"), "local collision fixture failed");
        QProcess server;
        server.start(python, {QStringLiteral(APM_SFTP_SERVER_SCRIPT), remote.path()});
        check(server.waitForStarted(5000) && server.waitForReadyRead(10000), "hermetic SSH server failed to start");
        const auto config = QJsonDocument::fromJson(server.readLine()).object();
        check(config["port"].toInt() > 0, "hermetic port missing");
        const auto fields = [&] {
            find<QLineEdit>(window, "SftpHost")->setText("127.0.0.1");
            find<QLineEdit>(window, "SftpPort")->setText(QString::number(config["port"].toInt()));
            find<QLineEdit>(window, "SftpUsername")->setText("apm-test");
            find<QLineEdit>(window, "SftpPassword")->setText("apm-test-password");
            find<QLineEdit>(window, "SftpRemoteDirectory")->setText("/");
        };
        if (config["port"].toInt() > 0) {
            fields(); find<QPushButton>(window, "SftpRefreshButton")->click();
            check(wait([&] { return visible<QMessageBox>(window, "SftpHostKeyConfirmation"); }), "unknown key challenge missing");
            auto *trust = visible<QMessageBox>(window, "SftpHostKeyConfirmation");
            check(trust && trust->defaultButton() && trust->defaultButton()->text().contains("Cancel", Qt::CaseInsensitive), "host trust is not default-Cancel");
            check(!server.readAllStandardOutput().contains("authentication"), "password sent before key approval");
            if (!screenshot.isEmpty() && trust) check(trust->grab().save(screenshot + ".host.png"), "host-key screenshot failed");
            if (trust) trust->reject();
            check(wait([&] { return !window->busy(); }), "host Cancel left operation busy");
            fields(); find<QPushButton>(window, "SftpRefreshButton")->click();
            check(wait([&] { return visible<QMessageBox>(window, "SftpHostKeyConfirmation"); }), "retry key challenge missing");
            trust = visible<QMessageBox>(window, "SftpHostKeyConfirmation");
            auto *accept = find<QPushButton>(trust, "SftpTrustHostKeyButton");
            if (accept) accept->click(); else check(false, "named trust action missing");
            auto *grid = find<QTableWidget>(window, "RemoteLogGrid");
            check(wait([&] { return !window->busy() && grid->rowCount() == 1; }), "approved connection did not list real BIN");
            check(wait([&] { return grid->item(0, 1)
                && grid->item(0, 1)->text() == "fixture.bin"
                && grid->rowHeight(0) > 0 && !grid->visualItemRect(grid->item(0, 1)).isEmpty(); }),
                "listed BIN row exists in model but is not displayed");
            check(find<QLineEdit>(window, "SftpPassword")->text().isEmpty(), "password retained in field");
            if (!window->busy() && grid->rowCount() == 1) {
                find<QPushButton>(window, "SftpDownloadAllButton")->click();
                auto *picker = visible<QFileDialog>(window, "SftpDownloadDirectoryDialog");
                check(picker, "download folder picker missing");
                if (picker) picker->reject(); QApplication::processEvents();
                check(read(destination.filePath("fixture.bin")) == "keep this file", "folder Cancel changed files");
                find<QPushButton>(window, "SftpDownloadAllButton")->click();
                picker = visible<QFileDialog>(window, "SftpDownloadDirectoryDialog");
                if (picker) { picker->setDirectory(destination.path()); picker->selectFile(destination.path()); QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection); }
                check(wait([&] { return !window->busy() && QFile::exists(destination.filePath("fixture-1.kml")); }), "real SFTP BIN/LOG/KML download failed");
                check(read(destination.filePath("fixture-1.bin")) == bin && !read(destination.filePath("fixture-1.log")).isEmpty(), "downloaded BIN/LOG bytes missing");
                check(read(destination.filePath("fixture.bin")) == "keep this file", "download overwrote local file");
                if (!screenshot.isEmpty()) check(window->grab().save(screenshot + ".window.png"), "window screenshot failed");
                find<QPushButton>(window, "SftpDeleteAllButton")->click();
                auto *confirm = visible<QDialog>(window, "SftpDeleteConfirmation");
                auto *plan = find<QPlainTextEdit>(confirm, "SftpDeletePlan");
                check(plan && plan->toPlainText().contains("/fixture.bin"), "delete consent lacks frozen remote path");
                if (!screenshot.isEmpty() && confirm) check(confirm->grab().save(screenshot + ".delete.png"), "delete screenshot failed");
                if (confirm) confirm->reject(); QApplication::processEvents();
                check(QFile::exists(remote.filePath("fixture.bin")), "delete Cancel removed remote BIN");
                find<QPushButton>(window, "SftpDeleteAllButton")->click();
                confirm = visible<QDialog>(window, "SftpDeleteConfirmation");
                auto *yes = find<QPushButton>(confirm, "SftpDeleteConfirmButton");
                if (yes) yes->click();
                check(wait([&] { return !window->busy() && grid->rowCount() == 0; }), "confirmed remote delete did not finish");
                check(!QFile::exists(remote.filePath("fixture.bin")), "remote file still exists after delete");
            }
        }
        if (window->busy()) { window->cancel(); wait([&] { return !window->busy(); }); }
        window->close(); wait([&] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); return !window; });
        server.terminate(); if (!server.waitForFinished(3000)) { server.kill(); server.waitForFinished(3000); }
        shared->trigger(); QApplication::processEvents(); window = main->findChild<SftpLogDownloadWindow *>();
    }
    check(window && window->isVisible(), "window failed to reopen");
    if (window) {
        QTcpServer silent;
        check(silent.listen(QHostAddress::LocalHost, 0), "silent cancellation fixture failed");
        find<QLineEdit>(window, "SftpHost")->setText("127.0.0.1");
        find<QLineEdit>(window, "SftpPort")->setText(QString::number(silent.serverPort()));
        find<QLineEdit>(window, "SftpUsername")->setText("apm-test");
        find<QLineEdit>(window, "SftpRemoteDirectory")->setText("/");
        find<QPushButton>(window, "SftpRefreshButton")->click();
        check(wait([&] { return silent.hasPendingConnections(); }), "production session did not start");
        main->close();
        auto *close = visible<QMessageBox>(window, "SftpCloseConfirmation");
        check(close && main->isVisible(), "main Close bypassed busy SFTP consent");
        if (close) close->reject(); QApplication::processEvents();
        check(main->isVisible() && window->isVisible(), "close Cancel closed application");
        window->cancel(); check(wait([&] { return !window->busy(); }), "silent handshake cancellation did not drain");
        window->close(); check(wait([&] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); return !window; }), "window did not close after cancellation");
        check(main->isVisible(), "standalone close reused cancelled main Close");
    }
    main->close();
    qInfo() << "SFTP runtime audit failures:" << failures;
    return failures ? 1 : 0;
}
