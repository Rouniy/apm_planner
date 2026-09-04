#ifndef CONFIGEXTENDEDTUNINGVIEWMODEL_H
#define CONFIGEXTENDEDTUNINGVIEWMODEL_H

#include "ParamField.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

struct ExtendedTuningGroupDescriptor
{
    QString title;
    int firstRow = 0;
    int rowCount = 0;
};

struct ExtendedTuningRow
{
    QString groupTitle;
    QString label;
    QStringList candidates;
    QString resolvedName;
    ParamField field;
    QVariant acceptedValue;
    int mirrorTargetRow = -1;
    bool exists = false;

    bool dirty() const;
};

/*
 * Transport-neutral state for Mission Planner 10's Plane QuadPlane
 * Extended Tuning page.
 *
 * All 17 groups and 68 logical rows remain stable even when a firmware alias
 * is absent. A complete exact-component snapshot resolves each row, with the
 * Plane Q_A/Q_P/Q_WP aliases deliberately preferred over legacy aliases.
 * Edits are staged locally and Save() emits one changed-only exact-component
 * batch; raw PARAM_VALUE observations never complete that batch.
 */
class ConfigExtendedTuningViewModel final : public QObject
{
    Q_OBJECT

public:
    enum class QuadPlaneState
    {
        Unknown,
        Disabled,
        Enabled,
        Unavailable
    };
    Q_ENUM(QuadPlaneState)

    explicit ConfigExtendedTuningViewModel(QObject *parent = nullptr);
    ~ConfigExtendedTuningViewModel() override = default;

    static int WriteTimeoutMs();
    static QList<ExtendedTuningGroupDescriptor> ReferenceGroups();
    static QList<ExtendedTuningRow> ReferenceRows();
    static QStringList ReferenceParameterNames();

    QString Title() const;
    QString Intro() const;
    QList<ExtendedTuningGroupDescriptor> Groups() const { return m_groups; }
    QList<ExtendedTuningRow> Rows() const { return m_rows; }
    QString Status() const { return m_status; }
    QStringList LargeIncreaseNames() const;
    QuadPlaneState QuadPlaneModeState() const { return m_quadPlaneState; }
    int ComponentId() const { return m_componentId; }
    bool Connected() const { return m_connected; }
    bool HeartbeatFresh() const { return m_heartbeatFresh; }
    bool Armed() const { return m_armed; }
    bool SnapshotComplete() const { return m_snapshotComplete; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool LockRollPitch() const { return m_lockRollPitch; }
    bool Dirty() const;
    bool Busy() const { return m_pending.active; }
    bool HasPendingWrites() const { return m_pending.active; }
    bool ReconciliationRequired() const { return m_reconciliationRequired; }
    bool CanEdit() const;
    bool CanEditRow(int zeroBasedRow) const;
    bool CanSave() const;
    bool CanWrite() const { return CanSave(); }
    bool RequiresLargeIncreaseConfirmation() const;

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = true);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent, bool completeSnapshot,
        bool preserveStagedEdits = false);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setHeartbeatFresh(bool fresh);
    void setHeartbeat(bool fresh, bool armed);
    void setLockRollPitch(bool enabled);

    bool stageValue(int zeroBasedRow, const QVariant &value);
    bool stageValue(const QString &resolvedName, const QVariant &value);
    QVariantList DirtyChanges() const;
    bool Save(bool confirmedLargeIncrease = false);
    bool Refresh();
    bool Discard();
    bool RefreshScreen() { return Discard(); }

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
    void structureChanged();
    void rowsChanged();
    void stateChanged();
    void largeIncreaseConfirmationRequested(QStringList parameterNames);
    void writeRequested(quint64 requestId, int componentId,
                        QVariantList changes);
    void refreshRequested(int componentId);

private:
    struct PendingWrite
    {
        QVariantList changes;
        quint64 requestId = 0;
        quint64 snapshotGeneration = 0;
        qulonglong batchId = 0;
        QString failureReason;
        bool active = false;
    };

    void rebuildRows();
    void updateStatus();
    void updateInitialRollPitchLock();
    bool stageRowValue(int zeroBasedRow, const QVariant &value,
                       bool permitMirror);
    bool pendingContains(const QString &name) const;
    void finishSuccess();
    void finishFailure(const QString &reason);
    void finishUncertain(const QString &reason, bool requestRefresh);
    static QString normalizedName(const QString &name);
    static bool isQuadPlaneParameter(const QString &name);
    static bool valuesEqual(const QVariant &left, const QVariant &right);
    static QVariant typedValue(const QVariant &candidate,
                               const QVariant &reference);

    ParameterMetaDataCatalog m_catalog;
    QList<ExtendedTuningGroupDescriptor> m_groups;
    QList<ExtendedTuningRow> m_rows;
    QList<ConfigFriendlyParameterValue> m_snapshotParameters;
    QString m_status;
    QString m_operationStatus;
    PendingWrite m_pending;
    QuadPlaneState m_quadPlaneState = QuadPlaneState::Unknown;
    int m_componentId = 1;
    quint64 m_requestGeneration = 0;
    quint64 m_snapshotGeneration = 0;
    bool m_enforceMetadataRanges = true;
    bool m_snapshotComplete = false;
    bool m_snapshotReady = false;
    bool m_connected = false;
    bool m_heartbeatFresh = false;
    bool m_armed = false;
    bool m_lockRollPitch = true;
    bool m_reconciliationRequired = false;
};

Q_DECLARE_METATYPE(ExtendedTuningGroupDescriptor)
Q_DECLARE_METATYPE(QList<ExtendedTuningGroupDescriptor>)
Q_DECLARE_METATYPE(ExtendedTuningRow)
Q_DECLARE_METATYPE(QList<ExtendedTuningRow>)

#endif // CONFIGEXTENDEDTUNINGVIEWMODEL_H
