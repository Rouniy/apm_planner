#ifndef LOGINDEXTILECACHE_H
#define LOGINDEXTILECACHE_H

#include "LogIndexTypes.h"
#include "maptype.h"

class LogIndexTileCache final
{
public:
    // Reads only the canonical shared layout; never downloads, creates tiles,
    // changes map access mode or touches the cache's access-time metadata.
    static LogIndex::TileReader reader(const QString &cacheRoot,
                                      core::MapType::Types mapType);
};

#endif
