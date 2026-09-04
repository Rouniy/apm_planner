#include "comm/LinkManager.h"
#include "comm/SwarmTelemetryRegistry.h"

SwarmTelemetryRegistry *SwarmSequenceApplicationRegistry()
{
    LinkManager *manager = LinkManager::instance();
    return manager ? manager->swarmTelemetryRegistry() : nullptr;
}
