#ifndef STATUSTEXTPOLICY_H
#define STATUSTEXTPOLICY_H

#include <QString>

class APMFirmwareVersion;

/** Shared Mission Planner STATUSTEXT classification and compatibility rules. */
class StatusTextPolicy final
{
public:
    static constexpr int DefaultSeverity = 4;
    static constexpr int MinimumSeverity = 0;
    static constexpr int MaximumSeverity = 7;

    static bool isValidSeverity(int severity) noexcept;
    static bool isForcedHighMessage(const QString &text);
    static bool shouldPromote(const QString &text, int severity,
                              int threshold);
    static QString speechText(const QString &text, bool promoted);

    static int normalizeLegacySeverity(int severity) noexcept;
    static bool requiresLegacySeverityCompatibility(
        const APMFirmwareVersion &version);
    static QString firmwareVehicleType(int mavType);
};

#endif // STATUSTEXTPOLICY_H
