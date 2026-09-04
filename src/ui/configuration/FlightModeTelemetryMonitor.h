#ifndef FLIGHTMODETELEMETRYMONITOR_H
#define FLIGHTMODETELEMETRYMONITOR_H

#include "comm/VehicleEndpoint.h"

#include <QObject>
#include <QPointer>

#include <mavlink.h>

class VehicleTargetManager;

/**
 * Read-only, exact-target telemetry adapter for the Flight Modes pages.
 *
 * The legacy page consumes active-UAS telemetry, which cannot distinguish two
 * vehicles with the same MAVLink ids on different links.  This adapter owns an
 * immutable target lease and accepts data only from that physical endpoint.
 * RC samples are withheld until an exact heartbeat establishes the current
 * link/target epoch.
 */
class FlightModeTelemetryMonitor final : public QObject
{
    Q_OBJECT

public:
    explicit FlightModeTelemetryMonitor(
        VehicleTargetManager *targets,
        const VehicleTargetLease &lease,
        QObject *parent = nullptr);

    VehicleTargetLease lease() const { return m_lease; }
    bool heartbeatSeen() const { return m_heartbeatSeen; }
    bool invalidated() const { return m_invalidated; }

public slots:
    void observeMessage(int linkId, const mavlink_message_t &message);

signals:
    void heartbeatObserved(quint32 customMode, bool armed,
                           int autopilot, int vehicleType);
    void rcInputObserved(int channelOneBased, int pwm);
    void targetInvalidated();

private:
    bool accepts(int linkId, const mavlink_message_t &message);
    void invalidate();
    void emitChannels(const quint16 *channels, int count,
                      int firstChannelOneBased);

    QPointer<VehicleTargetManager> m_targets;
    const VehicleTargetLease m_lease;
    bool m_heartbeatSeen = false;
    bool m_invalidated = false;
};

#endif // FLIGHTMODETELEMETRYMONITOR_H
