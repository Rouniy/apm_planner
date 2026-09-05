#ifndef LOGDOWNLOADVIEWMODEL_H
#define LOGDOWNLOADVIEWMODEL_H

#include "comm/ExactLogTransferService.h"
#include "Loghandling/DataFlashKmlExporter.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QThread;

struct LogDownloadRow
{
    quint16 id = 0;
    quint32 sizeBytes = 0;
    QDateTime timeUtc;

    QString timeText() const;
    QString sizeText() const;
};

/**
 * Window-local MP10 DataFlash download workflow over the shared exact-link
 * service. The model owns only its operation token; Cancel and destruction
 * can never cancel another window's transfer.
 */
class LogDownloadViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ isBusy NOTIFY stateChanged)
    Q_PROPERTY(bool downloading READ isDownloading NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString targetSource READ targetSource NOTIFY stateChanged)
    Q_PROPERTY(bool createKmlAfterDownload READ createKmlAfterDownload
               WRITE setCreateKmlAfterDownload NOTIFY stateChanged)

public:
    using TargetAcquirer = std::function<SwarmVehicleInstanceLease()>;
    using KmlExporter = std::function<DataFlashKmlExporter::Result(
        const QString &, const QString &,
        const DataFlashKmlExporter::CancellationCheck &)>;

    explicit LogDownloadViewModel(
        ExactLogTransferService *service,
        TargetAcquirer acquireTarget,
        KmlExporter kmlExporter = {},
        QObject *parent = nullptr);
    ~LogDownloadViewModel() override;

    QVector<LogDownloadRow> logs() const { return m_logs; }
    int selectedLogId() const { return m_selectedLogId; }
    void setSelectedLogId(int id);

    bool isBusy() const;
    bool isDownloading() const { return m_batch.active; }
    double progress() const { return m_progress; }
    QString status() const { return m_status; }
    QString targetSource() const;
    void setTargetSource(const QString &source);
    bool createKmlAfterDownload() const { return m_createKml; }
    void setCreateKmlAfterDownload(bool enabled);

    bool refresh();
    void requestDownloadAll();

    /** Claims this model's busy gate and pins the current target before UI. */
    quint64 prepareSelectedDownload(QString *suggestedFileName = nullptr);
    quint64 prepareDownloadAll();
    quint64 prepareErase();

    QStringList existingDownloadAllFiles(
        quint64 preparationId, const QString &directory) const;
    bool startPreparedSelectedDownload(
        quint64 preparationId, const QString &destination,
        bool overwriteKmlConfirmed = false);
    bool startPreparedDownloadAll(
        quint64 preparationId, const QString &directory,
        bool overwriteConfirmed);
    bool completePreparedErase(quint64 preparationId, bool confirmed);
    void abandonPreparation(quint64 preparationId);

    void cancelOwnDownload();
    void shutdown();

    static QString suggestedFileName(const LogDownloadRow &row);
    static QString kmlFileName(const QString &logFileName);

signals:
    void stateChanged();
    void logsChanged();
    /** Emitted after Download All refreshes an initially empty list. */
    void downloadAllDestinationRequested();

private:
    enum class PreparationKind {
        None,
        SelectedDownload,
        DownloadAll,
        Erase
    };

    struct Preparation
    {
        quint64 id = 0;
        PreparationKind kind = PreparationKind::None;
        SwarmVehicleInstanceLease target;
        QVector<LogDownloadRow> rows;
        QString source;
        bool createKml = false;

        bool isValid() const
        {
            return id != 0 && kind != PreparationKind::None
                && target.isValid();
        }
    };

    struct DownloadBatch
    {
        bool active = false;
        SwarmVehicleInstanceLease target;
        QVector<LogDownloadRow> rows;
        QStringList destinations;
        QString source;
        int nextIndex = 0;
        int saved = 0;
        quint64 completedBytes = 0;
        quint64 totalBytes = 0;
        bool createKml = false;
        QStringList kmlErrors;
        bool cancelRequested = false;
    };

    quint64 prepare(PreparationKind kind,
                    const QVector<LogDownloadRow> &rows);
    bool validatePreparation(quint64 id, PreparationKind kind);
    bool discardStaleListForTarget(
        const SwarmVehicleInstanceLease &target);
    bool revalidateTarget(const SwarmVehicleInstanceLease &target,
                          const QString &context);
    void clearPreparation();
    bool startList(const SwarmVehicleInstanceLease &target);
    bool startErase(const SwarmVehicleInstanceLease &target);
    bool startNextDownload();
    void startKmlExport(const QString &input, quint16 logId);
    void handleKmlFinished(
        quint64 generation, quint16 logId,
        const DataFlashKmlExporter::Result &result);
    void handleTransferFinished(const ExactLogTransferResult &result);
    void processTransferFinished(const ExactLogTransferResult &result);
    void handleProgress(ExactLogTransferToken token, quint64 completed,
                        quint64 total, bool totalKnown);
    bool startServiceOperation(
        ExactLogTransferService::Operation operation,
        const QString &failureContext,
        const std::function<ExactLogTransferService::StartResult(
            ExactLogTransferToken *, QString *)> &start);
    void finishBatch(const QString &status, bool completed);
    void setStatus(const QString &status);
    void setProgress(double progress);
    void notifyState();
    QString sourceSuffix() const;
    QString serviceStartError(
        ExactLogTransferService::StartResult result) const;
    QString transferError(const ExactLogTransferResult &result) const;
    LogDownloadRow *selectedRow();
    const LogDownloadRow *selectedRow() const;

    QPointer<ExactLogTransferService> m_service;
    TargetAcquirer m_acquireTarget;
    KmlExporter m_kmlExporter;
    QVector<LogDownloadRow> m_logs;
    int m_selectedLogId = -1;
    double m_progress = 0.0;
    QString m_status;
    QString m_liveTargetSource;
    QString m_pinnedTargetSource;
    SwarmVehicleInstanceLease m_listTarget;
    bool m_createKml = false;
    bool m_refreshForDownloadAll = false;
    bool m_startingService = false;
    bool m_shuttingDown = false;
    quint64 m_nextPreparationId = 0;
    quint64 m_kmlGeneration = 0;
    Preparation m_preparation;
    DownloadBatch m_batch;
    ExactLogTransferToken m_activeToken;
    ExactLogTransferService::Operation m_activeOperation =
        ExactLogTransferService::Operation::None;
    QVector<ExactLogTransferResult> m_deferredResults;
    QPointer<QThread> m_kmlThread;
    std::shared_ptr<std::atomic_bool> m_kmlCancel;
};

#endif // LOGDOWNLOADVIEWMODEL_H
