#ifndef TELEMETRYLOGANONYMIZER_H
#define TELEMETRYLOGANONYMIZER_H

#include "LogAnonymizer.h"

class QIODevice;

/**
 * Streaming backends for Mission Planner text DataFlash and telemetry logs.
 * Tlog privacy filtering drops opaque GPS correction, FILE_TRANSFER_PROTOCOL,
 * LOG_DATA and SETUP_SIGNING frames
 * from all senders, including with zero coordinate offsets. Per-message drop
 * counts and reasons are returned in warnings; records counts retained output
 * records. This limited transform/filter is not a complete anonymity guarantee.
 * COMMAND_LONG is retained only for explicitly audited non-coordinate commands.
 * COMMAND_INT uses known global mission-command x/y semantics; other commands
 * are dropped unless explicitly known to be non-coordinate. Existing mission
 * item local-frame and non-location-command behavior is unchanged.
 */
class TelemetryLogAnonymizer final
{
public:
    static LogAnonymizeResult anonymizeText(
        QIODevice *input, QIODevice *output,
        const LogAnonymizeOptions &options,
        const LogAnonymizeCancel &cancel = {},
        const LogAnonymizeProgress &progress = {});

    static LogAnonymizeResult anonymizeTlog(
        QIODevice *input, QIODevice *output,
        const LogAnonymizeOptions &options,
        const LogAnonymizeCancel &cancel = {},
        const LogAnonymizeProgress &progress = {});
};

#endif // TELEMETRYLOGANONYMIZER_H
