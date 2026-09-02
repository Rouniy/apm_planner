#ifndef SPEECHANNOUNCER_H
#define SPEECHANNOUNCER_H

#include <QElapsedTimer>
#include <QObject>

class FlightDataViewModel;
class UASInterface;

/** Mission Planner-compatible periodic speech alerts for the active vehicle. */
class SpeechAnnouncer final : public QObject
{
    Q_OBJECT

public:
    explicit SpeechAnnouncer(
        FlightDataViewModel *flightData, QObject *parent = nullptr);

private:
    void resetCountdowns(UASInterface *uas = nullptr);
    void handleBatteryTelemetry(double voltage, double remainingPercent);

    FlightDataViewModel *const m_flightData;
    QElapsedTimer m_batteryAlertInterval;
};

#endif // SPEECHANNOUNCER_H
