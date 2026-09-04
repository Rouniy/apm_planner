#ifndef PROPAGATIONOVERLAYCONTROLLER_H
#define PROPAGATIONOVERLAYCONTROLLER_H

#include "PropagationCore.h"
#include "PropagationSettingsStore.h"
#include "PropagationTelemetrySource.h"
#include "ui/map/AbstractMapWidget.h"

#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

class QThread;

struct PropagationMapState
{
    bool homeValid = false;
    PropagationCore::GeoPoint home;
    PropagationTelemetrySnapshot telemetry;
};

/** Coordinates MP10 propagation work and publishes backend-neutral overlays. */
class PropagationOverlayController final : public QObject
{
    Q_OBJECT

public:
    using StateProvider = std::function<PropagationMapState()>;
    using TerrainProvider = PropagationCore::TerrainProvider;

    explicit PropagationOverlayController(
        AbstractMapWidget *map,
        StateProvider stateProvider,
        TerrainProvider terrainProvider,
        PropagationSettingsStore *settings = nullptr,
        QObject *parent = nullptr);
    ~PropagationOverlayController() override;

    static double quantizeAltitude(double altitudeAmsl);

public slots:
    void setActive(bool active);
    void refresh();
    void invalidateTerrain();

private:
    struct RasterWorkerResult;
    struct RfWorkerResult;

    void refreshDistance(const PropagationMapState &state,
                         const PropagationSettings &settings);
    void refreshRaster(const PropagationMapState &state,
                       const PropagationSettings &settings);
    void refreshRf(const PropagationMapState &state,
                   const PropagationSettings &settings);
    void startRaster(const QString &key, const QString &baseKey,
                     const PropagationCore::RasterRequest &request,
                     const MapGeoBounds &bounds,
                     const PropagationSettings &settings);
    void startRf(const QString &key, const QString &baseKey,
                 const PropagationCore::CoverageRequest &request);
    void finishRaster(const QString &key, quint64 generation,
                      const std::shared_ptr<RasterWorkerResult> &result,
                      QThread *thread);
    void finishRf(const QString &key, quint64 generation,
                  const std::shared_ptr<RfWorkerResult> &result,
                  QThread *thread);
    void cancelRaster();
    void cancelRf();
    void stopWorkers();
    void publishPolylines();
    void publishStatus();
    static PropagationCore::Parameters parameters(
        const PropagationSettings &settings);

    QPointer<AbstractMapWidget> m_map;
    QPointer<PropagationSettingsStore> m_settings;
    StateProvider m_stateProvider;
    TerrainProvider m_terrainProvider;
    QTimer m_timer;
    bool m_active = false;
    QPointer<QThread> m_rasterThread;
    QPointer<QThread> m_rfThread;
    std::shared_ptr<std::atomic_bool> m_rasterCancellation;
    std::shared_ptr<std::atomic_bool> m_rfCancellation;
    quint64 m_rasterGeneration = 0;
    quint64 m_rfGeneration = 0;
    QString m_runningRasterKey;
    QString m_runningRfKey;
    QString m_runningRasterBaseKey;
    QString m_runningRfBaseKey;
    QString m_appliedRasterKey;
    QString m_appliedRfKey;
    QElapsedTimer m_retryClock;
    bool m_rasterNeedsRetry = false;
    bool m_rfNeedsRetry = false;
    qint64 m_nextRasterRetryMs = 0;
    qint64 m_nextRfRetryMs = 0;
    QVector<MapOverlayPolyline> m_rfPolylines;
    QVector<MapOverlayPolyline> m_distancePolylines;
    QString m_rasterStatus;
    QString m_rfStatus;
    QString m_legend;
};

#endif // PROPAGATIONOVERLAYCONTROLLER_H
