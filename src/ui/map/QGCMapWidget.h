#ifndef QGCMAPWIDGET_H
#define QGCMAPWIDGET_H

#include <QMap>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include "AbstractMapWidget.h"
#include "../../../libs/opmapcontrol/opmapcontrol.h"
#include "../flightplanner/FlightPlannerMissionModel.h"
#include "../flightplanner/FlightPlannerNavigation.h"
#include "../flightplanner/WpRow.h"

class UASInterface;
class UASWaypointManager;
class Waypoint;
class QContextMenuEvent;
class MovingBaseMapMarkerItem;
class QGraphicsItemGroup;
class QGraphicsPixmapItem;
typedef mapcontrol::WayPointItem WayPointItem;

/**
 * @brief Class representing a 2D map using aerial imagery
 */
class QGCMapWidget : public mapcontrol::OPMapWidget
{
    Q_OBJECT
public:
    explicit QGCMapWidget(const QString &settingsGroup,
                          bool liveVehicleEnabled,
                          QWidget *parent = nullptr);
    ~QGCMapWidget();
//    /** @brief Convert meters to pixels */
//    float metersToPixels(double meters);
//    double headingP1P2(internals::PointLatLng p1, internals::PointLatLng p2);
//    internals::PointLatLng targetLatLon(internals::PointLatLng source, double heading, double dist);

    /** @brief Map centered on current active system */
    bool getFollowUAVEnabled() { return followUAVEnabled; }
    /** @brief The maximum map update rate */
    float getUpdateRateLimit() { return maxUpdateInterval; }
    /** @brief Get the trail type */
    int getTrailType() { return static_cast<int>(trailType); }
    /** @brief Get the trail interval */
    float getTrailInterval() { return trailInterval; }
    bool missionPlanningEnabled() const { return m_missionPlanningEnabled; }
    int CurrentZoomLevel() const;
    internals::RectLatLng VisibleTileExtent() const;
    bool hasMovingBaseMarker() const
    {
        return m_movingBaseMarker != nullptr;
    }
    MapCoordinate movingBaseCoordinate() const
    {
        return m_movingBaseCoordinate;
    }
    QString movingBaseTag() const { return m_movingBaseTag; }
    QString movingBaseLabel() const;

signals:
    void homePositionChanged(double latitude, double longitude, double altitude);
    /** @brief Signal for newly created map waypoints */
    void waypointCreated(Waypoint* wp);
    void waypointChanged(Waypoint* wp);
    void plannerCoordinateRequested(double latitude, double longitude);
    void plannerContextMenuRequested(double latitude, double longitude,
                                     const QPoint &globalPosition,
                                     int waypointSequence);
    void plannerWaypointMoved(int seq, double latitude, double longitude);

public slots:
    void setMissionPlanningEnabled(bool enabled);
    void setPlannerRows(const QVector<WpRowData> &rows,
                        FlightPlannerMissionModel::MissionStore store);
    void setPlannerAltitudePresentation(double multiplier,
                                        const QString &unit);
    void setPlannerNavigationParameters(
        const FlightPlannerNavigationParameters &parameters);
    void setPlannerDrawnPolygon(const QVector<MapCoordinate> &points);
    void setPlannerMeasurement(const QVector<MapCoordinate> &points);
    void setPlannerHome(double latitude, double longitude, double altitude);
    void clearPlannerHome();
    void setPlannerSelection(int seq);
    /** @brief Action triggered with point-camera action is selected from the context menu */
    void cameraActionTriggered();
    /** @brief Action triggered when guided action is selected from the context menu */
    void guidedActionTriggered();
    /** @brief Action triggered when guided action is selected from the context menu, allows for altitude selection */
    void guidedAltActionTriggered();
    /** @brief Add system to map view */
    void addUAS(UASInterface* uas);
    /** @brief Update the global position of a system */
    void updateGlobalPosition(UASInterface* uas, double lat, double lon, double alt, quint64 usec);
    /** @brief Update the global position of all systems */
    void updateGlobalPosition();
    /** @brief Update the local position and draw it converted to GPS reference */
    void updateLocalPosition();
    /** @brief Update the local position estimates (individual sensors) and draw it converted to GPS reference */
    void updateLocalPositionEstimates();
    /** @brief Update the type, size, etc. of this system */
    void updateSystemSpecs(int uas);
    /** @brief Change current system in focus / editing */
    void activeUASSet(UASInterface* uas);
    /** @brief Show a dialog to jump to given GPS coordinates */
    void showGoToDialog();
    /** @brief Jump to the home position on the map */
    void goHome();
    /** @brief Jump to the last recorded position of an active UAS */
    void lastPosition();
    /** @brief Update this waypoint for this UAS */
    void updateWaypoint(int uas, Waypoint* wp);
    /** @brief Update the whole waypoint */
    void updateWaypointList(int uas);
    /** @brief Redraw lines between waypoints */
    void redrawWaypointLines();
    void redrawWaypointLines(int uas);
    /** @brief Update the home position on the map */
    void updateHomePosition(double latitude, double longitude, double altitude);
    void setMovingBase(const MapCoordinate &position, const QString &tag);
    void clearMovingBase();
    void setPropagationRaster(const QImage &image,
                              const MapGeoBounds &bounds);
    void clearPropagationRaster();
    void setPropagationContour(const MapOverlayPolyline &contour);
    void clearPropagationContour();
    void setPropagationRings(const QVector<MapOverlayPolyline> &rings);
    void clearPropagationRings();
    void setPropagationStatus(const QString &legend,
                              const QString &status);
    /** @brief Set update rate limit */
    void setUpdateRateLimit(float seconds);
    /** @brief Cache visible region to harddisk */
    void cacheVisibleRegion();
    /** @brief Set follow mode */
    void setFollowUAVEnabled(bool enabled) { followUAVEnabled = enabled; }
    /** @brief Set trail to time mode and set time @param seconds The minimum time between trail dots in seconds. If set to a value < 0, trails will be disabled*/
    void setTrailModeTimed(int seconds)
    {
        trailType = seconds >= 0
            ? mapcontrol::UAVTrailType::ByTimeElapsed
            : mapcontrol::UAVTrailType::NoTrail;
        trailInterval = seconds;
        foreach(mapcontrol::UAVItem* uav, GetUAVS())
        {
            configureTrail(uav);
        }
    }
    /** @brief Set trail to distance mode and set time @param meters The minimum distance between trail dots in meters. The actual distance depends on the MAV's update rate as well. If set to a value < 0, trails will be disabled*/
    void setTrailModeDistance(int meters)
    {
        trailType = meters >= 0
            ? mapcontrol::UAVTrailType::ByDistance
            : mapcontrol::UAVTrailType::NoTrail;
        trailInterval = meters;
        foreach(mapcontrol::UAVItem* uav, GetUAVS())
        {
            configureTrail(uav);
        }
    }
    /** @brief Delete all trails */
    void deleteTrails()
    {
        foreach(mapcontrol::UAVItem* uav, GetUAVS())
        {
            uav->DeleteTrail();
        }
    }

    /** @brief Load the settings for this widget from disk */
    void loadSettings();
    /** @brief Store the settings for this widget to disk */
    void storeSettings();
    void setGlobalMapType(core::MapType::Types type);
    void refreshGlobalMapType();

protected slots:
    /** @brief Convert a map edit into a QGC waypoint event */
    void handleMapWaypointEdit(WayPointItem* waypoint);

private:
    void sendGuidedAction(Waypoint *wp, double alt);
    bool isValidGpsLocation(UASInterface* system) const;
    void configureTrail(mapcontrol::UAVItem *uav);
    void rebuildPlannerGraphics();
    void rebuildPlannerRoute();
    void clearPlannerGraphics();
    void clearPlannerLines();
    void redrawPlannerLines();
    void redrawPlannerMeasurement();
    int plannerWaypointSequenceAt(const QPoint &viewportPosition) const;
    void updateLegacyWaypointVisibility();
    void refreshMovingBaseMarker();
    void redrawPropagationRaster();
    void redrawPropagationVectors();
    void redrawPropagationStatus();
    QColor plannerColor() const;

    void shiftOtherSelectedWaypoints(mapcontrol::WayPointItem* selectedWaypoint,
                                     double shiftLong, double shiftLat);

protected:
    /** @brief Update the highlighting of the currently controlled system */
    void updateSelectedSystem(int uas);
    /** @brief Initialize */
    void showEvent(QShowEvent* event);
    void hideEvent(QHideEvent* event);
    void mousePressEvent(QMouseEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);
    void mouseDoubleClickEvent(QMouseEvent* event);
    void contextMenuEvent(QContextMenuEvent *event) override;

    QPointer<UASWaypointManager> currWPManager; ///< The current waypoint manager
    bool offlineMode;
    QMap<Waypoint* , mapcontrol::WayPointItem*> waypointsToIcons;
    QMap<mapcontrol::WayPointItem*, Waypoint*> iconsToWaypoints;
    Waypoint* firingWaypointChange;
    QTimer updateTimer;
    float maxUpdateInterval;
    enum editMode {
        EDIT_MODE_NONE,
        EDIT_MODE_WAYPOINTS,
        EDIT_MODE_SWEEP,
        EDIT_MODE_UAVS,
        EDIT_MODE_HOME,
        EDIT_MODE_SAFE_AREA,
        EDIT_MODE_CACHING
    };
    editMode currEditMode;              ///< The current edit mode on the map
    bool followUAVEnabled;              ///< Does the map follow the UAV?
    mapcontrol::UAVTrailType::Types trailType; ///< Time or distance based trail dots
    float trailInterval;                ///< Time or distance between trail items
    int followUAVID;                    ///< Which UAV should be tracked?
    bool mapInitialized;                ///< Map initialized?
    float homeAltitude;                 ///< Home altitude
    QPoint mousePressPos;               ///< Mouse position when the button is released.
    double defaultGuidedRelativeAlt;            ///< Default relative altitude for guided mode
    int defaultGuidedFrame;             ///< Default guided frame
    bool defaultGuidedAltFirstTimeSet;   ///< manages the first time set of guided alt
    QPointer<UASInterface> uas;         ///< Currently selected UAS.
    // Atlantic Ocean near Africa, coordinate origin
    double m_lastZoom;
    double m_lastLat;
    double m_lastLon;

    bool m_missionPlanningEnabled = false;
    bool m_liveVehicleEnabled = true;
    bool m_plannerGraphicsUpdate = false;
    bool m_plannerHomeValid = false;
    double m_plannerHomeLatitude = 0.0;
    double m_plannerHomeLongitude = 0.0;
    double m_plannerHomeAltitude = 0.0;
    double m_plannerAltitudeMultiplier = 1.0;
    QString m_plannerAltitudeUnit = QStringLiteral("m");
    int m_plannerSelection = -1;
    QVector<WpRowData> m_plannerRows;
    QVector<MapCoordinate> m_plannerDrawnPolygon;
    QVector<MapCoordinate> m_plannerMeasurementPoints;
    QVector<FlightPlannerRoutePoint> m_plannerRenderedRoute;
    FlightPlannerNavigationParameters m_plannerNavigation;
    FlightPlannerMissionModel::MissionStore m_plannerStore =
            FlightPlannerMissionModel::MissionStore::Mission;
    QMap<int, mapcontrol::WayPointItem*> m_plannerIcons;
    QMap<mapcontrol::WayPointItem*, int> m_plannerIconSequences;
    mapcontrol::WayPointItem *m_plannerHomeIcon = nullptr;
    QGraphicsItemGroup *m_plannerLineGroup = nullptr;
    QGraphicsItemGroup *m_plannerMeasurementGroup = nullptr;
    MovingBaseMapMarkerItem *m_movingBaseMarker = nullptr;
    MapCoordinate m_movingBaseCoordinate;
    QString m_movingBaseTag;
    QGraphicsPixmapItem *m_propagationRasterItem = nullptr;
    QGraphicsItemGroup *m_propagationContourGroup = nullptr;
    QGraphicsItemGroup *m_propagationRingGroup = nullptr;
    QGraphicsItemGroup *m_propagationStatusGroup = nullptr;
    QImage m_propagationRaster;
    MapGeoBounds m_propagationRasterBounds;
    MapOverlayPolyline m_propagationContour;
    QVector<MapOverlayPolyline> m_propagationRings;
    QString m_propagationLegend;
    QString m_propagationStatus;
    QString m_settingsGroup;

};

#endif // QGCMAPWIDGET_H
