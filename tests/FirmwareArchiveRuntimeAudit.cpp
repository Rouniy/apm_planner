#include "FirmwareArchiveRuntimeAudit.h"
#include "ui/MainWindow.h"
#include "ui/configuration/ConfigDeveloperToolsView.h"
#include "ui/configuration/FirmwareArchiveController.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>

namespace {
bool waitFor(const std::function<bool()> &ready, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}
QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
template<class T> T *find(QObject *root, const char *name) {
    return root ? root->findChild<T *>(QString::fromLatin1(name)) : nullptr;
}
template<class T> T *visible(QObject *root, const char *name) {
    if (root)
        for (auto *dialog : root->findChildren<T *>(QString::fromLatin1(name)))
            if (dialog->isVisible()) return dialog;
    return nullptr;
}
struct Fixture {
    QMutex mutex;
    QStringList requests;
    std::atomic_bool holdFirmware{false};
    std::atomic_int firmwareStarted{0};
    const QByteArray a = QByteArray("\0A\xff", 3);
    const QByteArray b = QByteArray("\0B\0CD", 5);
    const QByteArray manifest =
        "<?xml version=\"1.0\"?><options><!--fixture--><Firmware><name>Copter</name>"
        "<url>http://legacy.invalid/a.apj</url><urlDuplicate>http://legacy.invalid/a.apj</urlDuplicate>"
        "<url2560-2>http://legacy.invalid/b.hex</url2560-2>"
        "<urlMissing>https://legacy.invalid/missing.apj</urlMissing></Firmware></options>";
};
FirmwareArchive::Fetch transport(const std::shared_ptr<Fixture> &fixture) {
    return [fixture](const QUrl &url, qint64 cap, bool,
                     const FirmwareArchive::Cancel &cancel,
                     const FirmwareArchive::ChunkSink &sink) {
        using namespace FirmwareArchive;
        const QString key = url.toString(QUrl::FullyEncoded);
        {
            QMutexLocker lock(&fixture->mutex);
            fixture->requests.append(key);
        }
        QByteArray bytes;
        if (key == QStringLiteral("https://archive.invalid/firmware2.xml")) {
            bytes = fixture->manifest;
        } else if (key.contains(QStringLiteral("legacy.invalid"))) {
            fixture->firmwareStarted.fetch_add(1);
            while (fixture->holdFirmware.load() && !(cancel && cancel())) QThread::msleep(1);
            if (cancel && cancel()) return FetchResult{Failure::Cancelled, QStringLiteral("fixture cancellation"), 0};
            if (key == QStringLiteral("https://legacy.invalid/a.apj")) bytes = fixture->a;
            else if (key == QStringLiteral("http://legacy.invalid/b.hex")) bytes = fixture->b;
            else return FetchResult{Failure::Network, QStringLiteral("fixture unavailable"), 0};
        } else return FetchResult{Failure::Network, QStringLiteral("fixture mirror unavailable"), 0};
        if (bytes.size() > cap) return FetchResult{Failure::Limit, QStringLiteral("fixture cap"), 0};
        if (!sink(bytes)) return FetchResult{Failure::LocalIo, QStringLiteral("fixture sink failed"), 0};
        return FetchResult{Failure::None, {}, bytes.size()};
    };
}
}

int RunFirmwareArchiveRuntimeAudit()
{
    int failures = 0;
    const auto expect = [&](bool condition, const char *why) {
        if (!condition) { ++failures; qCritical() << "Firmware Archive runtime:" << why; }
    };
    QTemporaryDir parent;
    expect(parent.isValid(), "temporary archive parent unavailable");
    if (!parent.isValid()) return 1;
    const auto fixture = std::make_shared<Fixture>();
    auto *main = MainWindow::instance();
    auto *action = find<QAction>(main, "actionDeveloperTools");
    expect(action, "shared Developer route missing");
    if (!action) return 1;
    action->trigger();
    QApplication::processEvents();
    auto *page = main->findChild<ConfigDeveloperToolsView *>();
    auto *controller = page ? page->findChild<FirmwareArchiveController *>() : nullptr;
    auto *start = find<QPushButton>(page, "DownloadFirmwareArchiveButton");
    auto *cancel = find<QPushButton>(page, "CancelFirmwareArchiveButton");
    auto *split = find<QPushButton>(page, "SplitDataFlashLogButton");
    expect(page && page->ActionCount() == 32 && page->ImplementedActionCount() == 31,
           "Developer inventory is not 31 of 32");
    expect(controller && start && cancel && split && start->isEnabled() && !cancel->isEnabled(),
           "archive actions are not available offline with owned idle Cancel");
    if (!controller || !start || !cancel || !split) return 1;
    controller->setBackend({QUrl("https://primary.invalid/firmware2.xml"),
                            QUrl("https://archive.invalid/firmware2.xml")}, transport(fixture));
    start->click();
    auto *picker = visible<QFileDialog>(page, "DeveloperFirmwareArchiveDirectoryDialog");
    expect(picker && !split->isEnabled() && cancel->isEnabled(), "directory picker did not reserve Developer file gate");
    if (picker) picker->reject();
    expect(waitFor([&] { return !controller->busy(); }), "picker cancellation did not finish");
    expect(fixture->requests.isEmpty() && QDir(parent.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(),
           "picker cancellation made network or filesystem changes");
    const auto choose = [&]() -> QMessageBox * {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        start->click();
        auto *dialog = visible<QFileDialog>(page, "DeveloperFirmwareArchiveDirectoryDialog");
        if (!dialog) return nullptr;
        auto *name = find<QLineEdit>(dialog, "fileNameEdit");
        if (!name) return nullptr;
        name->setText(parent.path());
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        return visible<QMessageBox>(page, "DeveloperFirmwareArchiveConfirmation");
    };
    auto *consent = choose();
    expect(consent && consent->defaultButton() == consent->button(QMessageBox::Cancel)
               && consent->escapeButton() == consent->button(QMessageBox::Cancel),
           "archive consent missing or not default/Escape Cancel");
    if (!consent) return 1;
    expect(consent->text().contains(parent.path()) && consent->text().contains("HTTP")
               && consent->text().contains("SHA-256"), "exact destination or integrity warning missing");
    consent->button(QMessageBox::Cancel)->click();
    expect(waitFor([&] { return !controller->busy(); }), "consent Cancel did not finish");
    expect(fixture->requests.isEmpty(), "network started before affirmative consent");
    consent = choose();
    expect(consent, "archive consent did not reopen");
    if (!consent) return 1;
    const QString screenshot = qEnvironmentVariable("APM_FIRMWARE_ARCHIVE_AUDIT_SCREENSHOT");
    if (!screenshot.isEmpty()) expect(consent->grab().save(screenshot), "consent screenshot failed");
    consent->button(QMessageBox::Yes)->click();
    expect(waitFor([&] { return !controller->busy(); }), "archive fixture did not finish");
    const QStringList published = QDir(parent.path()).entryList(
        {QStringLiteral("MissionPlanner-Firmware-Archive-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    expect(published.size() == 1 && !published.value(0).contains(".partial-"), "archive publication/cleanup failed");
    if (published.size() != 1) return 1;
    const QString directory = parent.filePath(published.first());
    const QByteArray xml = read(directory + "/firmware2.xml");
    QDomDocument document;
    expect(document.setContent(xml), "published manifest is not valid XML");
    const auto field = [&](const QString &tag) { return document.elementsByTagName(tag).at(0).toElement().text(); };
    const QString a = field("url"), b = field("url2560-2");
    expect(a.startsWith("files/") && b.startsWith("files/") && field("urlDuplicate") == a
               && field("urlMissing") == QStringLiteral("https://legacy.invalid/missing.apj"),
           "manifest duplicates or unavailable references rewritten incorrectly");
    expect(read(directory + '/' + a) == fixture->a && read(directory + '/' + b) == fixture->b,
           "published firmware bytes differ from source responses");
    const QByteArray sums = read(directory + "/checksums.sha256");
    expect(sums.contains(QCryptographicHash::hash(fixture->a, QCryptographicHash::Sha256).toHex() + "  " + a.toUtf8())
               && sums.contains(QCryptographicHash::hash(fixture->b, QCryptographicHash::Sha256).toHex() + "  " + b.toUtf8()),
           "checksums do not match saved-byte hashes");
    const QByteArray report = read(directory + "/archive-report.txt");
    expect(report.contains("Downloaded: 2") && report.contains("Unavailable: 1") && report.contains("Bytes: 8"),
           "published partial archive report is untruthful");
    {
        QMutexLocker lock(&fixture->mutex);
        expect(fixture->requests.count("https://legacy.invalid/a.apj") == 1
                   && !fixture->requests.contains("http://legacy.invalid/a.apj")
                   && fixture->requests.contains("https://legacy.invalid/b.hex")
                   && fixture->requests.contains("http://legacy.invalid/b.hex"),
               "HTTPS preference, fallback or deduplication differs from reference");
    }
    fixture->holdFirmware = true;
    const int before = fixture->firmwareStarted.load();
    consent = choose();
    expect(consent, "second archive consent missing");
    if (!consent) return 1;
    consent->button(QMessageBox::Yes)->click();
    expect(waitFor([&] { return fixture->firmwareStarted.load() > before; }), "cancellation fixture never reached streaming");
    expect(cancel->isEnabled() && !start->isEnabled() && !split->isEnabled(), "active archive did not retain gate");
    cancel->click();
    expect(waitFor([&] { return !controller->busy(); }), "owned Cancel did not stop archive workers");
    expect(QDir(parent.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot) == published,
           "cancelled archive left a completed-looking destination or staging");
    expect(read(directory + "/firmware2.xml") == xml && read(directory + "/checksums.sha256") == sums,
           "cancellation changed an earlier completed archive");
    expect(start->isEnabled() && !cancel->isEnabled() && split->isEnabled(), "Developer file gate did not recover");
    if (!screenshot.isEmpty()) expect(page->grab().save(screenshot + ".page.png"), "Developer result screenshot failed");
    qInfo() << "Firmware Archive runtime audit failures:" << failures;
    return failures ? 1 : 0;
}
