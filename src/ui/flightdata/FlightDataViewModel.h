#ifndef FLIGHTDATAVIEWMODEL_H
#define FLIGHTDATAVIEWMODEL_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>

class HudControl;
class UASInterface;
class UASWaypointManager;

class FlightDataViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double Roll READ roll NOTIFY telemetryChanged)
    Q_PROPERTY(double Pitch READ pitch NOTIFY telemetryChanged)
    Q_PROPERTY(double Yaw READ yaw NOTIFY telemetryChanged)
    Q_PROPERTY(double Alt READ alt NOTIFY telemetryChanged)
    Q_PROPERTY(double AirSpeed READ airSpeed NOTIFY telemetryChanged)
    Q_PROPERTY(double GroundSpeed READ groundSpeed NOTIFY telemetryChanged)
    Q_PROPERTY(double VerticalSpeed READ verticalSpeed NOTIFY telemetryChanged)
    Q_PROPERTY(double SatCount READ satCount NOTIFY telemetryChanged)
    Q_PROPERTY(double GpsHdop READ gpsHdop NOTIFY telemetryChanged)
    Q_PROPERTY(int GpsFixType READ gpsFixType NOTIFY telemetryChanged)
    Q_PROPERTY(bool Armed READ armed NOTIFY telemetryChanged)
    Q_PROPERTY(bool PrearmOk READ prearmOk NOTIFY telemetryChanged)
    Q_PROPERTY(QString Mode READ mode NOTIFY telemetryChanged)
    Q_PROPERTY(double BatteryVoltage READ batteryVoltage NOTIFY telemetryChanged)
    Q_PROPERTY(int BatteryRemaining READ batteryRemaining NOTIFY telemetryChanged)
    Q_PROPERTY(double BatCurrent READ currentAmps NOTIFY telemetryChanged)
    Q_PROPERTY(double NavBearing READ navBearing NOTIFY telemetryChanged)
    Q_PROPERTY(double TargetAlt READ targetAlt NOTIFY telemetryChanged)
    Q_PROPERTY(double TargetSpeed READ targetSpeed NOTIFY telemetryChanged)
    Q_PROPERTY(double WindDir READ windDir NOTIFY telemetryChanged)
    Q_PROPERTY(double WindVel READ windVel NOTIFY telemetryChanged)
    Q_PROPERTY(double Aoa READ aoa NOTIFY telemetryChanged)
    Q_PROPERTY(double Ssa READ ssa NOTIFY telemetryChanged)
    Q_PROPERTY(double XTrackError READ xTrackError NOTIFY telemetryChanged)
    Q_PROPERTY(double TurnRate READ turnRate NOTIFY telemetryChanged)
    Q_PROPERTY(double WpDist READ wpDist NOTIFY telemetryChanged)
    Q_PROPERTY(int WpNo READ wpNo NOTIFY telemetryChanged)
    Q_PROPERTY(int MissionItemCount READ missionItemCount NOTIFY telemetryChanged)
    Q_PROPERTY(double MissionProgress READ missionProgress NOTIFY telemetryChanged)
    Q_PROPERTY(QString MissionProgressText READ missionProgressText NOTIFY telemetryChanged)
    Q_PROPERTY(double BatteryVoltage2 READ batteryVoltage2 NOTIFY telemetryChanged)
    Q_PROPERTY(int BatteryRemaining2 READ batteryRemaining2 NOTIFY telemetryChanged)
    Q_PROPERTY(double BatCurrent2 READ currentAmps2 NOTIFY telemetryChanged)
    Q_PROPERTY(double ThrottlePercent READ throttlePercent NOTIFY telemetryChanged)
    Q_PROPERTY(bool Failsafe READ failsafe NOTIFY telemetryChanged)
    Q_PROPERTY(bool SafetyActive READ safetyActive NOTIFY telemetryChanged)
    Q_PROPERTY(double LinkQuality READ linkQuality NOTIFY telemetryChanged)

public:
    explicit FlightDataViewModel(QObject *parent = nullptr, bool bindToUasManager = true);
    ~FlightDataViewModel() override;

    void attachHud(HudControl *hud);
    UASInterface *activeUAS() const;

    double roll() const { return m_roll; }
    double pitch() const { return m_pitch; }
    double yaw() const { return m_yaw; }
    double alt() const { return m_alt; }
    double airSpeed() const { return m_airSpeed; }
    double groundSpeed() const { return m_groundSpeed; }
    double verticalSpeed() const { return m_verticalSpeed; }
    double satCount() const { return m_satCount; }
    double gpsHdop() const { return m_gpsHdop; }
    int gpsFixType() const { return m_gpsFixType; }
    bool armed() const { return m_armed; }
    bool prearmOk() const { return m_prearmOk; }
    QString mode() const { return m_mode; }
    double batteryVoltage() const { return m_batteryVoltage; }
    int batteryRemaining() const { return m_batteryRemaining; }
    double currentAmps() const { return m_currentAmps; }
    double navBearing() const { return m_navBearing; }
    double targetAlt() const { return m_targetAlt; }
    double targetSpeed() const { return m_targetSpeed; }
    double windDir() const { return m_windDir; }
    double windVel() const { return m_windVel; }
    double aoa() const { return m_aoa; }
    double ssa() const { return m_ssa; }
    double xTrackError() const { return m_xTrackError; }
    double turnRate() const { return m_turnRate; }
    double wpDist() const { return m_wpDist; }
    int wpNo() const { return m_wpNo; }
    int missionItemCount() const { return m_missionItemCount; }
    double missionProgress() const { return m_missionProgress; }
    QString missionProgressText() const { return m_missionProgressText; }
    double batteryVoltage2() const { return m_batteryVoltage2; }
    int batteryRemaining2() const { return m_batteryRemaining2; }
    double currentAmps2() const { return m_currentAmps2; }
    double throttlePercent() const { return m_throttlePercent; }
    bool failsafe() const { return m_failsafe; }
    bool safetyActive() const { return m_safetyActive; }
    double linkQuality() const { return m_linkQuality; }

public slots:
    void setActiveUAS(UASInterface *uas);
    void updateAttitude(UASInterface *uas, double roll, double pitch, double yaw,
                        quint64 timestamp);
    void updateAttitudeRates(int uasId, double rollRate, double pitchRate,
                             double yawRate, quint64 timestamp);
    void updateAltitude(UASInterface *uas, double altitudeAmsl,
                        double altitudeRelative, double climbRate, quint64 timestamp);
    void updateSpeed(UASInterface *uas, double groundSpeed, double airSpeed,
                     quint64 timestamp);
    void updateBattery(UASInterface *uas, double voltage, double current,
                       double percent, int seconds);
    void updateThrust(UASInterface *uas, double thrust);
    void updateArmed(bool armed);
    void updateMode(int uasId, const QString &mode, const QString &description);
    void updateNavMode(int uasId, int mode, const QString &text);
    void updateGpsFix(UASInterface *uas, int fix);
    void updateSatelliteCount(int count, const QString &name);
    void updateGpsHdop(double value, const QString &name);
    void updateDropRate(int uasId, float receiveDrop);
    void updateNavigation(UASInterface *uas, double altitudeError,
                          double speedError, double xtrackError);
    void updateValue(int uasId, const QString &name, const QString &unit,
                     const QVariant &value, quint64 timestamp);
    void updateTextMessage(int uasId, int componentId, int severity, const QString &text);
    void updateStatus(UASInterface *uas, const QString &status,
                      const QString &description);
    void updateHeartbeat(UASInterface *uas);
    void updateHeartbeatTimeout(bool timeout, unsigned int milliseconds);
    void updateWaypoint(quint16 sequence);
    void updateWaypointDistance(double distance);

signals:
    void telemetryChanged();
    void batteryTelemetryChanged(double voltage, double remainingPercent);
    void activeUASChanged(UASInterface *uas);

private:
    void resetTelemetry();
    void publish();
    void applyToHud();
    void connectWaypointManager(UASWaypointManager *manager);
    void updateMissionProgress();
    bool isCurrentUas(UASInterface *uas) const;

    QPointer<UASInterface> m_uas;
    QPointer<UASWaypointManager> m_waypointManager;
    QPointer<HudControl> m_hud;
    QMetaObject::Connection m_hudConnection;
    double m_roll = 0.0;
    double m_pitch = 0.0;
    double m_yaw = 0.0;
    double m_alt = 0.0;
    double m_airSpeed = 0.0;
    double m_groundSpeed = 0.0;
    double m_verticalSpeed = 0.0;
    double m_satCount = 0.0;
    double m_gpsHdop = 0.0;
    int m_gpsFixType = 0;
    bool m_armed = false;
    bool m_prearmOk = false;
    QString m_mode = QStringLiteral("UNKNOWN");
    double m_batteryVoltage = 0.0;
    int m_batteryRemaining = 0;
    double m_currentAmps = 0.0;
    double m_navBearing = 0.0;
    double m_targetAlt = 0.0;
    double m_targetSpeed = 0.0;
    double m_windDir = 0.0;
    double m_windVel = 0.0;
    double m_aoa = 0.0;
    double m_ssa = 0.0;
    double m_xTrackError = 0.0;
    double m_turnRate = 0.0;
    double m_wpDist = 0.0;
    int m_wpNo = 0;
    int m_missionItemCount = 0;
    double m_missionProgress = 0.0;
    QString m_missionProgressText = QStringLiteral("No mission loaded");
    double m_batteryVoltage2 = 0.0;
    int m_batteryRemaining2 = 0;
    double m_currentAmps2 = 0.0;
    double m_throttlePercent = 0.0;
    bool m_failsafe = false;
    bool m_safetyActive = false;
    double m_linkQuality = 0.0;
};

#endif
