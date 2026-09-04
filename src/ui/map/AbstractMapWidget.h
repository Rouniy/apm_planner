#ifndef ABSTRACTMAPWIDGET_H
#define ABSTRACTMAPWIDGET_H

#include "maptype.h"
#include "ui/flightplanner/FlightPlannerMissionModel.h"
#include "ui/flightplanner/WpRow.h"

#include <QObject>
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QVector>

#include <cmath>

class QWidget;
struct FlightPlannerNavigationParameters;

enum class MapWidgetRole
{
    FlightData,
    FlightPlanner,
    Simulation,
    LogAnalysis,
    Preview
};

struct MapGeoBounds
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    bool IsValid() const
    {
        return std::isfinite(left) && std::isfinite(top)
            && std::isfinite(right) && std::isfinite(bottom)
            && left >= -180.0 && left <= 180.0
            && right >= -180.0 && right <= 180.0
            && top >= -90.0 && top <= 90.0
            && bottom >= -90.0 && bottom <= 90.0
            && right != left && top > bottom;
    }

    bool CrossesDateLine() const { return IsValid() && right < left; }
};

struct MapCoordinate
{
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;

    bool IsValid() const
    {
        return std::isfinite(latitude) && std::isfinite(longitude)
            && std::isfinite(altitude)
            && latitude >= -90.0 && latitude <= 90.0
            && longitude >= -180.0 && longitude <= 180.0;
    }
};

struct MapOverlayPolyline
{
    QVector<MapCoordinate> points;
    QColor color = Qt::white;
    qreal width = 1.0;
    bool dashed = false;
};

// Backend-neutral contract used by PLAN, DATA and SIMULATION. A backend owns
// the rendering QWidget but exposes Mission Planner map behavior here so the
// shell never needs to know whether OPMap or a Qt Quick backend is active.
class AbstractMapWidget : public QObject
{
    Q_OBJECT

public:
    explicit AbstractMapWidget(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~AbstractMapWidget() override = default;

    virtual QString BackendId() const = 0;
    virtual QString SharedCacheRoot() const = 0;
    virtual QWidget *Widget() const = 0;

    virtual int MinZoom() const = 0;
    virtual int MaxZoom() const = 0;
    virtual double ZoomReal() const = 0;
    virtual int CurrentZoomLevel() const = 0;
    virtual QSize ViewportPixelSize() const = 0;
    virtual MapGeoBounds VisibleTileExtent() const = 0;
    virtual MapCoordinate CurrentPosition() const = 0;
    virtual core::MapType::Types CurrentMapType() const = 0;

    virtual bool FollowUAVEnabled() const = 0;
    virtual float UpdateRateLimit() const = 0;
    virtual int TrailType() const = 0;
    virtual float TrailInterval() const = 0;

    virtual void SetZoom(double zoom) = 0;
    virtual void SetCurrentPosition(double latitude, double longitude) = 0;
    virtual void SetAcceleratedRenderingEnabled(bool enabled) = 0;
    virtual void SetFollowUAVEnabled(bool enabled) = 0;
    virtual void SetTrailModeTimed(int seconds) = 0;
    virtual void SetTrailModeDistance(int metres) = 0;
    virtual void DeleteTrails() = 0;
    virtual void SetUpdateRateLimit(float seconds) = 0;
    virtual void ShowGoToDialog() = 0;
    virtual void GoHome() = 0;
    virtual void LastPosition() = 0;
    virtual void CacheVisibleRegion() = 0;
    virtual void UpdateHomePosition(double latitude, double longitude,
                                    double altitude) = 0;
    virtual void SetMovingBase(const MapCoordinate &position,
                               const QString &tag)
    {
        Q_UNUSED(position)
        Q_UNUSED(tag)
    }
    virtual void ClearMovingBase() {}
    virtual void SetPropagationRaster(const QImage &image,
                                      const MapGeoBounds &bounds) = 0;
    virtual void ClearPropagationRaster() = 0;
    virtual void SetPropagationContour(
        const MapOverlayPolyline &contour) = 0;
    virtual void ClearPropagationContour() = 0;
    virtual void SetPropagationRings(
        const QVector<MapOverlayPolyline> &rings) = 0;
    virtual void ClearPropagationRings() = 0;
    virtual void SetPropagationStatus(const QString &legend,
                                      const QString &status) = 0;
    void ClearPropagationOverlay()
    {
        ClearPropagationRaster();
        ClearPropagationContour();
        ClearPropagationRings();
        SetPropagationStatus({}, {});
    }

    virtual void SetMissionPlanningEnabled(bool enabled) = 0;
    virtual void SetPlannerRows(
        const QVector<WpRowData> &rows,
        FlightPlannerMissionModel::MissionStore store) = 0;
    virtual void SetPlannerAltitudePresentation(
        double multiplier, const QString &unit) = 0;
    virtual void SetPlannerNavigationParameters(
        const FlightPlannerNavigationParameters &parameters)
    {
        Q_UNUSED(parameters)
    }
    virtual void SetPlannerDrawnPolygon(
        const QVector<MapCoordinate> &points)
    {
        Q_UNUSED(points)
    }
    virtual void SetPlannerMeasurement(
        const QVector<MapCoordinate> &points)
    {
        Q_UNUSED(points)
    }
    virtual void SetPlannerHome(double latitude, double longitude,
                                double altitude) = 0;
    virtual void ClearPlannerHome() = 0;
    virtual void SetPlannerSelection(int sequence) = 0;
    virtual void SetLogTrail(const QVector<MapCoordinate> &points) = 0;
    virtual void SetLogCursor(const MapCoordinate &position,
                              double heading) = 0;

signals:
    void ZoomChanged(int zoom);
    void TileLoadStarted();
    void TileLoadCompleted();
    void TilesStillToLoad(int count);
    void PlannerCoordinateRequested(double latitude, double longitude);
    void PlannerContextMenuRequested(double latitude, double longitude,
                                     const QPoint &globalPosition,
                                     int waypointSequence);
    void PlannerWaypointMoved(int sequence, double latitude,
                              double longitude);
};

#endif // ABSTRACTMAPWIDGET_H
