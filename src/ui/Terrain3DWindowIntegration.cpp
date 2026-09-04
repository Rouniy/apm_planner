#include "Terrain3DWindow.h"

#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "services/SpeechTelemetrySource.h"
#include "terrain/Terrain3DTelemetrySource.h"
#include "configuration/ElevationSourceService.h"

#include <QApplication>
#include <QPointer>

#include <limits>
#include <utility>

namespace
{
Terrain3DCore::Snapshot coreSnapshot(
    const Terrain3DTelemetrySnapshot &telemetry)
{
    Terrain3DCore::Snapshot snapshot;
    if (!telemetry.isValid()) {
        return snapshot;
    }
    snapshot.linkId = telemetry.lease.endpoint.linkId;
    snapshot.targetGeneration = telemetry.lease.generation;
    snapshot.systemId = quint8(telemetry.lease.endpoint.systemId);
    snapshot.componentId = quint8(telemetry.lease.endpoint.componentId);
    snapshot.mode = telemetry.mode;
    snapshot.armed = telemetry.armed;
    if (!telemetry.positionValid) {
        return snapshot;
    }
    snapshot.vehicle = {telemetry.latitude, telemetry.longitude,
                        telemetry.altitudeAmslM};
    snapshot.relativeAltitudeM = telemetry.altitudeRelativeM;
    snapshot.velocityNorthMps = telemetry.velocityNorthMps;
    snapshot.velocityEastMps = telemetry.velocityEastMps;
    snapshot.velocityVerticalMps = telemetry.velocityUpMps;
    snapshot.capturedMonotonicMs = telemetry.positionObservedMs;
    if (telemetry.attitudeValid) {
        snapshot.rollDeg = telemetry.rollDeg;
        snapshot.pitchDeg = telemetry.pitchDeg;
        snapshot.yawDeg = telemetry.yawDeg;
    }
    return snapshot;
}
}

Terrain3DWindow *Terrain3DWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    LinkManager *manager = LinkManager::instance();
    VehicleTargetManager *targets = manager
        ? manager->vehicleTargetManager() : nullptr;
    auto *source = new Terrain3DTelemetrySource(
        targets, {},
        [](int autopilot, int vehicleType, quint32 customMode,
           quint8 baseMode) {
            return SpeechTelemetrySource::modeText(
                autopilot, vehicleType, customMode, baseMode);
        });
    const QPointer<Terrain3DTelemetrySource> guardedSource(source);

    Dependencies dependencies;
    dependencies.snapshot = [guardedSource]() {
        return guardedSource
            ? coreSnapshot(guardedSource->snapshot())
            : Terrain3DCore::Snapshot{};
    };
    dependencies.monotonicClock = [guardedSource]() {
        return guardedSource ? guardedSource->monotonicTimeMs() : qint64(0);
    };
    ElevationSourceService *elevation = ElevationSourceService::instance();
    dependencies.elevation = [elevation](double latitude, double longitude) {
        double altitude = std::numeric_limits<double>::quiet_NaN();
        return elevation
                && elevation->sampleAltitude(latitude, longitude, &altitude)
            ? altitude : std::numeric_limits<double>::quiet_NaN();
    };

    auto *window = new Terrain3DWindow(std::move(dependencies), resolvedOwner);
    source->setParent(window);
    if (manager) {
        connect(manager, &LinkManager::messageReceived, source,
                [guardedSource](LinkInterface *link,
                                const mavlink_message_t &message) {
            if (guardedSource && link) {
                guardedSource->observeMessage(link->getId(), message);
            }
        });
    }
    if (elevation) {
        connect(elevation, &ElevationSourceService::srtmTileAvailable,
                window, [window](const QString &) {
            window->ReloadTerrain();
        });
        connect(elevation, &ElevationSourceService::nativeRastersChanged,
                window, &Terrain3DWindow::ReloadTerrain);
    }
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}
