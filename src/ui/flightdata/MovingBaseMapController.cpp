#include "MovingBaseMapController.h"

#include "comm/MovingBasePositionStore.h"
#include "comm/VehicleTargetManager.h"
#include "ui/map/AbstractMapWidget.h"

namespace
{

bool sameLease(const VehicleTargetLease &left,
               const VehicleTargetLease &right) noexcept
{
    return left.generation == right.generation
        && left.endpoint.sameIdentity(right.endpoint);
}

} // namespace

MovingBaseMapController::MovingBaseMapController(
    MovingBasePositionStore *positionStore,
    VehicleTargetManager *targetManager,
    QObject *parent)
    : QObject(parent),
      m_positionStore(positionStore),
      m_targetManager(targetManager)
{
    Q_ASSERT(positionStore);
    Q_ASSERT(targetManager);

    if (positionStore) {
        connect(positionStore,
                &MovingBasePositionStore::currentSnapshotChanged,
                this,
                [this](const MovingBasePositionSnapshot &) {
                    synchronize();
                });
        connect(positionStore, &QObject::destroyed, this, [this]() {
            m_positionStore = nullptr;
            synchronize();
        });
    }
    if (targetManager) {
        // Clear synchronously during the invalidation phase.  The store emits
        // the replay (or an empty snapshot) after the final generation settles.
        connect(targetManager,
                &VehicleTargetManager::targetGenerationChanged,
                this,
                [this](qulonglong) { synchronize(); });
        connect(targetManager, &QObject::destroyed, this, [this]() {
            m_targetManager = nullptr;
            synchronize();
        });
    }
}

MovingBaseMapController::~MovingBaseMapController()
{
    if (m_positionStore) {
        disconnect(m_positionStore.data(), nullptr, this, nullptr);
    }
    if (m_targetManager) {
        disconnect(m_targetManager.data(), nullptr, this, nullptr);
    }

    QPointer<AbstractMapWidget> map = m_map;
    m_map = nullptr;
    if (map) {
        map->ClearMovingBase();
    }
}

void MovingBaseMapController::attachMap(AbstractMapWidget *map)
{
    const quint64 attachmentRevision = ++m_attachmentRevision;
    if (m_map == map) {
        synchronize();
        return;
    }

    QPointer<MovingBaseMapController> guard(this);
    QPointer<AbstractMapWidget> requestedMap(map);
    QPointer<AbstractMapWidget> previousMap = m_map;
    m_map = nullptr;
    if (previousMap) {
        previousMap->ClearMovingBase();
    }
    // A backend callback may replace the attachment or destroy this binding.
    // In that case the nested operation owns the final state.
    if (!guard || attachmentRevision != m_attachmentRevision) {
        return;
    }

    m_map = requestedMap;
    synchronize();
}

void MovingBaseMapController::detachMap()
{
    attachMap(nullptr);
}

AbstractMapWidget *MovingBaseMapController::attachedMap() const
{
    return m_map.data();
}

void MovingBaseMapController::synchronize()
{
    if (m_synchronizing) {
        m_resynchronizeRequested = true;
        return;
    }

    QPointer<MovingBaseMapController> guard(this);
    m_synchronizing = true;
    do {
        m_resynchronizeRequested = false;
        applyCurrentSnapshotOnce();
        if (!guard) {
            return;
        }
    } while (m_resynchronizeRequested);
    m_synchronizing = false;
}

void MovingBaseMapController::applyCurrentSnapshotOnce()
{
    QPointer<AbstractMapWidget> map = m_map;
    if (!map) {
        return;
    }

    MovingBasePositionSnapshot snapshot;
    if (m_positionStore && m_targetManager
        && m_targetManager->isTargetGenerationSettled()) {
        snapshot = m_positionStore->currentSnapshot();
        const VehicleTargetLease currentTarget =
            m_targetManager->acquireTarget();
        if (!snapshot.isValid()
            || !sameLease(snapshot.target, currentTarget)) {
            snapshot = {};
        }
    }

    if (!snapshot.isValid()) {
        map->ClearMovingBase();
        return;
    }

    const MapCoordinate coordinate{
        snapshot.fix.latitudeDegrees,
        snapshot.fix.longitudeDegrees,
        snapshot.fix.altitudeAmslMetres};
    if (!coordinate.IsValid()) {
        map->ClearMovingBase();
        return;
    }
    map->SetMovingBase(coordinate, snapshot.fix.displayTag());
}
