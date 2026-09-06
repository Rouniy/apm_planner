#include "SftpLogDownloadService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSettings>
#include <QThread>

#include <atomic>
#include <exception>
#include <limits>
#include <utility>

struct SftpLogDownloadService::SharedState
{
    std::atomic_bool cancelled{false};
    std::atomic<qint64> completed{0};
    std::atomic<qint64> total{0};
    mutable QMutex mutex;
    QString status;

    void setStatus(const QString &value)
    {
        QMutexLocker locker(&mutex);
        status = value;
    }

    QString statusSnapshot() const
    {
        QMutexLocker locker(&mutex);
        return status;
    }
};

struct SftpLogDownloadService::Request
{
    Operation operation = Operation::None;
    SftpLogConnection connection;
    QString remoteDirectory;
    QVector<SftpLogEntry> entries;
    QString destinationDirectory;
    bool createKml = false;
};

struct SftpLogDownloadService::WorkerResult
{
    bool needsTrust = false;
    bool connected = false;
    SshHostKeyChallenge challenge;
    ListResult list;
    SftpLogDownloadSupport::Result download;
    DeleteResult deletion;
};

namespace
{
void scrub(QString *value)
{
    if (!value)
        return;
    value->fill(QChar('\0'));
    value->clear();
    value->squeeze();
}

QString connectionIdentity(const SftpLogConnection &connection)
{
    return connection.username + QLatin1Char('@')
        + connection.host.toCaseFolded() + QLatin1Char(':')
        + QString::number(connection.port);
}

void setError(QString *error, const QString &value)
{
    if (error)
        *error = value;
}
} // namespace

class SftpLogDownloadService::Worker final : public QObject
{
public:
    QString initialize(const SftpLogSessionFactory &factory)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        try {
            m_session = factory ? factory() : nullptr;
        } catch (...) {
            return QStringLiteral("The SFTP backend factory threw an exception.");
        }
        return m_session ? QString()
                         : QStringLiteral("The SFTP backend is unavailable.");
    }

    WorkerResult execute(const std::shared_ptr<Request> &request,
                         const std::shared_ptr<SharedState> &state,
                         const QString &trustedFingerprint)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        WorkerResult result;
        try {
        if (!request || !state || !m_session) {
            const QString error = QStringLiteral("The SFTP backend is unavailable.");
            assignError(request, &result, error);
            return result;
        }
        const auto cancelled = [state]() {
            return state->cancelled.load(std::memory_order_relaxed);
        };
        if (cancelled()) {
            assignCancelled(request, &result);
            return result;
        }

        const QString identity = connectionIdentity(request->connection);
        if (!m_session->isConnected() || identity != m_identity) {
            if (m_session->isConnected())
                m_session->stop();
            m_identity.clear();
            state->setStatus(QStringLiteral("Connecting to %1:%2 over SFTP…")
                .arg(request->connection.host,
                     QString::number(request->connection.port)));
            QString error;
            SshHostKeyChallenge challenge;
            const bool connected = m_session->connect(
                request->connection, trustedFingerprint, &challenge, &error,
                cancelled);
            if (!connected) {
                if (!challenge.presentedFingerprint.isEmpty()) {
                    result.needsTrust = true;
                    result.challenge = challenge;
                    result.connected = false;
                    state->setStatus(challenge.isChanged()
                        ? QStringLiteral("The SSH host key changed; explicit trust is required.")
                        : QStringLiteral("The SSH host key is unknown; explicit trust is required."));
                    return result;
                }
                if (cancelled())
                    assignCancelled(request, &result);
                else
                    assignError(request, &result,
                        error.isEmpty() ? QStringLiteral("SFTP connection failed.") : error);
                result.connected = m_session->isConnected();
                return result;
            }
            m_identity = identity;
        }

        // The backend has either consumed the password during authentication
        // or reused an already authenticated session. Retain no request copy.
        scrub(&request->connection.password);
        result.connected = true;
        switch (request->operation) {
        case Operation::Refresh:
            runList(request, state, &result);
            break;
        case Operation::Download:
            runDownload(request, state, &result);
            break;
        case Operation::Delete:
            runDelete(request, state, &result);
            break;
        case Operation::None:
            assignError(request, &result, QStringLiteral("No SFTP operation was selected."));
            break;
        }
        if (state->cancelled.load(std::memory_order_relaxed)) {
            // Cancellation never leaves an ambiguously interrupted channel for
            // reuse. stop() still executes on this same dedicated thread.
            m_session->stop();
            m_identity.clear();
        }
        result.connected = m_session->isConnected();
        return result;
        } catch (const std::exception &exception) {
            assignError(request, &result,
                QStringLiteral("SFTP backend exception: %1")
                    .arg(QString::fromLocal8Bit(exception.what())));
        } catch (...) {
            assignError(request, &result,
                        QStringLiteral("The SFTP backend threw an unknown exception."));
        }
        try {
            if (m_session)
                m_session->stop();
        } catch (...) {
        }
        m_identity.clear();
        result.connected = false;
        return result;
    }

    void shutdown()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        if (m_session) {
            m_session->stop();
            m_session.reset();
        }
        m_identity.clear();
    }

public:
    static void assignError(const std::shared_ptr<Request> &request,
                            WorkerResult *result, const QString &error)
    {
        if (!request || !result)
            return;
        switch (request->operation) {
        case Operation::Refresh: result->list.error = error; break;
        case Operation::Download: result->download.error = error; break;
        case Operation::Delete: result->deletion.error = error; break;
        case Operation::None: break;
        }
    }

    static void assignCancelled(const std::shared_ptr<Request> &request,
                                WorkerResult *result)
    {
        if (!request || !result)
            return;
        switch (request->operation) {
        case Operation::Refresh: result->list.cancelled = true; break;
        case Operation::Download: result->download.cancelled = true; break;
        case Operation::Delete: result->deletion.cancelled = true; break;
        case Operation::None: break;
        }
    }

private:

    void runList(const std::shared_ptr<Request> &request,
                 const std::shared_ptr<SharedState> &state,
                 WorkerResult *result)
    {
        state->setStatus(QStringLiteral("Listing %1…").arg(request->remoteDirectory));
        QString error;
        QVector<SftpLogEntry> entries;
        const bool success = m_session->listLogs(
            request->remoteDirectory, &entries, &error,
            [state]() { return state->cancelled.load(std::memory_order_relaxed); });
        if (state->cancelled.load(std::memory_order_relaxed)) {
            result->list.cancelled = true;
            return;
        }
        result->list.success = success;
        result->list.error = error;
        if (success)
            result->list.entries = std::move(entries);
        state->completed.store(success ? result->list.entries.size() : 0,
                               std::memory_order_relaxed);
        state->total.store(success ? result->list.entries.size() : 0,
                           std::memory_order_relaxed);
    }

    void runDownload(const std::shared_ptr<Request> &request,
                     const std::shared_ptr<SharedState> &state,
                     WorkerResult *result)
    {
        state->setStatus(QStringLiteral("Downloading %1 remote log(s)…")
            .arg(request->entries.size()));
        result->download = SftpLogDownloadSupport::download(
            *m_session, request->entries, request->destinationDirectory,
            request->createKml,
            [state]() { return state->cancelled.load(std::memory_order_relaxed); },
            [state](qint64 done, qint64 total, const QString &status) {
                state->completed.store(qMax<qint64>(0, done), std::memory_order_relaxed);
                state->total.store(qMax<qint64>(0, total), std::memory_order_relaxed);
                state->setStatus(status);
            });
    }

    void runDelete(const std::shared_ptr<Request> &request,
                   const std::shared_ptr<SharedState> &state,
                   WorkerResult *result)
    {
        result->deletion.requested = request->entries.size();
        state->total.store(request->entries.size(), std::memory_order_relaxed);
        for (int index = 0; index < request->entries.size(); ++index) {
            if (state->cancelled.load(std::memory_order_relaxed)) {
                result->deletion.cancelled = true;
                break;
            }
            const SftpLogEntry entry = request->entries.at(index);
            state->setStatus(QStringLiteral("Deleting %1 (%2/%3)…")
                .arg(entry.remotePath(), QString::number(index + 1),
                     QString::number(request->entries.size())));
            QString error;
            if (m_session->remove(entry, &error,
                    [state]() { return state->cancelled.load(std::memory_order_relaxed); })) {
                result->deletion.deletedEntries.append(entry);
            } else if (state->cancelled.load(std::memory_order_relaxed)) {
                // Cancellation can race an already-submitted SFTP unlink. Keep
                // the backend's outcome-unknown receipt instead of presenting
                // this entry as merely untouched; a refresh is required before
                // any retry.
                result->deletion.failedPaths.append(
                    entry.remotePath() + QStringLiteral(": ")
                    + (error.isEmpty()
                           ? QStringLiteral("delete interrupted; the remote outcome is unknown")
                           : error));
                result->deletion.cancelled = true;
                break;
            } else {
                result->deletion.failedPaths.append(
                    entry.remotePath() + QStringLiteral(": ")
                    + (error.isEmpty() ? QStringLiteral("delete failed") : error));
            }
            state->completed.store(index + 1, std::memory_order_relaxed);
        }
        result->deletion.success = !result->deletion.cancelled;
    }

    std::unique_ptr<SftpLogSession> m_session;
    QString m_identity;
};

SftpLogDownloadService::SftpLogDownloadService(
    SftpLogSessionFactory factory, QObject *parent)
    : QObject(parent)
    , m_idleStatus(QStringLiteral(
          "Enter the companion-computer SSH details, then refresh the list."))
{
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("SftpLogDownloadWorker"));
    m_worker = new Worker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
    QMetaObject::invokeMethod(m_worker, [this, factory]() {
        m_startupError = m_worker->initialize(factory);
    }, Qt::BlockingQueuedConnection);
    if (!m_startupError.isEmpty())
        m_idleStatus = m_startupError;
}

SftpLogDownloadService::~SftpLogDownloadService()
{
    shutdown();
}

QString SftpLogDownloadService::status() const
{
    return m_state ? m_state->statusSnapshot() : m_idleStatus;
}

qint64 SftpLogDownloadService::progressCompleted() const noexcept
{
    return m_state ? m_state->completed.load(std::memory_order_relaxed) : 0;
}

qint64 SftpLogDownloadService::progressTotal() const noexcept
{
    return m_state ? m_state->total.load(std::memory_order_relaxed) : 0;
}

bool SftpLogDownloadService::refresh(
    const SftpLogConnection &connection, const QString &remoteDirectory,
    quint64 *operationIdOut, QString *error)
{
    if (operationIdOut)
        *operationIdOut = 0;
    auto request = std::make_shared<Request>();
    request->operation = Operation::Refresh;
    request->connection = connection;
    request->remoteDirectory = SftpLogSession::normalizeDirectory(remoteDirectory, error);
    if (request->remoteDirectory.isEmpty()) {
        scrub(&request->connection.password);
        return false;
    }
    return start(std::move(request), operationIdOut, error);
}

bool SftpLogDownloadService::download(
    const SftpLogConnection &connection, const QVector<SftpLogEntry> &entries,
    const QString &destinationDirectory, bool createKml,
    quint64 *operationIdOut, QString *error)
{
    if (operationIdOut)
        *operationIdOut = 0;
    auto request = std::make_shared<Request>();
    request->operation = Operation::Download;
    request->connection = connection;
    request->entries = entries;
    request->destinationDirectory = destinationDirectory;
    request->createKml = createKml;
    if (entries.isEmpty()) {
        scrub(&request->connection.password);
        setError(error, QStringLiteral("Select at least one listed remote log."));
        return false;
    }
    return start(std::move(request), operationIdOut, error);
}

bool SftpLogDownloadService::remove(
    const SftpLogConnection &connection, const QVector<SftpLogEntry> &entries,
    quint64 *operationIdOut, QString *error)
{
    if (operationIdOut)
        *operationIdOut = 0;
    auto request = std::make_shared<Request>();
    request->operation = Operation::Delete;
    request->connection = connection;
    request->entries = entries;
    if (entries.isEmpty()) {
        scrub(&request->connection.password);
        setError(error, QStringLiteral("Select at least one listed remote log."));
        return false;
    }
    return start(std::move(request), operationIdOut, error);
}

bool SftpLogDownloadService::start(
    std::shared_ptr<Request> request, quint64 *operationIdOut, QString *error)
{
    if (operationIdOut)
        *operationIdOut = 0;
    if (!request || request->operation == Operation::None) {
        setError(error, QStringLiteral("No SFTP operation was selected."));
        return false;
    }
    if (m_shuttingDown || !m_worker || !m_startupError.isEmpty()) {
        scrub(&request->connection.password);
        setError(error, m_startupError.isEmpty()
            ? QStringLiteral("The SFTP service is shutting down.") : m_startupError);
        return false;
    }
    if (busy()) {
        scrub(&request->connection.password);
        setError(error, QStringLiteral("Another SFTP operation is already active."));
        return false;
    }
    const QString endpointInput = request->connection.host;
    QString host;
    quint16 port = request->connection.port;
    QString parseError;
    if (!SftpLogSession::parseEndpoint(endpointInput, port, &host, &port, &parseError)
        || request->connection.username.trimmed().isEmpty()) {
        scrub(&request->connection.password);
        setError(error, parseError.isEmpty()
            ? QStringLiteral("Enter the SSH username.") : parseError);
        return false;
    }
    if (request->entries.size() > SftpLogSession::MaximumEntries) {
        scrub(&request->connection.password);
        setError(error, QStringLiteral("The selected remote-log count exceeds the safety limit."));
        return false;
    }
    request->connection.host = host;
    request->connection.port = port;
    request->connection.username = request->connection.username.trimmed();
    for (const SftpLogEntry &entry : request->entries) {
        if (!SftpLogSession::isSafeName(entry.name)
            || !entry.name.endsWith(QStringLiteral(".bin"), Qt::CaseInsensitive)
            || entry.length < 0 || entry.length > SftpLogSession::MaximumFileBytes
            || !entry.lastWriteTimeUtc.isValid() || entry.remotePath().isEmpty()) {
            scrub(&request->connection.password);
            setError(error, QStringLiteral("A selected remote-log snapshot is invalid."));
            return false;
        }
    }

    quint64 next = ++m_nextOperationId;
    if (next == 0)
        next = ++m_nextOperationId;
    m_request = std::move(request);
    m_state = std::make_shared<SharedState>();
    m_state->setStatus(QStringLiteral("Starting SFTP operation…"));
    m_operationId = next;
    m_operation = m_request->operation;
    m_awaitingTrust = false;
    m_challenge = {};
    m_listResult = {};
    m_downloadResult = {};
    m_deleteResult = {};
    if (operationIdOut)
        *operationIdOut = next;
    if (error)
        error->clear();

    QPointer<SftpLogDownloadService> guard(this);
    emit stateChanged();
    if (!guard || m_operationId != next || m_shuttingDown)
        return true;
    dispatch(trustedFingerprint(m_request->connection));
    return true;
}

void SftpLogDownloadService::dispatch(const QString &trustedFingerprint)
{
    if (!m_worker || !m_request || !m_state || !m_operationId)
        return;
    Worker *worker = m_worker;
    const auto request = m_request;
    const auto state = m_state;
    const quint64 operationId = m_operationId;
    QPointer<SftpLogDownloadService> service(this);
    QMetaObject::invokeMethod(worker,
        [worker, request, state, trustedFingerprint, service, operationId]() {
        WorkerResult result = worker->execute(request, state, trustedFingerprint);
        if (!service)
            return;
        QMetaObject::invokeMethod(service.data(),
            [service, operationId, request, state, result]() mutable {
            if (service)
                service->handleWorkerResult(operationId, request, state,
                                            std::move(result));
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void SftpLogDownloadService::handleWorkerResult(
    quint64 operationId, const std::shared_ptr<Request> &request,
    const std::shared_ptr<SharedState> &state, WorkerResult result)
{
    if (m_shuttingDown || operationId != m_operationId
        || request != m_request || state != m_state) {
        return;
    }
    m_connected = result.connected;
    if (result.needsTrust) {
        if (result.challenge.host.compare(request->connection.host,
                                          Qt::CaseInsensitive) != 0
            || result.challenge.port != request->connection.port
            || !result.challenge.presentedFingerprint.startsWith(
                QStringLiteral("SHA256:"))) {
            WorkerResult invalid;
            invalid.connected = false;
            Worker::assignError(request, &invalid,
                QStringLiteral("The SFTP backend returned a host-key challenge for a different or invalid endpoint."));
            finish(operationId, std::move(invalid));
            return;
        }
        m_awaitingTrust = true;
        m_challenge = result.challenge;
        QPointer<SftpLogDownloadService> guard(this);
        emit stateChanged();
        if (!guard || operationId != m_operationId || !m_awaitingTrust)
            return;
        emit hostKeyChallengeAvailable(operationId);
        return;
    }
    finish(operationId, std::move(result));
}

bool SftpLogDownloadService::trustHostKey(quint64 operationId, QString *error)
{
    if (operationId == 0 || operationId != m_operationId || !m_awaitingTrust
        || m_shuttingDown || !m_request) {
        setError(error, QStringLiteral("The host-key challenge is no longer current."));
        return false;
    }
    const SshHostKeyChallenge challenge = m_challenge;
    if (!persistTrust(challenge, error)) {
        WorkerResult result;
        result.connected = false;
        const QString failure = error && !error->isEmpty()
            ? *error : QStringLiteral("The trusted host key could not be saved.");
        Worker::assignError(m_request, &result, failure);
        finish(operationId, std::move(result));
        return false;
    }
    m_awaitingTrust = false;
    m_challenge = {};
    m_state->setStatus(QStringLiteral("Host key pinned; reconnecting before authentication…"));
    if (error)
        error->clear();
    QPointer<SftpLogDownloadService> guard(this);
    emit stateChanged();
    if (!guard || operationId != m_operationId || m_shuttingDown)
        return true;
    dispatch(challenge.presentedFingerprint);
    return true;
}

bool SftpLogDownloadService::rejectHostKey(quint64 operationId)
{
    if (operationId == 0 || operationId != m_operationId || !m_awaitingTrust)
        return false;
    m_state->cancelled.store(true, std::memory_order_relaxed);
    WorkerResult result;
    result.connected = false;
    Worker::assignCancelled(m_request, &result);
    finish(operationId, std::move(result));
    return true;
}

bool SftpLogDownloadService::cancel(quint64 operationId)
{
    if (operationId == 0 || operationId != m_operationId || !m_state)
        return false;
    if (m_awaitingTrust)
        return rejectHostKey(operationId);
    const bool wasCancelled = m_state->cancelled.exchange(true,
                                                          std::memory_order_relaxed);
    if (!wasCancelled) {
        m_state->setStatus(QStringLiteral(
            "Cancelling SFTP operation; waiting for the worker to stop safely…"));
        emit stateChanged();
    }
    return !wasCancelled;
}

void SftpLogDownloadService::finish(quint64 operationId, WorkerResult result)
{
    if (operationId == 0 || operationId != m_operationId)
        return;
    const Operation completedOperation = m_operation;
    m_connected = result.connected;
    switch (completedOperation) {
    case Operation::Refresh:
        m_listResult = std::move(result.list);
        if (m_listResult.success) {
            m_idleStatus = m_listResult.entries.isEmpty()
                ? QStringLiteral("No DataFlash BIN logs were found in the remote directory.")
                : QStringLiteral("Found %1 DataFlash log(s).").arg(m_listResult.entries.size());
        } else if (m_listResult.cancelled) {
            m_idleStatus = QStringLiteral("SFTP list operation cancelled.");
        } else {
            m_idleStatus = QStringLiteral("SFTP list failed: %1").arg(m_listResult.error);
        }
        break;
    case Operation::Download:
        m_downloadResult = std::move(result.download);
        if (m_downloadResult.success) {
            m_idleStatus = QStringLiteral("Downloaded %1 log(s).")
                .arg(m_downloadResult.savedLogs);
        } else if (m_downloadResult.cancelled) {
            m_idleStatus = QStringLiteral("SFTP download cancelled. Completed files are retained.");
        } else {
            m_idleStatus = QStringLiteral("SFTP download failed: %1")
                .arg(m_downloadResult.error);
        }
        break;
    case Operation::Delete:
        m_deleteResult = std::move(result.deletion);
        if (m_deleteResult.cancelled) {
            m_idleStatus = QStringLiteral("Remote deletion cancelled after %1/%2 file(s).")
                .arg(m_deleteResult.deletedEntries.size()).arg(m_deleteResult.requested);
        } else {
            m_idleStatus = QStringLiteral("Deleted %1/%2 remote log(s); %3 failed.")
                .arg(m_deleteResult.deletedEntries.size()).arg(m_deleteResult.requested)
                .arg(m_deleteResult.failedPaths.size());
        }
        break;
    case Operation::None:
        m_idleStatus = QStringLiteral("SFTP operation finished without a result.");
        break;
    }
    clearActive();
    m_finishing = true;
    QPointer<SftpLogDownloadService> guard(this);
    emit operationFinished(operationId);
    if (!guard)
        return;
    emit stateChanged();
    if (!guard)
        return;
    m_finishing = false;
    emit stateChanged();
}

QString SftpLogDownloadService::trustedFingerprint(
    const SftpLogConnection &connection) const
{
    QSettings settings;
    const QString name = SftpLogSession::trustedKeySettingName(
        connection.host, connection.port);
    return settings.value(name).toString();
}

bool SftpLogDownloadService::persistTrust(
    const SshHostKeyChallenge &challenge, QString *error)
{
    if (challenge.host.trimmed().isEmpty()
        || challenge.port == 0
        || challenge.presentedFingerprint.trimmed().isEmpty()) {
        setError(error, QStringLiteral("The presented SSH host-key fingerprint is invalid."));
        return false;
    }
    const QString setting = SftpLogSession::trustedKeySettingName(
        challenge.host, challenge.port);
    QSettings settings;
    settings.setValue(setting, challenge.presentedFingerprint);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setError(error, QStringLiteral("The trusted SSH host key could not be saved."));
        return false;
    }
    QSettings verify;
    verify.sync();
    if (verify.status() != QSettings::NoError
        || !SftpLogSession::fingerprintsEqual(
            verify.value(setting).toString(), challenge.presentedFingerprint)) {
        setError(error, QStringLiteral("The saved SSH host key could not be verified."));
        return false;
    }
    return true;
}

void SftpLogDownloadService::clearActive()
{
    if (m_request)
        scrub(&m_request->connection.password);
    m_request.reset();
    m_state.reset();
    m_operationId = 0;
    m_operation = Operation::None;
    m_awaitingTrust = false;
    m_challenge = {};
}

void SftpLogDownloadService::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    if (m_state)
        m_state->cancelled.store(true, std::memory_order_relaxed);
    if (m_worker && m_thread && m_thread->isRunning()) {
        Worker *worker = m_worker;
        QMetaObject::invokeMethod(worker, [worker]() { worker->shutdown(); },
                                  Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
    }
    m_worker = nullptr;
    clearActive();
    m_finishing = false;
    m_connected = false;
}
