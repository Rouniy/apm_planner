#ifndef PARAMETERSTORE_H
#define PARAMETERSTORE_H

#include "core/VehicleTarget.h"
#include "core/parameters/ParameterCodec.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QReadWriteLock>
#include <QSet>

struct ParameterKey
{
    quint8 componentId = 0;
    QString name;
};

inline bool operator==(const ParameterKey &left, const ParameterKey &right)
{
    return left.componentId == right.componentId && left.name == right.name;
}

inline uint qHash(const ParameterKey &key, uint seed = 0)
{
    return qHash(key.name, seed) ^ (static_cast<uint>(key.componentId) << 24U);
}

struct ParameterRecord
{
    ParameterKey key;
    QVariant value;
    ParameterType type = ParameterType::Unknown;
    int index = -1;
    int reportedCount = 0;
    quint64 targetRevision = 0;
    QDateTime receivedAtUtc;
};

enum class ParameterLoadState
{
    Idle,
    Loading,
    Partial,
    Complete,
    Cancelled,
    Failed
};

struct ParameterProgress
{
    int received = 0;
    int reported = 0;

    int percent() const
    {
        return reported > 0 ? qMin(100, received * 100 / reported) : 0;
    }

    bool complete() const
    {
        return reported > 0 && received >= reported;
    }
};

class ParameterSnapshot
{
public:
    VehicleTarget target() const { return m_target; }
    ParameterLoadState state() const { return m_state; }
    ParameterProgress progress() const { return m_progress; }
    QList<ParameterRecord> records() const { return m_records.values(); }
    bool contains(quint8 componentId, const QString &name) const;
    ParameterRecord value(quint8 componentId, const QString &name) const;

private:
    friend class ParameterStore;
    VehicleTarget m_target;
    ParameterLoadState m_state = ParameterLoadState::Idle;
    ParameterProgress m_progress;
    QHash<ParameterKey, ParameterRecord> m_records;
};

class ParameterStore final : public QObject
{
    Q_OBJECT
public:
    explicit ParameterStore(QObject *parent = nullptr);

    VehicleTarget target() const;
    ParameterLoadState state() const;
    ParameterProgress progress() const;
    ParameterSnapshot snapshot() const;
    bool specializedPagesReady() const;

    void selectTarget(int linkId, quint8 systemId, quint8 componentId);
    void beginLoad();
    void finishLoad();
    void cancelLoad();
    void failLoad();
    bool ingest(quint8 componentId,
                int parameterCount,
                int parameterIndex,
                const QString &name,
                const QVariant &value,
                ParameterType type);

signals:
    void targetChanged();
    void stateChanged(ParameterLoadState state);
    void progressChanged(int received, int reported, int percent);
    void parameterChanged(int componentId, const QString &name);

private:
    struct ParameterIndexKey
    {
        quint8 componentId = 0;
        int index = -1;
    };
    friend inline bool operator==(const ParameterIndexKey &left, const ParameterIndexKey &right)
    {
        return left.componentId == right.componentId && left.index == right.index;
    }
    friend inline uint qHash(const ParameterIndexKey &key, uint seed)
    {
        return ::qHash(key.index, seed) ^ (static_cast<uint>(key.componentId) << 24U);
    }

    ParameterProgress progressLocked() const;
    void clearLocked();

    mutable QReadWriteLock m_lock;
    VehicleTarget m_target;
    quint64 m_revisionCounter = 0;
    ParameterLoadState m_state = ParameterLoadState::Idle;
    QHash<ParameterKey, ParameterRecord> m_records;
    QHash<quint8, int> m_reportedByComponent;
    QSet<ParameterIndexKey> m_receivedIndices;
};

Q_DECLARE_METATYPE(ParameterLoadState)

#endif
