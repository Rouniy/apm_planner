#include "MainWindow.h"
#include "SftpLogDownloadWindow.h"
#include <QTimer>

void MainWindow::showSftpLogDownload()
{
    if (aboutToCloseFlag) return;
    if (m_sftpLogDownloadWindow && m_sftpLogDownloadWindow->isClosing()) {
        // A pending cancel-and-close still owns its worker. Do not create a
        // second connection or re-show a window awaiting DeferredDelete.
        return;
    }
    if (!m_sftpLogDownloadWindow) {
        auto *window = new SftpLogDownloadWindow(this);
        m_sftpLogDownloadWindow = window;
        connect(window, &SftpLogDownloadWindow::closeResolved, this, [this](bool accepted) {
            if (!m_waitingForSftpLogDownloadClose) return;
            m_waitingForSftpLogDownloadClose = false;
            if (accepted) QTimer::singleShot(0, this, [this] { if (!aboutToCloseFlag) close(); });
        });
    }
    m_sftpLogDownloadWindow->show();
    m_sftpLogDownloadWindow->raise();
    m_sftpLogDownloadWindow->activateWindow();
}

bool MainWindow::requestSftpLogDownloadClose()
{
    const QPointer<SftpLogDownloadWindow> window(m_sftpLogDownloadWindow);
    if (!window) return true;
    m_waitingForSftpLogDownloadClose = true;
    if (!window->close()) return false;
    m_waitingForSftpLogDownloadClose = false;
    return true;
}

void MainWindow::closeSftpLogDownload()
{
    m_waitingForSftpLogDownloadClose = false;
    if (m_sftpLogDownloadWindow) {
        m_sftpLogDownloadWindow->shutdown();
        delete m_sftpLogDownloadWindow.data();
        m_sftpLogDownloadWindow.clear();
    }
}
