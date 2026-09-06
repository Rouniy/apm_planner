#ifndef MAVLINKLOGWINDOW_H
#define MAVLINKLOGWINDOW_H

#include "comm/TlogExportService.h"

#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class QCloseEvent;
class QFileDialog;
class QLabel;
class QMessageBox;
class QProgressBar;
class QPushButton;
class QThread;
class QTimer;

/** Mission Planner 10 TOOLS > Tlog Convert / Extract window. */
class MavlinkLogWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        std::function<bool(const QString &label)> confirmExport;
        std::function<QString(const QString &suggested,
                              const QString &label,
                              const QString &extension)> chooseOutput;
        std::function<TlogExportResult(
            TlogExportFormat format, const QString &input,
            const QString &selectedOutput,
            const TlogExportService::CancelRequested &cancel)> exportLog;
        std::function<TlogExportResult(
            TlogExportFormat format, const QString &input,
            const QString &selectedOutput,
            const TlogExportService::CancelRequested &cancel,
            const TlogExportService::Progress &progress)> exportLogWithProgress;
    };

    static constexpr int WindowWidth = 460;
    static constexpr int WindowHeight = 340;

    explicit MavlinkLogWindow(QWidget *owner = nullptr);
    MavlinkLogWindow(Dependencies dependencies, QWidget *owner = nullptr);
    ~MavlinkLogWindow() override;

    /** Creates a new independent modeless top-level window on every call. */
    static MavlinkLogWindow *OpenWindow(QWidget *owner = nullptr);

    /** Selects a local .tlog. Empty or missing paths restore the initial state. */
    void setTlogPath(const QString &path);
    QString tlogPath() const { return m_tlogPath; }
    QString statusText() const;
    bool isBusy() const { return m_busy; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct PendingExport;
    struct ProgressState;

    enum class Operation
    {
        Kml,
        Gpx,
        Matlab,
        Csv,
        Text,
        Parameters,
        Missions
    };

    void buildUi(QWidget *owner);
    void pickTlog();
    void beginExport(Operation operation);
    void continueAfterConfirmation(
        const std::shared_ptr<PendingExport> &pending);
    void continueAfterOutput(
        const QString &output, const std::shared_ptr<PendingExport> &pending);
    void startExportWorker(
        const QString &output, const std::shared_ptr<PendingExport> &pending);
    void finishExport(const TlogExportResult &result, const QString &label,
                      quint64 flow);
    void setBusy(bool busy);
    void stopWorker();
    void cancelCurrent();
    void updateProgress();
    bool dismissDialogs();
    void finishPending(const QString &status = QString());
    QString suggestedOutput(Operation operation, const QString &input) const;
    QString operationLabel(Operation operation) const;
    QString operationExtension(Operation operation) const;
    TlogExportFormat exportFormat(Operation operation) const;

    QString m_tlogPath;
    Dependencies m_dependencies;
    bool m_busy = false;
    bool m_closing = false;
    bool m_closeWhenIdle = false;
    quint64 m_flow = 0;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    std::shared_ptr<PendingExport> m_pending;
    std::shared_ptr<ProgressState> m_progressState;
    QPointer<QThread> m_thread;
    QPointer<QFileDialog> m_inputDialog;
    QPointer<QFileDialog> m_outputDialog;
    QPointer<QMessageBox> m_confirmationDialog;
    QLabel *m_tlogName = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_pick = nullptr;
    QPushButton *m_cancel = nullptr;
    QProgressBar *m_progress = nullptr;
    QTimer *m_progressTimer = nullptr;
    QList<QPushButton *> m_exportButtons;
};

#endif // MAVLINKLOGWINDOW_H
