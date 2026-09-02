#ifndef CONFIGGPSORDERVIEWMODEL_H
#define CONFIGGPSORDERVIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

struct GpsCanRow
{
    int Order = 0;
    QString Name;
    int NodeID = 0;
};

inline bool operator==(const GpsCanRow &left, const GpsCanRow &right)
{
    return left.Order == right.Order && left.Name == right.Name
        && left.NodeID == right.NodeID;
}

class ConfigGPSOrderViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)
    Q_PROPERTY(QString Intro READ Intro CONSTANT)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(bool Busy READ Busy NOTIFY stateChanged)
    Q_PROPERTY(bool CanEdit READ CanEdit NOTIFY stateChanged)

public:
    explicit ConfigGPSOrderViewModel(QObject *parent = nullptr);

    static QStringList FieldNames();
    static int WriteTimeoutMs();

    QString Title() const;
    QString Intro() const;
    QString Status() const { return m_status; }
    QList<GpsCanRow> Rows() const { return m_rows; }
    QList<ParamField> Fields() const { return m_fields; }
    bool Busy() const;
    bool CanEdit() const;
    bool Connected() const { return m_connected; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool HasPendingWrites() const { return m_pending.kind != NoWrite; }
    int ComponentId() const { return m_componentId; }

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    bool setFieldValue(const QString &name, const QVariant &value);

    // Mission Planner 10 action names are retained deliberately.
    bool Override1(const GpsCanRow &row);
    bool Override2(const GpsCanRow &row);
    bool Refresh();
    void refreshFailed(const QString &reason);
    void refreshCanceled();

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void rowsChanged();
    void structureChanged();
    void fieldChanged(const QString &name);
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
        OverrideWrite
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

    bool writeOverride(const QString &parameter, const GpsCanRow &row);
    void rebuildRows(bool updateAvailabilityStatus = true);
    void rebuildFields();
    ParamField makeField(const QString &name) const;
    ParamField *fieldForName(const QString &name);
    QString normalizedName(const QString &name) const;
    QVariant typedNodeValue(double value, const QVariant &reference) const;
    void queueWrite(PendingKind kind, const QString &name,
                    const QVariant &value, const QVariant &previousValue);
    void failPending(const QString &reason, bool authoritativeEcho = false);
    void cancelPendingForNewOwner();
    void rememberStaleEcho(const QString &name, const QVariant &value);
    bool consumeStaleEcho(const QString &name, const QVariant &value);
    void setStatus(const QString &status);

    ParameterMetaDataCatalog m_catalog;
    QList<GpsCanRow> m_rows;
    QList<ParamField> m_fields;
    QHash<QString, QVariant> m_values;
    QHash<QString, QList<QVariant>> m_staleEchoes;
    PendingWrite m_pending;
    QString m_status;
    int m_componentId = 1;
    quint64 m_writeGeneration = 0;
    quint64 m_snapshotGeneration = 0;
    bool m_connected = false;
    bool m_snapshotReady = false;
    bool m_refreshing = false;
};

#endif
