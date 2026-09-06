#include "DataFlashLogToolsRuntimeAudit.h"
#include "ui/MainWindow.h"
#include "ui/LogDownloadViewModel.h"
#include "ui/LogDownloadWindow.h"
#include "ui/Loghandling/LogAnalysis.h"
#include "ui/flightdata/DataFlashLogsWidget.h"
#include "ui/flightdata/DataFlashLogToolsController.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>
#include <QXmlStreamReader>
#include <QDebug>

namespace {
template<class T> T *find(QObject *owner, const char *name) {
    return owner ? owner->findChild<T *>(QString::fromLatin1(name)) : nullptr;
}
template<class T> T *visible(QObject *owner, const char *name) {
    if (owner) for (auto *item : owner->findChildren<T *>(QString::fromLatin1(name)))
        if (item->isVisible()) return item;
    return nullptr;
}
bool wait(const std::function<bool()> &ready, int milliseconds = 15000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}
bool choose(QFileDialog *dialog, const QString &path) {
    if (!dialog) return false;
    if (auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        name->setText(path);
    else
        dialog->selectFile(path);
    const QStringList selected = dialog->selectedFiles();
    if (selected.size() != 1
        || QDir::cleanPath(QFileInfo(selected.first()).absoluteFilePath())
            != QDir::cleanPath(QFileInfo(path).absoluteFilePath())) {
        qCritical() << "DataFlash runtime picker resolved unexpected path"
                    << selected << "expected" << path;
        return false;
    }
    return QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
}
QByteArray bytes(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray fixed(const QByteArray &value, int size) {
    QByteArray result(size, '\0');
    for (int i = 0; i < qMin(size, value.size()); ++i) result[i] = value[i];
    return result;
}
void fmt(QByteArray &log, quint8 id, quint8 length, const QByteArray &name,
         const QByteArray &format, const QByteArray &columns) {
    log += QByteArray::fromHex("a39580");
    log += char(id); log += char(length);
    log += fixed(name, 4) + fixed(format, 16) + fixed(columns, 64);
}
bool fixture(const QString &path, int points) {
    QByteArray log;
    fmt(log, 128, 89, "FMT", "BBnNZ", "Type,Length,Name,Format,Columns");
    fmt(log, 150, 51, "GPS", "QBBIHBcLLeffffB",
        "TimeUS,I,Status,GMS,GWk,NSats,HDop,Lat,Lng,Alt,Spd,GCrs,VZ,Yaw,U");
    // Regression: GPS data immediately follows the first timestamp-bearing
    // FMT. The retained parser must not wait for an unrelated later FMT.
    for (int i = 0; i < points; ++i) {
        log += QByteArray::fromHex("a39596");
        QDataStream stream(&log, QIODevice::Append);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
        stream << quint64(2000000 + qint64(i) * 200000) << quint8(0) << quint8(3)
               << quint32(200000 + i * 200) << quint16(2400) << quint8(10)
               << qint16(120) << qint32(473977419 + i) << qint32(85455938 + i)
               << qint32(45000) << 0.0F << 0.0F << 0.0F << 0.0F << quint8(1);
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(log) == log.size() && file.flush();
}
bool confirm(QDialog *dialog) {
    if (!dialog) return false;
    if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        if (auto *yes = message->button(QMessageBox::Yes)) { yes->click(); return true; }
        if (auto *ok = message->button(QMessageBox::Ok)) { ok->click(); return true; }
    }
    if (auto *box = dialog->findChild<QDialogButtonBox *>()) {
        for (auto *button : box->buttons()) {
            if (box->buttonRole(button) == QDialogButtonBox::AcceptRole
                || box->buttonRole(button) == QDialogButtonBox::YesRole) {
                button->click(); return true;
            }
        }
    }
    return false;
}
bool defaultCancel(QDialog *dialog) {
    if (!dialog) return false;
    for (auto *button : dialog->findChildren<QPushButton *>()) {
        if (button->isDefault())
            return button->text().remove('&').contains("Cancel", Qt::CaseInsensitive);
    }
    return false;
}
}

int RunDataFlashLogToolsRuntimeAudit()
{
    QElapsedTimer auditTimer;
    auditTimer.start();
    const auto stage = [&auditTimer](const char *name) {
        qInfo() << "DataFlash runtime stage" << name
                << "at" << auditTimer.elapsed() << "ms";
    };
    int failures = 0;
    const auto check = [&](bool ok, const char *why) {
        if (!ok) { ++failures; qCritical() << "DataFlash runtime:" << why; }
    };
    auto *main = MainWindow::instance();
    auto *tabs = find<QTabWidget>(main, "FdTabs");
    auto *page = main->findChild<DataFlashLogsWidget *>();
    auto *controller = main->findChild<DataFlashLogToolsController *>();
    stage("route lookup");
    check(tabs && page && controller, "production DATA tab/controller missing");
    if (!tabs || !page || !controller) return 1;
    check(tabs->indexOf(page) >= 0
          && tabs->tabText(tabs->indexOf(page)) == "DataFlash Logs", "wrong DATA tab route");
    tabs->setCurrentWidget(page);
    main->show(); main->raise(); main->activateWindow();
    check(wait([&] { return page->isVisible(); }), "DataFlash tab not visible");
    const QString screenshots = qEnvironmentVariable("APM_DATAFLASH_TOOLS_AUDIT_SCREENSHOT");
    const char *working[] = {"DataFlashDownloadButton", "DataFlashReviewButton",
        "DataFlashAutoAnalysisButton", "DataFlashKmlGpxButton",
        "DataFlashBinToLogButton", "DataFlashOrganizeButton"};
    for (const char *name : working) {
        auto *button = find<QPushButton>(page, name);
        check(button && button->isVisible() && button->isEnabled(), "implemented offline tool unavailable");
        if (!button) return 1;
    }
    for (const char *name : {"DataFlashMatlabButton", "DataFlashGeoReferenceButton"}) {
        auto *button = find<QPushButton>(page, name);
        check(button && button->isVisible() && !button->isEnabled()
              && button->toolTip().contains("not yet ported"), "missing workflow falsely presented as working");
    }
    find<QPushButton>(page, "DataFlashDownloadButton")->click();
    check(wait([&] { return main->findChild<LogDownloadWindow *>(); }), "Download did not open real shared window offline");
    if (auto *download = main->findChild<LogDownloadWindow *>()) download->close();
    stage("download route closed");

    QTemporaryDir directory;
    const QString input = directory.filePath("flight.bin");
    const QString kml = directory.filePath("flight.kml");
    const QString gpx = directory.filePath("flight.gpx");
    check(fixture(input, 1000), "cannot create isolated BIN fixture");
    stage("valid binary fixture written");
    const QByteArray sourceHash = QCryptographicHash::hash(bytes(input), QCryptographicHash::Sha256);
    find<QPushButton>(page, "DataFlashReviewButton")->click();
    check(wait([&] { return visible<QFileDialog>(main, "DataFlashLogInputDialog"); }), "Review input dialog missing");
    check(choose(visible<QFileDialog>(main, "DataFlashLogInputDialog"), input), "Review picker unusable");
    check(wait([&] { return visible<LogAnalysis>(main, "DataFlashLogReviewWindow"); }), "Review did not open retained real log browser");
    QPointer<LogAnalysis> browser = visible<LogAnalysis>(main, "DataFlashLogReviewWindow");
    check(browser && wait([&] { return !browser || !browser->isLoadingLog(); }, 30000),
          "Review browser did not finish loading the valid fixture");
    if (browser) browser->close();
    check(wait([&] { return !browser || !browser->isVisible(); }),
          "Review browser did not close after loading completed");
    stage("review loaded and closed");
    check(controller->selectedLogPath() == input, "review path not shared with other tools");

    const auto openConsent = [&]() {
        find<QPushButton>(page, "DataFlashKmlGpxButton")->click();
        return wait([&] { return visible<QDialog>(main, "DataFlashKmlGpxConfirmationDialog"); });
    };
    check(openConsent(), "KML/GPX confirmation missing");
    auto *consent = visible<QDialog>(main, "DataFlashKmlGpxConfirmationDialog");
    check(defaultCancel(consent), "KML/GPX consent not default-Cancel");
    if (!screenshots.isEmpty() && consent) check(consent->grab().save(screenshots + ".consent.png"), "consent screenshot failed");
    if (consent) consent->reject();
    check(wait([&] { return !controller->busy(); }) && !QFile::exists(kml)
          && !QFile::exists(gpx), "Cancel published an export");
    check(openConsent() && confirm(visible<QDialog>(main, "DataFlashKmlGpxConfirmationDialog")), "KML/GPX consent cannot execute");
    check(wait([&] { return !controller->busy(); }) && QFile::exists(kml) && QFile::exists(gpx), "paired export failed");
    stage("KML and GPX completed");
    QXmlStreamReader xml(bytes(gpx)); int points = 0;
    while (!xml.atEnd()) { xml.readNext(); if (xml.isStartElement() && xml.name() == "trkpt") ++points; }
    check(!xml.hasError() && points == 1000, "GPX output is not a complete valid track");
    check(bytes(kml).contains("Flight Path"), "KML output missing reference track");
    const QByteArray oldGpx = bytes(gpx), oldKml = bytes(kml);
    find<QPushButton>(page, "DataFlashKmlGpxButton")->click();
    QApplication::processEvents();
    if (auto *again = visible<QDialog>(main, "DataFlashKmlGpxConfirmationDialog")) confirm(again);
    check(wait([&] { return !controller->busy(); }) && bytes(gpx) == oldGpx && bytes(kml) == oldKml,
          "existing outputs were replaced");

    find<QPushButton>(page, "DataFlashBinToLogButton")->click();
    check(wait([&] { return visible<QFileDialog>(main, "DataFlashBinToLogOutputDialog"); }), "BIN to LOG Save As missing");
    const QString textLog = directory.filePath("converted.log");
    check(choose(visible<QFileDialog>(main, "DataFlashBinToLogOutputDialog"), textLog), "BIN to LOG Save As unusable");
    check(wait([&] { return !controller->busy(); }) && bytes(textLog).contains("GPS,"), "BIN to LOG did not produce text records");
    stage("BIN to LOG completed");
    find<QPushButton>(page, "DataFlashAutoAnalysisButton")->click();
    check(wait([&] { return visible<QDialog>(main, "DataFlashAutoAnalysisDialog"); }), "Auto Analysis report missing");
    if (auto *report = visible<QDialog>(main, "DataFlashAutoAnalysisDialog")) {
        auto *text = find<QPlainTextEdit>(report, "DataFlashAutoAnalysisReport");
        check(text && text->toPlainText().split('\n').size() >= 17
              && text->toPlainText().contains("IMU mismatch")
              && text->toPlainText().contains("Optical flow")
              && text->toPlainText().contains("Duplicate data"),
              "Auto Analysis did not show all 17 reference checks");
        if (!screenshots.isEmpty()) check(report->grab().save(screenshots + ".analysis.png"), "analysis screenshot failed");
        report->close();
    }
    stage("Auto Analysis displayed");

    const QString organizeRoot = directory.filePath("organize");
    QDir().mkpath(organizeRoot);
    QFile empty(organizeRoot + "/empty.log");
    check(empty.open(QIODevice::WriteOnly | QIODevice::NewOnly), "cannot create organizer fixture"); empty.close();
    const auto openPlan = [&]() {
        find<QPushButton>(page, "DataFlashOrganizeButton")->click();
        if (!wait([&] { return visible<QFileDialog>(main, "DataFlashLogOrganizerDirectoryDialog"); })) return false;
        if (!choose(visible<QFileDialog>(main, "DataFlashLogOrganizerDirectoryDialog"), organizeRoot)) return false;
        return wait([&] { return visible<QDialog>(main, "DataFlashLogOrganizerPlanDialog"); });
    };
    check(openPlan(), "organizer directory/plan workflow missing");
    auto *plan = visible<QDialog>(main, "DataFlashLogOrganizerPlanDialog");
    check(defaultCancel(plan), "organizer plan not default-Cancel");
    if (!screenshots.isEmpty() && plan) check(plan->grab().save(screenshots + ".organize.png"), "organizer screenshot failed");
    if (plan) plan->reject();
    check(wait([&] { return !controller->busy(); }) && QFile::exists(empty.fileName()), "organizer Cancel deleted a file");
    check(openPlan() && confirm(visible<QDialog>(main, "DataFlashLogOrganizerPlanDialog")), "organizer plan cannot execute");
    check(wait([&] { return !controller->busy(); }) && !QFile::exists(empty.fileName()), "confirmed organizer did not execute");
    stage("organizer completed");

    check(QCryptographicHash::hash(bytes(input), QCryptographicHash::Sha256) == sourceHash,
          "an offline operation changed its source BIN");
    if (!screenshots.isEmpty()) {
        main->resize(1280, 800); QApplication::processEvents();
        check(main->grab().save(screenshots + ".data.png"), "DATA screenshot failed");
        main->resize(1120, 720); QApplication::processEvents();
        check(main->width() <= 1120, "log tab forces main window beyond minimum width");
        check(main->grab().save(screenshots + ".minimum.png"), "minimum DATA screenshot failed");
    }
    const QString cancelInput = directory.filePath("cancel.bin");
    check(fixture(cancelInput, 100000), "cannot create cancellation fixture");
    controller->setSelectedLogPath(cancelInput);
    check(openConsent() && confirm(visible<QDialog>(main, "DataFlashKmlGpxConfirmationDialog")),
          "cancellable conversion could not start");
    check(controller->busy() && visible<QDialog>(main, "DataFlashLogProgressDialog"),
          "working export has no progress/cancellation window");
    controller->cancel();
    check(wait([&] { return !controller->busy(); })
          && !QFile::exists(directory.filePath("cancel.kml"))
          && !QFile::exists(directory.filePath("cancel.gpx")),
          "early cancellation did not drain or published outputs");
    stage("explicit cancellation drained");
    check(openConsent() && confirm(visible<QDialog>(main, "DataFlashKmlGpxConfirmationDialog")),
          "shutdown conversion could not start");
    main->close();
    check(controller->shutdownPending(), "MainWindow Close did not request controller shutdown");
    check(wait([&] { return !controller->busy() && !main->isVisible(); })
          && !QFile::exists(directory.filePath("cancel.kml"))
          && !QFile::exists(directory.filePath("cancel.gpx")),
          "MainWindow Close did not drain cancellation before closing");
    stage("shutdown cancellation drained");
    qInfo() << "DataFlash runtime audit failures:" << failures
            << "(actual DATA routes; local files only, no network vehicle)";
    return failures ? 1 : 0;
}
