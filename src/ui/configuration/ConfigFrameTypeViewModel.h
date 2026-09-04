#ifndef CONFIGFRAMETYPEVIEWMODEL_H
#define CONFIGFRAMETYPEVIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

class ConfigFrameTypeViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigFrameTypeViewModel(QObject *parent = nullptr);

    static QList<ParamOption> FrameOptions();
    static int WriteTimeoutMs();

    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);

    QVariant SelectedFrame() const { return m_selectedFrame; }
    QString Status() const { return m_status; }
    int ComponentId() const { return m_componentId; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool IsAvailable() const { return m_snapshotReady; }
    bool Connected() const { return m_connected; }
    bool Armed() const { return m_armed; }
    bool Busy() const { return m_pending.active; }
    bool HasPendingWrites() const { return m_pending.active; }

    bool selectFrame(const QVariant &frame);
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
    void stateChanged();
    void writeRequested(quint64 requestId, int componentId,
                        QVariantList changes);
    void refreshRequested(int componentId);

private:
    struct PendingWrite
    {
        QVariant expectedValue;
        QVariant previousValue;
        quint64 requestId = 0;
        quint64 snapshotGeneration = 0;
        qulonglong batchId = 0;
        bool active = false;
    };

    void finishSuccess();
    void finishFailure(const QString &reason);
    void rollbackPending();
    static QString normalizedName(const QString &name);
    static bool valuesEqual(const QVariant &left, const QVariant &right);

    QHash<QString, QVariant> m_values;
    QVariant m_selectedFrame;
    QString m_status;
    PendingWrite m_pending;
    int m_componentId = 1;
    quint64 m_requestGeneration = 0;
    quint64 m_snapshotGeneration = 0;
    bool m_snapshotReady = false;
    bool m_connected = false;
    bool m_armed = false;
};

#endif // CONFIGFRAMETYPEVIEWMODEL_H
