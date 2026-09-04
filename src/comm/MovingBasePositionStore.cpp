#include "MovingBasePositionStore.h"

#include "VehicleTargetManager.h"

#include <QtMath>

namespace
{

QString compactDecimal(double value)
{
    QString text = QString::number(value, 'f', 2);
    while (text.contains(QLatin1Char('.'))
           && text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    return text;
}

} // namespace

bool MovingBasePositionFix::isValid() const noexcept
{
    return qIsFinite(latitudeDegrees) && qIsFinite(longitudeDegrees)
        && qIsFinite(altitudeAmslMetres) && qIsFinite(hdop)
        && latitudeDegrees >= -90.0 && latitudeDegrees <= 90.0
        && longitudeDegrees >= -180.0 && longitudeDegrees <= 180.0
        && satellites >= 0 && hdop >= 0.0 && observedMonotonicMs >= 0;
}

QString MovingBasePositionFix::displayTag() const
{
    return QStringLiteral("Sats %1 hdop %2")
        .arg(satellites)
        .arg(compactDecimal(hdop));
}

MovingBasePositionStore::MovingBasePositionStore(
    VehicleTargetManager *targetManager, QObject *parent)
    : QObject(parent),
      m_targetManager(targetManager)
{
    Q_ASSERT(targetManager);
    qRegisterMetaType<MovingBasePositionFix>("MovingBasePositionFix");
    qRegisterMetaType<MovingBasePositionSnapshot>(
        "MovingBasePositionSnapshot");

    if (!targetManager) {
        return;
    }
    connect(targetManager, &VehicleTargetManager::endpointRemoved,
            this, &MovingBasePositionStore::clearEndpoint);
    connect(targetManager, &VehicleTargetManager::endpointsReset,
            this, &MovingBasePositionStore::clearAll);
    connect(targetManager, &VehicleTargetManager::targetGenerationSettled,
            this, [this](quint64) { publishSettledCurrentSnapshot(); });
    connect(targetManager, &VehicleTargetManager::currentTargetChanged,
            this, [this]() {
                if (m_targetManager
                    && m_targetManager->isTargetGenerationSettled()) {
                    publishSettledCurrentSnapshot();
                }
            });
}

MovingBasePositionSnapshot MovingBasePositionStore::snapshot(
    const VehicleTargetLease &target) const
{
    if (!target.isValid()) {
        return {};
    }
    const auto stored = m_snapshots.constFind(target.endpoint);
    if (stored == m_snapshots.constEnd()
        || !sameLease(stored->target, target)) {
        return {};
    }
    return *stored;
}

MovingBasePositionSnapshot MovingBasePositionStore::currentSnapshot() const
{
    return m_targetManager
        ? snapshot(m_targetManager->acquireTarget())
        : MovingBasePositionSnapshot{};
}

bool MovingBasePositionStore::update(
    const VehicleTargetLease &target, const MovingBasePositionFix &fix)
{
    if (!fix.isValid() || !isCurrentLease(target)) {
        return false;
    }

    const auto previous = m_snapshots.constFind(target.endpoint);
    if (previous != m_snapshots.constEnd()) {
        if (previous->target.generation > target.generation) {
            return false;
        }
        if (previous->target.generation == target.generation
            && previous->fix.observedMonotonicMs
                > fix.observedMonotonicMs) {
            return false;
        }
    }

    const MovingBasePositionSnapshot committed{target, fix};
    m_snapshots.insert(target.endpoint, committed);
    emit positionUpdated(committed);
    // A positionUpdated observer may synchronously change the target. Never
    // announce the old snapshot as current after such re-entrancy.
    if (isCurrentLease(target)) {
        emit currentSnapshotChanged(currentSnapshot());
    }
    return true;
}

bool MovingBasePositionStore::clear(const VehicleTargetLease &target)
{
    if (!isCurrentLease(target)) {
        return false;
    }
    const auto stored = m_snapshots.constFind(target.endpoint);
    if (stored == m_snapshots.constEnd()
        || !sameLease(stored->target, target)) {
        return false;
    }

    m_snapshots.erase(stored);
    emit positionCleared(target);
    if (isCurrentLease(target)) {
        emit currentSnapshotChanged(currentSnapshot());
    }
    return true;
}

bool MovingBasePositionStore::clearCurrent()
{
    return m_targetManager && clear(m_targetManager->acquireTarget());
}

bool MovingBasePositionStore::sameLease(
    const VehicleTargetLease &left,
    const VehicleTargetLease &right) noexcept
{
    return left.generation == right.generation
        && left.endpoint.sameIdentity(right.endpoint);
}

bool MovingBasePositionStore::isCurrentLease(
    const VehicleTargetLease &target) const
{
    return m_targetManager && target.isValid()
        && m_targetManager->isTargetGenerationSettled()
        && m_targetManager->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation);
}

void MovingBasePositionStore::clearEndpoint(
    int linkId, int systemId, int componentId)
{
    VehicleEndpoint endpoint;
    endpoint.linkId = linkId;
    endpoint.systemId = systemId;
    endpoint.componentId = componentId;
    const auto stored = m_snapshots.constFind(endpoint);
    if (stored == m_snapshots.constEnd()) {
        return;
    }
    const VehicleTargetLease cleared = stored->target;
    m_snapshots.erase(stored);
    emit positionCleared(cleared);
}

void MovingBasePositionStore::clearAll()
{
    const QList<MovingBasePositionSnapshot> removed = m_snapshots.values();
    m_snapshots.clear();
    for (const MovingBasePositionSnapshot &snapshot : removed) {
        emit positionCleared(snapshot.target);
    }
}

void MovingBasePositionStore::publishSettledCurrentSnapshot()
{
    emit currentSnapshotChanged(currentSnapshot());
}
