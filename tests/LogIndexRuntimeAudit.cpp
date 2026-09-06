#include "LogIndexRuntimeAudit.h"
#include "ui/MainWindow.h"
#include "ui/LogIndexWindow.h"
#include "ui/Loghandling/LogAnalysis.h"
#include "ui/configuration/ConfigDeveloperToolsView.h"
#include "MAVLinkProtocol.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <functional>

namespace {
bool waitFor(const std::function<bool()> &condition, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
template<class T> T *control(QObject *parent, const char *name)
{
    return parent ? parent->findChild<T *>(QString::fromLatin1(name)) : nullptr;
}
QDialog *visibleDialog(QObject *parent, const char *name)
{
    if (parent)
        for (auto *dialog : parent->findChildren<QDialog *>(QString::fromLatin1(name)))
            if (dialog->isVisible()) return dialog;
    return nullptr;
}
QByteArray tlogFixture()
{
    QByteArray bytes;
    const quint64 startUs = 1787306400000000ULL; // 2026-08-21T10:00:00Z
    auto append = [&](quint64 offsetUs, const mavlink_message_t &message) {
        const quint64 time = startUs + offsetUs;
        for (int shift = 56; shift >= 0; shift -= 8) bytes.append(char(time >> shift));
        quint8 frame[MAVLINK_MAX_PACKET_LEN]{};
        const int size = mavlink_msg_to_send_buffer(frame, &message);
        bytes.append(reinterpret_cast<const char *>(frame), size);
    };
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(12, 1, &message, MAV_TYPE_QUADROTOR,
        MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_MODE_FLAG_SAFETY_ARMED, 0, MAV_STATE_ACTIVE);
    append(0, message);
    mavlink_msg_global_position_int_pack(12, 1, &message, 1000,
        351000000, 332000000, 100000, 0, 1200, 0, 0, 0);
    append(1000000, message);
    mavlink_msg_global_position_int_pack(12, 1, &message, 3000,
        351001800, 332000000, 105000, 5000, 1200, 0, 0, 0);
    append(3000000, message);
    mavlink_camera_feedback_t camera{};
    camera.time_usec = 4000000;
    mavlink_msg_camera_feedback_encode(12, 1, &message, &camera);
    append(4000000, message);
    return bytes;
}
}

int RunLogIndexRuntimeAudit()
{
    int failures = 0;
    const auto expect = [&](bool condition, const char *reason) {
        if (!condition) { ++failures; qCritical() << "Log Index runtime:" << reason; }
    };
    QTemporaryDir directory;
    expect(directory.isValid(), "fixture directory unavailable");
    if (!directory.isValid()) return 1;
    const QString source = directory.filePath(QStringLiteral("flight with spaces.tlog"));
    const QString paired = directory.filePath(QStringLiteral("flight with spaces.rlog"));
    const QString unrelated = directory.filePath(QStringLiteral("flight with spaces.tlog.param"));
    const QString dataflash = directory.filePath(QStringLiteral("flight.log"));
    const QByteArray tlog = tlogFixture();
    const QByteArray log =
        "FMT, 128, 89, FMT, BBnNZ, Type,Length,Name,Format,Columns\n"
        "FMT, 129, 28, GPS, QBffff, TimeUS,Status,Lat,Lng,Alt,Spd\n"
        "FMT, 130, 75, MSG, QZ, TimeUS,Message\n"
        "FMT, 131, 31, PARM, QNf, TimeUS,Name,Value\n"
        "FMT, 132, 11, CAM, Q, TimeUS\n"
        "MSG, 500000, ArduCopter V4.6\n"
        "PARM, 600000, SYSID_THISMAV, 7\n"
        "GPS, 1000000, 3, 35.100000, 33.200000, 100, 12\n"
        "GPS, 3000000, 3, 35.100180, 33.200000, 105, 12\n"
        "CAM, 3100000\n";
    expect(writeFile(source, tlog) && writeFile(paired, "paired telemetry")
               && writeFile(unrelated, "must stay") && writeFile(dataflash, log),
           "fixture files could not be written");
    auto *main = MainWindow::instance();
    auto *developer = control<QAction>(main, "actionDeveloperTools");
    auto *indexAction = control<QAction>(main, "actionFlightLogIndex");
    expect(developer && indexAction, "production shared actions missing");
    if (!developer || !indexAction) return 1;
    developer->trigger();
    QApplication::processEvents();
    auto *page = main->findChild<ConfigDeveloperToolsView *>();
    auto *open = control<QPushButton>(page, "FlightLogIndexButton");
    expect(page && page->ActionCount() == 32 && page->ImplementedActionCount() == 27
               && open && open->isEnabled(), "Developer Index action not bound offline");
    if (!open) return 1;
    open->click();
    QPointer<LogIndexWindow> index(main->findChild<LogIndexWindow *>());
    expect(index && index->isVisible() && index->isWindow(), "Index is not a real modeless window");
    if (!index) return 1;
    expect(waitFor([&] { return index && !index->busy(); }), "initial index scan did not finish");
    indexAction->trigger();
    expect(main->findChildren<LogIndexWindow *>().size() == 1, "shared Index route created competing windows");
    auto *choose = control<QPushButton>(index, "ChooseDirectoryButton");
    auto *grid = control<QTableView>(index, "LogGrid");
    auto *remove = control<QPushButton>(index, "DeleteButton");
    expect(choose && grid && remove, "Index controls missing");
    if (!choose || !grid || !remove) return 1;
    expect(grid->model()->columnCount() == 13
               && grid->selectionMode() == QAbstractItemView::ExtendedSelection,
           "Index columns or extended selection differ from MP10");
    choose->click();
    auto *picker = control<QFileDialog>(index, "LogIndexCustomDirectoryDialog");
    expect(picker && picker->isVisible(), "Custom Directory picker missing");
    if (picker) picker->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(!QFile::exists(source + QStringLiteral(".jpg")), "cancelled picker scanned an unselected root");
    choose->click();
    picker = control<QFileDialog>(index, "LogIndexCustomDirectoryDialog");
    if (picker) {
        auto *name = control<QLineEdit>(picker, "fileNameEdit");
        expect(name, "directory filename editor missing");
        if (name) name->setText(directory.path());
        expect(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection), "directory picker cannot accept");
    }
    expect(waitFor([&] { return index && !index->busy() && grid->model()->rowCount() == 2; }),
           "custom directory did not yield two indexed logs");
    if (grid->model()->rowCount() != 2) return 1;
    const auto rowFor = [&](const QString &name) {
        for (int row = 0; row < grid->model()->rowCount(); ++row)
            if (grid->model()->index(row, 6).data().toString() == name) return row;
        return -1;
    };
    int row = rowFor(QFileInfo(source).fileName());
    expect(row >= 0, "tlog row missing");
    if (row < 0) return 1;
    expect(grid->model()->index(row, 3).data().toString() == QStringLiteral("QUADROTOR")
               && grid->model()->index(row, 4).data().toString() == QStringLiteral("12")
               && grid->model()->index(row, 5).data().toString() == QStringLiteral("00:00:04")
               && grid->model()->index(row, 9).data().toString() == QStringLiteral("00:00:02")
               && grid->model()->index(row, 11).data().toString() == QStringLiteral("1"),
           "TLOG metrics are not displayed correctly");
    expect(readFile(source) == tlog && readFile(dataflash) == log, "index scan changed source bytes");
    QImage thumbnail(source + QStringLiteral(".jpg"));
    expect(!thumbnail.isNull() && thumbnail.size() == QSize(240, 140), "canonical JPEG sidecar missing");
    grid->selectRow(row);
    const QString screenshot = qEnvironmentVariable("APM_LOG_INDEX_AUDIT_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        expect(index->grab().save(screenshot), "Index screenshot failed");
        grid->horizontalScrollBar()->setValue(grid->horizontalScrollBar()->maximum());
        QApplication::processEvents();
        expect(index->grab().save(screenshot + QStringLiteral(".right.png")),
               "Index right-hand columns screenshot failed");
        grid->horizontalScrollBar()->setValue(0);
    }
    // Keep a parser warning from blocking the audit inside its legacy exec().
    // Such a warning still fails the golden file check; it is not suppressed.
    bool parserWarning = false;
    QTimer parserDialogs;
    QObject::connect(&parserDialogs, &QTimer::timeout, main, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            auto *message = qobject_cast<QMessageBox *>(widget);
            if (message && message->isVisible()) {
                parserWarning = true;
                qWarning() << "Log Index browser warning:" << message->text() << message->informativeText();
                message->accept();
            }
        }
    });
    parserDialogs.start(10);
    // Follow the real table's activation connection to the retained Log Browser.
    expect(QMetaObject::invokeMethod(grid, "doubleClicked", Qt::DirectConnection,
                                    Q_ARG(QModelIndex, grid->model()->index(row, 6))),
           "Index row activation unavailable");
    auto *browser = main->findChild<LogAnalysis *>();
    expect(browser && browser->isVisible()
               && browser->windowTitle().contains(QFileInfo(source).fileName()),
           "Index row did not open the selected file in Log Browser");
    auto *browseIndex = control<QPushButton>(browser, "IndexBtn");
    expect(browseIndex && browseIndex->isVisible(), "Log Browser Index entry missing");
    if (browseIndex) browseIndex->click();
    expect(main->findChildren<LogIndexWindow *>().size() == 1, "Log Browser did not reuse Index singleton");
    if (browser) {
        // Loading is an existing asynchronous legacy-parser workflow. Finish
        // its event delivery before destroying this independent browser.
        expect(waitFor([&] { return !browser->isLoadingLog(); }),
               "Log Browser parser did not finish");
        expect(!parserWarning, "Log Browser could not parse the indexed golden file cleanly");
        browser->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    parserDialogs.stop();
    grid->selectRow(rowFor(QFileInfo(source).fileName()));
    remove->click();
    expect(waitFor([&] { return visibleDialog(index, "LogIndexDeleteConfirmation"); }), "Delete consent missing");
    auto *prompt = visibleDialog(index, "LogIndexDeleteConfirmation");
    if (prompt) {
        auto *buttons = prompt->findChild<QDialogButtonBox *>();
        expect(buttons && buttons->button(QDialogButtonBox::Cancel)->isDefault()
                   && !buttons->button(QDialogButtonBox::Ok)->isDefault(),
               "production destructive consent does not default to Cancel");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(prompt, &escape);
        expect(!prompt->isVisible(), "Escape did not cancel destructive consent");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(QFile::exists(source) && QFile::exists(paired) && QFile::exists(source + QStringLiteral(".jpg")),
           "Delete Cancel removed a file");
    remove->click();
    expect(waitFor([&] { return visibleDialog(index, "LogIndexDeleteConfirmation"); }), "Delete consent did not reopen");
    prompt = visibleDialog(index, "LogIndexDeleteConfirmation");
    if (prompt) {
        auto *paths = control<QPlainTextEdit>(prompt, "LogIndexDeletePaths");
        auto *confirm = control<QPushButton>(prompt, "LogIndexDeleteConfirmButton");
        expect(paths && paths->toPlainText().contains(source) && paths->toPlainText().contains(paired)
                   && paths->toPlainText().contains(source + QStringLiteral(".jpg"))
                   && !paths->toPlainText().contains(unrelated), "Delete consent does not list exact companions");
        if (!screenshot.isEmpty()) expect(prompt->grab().save(screenshot + QStringLiteral(".delete.png")), "Delete screenshot failed");
        if (confirm) confirm->click();
    }
    expect(waitFor([&] { return index && !index->busy() && !QFile::exists(source); }), "confirmed deletion did not finish");
    expect(!QFile::exists(paired) && !QFile::exists(source + QStringLiteral(".jpg"))
               && readFile(unrelated) == QByteArray("must stay") && readFile(dataflash) == log
               && grid->model()->rowCount() == 1, "Delete removed collateral files or kept a stale row");
    index->close();
    indexAction->trigger(); // queued WA_DeleteOnClose must not consume the successor.
    auto *reopened = main->findChild<LogIndexWindow *>();
    expect(!index && reopened && reopened->isVisible() && !reopened->isClosing(), "immediate Index reopen reused a closing window");
    if (reopened) {
        expect(waitFor([&] { return !reopened->busy(); }), "reopened default scan did not finish");
        reopened->close();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo() << "Log Index runtime audit failures:" << failures;
    return failures ? 1 : 0;
}
