#ifndef MOVINGBASEPOSITIONSTORE_H
#define MOVINGBASEPOSITIONSTORE_H

#include "VehicleEndpoint.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class VehicleTargetManager;

/** A validated NMEA moving-base fix. Altitude is always metres AMSL. */
struct MovingBasePositionFix
{
    double latitudeDegrees = 0.0;
    double longitudeDegrees = 0.0;
    double altitudeAmslMetres = 0.0;
    int satellites = 0;
    double hdop = 0.0;
    /** Milliseconds from the process monotonic clock used by the producer. */
    qint64 observedMonotonicMs = -1;

    bool isValid() const noexcept;
    QString displayTag() const;
};

/** Position and immutable target epoch to which the fix belongs. */
struct MovingBasePositionSnapshot
{
    VehicleTargetLease target;
    MovingBasePositionFix fix;

    bool isValid() const noexcept
    {
        return target.isValid() && fix.isValid();
    }
};

Q_DECLARE_METATYPE(MovingBasePositionFix)
Q_DECLARE_METATYPE(MovingBasePositionSnapshot)

/**
 * Application-level moving-base position registry.
 *
 * Mutations are accepted only for the exact link/system/component/generation
 * currently selected by VehicleTargetManager. Entries remain isolated by
 * physical endpoint and carry their producing target generation, so a stale
 * input session cannot overwrite or clear a newer target epoch.
 */
class MovingBasePositionStore final : public QObject
{
    Q_OBJECT

public:
    explicit MovingBasePositionStore(VehicleTargetManager *targetManager,
                                     QObject *parent = nullptr);

    MovingBasePositionSnapshot snapshot(
        const VehicleTargetLease &target) const;
    MovingBasePositionSnapshot currentSnapshot() const;

    bool update(const VehicleTargetLease &target,
                const MovingBasePositionFix &fix);
    bool clear(const VehicleTargetLease &target);
    bool clearCurrent();

signals:
    void positionUpdated(const MovingBasePositionSnapshot &snapshot);
    void positionCleared(const VehicleTargetLease &target);
    void currentSnapshotChanged(
        const MovingBasePositionSnapshot &snapshot);

private:
    static bool sameLease(const VehicleTargetLease &left,
                          const VehicleTargetLease &right) noexcept;
    bool isCurrentLease(const VehicleTargetLease &target) const;
    void clearEndpoint(int linkId, int systemId, int componentId);
    void clearAll();
    void publishSettledCurrentSnapshot();

    QPointer<VehicleTargetManager> m_targetManager;
    QHash<VehicleEndpoint, MovingBasePositionSnapshot> m_snapshots;
};

#endif // MOVINGBASEPOSITIONSTORE_H
