#ifndef DATAFLASHLOGTOOLSCONTROLLER_H
#define DATAFLASHLOGTOOLSCONTROLLER_H

#include "ui/Loghandling/DataFlashBinToLogConverter.h"
#include "ui/Loghandling/DataFlashKmlExporter.h"
#include "ui/Loghandling/DataFlashGpxExporter.h"
#include "ui/Loghandling/DataFlashLogAnalyzer.h"
#include "ui/Loghandling/DataFlashMatlabExporter.h"
#include "ui/Loghandling/FlightLogOrganizer.h"

#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>

class DataFlashLogsWidget;
class QDialog;
class QFileDialog;
class QFutureWatcherBase;
class QProgressDialog;
class QTimer;
class QWidget;

class DataFlashLogToolsController final : public QObject
{
    Q_OBJECT

public:
    struct Operations {
        std::function<DataFlashBinToLogConverter::Result(
            const QString &, const QString &,
            const DataFlashBinToLogConverter::CancelCheck &,
            const DataFlashBinToLogConverter::Progress &)> convertBinToLog;
        std::function<DataFlashKmlExporter::Result(
            const QString &, const QString &,
            const DataFlashKmlExporter::CancellationCheck &)> exportKml;
        std::function<DataFlashGpxExporter::Result(
            const QString &, const QString &,
            const DataFlashGpxExporter::CancelCheck &,
            const DataFlashGpxExporter::Progress &)> exportGpx;
        std::function<DataFlashLogAnalyzer::Result(
            const QString &, DataFlashLogAnalyzer::Cancel,
            DataFlashLogAnalyzer::Progress)> analyze;
        std::function<DataFlashMatlabExporter::PlanResult(
            const QString &, const DataFlashMatlabExporter::CancelCheck &,
            const DataFlashMatlabExporter::Progress &)> prepareMatlab;
        std::function<DataFlashMatlabExporter::Result(
            const DataFlashMatlabExporter::Plan &,
            const DataFlashMatlabExporter::CancelCheck &,
            const DataFlashMatlabExporter::Progress &)> exportMatlab;
        std::function<FlightLogOrganizer::Analysis(
            const QString &, const FlightLogOrganizer::Cancel &,
            const FlightLogOrganizer::Progress &)> analyzeDirectory;
        std::function<FlightLogOrganizer::Result(
            const FlightLogOrganizer::Plan &,
            const FlightLogOrganizer::Cancel &,
            const FlightLogOrganizer::Progress &)> executeOrganizer;
    };

    explicit DataFlashLogToolsController(DataFlashLogsWidget *widget,
                                         QWidget *dialogParent = nullptr);
    ~DataFlashLogToolsController() override;

    bool busy() const noexcept;
    bool shutdownPending() const noexcept;
    QString selectedLogPath() const;

    void setSelectedLogPath(const QString &path);
    void setOperationsForTesting(const Operations &operations);

public slots:
    void startReview();
    void startAutoAnalysis();
    void startKmlGpx();
    void startBinToLog();
    void startMatlab();
    void startOrganize();
    void cancel();
    void shutdown();

signals:
    void busyChanged(bool busy);
    void shutdownReady();
    void logMessage(const QString &message);
    void reviewLogRequested(const QString &path);

private:
    enum class Intent { None, Review, Analyze, KmlGpx, BinToLog, Matlab, Organize };
    enum class Phase {
        Idle,
        InputPrompt,
        OutputPrompt,
        Consent,
        DirectoryPrompt,
        Working
    };
    enum class Work {
        None, Analyze, KmlGpx, BinToLog, MatlabPrepare, MatlabExport,
        OrganizeAnalyze, OrganizeExecute
    };
    struct JobState;
    struct WorkerResult;

    void begin(Intent intent);
    void showInputPrompt(quint64 flow, Intent intent);
    void acceptInput(quint64 flow, Intent intent, const QString &path);
    void showBinOutputPrompt(quint64 flow);
    void showKmlGpxConsent(quint64 flow);
    void showMatlabConsent(quint64 flow);
    void showOrganizerDirectoryPrompt(quint64 flow);
    void showOrganizerConsent(quint64 flow);
    void startWorker(quint64 flow, Work work);
    void finishWorker(quint64 flow, const std::shared_ptr<JobState> &state,
                      const WorkerResult &result);
    void showAnalysisReport(const DataFlashLogAnalyzer::Result &result);
    void finishFlow(quint64 flow, const QStringList &messages = {});
    bool dismissPrompt();
    bool dismissProgress();
    void showProgress(const QString &label);
    void updateProgress();
    bool publishLog(const QString &message);
    void setWidgetStatus(const QString &message);
    bool selectedPathUsableFor(Intent intent) const;
    QStringList workerMessages(const WorkerResult &result) const;
    static QString suggestedLogOutput(const QString &input);
    static QStringList suggestedMapOutputs(const QString &input);
    static Operations defaultOperations();

    QPointer<DataFlashLogsWidget> m_widget;
    QPointer<QWidget> m_dialogParent;
    QPointer<QDialog> m_prompt;
    QPointer<QDialog> m_report;
    QPointer<QProgressDialog> m_progress;
    QPointer<QFutureWatcherBase> m_watcher;
    QTimer *m_progressTimer = nullptr;
    Operations m_operations;
    std::shared_ptr<JobState> m_job;
    FlightLogOrganizer::Plan m_organizerPlan;
    std::shared_ptr<const DataFlashMatlabExporter::Plan> m_matlabPlan;
    QStringList m_matlabWarnings;
    QString m_selectedLog;
    QString m_outputPath;
    QStringList m_mapOutputs;
    QString m_mapParentCanonical;
    QString m_organizerRoot;
    quint64 m_flow = 0;
    Intent m_intent = Intent::None;
    Work m_work = Work::None;
    Phase m_phase = Phase::Idle;
    bool m_shutdownPending = false;
    bool m_destroying = false;
    bool m_cancelReported = false;
};

#endif // DATAFLASHLOGTOOLSCONTROLLER_H
