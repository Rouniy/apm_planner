#include "comm/LinkManager.h"
#include "comm/SwarmCommandService.h"
#include "comm/SwarmTelemetryRegistry.h"

SwarmTelemetryRegistry *SwarmFormationApplicationRegistry()
{
    LinkManager *manager = LinkManager::instance();
    return manager ? manager->swarmTelemetryRegistry() : nullptr;
}

SwarmCommandService *SwarmFormationApplicationCommandService()
{
    LinkManager *manager = LinkManager::instance();
    return manager ? manager->swarmCommandService() : nullptr;
}
