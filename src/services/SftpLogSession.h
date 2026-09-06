#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

class QIODevice;

struct SftpLogConnection {
    QString host;
    quint16 port = 22;
    QString username;
    QString password; // Transient only. Never settings, logs, process arguments.
};
struct SftpLogEntry {
    QString remoteDirectory;
    QString name;
    qint64 length = 0;
    QDateTime lastWriteTimeUtc;
    QString remotePath() const;
};
struct SshHostKeyChallenge {
    QString host;
    quint16 port = 22;
    QString algorithm;
    int keyLength = 0;
    QString expectedFingerprint;
    QString presentedFingerprint;
    bool isChanged() const { return !expectedFingerprint.isEmpty(); }
};

// All methods including destruction execute on ONE dedicated worker thread.
// Cancellation callbacks must be cheap, thread-safe, and free of GUI access.
// Host-key rejection happens before password authentication. An untrusted key
// returns false plus a nonempty challenge; explicit trust requires reconnect.
class SftpLogSession {
public:
    using Cancel = std::function<bool()>;
    using Progress = std::function<void(qint64)>;
    virtual ~SftpLogSession() = default;
    virtual bool isConnected() const = 0;
    virtual bool connect(const SftpLogConnection &connection, const QString &trustedFingerprint,
                         SshHostKeyChallenge *challenge, QString *error, Cancel cancel = {}) = 0;
    virtual bool listLogs(const QString &remoteDirectory, QVector<SftpLogEntry> *entries,
                          QString *error, Cancel cancel = {}) = 0;
    virtual bool download(const SftpLogEntry &entry, QIODevice *destination, qint64 *copied,
                          QString *error, Cancel cancel = {}, Progress progress = {}) = 0;
    virtual bool remove(const SftpLogEntry &entry, QString *error, Cancel cancel = {}) = 0;
    virtual void stop() = 0;

    static constexpr int MaximumEntries = 20000;
    static constexpr qint64 MaximumFileBytes = 16LL * 1024 * 1024 * 1024;
    static bool parseEndpoint(const QString &input, int defaultPort, QString *host,
                              quint16 *port, QString *error = nullptr);
    static QString normalizeDirectory(const QString &value, QString *error = nullptr);
    static bool isSafeName(const QString &value);
    static QString trustedKeySettingName(const QString &host, quint16 port);
    static QString fingerprint(const QByteArray &hostKey);
    static bool fingerprintsEqual(const QString &left, const QString &right);
    // Size/mtime consistency only, not file-identity proof or authorization.
    // Allows monotonic append growth; equal size requires unchanged mtime.
    // Paths and regular/non-symlink type must be checked independently.
    static bool compatibleDownloadMetadata(const SftpLogEntry &before,
                                           const SftpLogEntry &after);
};

using SftpLogSessionFactory = std::function<std::unique_ptr<SftpLogSession>()>;
std::unique_ptr<SftpLogSession> createSftpLogSession();
