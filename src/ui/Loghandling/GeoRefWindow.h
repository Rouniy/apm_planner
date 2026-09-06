#ifndef GEOREFWINDOW_H
#define GEOREFWINDOW_H

#include "GeoRefService.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class QCheckBox;
class QCloseEvent;
class QDialog;
class QDoubleSpinBox;
class QFileDialog;
class QFutureWatcherBase;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTableWidget;
class QTimer;

/**
 * Modeless, offline Geo Reference Images workflow.
 *
 * Matching and image publication are value-only worker operations.  The
 * window owns at most one worker and never lets a worker capture a QWidget.
 */
class GeoRefWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Operations {
        std::function<GeoRefService::PlanResult(
            const GeoRefService::Options &, const GeoRefService::Cancel &,
            const GeoRefService::Progress &)> prepare;
        std::function<GeoRefService::EstimateResult(
            const GeoRefService::Options &, const GeoRefService::Cancel &,
            const GeoRefService::Progress &)> estimate;
        std::function<GeoRefService::Result(
            const GeoRefService::Plan &, const GeoRefService::Cancel &,
            const GeoRefService::Progress &)> execute;
    };

    explicit GeoRefWindow(QWidget *parent = nullptr);
    ~GeoRefWindow() override;

    bool isBusy() const noexcept;
    bool isClosing() const noexcept;
    QString sourcePath() const;
    QString statusText() const;

    void setSource(const QString &path);
    void setOperationsForTesting(const Operations &operations);

public slots:
    void requestShutdown();

signals:
    void busyChanged(bool busy);
    void shutdownReady();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum class Phase { Idle, LogPrompt, PhotoPrompt, OutputPrompt, Working, Consent };
    enum class Work { None, Estimate, Prepare, Execute };
    struct JobState;
    struct WorkerResult;

    static Operations defaultOperations();
    void buildUi();
    void browseLog();
    void browsePhotoDirectory();
    void browseOutputDirectory();
    void showFilePrompt(Phase phase);
    void acceptFilePrompt(quint64 flow, Phase phase, const QString &path);
    void beginEstimate();
    void beginPrepare();
    void startWorker(quint64 flow, Work work,
                     const GeoRefService::Options &options,
                     std::shared_ptr<const GeoRefService::Plan> plan = {});
    void finishWorker(quint64 flow, Work work,
                      const std::shared_ptr<JobState> &state,
                      const WorkerResult &result);
    void showConsent(quint64 flow,
                     const std::shared_ptr<const GeoRefService::Plan> &plan,
                     const QStringList &warnings);
    void beginExecute(quint64 flow,
                      const std::shared_ptr<const GeoRefService::Plan> &plan);
    void cancelCurrent();
    void updateProgress();
    void refreshControls();
    void invalidateResults();
    void populateResults(const QVector<GeoRefService::Match> &matches);
    GeoRefService::Options currentOptions() const;
    bool beginPhase(Phase phase, quint64 *flowOut);
    void finishFlow(quint64 flow, const QString &status,
                    const QStringList &messages = {});
    void resolveDeferredClose();
    bool appendLog(const QString &line);
    bool setStatus(const QString &text);
    bool dismissDialog(QPointer<QDialog> &stored);
    void dismissPrompts();

    Operations m_operations;
    QPointer<QFileDialog> m_filePrompt;
    QPointer<QDialog> m_consent;
    QPointer<QDialog> m_progressDialog;
    QPointer<QFutureWatcherBase> m_watcher;
    std::shared_ptr<JobState> m_job;
    std::shared_ptr<const GeoRefService::Plan> m_plan;
    QTimer *m_progressTimer = nullptr;

    QLineEdit *m_logPath = nullptr;
    QPushButton *m_browseLog = nullptr;
    QLineEdit *m_photoDirectory = nullptr;
    QPushButton *m_browsePhoto = nullptr;
    QLineEdit *m_outputDirectory = nullptr;
    QPushButton *m_browseOutput = nullptr;
    QRadioButton *m_camMode = nullptr;
    QRadioButton *m_trigMode = nullptr;
    QRadioButton *m_timeOffsetMode = nullptr;
    QDoubleSpinBox *m_timeOffset = nullptr;
    QCheckBox *m_useGps2 = nullptr;
    QSpinBox *m_shutterLag = nullptr;
    QCheckBox *m_useAmslAltitude = nullptr;
    QCheckBox *m_useGpsAltitude = nullptr;
    QDoubleSpinBox *m_baseAltitude = nullptr;
    QPushButton *m_geoTag = nullptr;
    QPushButton *m_estimate = nullptr;
    QPushButton *m_cancel = nullptr;
    QLabel *m_status = nullptr;
    QTableWidget *m_results = nullptr;
    QPlainTextEdit *m_outputLog = nullptr;

    quint64 m_flow = 0;
    Phase m_phase = Phase::Idle;
    Work m_work = Work::None;
    bool m_cancelReported = false;
    bool m_closeWhenIdle = false;
    bool m_allowClose = false;
    bool m_closing = false;
    bool m_shutdownPending = false;
    bool m_shutdownReadyEmitted = false;
    bool m_destroying = false;
};

#endif // GEOREFWINDOW_H
