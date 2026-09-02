#ifndef FLIGHTPLANNERVIEWMODEL_H
#define FLIGHTPLANNERVIEWMODEL_H

#include "FlightPlannerNavigation.h"
#include "FlightPlannerMissionModel.h"
#include "FlightPlannerPolygonModel.h"
#include "MissionElevationProfile.h"
#include "SurveyMissionBuilder.h"

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

class MissionTransferController;
struct MissionTransferResult;

class FlightPlannerViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(FlightPlannerMissionModel *Waypoints READ Waypoints CONSTANT)
    Q_PROPERTY(FlightPlannerPolygonModel *DrawnPolygon READ DrawnPolygon
               CONSTANT)
    Q_PROPERTY(bool PolygonDrawMode READ PolygonDrawMode
               WRITE setPolygonDrawMode NOTIFY polygonDrawModeChanged)
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
    Q_PROPERTY(double HomeAltDisplay READ HomeAltDisplay WRITE setHomeAltDisplay
               NOTIFY homeAltDisplayChanged)
    Q_PROPERTY(bool HomeValid READ HomeValid NOTIFY homeValidChanged)
    Q_PROPERTY(QString HomeAltLabel READ HomeAltLabel
               NOTIFY altitudePresentationChanged)
    Q_PROPERTY(double DefaultAltitude READ DefaultAltitude
               WRITE setDefaultAltitude NOTIFY defaultAltitudeChanged)
    Q_PROPERTY(double DefaultAltitudeDisplay READ DefaultAltitudeDisplay
               WRITE setDefaultAltitudeDisplay
               NOTIFY defaultAltitudeDisplayChanged)
    Q_PROPERTY(QStringList DefaultFrames READ DefaultFrames CONSTANT)
    Q_PROPERTY(QString DefaultFrame READ DefaultFrame WRITE setDefaultFrame
               NOTIFY defaultFrameChanged)
    Q_PROPERTY(double WpRadius READ WpRadius WRITE setWpRadius
               NOTIFY wpRadiusChanged)
    Q_PROPERTY(double WpRadiusDisplay READ WpRadiusDisplay
               WRITE setWpRadiusDisplay NOTIFY wpRadiusDisplayChanged)
    Q_PROPERTY(double LoiterRadius READ LoiterRadius WRITE setLoiterRadius
               NOTIFY loiterRadiusChanged)
    Q_PROPERTY(double LoiterRadiusDisplay READ LoiterRadiusDisplay
               WRITE setLoiterRadiusDisplay NOTIFY loiterRadiusDisplayChanged)
    Q_PROPERTY(double AltWarn READ AltWarn WRITE setAltWarn
               NOTIFY altWarnChanged)
    Q_PROPERTY(double AltWarnDisplay READ AltWarnDisplay WRITE setAltWarnDisplay
               NOTIFY altWarnDisplayChanged)
    Q_PROPERTY(QStringList AltitudeUnits READ AltitudeUnits CONSTANT)
    Q_PROPERTY(QString AltUnits READ AltUnits WRITE setAltUnits
               NOTIFY altUnitsChanged)
    Q_PROPERTY(QString AltUnit READ AltUnit
               NOTIFY altitudePresentationChanged)
    Q_PROPERTY(double AltitudeMultiplier READ AltitudeMultiplier
               NOTIFY altitudePresentationChanged)
    Q_PROPERTY(QStringList DistanceUnits READ DistanceUnits CONSTANT)
    Q_PROPERTY(QString DistUnits READ DistUnits WRITE setDistUnits
               NOTIFY distUnitsChanged)
    Q_PROPERTY(QString DistanceUnit READ DistanceUnit
               NOTIFY distancePresentationChanged)
    Q_PROPERTY(double DistanceMultiplier READ DistanceMultiplier
               NOTIFY distancePresentationChanged)
    Q_PROPERTY(bool SplineDefault READ SplineDefault WRITE setSplineDefault
               NOTIFY splineDefaultChanged)
    Q_PROPERTY(bool VerifyHeight READ VerifyHeight WRITE setVerifyHeight
               NOTIFY verifyHeightChanged)
    Q_PROPERTY(bool ShowWpRadius READ ShowWpRadius NOTIFY vehicleTypeChanged)
    Q_PROPERTY(bool ShowLoiterRadius READ ShowLoiterRadius
               NOTIFY vehicleTypeChanged)
    Q_PROPERTY(QString TotalDist READ TotalDist NOTIFY routeMetricsChanged)
    Q_PROPERTY(QString HomeDist READ HomeDist NOTIFY routeMetricsChanged)
    Q_PROPERTY(QString PrevDist READ PrevDist NOTIFY routeMetricsChanged)
    Q_PROPERTY(double WpAccelerationCms READ WpAccelerationCms
               NOTIFY plannerNavigationChanged)
    Q_PROPERTY(double WpSpeedCms READ WpSpeedCms
               NOTIFY plannerNavigationChanged)
    Q_PROPERTY(bool TransferBusy READ TransferBusy NOTIFY transferBusyChanged)
    Q_PROPERTY(int TransferProgress READ TransferProgress
               NOTIFY transferProgressChanged)
    Q_PROPERTY(bool CanReadWaypoints READ CanReadWaypoints
               NOTIFY transferAvailabilityChanged)
    Q_PROPERTY(bool CanWriteWaypoints READ CanWriteWaypoints
               NOTIFY transferAvailabilityChanged)
    Q_PROPERTY(bool CanCancelTransfer READ CanCancelTransfer
               NOTIFY transferAvailabilityChanged)
    Q_PROPERTY(bool CanSetHomeFromVehicle READ CanSetHomeFromVehicle
               NOTIFY vehicleHomeProviderChanged)
    Q_PROPERTY(bool CanUndo READ CanUndo NOTIFY canUndoChanged)

public:
    using VehicleHomeProvider =
            std::function<bool(double *latitude, double *longitude,
                               double *altitudeAsl)>;
    using VehiclePositionProvider =
            std::function<bool(double *latitude, double *longitude,
                               double *altitudeRelative)>;
    using VehicleParameterReader =
            std::function<bool(const QString &name, double *value)>;
    using VehicleParameterWriter =
            std::function<bool(const QString &name, double value)>;
    using TerrainAltitudeProvider =
            std::function<bool(double latitude, double longitude,
                               double *altitudeAmslMeters)>;

    explicit FlightPlannerViewModel(QObject *parent = nullptr);
    ~FlightPlannerViewModel() override;

    FlightPlannerMissionModel *Waypoints();
    const FlightPlannerMissionModel *Waypoints() const;
    FlightPlannerPolygonModel *DrawnPolygon();
    const FlightPlannerPolygonModel *DrawnPolygon() const;
    bool PolygonDrawMode() const;
    QStringList MissionTypes() const;
    QString MissionType() const;
    QString Status() const;
    bool UseMavFtp() const;
    double HomeLat() const;
    double HomeLng() const;
    double HomeAlt() const;
    double HomeAltDisplay() const;
    bool HomeValid() const;
    QString HomeAltLabel() const;
    double DefaultAltitude() const;
    double DefaultAltitudeDisplay() const;
    QStringList DefaultFrames() const;
    QString DefaultFrame() const;
    quint8 DefaultFrameId() const;
    double WpRadius() const;
    double WpRadiusDisplay() const;
    double LoiterRadius() const;
    double LoiterRadiusDisplay() const;
    double AltWarn() const;
    double AltWarnDisplay() const;
    QStringList AltitudeUnits() const;
    QString AltUnits() const;
    QString AltUnit() const;
    double AltitudeMultiplier() const;
    QStringList DistanceUnits() const;
    QString DistUnits() const;
    QString DistanceUnit() const;
    double DistanceMultiplier() const;
    bool SplineDefault() const;
    bool VerifyHeight() const;
    bool ShowWpRadius() const;
    bool ShowLoiterRadius() const;
    QString TotalDist() const;
    QString HomeDist() const;
    QString PrevDist() const;
    double WpAccelerationCms() const;
    double WpSpeedCms() const;
    FlightPlannerNavigationParameters NavigationParameters() const;
    MissionElevationProfileResult BuildElevationProfile(
        double sampleSpacingMeters = 10.0,
        int maximumSamples = 20000) const;
    bool TransferBusy() const;
    int TransferProgress() const;
    bool CanReadWaypoints() const;
    bool CanWriteWaypoints() const;
    bool CanCancelTransfer() const;
    bool CanSetHomeFromVehicle() const;
    bool CanUndo() const;

    Q_INVOKABLE bool SetHome(double latitude, double longitude);
    Q_INVOKABLE bool SetFenceReturn(double latitude, double longitude);
    Q_INVOKABLE bool InsertWaypointAt(int index, double latitude,
                                      double longitude, double altitude);
    Q_INVOKABLE bool InsertRegularWaypointAt(int index, double latitude,
                                             double longitude,
                                             double altitude);
    Q_INVOKABLE bool InsertSplineWaypointAt(int index, double latitude,
                                            double longitude,
                                            double altitude);
    Q_INVOKABLE bool AddWaypointAtCurrentPosition();
    Q_INVOKABLE bool MoveWaypoint(int row, double latitude,
                                  double longitude);
    Q_INVOKABLE bool AddTakeoff(double latitude, double longitude,
                                double altitude);
    Q_INVOKABLE bool AddLand(double latitude, double longitude);
    Q_INVOKABLE bool AddRtl();
    Q_INVOKABLE bool AddRoi(double latitude, double longitude);
    Q_INVOKABLE bool AddLoiterForever(double latitude, double longitude);
    Q_INVOKABLE bool AddLoiterTime(double latitude, double longitude,
                                   double seconds);
    Q_INVOKABLE bool AddLoiterTurns(double latitude, double longitude,
                                    double turns);
    Q_INVOKABLE bool AddJump(int targetMissionItem, int repeatCount);
    Q_INVOKABLE bool ReverseWaypoints();
    Q_INVOKABLE bool ModifyAllAlt(const QString &expression);
    Q_INVOKABLE bool Undo();

    /**
     * Bind the mission controller owned by the currently active UAS.
     * Rebinding cancels a transfer started by this ViewModel on the old
     * controller before attaching the new one. The controller remains owned
     * by its UAS.
     */
    void setMissionTransferController(MissionTransferController *controller);
    void setVehicleHomeProvider(VehicleHomeProvider provider);
    void setVehiclePositionProvider(VehiclePositionProvider provider);
    void setTerrainAltitudeProvider(TerrainAltitudeProvider provider);
    void setVehicleType(int mavType);
    void setVehicleParameterAccess(VehicleParameterReader reader,
                                   VehicleParameterWriter writer);
    int WriteRadiusParams();

public slots:
    void setMissionType(const QString &type);
    void setPolygonDrawMode(bool enabled);
    void setUseMavFtp(bool enabled);
    void setHomeLat(double value);
    void setHomeLng(double value);
    void setHomeAlt(double value);
    void setHomeAltDisplay(double value);
    void setDefaultAltitude(double value);
    void setDefaultAltitudeDisplay(double value);
    void setDefaultFrame(const QString &value);
    void setWpRadius(double value);
    void setWpRadiusDisplay(double value);
    void setLoiterRadius(double value);
    void setLoiterRadiusDisplay(double value);
    void setAltWarn(double value);
    void setAltWarnDisplay(double value);
    void setAltUnits(const QString &value);
    void setDistUnits(const QString &value);
    void setSplineDefault(bool enabled);
    void setVerifyHeight(bool enabled);
    void setStatus(const QString &status);
    void RefreshVehicleParameters();
    void UpdateVehicleParameter(const QString &name, const QVariant &value);
    void SetHomeFromVehicle();
    void SetHomeFromVehicle(double latitude, double longitude,
                            double altitudeAsl);

    void ReadWaypoints();
    void WriteWaypoints();
    void CancelTransfer();

    WpRow *AddWaypointAt(double latitude, double longitude);
    WpRow *AddWaypointAt(double latitude, double longitude, double altitude);
    bool DeleteWaypoint(int row);
    bool MoveWaypointUp(int row);
    bool MoveWaypointDown(int row);
    void ClearWaypoints();

    bool AddPolygonPoint(double latitude, double longitude);
    bool AddPolygonPoint(double latitude, double longitude, double altitude);
    void ClearPolygon();
    bool BuildPolygonFromWaypoints();
    bool OffsetDrawnPolygon(double meters);
    bool LoadPolygon(const QString &path, bool append = false);
    bool SavePolygon(const QString &path);
    bool AddDrawnPolygonToFence(bool inclusion);

    bool LoadFile(const QString &path, bool append = false);
    bool LoadAndAppend(const QString &path);
    bool SaveFile(const QString &path);
    bool LoadPlanFile(const QString &path, bool append = false);
    bool SavePlanFile(const QString &path);
    QVector<SurveyGridCoordinate> SurveyBoundary(double altitude) const;
    bool AppendSurveyPlan(const SurveyMissionPlan &plan);

signals:
    void missionTypeChanged(const QString &type);
    void polygonDrawModeChanged(bool enabled);
    void statusChanged(const QString &status);
    void useMavFtpChanged(bool enabled);
    void homeLatChanged(double value);
    void homeLngChanged(double value);
    void homeAltChanged(double value);
    void homeAltDisplayChanged(double value);
    void homeValidChanged(bool valid);
    void defaultAltitudeChanged(double value);
    void defaultAltitudeDisplayChanged(double value);
    void defaultFrameChanged(const QString &value);
    void wpRadiusChanged(double value);
    void wpRadiusDisplayChanged(double value);
    void loiterRadiusChanged(double value);
    void loiterRadiusDisplayChanged(double value);
    void altWarnChanged(double value);
    void altWarnDisplayChanged(double value);
    void altUnitsChanged(const QString &value);
    void altitudePresentationChanged();
    void distUnitsChanged(const QString &value);
    void distancePresentationChanged();
    void splineDefaultChanged(bool enabled);
    void verifyHeightChanged(bool enabled);
    void vehicleTypeChanged(int mavType);
    void routeMetricsChanged();
    void plannerNavigationChanged();
    void transferBusyChanged(bool busy);
    void transferProgressChanged(int progress);
    void transferAvailabilityChanged();
    void vehicleHomeProviderChanged(bool available);
    void canUndoChanged(bool available);

private:
    void setTransferBusy(bool busy);
    void setTransferProgress(int progress);
    void clearActiveTransfer();
    void handleTransferFinished(MissionTransferController *controller,
                                quint64 bindingGeneration,
                                const MissionTransferResult &result);
    void markHomeValid();
    void clearHome();
    void applyVehicleParameters();
    bool requireEditableMission(const QString &operation);
    void captureStoreUndo(FlightPlannerMissionModel::MissionStore store);
    void captureMissionUndo();
    bool replaceMissionRows(const QVector<WpRowData> &rows,
                            const QString &status);
    bool appendMissionCommand(quint16 command, quint8 frame,
                              double latitude, double longitude,
                              double altitude, double param1 = 0.0,
                              double param2 = 0.0, double param3 = 0.0,
                              double param4 = 0.0);
    bool insertNavigationWaypointAt(int index, quint16 command,
                                    double latitude, double longitude,
                                    double altitude);
    bool sampleTerrainAltitude(double latitude, double longitude,
                               double *altitudeAmslMeters) const;
    double verifyPlaceAltitude(double latitude, double longitude,
                               double baseAltitude, quint8 frame) const;
    void recomputeRouteMetrics();

    FlightPlannerMissionModel m_waypoints;
    FlightPlannerPolygonModel m_drawnPolygon;
    bool m_polygonDrawMode = false;
    QString m_status = QStringLiteral("ready");
    bool m_useMavFtp = false;
    double m_homeLat = 0.0;
    double m_homeLng = 0.0;
    double m_homeAlt = 0.0;
    bool m_homeValid = false;
    double m_defaultAltitude = 100.0;
    QString m_defaultFrame = QStringLiteral("Relative");
    double m_wpRadius = 90.0;
    double m_loiterRadius = 100.0;
    double m_altWarn = 0.0;
    QString m_altUnits = QStringLiteral("Meters");
    double m_altitudeMultiplier = 1.0;
    QString m_distUnits = QStringLiteral("Meters");
    double m_distanceMultiplier = 1.0;
    bool m_splineDefault = false;
    bool m_verifyHeight = false;
    int m_vehicleType = -1;
    QString m_totalDist = QStringLiteral("0.0000 km");
    QString m_homeDist = QStringLiteral("0.00 m");
    QString m_prevDist = QStringLiteral("0.00 m");
    bool m_recomputingRouteMetrics = false;
    FlightPlannerNavigationParameters m_navigationParameters;
    QPointer<MissionTransferController> m_missionTransferController;
    QPointer<MissionTransferController> m_activeTransferController;
    quint64 m_controllerBindingGeneration = 0;
    quint64 m_activeTransferId = 0;
    quint8 m_activeMissionType = 0;
    FlightPlannerMissionModel::MissionStore m_activeTransferStore =
            FlightPlannerMissionModel::MissionStore::Mission;
    int m_activeTransferRowCount = 0;
    bool m_activeTransferIsDownload = false;
    bool m_transferBusy = false;
    int m_transferProgress = 0;
    VehicleHomeProvider m_vehicleHomeProvider;
    VehiclePositionProvider m_vehiclePositionProvider;
    TerrainAltitudeProvider m_terrainAltitudeProvider;
    VehicleParameterReader m_vehicleParameterReader;
    VehicleParameterWriter m_vehicleParameterWriter;
    QMap<QString, QVariant> m_vehicleParameters;
    struct UndoEntry
    {
        FlightPlannerMissionModel::MissionStore changedStore =
                FlightPlannerMissionModel::MissionStore::Mission;
        FlightPlannerMissionModel::MissionStore previousActiveStore =
                FlightPlannerMissionModel::MissionStore::Mission;
        QVector<WpRowData> rows;
    };
    QVector<UndoEntry> m_undoHistory;
};

#endif
