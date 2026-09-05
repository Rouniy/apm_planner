#ifndef MAVFTPFILEDOWNLOAD_H
#define MAVFTPFILEDOWNLOAD_H

#include "comm/MavFtpServiceInterface.h"

#include <QObject>
#include <QPointer>
#include <QDateTime>

#include <memory>

class QDialog;
class QEvent;
class QProgressDialog;
class QTimer;
class VehicleTargetManager;
class QWidget;

/**
 * Mission Planner Developer Tools' single-file MAVFTP workflow.
 *
 * The controller owns only the operation it admitted.  It pins one exact
 * target before displaying either picker, never cancels another MAVFTP user,
 * and publishes successful bytes through QSaveFile on a worker thread.
 */
class MavFtpFileDownload final : public QObject
{
    Q_OBJECT

public:
    explicit MavFtpFileDownload(MavFtpServiceInterface *service,
                                VehicleTargetManager *targetManager,
                                QWidget *dialogParent);
    ~MavFtpFileDownload() override;

    bool busy() const { return m_phase != Phase::Idle; }
    void setTimeoutForTesting(int timeoutMs);

public slots:
    void start();
    void cancel();

signals:
    void busyChanged();
    void logMessage(const QString &message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Phase {
        Idle,
        PathPrompt,
        OutputPrompt,
        Starting,
        RemoteTransfer,
        LocalWrite
    };

    struct SaveState;
    struct OutputSnapshot {
        QString path;
        QString canonicalParent;
        bool existed = false;
        qint64 size = -1;
        QDateTime lastModified;
    };

    bool pinnedTargetIsCurrent() const;
    bool emitLog(const QString &message);
    void openPathPrompt(quint64 flow);
    void openOutputPrompt(quint64 flow);
    bool captureOutput(const QString &selected, OutputSnapshot *snapshot,
                       QString *error) const;
    void beginRemoteTransfer(quint64 flow);
    void beginLocalWrite(const QByteArray &data);
    void handleFinished(const MavFtpServiceInterface::Result &result);
    void handleProgress(qulonglong operationId, qulonglong generation,
                        qint64 completed, qint64 total);
    void handleTargetChanged();
    void handleDeadline();
    void requestOwnedCancellation(const QString &reason);
    void finish(QString message);
    void discardPrompt();
    void discardProgress();
    static QString startFailureText(MavFtpServiceInterface::StartResult result);

    QPointer<MavFtpServiceInterface> m_service;
    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<QWidget> m_dialogParent;
    QPointer<QDialog> m_prompt;
    QPointer<QProgressDialog> m_progress;
    QTimer *m_deadline = nullptr;
    VehicleTargetLease m_target;
    OutputSnapshot m_output;
    QString m_remotePath;
    QString m_cancelReason;
    std::shared_ptr<SaveState> m_saveState;
    quint64 m_flow = 0;
    quint64 m_operationId = 0;
    int m_timeoutMs = 30000;
    Phase m_phase = Phase::Idle;
};

#endif // MAVFTPFILEDOWNLOAD_H
