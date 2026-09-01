#ifndef FLIGHTPLANNERVIEWMODEL_H
#define FLIGHTPLANNERVIEWMODEL_H

#include "FlightPlannerMissionModel.h"

#include <QObject>
#include <QStringList>

class FlightPlannerViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(FlightPlannerMissionModel *Waypoints READ Waypoints CONSTANT)
    Q_PROPERTY(QStringList MissionTypes READ MissionTypes CONSTANT)
    Q_PROPERTY(QString MissionType READ MissionType WRITE setMissionType
               NOTIFY missionTypeChanged)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(bool UseMavFtp READ UseMavFtp WRITE setUseMavFtp
               NOTIFY useMavFtpChanged)
    Q_PROPERTY(bool UseMissionMavFtp READ UseMavFtp WRITE setUseMavFtp
               NOTIFY useMavFtpChanged)
    Q_PROPERTY(double HomeLat READ HomeLat WRITE setHomeLat NOTIFY homeLatChanged)
    Q_PROPERTY(double HomeLng READ HomeLng WRITE setHomeLng NOTIFY homeLngChanged)
    Q_PROPERTY(double HomeAlt READ HomeAlt WRITE setHomeAlt NOTIFY homeAltChanged)
    Q_PROPERTY(double HomeAltDisplay READ HomeAlt WRITE setHomeAlt
               NOTIFY homeAltChanged)
    Q_PROPERTY(bool HomeValid READ HomeValid NOTIFY homeValidChanged)
    Q_PROPERTY(QString HomeAltLabel READ HomeAltLabel CONSTANT)
    Q_PROPERTY(double DefaultAltitude READ DefaultAltitude
               WRITE setDefaultAltitude NOTIFY defaultAltitudeChanged)

public:
    explicit FlightPlannerViewModel(QObject *parent = nullptr);

    FlightPlannerMissionModel *Waypoints();
    const FlightPlannerMissionModel *Waypoints() const;
    QStringList MissionTypes() const;
    QString MissionType() const;
    QString Status() const;
    bool UseMavFtp() const;
    double HomeLat() const;
    double HomeLng() const;
    double HomeAlt() const;
    bool HomeValid() const;
    QString HomeAltLabel() const;
    double DefaultAltitude() const;

public slots:
    void setMissionType(const QString &type);
    void setUseMavFtp(bool enabled);
    void setHomeLat(double value);
    void setHomeLng(double value);
    void setHomeAlt(double value);
    void setDefaultAltitude(double value);
    void SetHomeFromVehicle(double latitude, double longitude,
                            double altitudeAsl);

    WpRow *AddWaypointAt(double latitude, double longitude);
    WpRow *AddWaypointAt(double latitude, double longitude, double altitude);
    bool DeleteWaypoint(int row);
    bool MoveWaypointUp(int row);
    bool MoveWaypointDown(int row);
    void ClearWaypoints();

    bool LoadFile(const QString &path, bool append = false);
    bool LoadAndAppend(const QString &path);
    bool SaveFile(const QString &path);

signals:
    void missionTypeChanged(const QString &type);
    void statusChanged(const QString &status);
    void useMavFtpChanged(bool enabled);
    void homeLatChanged(double value);
    void homeLngChanged(double value);
    void homeAltChanged(double value);
    void homeValidChanged(bool valid);
    void defaultAltitudeChanged(double value);

private:
    void setStatus(const QString &status);
    void markHomeValid();
    void clearHome();

    FlightPlannerMissionModel m_waypoints;
    QString m_status = QStringLiteral("ready");
    bool m_useMavFtp = false;
    double m_homeLat = 0.0;
    double m_homeLng = 0.0;
    double m_homeAlt = 0.0;
    bool m_homeValid = false;
    double m_defaultAltitude = 100.0;
};

#endif
