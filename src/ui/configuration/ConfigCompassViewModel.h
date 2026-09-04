#ifndef CONFIGCOMPASSVIEWMODEL_H
#define CONFIGCOMPASSVIEWMODEL_H

#include "ParamField.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

struct CompassPriorityRow
{
    int priority = 0;
    quint32 rawDevId = 0;
    qint32 devId = 0;
    QString busType;
    int bus = 0;
    int address = 0;
    QString devType;
    bool missing = false;
    bool external = false;
    QString orientation;
    int physicalSlot = 0;

    bool operator==(const CompassPriorityRow &other) const
    {
        return priority == other.priority && rawDevId == other.rawDevId
            && devId == other.devId && busType == other.busType
            && bus == other.bus && address == other.address
            && devType == other.devType && missing == other.missing
            && external == other.external
            && orientation == other.orientation
            && physicalSlot == other.physicalSlot;
    }
    bool operator!=(const CompassPriorityRow &other) const
    {
        return !(*this == other);
    }
};

/*
 * Mission Planner 10 SETUP > Mandatory Hardware > Compass.
 *
 * This model deliberately owns no widgets or transport objects.  A complete
 * parameter snapshot is the only authority which can make it writable or
 * reconcile an uncertain batch.  Every mutation is emitted as one exact-
 * component batch, identified by requestId.
 */
class ConfigCompassViewModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        PriorityColumn,
        DevIDColumn,
        BusTypeColumn,
        BusColumn,
        AddressColumn,
        DevTypeColumn,
        MissingColumn,
        ExternalColumn,
        OrientationColumn,
        UpColumn,
        DownColumn,
        ColumnCount
    };
    Q_ENUM(Column)

    enum class WriteOperation
    {
        None,
        Priority,
        Field,
        Use,
        Learn,
        Declination,
        QuickPixhawk
    };
    Q_ENUM(WriteOperation)

    static constexpr int SortRole = Qt::UserRole;

    explicit ConfigCompassViewModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    static QStringList AdvancedFieldNames();
    static int WriteTimeoutMs();
    static int RebootAckTimeoutMs();

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceRanges = false);
    // completeSnapshot is intentionally mandatory.  A partial stream must
    // never silently unlock Compass writes.
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent, bool completeSnapshot);
    void setConnected(bool connected);
    void setArmed(bool armed);

    QVector<CompassPriorityRow> Rows() const { return m_rows; }
    QList<ParamField> Fields() const { return m_fields; }
    bool UseCompass(int slot) const;
    bool HasUseCompass(int slot) const;
    bool LearnOffsets() const;
    bool HasLearn() const;
    double DeclinationDegrees() const;
    bool HasDeclination() const;
    QString Status() const { return m_status; }
    QString CompassStatus() const { return m_compassStatus; }
    int ComponentId() const { return m_componentId; }
    bool Connected() const { return m_connected; }
    bool Armed() const { return m_armed; }
    bool SnapshotComplete() const { return m_snapshotComplete; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool Available() const { return m_snapshotReady; }
    bool IsAvailable() const { return m_snapshotReady; }
    bool Busy() const { return m_pending.active || m_rebootPending; }
    bool HasPendingWrites() const { return m_pending.active; }
    bool ReconciliationRequired() const
    {
        return m_reconciliationRequired;
    }
    bool RebootRequired() const { return m_rebootRequired; }
    bool RebootPending() const { return m_rebootPending; }
    bool RebootOutcomeUncertain() const { return m_rebootOutcomeUncertain; }

    bool moveUp(int row);
    bool moveDown(int row);
    bool removeMissing();
    bool setFieldValue(const QString &name, const QVariant &value);
    bool setUseCompass(int slot, bool use);
    bool setLearnOffsets(bool learn);
    bool writeDeclinationDegrees(double degrees);
    bool quickPixhawk();
    bool Refresh();
    bool Reboot();

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

    void rebootSubmitted(quint64 requestId);
    void rebootSubmissionFailed(quint64 requestId, const QString &reason);
    void rebootAcknowledged(quint64 requestId, bool accepted,
                            const QString &reason = QString());
    void rebootCancelled(quint64 requestId,
                         const QString &reason = QString());

signals:
    void rowsChanged(int count);
    void fieldsChanged();
    void stateChanged();
    void writeRequested(quint64 requestId, int componentId,
                        QVariantList changes);
    void refreshRequested(int componentId);
    void rebootRequested(quint64 requestId);

private:
    struct PendingWrite
    {
        QVariantList changes;
        QHash<QString, QVariant> previousValues;
        quint64 requestId = 0;
        quint64 snapshotGeneration = 0;
        qulonglong batchId = 0;
        QString failureReason;
        WriteOperation operation = WriteOperation::None;
        bool active = false;
    };

    void rebuildPresentation();
    void rebuildRows();
    void rebuildFields();
    void updateCompassStatus();
    bool beginWrite(const QVariantList &changes, WriteOperation operation);
    bool canWrite(const QString &action);
    void finishSuccess();
    void finishFailure(const QString &reason);
    void finishUncertain(const QString &reason, bool requestRefresh);
    void rollbackPending();
    void clearPresentation();
    bool pendingContains(const QString &name) const;
    QVariantList priorityChanges(
        const QVector<CompassPriorityRow> &candidate) const;
    QString resolvedUseName(int slot) const;
    QString resolvedDevIdName(int slot) const;
    QString resolvedExternalName(int slot) const;
    QString resolvedOrientationName(int slot) const;
    QString orientationText(int slot, const QVariant &value) const;
    ParamField fieldForName(const QString &name) const;
    void finishReboot(const QString &status, bool accepted);
    static QString normalizedName(const QString &name);
    static bool valuesEqual(const QVariant &left, const QVariant &right);

    ParameterMetaDataCatalog m_catalog;
    bool m_enforceRanges = false;
    QHash<QString, QVariant> m_values;
    QVector<CompassPriorityRow> m_rows;
    QList<ParamField> m_fields;
    QString m_status;
    QString m_compassStatus;
    PendingWrite m_pending;
    int m_componentId = 1;
    quint64 m_requestGeneration = 0;
    quint64 m_snapshotGeneration = 0;
    quint64 m_rebootRequestId = 0;
    quint64 m_rebootSnapshotGeneration = 0;
    bool m_rebootSubmitted = false;
    bool m_snapshotComplete = false;
    bool m_snapshotReady = false;
    bool m_connected = false;
    bool m_armed = false;
    bool m_reconciliationRequired = false;
    bool m_rebootRequired = false;
    bool m_rebootPending = false;
    bool m_rebootOutcomeUncertain = false;
};

#endif // CONFIGCOMPASSVIEWMODEL_H
