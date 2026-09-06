#ifndef GUIDEDALTITUDESTORE_H
#define GUIDEDALTITUDESTORE_H

#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleTargetManager.h"

#include <QHash>
#include <QObject>
#include <QPointer>

/** Application-owned, thread-confined, in-memory guided intent; never sends. */
class GuidedAltitudeStore final : public QObject
{
    Q_OBJECT
public:
    struct Context {
        VehicleTargetLease target;
        SwarmVehicleInstanceLease vehicle;
        quint64 revision = 0;
        bool altitudeSet = false;
        float altitudeM = 0;
        MAV_FRAME frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        bool pointSet = false;
        double latitude = 0;
        double longitude = 0;
    };

    explicit GuidedAltitudeStore(VehicleTargetManager *targets,
        SwarmTelemetryRegistry *registry, QObject *parent = nullptr);

    // Requires settled selection and a fresh exact autopilot instance. Empty
    // intent is valid (revision 0); telemetry and saved UI defaults never fill it.
    bool prepareCurrent(Context *context, QString *error = nullptr) const;
    bool validate(const Context &context, QString *error = nullptr) const;
    Context current() const;

    // Metres only, independent of display units. Supports GLOBAL, RELATIVE_ALT,
    // TERRAIN_ALT, including signed/zero altitude. Leaves any explicit point.
    bool commitAltitude(const Context &context, double metres, MAV_FRAME frame,
        Context *updated = nullptr, QString *error = nullptr);
    // Caller decides whether the target was explicitly requested/accepted.
    // This does not record vehicle position or imply a transmitted/ACKed target.
    bool recordTarget(const Context &context, double latitude, double longitude,
        double metres, MAV_FRAME frame, Context *updated = nullptr,
        QString *error = nullptr);

signals:
    void changed();
    void contextInvalidated();

private:
    bool commit(const Context &context, double metres, MAV_FRAME frame,
        bool setPoint, double latitude, double longitude,
        Context *updated, QString *error);
    void notifyInvalidated();
    void retire(const SwarmVehicleInstanceLease &vehicle);

    QPointer<VehicleTargetManager> m_targets;
    QPointer<SwarmTelemetryRegistry> m_registry;
    QHash<VehicleEndpoint, Context> m_entries;
    quint64 m_nextRevision = 0;
    quint64 m_mutation = 0;
};

#endif // GUIDEDALTITUDESTORE_H
