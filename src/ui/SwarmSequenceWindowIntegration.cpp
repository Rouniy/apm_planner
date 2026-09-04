#include "SwarmSequenceWindowAdapter.h"

#include "comm/LinkManager.h"
#include "comm/SwarmTelemetryRegistry.h"

SwarmTelemetryRegistry *SwarmSequenceApplicationRegistry()
{
    LinkManager *manager = LinkManager::instance();
    return manager ? manager->swarmTelemetryRegistry() : nullptr;
}

SwarmSequenceWindowInterface *SwarmSequenceApplicationInterface(
    QObject *parent)
{
    LinkManager *manager = LinkManager::instance();
    return manager
        ? new SwarmSequenceWindowAdapter(
              manager->swarmSequenceExecutor(), parent)
        : nullptr;
}
