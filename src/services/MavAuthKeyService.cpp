#include "MavAuthKeyService.h"
#include "MavAuthKeyStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QWaitCondition>
#include <openssl/crypto.h>
#include <limits>
#include <utility>

namespace {
enum Operation {
    Create, Unlock, AddSeed, RemoveKey, Lock, RequestKey,
    RequestKeyByFingerprint
};

void cleanse(QByteArray &bytes)
{
    if (!bytes.isEmpty()) OPENSSL_cleanse(bytes.data(), size_t(bytes.size()));
    bytes.clear();
}
void cleanse(QString &text)
{
    if (!text.isEmpty()) OPENSSL_cleanse(text.data(), size_t(text.size()) * sizeof(QChar));
    text.clear();
}

bool validText(const QString &text, int minimumBytes, int maximumBytes, bool name = false)
{
    // Bound UTF-16 before conversion/copy, then enforce the store's UTF-8 bound.
    if (text.isEmpty() || text.size() > maximumBytes) return false;
    bool nonSpace = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (ch.isNull() || (name && ch.category() == QChar::Other_Control)) return false;
        nonSpace |= !ch.isSpace();
        if (ch.isHighSurrogate()) {
            if (++i == text.size() || !text[i].isLowSurrogate()) return false;
        } else if (ch.isLowSurrogate()) return false;
    }
    if (!nonSpace) return false;
    QByteArray encoded = text.toUtf8();
    const bool valid = encoded.size() >= minimumBytes && encoded.size() <= maximumBytes;
    cleanse(encoded);
    return valid;
}

struct Job final
{
    int operation = Lock;
    quint64 token = 0;
    QString name;
    QString secret;
    MavAuthKeyService::KeyCallback callback;
    bool success = false;
    bool unlocked = false;
    QStringList names;
    QString error;
    QByteArray key;
    QByteArray fingerprint;
    ~Job() { cleanse(secret); cleanse(key); cleanse(fingerprint); }
};

struct CleansedBytes final
{
    QByteArray bytes;
    ~CleansedBytes() { cleanse(bytes); }
};
}

// QThread's QObject remains on the owner thread. Only run() touches the store;
// submit/stop access only the mutex-protected, single-item admission mailbox.
class MavAuthKeyWorker final : public QThread
{
    Q_OBJECT
public:
    explicit MavAuthKeyWorker(QString path) : m_path(std::move(path)) {}
    bool submit(const std::shared_ptr<Job> &job) {
        QMutexLocker locker(&m_mutex);
        if (m_stop || m_pending) return false;
        m_pending = job;
        m_ready.wakeOne();
        return true;
    }
    void stop() {
        QMutexLocker locker(&m_mutex);
        m_stop = true;
        m_ready.wakeOne();
    }
signals:
    void completed(quint64 token); // metadata only; no secret-bearing Qt signal
protected:
    void run() override {
        MavAuthKeyStore store(m_path);
        for (;;) {
            std::shared_ptr<Job> job;
            {
                QMutexLocker locker(&m_mutex);
                while (!m_pending && !m_stop) m_ready.wait(&m_mutex);
                if (!m_pending) break;
                job = std::move(m_pending);
            }
            try {
                switch (job->operation) {
                case Create: job->success = store.create(job->secret, &job->error); break;
                case Unlock: job->success = store.unlock(job->secret, &job->error); break;
                case AddSeed: job->success = store.addSeed(job->name, job->secret, &job->error); break;
                case RemoveKey: job->success = store.removeKey(job->name, &job->error); break;
                case Lock: store.lock(); job->success = true; break;
                case RequestKey: job->success = store.key(job->name, &job->key, &job->error); break;
                case RequestKeyByFingerprint: {
                    const QStringList keyNames = store.keyNames();
                    if (!store.isUnlocked()) {
                        job->error = QStringLiteral(
                            "The signing-key vault is locked.");
                        break;
                    }
                    if (keyNames.size() > MavAuthKeyStore::MaximumKeys) {
                        job->error = QStringLiteral(
                            "The signing-key vault exceeds its bounded key limit.");
                        break;
                    }
                    for (const QString &keyName : keyNames) {
                        CleansedBytes candidate;
                        QString candidateError;
                        if (!store.key(
                                keyName, &candidate.bytes,
                                &candidateError)) {
                            job->error = candidateError.isEmpty()
                                ? QStringLiteral(
                                    "A signing key could not be inspected safely.")
                                : candidateError;
                            break;
                        }
                        const QByteArray fingerprint = QCryptographicHash::hash(
                            candidate.bytes, QCryptographicHash::Sha256);
                        const bool matches = fingerprint.size() == job->fingerprint.size()
                            && CRYPTO_memcmp(
                                   fingerprint.constData(),
                                   job->fingerprint.constData(),
                                   size_t(fingerprint.size())) == 0;
                        if (matches) {
                            job->key = std::move(candidate.bytes);
                            job->success = true;
                            break;
                        }
                    }
                    if (!job->success && job->error.isEmpty()) {
                        job->error = QStringLiteral(
                            "No unlocked signing key matches the required fingerprint.");
                    }
                    break;
                }
                }
                job->unlocked = store.isUnlocked();
                job->names = store.keyNames();
            } catch (...) {
                job->success = false;
                job->error = QStringLiteral("The vault operation could not be completed.");
                store.lock();
                job->unlocked = false;
                job->names.clear();
                cleanse(job->key);
            }
            cleanse(job->secret);
            emit completed(job->token);
        }
        // Store destruction (including wrapping-key cleansing and file-lock
        // release) is guaranteed here in the same thread that constructed it.
    }
private:
    const QString m_path;
    QMutex m_mutex;
    QWaitCondition m_ready;
    bool m_stop = false;
    std::shared_ptr<Job> m_pending;
};

struct MavAuthKeyService::State
{
    std::unique_ptr<MavAuthKeyWorker> worker;
    std::shared_ptr<Job> job;
    quint64 nextToken = 0;
    bool stopping = false;
    bool admitting = false;
    quint64 deferredCompletion = 0;
    bool unlocked = false;
    QStringList names;
    QString error;
};

MavAuthKeyService::MavAuthKeyService(const QString &absoluteVaultPath, QObject *parent)
    : QObject(parent), m_state(new State)
{
    if (absoluteVaultPath.size() > 32768 || !QDir::isAbsolutePath(absoluteVaultPath)
        || absoluteVaultPath.contains(QChar(0))) {
        m_state->error = QStringLiteral("An absolute vault file path is required.");
        return;
    }
    m_state->worker.reset(new MavAuthKeyWorker(absoluteVaultPath));
    connect(m_state->worker.get(), &MavAuthKeyWorker::completed,
            this, &MavAuthKeyService::complete, Qt::QueuedConnection);
    m_state->worker->start();
}

MavAuthKeyService::~MavAuthKeyService()
{
    // Do not emit signals or dispatch queued callbacks during destruction.
    if (m_state->worker) {
        m_state->worker->stop();
        m_state->worker->wait();
    }
}

quint64 MavAuthKeyService::create(const QString &masterPassphrase)
{ return submit(Create, {}, masterPassphrase); }
quint64 MavAuthKeyService::unlock(const QString &masterPassphrase)
{ return submit(Unlock, {}, masterPassphrase); }
quint64 MavAuthKeyService::addSeed(const QString &name, const QString &seed)
{ return submit(AddSeed, name, seed); }
quint64 MavAuthKeyService::removeKey(const QString &name)
{ return submit(RemoveKey, name, {}); }
quint64 MavAuthKeyService::lock()
{ return submit(Lock, {}, {}); }
quint64 MavAuthKeyService::requestKey(const QString &name, KeyCallback callback)
{ return submit(RequestKey, name, {}, std::move(callback)); }
quint64 MavAuthKeyService::requestKeyByFingerprint(
    const QByteArray &fingerprint, KeyCallback callback)
{
    return submit(RequestKeyByFingerprint, {}, {}, std::move(callback),
                  fingerprint);
}

bool MavAuthKeyService::busy() const { return bool(m_state->job); }
bool MavAuthKeyService::isUnlocked() const { return m_state->unlocked; }
bool MavAuthKeyService::isShuttingDown() const { return m_state->stopping; }
QStringList MavAuthKeyService::keyNames() const { return m_state->names; }
QString MavAuthKeyService::lastError() const { return m_state->error; }

quint64 MavAuthKeyService::refuse(const QString &error)
{
    if (m_state->error == error) return 0;
    m_state->error = error;
    emit stateChanged();
    return 0;
}

quint64 MavAuthKeyService::submit(
    int operation, const QString &name, const QString &secret,
    KeyCallback callback, const QByteArray &fingerprint)
{
    if (QThread::currentThread() != thread()) return 0;
    if (m_state->stopping) return refuse(QStringLiteral("The vault service is shutting down."));
    if (!m_state->worker) return refuse(QStringLiteral("An absolute vault file path is required."));
    if (m_state->job) return refuse(QStringLiteral("Another vault operation is in progress."));
    if (m_state->nextToken == std::numeric_limits<quint64>::max())
        return refuse(QStringLiteral("The vault operation token space is exhausted."));
    if ((operation == Create || operation == Unlock)
        && !validText(secret, MavAuthKeyStore::MinimumPassphraseBytes,
                      MavAuthKeyStore::MaximumPassphraseBytes))
        return refuse(QStringLiteral("The master passphrase must contain 12..1024 UTF-8 bytes of valid non-blank text."));
    if ((operation == AddSeed || operation == RemoveKey || operation == RequestKey)
        && !validText(name, 1, MavAuthKeyStore::MaximumNameBytes, true))
        return refuse(QStringLiteral("A key name must contain 1..128 UTF-8 bytes of valid non-blank text without control characters."));
    if (operation == AddSeed && !validText(secret, 1, MavAuthKeyStore::MaximumSeedBytes))
        return refuse(QStringLiteral("A signing seed must contain 1..4096 UTF-8 bytes of valid non-blank text."));
    if (operation == AddSeed && m_state->unlocked
        && m_state->names.size() >= MavAuthKeyStore::MaximumKeys)
        return refuse(QStringLiteral("The vault has reached its 128-key limit."));
    if ((operation == RequestKey || operation == RequestKeyByFingerprint)
        && !callback)
        return refuse(QStringLiteral("An explicit signing-key recipient is required."));
    if (operation == RequestKeyByFingerprint
        && fingerprint.size() != MavAuthKeyStore::KeyBytes)
        return refuse(QStringLiteral(
            "A signing-key fingerprint must contain 32 bytes."));
    auto job = std::make_shared<Job>();
    job->operation = operation;
    job->token = ++m_state->nextToken;
    job->name = name;
    // Deep copy: cleansing our owned UTF-16 must not alter or rely on the
    // lifetime of a QLineEdit/caller's implicitly shared input buffer.
    job->secret = QString(secret.constData(), secret.size());
    job->callback = std::move(callback);
    job->fingerprint = QByteArray(
        fingerprint.constData(), fingerprint.size());
    m_state->job = job;
    m_state->error.clear();
    if (!m_state->worker->submit(job)) {
        m_state->job.reset();
        return refuse(QStringLiteral("The vault worker no longer accepts operations."));
    }
    const quint64 token = job->token;
    const QPointer<MavAuthKeyService> guard(this);
    m_state->admitting = true;
    emit stateChanged(); // may delete this or run a nested event loop
    if (!guard) return token;
    m_state->admitting = false;
    const quint64 deferred = std::exchange(m_state->deferredCompletion, 0);
    if (deferred != 0) {
        // Never let a nested event loop deliver a nominally asynchronous
        // completion before submit() has returned its operation token.
        QMetaObject::invokeMethod(
            this, [this, deferred] { complete(deferred); },
            Qt::QueuedConnection);
    }
    return token;
}

void MavAuthKeyService::complete(quint64 token)
{
    if (!m_state->job || m_state->job->token != token) return;
    if (m_state->admitting) {
        m_state->deferredCompletion = token;
        return;
    }
    const auto job = m_state->job;
    const QPointer<MavAuthKeyService> guard(this);
    bool success = job->success;
    QString error = job->error;
    if (job->callback) {
        if (m_state->stopping) {
            success = false;
            error = QStringLiteral("The vault service shut down before key delivery.");
            cleanse(job->key);
        }
        const auto callback = std::move(job->callback);
        try {
            callback(success, job->key, error);
        } catch (...) {
            success = false;
            error = QStringLiteral("The signing-key recipient could not complete its callback.");
        }
        cleanse(job->key);
        if (!guard) return;
    }
    m_state->job.reset();
    m_state->unlocked = !m_state->stopping && job->unlocked;
    m_state->names = m_state->stopping ? QStringList() : job->names;
    m_state->error = error;
    emit stateChanged();
    if (!guard) return;
    emit operationFinished(token, success, error);
}

void MavAuthKeyService::shutdown()
{
    if (QThread::currentThread() != thread()) return;
    if (m_state->stopping) return;
    m_state->stopping = true;
    m_state->unlocked = false;
    m_state->names.clear();
    if (m_state->worker) m_state->worker->stop();
    emit stateChanged();
}

#include "MavAuthKeyService.moc"
