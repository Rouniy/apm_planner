#ifndef CONFIGPARACHUTEVIEWMODEL_H
#define CONFIGPARACHUTEVIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

class ConfigParachuteViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)
    Q_PROPERTY(QString Intro READ Intro CONSTANT)
    Q_PROPERTY(QStringList ServoOptions READ ServoOptions CONSTANT)
    Q_PROPERTY(QString SelectedServo READ SelectedServo
               NOTIFY selectedServoChanged)
    Q_PROPERTY(QString ServoStatus READ ServoStatus
               NOTIFY servoStatusChanged)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(bool Busy READ Busy NOTIFY stateChanged)
    Q_PROPERTY(bool CanEdit READ CanEdit NOTIFY stateChanged)

public:
    explicit ConfigParachuteViewModel(QObject *parent = nullptr);

    static QStringList FieldNames();
    static int WriteTimeoutMs();

    QString Title() const;
    QString Intro() const;
    QStringList ServoOptions() const;
    QString SelectedServo() const { return m_selectedServo; }
    QString ServoStatus() const { return m_servoStatus; }
    QString Status() const { return m_status; }
    bool Busy() const;
    bool CanEdit() const;
    bool Connected() const { return m_connected; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool HasPendingWrites() const { return m_pending.kind != NoWrite; }
    int ComponentId() const { return m_componentId; }
    QList<ParamField> Fields() const { return m_fields; }

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    bool setFieldValue(const QString &name, const QVariant &value);

    // Mission Planner 10 action names are retained deliberately.
    bool Refresh();
    QString DetectServo();
    bool AssignServo(const QString &servo);
    QStringList EnsureDisabled(const QString &exclude) const;
    void refreshFailed(const QString &reason);
    void refreshCanceled();

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void structureChanged();
    void fieldChanged(const QString &name);
    void selectedServoChanged();
    void servoStatusChanged();
    void statusChanged();
    void stateChanged();
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);
    void refreshRequested(int componentId);

private:
    enum PendingKind
    {
        NoWrite,
        FieldWrite,
        DisableServoWrite,
        AssignServoWrite
    };

    struct PendingWrite
    {
        PendingKind kind = NoWrite;
        QString name;
        QVariant expectedValue;
        QVariant previousValue;
        quint64 writeGeneration = 0;
        quint64 snapshotGeneration = 0;
    };

    void rebuildFields();
    ParamField makeField(const QString &name, int index) const;
    ParamField *fieldForName(const QString &name);
    QString normalizedName(const QString &name) const;
    QString normalizedServo(const QString &servo) const;
    QString resolvedFunctionParameter(int channel) const;
    QVariant typedValue(const QVariant &value,
                        const QVariant &reference) const;
    void queueWrite(PendingKind kind, const QString &name,
                    const QVariant &value,
                    const QVariant &previousValue = QVariant());
    void startNextServoWrite();
    void finishServoAssignment();
    void failPending(const QString &reason, bool revertField);
    void cancelPendingForNewOwner(const QString &servoStatus);
    void rememberStaleEcho(const QString &name, const QVariant &value);
    bool isStaleEcho(const QString &name, const QVariant &value) const;
    bool consumeStaleEcho(const QString &name, const QVariant &value);
    void rememberOperationAsStale();
    void setSelectedServo(const QString &servo);
    void setServoStatus(const QString &status);
    void setStatus(const QString &status);

    ParameterMetaDataCatalog m_catalog;
    QList<ParamField> m_fields;
    QHash<QString, QVariant> m_values;
    QHash<QString, QList<QVariant>> m_operationWrites;
    QHash<QString, QList<QVariant>> m_staleEchoes;
    PendingWrite m_pending;
    QStringList m_disableQueue;
    QString m_assignmentTargetServo;
    QString m_assignmentTargetParameter;
    QString m_selectedServo;
    QString m_servoStatus;
    QString m_status;
    int m_componentId = 1;
    quint64 m_writeGeneration = 0;
    quint64 m_snapshotGeneration = 0;
    bool m_assignmentHadDisable = false;
    bool m_connected = false;
    bool m_snapshotReady = false;
    bool m_refreshing = false;
};

#endif
