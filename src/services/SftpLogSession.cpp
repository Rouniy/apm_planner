#include "SftpLogSession.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QIODevice>
#include <QPointer>
#include <QSet>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

namespace {
constexpr qint64 BufferSize = 64 * 1024;
constexpr int ConnectTimeoutMs = 20000, InactivityTimeoutMs = 30000;
constexpr int MaximumDirectoryRecords = 200000, MaximumPathBytes = 4096;
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
void cleanse(QByteArray &bytes) {
    volatile char *data = bytes.data(); for (int i = 0; i < bytes.size(); ++i) data[i] = 0;
    bytes.clear();
}
struct SecretBytes {
    QByteArray value;
    ~SecretBytes() { cleanse(value); }
};
struct SecretString {
    QString &value;
    ~SecretString() {
        volatile ushort *data = reinterpret_cast<volatile ushort *>(value.data());
        for (int i = 0; i < value.size(); ++i) data[i] = 0;
        value.clear();
    }
};
struct Library {
    int status = libssh2_init(0);
    ~Library() { if (!status) libssh2_exit(); }
};
bool libraryReady() { static const Library library; return library.status == 0; }

struct State {
    QTcpSocket socket;
    QTimer keepAlive;
    QThread *thread = QThread::currentThread();
    LIBSSH2_SESSION *ssh = nullptr;
    LIBSSH2_SFTP *sftp = nullptr;
    LIBSSH2_SFTP_HANDLE *handle = nullptr;
    quint64 generation = 1;
    bool busy = false, connected = false, closing = false;
    QElapsedTimer clock;
    qint64 lastActivity = 0;

    State() {
        clock.start(); socket.setReadBufferSize(BufferSize);
        keepAlive.setInterval(15000);
        QObject::connect(&keepAlive, &QTimer::timeout, &socket, [this] {
            if (!busy && connected && ssh && socket.state() == QAbstractSocket::ConnectedState) {
                int next = 0;
                if (libssh2_keepalive_send(ssh, &next) != 0) stop();
                else socket.flush();
            }
        });
    }
    ~State() { stop(); }
    bool onThread(QString *error) const {
        return thread == QThread::currentThread()
            || fail(error, QStringLiteral("The SFTP session must be used on its owning worker thread."));
    }
    void stop() {
        ++generation; busy = false; connected = false; closing = true; keepAlive.stop();
        // Fatal callback results make native cleanup independent of remote
        // cooperation. Never perform a blocking disconnect handshake here.
        socket.abort();
        if (handle) {
            libssh2_sftp_close_handle(handle);
            handle = nullptr;
        }
        if (sftp) {
            libssh2_sftp_shutdown(sftp);
            sftp = nullptr;
        }
        if (ssh) {
            libssh2_session_free(ssh);
            ssh = nullptr;
        }
        closing = false;
    }
    static LIBSSH2_SEND_FUNC(send) {
        Q_UNUSED(socket); Q_UNUSED(flags);
        auto *self = static_cast<State *>(*abstract);
        if (!self || self->closing || self->socket.state() != QAbstractSocket::ConnectedState) return -EPIPE;
        const qint64 available = BufferSize - self->socket.bytesToWrite();
        if (available <= 0) return -EAGAIN;
        const qint64 written = self->socket.write(static_cast<const char *>(buffer),
            qMin<qint64>(available, qint64(qMin<size_t>(length, BufferSize))));
        if (written < 0) return -EIO;
        if (written > 0) self->lastActivity = self->clock.elapsed();
        return written ? ssize_t(written) : -EAGAIN;
    }
    static LIBSSH2_RECV_FUNC(receive) {
        Q_UNUSED(socket); Q_UNUSED(flags);
        auto *self = static_cast<State *>(*abstract);
        if (!self || self->closing) return -ECONNRESET;
        if (!self->socket.bytesAvailable())
            return self->socket.state() == QAbstractSocket::ConnectedState ? -EAGAIN : 0;
        const qint64 count = self->socket.read(static_cast<char *>(buffer), qint64(qMin<size_t>(length, BufferSize)));
        if (count < 0) return -EIO;
        if (count > 0) self->lastActivity = self->clock.elapsed();
        return ssize_t(count);
    }
};

struct Operation {
    std::shared_ptr<State> state;
    SftpLogSession::Cancel cancel;
    QString *error;
    quint64 generation = 0;
    qint64 started = 0;
    int absoluteMs = 0;
    bool admitted = false;
    Operation(std::shared_ptr<State> value, SftpLogSession::Cancel check, QString *message, bool connecting = false)
        : state(std::move(value)), cancel(std::move(check)), error(message), absoluteMs(connecting ? ConnectTimeoutMs : 0) {
        if (error) error->clear();
        if (!state->onThread(error)) return;
        if (state->busy) { fail(error, QStringLiteral("Another SFTP operation is still running.")); return; }
        if (!connecting && (!state->connected || state->socket.state() != QAbstractSocket::ConnectedState)) {
            fail(error, QStringLiteral("The SFTP session is not connected.")); return;
        }
        generation = state->generation; state->busy = true; admitted = true;
        started = state->lastActivity = state->clock.elapsed();
    }
    ~Operation() { if (admitted && state->generation == generation) state->busy = false; }
    bool current() {
        if (!admitted) return false;
        const bool requested = cancel && cancel();
        if (state->generation != generation)
            return fail(error, QStringLiteral("The SFTP operation was stopped or replaced."));
        if (requested) { state->stop(); return fail(error, QStringLiteral("SFTP operation cancelled.")); }
        const auto now = state->clock.elapsed();
        if ((absoluteMs && now - started >= absoluteMs) || now - state->lastActivity >= InactivityTimeoutMs) {
            state->stop(); return fail(error, QStringLiteral("The SSH connection or SFTP inactivity deadline expired."));
        }
        return true;
    }
    bool wait(bool connecting = false) {
        if (!current()) return false;
        if (!connecting && state->socket.state() != QAbstractSocket::ConnectedState) {
            state->stop(); return fail(error, QStringLiteral("The SSH connection closed during the SFTP operation."));
        }
        if (!connecting && state->ssh) {
            const int directions = libssh2_session_block_directions(state->ssh);
            if ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) && state->socket.bytesAvailable()) return true;
            if (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) {
                state->socket.flush();
                if (state->socket.bytesToWrite() < BufferSize) return true;
            }
        }
        // Let Qt own all DNS/socket I/O; never combine QTcpSocket's receive
        // buffer with native recv(). No waitForConnected(50) early-abort trap.
        QEventLoop loop; QTimer slice; slice.setSingleShot(true);
        QObject::connect(&slice, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&state->socket, &QTcpSocket::readyRead, &loop, &QEventLoop::quit);
        QObject::connect(&state->socket, &QTcpSocket::bytesWritten, &loop, &QEventLoop::quit);
        QObject::connect(&state->socket, &QTcpSocket::stateChanged, &loop, &QEventLoop::quit);
        slice.start(50); loop.exec();
        return current();
    }
    bool nativeError(const QString &action, int code, bool destructive = false) {
        const unsigned long sftpError = state->sftp ? libssh2_sftp_last_error(state->sftp) : 0;
        const QString message = QStringLiteral("%1 failed (SSH %2, SFTP %3).%4").arg(action).arg(code).arg(sftpError)
            .arg(destructive ? QStringLiteral(" The remote delete outcome may be unknown; refresh the list before retrying.") : QString());
        if (code != LIBSSH2_ERROR_SFTP_PROTOCOL) state->stop();
        return fail(error, message);
    }
};

bool regularAttributes(const LIBSSH2_SFTP_ATTRIBUTES &attributes) {
    const unsigned long required = LIBSSH2_SFTP_ATTR_SIZE | LIBSSH2_SFTP_ATTR_PERMISSIONS | LIBSSH2_SFTP_ATTR_ACMODTIME;
    return (attributes.flags & required) == required && LIBSSH2_SFTP_S_ISREG(attributes.permissions)
        && attributes.filesize <= quint64(SftpLogSession::MaximumFileBytes)
        && attributes.mtime <= std::numeric_limits<quint32>::max();
}
bool compatibleDownloadAttributes(const LIBSSH2_SFTP_ATTRIBUTES &before,
                                  const LIBSSH2_SFTP_ATTRIBUTES &after) {
    // SFTP v3 exposes no inode/file identity. These are consistency checks,
    // not an identity proof or a lock against concurrent remote replacement.
    // Active append-only logs may grow between path/handle metadata reads.
    return regularAttributes(before) && regularAttributes(after)
        && SftpLogSession::compatibleDownloadMetadata(
            {{}, {}, qint64(before.filesize), QDateTime::fromSecsSinceEpoch(qint64(before.mtime), Qt::UTC)},
            {{}, {}, qint64(after.filesize), QDateTime::fromSecsSinceEpoch(qint64(after.mtime), Qt::UTC)});
}
QDateTime modified(const LIBSSH2_SFTP_ATTRIBUTES &attributes) {
    return QDateTime::fromSecsSinceEpoch(qint64(attributes.mtime), Qt::UTC);
}
bool lstat(Operation &operation, const QByteArray &path, LIBSSH2_SFTP_ATTRIBUTES *attributes) {
    for (;;) {
        if (!operation.current()) return false;
        const int result = libssh2_sftp_stat_ex(operation.state->sftp, path.constData(), unsigned(path.size()),
                                               LIBSSH2_SFTP_LSTAT, attributes);
        if (!result) return true;
        if (result != LIBSSH2_ERROR_EAGAIN) return operation.nativeError(QStringLiteral("Remote log metadata read"), result);
        if (!operation.wait()) return false;
    }
}
bool closeHandle(Operation &operation) {
    if (!operation.state->handle) return true;
    for (;;) {
        if (!operation.current()) return false;
        const int result = libssh2_sftp_close_handle(operation.state->handle);
        if (result != LIBSSH2_ERROR_EAGAIN) {
            operation.state->handle = nullptr;
            return !result || operation.nativeError(QStringLiteral("Remote handle close"), result);
        }
        if (!operation.wait()) return false;
    }
}
bool revalidate(Operation &operation, const SftpLogEntry &entry, bool unchanged,
                QByteArray *path, LIBSSH2_SFTP_ATTRIBUTES *attributes) {
    const QString directory = SftpLogSession::normalizeDirectory(entry.remoteDirectory, operation.error);
    if (directory.isEmpty() || !SftpLogSession::isSafeName(entry.name) || !entry.name.endsWith(".bin", Qt::CaseInsensitive))
        return fail(operation.error, QStringLiteral("Select a safe regular BIN log entry."));
    *path = (directory == "/" ? directory + entry.name : directory + '/' + entry.name).toUtf8();
    if (path->size() > MaximumPathBytes) return fail(operation.error, QStringLiteral("Remote log path exceeds the 4096-byte limit."));
    if (!lstat(operation, *path, attributes)) return false;
    if (!regularAttributes(*attributes)) return fail(operation.error, QStringLiteral("Remote log is no longer a regular, non-symlink BIN within the 16 GiB limit."));
    if (unchanged && (entry.length < 0 || quint64(entry.length) != attributes->filesize
        || !entry.lastWriteTimeUtc.isValid() || entry.lastWriteTimeUtc.toUTC() != modified(*attributes)))
        return fail(operation.error, QStringLiteral("Remote log changed since it was listed; refresh before deleting."));
    return true;
}
QByteArray sshString(const QByteArray &key, int *position) {
    if (*position < 0 || key.size() - *position < 4) return {};
    const auto *p = reinterpret_cast<const unsigned char *>(key.constData() + *position);
    const quint32 count = (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3];
    *position += 4;
    if (count > quint32(key.size() - *position)) return {};
    const auto value = key.mid(*position, int(count)); *position += int(count); return value;
}
int significantBits(const QByteArray &bytes) {
    int first = 0; while (first < bytes.size() && !bytes.at(first)) ++first;
    if (first == bytes.size()) return 0;
    unsigned char head = static_cast<unsigned char>(bytes.at(first)); int bits = 0;
    while (head) { ++bits; head >>= 1; }
    return (bytes.size() - first - 1) * 8 + bits;
}
int keyBits(const QByteArray &key, const QByteArray &algorithm, int position) {
    if (algorithm == "ssh-ed25519") return 256;
    if (algorithm.endsWith("nistp256")) return 256;
    if (algorithm.endsWith("nistp384")) return 384;
    if (algorithm.endsWith("nistp521")) return 521;
    if (algorithm == "ssh-rsa") { sshString(key, &position); return significantBits(sshString(key, &position)); }
    if (algorithm == "ssh-dss") return significantBits(sshString(key, &position));
    return 0;
}

class Libssh2SftpLogSession final : public SftpLogSession {
    std::shared_ptr<State> m_state = std::make_shared<State>();
public:
    ~Libssh2SftpLogSession() override { m_state->stop(); }
    bool isConnected() const override {
        const auto state = m_state;
        return state->onThread(nullptr) && state->connected && state->ssh && state->sftp
            && state->socket.state() == QAbstractSocket::ConnectedState;
    }
    void stop() override { const auto state = m_state; if (state->onThread(nullptr)) state->stop(); }

    bool connect(const SftpLogConnection &requested, const QString &trusted,
                 SshHostKeyChallenge *challenge, QString *error, Cancel cancel) override {
        const auto state = m_state; auto connection = requested;
        const SecretString passwordString{connection.password};
        const QString trustedFingerprint = trusted;
        if (challenge) *challenge = {};
        if (error) error->clear();
        if (!state->onThread(error)) return false;
        if (state->busy) return fail(error, QStringLiteral("Another SFTP operation is still running."));
        state->stop();
        if (connection.host.size() > 1024 || connection.host.trimmed().isEmpty() || connection.host.toUtf8().size() > 1024
            || connection.host.contains(QChar(0)) || !connection.port || connection.username.trimmed().isEmpty()
            || connection.username.size() > 1024 || connection.username.toUtf8().size() > 1024 || connection.username.contains(QChar(0))
            || connection.password.size() > 64 * 1024 || trustedFingerprint.size() > 256)
            return fail(error, QStringLiteral("Enter a bounded SSH host, port, username and password."));
        Operation operation(state, std::move(cancel), error, true);
        if (!operation.current()) return false;
        if (!libraryReady()) return fail(error, QStringLiteral("The bundled SSH library could not be initialized."));
        state->socket.connectToHost(connection.host.trimmed(), connection.port);
        while (state->socket.state() != QAbstractSocket::ConnectedState) {
            if (state->socket.state() == QAbstractSocket::UnconnectedState) {
                state->stop(); return fail(error, QStringLiteral("Could not connect to the SSH server."));
            }
            if (!operation.wait(true)) return false;
        }
        state->ssh = libssh2_session_init_ex(nullptr, nullptr, nullptr, state.get());
        if (!state->ssh) { state->stop(); return fail(error, QStringLiteral("Cannot allocate an SSH session.")); }
        libssh2_session_set_blocking(state->ssh, 0);
        libssh2_session_callback_set2(state->ssh, LIBSSH2_CALLBACK_SEND,
            reinterpret_cast<libssh2_cb_generic *>(&State::send));
        libssh2_session_callback_set2(state->ssh, LIBSSH2_CALLBACK_RECV,
            reinterpret_cast<libssh2_cb_generic *>(&State::receive));
        for (;;) {
            if (!operation.current()) return false;
            const int result = libssh2_session_handshake(state->ssh, libssh2_socket_t(state->socket.socketDescriptor()));
            if (!result) break;
            if (result != LIBSSH2_ERROR_EAGAIN) { operation.nativeError(QStringLiteral("SSH handshake"), result); state->stop(); return false; }
            if (!operation.wait()) return false;
        }
        if (!operation.current()) return false;
        size_t keyLength = 0; int keyType = 0;
        const char *rawKey = libssh2_session_hostkey(state->ssh, &keyLength, &keyType); Q_UNUSED(keyType);
        if (!rawKey || !keyLength || keyLength > 64 * 1024) { state->stop(); return fail(error, QStringLiteral("SSH server did not provide a bounded host key.")); }
        const QByteArray key(rawKey, int(keyLength)); int position = 0; const auto algorithm = sshString(key, &position);
        const QString presented = fingerprint(key);
        if (!fingerprintsEqual(trustedFingerprint, presented)) {
            if (challenge) *challenge = {connection.host.trimmed(), connection.port, QString::fromLatin1(algorithm),
                keyBits(key, algorithm, position), trustedFingerprint.trimmed(), presented};
            state->stop();
            return fail(error, trustedFingerprint.trimmed().isEmpty() ? QStringLiteral("The SSH server host key has not been trusted yet.")
                                                          : QStringLiteral("The SSH server host key does not match the trusted key."));
        }
        // Password bytes are not even created until the exact server key is
        // trusted. Neither SSH diagnostics nor process arguments contain it.
        SecretBytes password{connection.password.toUtf8()}; const auto user = connection.username.toUtf8();
        if (password.value.size() > 64 * 1024) {
            state->stop(); return fail(error, QStringLiteral("The UTF-8 password exceeds the 64 KiB safety limit."));
        }
        for (;;) {
            if (!operation.current()) return false;
            const int result = libssh2_userauth_password_ex(state->ssh, user.constData(), unsigned(user.size()),
                password.value.constData(), unsigned(password.value.size()), nullptr);
            if (!result) break;
            if (result != LIBSSH2_ERROR_EAGAIN) { state->stop(); return fail(error, QStringLiteral("SSH password authentication failed.")); }
            if (!operation.wait()) return false;
        }
        cleanse(password.value);
        for (;;) {
            if (!operation.current()) return false;
            state->sftp = libssh2_sftp_init(state->ssh);
            if (state->sftp) break;
            const int result = libssh2_session_last_errno(state->ssh);
            if (result != LIBSSH2_ERROR_EAGAIN) { operation.nativeError(QStringLiteral("SFTP initialization"), result); state->stop(); return false; }
            if (!operation.wait()) return false;
        }
        if (!operation.current()) return false;
        state->connected = true; libssh2_keepalive_config(state->ssh, 0, 15); state->keepAlive.start();
        return true;
    }

    bool listLogs(const QString &requested, QVector<SftpLogEntry> *entries, QString *error, Cancel cancel) override {
        const auto state = m_state; const QString requestedDirectory = requested;
        if (entries) entries->clear();
        Operation operation(state, std::move(cancel), error);
        if (!operation.current()) return false;
        if (!entries) return fail(error, QStringLiteral("A log listing destination is required."));
        const QString directory = normalizeDirectory(requestedDirectory, error); if (directory.isEmpty()) return false;
        const auto path = directory.toUtf8();
        for (;;) {
            if (!operation.current()) return false;
            state->handle = libssh2_sftp_open_ex(state->sftp, path.constData(), unsigned(path.size()), 0, 0, LIBSSH2_SFTP_OPENDIR);
            if (state->handle) break;
            const int result = libssh2_session_last_errno(state->ssh);
            if (result != LIBSSH2_ERROR_EAGAIN) return operation.nativeError(QStringLiteral("Remote directory open"), result);
            if (!operation.wait()) return false;
        }
        QVector<SftpLogEntry> found; QSet<QString> names; int scanned = 0;
        char nameBuffer[MaximumPathBytes + 1];
        for (;;) {
            if (!operation.current()) return false;
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            const int result = libssh2_sftp_readdir_ex(state->handle, nameBuffer, sizeof(nameBuffer), nullptr, 0, &attributes);
            if (result == LIBSSH2_ERROR_EAGAIN) { if (!operation.wait()) return false; continue; }
            if (result < 0) { const bool failed = operation.nativeError(QStringLiteral("Remote directory read"), result); state->stop(); return failed; }
            if (!result) break;
            if (++scanned > MaximumDirectoryRecords || result > MaximumPathBytes) {
                state->stop(); return fail(error, QStringLiteral("Remote directory exceeds the bounded record/name limit."));
            }
            const QByteArray raw(nameBuffer, result); const auto name = QString::fromUtf8(raw);
            if (name.toUtf8() != raw || !isSafeName(name) || !name.endsWith(".bin", Qt::CaseInsensitive) || !regularAttributes(attributes)) continue;
            if (found.size() >= MaximumEntries || names.contains(name)) {
                state->stop(); return fail(error, QStringLiteral("Remote BIN listing exceeds 20000 entries or contains duplicate names."));
            }
            names.insert(name); found.append({directory, name, qint64(attributes.filesize), modified(attributes)});
        }
        if (!closeHandle(operation)) return false;
        std::sort(found.begin(), found.end(), [](const SftpLogEntry &a, const SftpLogEntry &b) {
            return a.lastWriteTimeUtc == b.lastWriteTimeUtc ? a.name < b.name : a.lastWriteTimeUtc > b.lastWriteTimeUtc;
        });
        if (!operation.current()) return false;
        *entries = std::move(found); return true;
    }

    bool download(const SftpLogEntry &requested, QIODevice *destination, qint64 *copied,
                  QString *error, Cancel cancel, Progress progress) override {
        const auto state = m_state; const auto entry = requested;
        if (copied) *copied = 0;
        Operation operation(state, std::move(cancel), error);
        if (!operation.current()) return false;
        QPointer<QIODevice> sink(destination);
        if (!sink || !sink->isOpen() || !sink->isWritable()) return fail(error, QStringLiteral("A writable download destination is required."));
        QByteArray path;
        LIBSSH2_SFTP_ATTRIBUTES beforeOpen{}, opened{}, afterOpen{};
        if (!revalidate(operation, entry, false, &path, &beforeOpen)) return false;
        for (;;) {
            if (!operation.current()) return false;
            state->handle = libssh2_sftp_open_ex(state->sftp, path.constData(), unsigned(path.size()), LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
            if (state->handle) break;
            const int result = libssh2_session_last_errno(state->ssh);
            if (result != LIBSSH2_ERROR_EAGAIN) return operation.nativeError(QStringLiteral("Remote log open"), result);
            if (!operation.wait()) return false;
        }
        for (;;) {
            if (!operation.current()) return false;
            const int result = libssh2_sftp_fstat_ex(state->handle, &opened, 0);
            if (!result) break;
            if (result != LIBSSH2_ERROR_EAGAIN) { operation.nativeError(QStringLiteral("Opened log metadata read"), result); state->stop(); return false; }
            if (!operation.wait()) return false;
        }
        if (!compatibleDownloadAttributes(beforeOpen, opened)) {
            if (state->generation == operation.generation) state->stop();
            return fail(error, QStringLiteral("The opened log has inconsistent type, size or modification time compared with its path before opening; refresh before retrying."));
        }
        if (!lstat(operation, path, &afterOpen)) {
            if (state->generation == operation.generation) state->stop();
            return false;
        }
        if (!compatibleDownloadAttributes(opened, afterOpen)) {
            state->stop(); return fail(error, QStringLiteral("The log path became linked or has inconsistent size or modification time compared with the opened handle; refresh before retrying."));
        }
        QByteArray buffer(int(BufferSize), '\0'); qint64 completed = 0;
        for (;;) {
            if (!operation.current()) return false;
            const auto count = libssh2_sftp_read(state->handle, buffer.data(), size_t(buffer.size()));
            if (count == LIBSSH2_ERROR_EAGAIN) { if (!operation.wait()) return false; continue; }
            if (count < 0) { operation.nativeError(QStringLiteral("Remote log read"), int(count)); state->stop(); return false; }
            if (!count) break;
            if (count > MaximumFileBytes - completed) { state->stop(); return fail(error, QStringLiteral("Growing remote log exceeded the 16 GiB download limit.")); }
            qint64 position = 0;
            while (position < count) {
                if (!operation.current()) return false;
                if (!sink) { state->stop(); return fail(error, QStringLiteral("Download destination was closed or destroyed.")); }
                const qint64 written = sink->write(buffer.constData() + position, count - position);
                if (written <= 0 || written > count - position) { state->stop(); return fail(error, QStringLiteral("Writing the local download failed.")); }
                position += written; completed += written;
                if (copied) *copied = completed;
                if (state->generation != operation.generation) return fail(error, QStringLiteral("SFTP download was stopped during the local write."));
            }
            if (progress) progress(completed);
        }
        return closeHandle(operation);
    }

    bool remove(const SftpLogEntry &requested, QString *error, Cancel cancel) override {
        const auto state = m_state; const auto entry = requested;
        Operation operation(state, std::move(cancel), error);
        if (!operation.current()) return false;
        QByteArray path; LIBSSH2_SFTP_ATTRIBUTES attributes{};
        if (!revalidate(operation, entry, true, &path, &attributes)) return false;
        bool submitted = false;
        for (;;) {
            if (!operation.current()) {
                if (submitted) fail(error, QStringLiteral("Delete interrupted after submission; the remote outcome is unknown. Refresh before retrying."));
                return false;
            }
            submitted = true;
            const int result = libssh2_sftp_unlink_ex(state->sftp, path.constData(), unsigned(path.size()));
            if (!result) return true; // Authoritative SFTP status, even if cancellation races afterwards.
            if (result != LIBSSH2_ERROR_EAGAIN) return operation.nativeError(QStringLiteral("Remote log delete"), result, true);
            if (!operation.wait()) {
                fail(error, QStringLiteral("Delete interrupted after submission; the remote outcome is unknown. Refresh before retrying.")); return false;
            }
        }
    }
};
}

QString SftpLogEntry::remotePath() const {
    const auto directory = SftpLogSession::normalizeDirectory(remoteDirectory);
    if (directory.isEmpty() || !SftpLogSession::isSafeName(name)) return {};
    return directory == "/" ? directory + name : directory + '/' + name;
}
bool SftpLogSession::parseEndpoint(const QString &input, int defaultPort, QString *host,
                                 quint16 *port, QString *error) {
    if (host) host->clear(); if (port) *port = 0; if (error) error->clear();
    QString value = input.trimmed(); int resultPort = defaultPort;
    if (value.isEmpty() || value.size() > 1024 || value.toUtf8().size() > 1024 || value.contains(QChar(0)) || defaultPort < 1 || defaultPort > 65535)
        return fail(error, QStringLiteral("Enter an SSH host and a port between 1 and 65535."));
    const auto parsePort = [&](const QString &text) {
        bool ok = false; resultPort = text.toInt(&ok); return ok && resultPort >= 1 && resultPort <= 65535;
    };
    if (value.startsWith('[')) {
        const int closing = value.indexOf(']');
        if (closing < 2) return fail(error, QStringLiteral("Bracketed SSH endpoint is invalid."));
        const auto suffix = value.mid(closing + 1);
        if (!suffix.isEmpty() && (!suffix.startsWith(':') || !parsePort(suffix.mid(1))))
            return fail(error, QStringLiteral("SSH port is invalid."));
        value = value.mid(1, closing - 1);
    } else if (value.indexOf(':') >= 0 && value.indexOf(':') == value.lastIndexOf(':')) {
        const int colon = value.indexOf(':');
        if (!parsePort(value.mid(colon + 1))) return fail(error, QStringLiteral("SSH port is invalid."));
        value = value.left(colon).trimmed();
    }
    if (value.isEmpty()) return fail(error, QStringLiteral("SSH host is missing."));
    if (host) *host = value; if (port) *port = quint16(resultPort); return true;
}
QString SftpLogSession::normalizeDirectory(const QString &value, QString *error) {
    if (error) error->clear(); const auto path = value.trimmed();
    if (!path.startsWith('/') || path.size() > MaximumPathBytes || path.contains(QChar(0)) || path.toUtf8().size() > MaximumPathBytes) {
        fail(error, QStringLiteral("Remote log directory must be an absolute POSIX path of at most 4096 UTF-8 bytes.")); return {};
    }
    const auto parts = path.split('/', Qt::SkipEmptyParts);
    for (const auto &part : parts) if (part == "." || part == "..") {
        fail(error, QStringLiteral("Remote log directory cannot contain '.' or '..' segments.")); return {};
    }
    return '/' + parts.join('/');
}
bool SftpLogSession::isSafeName(const QString &value) {
    return !value.isEmpty() && value.size() <= MaximumPathBytes && value != "." && value != ".." && !value.contains('/')
        && !value.contains(QChar(0)) && value.toUtf8().size() <= MaximumPathBytes;
}
QString SftpLogSession::trustedKeySettingName(const QString &host, quint16 port) {
    const auto identity = host.trimmed().toLower() + ':' + QString::number(port);
    return QStringLiteral("SSHHostKey_") + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().toUpper());
}
QString SftpLogSession::fingerprint(const QByteArray &hostKey) {
    QByteArray digest = QCryptographicHash::hash(hostKey, QCryptographicHash::Sha256).toBase64();
    while (digest.endsWith('=')) digest.chop(1);
    return QStringLiteral("SHA256:") + QString::fromLatin1(digest);
}
bool SftpLogSession::fingerprintsEqual(const QString &left, const QString &right) {
    const auto expected = left.trimmed().toLatin1(), presented = right.trimmed().toLatin1();
    if (expected.isEmpty() || expected.size() != presented.size()) return false;
    volatile unsigned char difference = 0;
    for (int i = 0; i < expected.size(); ++i) difference |= static_cast<unsigned char>(expected.at(i) ^ presented.at(i));
    return difference == 0;
}
bool SftpLogSession::compatibleDownloadMetadata(const SftpLogEntry &before,
                                              const SftpLogEntry &after) {
    if (before.length < 0 || after.length < before.length || after.length > MaximumFileBytes
        || !before.lastWriteTimeUtc.isValid() || !after.lastWriteTimeUtc.isValid()) return false;
    const qint64 initialMs = before.lastWriteTimeUtc.toMSecsSinceEpoch();
    const qint64 finalMs = after.lastWriteTimeUtc.toMSecsSinceEpoch();
    // SFTP v3 timestamps are unsigned, whole-second values.
    constexpr qint64 MaximumTimeMs = qint64(std::numeric_limits<quint32>::max()) * 1000;
    if (initialMs < 0 || finalMs < initialMs || finalMs > MaximumTimeMs
        || initialMs % 1000 || finalMs % 1000) return false;
    return after.length > before.length || initialMs == finalMs;
}
std::unique_ptr<SftpLogSession> createSftpLogSession() { return std::make_unique<Libssh2SftpLogSession>(); }
