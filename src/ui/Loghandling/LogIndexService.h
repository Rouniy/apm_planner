#ifndef LOGINDEXSERVICE_H
#define LOGINDEXSERVICE_H

#include "LogIndexTypes.h"

// Worker-only value API: no UI, live telemetry, QObject or vehicle mutation.
class LogIndexService final
{
public:
    static LogIndex::Analysis analyzeFile(const LogIndex::Entry &source,
                                         const LogIndex::Cancel &cancel = {});
    static LogIndex::ScanResult scan(
        const QString &root, const LogIndex::Cancel &cancel = {},
        const LogIndex::Progress &progress = {},
        const LogIndex::TileReader &tiles = {});
};

#endif
