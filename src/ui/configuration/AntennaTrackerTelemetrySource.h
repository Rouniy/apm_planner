#ifndef ANTENNATRACKERTELEMETRYSOURCE_H
#define ANTENNATRACKERTELEMETRYSOURCE_H

#include "AntennaTrackerGeometry.h"

#include <QObject>

/*
 * Vehicle-side inputs of Mission Planner 10's antenna tracker pages
 * (AntennaTrackerUIViewModel.StartLoop reads cs.AZToMAV / cs.ELToMAV, which
 * derive from cs.TrackerLocation and the vehicle position, and FindTrimPan
 * reads cs.localsnrdb). The shared view model only sees this interface, so unit
 * tests inject a fake and the application injects the UASManager-backed
 * implementation (UasAntennaTrackerTelemetrySource).
 */

struct AntennaTrackerVehicleFix
{
    bool valid = false;         // false when no vehicle or no position yet
    double latitude = 0.0;      // degrees
    double longitude = 0.0;     // degrees
    double altitudeAmsl = 0.0;  // metres AMSL (MP10 cs.altasl in raw metres)
};

class AntennaTrackerTelemetrySource : public QObject
{
    Q_OBJECT

public:
    explicit AntennaTrackerTelemetrySource(QObject *parent = nullptr);
    ~AntennaTrackerTelemetrySource() override;

    // The active vehicle's current position.
    virtual AntennaTrackerVehicleFix vehicleFix() const = 0;
    // MP10 cs.TrackerLocation: the explicit tracker home when one was set,
    // otherwise the vehicle home. *valid is false when neither is known.
    virtual AntennaTrackerPosition trackerLocation(bool *valid) const = 0;
    // MP10 cs.localsnrdb; 0 means "no SiK radio statistics".
    virtual double localSnrDb() const = 0;

signals:
    // The active vehicle or the tracker location changed.
    void telemetrySourceChanged();
};

#endif // ANTENNATRACKERTELEMETRYSOURCE_H
