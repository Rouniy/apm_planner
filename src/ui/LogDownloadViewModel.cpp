#include "LogDownloadViewModel.h"

#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace {

bool sameTarget(const SwarmVehicleInstanceLease &left,
                const SwarmVehicleInstanceLease &right)
{
    return left.isValid() && right.isValid() && left.sameInstance(right);
}

QString sourceText(const QString &source)
{
    return source.trimmed().isEmpty()
        ? QString() : QStringLiteral(" [%1]").arg(source.trimmed());
}

} // namespace

QString LogDownloadRow::timeText() const
{
    if (!timeUtc.isValid()) {
        return QString::fromUtf8("—");
    }
    return timeUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString LogDownloadRow::sizeText() const
{
    if (sizeBytes >= 1024U * 1024U) {
        return QStringLiteral("%1 MB")
            .arg(double(sizeBytes) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 KB")
        .arg(double(sizeBytes) / 1024.0, 0, 'f', 1);
}

LogDownloadViewModel::LogDownloadViewModel(
    ExactLogTransferService *service,
    TargetAcquirer acquireTarget,
    KmlExporter kmlExporter,
    QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_acquireTarget(std::move(acquireTarget))
    , m_kmlExporter(std::move(kmlExporter))
{
    if (!m_kmlExporter) {
        m_kmlExporter = [](
            const QString &input, const QString &output,
            const DataFlashKmlExporter::CancellationCheck &cancelled) {
            return DataFlashKmlExporter::Export(input, output, cancelled);
        };
    }

    if (m_service) {
        connect(m_service, &ExactLogTransferService::busyChanged,
                this, [this](bool) { notifyState(); });
        connect(m_service, &ExactLogTransferService::progress,
                this, &LogDownloadViewModel::handleProgress);
        connect(m_service, &ExactLogTransferService::transferFinished,
                this, &LogDownloadViewModel::handleTransferFinished);
        connect(m_service, &QObject::destroyed, this, [this]() {
            m_service = nullptr;
            if (m_batch.active) {
                finishBatch(tr("Download failed: transfer service stopped."),
                            false);
            } else if (m_activeToken.isValid()) {
                m_activeToken = {};
                m_activeOperation = ExactLogTransferService::Operation::None;
                m_pinnedTargetSource.clear();
                setStatus(tr("Log operation failed: transfer service stopped."));
            }
            notifyState();
        });
    }
}

LogDownloadViewModel::~LogDownloadViewModel()
{
    shutdown();

    QThread *const worker = m_kmlThread.data();
    if (!worker) {
        return;
    }
    worker->requestInterruption();
    // The exporter polls our shared flag between records. Join it completely:
    // orphaning a running QThread would make window close/process exit unsafe.
    while (!worker->wait(100)) {
        worker->requestInterruption();
    }
    disconnect(worker, nullptr, this, nullptr);
    m_kmlThread = nullptr;
    delete worker;
}

void LogDownloadViewModel::setSelectedLogId(int id)
{
    if (m_selectedLogId == id) {
        return;
    }
    m_selectedLogId = id;
    notifyState();
}

bool LogDownloadViewModel::isBusy() const
{
    return m_preparation.isValid() || m_batch.active || m_kmlThread
        || (m_service && m_service->busy());
}

QString LogDownloadViewModel::targetSource() const
{
    if (m_preparation.isValid() || m_batch.active
        || m_activeToken.isValid() || m_kmlThread) {
        return m_pinnedTargetSource;
    }
    return m_liveTargetSource;
}

void LogDownloadViewModel::setTargetSource(const QString &source)
{
    const QString normalized = source.trimmed();
    if (m_liveTargetSource == normalized) {
        return;
    }
    const QString previousVisible = targetSource();
    m_liveTargetSource = normalized;
    if (targetSource() != previousVisible) {
        notifyState();
    }
}

void LogDownloadViewModel::setCreateKmlAfterDownload(bool enabled)
{
    if (m_createKml == enabled) {
        return;
    }
    m_createKml = enabled;
    notifyState();
}

bool LogDownloadViewModel::refresh()
{
    if (m_shuttingDown || isBusy()) {
        return false;
    }
    if (!m_service || !m_acquireTarget) {
        setStatus(tr("Not connected."));
        m_refreshForDownloadAll = false;
        return false;
    }

    const SwarmVehicleInstanceLease target = m_acquireTarget();
    if (!target.isValid()) {
        setStatus(tr("Not connected."));
        m_refreshForDownloadAll = false;
        return false;
    }

    m_logs.clear();
    m_selectedLogId = -1;
    m_listTarget = {};
    emit logsChanged();
    m_pinnedTargetSource = m_liveTargetSource;
    setProgress(0.0);
    setStatus(tr("Requesting log list…") + sourceSuffix());
    return startList(target);
}

void LogDownloadViewModel::requestDownloadAll()
{
    if (m_shuttingDown || isBusy()) {
        return;
    }
    if (!m_logs.isEmpty()) {
        emit downloadAllDestinationRequested();
        return;
    }
    m_refreshForDownloadAll = true;
    if (!refresh()) {
        m_refreshForDownloadAll = false;
    }
}

quint64 LogDownloadViewModel::prepareSelectedDownload(
    QString *suggestedName)
{
    const LogDownloadRow *const row = selectedRow();
    if (!row) {
        setStatus(tr("Select a log first."));
        return 0;
    }
    const quint64 id = prepare(
        PreparationKind::SelectedDownload, {*row});
    if (id != 0 && suggestedName) {
        *suggestedName = QStringLiteral("log_%1.bin").arg(row->id);
    }
    return id;
}

quint64 LogDownloadViewModel::prepareDownloadAll()
{
    if (m_logs.isEmpty()) {
        setStatus(tr("No logs to download."));
        return 0;
    }
    return prepare(PreparationKind::DownloadAll, m_logs);
}

quint64 LogDownloadViewModel::prepareErase()
{
    return prepare(PreparationKind::Erase, {});
}

QStringList LogDownloadViewModel::existingDownloadAllFiles(
    quint64 preparationId, const QString &directory) const
{
    QStringList result;
    if (!m_preparation.isValid()
        || m_preparation.id != preparationId
        || m_preparation.kind != PreparationKind::DownloadAll
        || directory.trimmed().isEmpty()) {
        return result;
    }

    const QDir folder(directory);
    for (const LogDownloadRow &row : m_preparation.rows) {
        const QString path = folder.filePath(suggestedFileName(row));
        if (QFileInfo::exists(path)) {
            result.append(QDir::cleanPath(path));
        }
        if (m_preparation.createKml) {
            const QString kmlPath = kmlFileName(path);
            if (QFileInfo::exists(kmlPath)) {
                result.append(QDir::cleanPath(kmlPath));
            }
        }
    }
    return result;
}

bool LogDownloadViewModel::startPreparedSelectedDownload(
    quint64 preparationId, const QString &destination,
    bool overwriteKmlConfirmed)
{
    if (!validatePreparation(
            preparationId, PreparationKind::SelectedDownload)) {
        return false;
    }
    if (destination.trimmed().isEmpty()) {
        abandonPreparation(preparationId);
        return false;
    }
    const Preparation prepared = m_preparation;
    if (!revalidateTarget(prepared.target,
                          tr("selecting a download destination"))) {
        clearPreparation();
        return false;
    }

    const QFileInfo destinationInfo(destination);
    if (destinationInfo.exists() && destinationInfo.isDir()) {
        setStatus(tr("Download destination is a directory."));
        clearPreparation();
        return false;
    }
    if (prepared.createKml && QFileInfo::exists(kmlFileName(destination))
        && !overwriteKmlConfirmed) {
        setStatus(tr("Download canceled; the existing KML file was not "
                     "overwritten."));
        clearPreparation();
        return false;
    }
    if (!QDir().mkpath(destinationInfo.absolutePath())) {
        setStatus(tr("Could not create the download folder."));
        clearPreparation();
        return false;
    }

    m_batch = {};
    m_batch.active = true;
    m_batch.target = prepared.target;
    m_batch.rows = prepared.rows;
    m_batch.destinations = QStringList{QDir::cleanPath(destination)};
    m_batch.source = prepared.source;
    m_batch.createKml = prepared.createKml;
    m_batch.totalBytes = prepared.rows.first().sizeBytes;
    clearPreparation();
    notifyState();
    return startNextDownload();
}

bool LogDownloadViewModel::startPreparedDownloadAll(
    quint64 preparationId, const QString &directory,
    bool overwriteConfirmed)
{
    if (!validatePreparation(preparationId, PreparationKind::DownloadAll)) {
        return false;
    }
    if (directory.trimmed().isEmpty()) {
        abandonPreparation(preparationId);
        return false;
    }
    if (!overwriteConfirmed
        && !existingDownloadAllFiles(preparationId, directory).isEmpty()) {
        setStatus(tr("Download canceled; existing files were not overwritten."));
        clearPreparation();
        return false;
    }

    const Preparation prepared = m_preparation;
    if (!revalidateTarget(prepared.target,
                          tr("selecting a download folder"))) {
        clearPreparation();
        return false;
    }
    const QFileInfo folderInfo(directory);
    if ((folderInfo.exists() && !folderInfo.isDir())
        || !QDir().mkpath(folderInfo.absoluteFilePath())) {
        setStatus(tr("Could not create the download folder."));
        clearPreparation();
        return false;
    }

    const QDir folder(folderInfo.absoluteFilePath());
    QStringList destinations;
    quint64 total = 0;
    for (const LogDownloadRow &row : prepared.rows) {
        destinations.append(
            QDir::cleanPath(folder.filePath(suggestedFileName(row))));
        total += row.sizeBytes;
    }

    m_batch = {};
    m_batch.active = true;
    m_batch.target = prepared.target;
    m_batch.rows = prepared.rows;
    m_batch.destinations = destinations;
    m_batch.source = prepared.source;
    m_batch.createKml = prepared.createKml;
    m_batch.totalBytes = total;
    clearPreparation();
    notifyState();
    return startNextDownload();
}

bool LogDownloadViewModel::completePreparedErase(
    quint64 preparationId, bool confirmed)
{
    if (!validatePreparation(preparationId, PreparationKind::Erase)) {
        return false;
    }
    if (!confirmed) {
        abandonPreparation(preparationId);
        return false;
    }
    const Preparation prepared = m_preparation;
    if (!revalidateTarget(prepared.target, tr("confirming log erase"))) {
        clearPreparation();
        return false;
    }
    clearPreparation();
    m_pinnedTargetSource = prepared.source;
    setStatus(tr("Sending erase request…") + sourceSuffix());
    return startErase(prepared.target);
}

void LogDownloadViewModel::abandonPreparation(quint64 preparationId)
{
    if (!m_preparation.isValid()
        || m_preparation.id != preparationId) {
        return;
    }
    clearPreparation();
}

void LogDownloadViewModel::cancelOwnDownload()
{
    if (!m_batch.active) {
        return;
    }
    m_batch.cancelRequested = true;
    if (m_kmlCancel) {
        m_kmlCancel->store(true);
    }
    if (m_activeToken.isValid() && m_service) {
        m_service->cancel(m_activeToken, tr("Canceled by this log window."));
    } else if (!m_kmlThread) {
        finishBatch(tr("Download canceled after %1/%2.")
                        .arg(m_batch.saved)
                        .arg(m_batch.rows.size()), false);
    }
    notifyState();
}

void LogDownloadViewModel::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    clearPreparation();
    if (m_batch.active) {
        m_batch.cancelRequested = true;
    }
    if (m_kmlCancel) {
        m_kmlCancel->store(true);
    }
    if (m_activeToken.isValid() && m_service) {
        m_service->cancel(m_activeToken, tr("Log window closed."));
    }
    m_activeToken = {};
    m_activeOperation = ExactLogTransferService::Operation::None;
    m_batch.active = false;
}

QString LogDownloadViewModel::suggestedFileName(const LogDownloadRow &row)
{
    if (!row.timeUtc.isValid()) {
        return QStringLiteral("log_%1.bin").arg(row.id);
    }
    return QStringLiteral("%1_%2.bin")
        .arg(row.timeUtc.toLocalTime().toString(
                 QStringLiteral("yyyy-MM-dd HH-mm-ss")))
        .arg(row.id);
}

QString LogDownloadViewModel::kmlFileName(const QString &logFileName)
{
    const QFileInfo info(logFileName);
    return QDir(info.absolutePath()).filePath(
        info.completeBaseName() + QStringLiteral(".kml"));
}

quint64 LogDownloadViewModel::prepare(
    PreparationKind kind, const QVector<LogDownloadRow> &rows)
{
    if (m_shuttingDown || isBusy()) {
        return 0;
    }
    if (!m_service || !m_acquireTarget) {
        setStatus(tr("Not connected."));
        return 0;
    }
    const SwarmVehicleInstanceLease target = m_acquireTarget();
    if (!target.isValid()) {
        discardStaleListForTarget(target);
        setStatus(tr("Not connected."));
        return 0;
    }
    const bool staleList = discardStaleListForTarget(target);
    if (staleList
        && (kind == PreparationKind::SelectedDownload
            || kind == PreparationKind::DownloadAll)) {
        setStatus(tr("The log list belongs to a previous vehicle or physical "
                     "link. Refresh List before downloading."));
        return 0;
    }

    ++m_nextPreparationId;
    if (m_nextPreparationId == 0) {
        ++m_nextPreparationId;
    }
    m_preparation.id = m_nextPreparationId;
    m_preparation.kind = kind;
    m_preparation.target = target;
    m_preparation.rows = rows;
    m_preparation.source = m_liveTargetSource;
    m_preparation.createKml = m_createKml;
    m_pinnedTargetSource = m_preparation.source;
    notifyState();
    return m_preparation.id;
}

bool LogDownloadViewModel::discardStaleListForTarget(
    const SwarmVehicleInstanceLease &target)
{
    if (m_logs.isEmpty()
        || (m_listTarget.isValid() && sameTarget(m_listTarget, target))) {
        return false;
    }
    m_logs.clear();
    m_selectedLogId = -1;
    m_listTarget = {};
    emit logsChanged();
    notifyState();
    return true;
}

bool LogDownloadViewModel::validatePreparation(
    quint64 id, PreparationKind kind)
{
    if (!m_preparation.isValid()
        || m_preparation.id != id || m_preparation.kind != kind) {
        setStatus(tr("The pending log action is no longer current."));
        return false;
    }
    return true;
}

bool LogDownloadViewModel::revalidateTarget(
    const SwarmVehicleInstanceLease &target, const QString &context)
{
    const SwarmVehicleInstanceLease current = m_acquireTarget
        ? m_acquireTarget() : SwarmVehicleInstanceLease{};
    if (sameTarget(target, current)) {
        return true;
    }
    discardStaleListForTarget(current);
    setStatus(tr("Vehicle or physical link changed while %1; action canceled.")
                  .arg(context));
    return false;
}

void LogDownloadViewModel::clearPreparation()
{
    if (!m_preparation.isValid()) {
        return;
    }
    m_preparation = {};
    if (!m_batch.active && !m_activeToken.isValid() && !m_kmlThread) {
        m_pinnedTargetSource.clear();
    }
    notifyState();
}

bool LogDownloadViewModel::startList(
    const SwarmVehicleInstanceLease &target)
{
    return startServiceOperation(
        ExactLogTransferService::Operation::List,
        tr("Could not request the log list"),
        [this, target](ExactLogTransferToken *token, QString *error) {
            return m_service->requestList(this, target, token, error);
        });
}

bool LogDownloadViewModel::startErase(
    const SwarmVehicleInstanceLease &target)
{
    return startServiceOperation(
        ExactLogTransferService::Operation::Erase,
        tr("Could not send the erase request"),
        [this, target](ExactLogTransferToken *token, QString *error) {
            return m_service->erase(this, target, token, error);
        });
}

bool LogDownloadViewModel::startNextDownload()
{
    if (!m_batch.active || m_shuttingDown) {
        return false;
    }
    if (m_batch.cancelRequested) {
        finishBatch(tr("Download canceled after %1/%2.")
                        .arg(m_batch.saved)
                        .arg(m_batch.rows.size()), false);
        return false;
    }
    if (m_batch.nextIndex >= m_batch.rows.size()) {
        QString completed = tr("Downloaded %1 log(s).").arg(m_batch.saved);
        if (m_batch.createKml) {
            completed += m_batch.kmlErrors.isEmpty()
                ? tr(" KML track(s) created.")
                : tr(" KML failed for %1: %2")
                      .arg(m_batch.kmlErrors.size())
                      .arg(m_batch.kmlErrors.join(QStringLiteral("; ")));
        }
        finishBatch(completed, true);
        return true;
    }
    if (!revalidateTarget(m_batch.target, tr("downloading logs"))) {
        finishBatch(tr("Download canceled after %1/%2 because the vehicle "
                       "or physical link changed.")
                        .arg(m_batch.saved)
                        .arg(m_batch.rows.size()), false);
        return false;
    }

    const LogDownloadRow row = m_batch.rows.at(m_batch.nextIndex);
    const QString destination =
        m_batch.destinations.at(m_batch.nextIndex);
    setStatus(tr("Downloading log %1 (%2/%3)…")
                  .arg(row.id)
                  .arg(m_batch.nextIndex + 1)
                  .arg(m_batch.rows.size())
              + sourceText(m_batch.source));

    const bool started = startServiceOperation(
        ExactLogTransferService::Operation::Download,
        tr("Could not start log %1").arg(row.id),
        [this, row, destination](ExactLogTransferToken *token,
                                QString *error) {
            return m_service->startDownload(
                this, m_batch.target, row.id, row.sizeBytes,
                destination, token, error);
        });
    if (!started && m_batch.active) {
        finishBatch(tr("Download failed after %1/%2: %3")
                        .arg(m_batch.saved)
                        .arg(m_batch.rows.size())
                        .arg(m_status), false);
    }
    return started;
}

void LogDownloadViewModel::startKmlExport(
    const QString &input, quint16 logId)
{
    if (!m_batch.active || m_kmlThread || !m_kmlExporter) {
        return;
    }
    const QString output = kmlFileName(input);
    const quint64 generation = ++m_kmlGeneration;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto result = std::make_shared<DataFlashKmlExporter::Result>();
    const KmlExporter exporter = m_kmlExporter;
    m_kmlCancel = cancelled;

    QThread *const worker = QThread::create(
        [exporter, input, output, cancelled, result]() {
            try {
                *result = exporter(input, output, [cancelled]() {
                    return cancelled->load();
                });
            } catch (const std::exception &error) {
                result->error = QString::fromUtf8(error.what());
            } catch (...) {
                result->error = QStringLiteral(
                    "KML exporter raised an unknown exception.");
            }
        });
    m_kmlThread = worker;
    connect(worker, &QThread::finished, this,
            [this, worker, generation, logId, result]() {
        if (m_kmlThread == worker) {
            m_kmlThread = nullptr;
            m_kmlCancel.reset();
        }
        worker->deleteLater();
        handleKmlFinished(generation, logId, *result);
    });
    setStatus(tr("Creating KML for log %1…").arg(logId)
              + sourceText(m_batch.source));
    notifyState();
    worker->start();
}

void LogDownloadViewModel::handleKmlFinished(
    quint64 generation, quint16 logId,
    const DataFlashKmlExporter::Result &result)
{
    if (m_shuttingDown || generation != m_kmlGeneration
        || !m_batch.active) {
        return;
    }
    if (m_batch.cancelRequested || result.cancelled) {
        finishBatch(tr("Download canceled after %1/%2.")
                        .arg(m_batch.saved)
                        .arg(m_batch.rows.size()), false);
        return;
    }
    if (!result.succeeded) {
        m_batch.kmlErrors.append(
            tr("log %1: %2").arg(logId).arg(
                result.error.isEmpty() ? tr("unknown export error")
                                       : result.error));
    }
    startNextDownload();
}

void LogDownloadViewModel::handleTransferFinished(
    const ExactLogTransferResult &result)
{
    if (m_startingService) {
        m_deferredResults.append(result);
        return;
    }
    processTransferFinished(result);
}

void LogDownloadViewModel::processTransferFinished(
    const ExactLogTransferResult &result)
{
    if (!m_activeToken.isValid() || result.token != m_activeToken
        || result.operation != m_activeOperation) {
        return;
    }
    m_activeToken = {};
    const ExactLogTransferService::Operation operation = m_activeOperation;
    m_activeOperation = ExactLogTransferService::Operation::None;

    if (operation == ExactLogTransferService::Operation::List) {
        const bool requestAll = m_refreshForDownloadAll;
        m_refreshForDownloadAll = false;
        if (result.succeeded()) {
            QVector<LogDownloadRow> rows;
            rows.reserve(result.entries.size());
            for (const ExactLogEntry &entry : result.entries) {
                if (entry.size == 0) {
                    continue;
                }
                LogDownloadRow row;
                row.id = entry.id;
                row.sizeBytes = entry.size;
                if (entry.timeUtc != 0) {
                    row.timeUtc = QDateTime::fromSecsSinceEpoch(
                        entry.timeUtc, Qt::UTC);
                }
                rows.append(row);
            }
            std::sort(rows.begin(), rows.end(),
                      [](const LogDownloadRow &left,
                         const LogDownloadRow &right) {
                return left.id < right.id;
            });
            m_logs = rows;
            m_listTarget = result.vehicle;
            m_selectedLogId = -1;
            emit logsChanged();
            setStatus(tr("%1 log(s) on board.").arg(m_logs.size())
                      + sourceText(m_pinnedTargetSource));
            m_pinnedTargetSource.clear();
            notifyState();
            if (requestAll && !m_logs.isEmpty()) {
                // ExactLogTransferService retains its global finishing guard
                // until all transferFinished receivers return.
                QTimer::singleShot(0, this, [this]() {
                    if (!m_shuttingDown && !isBusy()) {
                        emit downloadAllDestinationRequested();
                    }
                });
            }
        } else {
            m_listTarget = {};
            setStatus(tr("List failed: %1").arg(transferError(result))
                      + sourceText(m_pinnedTargetSource));
            m_pinnedTargetSource.clear();
            notifyState();
        }
        return;
    }

    if (operation == ExactLogTransferService::Operation::Erase) {
        if (result.outcome
            == ExactLogTransferService::Outcome::SubmittedUnconfirmed) {
            m_logs.clear();
            m_listTarget = {};
            m_selectedLogId = -1;
            setProgress(0.0);
            emit logsChanged();
            setStatus(tr("Erase request sent twice; MAVLink does not confirm "
                         "completion. Refresh List to verify.")
                      + sourceText(m_pinnedTargetSource));
        } else {
            setStatus(tr("Erase request failed: %1")
                          .arg(transferError(result))
                      + sourceText(m_pinnedTargetSource));
        }
        m_pinnedTargetSource.clear();
        notifyState();
        return;
    }

    if (operation != ExactLogTransferService::Operation::Download
        || !m_batch.active
        || !sameTarget(result.vehicle, m_batch.target)) {
        return;
    }

    const int rowIndex = m_batch.nextIndex;
    const LogDownloadRow row = m_batch.rows.at(rowIndex);
    const QString destination = m_batch.destinations.at(rowIndex);
    if (!result.succeeded()) {
        const QString message = result.outcome
                == ExactLogTransferService::Outcome::Cancelled
            ? tr("Download canceled after %1/%2.")
                  .arg(m_batch.saved).arg(m_batch.rows.size())
            : tr("Download failed after %1/%2: %3")
                  .arg(m_batch.saved).arg(m_batch.rows.size())
                  .arg(transferError(result));
        finishBatch(message, false);
        return;
    }

    m_batch.completedBytes += row.sizeBytes;
    ++m_batch.saved;
    ++m_batch.nextIndex;
    setProgress(m_batch.totalBytes > 0
        ? 100.0 * double(m_batch.completedBytes)
              / double(m_batch.totalBytes)
        : 100.0);
    if (m_batch.cancelRequested) {
        finishBatch(tr("Download canceled after %1/%2.")
                        .arg(m_batch.saved).arg(m_batch.rows.size()), false);
    } else if (m_batch.createKml) {
        startKmlExport(destination, row.id);
    } else {
        // Do not ask the shared service for the next row while it is still in
        // the previous transferFinished emission's finishing guard.
        QTimer::singleShot(0, this, [this]() {
            startNextDownload();
        });
    }
}

void LogDownloadViewModel::handleProgress(
    ExactLogTransferToken token, quint64 completed,
    quint64 total, bool totalKnown)
{
    Q_UNUSED(total)
    Q_UNUSED(totalKnown)
    if (!m_batch.active || token != m_activeToken
        || m_activeOperation
            != ExactLogTransferService::Operation::Download) {
        return;
    }
    const quint64 current = m_batch.completedBytes + completed;
    setProgress(m_batch.totalBytes > 0
        ? 100.0 * double(qMin(current, m_batch.totalBytes))
              / double(m_batch.totalBytes)
        : 0.0);
}

bool LogDownloadViewModel::startServiceOperation(
    ExactLogTransferService::Operation operation,
    const QString &failureContext,
    const std::function<ExactLogTransferService::StartResult(
        ExactLogTransferToken *, QString *)> &start)
{
    if (!m_service || m_shuttingDown || !start) {
        setStatus(failureContext + QStringLiteral(": ")
                  + tr("transfer service unavailable"));
        return false;
    }

    ExactLogTransferToken token;
    QString error;
    m_deferredResults.clear();
    m_startingService = true;
    const ExactLogTransferService::StartResult result = start(&token, &error);
    m_startingService = false;
    if (result != ExactLogTransferService::StartResult::Started) {
        m_deferredResults.clear();
        setStatus(failureContext + QStringLiteral(": ")
                  + (error.isEmpty() ? serviceStartError(result) : error));
        m_activeToken = {};
        m_activeOperation = ExactLogTransferService::Operation::None;
        notifyState();
        return false;
    }

    m_activeToken = token;
    m_activeOperation = operation;
    const QVector<ExactLogTransferResult> deferred =
        std::exchange(m_deferredResults, {});
    notifyState();
    for (const ExactLogTransferResult &terminal : deferred) {
        processTransferFinished(terminal);
    }
    return true;
}

void LogDownloadViewModel::finishBatch(
    const QString &statusText, bool completed)
{
    if (completed) {
        setProgress(100.0);
    }
    if (m_kmlCancel) {
        m_kmlCancel->store(true);
    }
    m_activeToken = {};
    m_activeOperation = ExactLogTransferService::Operation::None;
    m_batch = {};
    m_pinnedTargetSource.clear();
    setStatus(statusText);
    notifyState();
}

void LogDownloadViewModel::setStatus(const QString &statusText)
{
    if (m_status == statusText) {
        return;
    }
    m_status = statusText;
    notifyState();
}

void LogDownloadViewModel::setProgress(double value)
{
    const double bounded = std::max(0.0, std::min(100.0, value));
    if (std::abs(m_progress - bounded) < 0.001) {
        return;
    }
    m_progress = bounded;
    notifyState();
}

void LogDownloadViewModel::notifyState()
{
    if (!m_shuttingDown) {
        emit stateChanged();
    }
}

QString LogDownloadViewModel::sourceSuffix() const
{
    return sourceText(m_pinnedTargetSource);
}

QString LogDownloadViewModel::serviceStartError(
    ExactLogTransferService::StartResult result) const
{
    switch (result) {
    case ExactLogTransferService::StartResult::Started:
        return QString();
    case ExactLogTransferService::StartResult::Busy:
        return tr("another log operation is already running");
    case ExactLogTransferService::StartResult::InvalidOwner:
        return tr("invalid operation owner");
    case ExactLogTransferService::StartResult::InvalidLease:
        return tr("vehicle selection is no longer current");
    case ExactLogTransferService::StartResult::InvalidArgument:
        return tr("invalid request");
    case ExactLogTransferService::StartResult::UnsafeRoute:
        return tr("the physical link is not safe for log transfer");
    case ExactLogTransferService::StartResult::IoError:
        return tr("could not open the destination file");
    case ExactLogTransferService::StartResult::TransportUnavailable:
        return tr("physical link unavailable");
    case ExactLogTransferService::StartResult::IdentifierExhausted:
        return tr("operation identifiers exhausted");
    case ExactLogTransferService::StartResult::ShuttingDown:
        return tr("transfer service is shutting down");
    }
    return tr("unknown transfer error");
}

QString LogDownloadViewModel::transferError(
    const ExactLogTransferResult &result) const
{
    if (!result.errorString.trimmed().isEmpty()) {
        return result.errorString;
    }
    switch (result.outcome) {
    case ExactLogTransferService::Outcome::Completed:
        return tr("completed");
    case ExactLogTransferService::Outcome::SubmittedUnconfirmed:
        return tr("request submitted without confirmation");
    case ExactLogTransferService::Outcome::Cancelled:
        return tr("canceled");
    case ExactLogTransferService::Outcome::TimedOut:
        return tr("timed out");
    case ExactLogTransferService::Outcome::LeaseRetired:
        return tr("vehicle or physical-link session changed");
    case ExactLogTransferService::Outcome::TransportFailed:
        return tr("transport failed");
    case ExactLogTransferService::Outcome::IoError:
        return tr("file write failed");
    case ExactLogTransferService::Outcome::ProtocolError:
        return tr("protocol error");
    }
    return tr("unknown transfer error");
}

LogDownloadRow *LogDownloadViewModel::selectedRow()
{
    for (LogDownloadRow &row : m_logs) {
        if (row.id == m_selectedLogId) {
            return &row;
        }
    }
    return nullptr;
}

const LogDownloadRow *LogDownloadViewModel::selectedRow() const
{
    for (const LogDownloadRow &row : m_logs) {
        if (row.id == m_selectedLogId) {
            return &row;
        }
    }
    return nullptr;
}
