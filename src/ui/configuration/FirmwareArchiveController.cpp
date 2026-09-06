#include "FirmwareArchiveController.h"

#include "services/FirmwareArchiveHttp.h"
#include "services/FirmwareArchiveService.h"

#include <QtConcurrentRun>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QEvent>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QLabel>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <exception>
#include <utility>

namespace
{
QString warningsText(const QStringList &warnings)
{
    if (warnings.isEmpty())
        return {};
    const int displayed = qMin(3, warnings.size());
    QString text = warnings.mid(0, displayed).join(QStringLiteral(" "));
    if (warnings.size() > displayed) {
        text += QObject::tr(" (+%1 more warning(s))")
            .arg(warnings.size() - displayed);
    }
    return text;
}
} // namespace

struct FirmwareArchiveController::JobState
{
    std::atomic_bool cancelled{false};
    std::atomic_int completed{0};
    std::atomic_int total{0};
    QMutex mutex;
    QString item;
};

FirmwareArchiveController::FirmwareArchiveController(QWidget *owner)
    : QObject(owner)
    , m_owner(owner)
    , m_progressTimer(new QTimer(this))
    , m_manifests(FirmwareArchiveService::officialManifestUris())
    , m_fetch(FirmwareArchiveHttp::transport())
{
    m_progressTimer->setInterval(100);
    connect(m_progressTimer, &QTimer::timeout,
            this, &FirmwareArchiveController::updateProgress);
    if (owner)
        owner->installEventFilter(this);
}

FirmwareArchiveController::~FirmwareArchiveController()
{
    m_destroying = true;
    ++m_flow;
    if (m_job)
        m_job->cancelled.store(true, std::memory_order_release);
    if (m_progressTimer)
        m_progressTimer->stop();
    if (m_prompt) {
        const QPointer<QDialog> prompt(m_prompt);
        m_prompt.clear();
        disconnect(prompt.data(), nullptr, nullptr, nullptr);
        prompt->blockSignals(true);
        if (!QCoreApplication::closingDown())
            prompt->reject();
        if (prompt && !QCoreApplication::closingDown())
            prompt->deleteLater();
    }
    if (m_progress) {
        const QPointer<QProgressDialog> progress(m_progress);
        m_progress.clear();
        disconnect(progress.data(), nullptr, nullptr, nullptr);
        progress->blockSignals(true);
        if (!QCoreApplication::closingDown()) {
            progress->hide();
            if (progress)
                progress->deleteLater();
        }
    }
}

bool FirmwareArchiveController::busy() const noexcept
{
    return m_phase != Phase::Idle;
}

void FirmwareArchiveController::setBackend(
    QVector<QUrl> manifests, FirmwareArchive::Fetch fetch)
{
    if (busy()) {
        emitLog(tr("Firmware archive: the backend cannot change during an active workflow."));
        return;
    }
    m_manifests = std::move(manifests);
    m_fetch = std::move(fetch);
}

bool FirmwareArchiveController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_owner && event && event->type() == QEvent::Close
        && busy()) {
        const QPointer<FirmwareArchiveController> guard(this);
        const QPointer<QObject> watchedGuard(watched);
        cancel();
        // A synchronous terminal observer may delete the page while its Close
        // event is filtered. Consume that event rather than returning it to a
        // receiver which no longer exists.
        if (!guard || !watchedGuard)
            return true;
    }
    return QObject::eventFilter(watched, event);
}

bool FirmwareArchiveController::emitLog(const QString &message)
{
    const QPointer<FirmwareArchiveController> guard(this);
    emit logMessage(message);
    return !guard.isNull();
}

void FirmwareArchiveController::start()
{
    if (m_destroying)
        return;
    if (busy()) {
        emitLog(tr("Firmware archive: this controller is already busy."));
        return;
    }
    if (!m_owner) {
        emitLog(tr("Firmware archive: no window is available for selecting an output directory."));
        return;
    }
    if (m_manifests.isEmpty() || !m_fetch) {
        emitLog(tr("Firmware archive: the manifest or download transport is unavailable."));
        return;
    }

    m_destination.clear();
    m_job.reset();
    m_lastReportedProgress = 0;
    m_cancelReported = false;
    m_phase = Phase::DirectoryPrompt;
    ++m_flow;
    if (m_flow == 0)
        ++m_flow;
    const quint64 flow = m_flow;
    const QPointer<FirmwareArchiveController> guard(this);
    emit busyChanged(true);
    if (!guard || m_destroying || flow != m_flow
        || m_phase != Phase::DirectoryPrompt) {
        return;
    }
    showDirectoryPrompt(flow);
}

void FirmwareArchiveController::showDirectoryPrompt(quint64 flow)
{
    if (m_destroying || flow != m_flow
        || m_phase != Phase::DirectoryPrompt || !m_owner) {
        return;
    }
    auto *dialog = new QFileDialog(
        m_owner, tr("Select parent directory for the firmware archive"));
    dialog->setObjectName(
        QStringLiteral("DeveloperFirmwareArchiveDirectoryDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (m_destroying || flow != m_flow
            || m_phase != Phase::DirectoryPrompt
            || m_prompt != dialog) {
            return;
        }
        m_prompt.clear();
        const QStringList selected = dialog->selectedFiles();
        if (result != QDialog::Accepted || selected.size() != 1) {
            finishPrompt(flow, tr(
                "Firmware archive: cancelled before an output parent directory was selected."));
            return;
        }
        QString error;
        const QString destination = FirmwareArchiveService::nextDirectory(
            selected.first(), QDateTime::currentDateTimeUtc(), &error);
        if (destination.isEmpty()) {
            finishPrompt(flow, tr("Firmware archive failed: %1").arg(error));
            return;
        }
        m_destination = destination;
        m_phase = Phase::ConsentPrompt;
        showConsentPrompt(flow);
    });
    const QPointer<FirmwareArchiveController> guard(this);
    dialog->open();
    if (!guard)
        return;
}

void FirmwareArchiveController::showConsentPrompt(quint64 flow)
{
    if (m_destroying || flow != m_flow
        || m_phase != Phase::ConsentPrompt || !m_owner
        || m_destination.isEmpty()) {
        return;
    }
    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Download Firmware Archive"),
        tr("Download every firmware binary referenced by Mission Planner's "
           "official firmware2.xml manifests? This can transfer a large amount "
           "of data and may take a long time.\n\n"
           "The legacy manifest contains unsigned HTTP firmware URLs. The "
           "downloader tries HTTPS first but may fall back to HTTP for legacy "
           "hosts. Recorded SHA-256 digests detect later changes; they do not "
           "authenticate the source.\n\n"
           "The archive is not firmware flashing. A published archive can be "
           "partial: unavailable files remain network URLs in its local "
           "manifest. Cancellation or failure normally prevents publication, "
           "but cleanup failure can leave an explicitly reported staging "
           "directory.\n\nDestination: %1").arg(m_destination),
        QMessageBox::Yes | QMessageBox::Cancel, m_owner);
    dialog->setObjectName(
        QStringLiteral("DeveloperFirmwareArchiveConfirmation"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->button(QMessageBox::Yes)->setText(tr("Download all firmware"));
    dialog->button(QMessageBox::Yes)->setObjectName(
        QStringLiteral("DeveloperFirmwareArchiveConfirmButton"));
    if (auto *destinationLabel = dialog->findChild<QLabel *>(
            QStringLiteral("qt_msgbox_label"))) {
        destinationLabel->setObjectName(
            QStringLiteral("DeveloperFirmwareArchiveDestination"));
        destinationLabel->setTextFormat(Qt::PlainText);
    }
    if (auto *confirm = qobject_cast<QPushButton *>(
            dialog->button(QMessageBox::Yes))) {
        confirm->setAutoDefault(false);
    }
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (m_destroying || flow != m_flow
            || m_phase != Phase::ConsentPrompt || m_prompt != dialog) {
            return;
        }
        m_prompt.clear();
        if (result != QMessageBox::Yes) {
            finishPrompt(flow, tr(
                "Firmware archive: cancelled before any download was started."));
            return;
        }
        startWorker(flow);
    });
    const QPointer<FirmwareArchiveController> guard(this);
    dialog->open();
    if (!guard)
        return;
}

void FirmwareArchiveController::startWorker(quint64 flow)
{
    if (m_destroying || flow != m_flow
        || m_phase != Phase::ConsentPrompt || !m_owner
        || m_destination.isEmpty() || m_manifests.isEmpty() || !m_fetch) {
        finishPrompt(flow, tr(
            "Firmware archive failed: its frozen backend or destination is unavailable."));
        return;
    }

    const QVector<QUrl> manifests = m_manifests;
    const FirmwareArchive::Fetch fetch = m_fetch;
    const QString destination = m_destination;
    auto state = std::make_shared<JobState>();
    m_job = state;
    m_phase = Phase::Worker;

    auto *progress = new QProgressDialog(
        tr("Reading the official firmware2.xml mirrors…"), tr("Cancel"),
        0, 0, m_owner);
    progress->setObjectName(
        QStringLiteral("DeveloperFirmwareArchiveProgressDialog"));
    progress->setWindowTitle(tr("Download Firmware Archive"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    m_progress = progress;
    // QProgressDialog normally routes canceled() back to its own cancel()
    // slot, which hides and resets the dialog before an application can show
    // that bounded cancellation is still draining. Keep the dialog visible
    // for its button path; Escape/reject may hide it, but both paths request
    // cancellation and the progress poll must never show it again.
    // Qt5 installs this connection with the string-based API; a typed-slot
    // disconnect does not remove that connection on all supported Qt builds.
    disconnect(progress, SIGNAL(canceled()), progress, SLOT(cancel()));
    connect(progress, &QProgressDialog::canceled,
            this, &FirmwareArchiveController::cancel);
    connect(progress, &QDialog::rejected,
            this, &FirmwareArchiveController::cancel);

    auto *watcher = new QFutureWatcher<FirmwareArchive::Result>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<FirmwareArchive::Result>::finished,
            this, [this, watcher, state, flow]() {
        const FirmwareArchive::Result result = watcher->result();
        watcher->deleteLater();
        finishWorker(flow, state, result);
    });
    watcher->setFuture(QtConcurrent::run(
        [manifests, destination, fetch, state]() {
        try {
            const auto cancel = [state]() {
                return state->cancelled.load(std::memory_order_acquire);
            };
            const auto report = [state](int completed, int total,
                                        const QString &item) {
                QMutexLocker locker(&state->mutex);
                state->item = item;
                state->total.store(qMax(0, total), std::memory_order_relaxed);
                state->completed.store(completed, std::memory_order_relaxed);
            };
            return FirmwareArchiveService::download(
                manifests, destination, fetch, cancel, report);
        } catch (const std::exception &exception) {
            FirmwareArchive::Result result;
            result.error = QStringLiteral("Unexpected archive worker failure: %1")
                .arg(QString::fromLocal8Bit(exception.what()));
            return result;
        } catch (...) {
            FirmwareArchive::Result result;
            result.error = QStringLiteral("Unexpected archive worker failure.");
            return result;
        }
    }));
    m_progressTimer->start();
    const QPointer<FirmwareArchiveController> guard(this);
    progress->show();
    if (!guard || flow != m_flow || m_phase != Phase::Worker)
        return;
    emitLog(tr("Firmware archive: reading the official firmware2.xml mirrors…"));
}

void FirmwareArchiveController::cancel()
{
    if (m_destroying)
        return;
    if (m_phase == Phase::Idle) {
        emitLog(tr("Firmware archive: no download is running."));
        return;
    }
    if (m_phase == Phase::DirectoryPrompt
        || m_phase == Phase::ConsentPrompt) {
        finishPrompt(m_flow, tr(
            "Firmware archive: cancelled before any download was started."));
        return;
    }
    const std::shared_ptr<JobState> state = m_job;
    if (!state)
        return;
    state->cancelled.store(true, std::memory_order_release);
    const bool shouldReport = !m_cancelReported;
    m_cancelReported = true;
    const QPointer<FirmwareArchiveController> guard(this);
    const QPointer<QProgressDialog> progress(m_progress);
    if (progress) {
        progress->setLabelText(tr(
            "Cancellation requested; waiting for the next bounded network or file boundary…"));
        if (!guard || !progress || m_progress != progress)
            return;
        // Do not call setCancelButton(nullptr) from the button's own clicked
        // stack: Qt deletes the old button. Disabling it is synchronous-safe.
        const auto buttons = progress->findChildren<QPushButton *>(
            QString(), Qt::FindDirectChildrenOnly);
        const QPointer<QPushButton> cancelButton(
            buttons.isEmpty() ? nullptr : buttons.first());
        if (cancelButton)
            cancelButton->setEnabled(false);
        if (!guard || !progress || m_progress != progress)
            return;
    }
    if (!guard || !shouldReport)
        return;
    emitLog(tr("Firmware archive: cancellation requested…"));
}

void FirmwareArchiveController::finishPrompt(
    quint64 flow, const QString &message)
{
    if (m_destroying || flow != m_flow || m_phase == Phase::Worker)
        return;
    ++m_flow;
    const quint64 terminalFlow = m_flow;
    m_phase = Phase::Idle;
    m_destination.clear();
    if (!dismissPrompt() || m_flow != terminalFlow
        || m_phase != Phase::Idle)
        return;
    const QPointer<FirmwareArchiveController> guard(this);
    emit busyChanged(false);
    if (guard)
        emitLog(message);
}

void FirmwareArchiveController::finishWorker(
    quint64 flow, const std::shared_ptr<JobState> &state,
    const FirmwareArchive::Result &result)
{
    if (m_destroying || flow != m_flow || m_phase != Phase::Worker
        || m_job != state) {
        return;
    }
    ++m_flow;
    const quint64 terminalFlow = m_flow;
    m_phase = Phase::Idle;
    m_job.reset();
    m_destination.clear();
    m_watcher.clear();
    m_progressTimer->stop();
    const QString message = resultSummary(result);
    const QString warning = warningsText(result.warnings);
    if (!dismissProgress() || m_flow != terminalFlow
        || m_phase != Phase::Idle)
        return;
    const QPointer<FirmwareArchiveController> guard(this);
    emit busyChanged(false);
    if (!guard)
        return;
    if (!emitLog(message))
        return;
    if (!warning.isEmpty())
        emitLog(tr("Firmware archive warnings: %1").arg(warning));
}

QString FirmwareArchiveController::resultSummary(
    const FirmwareArchive::Result &result)
{
    QString message;
    if (result.cancelled) {
        message = tr("Firmware archive: cancelled; no completed archive was published.");
    } else if (!result.success) {
        message = tr("Firmware archive failed: %1").arg(
            result.error.isEmpty() ? tr("unknown archive failure") : result.error);
    } else {
        const QString state = result.failedFiles == 0
            ? tr("complete") : tr("published with unavailable files");
        message = tr("Firmware archive %1: %2 downloaded, %3 unavailable, "
                     "%4 bytes, manifest %5. Output: %6")
            .arg(state)
            .arg(result.fileCount)
            .arg(result.failedFiles)
            .arg(result.bytesDownloaded)
            .arg(result.manifestSource.toDisplayString(QUrl::FullyEncoded),
                 result.directory);
    }
    if (!result.retainedStaging.isEmpty()) {
        message += tr(" Cleanup was incomplete. Last known staging path: %1; it may have moved or become unsafe to access.")
            .arg(result.retainedStaging);
    }
    return message;
}

bool FirmwareArchiveController::dismissPrompt()
{
    const QPointer<FirmwareArchiveController> guard(this);
    const QPointer<QDialog> prompt(m_prompt);
    m_prompt.clear();
    if (prompt) {
        disconnect(prompt.data(), nullptr, this, nullptr);
        prompt->blockSignals(true);
        prompt->reject();
        if (prompt)
            prompt->deleteLater();
    }
    return !guard.isNull();
}

bool FirmwareArchiveController::dismissProgress()
{
    const QPointer<FirmwareArchiveController> guard(this);
    const QPointer<QProgressDialog> progress(m_progress);
    m_progress.clear();
    if (progress) {
        disconnect(progress.data(), nullptr, this, nullptr);
        progress->blockSignals(true);
        progress->hide();
        if (progress)
            progress->deleteLater();
    }
    return !guard.isNull();
}

void FirmwareArchiveController::updateProgress()
{
    const std::shared_ptr<JobState> state = m_job;
    if (m_destroying || m_phase != Phase::Worker || !state || !m_progress)
        return;
    if (m_cancelReported
        || state->cancelled.load(std::memory_order_acquire)) {
        return;
    }
    const QPointer<FirmwareArchiveController> guard(this);
    const QPointer<QProgressDialog> progress(m_progress);
    int total = 0;
    int completed = 0;
    QString item;
    {
        QMutexLocker locker(&state->mutex);
        total = qMax(0, state->total.load(std::memory_order_relaxed));
        completed = qBound(0,
            state->completed.load(std::memory_order_relaxed), total);
        item = state->item;
    }
    if (total > 0) {
        progress->setRange(0, total);
        if (!guard || !progress || m_progress != progress
            || m_phase != Phase::Worker || m_job != state
            || m_cancelReported
            || state->cancelled.load(std::memory_order_acquire)) {
            return;
        }
        progress->setValue(completed);
    } else {
        progress->setRange(0, 0);
    }
    if (!guard || !progress || m_progress != progress
        || m_phase != Phase::Worker || m_job != state
        || m_cancelReported
        || state->cancelled.load(std::memory_order_acquire))
        return;
    if (!item.isEmpty()) {
        progress->setLabelText(tr("Firmware archive %1/%2: %3")
                                   .arg(completed).arg(total).arg(item));
    }
    if (!guard || !progress || m_progress != progress
        || m_phase != Phase::Worker || m_job != state
        || m_cancelReported
        || state->cancelled.load(std::memory_order_acquire))
        return;
    if (total <= 0 || completed <= 0)
        return;
    const int interval = qMax(1, total / 20);
    if (completed != 1 && completed != total
        && completed - m_lastReportedProgress < interval) {
        return;
    }
    m_lastReportedProgress = completed;
    emitLog(tr("Firmware archive: %1/%2 — %3")
                .arg(completed).arg(total).arg(item));
}
