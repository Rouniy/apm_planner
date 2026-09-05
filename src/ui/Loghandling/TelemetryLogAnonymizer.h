#ifndef TELEMETRYLOGANONYMIZER_H
#define TELEMETRYLOGANONYMIZER_H

#include "LogAnonymizer.h"

class QIODevice;

/** Streaming backends for Mission Planner text DataFlash and telemetry logs. */
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
