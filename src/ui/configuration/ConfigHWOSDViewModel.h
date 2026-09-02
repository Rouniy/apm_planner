#ifndef CONFIGHWOSDVIEWMODEL_H
#define CONFIGHWOSDVIEWMODEL_H

#include "ParamField.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

/*
 * Mission Planner 10 SETUP > OSD (ConfigHWOSDViewModel): the legacy MinimOSD
 * telemetry helper. "Enable Telemetry" writes 2 Hz to the 24 SR0/SR1/SR3
 * stream-rate parameters (never SRx_PARAMS) of the committed component.
 *
 * The page never talks to a transport. The owner hydrates it with the
 * committed parameter snapshot of the exact target, receives one batch
 * request (writeParamsRequested) and reports the batch lifecycle back.
 * Results are accepted only for the batch id it reported and only while the
 * snapshot revision that produced the batch is still current.
 */
class ConfigHWOSDViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)
    Q_PROPERTY(QString Intro READ Intro CONSTANT)
    Q_PROPERTY(QString Note READ Note CONSTANT)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(bool Busy READ Busy NOTIFY stateChanged)
    Q_PROPERTY(bool CanEnableTelemetry READ CanEnableTelemetry NOTIFY stateChanged)

public:
    explicit ConfigHWOSDViewModel(QObject *parent = nullptr);
    ~ConfigHWOSDViewModel() override;

    // --- MP10 surface ---------------------------------------------------------------
    static QString Title();                // "OSD"
    static QString Intro();                // "MinimOSD telemetry helper."
    static QString Note();                 // "You only need to use this if you are ..."
    static QString EnableTelemetryText();  // "Enable Telemetry"
    static QStringList Streams();          // SR0, SR1, SR3
    static QStringList Suffixes();         // EXT_STAT, EXTRA1, ..., RC_CHAN
    static QStringList ParameterNames();   // the exact ordered 24 names
    static int TelemetryRateHz();          // 2
    static int ParameterCount() { return 24; }

    // MP10 statuses.
    static QString OfflineStatus();        // "offline — connect first."
    static QString SettingStatus();        // "Setting stream rates…"
    static QString SuccessStatus();        // "✓ Telemetry streams enabled (2 Hz)."
    // Qt-only statuses: the transport reports results here instead of throwing.
    static QString UnreadyStatus();        // "parameters unavailable — refresh the parameter list first."
    static QString MissingStatus(int componentId);
    static QString PartialSuccessStatus(int written, const QStringList &missing);
    static QString FailureStatus(const QString &detail); // WinForms "Failed to set OSD rates."
    static QString CancelledStatus();
    static QString TargetChangedStatus();
    static QString TimeoutStatus();

    QString Status() const { return m_status; }
    bool Busy() const { return m_batch.active; }
    bool Connected() const { return m_connected; }
    bool SnapshotReady() const { return m_snapshotReady; }
    // Connected, hydrated, idle and at least one stream-rate parameter present:
    // the button is never enabled when a click could not do anything.
    bool CanEnableTelemetry() const;
    int ComponentId() const { return m_componentId; }
    // The stream-rate parameters present in the committed snapshot, in
    // ParameterNames() order.
    QStringList PresentParameterNames() const { return m_present; }
    QStringList MissingParameterNames() const;
    qulonglong PendingBatchId() const { return m_batch.batchId; }
    quint64 SnapshotRevision() const { return m_snapshotRevision; }

    // Hydration from the owner's committed exact-component snapshot.
    void setParameterSnapshot(const QList<ConfigFriendlyParameterValue> &parameters,
                              int preferredComponent = 1);
    void setConnected(bool connected);
    // The selected (link, system, component) changed: the snapshot is stale
    // and any pending batch belongs to a target that no longer exists.
    void parameterTargetChanged();

    // MP10 EnableTelemetryCommand. Returns true when a batch was requested.
    bool EnableTelemetry();

    // The batch is expected to settle within this window (24 writes with
    // retries); afterwards the page reports a timeout instead of staying busy.
    void setBatchTimeoutForTesting(int milliseconds);
    static int DefaultBatchTimeoutMs() { return 60000; }

    // The exact batch payload for a snapshot: {name, value} maps in order.
    static QVariantList BuildChanges(const QStringList &presentNames);

public slots:
    // Batch lifecycle reported by the owner of the parameter transaction.
    void parameterBatchSubmitted(int componentId, qulonglong batchId);
    void parameterBatchProgress(qulonglong batchId, int completed, int total, int succeeded,
                                int failed);
    void parameterWriteFailed(qulonglong batchId, int componentId, const QString &name,
                              const QString &reason);
    void parameterBatchCompleted(qulonglong batchId, int succeeded, int failed);
    void parameterBatchCancelled(qulonglong batchId);
    void parameterWriteSubmissionFailed(const QString &reason);

signals:
    void statusChanged(const QString &status);
    void stateChanged();
    // One deterministic batch: componentId and ordered {name, value} maps.
    void writeParamsRequested(int componentId, QVariantList changes);

private:
    struct Batch
    {
        bool active = false;
        qulonglong batchId = 0; // 0 until the owner reports the submission
        quint64 revision = 0;
        quint64 serial = 0;
        int total = 0;
        QStringList failedNames;
        // A completion that arrived before the submission id (synchronous owners).
        bool earlyCompletion = false;
        qulonglong earlyBatchId = 0;
        int earlySucceeded = 0;
        int earlyFailed = 0;
    };

    bool ownsBatch(qulonglong batchId) const;
    void finishBatch(int succeeded, int failed);
    void abandonBatch(const QString &status);
    void setStatus(const QString &status);
    static QString normalizedName(const QString &name);

    QString m_status;
    QStringList m_present;
    Batch m_batch;
    int m_componentId = 1;
    quint64 m_snapshotRevision = 0;
    quint64 m_batchSerial = 0;
    int m_batchTimeoutMs = DefaultBatchTimeoutMs();
    bool m_connected = false;
    bool m_snapshotReady = false;
};

#endif // CONFIGHWOSDVIEWMODEL_H
