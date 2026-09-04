#include "SwarmFollowLeaderWindow.h"

#include "comm/LinkManager.h"

SwarmTelemetryRegistry *SwarmFollowLeaderApplicationRegistry()
{
    LinkManager *manager = LinkManager::instance();
    return manager ? manager->swarmTelemetryRegistry() : nullptr;
}

SwarmCommandService *SwarmFollowLeaderApplicationCommandService()
{
    LinkManager *manager = LinkManager::instance();
    return manager ? manager->swarmCommandService() : nullptr;
}
