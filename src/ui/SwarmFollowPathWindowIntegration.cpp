#include "comm/LinkManager.h"
#include "comm/SwarmCommandService.h"
#include "comm/SwarmTelemetryRegistry.h"

SwarmTelemetryRegistry *SwarmFollowPathApplicationRegistry()
{
    LinkManager *manager = LinkManager::instance();
    return manager ? manager->swarmTelemetryRegistry() : nullptr;
}

SwarmCommandService *SwarmFollowPathApplicationCommandService()
{
    LinkManager *manager = LinkManager::instance();
    return manager ? manager->swarmCommandService() : nullptr;
}
