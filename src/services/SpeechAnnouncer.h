#ifndef SPEECHANNOUNCER_H
#define SPEECHANNOUNCER_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <functional>

class FlightDataViewModel;
class SpeechSettings;
class UASInterface;

/** Mission Planner-compatible speech announcements for the active vehicle. */
class SpeechAnnouncer final : public QObject
{
    Q_OBJECT

public:
    struct VehicleState
    {
        int systemId = 0;
        bool armed = false;
        bool valid = false;
    };

    using Speaker = std::function<bool(const QString &)>;
    using VehicleStateProvider = std::function<VehicleState()>;
    using Clock = std::function<qint64()>;

    explicit SpeechAnnouncer(
        FlightDataViewModel *flightData, QObject *parent = nullptr);

    /** Injectable seam for deterministic policy and cadence tests. */
    SpeechAnnouncer(SpeechSettings *settings,
                    Speaker speaker,
                    VehicleStateProvider vehicleStateProvider,
                    Clock clock,
                    QObject *parent = nullptr);

public slots:
    void announceFlightMode(const QString &mode);
    void announceWaypoint(int sequence);
    void announceArmState(bool armed);
    void handleBatteryTelemetry(double voltage, double remainingPercent);

private slots:
    void resetCountdowns(UASInterface *uas = nullptr);

private:
    VehicleState currentVehicle() const;
    bool speak(const QString &message) const;
    qint64 nowMs() const;

    FlightDataViewModel *m_flightData = nullptr;
    SpeechSettings *m_settings = nullptr;
    Speaker m_speaker;
    VehicleStateProvider m_vehicleStateProvider;
    Clock m_clock;
    QElapsedTimer m_elapsedClock;
    qint64 m_nextBatteryAlertMs = 0;
};

#endif // SPEECHANNOUNCER_H
