#ifndef FIRMWAREARCHIVECONTROLLER_H
#define FIRMWAREARCHIVECONTROLLER_H

#include "services/FirmwareArchiveTypes.h"

#include <QObject>
#include <QPointer>

#include <memory>

class QDialog;
class QEvent;
class QFileDialog;
class QFutureWatcherBase;
class QProgressDialog;
class QTimer;
class QWidget;

class FirmwareArchiveController final : public QObject
{
    Q_OBJECT

public:
    explicit FirmwareArchiveController(QWidget *owner);
    ~FirmwareArchiveController() override;

    bool busy() const noexcept;

    // Test/runtime-audit seam. Normal instances use the official manifests
    // and the bounded FirmwareArchiveHttp transport. Changes while busy are
    // deliberately ignored so an admitted operation stays immutable.
    void setBackend(QVector<QUrl> manifests, FirmwareArchive::Fetch fetch);

public slots:
    void start();
    void cancel();

signals:
    void busyChanged(bool busy);
    void logMessage(const QString &message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Phase { Idle, DirectoryPrompt, ConsentPrompt, Worker };
    struct JobState;

    void showDirectoryPrompt(quint64 flow);
    void showConsentPrompt(quint64 flow);
    void startWorker(quint64 flow);
    void finishPrompt(quint64 flow, const QString &message);
    void finishWorker(quint64 flow,
                      const std::shared_ptr<JobState> &state,
                      const FirmwareArchive::Result &result);
    bool dismissPrompt();
    bool dismissProgress();
    void updateProgress();
    bool emitLog(const QString &message);
    static QString resultSummary(const FirmwareArchive::Result &result);

    QPointer<QWidget> m_owner;
    QPointer<QDialog> m_prompt;
    QPointer<QProgressDialog> m_progress;
    QPointer<QFutureWatcherBase> m_watcher;
    QTimer *m_progressTimer = nullptr;
    QVector<QUrl> m_manifests;
    FirmwareArchive::Fetch m_fetch;
    std::shared_ptr<JobState> m_job;
    QString m_destination;
    quint64 m_flow = 0;
    int m_lastReportedProgress = 0;
    bool m_cancelReported = false;
    bool m_destroying = false;
    Phase m_phase = Phase::Idle;
};

#endif // FIRMWAREARCHIVECONTROLLER_H
