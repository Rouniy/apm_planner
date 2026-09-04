#ifndef CONFIGFRAMECLASSTYPEVIEWMODEL_H
#define CONFIGFRAMECLASSTYPEVIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

struct FrameClassTypeEntry
{
    int frameClass = 0;
    QVariant frameType;
};

class ConfigFrameClassTypeViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigFrameClassTypeViewModel(QObject *parent = nullptr);

    static QList<FrameClassTypeEntry> ValidList();
    static int WriteTimeoutMs();

    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);

    QList<ParamOption> ClassOptions() const { return m_classOptions; }
    QList<ParamOption> TypeOptions() const { return m_typeOptions; }
    QVariant SelectedClass() const { return m_selectedClass; }
    QVariant SelectedType() const { return m_selectedType; }
    QString FrameImageKey() const { return m_frameImageKey; }
    QString FrameImageResource() const { return m_frameImageResource; }
    QString FrameImageCaption() const { return m_frameImageCaption; }
    QString Status() const { return m_status; }
    int ComponentId() const { return m_componentId; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool IsAvailable() const { return m_snapshotReady; }
    bool Connected() const { return m_connected; }
    bool Armed() const { return m_armed; }
    bool TypeEnabled() const { return !m_typeOptions.isEmpty(); }
    bool Busy() const { return m_pending.active; }
    bool HasPendingWrites() const { return m_pending.active; }
    bool ReconciliationRequired() const { return m_reconciliationRequired; }

    bool selectClass(const QVariant &frameClass);
    bool selectType(const QVariant &frameType);
    bool Refresh();

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
    void optionsChanged();
    void stateChanged();
    void writeRequested(quint64 requestId, int componentId,
                        QVariantList changes);
    void refreshRequested(int componentId);

private:
    struct PendingWrite
    {
        QVariantList changes;
        QVariant previousClass;
        QVariant previousType;
        quint64 requestId = 0;
        quint64 snapshotGeneration = 0;
        qulonglong batchId = 0;
        QString failureReason;
        bool active = false;
    };

    void buildClassOptions();
    void rebuildTypes(int frameClass, const QVariant &preferredType);
    void hydrateSelection();
    bool beginWrite(const QVariantList &changes,
                    const QVariant &newClass,
                    const QVariant &newType);
    void finishSuccess();
    void finishFailure(const QString &reason);
    void finishUncertain(const QString &reason, bool requestRefresh);
    void rollbackPending();
    void updateImage();
    bool pendingContains(const QString &name) const;
    static QList<ParamOption> typesForClass(int frameClass);
    static QString className(int frameClass);
    static QString typeName(int frameType);
    static QString typeImageKey(int frameType);
    static QString classImageKey(int frameClass);
    static QString resourceForImageKey(const QString &key);
    static QString normalizedName(const QString &name);
    static bool valuesEqual(const QVariant &left, const QVariant &right);

    QList<ParamOption> m_classOptions;
    QList<ParamOption> m_typeOptions;
    QHash<QString, QVariant> m_values;
    QVariant m_selectedClass;
    QVariant m_selectedType;
    QString m_frameImageKey;
    QString m_frameImageResource;
    QString m_frameImageCaption;
    QString m_status;
    PendingWrite m_pending;
    int m_componentId = 1;
    quint64 m_requestGeneration = 0;
    quint64 m_snapshotGeneration = 0;
    bool m_snapshotReady = false;
    bool m_connected = false;
    bool m_armed = false;
    bool m_reconciliationRequired = false;
};

#endif // CONFIGFRAMECLASSTYPEVIEWMODEL_H
