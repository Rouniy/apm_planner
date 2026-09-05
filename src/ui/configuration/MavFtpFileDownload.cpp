#include "MavFtpFileDownload.h"

#include "comm/MavFtpServiceInterface.h"
#include "comm/VehicleTargetManager.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QProgressDialog>
#include <QSaveFile>
#include <QTimer>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <exception>

namespace {
constexpr int MaximumDownloadBytes = 64 * 1024 * 1024;
constexpr qint64 WriteChunkBytes = 64 * 1024;

struct SaveResult
{
    bool success = false;
    bool cancelled = false;
    qint64 bytesWritten = 0;
    QString error;
};

bool outputStillMatches(const QString &path, const QString &canonicalParent,
                        bool existed, qint64 size,
                        const QDateTime &lastModified, QString *error)
{
    const QFileInfo parentInfo(QFileInfo(path).absolutePath());
    if (parentInfo.isSymLink()
        || parentInfo.canonicalFilePath() != canonicalParent) {
        if (error) *error = QObject::tr("The selected output directory changed identity while the download was active.");
        return false;
    }
    const QFileInfo current(path);
    if (current.isSymLink()) {
        if (error) *error = QObject::tr("The selected output became a symbolic link.");
        return false;
    }
    if (current.exists() != existed) {
        if (error) *error = QObject::tr("The selected output changed while the download was active.");
        return false;
    }
    if (existed && (!current.isFile() || current.size() != size
                    || current.lastModified() != lastModified)) {
        if (error) *error = QObject::tr("The selected output changed while the download was active.");
        return false;
    }
    return true;
}
} // namespace

struct MavFtpFileDownload::SaveState
{
    std::atomic_bool cancelled{false};
    std::atomic<qint64> completed{0};
    qint64 total = 0;
};

MavFtpFileDownload::MavFtpFileDownload(
    MavFtpServiceInterface *service, VehicleTargetManager *targetManager,
    QWidget *dialogParent)
    : QObject(dialogParent)
    , m_service(service)
    , m_targetManager(targetManager)
    , m_dialogParent(dialogParent)
    , m_deadline(new QTimer(this))
{
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout,
            this, &MavFtpFileDownload::handleDeadline);
    if (dialogParent)
        dialogParent->installEventFilter(this);
    if (service) {
        connect(service, &MavFtpServiceInterface::operationFinished,
                this, &MavFtpFileDownload::handleFinished);
        connect(service, &MavFtpServiceInterface::operationProgress,
                this, &MavFtpFileDownload::handleProgress);
        connect(service, &QObject::destroyed, this, [this]() {
            if (busy())
                finish(tr("MAVFTP download failed: the shared service was destroyed."));
        });
    }
    if (targetManager) {
        connect(targetManager, &VehicleTargetManager::targetGenerationChanged,
                this, &MavFtpFileDownload::handleTargetChanged);
        connect(targetManager, &QObject::destroyed, this, [this]() {
            if (busy())
                requestOwnedCancellation(
                    tr("MAVFTP download cancelled: the vehicle target manager was destroyed."));
        });
    }
}

MavFtpFileDownload::~MavFtpFileDownload()
{
    ++m_flow;
    if (m_saveState)
        m_saveState->cancelled.store(true, std::memory_order_release);
    const QPointer<MavFtpServiceInterface> service(m_service);
    const quint64 operationId = m_operationId;
    if (service)
        disconnect(service.data(), nullptr, this, nullptr);
    if (m_targetManager)
        disconnect(m_targetManager, nullptr, this, nullptr);
    if (m_prompt) {
        // Destruction is terminal for this controller.  Suppress every dialog
        // callback before closing it so an observer cannot recursively delete
        // the parent that is already tearing us down.
        const QPointer<QDialog> prompt(m_prompt);
        disconnect(prompt.data(), nullptr, nullptr, nullptr);
        prompt->reject();
        if (prompt)
            prompt->deleteLater();
    }
    if (m_progress) {
        const QPointer<QProgressDialog> progress(m_progress);
        disconnect(progress.data(), nullptr, nullptr, nullptr);
        progress->cancel();
        if (progress)
            progress->deleteLater();
    }
    if (service && operationId != 0)
        service->cancelOperation(operationId);
}

void MavFtpFileDownload::setTimeoutForTesting(int timeoutMs)
{
    if (busy() || timeoutMs <= 0)
        return;
    m_timeoutMs = timeoutMs;
}

bool MavFtpFileDownload::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_dialogParent && event && event->type() == QEvent::Close
        && busy()) {
        const QPointer<MavFtpFileDownload> guard(this);
        requestOwnedCancellation(
            tr("MAVFTP download cancelled because the Developer Tools page closed."));
        if (!guard)
            return false;
    }
    return QObject::eventFilter(watched, event);
}

bool MavFtpFileDownload::emitLog(const QString &message)
{
    const QPointer<MavFtpFileDownload> guard(this);
    emit logMessage(message);
    return !guard.isNull();
}

void MavFtpFileDownload::start()
{
    if (busy()) {
        emitLog(tr("MAVFTP download: this controller is already busy."));
        return;
    }
    if (!m_service || !m_targetManager) {
        emitLog(tr("MAVFTP download is unavailable: the service or target manager is missing."));
        return;
    }
    if (m_service->isBusy()) {
        emitLog(tr("MAVFTP download is unavailable while another MAVFTP operation is active."));
        return;
    }
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid() || !m_targetManager->isTargetGenerationSettled()
        || !m_targetManager->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation)) {
        emitLog(tr("MAVFTP download requires a stable selected vehicle target."));
        return;
    }

    m_target = target;
    m_remotePath.clear();
    m_output = OutputSnapshot();
    m_cancelReason.clear();
    m_operationId = 0;
    m_phase = Phase::PathPrompt;
    ++m_flow;
    if (m_flow == 0) ++m_flow;
    const quint64 flow = m_flow;
    const QPointer<MavFtpFileDownload> guard(this);
    emit busyChanged();
    if (!guard || flow != m_flow || m_phase != Phase::PathPrompt)
        return;
    openPathPrompt(flow);
}

bool MavFtpFileDownload::pinnedTargetIsCurrent() const
{
    return m_targetManager && m_target.isValid()
        && m_targetManager->isTargetGenerationSettled()
        && m_targetManager->isCurrentTarget(
            m_target.endpoint.linkId, m_target.endpoint.systemId,
            m_target.endpoint.componentId, m_target.generation);
}

void MavFtpFileDownload::openPathPrompt(quint64 flow)
{
    if (flow != m_flow || m_phase != Phase::PathPrompt)
        return;
    if (!pinnedTargetIsCurrent()) {
        finish(tr("MAVFTP download cancelled: the selected vehicle changed before path entry."));
        return;
    }
    auto *dialog = new QInputDialog(m_dialogParent);
    dialog->setObjectName(QStringLiteral("DeveloperMavFtpPathDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Download MAVFTP File"));
    dialog->setLabelText(tr("Remote path"));
    dialog->setInputMode(QInputDialog::TextInput);
    dialog->setTextValue(QStringLiteral("@SYS/threads.txt"));
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (flow != m_flow || m_phase != Phase::PathPrompt)
            return;
        m_prompt.clear();
        if (result != QDialog::Accepted) {
            finish(tr("MAVFTP download cancelled before a remote path was selected."));
            return;
        }
        if (!pinnedTargetIsCurrent()) {
            finish(tr("MAVFTP download cancelled: the selected vehicle changed while the path dialog was open."));
            return;
        }
        const QString path = dialog->textValue();
        if (path.trimmed().isEmpty()) {
            finish(tr("MAVFTP download failed: the remote path is empty."));
            return;
        }
        m_remotePath = path;
        m_phase = Phase::OutputPrompt;
        openOutputPrompt(flow);
    });
    dialog->open();
}

void MavFtpFileDownload::openOutputPrompt(quint64 flow)
{
    if (flow != m_flow || m_phase != Phase::OutputPrompt)
        return;
    if (!pinnedTargetIsCurrent()) {
        finish(tr("MAVFTP download cancelled: the selected vehicle changed before output selection."));
        return;
    }
    QString remote = m_remotePath;
    remote.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QString leaf = remote.section(QLatin1Char('/'), -1);
    if (leaf.isEmpty() || leaf == QStringLiteral(".") || leaf == QStringLiteral(".."))
        leaf = QStringLiteral("mavftp-download.bin");

    auto *dialog = new QFileDialog(m_dialogParent, tr("Save MAVFTP file"));
    dialog->setObjectName(QStringLiteral("DeveloperMavFtpOutputDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setOption(QFileDialog::DontConfirmOverwrite, false);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setNameFilter(tr("All files (*)"));
    dialog->selectFile(leaf);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, flow](int result) {
        if (flow != m_flow || m_phase != Phase::OutputPrompt)
            return;
        m_prompt.clear();
        const QStringList selected = dialog->selectedFiles();
        if (result != QDialog::Accepted || selected.size() != 1) {
            finish(tr("MAVFTP download cancelled before an output file was selected."));
            return;
        }
        if (!pinnedTargetIsCurrent()) {
            finish(tr("MAVFTP download cancelled: the selected vehicle changed while the Save dialog was open."));
            return;
        }
        QString error;
        if (!captureOutput(selected.first(), &m_output, &error)) {
            finish(tr("MAVFTP download failed: %1").arg(error));
            return;
        }
        beginRemoteTransfer(flow);
    });
    dialog->open();
}

bool MavFtpFileDownload::captureOutput(
    const QString &selected, OutputSnapshot *snapshot, QString *error) const
{
    if (!snapshot || selected.trimmed().isEmpty()) {
        if (error) *error = tr("the output path is empty.");
        return false;
    }
    const QFileInfo requested(selected);
    const QString leaf = requested.fileName();
    if (leaf.isEmpty() || leaf == QStringLiteral(".") || leaf == QStringLiteral("..")) {
        if (error) *error = tr("the output path does not name a file.");
        return false;
    }
    const QFileInfo parentInfo(requested.absolutePath());
    const QString canonicalParent = parentInfo.canonicalFilePath();
    if (canonicalParent.isEmpty() || !QFileInfo(canonicalParent).isDir()) {
        if (error) *error = tr("the output directory does not exist or cannot be resolved safely.");
        return false;
    }
    const QString path = QDir(canonicalParent).absoluteFilePath(leaf);
    const QFileInfo output(path);
    if (output.isSymLink()) {
        if (error) *error = tr("symbolic-link output files are not accepted.");
        return false;
    }
    if (output.exists() && !output.isFile()) {
        if (error) *error = tr("the selected output is not a regular file.");
        return false;
    }
    snapshot->path = path;
    snapshot->canonicalParent = canonicalParent;
    snapshot->existed = output.exists();
    snapshot->size = output.exists() ? output.size() : -1;
    snapshot->lastModified = output.exists() ? output.lastModified() : QDateTime();
    return true;
}

void MavFtpFileDownload::beginRemoteTransfer(quint64 flow)
{
    if (flow != m_flow || m_phase != Phase::OutputPrompt)
        return;
    if (!m_service || !pinnedTargetIsCurrent()) {
        finish(tr("MAVFTP download cancelled: the selected vehicle changed before transfer admission."));
        return;
    }
    auto *progress = new QProgressDialog(
        tr("Downloading %1…").arg(m_remotePath), tr("Cancel"),
        0, 1000, m_dialogParent);
    progress->setObjectName(QStringLiteral("DeveloperMavFtpProgressDialog"));
    progress->setWindowTitle(tr("Download MAVFTP File"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    m_progress = progress;
    connect(progress, &QProgressDialog::canceled,
            this, &MavFtpFileDownload::cancel);

    m_phase = Phase::Starting;
    // Include synchronous admission callbacks in the same 30-second remote
    // deadline.  Any terminal path below stops the timer.
    m_deadline->start(m_timeoutMs);
    const QPointer<MavFtpFileDownload> guard(this);
    const QPointer<MavFtpServiceInterface> service(m_service);
    const auto result = service->startDownloadForTarget(
        m_remotePath, m_target, &m_operationId);
    if (!guard || flow != m_flow)
        return;
    if (m_phase != Phase::Starting) {
        // A target/service callback may have ended this flow before start()
        // returned.  Cancel only the operation whose token was written into
        // our member; never call the service's unscoped cancel().
        if (result == MavFtpServiceInterface::StartResult::Started
            && service && m_operationId != 0
            && service->activeOperationId() == m_operationId) {
            service->cancelOperation(m_operationId);
        }
        return;
    }
    if (result != MavFtpServiceInterface::StartResult::Started
        || m_operationId == 0) {
        const QString detail = result == MavFtpServiceInterface::StartResult::Started
            ? tr("the service did not publish an ownership token.")
            : startFailureText(result);
        finish(tr("MAVFTP download could not start: %1").arg(detail));
        return;
    }
    if (!service || service->activeOperationId() != m_operationId
        || !pinnedTargetIsCurrent()) {
        requestOwnedCancellation(
            tr("MAVFTP download cancelled: its target or service operation changed during admission."));
        return;
    }
    m_phase = Phase::RemoteTransfer;
    const QPointer<MavFtpFileDownload> showGuard(this);
    progress->show();
    if (!showGuard || flow != m_flow || m_phase != Phase::RemoteTransfer)
        return;
    emitLog(tr("MAVFTP download started for %1 from %2:%3 on %4.")
                .arg(m_remotePath)
                .arg(m_target.endpoint.systemId)
                .arg(m_target.endpoint.componentId)
                .arg(m_target.endpoint.linkName));
}

void MavFtpFileDownload::handleProgress(
    qulonglong operationId, qulonglong generation,
    qint64 completed, qint64 total)
{
    if (m_phase != Phase::RemoteTransfer || operationId != m_operationId
        || generation != m_target.generation || !m_progress)
        return;
    if (total > 0) {
        m_progress->setRange(0, 1000);
        m_progress->setValue(static_cast<int>(qBound<qint64>(
            0, completed * 1000 / total, 1000)));
    } else {
        m_progress->setRange(0, 0);
    }
}

void MavFtpFileDownload::handleFinished(
    const MavFtpServiceInterface::Result &result)
{
    if ((m_phase != Phase::Starting && m_phase != Phase::RemoteTransfer)
        || m_operationId == 0 || result.operationId != m_operationId)
        return;
    if (result.operation != MavFtpServiceInterface::Operation::Download
        || result.targetGeneration != m_target.generation
        || result.remotePath != m_remotePath) {
        finish(tr("MAVFTP download failed: the owned terminal result did not match its immutable request."));
        return;
    }
    m_deadline->stop();
    m_operationId = 0;
    if (!m_cancelReason.isEmpty()) {
        finish(m_cancelReason);
        return;
    }
    if (result.cancelled) {
        finish(m_cancelReason.isEmpty()
            ? tr("MAVFTP download cancelled; no output was published.")
            : m_cancelReason);
        return;
    }
    if (!result.error.isEmpty()) {
        finish(tr("MAVFTP download failed: %1").arg(result.error));
        return;
    }
    if (!pinnedTargetIsCurrent()) {
        finish(tr("MAVFTP download cancelled: the selected vehicle changed before local publication."));
        return;
    }
    if (result.data.size() > MaximumDownloadBytes) {
        finish(tr("MAVFTP download failed: the received file exceeds the 64 MiB safety limit."));
        return;
    }
    beginLocalWrite(result.data);
}

void MavFtpFileDownload::beginLocalWrite(const QByteArray &data)
{
    m_phase = Phase::LocalWrite;
    const auto state = std::make_shared<SaveState>();
    state->total = data.size();
    m_saveState = state;
    if (m_progress) {
        m_progress->setLabelText(tr("Saving the downloaded file atomically…"));
        m_progress->setRange(0, 1000);
        m_progress->setValue(0);
    }
    const OutputSnapshot output = m_output;
    auto *watcher = new QFutureWatcher<SaveResult>(this);
    auto *timer = new QTimer(watcher);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this, state]() {
        if (m_phase != Phase::LocalWrite || m_saveState != state || !m_progress)
            return;
        const qint64 total = state->total;
        const qint64 completed = state->completed.load(std::memory_order_acquire);
        m_progress->setValue(total > 0
            ? static_cast<int>(qBound<qint64>(0, completed * 1000 / total, 1000))
            : 1000);
    });
    connect(watcher, &QFutureWatcher<SaveResult>::finished, this,
            [this, watcher, timer, state, output]() {
        timer->stop();
        const SaveResult result = watcher->result();
        watcher->deleteLater();
        if (m_phase != Phase::LocalWrite || m_saveState != state)
            return;
        m_saveState.reset();
        if (result.cancelled)
            finish(m_cancelReason.isEmpty()
                ? tr("MAVFTP download cancelled; no output was published.")
                : m_cancelReason);
        else if (!result.success)
            finish(tr("MAVFTP download failed while saving: %1").arg(result.error));
        else
            finish(tr("MAVFTP download completed: %1 bytes written to %2.")
                       .arg(result.bytesWritten).arg(output.path));
    });
    timer->start();
    watcher->setFuture(QtConcurrent::run([data, output, state]() {
        SaveResult result;
        try {
            if (state->cancelled.load(std::memory_order_acquire)) {
                result.cancelled = true;
                return result;
            }
            if (!outputStillMatches(output.path, output.canonicalParent,
                                    output.existed, output.size,
                                    output.lastModified, &result.error))
                return result;
            QSaveFile file(output.path);
            file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly)) {
                result.error = QObject::tr("Could not create %1: %2")
                                   .arg(output.path, file.errorString());
                return result;
            }
            qint64 offset = 0;
            while (offset < data.size()) {
                if (state->cancelled.load(std::memory_order_acquire)) {
                    file.cancelWriting();
                    result.cancelled = true;
                    return result;
                }
                const qint64 count = qMin<qint64>(WriteChunkBytes,
                                                  data.size() - offset);
                const qint64 written = file.write(data.constData() + offset, count);
                if (written != count) {
                    result.error = QObject::tr("Could not write %1: %2")
                                       .arg(output.path, file.errorString());
                    file.cancelWriting();
                    return result;
                }
                offset += written;
                state->completed.store(offset, std::memory_order_release);
            }
            if (state->cancelled.load(std::memory_order_acquire)) {
                file.cancelWriting();
                result.cancelled = true;
                return result;
            }
            if (!outputStillMatches(output.path, output.canonicalParent,
                                    output.existed, output.size,
                                    output.lastModified, &result.error)) {
                file.cancelWriting();
                return result;
            }
            if (!file.commit()) {
                result.error = QObject::tr("Could not commit %1: %2")
                                   .arg(output.path, file.errorString());
                return result;
            }
            result.success = true;
            result.bytesWritten = data.size();
            state->completed.store(data.size(), std::memory_order_release);
        } catch (const std::exception &error) {
            result.error = QString::fromUtf8(error.what());
        } catch (...) {
            result.error = QObject::tr("Unexpected local file error.");
        }
        return result;
    }));
}

void MavFtpFileDownload::cancel()
{
    requestOwnedCancellation(tr("MAVFTP download cancelled; no output was published."));
}

void MavFtpFileDownload::handleTargetChanged()
{
    if (busy() && !pinnedTargetIsCurrent())
        requestOwnedCancellation(
            tr("MAVFTP download cancelled because the selected vehicle changed."));
}

void MavFtpFileDownload::handleDeadline()
{
    if (m_phase == Phase::Starting || m_phase == Phase::RemoteTransfer)
        requestOwnedCancellation(
            tr("MAVFTP download timed out after %1 seconds; no output was published.")
                .arg(m_timeoutMs / 1000.0, 0, 'f', m_timeoutMs % 1000 ? 1 : 0));
}

void MavFtpFileDownload::requestOwnedCancellation(const QString &reason)
{
    if (!busy())
        return;
    m_cancelReason = reason;
    if (m_phase == Phase::PathPrompt || m_phase == Phase::OutputPrompt) {
        finish(reason);
        return;
    }
    if (m_phase == Phase::LocalWrite) {
        if (m_saveState)
            m_saveState->cancelled.store(true, std::memory_order_release);
        return;
    }
    if ((m_phase == Phase::Starting || m_phase == Phase::RemoteTransfer)
        && m_operationId != 0) {
        const quint64 operationId = m_operationId;
        const QPointer<MavFtpFileDownload> guard(this);
        const QPointer<MavFtpServiceInterface> service(m_service);
        const bool accepted = service && service->cancelOperation(operationId);
        if (!guard)
            return;
        if (!accepted && busy() && m_operationId == operationId)
            finish(reason + tr(" The owned service operation was no longer active."));
        return;
    }
    finish(reason);
}

void MavFtpFileDownload::finish(QString message)
{
    if (!busy())
        return;
    const quint64 finishingFlow = ++m_flow;
    m_deadline->stop();
    const QPointer<MavFtpFileDownload> guard(this);
    discardPrompt();
    if (!guard || m_flow != finishingFlow)
        return;
    discardProgress();
    if (!guard || m_flow != finishingFlow)
        return;
    if (m_saveState)
        m_saveState->cancelled.store(true, std::memory_order_release);
    m_saveState.reset();
    m_phase = Phase::Idle;
    m_operationId = 0;
    m_target = VehicleTargetLease();
    m_remotePath.clear();
    m_output = OutputSnapshot();
    m_cancelReason.clear();
    emit logMessage(message);
    if (!guard || m_flow != finishingFlow)
        return;
    emit busyChanged();
}

void MavFtpFileDownload::discardPrompt()
{
    const QPointer<QDialog> prompt(m_prompt);
    m_prompt.clear();
    if (prompt) {
        disconnect(prompt.data(), nullptr, this, nullptr);
        prompt->reject();
        if (prompt)
            prompt->deleteLater();
    }
}

void MavFtpFileDownload::discardProgress()
{
    const QPointer<QProgressDialog> progress(m_progress);
    m_progress.clear();
    if (progress) {
        disconnect(progress.data(), nullptr, this, nullptr);
        progress->cancel();
        if (progress)
            progress->deleteLater();
    }
}

QString MavFtpFileDownload::startFailureText(
    MavFtpServiceInterface::StartResult result)
{
    switch (result) {
    case MavFtpServiceInterface::StartResult::Started:
        return tr("the operation was not admitted.");
    case MavFtpServiceInterface::StartResult::Busy:
        return tr("another MAVFTP operation is active.");
    case MavFtpServiceInterface::StartResult::NoTarget:
        return tr("there is no selected exact vehicle target.");
    case MavFtpServiceInterface::StartResult::StaleTarget:
        return tr("the selected vehicle changed before admission.");
    case MavFtpServiceInterface::StartResult::InvalidPath:
        return tr("the remote path is invalid or too long.");
    case MavFtpServiceInterface::StartResult::InvalidData:
        return tr("the transfer data is invalid.");
    case MavFtpServiceInterface::StartResult::TransportUnavailable:
        return tr("the exact MAVLink transport is unavailable.");
    case MavFtpServiceInterface::StartResult::ShuttingDown:
        return tr("the MAVFTP service is shutting down.");
    }
    return tr("unknown start failure.");
}
