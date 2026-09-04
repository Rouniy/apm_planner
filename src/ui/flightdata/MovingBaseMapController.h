#ifndef MOVINGBASEMAPCONTROLLER_H
#define MOVINGBASEMAPCONTROLLER_H

#include <QObject>
#include <QPointer>

class AbstractMapWidget;
class MovingBasePositionStore;
class VehicleTargetManager;

/**
 * Binds the current exact-target moving-base fix to the Flight Data map.
 *
 * The controller deliberately depends only on the application-owned position
 * store, target registry and backend-neutral map contract.  A map may be
 * attached after its backend has been constructed or replaced at runtime.
 */
class MovingBaseMapController final : public QObject
{
public:
    explicit MovingBaseMapController(
        MovingBasePositionStore *positionStore,
        VehicleTargetManager *targetManager,
        QObject *parent = nullptr);
    ~MovingBaseMapController() override;

    void attachMap(AbstractMapWidget *map);
    void detachMap();
    AbstractMapWidget *attachedMap() const;

private:
    void synchronize();
    void applyCurrentSnapshotOnce();

    QPointer<MovingBasePositionStore> m_positionStore;
    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<AbstractMapWidget> m_map;
    quint64 m_attachmentRevision = 0;
    bool m_synchronizing = false;
    bool m_resynchronizeRequested = false;
};

#endif // MOVINGBASEMAPCONTROLLER_H
