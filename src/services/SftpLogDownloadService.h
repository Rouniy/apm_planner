#pragma once

#include "SftpLogDownloadSupport.h"
#include "SftpLogSession.h"

#include <QObject>
#include <QStringList>

#include <memory>

class QThread;

/**
 * Owner-thread facade for one persistent SFTP session. The session itself is
 * constructed, called, stopped and destroyed exclusively on one dedicated
 * worker QThread. Passwords are transient operation inputs only.
 */
class SftpLogDownloadService final : public QObject
{
    Q_OBJECT

public:
    enum class Operation { None, Refresh, Download, Delete };
    Q_ENUM(Operation)

    struct ListResult {
        bool success = false;
        bool cancelled = false;
        QString error;
        QVector<SftpLogEntry> entries;
    };
    struct DeleteResult {
        bool success = false;
        bool cancelled = false;
        QString error;
        int requested = 0;
        QVector<SftpLogEntry> deletedEntries;
        QStringList failedPaths;
    };

    explicit SftpLogDownloadService(
        SftpLogSessionFactory factory = createSftpLogSession,
        QObject *parent = nullptr);
    ~SftpLogDownloadService() override;

    bool busy() const noexcept { return m_operationId != 0 || m_finishing; }
    bool connected() const noexcept { return m_connected; }
    quint64 operationId() const noexcept { return m_operationId; }
    Operation operation() const noexcept { return m_operation; }
    QString status() const;
    qint64 progressCompleted() const noexcept;
    qint64 progressTotal() const noexcept;
    bool awaitingHostKeyTrust() const noexcept { return m_awaitingTrust; }
    SshHostKeyChallenge hostKeyChallenge() const { return m_challenge; }
    ListResult lastListResult() const { return m_listResult; }
    SftpLogDownloadSupport::Result lastDownloadResult() const { return m_downloadResult; }
    DeleteResult lastDeleteResult() const { return m_deleteResult; }

    bool refresh(const SftpLogConnection &connection,
                 const QString &remoteDirectory,
                 quint64 *operationIdOut = nullptr,
                 QString *error = nullptr);
    bool download(const SftpLogConnection &connection,
                  const QVector<SftpLogEntry> &entries,
                  const QString &destinationDirectory, bool createKml,
                  quint64 *operationIdOut = nullptr,
                  QString *error = nullptr);
    bool remove(const SftpLogConnection &connection,
                const QVector<SftpLogEntry> &entries,
                quint64 *operationIdOut = nullptr,
                QString *error = nullptr);
    bool trustHostKey(quint64 operationId, QString *error = nullptr);
    bool rejectHostKey(quint64 operationId);
    bool cancel(quint64 operationId);
    void shutdown();

signals:
    void stateChanged();
    void hostKeyChallengeAvailable(quint64 operationId);
    void operationFinished(quint64 operationId);

private:
    struct SharedState;
    struct Request;
    struct WorkerResult;
    class Worker;

    bool start(std::shared_ptr<Request> request, quint64 *operationIdOut,
               QString *error);
    void dispatch(const QString &trustedFingerprint);
    void handleWorkerResult(quint64 operationId,
                            const std::shared_ptr<Request> &request,
                            const std::shared_ptr<SharedState> &state,
                            WorkerResult result);
    void finish(quint64 operationId, WorkerResult result);
    QString trustedFingerprint(const SftpLogConnection &connection) const;
    bool persistTrust(const SshHostKeyChallenge &challenge, QString *error);
    void clearActive();

    QThread *m_thread = nullptr;
    Worker *m_worker = nullptr;
    std::shared_ptr<Request> m_request;
    std::shared_ptr<SharedState> m_state;
    ListResult m_listResult;
    SftpLogDownloadSupport::Result m_downloadResult;
    DeleteResult m_deleteResult;
    SshHostKeyChallenge m_challenge;
    quint64 m_nextOperationId = 0;
    quint64 m_operationId = 0;
    Operation m_operation = Operation::None;
    bool m_connected = false;
    bool m_awaitingTrust = false;
    bool m_finishing = false;
    bool m_shuttingDown = false;
    QString m_idleStatus;
    QString m_startupError;
};
