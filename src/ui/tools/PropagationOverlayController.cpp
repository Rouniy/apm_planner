#include "PropagationOverlayController.h"

#include <QImage>
#include <QThread>
#include <QStringList>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr double EarthRadiusMetres = 6378137.0;
constexpr double MercatorHalfWorld = Pi * EarthRadiusMetres;
constexpr double MaximumMercatorLatitude = 85.0511287798066;
constexpr int RasterPaddingPixels = 192;

double mercatorX(double longitude)
{
    return EarthRadiusMetres * qDegreesToRadians(longitude);
}

double mercatorY(double latitude)
{
    const double limited = std::clamp(
        latitude, -MaximumMercatorLatitude, MaximumMercatorLatitude);
    return EarthRadiusMetres * std::log(
        std::tan(Pi / 4.0 + qDegreesToRadians(limited) / 2.0));
}

bool renderableCoordinate(const PropagationCore::GeoPoint &point)
{
    return point.hasValidCoordinates()
        && (point.latitude != 0.0 || point.longitude != 0.0);
}

MapCoordinate mapCoordinate(const PropagationCore::GeoPoint &point)
{
    return {point.latitude, point.longitude, point.altitudeM};
}

MapOverlayPolyline polyline(
    const QVector<PropagationCore::GeoPoint> &points,
    const QColor &color, qreal width)
{
    MapOverlayPolyline line;
    line.color = color;
    line.width = width;
    line.dashed = true;
    line.points.reserve(points.size());
    for (const PropagationCore::GeoPoint &point : points) {
        line.points.append(mapCoordinate(point));
    }
    return line;
}

QString rasterBaseKey(const PropagationCore::RasterRequest &request,
                      const PropagationSettings &settings)
{
    return QString::number(request.extent.minimumX, 'g', 16)
        + QLatin1Char('|') + QString::number(request.extent.minimumY, 'g', 16)
        + QLatin1Char('|') + QString::number(request.extent.maximumX, 'g', 16)
        + QLatin1Char('|') + QString::number(request.extent.maximumY, 'g', 16)
        + QLatin1Char('|') + QString::number(request.width)
        + QLatin1Char('|') + QString::number(request.height)
        + QLatin1Char('|') + QString::number(settings.clearanceMeters, 'g', 12)
        + QLatin1Char('|') + QString::number(settings.resolutionPixels)
        + QLatin1Char('|') + QString::number(settings.minimumAltitude, 'g', 12)
        + QLatin1Char('|') + QString::number(settings.maximumAltitude, 'g', 12)
        + QLatin1Char('|') + QString::number(settings.elevationMap)
        + QLatin1Char('|') + QString::number(settings.terrainMap)
        + QLatin1Char('|') + QString::number(settings.manualAltitudeRange);
}

QString rasterKey(const QString &baseKey, double altitudeAmsl)
{
    return baseKey + QLatin1Char('|') + QString::number(
        PropagationOverlayController::quantizeAltitude(altitudeAmsl),
        'g', 12);
}

QString rfBaseKey(const PropagationCore::CoverageRequest &request)
{
    const PropagationCore::Parameters &value = request.parameters;
    return QString::number(request.home.latitude, 'g', 16)
        + QLatin1Char('|') + QString::number(request.home.longitude, 'g', 16)
        + QLatin1Char('|') + QString::number(request.home.altitudeM, 'g', 12)
        + QLatin1Char('|') + QString::number(value.clearanceMeters, 'g', 12)
        + QLatin1Char('|') + QString::number(value.azimuthStepDegrees, 'g', 12)
        + QLatin1Char('|') + QString::number(value.convergenceDegrees, 'g', 12)
        + QLatin1Char('|') + QString::number(value.rangeKilometers, 'g', 12)
        + QLatin1Char('|') + QString::number(value.baseHeightMeters, 'g', 12);
}

QString rfKey(const QString &baseKey, double altitudeAmsl)
{
    return baseKey + QLatin1Char('|') + QString::number(
        PropagationOverlayController::quantizeAltitude(altitudeAmsl),
        'g', 12);
}

QString telemetryEpochKey(const PropagationTelemetrySnapshot &telemetry)
{
    return QStringLiteral("|epoch=")
        + QString::number(telemetry.telemetryEpoch)
        + QLatin1Char('|') + QString::number(telemetry.lease.generation)
        + QLatin1Char('|')
        + QString::number(telemetry.lease.endpoint.linkId)
        + QLatin1Char('|')
        + QString::number(telemetry.lease.endpoint.systemId)
        + QLatin1Char('|')
        + QString::number(telemetry.lease.endpoint.componentId);
}
}

struct PropagationOverlayController::RasterWorkerResult
{
    PropagationCore::RasterGeometry raster;
    MapGeoBounds bounds;
    PropagationSettings settings;
    QString error;
};

struct PropagationOverlayController::RfWorkerResult
{
    PropagationCore::CoverageGeometry coverage;
    QString error;
};

PropagationOverlayController::PropagationOverlayController(
    AbstractMapWidget *map, StateProvider stateProvider,
    TerrainProvider terrainProvider, PropagationSettingsStore *settings,
    QObject *parent)
    : QObject(parent)
    , m_map(map)
    , m_settings(settings ? settings : PropagationSettingsStore::instance())
    , m_stateProvider(std::move(stateProvider))
    , m_terrainProvider(std::move(terrainProvider))
{
    m_retryClock.start();
    m_timer.setInterval(1000);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout,
            this, &PropagationOverlayController::refresh);
    if (m_settings) {
        connect(m_settings, &PropagationSettingsStore::settingsChanged,
                this, [this](const PropagationSettings &) {
            cancelRaster();
            cancelRf();
            m_appliedRasterKey.clear();
            m_appliedRfKey.clear();
            m_rasterNeedsRetry = false;
            m_rfNeedsRetry = false;
            refresh();
        });
    }
}

PropagationOverlayController::~PropagationOverlayController()
{
    m_active = false;
    m_timer.stop();
    stopWorkers();
    if (m_map) {
        m_map->ClearPropagationOverlay();
    }
}

double PropagationOverlayController::quantizeAltitude(double altitudeAmsl)
{
    return qIsFinite(altitudeAmsl)
        ? std::round(altitudeAmsl * 2.0) / 2.0 : 0.0;
}

void PropagationOverlayController::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (!active) {
        m_timer.stop();
        cancelRaster();
        cancelRf();
        return;
    }
    m_timer.start();
    refresh();
}

void PropagationOverlayController::refresh()
{
    if (!m_active || !m_map || !m_settings || !m_stateProvider) {
        return;
    }
    const PropagationMapState state = m_stateProvider();
    const PropagationSettings settings = m_settings->settings();
    refreshDistance(state, settings);
    refreshRaster(state, settings);
    refreshRf(state, settings);
    publishStatus();
}

void PropagationOverlayController::invalidateTerrain()
{
    cancelRaster();
    cancelRf();
    m_appliedRasterKey.clear();
    m_appliedRfKey.clear();
    m_rasterNeedsRetry = false;
    m_rfNeedsRetry = false;
    refresh();
}

void PropagationOverlayController::refreshDistance(
    const PropagationMapState &state, const PropagationSettings &settings)
{
    m_distancePolylines.clear();
    const double distance = state.telemetry.batteryKilometresLeft;
    if (!qIsFinite(distance) || distance <= 0.0) {
        publishPolylines();
        return;
    }
    const auto addRings = [this, distance, &settings](
            const PropagationCore::GeoPoint &center) {
        const PropagationCore::DistanceRingGeometry rings =
            PropagationCore::buildDistanceRings(
                center, distance, settings.tolerance);
        if (!rings.outer.isEmpty()) {
            m_distancePolylines.append(polyline(
                rings.outer, QColor(255, 0, 0), 1.0));
        }
        if (!rings.tolerance.isEmpty()) {
            m_distancePolylines.append(polyline(
                rings.tolerance, QColor(255, 165, 0), 1.0));
        }
    };
    if (settings.homeDistance && state.homeValid
        && renderableCoordinate(state.home)) {
        addRings(state.home);
    }
    if (settings.droneDistance && state.telemetry.positionValid) {
        addRings({state.telemetry.latitude, state.telemetry.longitude,
                  state.telemetry.altitudeAmsl});
    }
    publishPolylines();
}

void PropagationOverlayController::refreshRaster(
    const PropagationMapState &state, const PropagationSettings &settings)
{
    if (!settings.elevationMap && !settings.terrainMap) {
        cancelRaster();
        m_appliedRasterKey.clear();
        m_runningRasterKey.clear();
        m_runningRasterBaseKey.clear();
        m_rasterNeedsRetry = false;
        m_rasterStatus.clear();
        m_legend.clear();
        m_map->ClearPropagationRaster();
        return;
    }

    const MapGeoBounds visible = m_map->VisibleTileExtent();
    const QSize viewport = m_map->ViewportPixelSize();
    if (!visible.IsValid() || visible.CrossesDateLine()
        || viewport.width() < 2 || viewport.height() < 2) {
        cancelRaster();
        m_appliedRasterKey.clear();
        m_legend.clear();
        m_map->ClearPropagationRaster();
        m_rasterStatus = tr(
            "Propagation: current map extent cannot be rasterized");
        return;
    }
    const double visibleMinimumX = mercatorX(visible.left);
    const double visibleMaximumX = mercatorX(visible.right);
    const double visibleMinimumY = mercatorY(visible.bottom);
    const double visibleMaximumY = mercatorY(visible.top);
    const double horizontalResolution =
        (visibleMaximumX - visibleMinimumX) / viewport.width();
    const double verticalResolution =
        (visibleMaximumY - visibleMinimumY) / viewport.height();
    const PropagationCore::WebMercatorExtent paddedExtent{
        std::max(-MercatorHalfWorld,
                 visibleMinimumX
                    - RasterPaddingPixels * horizontalResolution),
        std::max(-MercatorHalfWorld,
                 visibleMinimumY
                    - RasterPaddingPixels * verticalResolution),
        std::min(MercatorHalfWorld,
                 visibleMaximumX
                    + RasterPaddingPixels * horizontalResolution),
        std::min(MercatorHalfWorld,
                 visibleMaximumY
                    + RasterPaddingPixels * verticalResolution)};
    const PropagationCore::GeoPoint paddedNorthWest =
        PropagationCore::webMercatorToGeoPoint(
            paddedExtent.minimumX, paddedExtent.maximumY);
    const PropagationCore::GeoPoint paddedSouthEast =
        PropagationCore::webMercatorToGeoPoint(
            paddedExtent.maximumX, paddedExtent.minimumY);
    MapGeoBounds padded{
        std::max(-180.0, paddedNorthWest.longitude),
        std::min(MaximumMercatorLatitude, paddedNorthWest.latitude),
        std::min(180.0, paddedSouthEast.longitude),
        std::max(-MaximumMercatorLatitude, paddedSouthEast.latitude)};
    PropagationCore::RasterRequest request;
    request.extent = paddedExtent;
    request.width = std::clamp(
        viewport.width() + 2 * RasterPaddingPixels, 1, 4096);
    request.height = std::clamp(
        viewport.height() + 2 * RasterPaddingPixels, 1, 4096);
    request.vehicleAltitudeAmsl =
        state.telemetry.positionValid
            ? state.telemetry.altitudeAmsl : 0.0;
    request.parameters = parameters(settings);
    const QString baseKey = rasterBaseKey(request, settings)
        + telemetryEpochKey(state.telemetry);
    const QString key = rasterKey(baseKey, request.vehicleAltitudeAmsl);
    const qint64 now = m_retryClock.elapsed();
    if ((key == m_appliedRasterKey
         && (!m_rasterNeedsRetry || now < m_nextRasterRetryMs))
        || key == m_runningRasterKey) {
        return;
    }
    if (m_rasterThread) {
        if (baseKey == m_runningRasterBaseKey) {
            return;
        }
        cancelRaster();
        return;
    }
    startRaster(key, baseKey, request, padded, settings);
}

void PropagationOverlayController::refreshRf(
    const PropagationMapState &state, const PropagationSettings &settings)
{
    if (!settings.rfMap || !state.homeValid
        || !renderableCoordinate(state.home)) {
        cancelRf();
        m_appliedRfKey.clear();
        m_runningRfKey.clear();
        m_runningRfBaseKey.clear();
        m_rfNeedsRetry = false;
        m_rfPolylines.clear();
        m_rfStatus = settings.rfMap
            ? tr("RF propagation: set a Home location first") : QString();
        publishPolylines();
        return;
    }
    PropagationCore::CoverageRequest request;
    request.home = state.home;
    request.droneAltitudeAmsl =
        state.telemetry.positionValid
            ? state.telemetry.altitudeAmsl : 0.0;
    request.parameters = parameters(settings);
    const QString baseKey = rfBaseKey(request)
        + telemetryEpochKey(state.telemetry);
    const QString key = rfKey(baseKey, request.droneAltitudeAmsl);
    const qint64 now = m_retryClock.elapsed();
    if ((key == m_appliedRfKey
         && (!m_rfNeedsRetry || now < m_nextRfRetryMs))
        || key == m_runningRfKey) {
        return;
    }
    if (m_rfThread) {
        if (baseKey == m_runningRfBaseKey) {
            return;
        }
        cancelRf();
        return;
    }
    startRf(key, baseKey, request);
}

void PropagationOverlayController::startRaster(
    const QString &key, const QString &baseKey,
    const PropagationCore::RasterRequest &request,
    const MapGeoBounds &bounds, const PropagationSettings &settings)
{
    const auto result = std::make_shared<RasterWorkerResult>();
    result->bounds = bounds;
    result->settings = settings;
    m_rasterCancellation = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = m_rasterCancellation;
    const TerrainProvider terrain = m_terrainProvider;
    const quint64 generation = ++m_rasterGeneration;
    m_runningRasterKey = key;
    m_runningRasterBaseKey = baseKey;
    m_rasterNeedsRetry = false;
    QThread *thread = QThread::create(
        [result, request, terrain, cancellation]() {
            try {
                result->raster = PropagationCore::buildRaster(
                    request, terrain,
                    [cancellation]() { return cancellation->load(); });
            } catch (const std::exception &exception) {
                result->error = QString::fromUtf8(exception.what());
            } catch (...) {
                result->error = QStringLiteral("unknown worker error");
            }
        });
    m_rasterThread = thread;
    connect(thread, &QThread::finished, this,
            [this, key, generation, result, thread]() {
        finishRaster(key, generation, result, thread);
    });
    thread->start();
}

void PropagationOverlayController::startRf(
    const QString &key, const QString &baseKey,
    const PropagationCore::CoverageRequest &request)
{
    const auto result = std::make_shared<RfWorkerResult>();
    m_rfCancellation = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = m_rfCancellation;
    const TerrainProvider terrain = m_terrainProvider;
    const quint64 generation = ++m_rfGeneration;
    m_runningRfKey = key;
    m_runningRfBaseKey = baseKey;
    m_rfNeedsRetry = false;
    QThread *thread = QThread::create(
        [result, request, terrain, cancellation]() {
            try {
                result->coverage = PropagationCore::buildRfCoverage(
                    request, terrain,
                    [cancellation]() { return cancellation->load(); });
            } catch (const std::exception &exception) {
                result->error = QString::fromUtf8(exception.what());
            } catch (...) {
                result->error = QStringLiteral("unknown worker error");
            }
        });
    m_rfThread = thread;
    connect(thread, &QThread::finished, this,
            [this, key, generation, result, thread]() {
        finishRf(key, generation, result, thread);
    });
    thread->start();
}

void PropagationOverlayController::finishRaster(
    const QString &key, quint64 generation,
    const std::shared_ptr<RasterWorkerResult> &result, QThread *thread)
{
    if (m_rasterThread == thread) {
        m_rasterThread = nullptr;
        m_runningRasterKey.clear();
        m_runningRasterBaseKey.clear();
        m_rasterCancellation.reset();
    }
    thread->deleteLater();
    if (!m_active || !m_map || generation != m_rasterGeneration
        || result->raster.status == PropagationCore::BuildStatus::Cancelled) {
        QTimer::singleShot(0, this, &PropagationOverlayController::refresh);
        return;
    }
    if (!result->error.isEmpty()) {
        m_appliedRasterKey = key;
        m_rasterNeedsRetry = true;
        m_nextRasterRetryMs = m_retryClock.elapsed() + 2000;
        m_map->ClearPropagationRaster();
        m_legend.clear();
        m_rasterStatus = tr("Terrain overlay failed: %1")
            .arg(result->error);
        publishStatus();
        return;
    }
    if (result->raster.status != PropagationCore::BuildStatus::Finished) {
        m_appliedRasterKey = key;
        m_rasterNeedsRetry = true;
        m_nextRasterRetryMs = m_retryClock.elapsed() + 2000;
        m_map->ClearPropagationRaster();
        m_legend.clear();
        m_rasterStatus = tr("Terrain overlay failed: invalid input");
        publishStatus();
        return;
    }
    QImage image;
    if (result->raster.validSampleCount > 0) {
        image = QImage(
            result->raster.rgba.constData(), result->raster.width,
            result->raster.height, result->raster.width * 4,
            QImage::Format_RGBA8888).copy();
    }
    m_map->SetPropagationRaster(image, result->bounds);
    m_appliedRasterKey = key;
    m_rasterNeedsRetry = result->raster.missingSampleCount > 0;
    m_nextRasterRetryMs = m_retryClock.elapsed() + 5000;
    m_rasterStatus = result->raster.validSampleCount == 0
        ? tr("Propagation: no elevation data for this map area")
        : result->raster.missingSampleCount > 0
            ? tr("Propagation: partial elevation data (%1 samples)")
                  .arg(result->raster.validSampleCount)
            : QString();
    m_legend = result->settings.showScale
            && result->raster.validSampleCount > 0
        ? result->settings.elevationMap
            ? tr("Rel to Terrain: 0 m → %1 m")
                  .arg(result->settings.clearanceMeters, 0, 'f', 1)
            : tr("Elevation (AMSL): %1 m → %2 m")
                  .arg(result->raster.maximumAltitudeM, 0, 'f', 0)
                  .arg(result->raster.minimumAltitudeM, 0, 'f', 0)
        : QString();
    publishStatus();
}

void PropagationOverlayController::finishRf(
    const QString &key, quint64 generation,
    const std::shared_ptr<RfWorkerResult> &result, QThread *thread)
{
    if (m_rfThread == thread) {
        m_rfThread = nullptr;
        m_runningRfKey.clear();
        m_runningRfBaseKey.clear();
        m_rfCancellation.reset();
    }
    thread->deleteLater();
    if (!m_active || !m_map || generation != m_rfGeneration
        || result->coverage.status == PropagationCore::BuildStatus::Cancelled) {
        QTimer::singleShot(0, this, &PropagationOverlayController::refresh);
        return;
    }
    if (!result->error.isEmpty()) {
        m_appliedRfKey = key;
        m_rfNeedsRetry = true;
        m_nextRfRetryMs = m_retryClock.elapsed() + 2000;
        m_rfPolylines.clear();
        m_rfStatus = tr("RF propagation failed: %1").arg(result->error);
        publishPolylines();
        publishStatus();
        return;
    }
    m_rfPolylines.clear();
    if (result->coverage.complete()) {
        m_rfPolylines.append(polyline(
            result->coverage.points, Qt::white, 3.0));
    }
    m_appliedRfKey = key;
    m_rfNeedsRetry = result->coverage.missingSampleCount > 0;
    m_nextRfRetryMs = m_retryClock.elapsed() + 5000;
    m_rfStatus = result->coverage.missingSampleCount > 0
        ? tr("RF propagation: elevation data is still unavailable")
        : result->coverage.complete()
            ? QString() : tr("RF propagation: no coverage polygon");
    publishPolylines();
    publishStatus();
}

void PropagationOverlayController::cancelRaster()
{
    if (m_rasterCancellation) {
        m_rasterCancellation->store(true);
    }
    ++m_rasterGeneration;
}

void PropagationOverlayController::cancelRf()
{
    if (m_rfCancellation) {
        m_rfCancellation->store(true);
    }
    ++m_rfGeneration;
}

void PropagationOverlayController::stopWorkers()
{
    cancelRaster();
    cancelRf();
    QThread *raster = m_rasterThread.data();
    QThread *rf = m_rfThread.data();
    if (raster) {
        raster->wait();
        delete raster;
        m_rasterThread = nullptr;
    }
    if (rf) {
        rf->wait();
        delete rf;
        m_rfThread = nullptr;
    }
}

void PropagationOverlayController::publishPolylines()
{
    if (!m_map) {
        return;
    }
    if (m_rfPolylines.isEmpty()) {
        m_map->ClearPropagationContour();
    } else {
        m_map->SetPropagationContour(m_rfPolylines.first());
    }
    if (m_distancePolylines.isEmpty()) {
        m_map->ClearPropagationRings();
    } else {
        m_map->SetPropagationRings(m_distancePolylines);
    }
}

void PropagationOverlayController::publishStatus()
{
    if (!m_map) {
        return;
    }
    QStringList messages;
    if (!m_rasterStatus.isEmpty()) {
        messages.append(m_rasterStatus);
    }
    if (!m_rfStatus.isEmpty()) {
        messages.append(m_rfStatus);
    }
    m_map->SetPropagationStatus(
        m_legend, messages.join(QStringLiteral(" · ")));
}

PropagationCore::Parameters PropagationOverlayController::parameters(
    const PropagationSettings &settings)
{
    PropagationCore::Parameters result;
    result.clearanceMeters = settings.clearanceMeters;
    result.resolutionPixels = settings.resolutionPixels;
    result.azimuthStepDegrees = settings.azimuthStepDegrees;
    result.convergenceDegrees = settings.convergenceDegrees;
    result.rangeKilometers = settings.rangeKilometers;
    result.baseHeightMeters = settings.baseHeightMeters;
    result.minimumAltitudeM = settings.minimumAltitude;
    result.maximumAltitudeM = settings.maximumAltitude;
    result.elevationMap = settings.elevationMap;
    result.manualAltitudeRange = settings.manualAltitudeRange;
    return result;
}
