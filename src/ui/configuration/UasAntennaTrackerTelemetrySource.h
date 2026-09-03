#ifndef UASANTENNATRACKERTELEMETRYSOURCE_H
#define UASANTENNATRACKERTELEMETRYSOURCE_H

#include "AntennaTrackerTelemetrySource.h"

#include <QString>

/*
 * Application implementation over UASManager / LinkManager. The vehicle fix
 * comes from the active UAS, the tracker location from an explicit tracker
 * home (MP10 cs.TrackerLocation set by PLAN "Tracker Home") or, as MP10 does
 * when that is unset, from the vehicle home. localSnrDb() is MP10's
 * cs.localsnrdb read from LinkManager's RadioStatusMonitor for the exact
 * physical link of the current VehicleTargetManager lease, so duplicate sysids
 * on other links never leak in; without LinkManager, monitor or a valid target
 * it fails closed to 0, which keeps MP10's "No valid SiK radio detected." path.
 */
class UasAntennaTrackerTelemetrySource final : public AntennaTrackerTelemetrySource
{
    Q_OBJECT

public:
    explicit UasAntennaTrackerTelemetrySource(QObject *parent = nullptr);

    AntennaTrackerVehicleFix vehicleFix() const override;
    AntennaTrackerPosition trackerLocation(bool *valid) const override;
    double localSnrDb() const override;

    // Explicit tracker home (MP10 _trackerloc). Clearing falls back to the
    // vehicle home again.
    void setTrackerHome(const AntennaTrackerPosition &position);
    void clearTrackerHome();
    bool hasTrackerHome() const { return m_trackerHomeSet; }

    // True when a connected main MAVLink serial link already owns portName.
    static bool PortOwnedByVehicleLink(const QString &portName);

private:
    AntennaTrackerPosition m_trackerHome;
    bool m_trackerHomeSet = false;
};

#endif // UASANTENNATRACKERTELEMETRYSOURCE_H
