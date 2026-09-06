#include "ui/configuration/FirmwareArchiveController.h"

#include <QtTest>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSignalSpy>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QWidget>

#include <atomic>
#include <memory>

namespace
{
template<typename T>
T *visibleDialog(QWidget *owner, const char *name)
{
    const auto dialogs = owner->findChildren<T *>(
        QString::fromLatin1(name), Qt::FindChildrenRecursively);
    for (T *dialog : dialogs) {
        if (dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

struct FetchState
{
    QHash<QString, QByteArray> bodies;
    QSet<QString> unavailable;
    QMutex callsMutex;
    QStringList calls;
    std::atomic_bool blockFirmware{false};
    std::atomic_bool firmwareEntered{false};
    std::atomic_bool firmwareExited{false};
    std::atomic_bool cancellationObserved{false};
    std::atomic_bool releaseAfterCancellation{true};
    QString foreignParent;
    QString blockedPath;
    bool retainForeignStaging = false;
};

struct CancellationDrainRelease
{
    std::shared_ptr<FetchState> state;
    ~CancellationDrainRelease()
    {
        if (state) {
            state->releaseAfterCancellation.store(
                true, std::memory_order_release);
        }
    }
};

FirmwareArchive::Fetch fetchFor(const std::shared_ptr<FetchState> &state)
{
    return [state](const QUrl &url, qint64 maximumBytes, bool httpsOnly,
                   const FirmwareArchive::Cancel &cancel,
                   const FirmwareArchive::ChunkSink &sink) {
        const QString encoded = url.toString(QUrl::FullyEncoded);
        {
            QMutexLocker locker(&state->callsMutex);
            state->calls.append(encoded);
        }
        if (httpsOnly && url.scheme().compare(
                QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
            return FirmwareArchive::FetchResult{
                FirmwareArchive::Failure::Policy,
                QStringLiteral("test rejected non-HTTPS request"), 0};
        }
        const bool firmware = !url.path().endsWith(
            QStringLiteral("firmware2.xml"), Qt::CaseInsensitive);
        if (firmware && state->retainForeignStaging) {
            const QStringList stages = QDir(state->foreignParent).entryList(
                QStringList{QStringLiteral(
                    "MissionPlanner-Firmware-Archive-*.partial-*")},
                QDir::Dirs | QDir::NoDotAndDotDot);
            if (!stages.isEmpty()) {
                QFile foreign(QDir(state->foreignParent).filePath(
                    stages.first() + QStringLiteral("/keep.txt")));
                if (foreign.open(QIODevice::WriteOnly))
                    foreign.write("foreign");
            }
            return FirmwareArchive::FetchResult{
                FirmwareArchive::Failure::LocalIo,
                QStringLiteral("injected local failure"), 0};
        }
        const bool selectedForBlocking = state->blockedPath.isEmpty()
            || url.path().endsWith(state->blockedPath);
        if (firmware && selectedForBlocking
            && state->blockFirmware.load(std::memory_order_acquire)) {
            state->firmwareEntered.store(true, std::memory_order_release);
            bool cancelled = false;
            QElapsedTimer blockDeadline;
            blockDeadline.start();
            while (state->blockFirmware.load(std::memory_order_acquire)) {
                if (cancel && cancel()) {
                    state->cancellationObserved.store(
                        true, std::memory_order_release);
                    cancelled = true;
                    QElapsedTimer drainDeadline;
                    drainDeadline.start();
                    while (!state->releaseAfterCancellation.load(
                               std::memory_order_acquire)
                           && drainDeadline.elapsed() < 5000) {
                        QThread::msleep(2);
                    }
                    break;
                }
                if (blockDeadline.elapsed() >= 10000) {
                    state->firmwareExited.store(
                        true, std::memory_order_release);
                    return FirmwareArchive::FetchResult{
                        FirmwareArchive::Failure::Network,
                        QStringLiteral("test cancellation deadline expired"), 0};
                }
                QThread::msleep(2);
            }
            if (cancelled || (cancel && cancel())) {
                state->firmwareExited.store(true, std::memory_order_release);
                return FirmwareArchive::FetchResult{
                    FirmwareArchive::Failure::Cancelled,
                    QStringLiteral("cancelled in test transport"), 0};
            }
            state->firmwareExited.store(true, std::memory_order_release);
        }
        if (cancel && cancel()) {
            return FirmwareArchive::FetchResult{
                FirmwareArchive::Failure::Cancelled,
                QStringLiteral("cancelled in test transport"), 0};
        }
        if (state->unavailable.contains(encoded)
            || !state->bodies.contains(encoded)) {
            return FirmwareArchive::FetchResult{
                FirmwareArchive::Failure::Network,
                QStringLiteral("unavailable in test transport"), 0};
        }
        const QByteArray bytes = state->bodies.value(encoded);
        if (bytes.size() > maximumBytes) {
            return FirmwareArchive::FetchResult{
                FirmwareArchive::Failure::Limit,
                QStringLiteral("test body exceeds limit"), 0};
        }
        qint64 sent = 0;
        for (int at = 0; at < bytes.size(); at += 3) {
            if (cancel && cancel()) {
                return FirmwareArchive::FetchResult{
                    FirmwareArchive::Failure::Cancelled,
                    QStringLiteral("cancelled in test transport"), sent};
            }
            const QByteArray chunk = bytes.mid(at, 3);
            if (!sink(chunk)) {
                return FirmwareArchive::FetchResult{
                    FirmwareArchive::Failure::Cancelled,
                    QStringLiteral("test sink refused data"), sent};
            }
            sent += chunk.size();
        }
        return FirmwareArchive::FetchResult{
            FirmwareArchive::Failure::None, {}, sent};
    };
}

std::shared_ptr<FetchState> oneFirmwareBackend()
{
    auto state = std::make_shared<FetchState>();
    state->bodies.insert(
        QStringLiteral("https://manifest.test/firmware2.xml"),
        QByteArray("<options><Firmware><url>https://cdn.test/one.apj</url>"
                   "</Firmware></options>"));
    state->bodies.insert(QStringLiteral("https://cdn.test/one.apj"),
                         QByteArray::fromHex("000102ff74657374"));
    return state;
}

template<typename T>
T *waitForVisibleDialog(QWidget *owner, const char *name, int timeout = 1000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() <= timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (T *dialog = visibleDialog<T>(owner, name))
            return dialog;
        QTest::qWait(2);
    }
    return nullptr;
}

QFileDialog *openDirectoryPicker(QWidget *owner,
                                 FirmwareArchiveController *controller)
{
    const QPointer<FirmwareArchiveController> guard(controller);
    controller->start();
    if (!guard || !guard->busy())
        return nullptr;
    return waitForVisibleDialog<QFileDialog>(
        owner, "DeveloperFirmwareArchiveDirectoryDialog");
}

QMessageBox *selectDirectory(QWidget *owner, QFileDialog *picker,
                             const QString &path)
{
    if (!picker)
        return nullptr;
    QLineEdit *name = picker->findChild<QLineEdit *>(
        QStringLiteral("fileNameEdit"));
    if (!name)
        return nullptr;
    name->setText(path);
    const QString expected = QDir::cleanPath(
        QFileInfo(path).absoluteFilePath());
    const QStringList selected = picker->selectedFiles();
    if (selected.size() != 1
        || QDir::cleanPath(QFileInfo(selected.first()).absoluteFilePath())
            != expected) {
        QTest::qFail("Directory picker did not freeze the exact requested path.",
                     __FILE__, __LINE__);
        return nullptr;
    }
    if (!QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection))
        return nullptr;
    return waitForVisibleDialog<QMessageBox>(
        owner, "DeveloperFirmwareArchiveConfirmation");
}

QString destinationFromConsent(const QMessageBox *dialog)
{
    if (!dialog)
        return {};
    const QString marker = QStringLiteral("\n\nDestination: ");
    const int markerAt = dialog->text().lastIndexOf(marker);
    if (markerAt < 0)
        return {};
    return dialog->text().mid(markerAt + marker.size()).trimmed();
}

bool acceptConsent(QMessageBox *dialog)
{
    if (!dialog)
        return false;
    QAbstractButton *button = dialog->button(QMessageBox::Yes);
    if (!button)
        return false;
    button->click();
    return true;
}

QString publishedDirectory(const QTemporaryDir &parent)
{
    const QStringList entries = QDir(parent.path()).entryList(
        QStringList{QStringLiteral("MissionPlanner-Firmware-Archive-*")},
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        if (!entry.contains(QStringLiteral(".partial-")))
            return QDir(parent.path()).filePath(entry);
    }
    return {};
}

bool anyLogContains(const QSignalSpy &spy, const QString &needle)
{
    for (const QList<QVariant> &arguments : spy) {
        if (!arguments.isEmpty()
            && arguments.first().toString().contains(
                needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QPushButton *progressCancelButton(QProgressDialog *dialog)
{
    if (!dialog)
        return nullptr;
    const auto buttons = dialog->findChildren<QPushButton *>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QPushButton *button : buttons) {
        if (button->text().contains(
                QStringLiteral("Cancel"), Qt::CaseInsensitive)) {
            return button;
        }
    }
    return buttons.isEmpty() ? nullptr : buttons.first();
}
} // namespace

class FirmwareArchiveControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void directoryPickerAndConsentAreDefaultCancel();
    void downloadsToFrozenNewDirectoryAndReportsProgress();
    void unavailableFirmwarePublishesTruthfulPartialArchive();
    void cleanupFailureReportsRetainedStaging();
    void cancellationPreventsPublication();
    void progressCancelButtonStaysVisibleWhileCancellationDrains();
    void progressEscapeCancelsWithoutReopening_data();
    void progressEscapeCancelsWithoutReopening();
    void ownerCloseAndDestructionCancelOwnedWork();
    void busyBackendIsImmutableAndIdleCancelIsHarmless();
    void progressCallbackMayDeleteOwnerWithoutRevivingWork();
    void reentrantObserversCannotOpenOrReviveAWorkflow();
};

void FirmwareArchiveControllerTest::directoryPickerAndConsentAreDefaultCancel()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QWidget owner;
    owner.show();
    auto *controller = new FirmwareArchiveController(&owner);
    const auto state = oneFirmwareBackend();
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));
    QSignalSpy busySpy(controller, &FirmwareArchiveController::busyChanged);
    QSignalSpy logSpy(controller, &FirmwareArchiveController::logMessage);

    QFileDialog *picker = openDirectoryPicker(&owner, controller);
    QVERIFY(picker);
    QCOMPARE(picker->fileMode(), QFileDialog::Directory);
    QVERIFY(picker->testOption(QFileDialog::ShowDirsOnly));
    QCOMPARE(picker->acceptMode(), QFileDialog::AcceptOpen);
    QVERIFY(QMetaObject::invokeMethod(
        picker, "reject", Qt::DirectConnection));
    QTRY_VERIFY(!controller->busy());
    QCOMPARE(busySpy.size(), 2);
    QVERIFY(anyLogContains(logSpy, QStringLiteral("cancelled")));
    {
        QMutexLocker locker(&state->callsMutex);
        QVERIFY(state->calls.isEmpty());
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    picker = openDirectoryPicker(&owner, controller);
    QVERIFY(picker);
    QMessageBox *confirmation = selectDirectory(
        &owner, picker, parent.path());
    QVERIFY(confirmation);
    QCOMPARE(confirmation->textFormat(), Qt::PlainText);
    QCOMPARE(confirmation->defaultButton(),
             qobject_cast<QPushButton *>(
                 confirmation->button(QMessageBox::Cancel)));
    QCOMPARE(confirmation->escapeButton(),
             confirmation->button(QMessageBox::Cancel));
    const QString text = confirmation->text();
    QVERIFY(text.contains(QStringLiteral("large amount")));
    QVERIFY(text.contains(QStringLiteral("HTTP")));
    QVERIFY(text.contains(QStringLiteral("SHA-256")));
    QVERIFY(text.contains(QStringLiteral("do not authenticate")));
    QVERIFY(text.contains(QStringLiteral("not firmware flashing")));
    QVERIFY(text.contains(QStringLiteral("partial")));
    QVERIFY(text.contains(QStringLiteral("staging")));
    QVERIFY(text.contains(QFileInfo(parent.path()).canonicalFilePath()));
    auto *destinationLabel = confirmation->findChild<QLabel *>(
        QStringLiteral("DeveloperFirmwareArchiveDestination"));
    QVERIFY(destinationLabel);
    QVERIFY(destinationLabel->text().contains(parent.path()));
    auto *confirmButton = confirmation->findChild<QPushButton *>(
        QStringLiteral("DeveloperFirmwareArchiveConfirmButton"));
    QCOMPARE(confirmButton, qobject_cast<QPushButton *>(
        confirmation->button(QMessageBox::Yes)));
    confirmation->button(QMessageBox::Cancel)->click();
    QTRY_VERIFY(!controller->busy());
    QCOMPARE(publishedDirectory(parent), QString());
    {
        QMutexLocker locker(&state->callsMutex);
        QVERIFY(state->calls.isEmpty());
    }
}

void FirmwareArchiveControllerTest::downloadsToFrozenNewDirectoryAndReportsProgress()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QWidget owner;
    owner.show();
    auto *controller = new FirmwareArchiveController(&owner);
    const auto state = oneFirmwareBackend();
    state->blockFirmware.store(true, std::memory_order_release);
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));
    QSignalSpy logSpy(controller, &FirmwareArchiveController::logMessage);

    QMessageBox *confirmation = selectDirectory(
        &owner, openDirectoryPicker(&owner, controller), parent.path());
    QVERIFY(confirmation);
    const QString frozenDestination = destinationFromConsent(confirmation);
    QVERIFY(!frozenDestination.isEmpty());
    QCOMPARE(QFileInfo(frozenDestination).absolutePath(),
             QFileInfo(parent.path()).canonicalFilePath());
    QVERIFY(QFileInfo(frozenDestination).fileName().startsWith(
        QStringLiteral("MissionPlanner-Firmware-Archive-")));
    QVERIFY(acceptConsent(confirmation));
    QProgressDialog *progress = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((progress = visibleDialog<QProgressDialog>(
        &owner, "DeveloperFirmwareArchiveProgressDialog")), 1000);
    QCOMPARE(progress->windowModality(), Qt::NonModal);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->firmwareEntered.load(std::memory_order_acquire), 3000);

    state->blockFirmware.store(false, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    const QString output = publishedDirectory(parent);
    QVERIFY2(!output.isEmpty(), qPrintable(logSpy.isEmpty()
        ? QStringLiteral("no controller log")
        : logSpy.last().first().toString()));
    QCOMPARE(QFileInfo(output).canonicalFilePath(),
             QFileInfo(frozenDestination).canonicalFilePath());
    QVERIFY(QFileInfo::exists(QDir(output).filePath(
        QStringLiteral("firmware2.xml"))));
    QVERIFY(QFileInfo::exists(QDir(output).filePath(
        QStringLiteral("checksums.sha256"))));
    QVERIFY(QFileInfo::exists(QDir(output).filePath(
        QStringLiteral("archive-report.txt"))));
    QDirIterator files(output, QDir::Files, QDirIterator::Subdirectories);
    bool firmwareFound = false;
    while (files.hasNext()) {
        files.next();
        if (files.fileName().endsWith(QStringLiteral("-one.apj")))
            firmwareFound = true;
    }
    QVERIFY(firmwareFound);
    QVERIFY(anyLogContains(logSpy, QStringLiteral("complete")));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("1 downloaded")));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("8 bytes")));
    QVERIFY(anyLogContains(logSpy, output));
}

void FirmwareArchiveControllerTest::unavailableFirmwarePublishesTruthfulPartialArchive()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QWidget owner;
    owner.show();
    auto *controller = new FirmwareArchiveController(&owner);
    auto state = std::make_shared<FetchState>();
    state->bodies.insert(
        QStringLiteral("https://manifest.test/firmware2.xml"),
        QByteArray("<options><Firmware>"
                   "<url>https://cdn.test/one.apj</url>"
                   "<url2>https://cdn.test/missing.apj</url2>"
                   "</Firmware></options>"));
    state->bodies.insert(QStringLiteral("https://cdn.test/one.apj"),
                         QByteArray("one"));
    state->unavailable.insert(QStringLiteral("https://cdn.test/missing.apj"));
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));
    QSignalSpy logSpy(controller, &FirmwareArchiveController::logMessage);

    QMessageBox *confirmation = selectDirectory(
        &owner, openDirectoryPicker(&owner, controller), parent.path());
    QVERIFY(confirmation);
    QVERIFY(acceptConsent(confirmation));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    const QString output = publishedDirectory(parent);
    QVERIFY(!output.isEmpty());
    QVERIFY(anyLogContains(
        logSpy, QStringLiteral("published with unavailable files")));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("1 unavailable")));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("warnings")));
    QFile manifest(QDir(output).filePath(QStringLiteral("firmware2.xml")));
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QByteArray xml = manifest.readAll();
    QVERIFY(xml.contains("files/cdn.test/"));
    QVERIFY(xml.contains("https://cdn.test/missing.apj"));
}

void FirmwareArchiveControllerTest::cleanupFailureReportsRetainedStaging()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QWidget owner;
    owner.show();
    auto *controller = new FirmwareArchiveController(&owner);
    const auto state = oneFirmwareBackend();
    state->foreignParent = parent.path();
    state->retainForeignStaging = true;
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));
    QSignalSpy logSpy(controller, &FirmwareArchiveController::logMessage);

    QMessageBox *confirmation = selectDirectory(
        &owner, openDirectoryPicker(&owner, controller), parent.path());
    QVERIFY(confirmation);
    QVERIFY(acceptConsent(confirmation));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QCOMPARE(publishedDirectory(parent), QString());
    QVERIFY(anyLogContains(logSpy, QStringLiteral("failed")));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("Last known staging path")));
    const QStringList stages = QDir(parent.path()).entryList(
        QStringList{QStringLiteral(
            "MissionPlanner-Firmware-Archive-*.partial-*")},
        QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(stages.size(), 1);
    QVERIFY(QFileInfo::exists(QDir(parent.path()).filePath(
        stages.first() + QStringLiteral("/keep.txt"))));
}

void FirmwareArchiveControllerTest::cancellationPreventsPublication()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QWidget owner;
    owner.show();
    auto *controller = new FirmwareArchiveController(&owner);
    const auto state = oneFirmwareBackend();
    state->blockFirmware.store(true, std::memory_order_release);
    state->releaseAfterCancellation.store(false, std::memory_order_release);
    const CancellationDrainRelease release{state};
    Q_UNUSED(release);
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));
    QSignalSpy logSpy(controller, &FirmwareArchiveController::logMessage);

    QMessageBox *confirmation = selectDirectory(
        &owner, openDirectoryPicker(&owner, controller), parent.path());
    QVERIFY(confirmation);
    QVERIFY(acceptConsent(confirmation));
    QProgressDialog *progress = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((progress = visibleDialog<QProgressDialog>(
        &owner, "DeveloperFirmwareArchiveProgressDialog")), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->firmwareEntered.load(std::memory_order_acquire), 3000);
    controller->cancel();
    controller->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(
        state->cancellationObserved.load(std::memory_order_acquire), 3000);
    QVERIFY(controller->busy());
    QVERIFY(progress->isVisible());
    const QString waiting = progress->labelText();
    QVERIFY(waiting.contains(QStringLiteral("Cancellation requested")));
    QPointer<QPushButton> cancelButton = progressCancelButton(progress);
    QVERIFY(cancelButton);
    QVERIFY(!cancelButton->isEnabled());
    QTest::qWait(250);
    QVERIFY(progress->isVisible());
    QCOMPARE(progress->labelText(), waiting);
    state->releaseAfterCancellation.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QVERIFY(state->firmwareExited.load(std::memory_order_acquire));
    QCOMPARE(publishedDirectory(parent), QString());
    QVERIFY(anyLogContains(logSpy, QStringLiteral("cancellation requested")));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("cancelled")));

    int requests = 0;
    for (const QList<QVariant> &arguments : logSpy) {
        if (arguments.first().toString().contains(
                QStringLiteral("cancellation requested"),
                Qt::CaseInsensitive)) {
            ++requests;
        }
    }
    QCOMPARE(requests, 1);
}

void FirmwareArchiveControllerTest::progressCancelButtonStaysVisibleWhileCancellationDrains()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QWidget owner;
    owner.show();
    auto *controller = new FirmwareArchiveController(&owner);
    auto state = std::make_shared<FetchState>();
    state->bodies.insert(
        QStringLiteral("https://manifest.test/firmware2.xml"),
        QByteArray("<options><Firmware>"
                   "<url>https://cdn.test/fast.apj</url>"
                   "<url2>https://cdn.test/blocked.apj</url2>"
                   "</Firmware></options>"));
    state->bodies.insert(QStringLiteral("https://cdn.test/fast.apj"),
                         QByteArray("fast"));
    state->bodies.insert(QStringLiteral("https://cdn.test/blocked.apj"),
                         QByteArray("blocked"));
    state->blockedPath = QStringLiteral("blocked.apj");
    state->blockFirmware.store(true, std::memory_order_release);
    state->releaseAfterCancellation.store(false, std::memory_order_release);
    const CancellationDrainRelease release{state};
    Q_UNUSED(release);
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));

    QMessageBox *confirmation = selectDirectory(
        &owner, openDirectoryPicker(&owner, controller), parent.path());
    QVERIFY(confirmation);
    QVERIFY(acceptConsent(confirmation));
    QProgressDialog *progress = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((progress = visibleDialog<QProgressDialog>(
        &owner, "DeveloperFirmwareArchiveProgressDialog")), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->firmwareEntered.load(std::memory_order_acquire), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(progress->value() > 0, 3000);
    QPointer<QPushButton> cancelButton = progressCancelButton(progress);
    QVERIFY(cancelButton);
    cancelButton->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        state->cancellationObserved.load(std::memory_order_acquire), 3000);
    QVERIFY(controller->busy());
    QVERIFY(progress->isVisible());
    QVERIFY(cancelButton);
    QVERIFY(!cancelButton->isEnabled());
    const QString waiting = progress->labelText();
    QVERIFY(waiting.contains(QStringLiteral("Cancellation requested")));
    QTest::qWait(250);
    QVERIFY(progress->isVisible());
    QCOMPARE(progress->labelText(), waiting);

    state->releaseAfterCancellation.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QCOMPARE(publishedDirectory(parent), QString());
}

void FirmwareArchiveControllerTest::progressEscapeCancelsWithoutReopening_data()
{
    QTest::addColumn<bool>("explicitReject");
    QTest::newRow("escape-key") << false;
    QTest::newRow("dialog-reject") << true;
}

void FirmwareArchiveControllerTest::progressEscapeCancelsWithoutReopening()
{
    QFETCH(bool, explicitReject);
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QWidget owner;
    owner.show();
    auto *controller = new FirmwareArchiveController(&owner);
    const auto state = oneFirmwareBackend();
    state->blockFirmware.store(true, std::memory_order_release);
    state->releaseAfterCancellation.store(false, std::memory_order_release);
    const CancellationDrainRelease release{state};
    Q_UNUSED(release);
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));

    QMessageBox *confirmation = selectDirectory(
        &owner, openDirectoryPicker(&owner, controller), parent.path());
    QVERIFY(confirmation);
    QVERIFY(acceptConsent(confirmation));
    QProgressDialog *progress = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((progress = visibleDialog<QProgressDialog>(
        &owner, "DeveloperFirmwareArchiveProgressDialog")), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->firmwareEntered.load(std::memory_order_acquire), 3000);
    if (explicitReject) progress->reject();
    else QTest::keyClick(progress, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->cancellationObserved.load(std::memory_order_acquire), 3000);
    QVERIFY(controller->busy());
    // Qt5 can route Escape through its Cancel shortcut (keeps our waiting
    // dialog visible) or QDialog::reject (hides it). Both must cancel; a
    // hidden dialog must never be revived by the next progress tick.
    const bool visibleAfterEscape = progress->isVisible();
    if (explicitReject) QVERIFY(!visibleAfterEscape);
    QVERIFY(progress->labelText().contains(
        QStringLiteral("Cancellation requested")));
    QTest::qWait(250);
    QCOMPARE(progress->isVisible(), visibleAfterEscape);

    state->releaseAfterCancellation.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QCOMPARE(publishedDirectory(parent), QString());
}

void FirmwareArchiveControllerTest::ownerCloseAndDestructionCancelOwnedWork()
{
    QTemporaryDir closeParent;
    QVERIFY(closeParent.isValid());
    QWidget closeOwner;
    closeOwner.show();
    auto *closeController = new FirmwareArchiveController(&closeOwner);
    const auto closeState = oneFirmwareBackend();
    closeState->blockFirmware.store(true, std::memory_order_release);
    closeController->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(closeState));
    QMessageBox *closeConfirmation = selectDirectory(
        &closeOwner, openDirectoryPicker(&closeOwner, closeController),
        closeParent.path());
    QVERIFY(closeConfirmation);
    QVERIFY(acceptConsent(closeConfirmation));
    QTRY_VERIFY_WITH_TIMEOUT(
        closeState->firmwareEntered.load(std::memory_order_acquire), 3000);
    closeOwner.close();
    QTRY_VERIFY_WITH_TIMEOUT(!closeController->busy(), 5000);
    QVERIFY(closeState->firmwareExited.load(std::memory_order_acquire));
    QCOMPARE(publishedDirectory(closeParent), QString());

    QTemporaryDir destroyParent;
    QVERIFY(destroyParent.isValid());
    QPointer<QWidget> owner = new QWidget;
    owner->show();
    auto *controller = new FirmwareArchiveController(owner);
    const auto destroyState = oneFirmwareBackend();
    destroyState->blockFirmware.store(true, std::memory_order_release);
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(destroyState));
    QMessageBox *destroyConfirmation = selectDirectory(
        owner, openDirectoryPicker(owner, controller), destroyParent.path());
    QVERIFY(destroyConfirmation);
    QVERIFY(acceptConsent(destroyConfirmation));
    QTRY_VERIFY_WITH_TIMEOUT(
        destroyState->firmwareEntered.load(std::memory_order_acquire), 3000);
    delete owner;
    QVERIFY(owner.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(
        destroyState->firmwareExited.load(std::memory_order_acquire), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(QDir(destroyParent.path()).entryList(
        QStringList{QStringLiteral(
            "MissionPlanner-Firmware-Archive-*.partial-*")},
        QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(), 5000);
    QCOMPARE(publishedDirectory(destroyParent), QString());
}

void FirmwareArchiveControllerTest::busyBackendIsImmutableAndIdleCancelIsHarmless()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QWidget owner;
    owner.show();
    auto *controller = new FirmwareArchiveController(&owner);
    const auto original = oneFirmwareBackend();
    original->blockFirmware.store(true, std::memory_order_release);
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(original));
    QSignalSpy logSpy(controller, &FirmwareArchiveController::logMessage);

    QMessageBox *confirmation = selectDirectory(
        &owner, openDirectoryPicker(&owner, controller), parent.path());
    QVERIFY(confirmation);
    QVERIFY(acceptConsent(confirmation));
    QTRY_VERIFY_WITH_TIMEOUT(
        original->firmwareEntered.load(std::memory_order_acquire), 3000);
    auto replacement = std::make_shared<FetchState>();
    controller->setBackend(
        {QUrl(QStringLiteral("https://replacement.test/firmware2.xml"))},
        fetchFor(replacement));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("cannot change")));
    controller->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    {
        QMutexLocker locker(&replacement->callsMutex);
        QVERIFY(replacement->calls.isEmpty());
    }
    const int logCount = logSpy.size();
    controller->cancel();
    QCOMPARE(logSpy.size(), logCount + 1);
    QVERIFY(logSpy.last().first().toString().contains(
        QStringLiteral("no download"), Qt::CaseInsensitive));
}

void FirmwareArchiveControllerTest::progressCallbackMayDeleteOwnerWithoutRevivingWork()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QWidget owner;
    owner.show();
    QPointer<FirmwareArchiveController> controller =
        new FirmwareArchiveController(&owner);
    auto state = std::make_shared<FetchState>();
    state->bodies.insert(
        QStringLiteral("https://manifest.test/firmware2.xml"),
        QByteArray("<options><Firmware>"
                   "<url>https://cdn.test/fast.apj</url>"
                   "<url2>https://cdn.test/blocked.apj</url2>"
                   "</Firmware></options>"));
    state->bodies.insert(QStringLiteral("https://cdn.test/fast.apj"),
                         QByteArray("fast"));
    state->bodies.insert(QStringLiteral("https://cdn.test/blocked.apj"),
                         QByteArray("blocked"));
    state->blockedPath = QStringLiteral("blocked.apj");
    state->blockFirmware.store(true, std::memory_order_release);
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));

    QMessageBox *confirmation = selectDirectory(
        &owner, openDirectoryPicker(&owner, controller), parent.path());
    QVERIFY(confirmation);
    QVERIFY(acceptConsent(confirmation));
    QProgressDialog *progress = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((progress = visibleDialog<QProgressDialog>(
        &owner, "DeveloperFirmwareArchiveProgressDialog")), 1000);
    QProgressBar *bar = progress->findChild<QProgressBar *>();
    QVERIFY(bar);
    connect(bar, &QProgressBar::valueChanged, &owner,
            [controller](int value) {
        if (value > 0 && controller)
            controller->deleteLater();
    }, Qt::DirectConnection);
    QTRY_VERIFY_WITH_TIMEOUT(controller.isNull(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->firmwareExited.load(std::memory_order_acquire), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(QDir(parent.path()).entryList(
        QStringList{QStringLiteral(
            "MissionPlanner-Firmware-Archive-*.partial-*")},
        QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(), 5000);
    QCOMPARE(publishedDirectory(parent), QString());
}

void FirmwareArchiveControllerTest::reentrantObserversCannotOpenOrReviveAWorkflow()
{
    QPointer<QWidget> owner = new QWidget;
    owner->show();
    auto *controller = new FirmwareArchiveController(owner);
    const auto state = oneFirmwareBackend();
    controller->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));
    connect(controller, &FirmwareArchiveController::busyChanged,
            owner, [owner](bool busy) {
        if (busy && owner)
            delete owner;
    }, Qt::DirectConnection);
    controller->start();
    QVERIFY(owner.isNull());
    {
        QMutexLocker locker(&state->callsMutex);
        QVERIFY(state->calls.isEmpty());
    }

    QWidget secondOwner;
    secondOwner.show();
    auto *second = new FirmwareArchiveController(&secondOwner);
    second->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));
    QFileDialog *picker = openDirectoryPicker(&secondOwner, second);
    QVERIFY(picker);
    connect(picker, &QDialog::finished, &secondOwner,
            [&secondOwner](int) { secondOwner.close(); },
            Qt::DirectConnection);
    QVERIFY(QMetaObject::invokeMethod(
        picker, "reject", Qt::DirectConnection));
    QTRY_VERIFY(!second->busy());
    QVERIFY(!visibleDialog<QMessageBox>(
        &secondOwner, "DeveloperFirmwareArchiveConfirmation"));

    QPointer<QWidget> closingOwner = new QWidget;
    closingOwner->show();
    auto *closingController = new FirmwareArchiveController(closingOwner);
    closingController->setBackend(
        {QUrl(QStringLiteral("https://manifest.test/firmware2.xml"))},
        fetchFor(state));
    QVERIFY(openDirectoryPicker(closingOwner, closingController));
    connect(closingController, &FirmwareArchiveController::busyChanged,
            closingController, [closingOwner](bool busy) {
        if (!busy && closingOwner)
            delete closingOwner;
    }, Qt::DirectConnection);
    closingOwner->close();
    QVERIFY(closingOwner.isNull());
}

QTEST_MAIN(FirmwareArchiveControllerTest)
#include "test_firmwarearchivecontroller.moc"
