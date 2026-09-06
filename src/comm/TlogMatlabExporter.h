#pragma once

#include "TlogExportService.h"

// Worker-safe, bounded-memory MP10 telemetry MATLAB export. All recorded
// senders share one scalar series per field/message, as in MatLab.tlog().
class TlogMatlabExporter
{
public:
    using Cancel = std::function<bool()>;
    using Progress = std::function<void(qint64, qint64)>;
    static TlogExportResult Export(const QString &input, const QString &output,
                                   Cancel cancel = {}, Progress progress = {});

    // MP10 intentionally uses LOCAL wall time, whole milliseconds, and
    // DateTime.AddYears(1).AddDays(2), including its leap-year quirk.
    // Out-of-range unsigned prefixes reproduce DateTime.MinValue -> 367.
    static double mp10SerialDate(qint64 unixUsec);
};
