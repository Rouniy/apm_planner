#ifndef CONFIGFFTVIEWMODEL_H
#define CONFIGFFTVIEWMODEL_H

#include "ParamField.h"
#include "ui/Loghandling/DataFlashFftAnalyzer.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QVariant>

#include <atomic>
#include <functional>
#include <memory>

/** Transport-neutral state for MP10 FFT Setup and offline log analysis. */
class ConfigFFTViewModel final : public QObject
{
    Q_OBJECT

public:
    using Analyzer = std::function<DataFlashFftAnalyzer::Result(
        const QString &, const DataFlashFftAnalyzer::Options &)>;

    explicit ConfigFFTViewModel(
        Analyzer analyzer = {}, QObject *parent = nullptr);
    ~ConfigFFTViewModel() override;

    static QStringList ReferenceParameterNames();
    static int WriteTimeoutMs();

    QString title() const;
    QString info() const;
    QList<ParamField> fields() const { return m_fields; }
    QString parameterStatus() const { return m_parameterStatus; }
    QString analysisStatus() const { return m_analysisStatus; }
    int componentId() const { return m_componentId; }
    bool snapshotReady() const { return m_snapshotComplete; }
    bool reconciliationRequired() const { return m_reconciliationRequired; }
    bool parameterWriteBusy() const { return m_pending.active; }
    bool canEditParameters() const;
    bool canEditParameter(const QString &name) const;
    bool canRefreshParameters() const;

    int bins() const { return m_bins; }
    double startFrequencyHz() const { return m_startFrequencyHz; }
    bool magnitude() const { return m_magnitude; }
    bool analysisBusy() const { return bool(m_worker); }
    bool canAnalyze() const { return !m_shuttingDown && !analysisBusy(); }
    DataFlashFftAnalyzer::Result analysisResult() const
    {
        return m_analysisResult;
    }

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = true);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1, bool completeSnapshot = true);
    void setParameterContext(bool connected, bool heartbeatFresh,
                             bool armed);
    void setConnected(bool connected);
    void setHeartbeatFresh(bool fresh);
    void setArmed(bool armed);

    bool setBins(int bins);
    bool setStartFrequencyHz(double frequencyHz);
    bool setMagnitude(bool magnitude);
    bool stageParameterValue(const QString &name, const QVariant &value);
    bool refreshParameters();
    bool analyzeFile(const QString &path);
    void cancelAnalysis();
    void shutdown();

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteSubmitted(quint64 requestId, qulonglong batchId);
    void parameterWriteSubmissionFailed(quint64 requestId,
                                        const QString &reason);
    void parameterWriteFailed(qulonglong batchId, int componentId,
                              const QString &name, const QString &reason);
    void parameterWriteCancelled(qulonglong batchId, int componentId,
                                 const QString &name);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void refreshFailed(const QString &reason);
    void refreshCanceled();

signals:
    void fieldsChanged();
    void stateChanged();
    void analysisResultChanged();
    void writeRequested(quint64 requestId, int componentId,
                        QString name, QVariant value);
    void refreshRequested(int componentId);

private:
    struct PendingWrite
    {
        quint64 requestId = 0;
        qulonglong batchId = 0;
        quint64 snapshotRevision = 0;
        QString name;
        QVariant acceptedValue;
        QVariant stagedValue;
        QString failure;
        bool active = false;
    };

    void rebuildFields();
    void updateParameterStatus();
    void finishWrite(bool succeeded, const QString &reason = QString(),
                     bool uncertain = false);
    void handleAnalysisFinished(
        quint64 generation, QThread *worker,
        const DataFlashFftAnalyzer::Result &result);
    int fieldIndex(const QString &name) const;
    static QString normalizedName(const QString &name);
    static bool valuesEqual(const QVariant &left, const QVariant &right);
    static QVariant typedValue(const QVariant &candidate,
                               const QVariant &reference);

    Analyzer m_analyzer;
    ParameterMetaDataCatalog m_catalog;
    QList<ConfigFriendlyParameterValue> m_parameters;
    QList<ParamField> m_fields;
    QString m_parameterStatus;
    QString m_analysisStatus;
    DataFlashFftAnalyzer::Result m_analysisResult;
    PendingWrite m_pending;
    QPointer<QThread> m_worker;
    std::shared_ptr<std::atomic_bool> m_cancel;
    int m_componentId = 1;
    int m_bins = 10;
    double m_startFrequencyHz = 5.0;
    quint64 m_requestGeneration = 0;
    quint64 m_snapshotRevision = 0;
    quint64 m_analysisGeneration = 0;
    bool m_magnitude = false;
    bool m_connected = false;
    bool m_heartbeatFresh = false;
    bool m_armed = false;
    bool m_snapshotComplete = false;
    bool m_enforceMetadataRanges = true;
    bool m_reconciliationRequired = false;
    bool m_shuttingDown = false;
};

#endif // CONFIGFFTVIEWMODEL_H
