#include "Terrain3DWindow.h"
#include "GuidedNavigationController.h"

#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "services/SpeechTelemetrySource.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "configuration/ElevationSourceService.h"

#include <QApplication>
#include <QPointer>

#include <limits>
#include <utility>

namespace
{
Terrain3DCore::Snapshot coreSnapshot(const VehicleTargetLease &target,
    const SwarmTelemetrySnapshot &telemetry)
{
    Terrain3DCore::Snapshot snapshot;
    if (!target.isValid() || !telemetry.lease.isValid()) {
        return snapshot;
    }
    snapshot.linkId = telemetry.lease.endpoint.linkId;
    snapshot.targetGeneration = target.generation;
    snapshot.linkSessionEpoch = telemetry.lease.linkSessionEpoch;
    snapshot.vehicleInstanceEpoch = telemetry.lease.instanceEpoch;
    snapshot.systemId = quint8(telemetry.lease.endpoint.systemId);
    snapshot.componentId = quint8(telemetry.lease.endpoint.componentId);
    snapshot.mode = SpeechTelemetrySource::modeText(telemetry.autopilot,
        telemetry.vehicleType, telemetry.customMode, telemetry.baseMode);
    snapshot.armed = telemetry.armed;
    if (!telemetry.positionValid) {
        return snapshot;
    }
    snapshot.vehicle = {telemetry.latitudeDegrees, telemetry.longitudeDegrees,
                        telemetry.altitudeAmslM};
    snapshot.relativeAltitudeM = telemetry.relativeAltitudeM;
    snapshot.velocityNorthMps = telemetry.velocityNorthMps;
    snapshot.velocityEastMps = telemetry.velocityEastMps;
    snapshot.velocityVerticalMps = -telemetry.velocityDownMps;
    snapshot.capturedMonotonicMs = telemetry.positionObservedMs;
    if (telemetry.attitudeValid) {
        constexpr double radiansToDegrees = 57.2957795130823208768;
        snapshot.rollDeg = telemetry.rollRadians * radiansToDegrees;
        snapshot.pitchDeg = telemetry.pitchRadians * radiansToDegrees;
        snapshot.yawDeg = telemetry.yawRadians * radiansToDegrees;
    }
    return snapshot;
}
}

Terrain3DWindow *Terrain3DWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    LinkManager *manager = LinkManager::instance();
    const QPointer<VehicleTargetManager> targets(manager
        ? manager->vehicleTargetManager() : nullptr);
    const QPointer<SwarmTelemetryRegistry> registry(manager
        ? manager->swarmTelemetryRegistry() : nullptr);
    // The callback cell is filled before show(); the window owns the UI
    // controller, while LinkManager owns command/altitude state across windows.
    auto controller = std::make_shared<QPointer<GuidedNavigationController>>();

    Dependencies dependencies;
    dependencies.snapshot = [targets, registry]() {
        if (!targets || !registry) return Terrain3DCore::Snapshot{};
        const VehicleTargetLease target = targets->acquireTarget();
        SwarmTelemetrySnapshot telemetry;
        if (!target.isValid() || !registry->acquireSnapshot(target.endpoint, &telemetry, 3000)
            || !targets || !registry || !targets->isTargetGenerationSettled()
            || !targets->isCurrentTarget(target.endpoint.linkId, target.endpoint.systemId,
                target.endpoint.componentId, target.generation)
            || !registry->validateLease(telemetry.lease, 3000))
            return Terrain3DCore::Snapshot{};
        // Never combine an old position cache with a new physical heartbeat.
        if (!registry->observationIsFresh(telemetry.positionObservedMs, 3000))
            telemetry.positionValid = false;
        if (!registry->observationIsFresh(telemetry.attitudeObservedMs, 3000))
            telemetry.attitudeValid = false;
        return coreSnapshot(target, telemetry);
    };
    dependencies.monotonicClock = [registry]() {
        return registry ? registry->observationClockNowMs() : qint64(0);
    };
    dependencies.guidedTargetRequested = [controller](const Terrain3DCore::GeoPoint &point,
        const Terrain3DCore::Snapshot &rendered) {
        if (*controller) (*controller)->terrainClick(point, rendered);
    };
    dependencies.guidedAltitudeEditRequested = [controller]() {
        if (*controller) (*controller)->editAltitude();
    };
    ElevationSourceService *elevation = ElevationSourceService::instance();
    dependencies.elevation = [elevation](double latitude, double longitude) {
        double altitude = std::numeric_limits<double>::quiet_NaN();
        return elevation
                && elevation->sampleAltitude(latitude, longitude, &altitude)
            ? altitude : std::numeric_limits<double>::quiet_NaN();
    };

    auto *window = new Terrain3DWindow(std::move(dependencies), resolvedOwner);
    GuidedNavigationController::Dependencies guided;
    guided.altitudes = manager ? manager->guidedAltitudeStore() : nullptr;
    guided.navigation = manager ? manager->guidedNavigationService() : nullptr;
    guided.telemetry = registry;
    guided.isGuided = [registry](const GuidedAltitudeStore::Context &context) {
        SwarmTelemetrySnapshot snapshot;
        return registry && registry->snapshotForLease(context.vehicle, &snapshot)
            && SpeechTelemetrySource::modeText(snapshot.autopilot, snapshot.vehicleType,
                snapshot.customMode, snapshot.baseMode).compare(QStringLiteral("Guided"),
                    Qt::CaseInsensitive) == 0;
    };
    *controller = new GuidedNavigationController(guided, window);
    connect(*controller, &GuidedNavigationController::statusChanged,
            window, &Terrain3DWindow::setGuidedStatus);
    if (!(*controller)->statusText().isEmpty())
        window->setGuidedStatus((*controller)->statusText());
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
