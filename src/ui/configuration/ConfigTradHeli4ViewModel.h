#ifndef CONFIGTRADHELI4VIEWMODEL_H
#define CONFIGTRADHELI4VIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>

struct Heli4ParameterSection
{
    QString title;
    QStringList parameters;
};

class ConfigTradHeli4ViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigTradHeli4ViewModel(QObject *parent = nullptr);

    static QList<Heli4ParameterSection> Sections();
    static QStringList ReferenceFieldNames();
    static int WriteTimeoutMs();

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = true);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setActive(bool active);

    QList<ParamField> Fields() const { return m_fields; }
    int ComponentId() const { return m_componentId; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool Connected() const { return m_connected; }
    bool Armed() const { return m_armed; }
    bool Active() const { return m_active; }
    bool HasPendingWrites() const { return !m_pendingWrites.isEmpty(); }
    bool ManualOverrideMayBeActive() const;
    QString Status() const { return m_status; }

    bool setFieldValue(const QString &name, const QVariant &value);
    bool Refresh();

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteSubmitted(quint64 requestId, qulonglong batchId);
    void parameterWriteSubmissionFailed(quint64 requestId,
                                        const QString &reason);
    void parameterWriteFailed(qulonglong batchId, int componentId,
                              const QString &name, const QString &reason);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void refreshFailed(const QString &reason);
    void refreshCanceled();

signals:
    void structureChanged();
    void fieldChanged(const QString &name);
    void stateChanged();
    void writeRequested(quint64 requestId, int componentId,
                        const QString &name, const QVariant &value);
    void refreshRequested(int componentId);
    void manualSafetyWarning(const QString &warning);

private:
    struct PendingWrite
    {
        QVariant expectedValue;
        quint64 requestId = 0;
        quint64 snapshotGeneration = 0;
        qulonglong batchId = 0;
        bool manualUncertainBefore = false;
    };

    void rebuildFields();
    ParamField makeField(const QString &name) const;
    ParamField *fieldForName(const QString &name);
    const ParamField *fieldForName(const QString &name) const;
    ParameterMetaData metadataFor(const QString &name) const;
    void finishPendingSuccess(const QString &name);
    void finishPending(const QString &name, const QString &error);
    QHash<QString, PendingWrite>::iterator pendingForRequest(
        quint64 requestId);
    QHash<QString, PendingWrite>::iterator pendingForBatch(
        qulonglong batchId);
    QVariant typedValue(const QVariant &value,
                        const QVariant &reference) const;
    static QString normalizedName(const QString &name);
    static bool valuesEqual(const QVariant &left, const QVariant &right);

    ParameterMetaDataCatalog m_catalog;
    QList<ParamField> m_fields;
    QHash<QString, QVariant> m_values;
    QHash<QString, PendingWrite> m_pendingWrites;
    QString m_status;
    int m_componentId = 1;
    quint64 m_requestGeneration = 0;
    quint64 m_snapshotGeneration = 0;
    bool m_enforceMetadataRanges = true;
    bool m_connected = false;
    bool m_armed = false;
    bool m_active = false;
    bool m_snapshotReady = false;
    bool m_manualOverrideUncertain = false;
};

#endif // CONFIGTRADHELI4VIEWMODEL_H
