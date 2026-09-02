#ifndef COMPILEDMAPBACKENDS_H
#define COMPILEDMAPBACKENDS_H

// Register every backend enabled by this build before the factory creates its
// first map. This keeps persisted backend selection deterministic.
void RegisterCompiledMapBackends();

#endif // COMPILEDMAPBACKENDS_H
