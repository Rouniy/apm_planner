#include "TlogMatlabRuntimeAudit.h"
#include "ui/MainWindow.h"
#include "ui/MavlinkLogWindow.h"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <QDebug>

namespace {
template<class T> T *find(QObject *owner, const char *name) { return owner ? owner->findChild<T *>(QString::fromLatin1(name)) : nullptr; }
template<class T> T *visible(QObject *owner, const char *name) {
    if (owner) for (auto *item : owner->findChildren<T *>(QString::fromLatin1(name))) if (item->isVisible()) return item;
    return nullptr;
}
bool wait(const std::function<bool()> &ready, int milliseconds = 10000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < milliseconds) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1); }
    return ready();
}
bool fixture(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
    for (int index = 0; index < 20000; ++index) {
        mavlink_message_t message{};
        mavlink_msg_attitude_pack(1, 1, &message, quint32(index * 20), 0.125f, -0.25f, 0.5f, 1.0f, 2.0f, 3.0f);
        uint8_t frame[MAVLINK_MAX_PACKET_LEN]{};
        const int count = mavlink_msg_to_send_buffer(frame, &message);
        uchar stamp[8]; qToBigEndian<quint64>(1757155200123456ULL + quint64(index) * 20000, stamp);
        if (file.write(reinterpret_cast<const char *>(stamp), 8) != 8
            || file.write(reinterpret_cast<const char *>(frame), count) != count) return false;
    }
    return file.flush();
}
bool choose(QFileDialog *dialog, const QString &path) {
    if (!dialog) return false;
    dialog->selectFile(path); QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    return true;
}
}

int RunTlogMatlabRuntimeAudit() {
    int failures = 0;
    const auto check = [&](bool good, const char *why) { if (!good) { ++failures; qCritical() << "MATLAB runtime:" << why; } };
    auto *main = MainWindow::instance();
    auto *route = find<QAction>(main, "actionTlogConvertExtract");
    check(route && route->isEnabled(), "production Tools route unavailable offline");
    if (!route) return 1;
    route->trigger(); QApplication::processEvents();
    QPointer<MavlinkLogWindow> window(main->findChild<MavlinkLogWindow *>());
    check(window && window->isVisible() && window->isWindow() && window->windowModality() == Qt::NonModal,
        "Tools route did not open the real modeless conversion window");
    if (!window) return 1;
    QTemporaryDir directory;
    const QString input = directory.filePath("flight.tlog");
    const QString output = directory.filePath("flight.tlog.mat");
    const QString cancelledOutput = directory.filePath("cancelled.mat");
    check(fixture(input), "TLOG fixture could not be written");
    auto *matlab = find<QPushButton>(window, "convertMatlabButton");
    check(matlab && !matlab->isEnabled(), "MATLAB missing or enabled without input");
    auto *pick = find<QPushButton>(window, "pickTlogButton");
    if (!pick || !matlab) return 1;
    pick->click();
    check(wait([&] { return visible<QFileDialog>(window, "MavlinkLogInputDialog"); }), "input picker missing");
    check(choose(visible<QFileDialog>(window, "MavlinkLogInputDialog"), input), "input picker unusable");
    check(wait([&] { return window->tlogPath() == input && matlab->isEnabled(); }), "selected TLOG did not enable MATLAB");
    const QString screenshot = qEnvironmentVariable("APM_MATLAB_AUDIT_SCREENSHOT");
    matlab->click();
    check(wait([&] { return visible<QMessageBox>(window, "MavlinkLogSensitiveExportConfirmation"); }), "sensitive export consent missing");
    auto *consent = visible<QMessageBox>(window, "MavlinkLogSensitiveExportConfirmation");
    check(consent && consent->defaultButton() == consent->button(QMessageBox::Cancel), "sensitive export not default-Cancel");
    if (!screenshot.isEmpty() && consent) check(consent->grab().save(screenshot + ".consent.png"), "consent screenshot failed");
    if (consent) consent->reject();
    check(wait([&] { return !window->isBusy(); }) && !QFile::exists(output), "consent Cancel created output or stayed busy");

    const auto start = [&](const QString &target) {
        matlab->click();
        if (!wait([&] { return visible<QMessageBox>(window, "MavlinkLogSensitiveExportConfirmation"); })) return false;
        auto *confirm = visible<QMessageBox>(window, "MavlinkLogSensitiveExportConfirmation");
        auto *accept = find<QPushButton>(confirm, "MavlinkLogExportConfirmButton");
        if (!accept) return false;
        accept->click();
        if (!wait([&] { return visible<QFileDialog>(window, "MavlinkLogOutputDialog"); })) return false;
        return choose(visible<QFileDialog>(window, "MavlinkLogOutputDialog"), target);
    };
    check(start(output), "MATLAB export controls did not admit the job");
    check(wait([&] { return !window->isBusy(); }, 20000) && QFile::exists(output), "real MATLAB export failed");
    QFile mat(output); check(mat.open(QIODevice::ReadOnly), "MAT output unavailable");
    const QByteArray header = mat.read(128);
    check(header.size() == 128 && header.startsWith("MATLAB 5.0 MAT-file") && header.mid(126) == "IM"
          && mat.size() > 128, "MATLAB output is not a populated Level5 file");
    mat.close();
    check(window->statusText().contains("Wrote", Qt::CaseInsensitive), "MATLAB completion status missing");
    if (!screenshot.isEmpty()) check(window->grab().save(screenshot + ".window.png"), "window screenshot failed");

    check(start(cancelledOutput), "second MATLAB operation was not admitted");
    auto *cancel = find<QPushButton>(window, "MavlinkLogExportCancelButton");
    check(cancel && cancel->isEnabled(), "visible cancellation unavailable during export");
    if (cancel) cancel->click();
    check(wait([&] { return !window->isBusy(); }, 20000) && !QFile::exists(cancelledOutput), "cancelled export published a partial output");
    check(QFile::exists(output), "Cancel removed the prior completed MAT file");
    window->close();
    check(wait([&] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); return !window; }), "completed window did not close");
    route->trigger(); QApplication::processEvents();
    window = main->findChild<MavlinkLogWindow *>();
    check(window && window->isVisible() && window->tlogPath().isEmpty(), "reopened Tools window retained a stale operation");
    if (window) window->close();
    main->close();
    qInfo() << "MATLAB runtime audit failures:" << failures;
    return failures ? 1 : 0;
}
