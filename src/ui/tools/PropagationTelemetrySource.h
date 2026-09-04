#ifndef PROPAGATIONTELEMETRYSOURCE_H
#define PROPAGATIONTELEMETRYSOURCE_H

#include "PropagationDistanceEstimator.h"
#include "comm/VehicleEndpoint.h"

#include <QElapsedTimer>
#include <QObject>

#include <functional>

#include <mavlink.h>

class VehicleTargetManager;

/** Exact-target telemetry consumed by both DATA and PLAN propagation maps. */
struct PropagationTelemetrySnapshot
{
    VehicleTargetLease lease;
    /** Changes on target replacement and explicit transport disconnect. */
    quint64 telemetryEpoch = 0;
    /** Display/test compatibility only; lease is the authoritative identity. */
    int vehicleIdentity = -1;

    bool heartbeatValid = false;
    bool armed = false;
    qint64 heartbeatObservedMs = -1;

    bool homeValid = false;
    double homeLatitude = 0.0;
    double homeLongitude = 0.0;
    double homeAltitudeAmslM = 0.0;
    qint64 homeObservedMs = -1;

    bool positionValid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeAmsl = 0.0;
    qint64 positionObservedMs = -1;

    int gpsFixType = 0;
    qint64 gpsObservedMs = -1;

    bool batteryRemainingValid = false;
    double batteryRemainingPercent = 0.0;
    bool usedMahValid = false;
    double usedMah = 0.0;
    bool batteryKilometresLeftValid = false;
    double batteryKilometresLeft = qQNaN();
    qint64 batteryObservedMs = -1;

    bool isValid() const noexcept { return lease.isValid(); }
};

/**
 * Read-only MAVLink observer bound to one physical VehicleTargetLease.
 *
 * Messages from a duplicate sysid on another link, another component, or an
 * obsolete target generation are rejected. No network request or vehicle
 * command is issued by this class.
 */
class PropagationTelemetrySource final : public QObject
{
    Q_OBJECT

public:
    using Clock = std::function<qint64()>;

    explicit PropagationTelemetrySource(
        VehicleTargetManager *targets,
        Clock clock = Clock(),
        QObject *parent = nullptr);

    PropagationTelemetrySnapshot snapshot() const { return m_snapshot; }
    qint64 monotonicTimeMs() const;
    bool isCurrentLease(const VehicleTargetLease &lease) const;

public slots:
    void observeMessage(int linkId, const mavlink_message_t &message);
    /** Clears state only when linkId owns the current exact target. */
    void observeLinkDisconnected(int linkId);

signals:
    void snapshotChanged();
    void telemetryEpochChanged(qulonglong telemetryEpoch);

private slots:
    void invalidateTargetEpoch();
    void resetTargetEpoch();

private:
    bool accepts(int linkId, const mavlink_message_t &message) const;
    bool capturedEpochIsCurrent(const VehicleTargetLease &lease,
                                quint64 telemetryEpoch) const;
    static bool validCoordinate(double latitude, double longitude,
                                double altitude);
    void copyBatteryEstimate(PropagationTelemetrySnapshot *snapshot) const;
    void publishFreshEpoch(const VehicleTargetLease &lease);

    VehicleTargetManager *const m_targets;
    Clock m_clock;
    QElapsedTimer m_elapsedClock;
    PropagationTelemetrySnapshot m_snapshot;
    PropagationDistanceEstimator m_distanceEstimator;
    quint64 m_epochCounter = 0;
};

#endif // PROPAGATIONTELEMETRYSOURCE_H
