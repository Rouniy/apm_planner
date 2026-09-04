#ifndef SPEECHANNOUNCER_H
#define SPEECHANNOUNCER_H

#include "SpeechTelemetrySource.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class FlightDataViewModel;
class SpeechSettings;
class StatusMessageSettings;
class QTimer;
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
        quint64 generation = 0;
        qint64 connectedSinceMs = -1;
        qint64 lastPacketMs = -1;
        bool altitudeValid = false;
        double altitudeMeters = 0.0;
        bool airspeedValid = false;
        double airspeedMps = 0.0;
        bool groundSpeedValid = false;
        double groundSpeedMps = 0.0;
        bool waypointValid = false;
        int waypointNumber = 0;
        int componentId = 0;
        bool modeValid = false;
        QString mode;
        bool batteryVoltageValid = false;
        double batteryVoltage = 0.0;
        bool batteryRemainingValid = false;
        double batteryRemainingPercent = 0.0;
        int linkId = -1;
    };

    struct DisplayUnitState
    {
        QString altitudeUnits = QStringLiteral("Meters");
        QString speedUnits = QStringLiteral("meters_per_second");
    };

    using Speaker = std::function<bool(const QString &)>;
    using VehicleStateProvider = std::function<VehicleState()>;
    using Clock = std::function<qint64()>;
    using ReadyProvider = std::function<bool()>;
    using UnitProvider = std::function<DisplayUnitState()>;

    explicit SpeechAnnouncer(
        FlightDataViewModel *flightData,
        SpeechTelemetrySource *telemetrySource = nullptr,
        QObject *parent = nullptr);

    /** Injectable seam for deterministic policy and cadence tests. */
    SpeechAnnouncer(SpeechSettings *settings,
                    Speaker speaker,
                    VehicleStateProvider vehicleStateProvider,
                    Clock clock,
                    QObject *parent = nullptr,
                    ReadyProvider readyProvider = ReadyProvider(),
                    UnitProvider unitProvider = UnitProvider(),
                    StatusMessageSettings *statusSettings = nullptr);

    static QString formatTelemetryTemplate(
        const QString &speechTemplate, const VehicleState &state,
        const QString &altitudeUnits = QStringLiteral("Meters"),
        const QString &speedUnits = QStringLiteral("meters_per_second"));
    QString highMessage() const { return m_highMessage; }
    int highMessageSeverity() const { return m_highMessageSeverity; }

public slots:
    void announceFlightMode(const QString &mode);
    void announceWaypoint(int sequence);
    void announceArmState(bool armed);
    void handleBatteryTelemetry(double voltage, double remainingPercent);
    void enqueueExactTelemetry(ExactSpeechTelemetryEvent event);
    void enqueueStatusText(ExactStatusText event);
    void tick();

signals:
    void highMessageChanged(const QString &message, int severity);

private slots:
    void resetCountdowns(UASInterface *uas = nullptr);

private:
    VehicleState currentVehicle() const;
    bool speak(const QString &message) const;
    qint64 nowMs() const;
    void observeArmState(bool armed);
    void resetPeriodicCountdowns(qint64 now);
    void setHighMessage(const QString &message, int severity,
                        qint64 expiresAtMs,
                        const QString &speechText);
    void clearHighMessage();
    bool trySpeakHighMessage(const VehicleState &vehicle);
    bool leaseMatchesCurrentVehicle(const VehicleTargetLease &lease,
                                    const VehicleState &vehicle) const;
    bool eventMatchesCurrentVehicle(const ExactStatusText &event,
                                    const VehicleState &vehicle) const;

    FlightDataViewModel *m_flightData = nullptr;
    QPointer<SpeechTelemetrySource> m_telemetrySource;
    SpeechSettings *m_settings = nullptr;
    StatusMessageSettings *m_statusSettings = nullptr;
    Speaker m_speaker;
    VehicleStateProvider m_vehicleStateProvider;
    Clock m_clock;
    ReadyProvider m_readyProvider;
    UnitProvider m_unitProvider;
    QElapsedTimer m_elapsedClock;
    QTimer *m_timer = nullptr;
    QTimer *m_highMessageTimer = nullptr;
    qint64 m_nextBatteryAlertMs = 0;
    qint64 m_lastCustomMs = 0;
    qint64 m_lastLowSpeedMs = 0;
    qint64 m_lastAltWarningMs = 0;
    qint64 m_lastNoDataMs = 0;
    double m_altitudeMaximumMeters = 0.0;
    quint64 m_targetGeneration = 0;
    bool m_armStateObserved = false;
    bool m_lastArmed = false;
    QString m_highMessage;
    int m_highMessageSeverity = MAV_SEVERITY_EMERGENCY;
    qint64 m_highMessageExpiresMs = -1;
    bool m_highMessageSpoken = false;
    QString m_highMessageSpeechText;
};

#endif // SPEECHANNOUNCER_H
