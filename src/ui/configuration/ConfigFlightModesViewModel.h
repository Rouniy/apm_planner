#ifndef CONFIGFLIGHTMODESVIEWMODEL_H
#define CONFIGFLIGHTMODESVIEWMODEL_H

#include "ParamField.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>

struct FlightModeRow
{
    int position = 0;
    QString parameterName;
    int minimumPwm = 0;
    // -1 denotes an open upper bound.
    int maximumPwm = 0;
    QString pwmBand;
    QVariant acceptedMode;
    QVariant mode;
    bool acceptedSimple = false;
    bool simple = false;
    bool acceptedSuperSimple = false;
    bool superSimple = false;
    bool active = false;

    bool modeDirty() const;
    bool simpleDirty() const { return simple != acceptedSimple; }
    bool superSimpleDirty() const
    {
        return superSimple != acceptedSuperSimple;
    }
    bool dirty() const
    {
        return modeDirty() || simpleDirty() || superSimpleDirty();
    }
};

/*
 * Transport-neutral state for Mission Planner 10's Flight Modes page.
 *
 * The widget supplies the firmware family, metadata-derived mode options, one
 * complete committed parameter snapshot and exact-target telemetry. Edits are
 * staged locally; Save() emits one changed-only exact-component batch. Raw
 * PARAM_VALUE observations never complete that batch.
 */
class ConfigFlightModesViewModel final : public QObject
{
    Q_OBJECT

public:
    enum class Family
    {
        Copter,
        Plane,
        Rover,
        Px4,
        Unsupported
    };
    Q_ENUM(Family)

    explicit ConfigFlightModesViewModel(QObject *parent = nullptr);
    ~ConfigFlightModesViewModel() override = default;

    static int WriteTimeoutMs();
    static int RowForPwm(int pwm);
    static QString PwmBand(int zeroBasedRow);
    static QString ModePrefix(Family family);
    static QString SwitchParameter(Family family);

    void setFamily(Family family,
                   const QList<ParamOption> &modeOptions = {});
    void setModeOptions(const QList<ParamOption> &modeOptions);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent, bool completeSnapshot,
        bool preserveStagedEdits = false);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setHeartbeat(quint32 customMode, bool fresh, bool armed);
    void setHeartbeatFresh(bool fresh);

    Family VehicleFamily() const { return m_family; }
    QList<FlightModeRow> Rows() const { return m_rows; }
    QList<ParamOption> ModeOptions() const { return m_modeOptions; }
    QString CurrentModeText() const;
    QString CurrentPwmText() const;
    QString Status() const { return m_status; }
    int ActiveRow() const { return m_activeRow; }
    int SwitchChannel() const { return m_switchChannel; }
    int ComponentId() const { return m_componentId; }
    int CurrentPwm() const { return m_currentPwm; }
    quint32 CurrentMode() const { return m_currentMode; }
    bool HasCurrentPwm() const { return m_hasCurrentPwm; }
    bool CanEdit() const;
    bool CanSave() const { return CanEdit() && Dirty(); }
    bool Dirty() const;
    bool IsAvailable() const { return m_snapshotReady; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool SnapshotComplete() const { return m_snapshotComplete; }
    bool HeartbeatFresh() const { return m_heartbeatFresh; }
    bool Armed() const { return m_armed; }
    bool Connected() const { return m_connected; }
    bool HasPendingWrites() const { return m_pending.active; }
    bool Busy() const { return m_pending.active; }
    bool ReconciliationRequired() const
    {
        return m_reconciliationRequired;
    }
    bool ShowSimple() const { return m_family == Family::Copter; }
    bool ShowSuperSimple() const { return m_family == Family::Copter; }
    bool HasSimpleParameter() const { return m_showSimple; }
    bool HasSuperSimpleParameter() const { return m_showSuperSimple; }

    // Row arguments are zero-based, matching QList/QWidget row indexing.
    bool stageMode(int row, const QVariant &mode);
    bool stageSimple(int row, bool enabled);
    bool stageSuperSimple(int row, bool enabled);
    bool Discard();
    QVariantList DirtyChanges() const;
    bool Save();
    bool Refresh();

    // RC channels are one-based. Samples for any other channel are ignored.
    bool setRcInput(int oneBasedChannel, int pwm);
    void clearRcInput();

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
    void rowsChanged();
    void optionsChanged();
    void stateChanged();
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

    void resetRows();
    void clearSnapshotPresentation();
    void rebuildOptions();
    void addUnknownOption(const QVariant &value);
    void updateStatus();
    void finishSuccess();
    void finishFailure(const QString &reason);
    void finishUncertain(const QString &reason, bool requestRefresh);
    bool pendingContains(const QString &name) const;
    int modeRow(const QString &normalizedName) const;
    quint32 stagedSimpleValue(bool superSimple) const;
    void applySimpleValue(quint32 value, bool superSimple,
                          bool preserveDirtyBits);
    static QString normalizedName(const QString &name);
    static bool valuesEqual(const QVariant &left, const QVariant &right);
    static bool optionListsEqual(const QList<ParamOption> &left,
                                 const QList<ParamOption> &right);

    Family m_family = Family::Unsupported;
    QList<FlightModeRow> m_rows;
    QList<ParamOption> m_suppliedOptions;
    QList<ParamOption> m_modeOptions;
    QString m_status;
    QString m_operationStatus;
    PendingWrite m_pending;
    int m_componentId = 1;
    int m_switchChannel = 0;
    int m_currentPwm = 0;
    int m_activeRow = -1;
    quint32 m_currentMode = 0;
    quint32 m_simpleValue = 0;
    quint32 m_superSimpleValue = 0;
    quint64 m_requestGeneration = 0;
    quint64 m_snapshotGeneration = 0;
    bool m_snapshotComplete = false;
    bool m_snapshotReady = false;
    bool m_connected = false;
    bool m_armed = false;
    bool m_heartbeatFresh = false;
    bool m_hasCurrentMode = false;
    bool m_hasCurrentPwm = false;
    bool m_showSimple = false;
    bool m_showSuperSimple = false;
    bool m_reconciliationRequired = false;
};

Q_DECLARE_METATYPE(FlightModeRow)
Q_DECLARE_METATYPE(QList<FlightModeRow>)

#endif // CONFIGFLIGHTMODESVIEWMODEL_H
