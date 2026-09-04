#ifndef OPMAPBACKENDWIDGET_H
#define OPMAPBACKENDWIDGET_H

#include "AbstractMapWidget.h"

#include <QPointer>

class QGCMapWidget;
namespace mapcontrol {
class GPSItem;
}

class OPMapBackendWidget final : public AbstractMapWidget
{
    Q_OBJECT

public:
    explicit OPMapBackendWidget(MapWidgetRole role,
                                const QString &sharedCacheRoot,
                                QWidget *widgetParent = nullptr,
                                QObject *parent = nullptr);
    ~OPMapBackendWidget() override;

    static void RegisterBackend();

    QString BackendId() const override;
    QString SharedCacheRoot() const override;
    QWidget *Widget() const override;
    QGCMapWidget *OPMapWidget() const;

    int MinZoom() const override;
    int MaxZoom() const override;
    double ZoomReal() const override;
    int CurrentZoomLevel() const override;
    QSize ViewportPixelSize() const override;
    MapGeoBounds VisibleTileExtent() const override;
    MapCoordinate CurrentPosition() const override;
    core::MapType::Types CurrentMapType() const override;
    bool FollowUAVEnabled() const override;
    float UpdateRateLimit() const override;
    int TrailType() const override;
    float TrailInterval() const override;

    void SetZoom(double zoom) override;
    void SetCurrentPosition(double latitude, double longitude) override;
    void SetAcceleratedRenderingEnabled(bool enabled) override;
    void SetFollowUAVEnabled(bool enabled) override;
    void SetTrailModeTimed(int seconds) override;
    void SetTrailModeDistance(int metres) override;
    void DeleteTrails() override;
    void SetUpdateRateLimit(float seconds) override;
    void ShowGoToDialog() override;
    void GoHome() override;
    void LastPosition() override;
    void CacheVisibleRegion() override;
    void UpdateHomePosition(double latitude, double longitude,
                            double altitude) override;
    void SetMovingBase(const MapCoordinate &position,
                       const QString &tag) override;
    void ClearMovingBase() override;
    void SetPropagationRaster(const QImage &image,
                              const MapGeoBounds &bounds) override;
    void ClearPropagationRaster() override;
    void SetPropagationContour(
        const MapOverlayPolyline &contour) override;
    void ClearPropagationContour() override;
    void SetPropagationRings(
        const QVector<MapOverlayPolyline> &rings) override;
    void ClearPropagationRings() override;
    void SetPropagationStatus(const QString &legend,
                              const QString &status) override;
    void SetMissionPlanningEnabled(bool enabled) override;
    void SetPlannerRows(
        const QVector<WpRowData> &rows,
        FlightPlannerMissionModel::MissionStore store) override;
    void SetPlannerAltitudePresentation(
        double multiplier, const QString &unit) override;
    void SetPlannerNavigationParameters(
        const FlightPlannerNavigationParameters &parameters) override;
    void SetPlannerDrawnPolygon(
        const QVector<MapCoordinate> &points) override;
    void SetPlannerMeasurement(
        const QVector<MapCoordinate> &points) override;
    void SetPlannerHome(double latitude, double longitude,
                        double altitude) override;
    void ClearPlannerHome() override;
    void SetPlannerSelection(int sequence) override;
    void SetLogTrail(const QVector<MapCoordinate> &points) override;
    void SetLogCursor(const MapCoordinate &position,
                      double heading) override;

private:
    static QString SettingsGroup(MapWidgetRole role);

    QPointer<QGCMapWidget> m_map;
    mapcontrol::GPSItem *m_logTrail = nullptr;
    mapcontrol::GPSItem *m_logCursor = nullptr;
};

#endif // OPMAPBACKENDWIDGET_H
