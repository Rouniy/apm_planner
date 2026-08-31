#include "ParameterStore.h"

#include <QReadLocker>
#include <QWriteLocker>

bool ParameterSnapshot::contains(quint8 componentId, const QString &name) const
{
    return m_records.contains(ParameterKey{componentId, name});
}

ParameterRecord ParameterSnapshot::value(quint8 componentId, const QString &name) const
{
    return m_records.value(ParameterKey{componentId, name});
}

ParameterStore::ParameterStore(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ParameterLoadState>();
}

VehicleTarget ParameterStore::target() const
{
    QReadLocker locker(&m_lock);
    return m_target;
}

ParameterLoadState ParameterStore::state() const
{
    QReadLocker locker(&m_lock);
    return m_state;
}

ParameterProgress ParameterStore::progress() const
{
    QReadLocker locker(&m_lock);
    return progressLocked();
}

ParameterSnapshot ParameterStore::snapshot() const
{
    QReadLocker locker(&m_lock);
    ParameterSnapshot result;
    result.m_target = m_target;
    result.m_state = m_state;
    result.m_progress = progressLocked();
    result.m_records = m_records;
    return result;
}

bool ParameterStore::specializedPagesReady() const
{
    return state() == ParameterLoadState::Complete;
}

void ParameterStore::selectTarget(int linkId, quint8 systemId, quint8 componentId)
{
    bool changed = false;
    {
        QWriteLocker locker(&m_lock);
        const VehicleTarget requested{linkId, systemId, componentId, 0};
        if (!m_target.sameEndpoint(requested)) {
            ++m_revisionCounter;
            m_target = requested;
            m_target.revision = m_revisionCounter;
            clearLocked();
            changed = true;
        }
    }
    if (changed) {
        emit targetChanged();
        emit stateChanged(ParameterLoadState::Idle);
        emit progressChanged(0, 0, 0);
    }
}

void ParameterStore::beginLoad()
{
    {
        QWriteLocker locker(&m_lock);
        ++m_revisionCounter;
        m_target.revision = m_revisionCounter;
        clearLocked();
        m_state = ParameterLoadState::Loading;
    }
    emit targetChanged();
    emit stateChanged(ParameterLoadState::Loading);
    emit progressChanged(0, 0, 0);
}

void ParameterStore::finishLoad()
{
    ParameterLoadState newState;
    ParameterProgress current;
    {
        QWriteLocker locker(&m_lock);
        current = progressLocked();
        newState = current.complete() ? ParameterLoadState::Complete : ParameterLoadState::Partial;
        m_state = newState;
    }
    emit stateChanged(newState);
    emit progressChanged(current.received, current.reported, current.percent());
}

void ParameterStore::cancelLoad()
{
    {
        QWriteLocker locker(&m_lock);
        m_state = ParameterLoadState::Cancelled;
    }
    emit stateChanged(ParameterLoadState::Cancelled);
}

void ParameterStore::failLoad()
{
    {
        QWriteLocker locker(&m_lock);
        m_state = ParameterLoadState::Failed;
    }
    emit stateChanged(ParameterLoadState::Failed);
}

bool ParameterStore::ingest(quint8 componentId,
                            int parameterCount,
                            int parameterIndex,
                            const QString &name,
                            const QVariant &value,
                            ParameterType type)
{
    if (name.isEmpty() || componentId == 0 || parameterIndex < 0 || parameterCount <= 0
        || parameterIndex >= parameterCount || type == ParameterType::Unknown || !value.isValid()) {
        return false;
    }

    ParameterProgress current;
    ParameterLoadState changedState = ParameterLoadState::Idle;
    bool stateWasChanged = false;
    {
        QWriteLocker locker(&m_lock);
        if (!m_target.isValid()) {
            return false;
        }

        m_reportedByComponent[componentId] = qMax(
            m_reportedByComponent.value(componentId), parameterCount);
        m_receivedIndices.insert(ParameterIndexKey{componentId, parameterIndex});

        const ParameterKey key{componentId, name};
        m_records.insert(key, ParameterRecord{
            key,
            value,
            type,
            parameterIndex,
            parameterCount,
            m_target.revision,
            QDateTime::currentDateTimeUtc()
        });

        current = progressLocked();
        if (m_state == ParameterLoadState::Loading || m_state == ParameterLoadState::Partial
            || m_state == ParameterLoadState::Complete) {
            const ParameterLoadState expectedState = current.complete()
                ? ParameterLoadState::Complete : ParameterLoadState::Loading;
            if (m_state != expectedState) {
                m_state = expectedState;
                changedState = m_state;
                stateWasChanged = true;
            }
        }
    }

    emit parameterChanged(componentId, name);
    emit progressChanged(current.received, current.reported, current.percent());
    if (stateWasChanged) {
        emit stateChanged(changedState);
    }
    return true;
}

ParameterProgress ParameterStore::progressLocked() const
{
    ParameterProgress result;
    result.received = m_receivedIndices.size();
    for (auto it = m_reportedByComponent.constBegin(); it != m_reportedByComponent.constEnd(); ++it) {
        result.reported += it.value();
    }
    return result;
}

void ParameterStore::clearLocked()
{
    m_state = ParameterLoadState::Idle;
    m_records.clear();
    m_reportedByComponent.clear();
    m_receivedIndices.clear();
}
