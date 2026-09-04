#include "SwarmWaypointLeaderWindowAdapter.h"

#include "comm/LinkManager.h"

SwarmWaypointLeaderWindowInterface *
SwarmWaypointLeaderApplicationInterface(QObject *parent)
{
    LinkManager *const manager = LinkManager::instance();
    if (!manager) {
        return nullptr;
    }
    return new SwarmWaypointLeaderWindowAdapter(
        manager->swarmTelemetryRegistry(),
        manager->exactMissionSnapshotService(),
        manager->swarmWaypointLeaderExecutor(),
        parent);
}
