#include "StatusTextPolicy.h"

#include "uas/APMFirmwareVersion.h"

#include <mavlink.h>

bool StatusTextPolicy::isValidSeverity(int severity) noexcept
{
    return severity >= MinimumSeverity && severity <= MaximumSeverity;
}

bool StatusTextPolicy::isForcedHighMessage(const QString &text)
{
    return text.startsWith(QStringLiteral("Tuning:"))
        || text.startsWith(QStringLiteral("PreArm:"))
        || text.startsWith(QStringLiteral("Arm:"));
}

bool StatusTextPolicy::shouldPromote(const QString &text, int severity,
                                     int threshold)
{
    if (text.trimmed().isEmpty()) {
        return false;
    }
    const int boundedThreshold = isValidSeverity(threshold)
        ? threshold : DefaultSeverity;
    return (isValidSeverity(severity) && severity <= boundedThreshold)
        || isForcedHighMessage(text);
}

QString StatusTextPolicy::speechText(const QString &text, bool promoted)
{
    if (text.startsWith(QStringLiteral("PX4v2 "))) {
        return QString();
    }
    if (text.startsWith(QStringLiteral("#audio:"))) {
        return text.mid(QStringLiteral("#audio:").size()).trimmed();
    }
    if (text.startsWith(QStringLiteral("PreArm:"))) {
        return QStringLiteral("Pre-arm check:")
            + text.mid(QStringLiteral("PreArm:").size());
    }
    if (text.startsWith(QStringLiteral("Arm:"))) {
        return QStringLiteral("Arm check:")
            + text.mid(QStringLiteral("Arm:").size());
    }
    return promoted ? text.trimmed() : QString();
}

int StatusTextPolicy::normalizeLegacySeverity(int severity) noexcept
{
    switch (severity) {
    case 1:
        return MAV_SEVERITY_WARNING;
    case 2:
        return MAV_SEVERITY_ALERT;
    case 3:
    case 5:
        return MAV_SEVERITY_CRITICAL;
    default:
        return MAV_SEVERITY_INFO;
    }
}

bool StatusTextPolicy::requiresLegacySeverityCompatibility(
    const APMFirmwareVersion &version)
{
    if (!version.isValid()) {
        return false;
    }

    const QString type = version.vehicleType();
    if (type.startsWith(QStringLiteral("ArduCopter"))
        || type.startsWith(QStringLiteral("APM:Copter"))) {
        return version < APMFirmwareVersion(
            QStringLiteral("APM:Copter V3.4.0"));
    }
    if (type.startsWith(QStringLiteral("ArduPlane"))
        || type.startsWith(QStringLiteral("APM:Plane"))) {
        return version < APMFirmwareVersion(
            QStringLiteral("APM:Plane V3.4.0"));
    }
    if (type.startsWith(QStringLiteral("ArduRover"))
        || type.startsWith(QStringLiteral("APM:Rover"))) {
        return version < APMFirmwareVersion(
            QStringLiteral("APM:Rover V2.6.0"));
    }
    if (type.startsWith(QStringLiteral("ArduSub"))
        || type.startsWith(QStringLiteral("APM:Sub"))) {
        return version < APMFirmwareVersion(
            QStringLiteral("APM:Sub V3.4.0"));
    }
    return false;
}

QString StatusTextPolicy::firmwareVehicleType(int mavType)
{
    switch (mavType) {
    case MAV_TYPE_FIXED_WING:
    case MAV_TYPE_VTOL_DUOROTOR:
    case MAV_TYPE_VTOL_QUADROTOR:
    case MAV_TYPE_VTOL_TILTROTOR:
    case MAV_TYPE_VTOL_RESERVED2:
    case MAV_TYPE_VTOL_RESERVED3:
    case MAV_TYPE_VTOL_RESERVED4:
    case MAV_TYPE_VTOL_RESERVED5:
        return QStringLiteral("ArduPlane");
    case MAV_TYPE_GROUND_ROVER:
    case MAV_TYPE_SURFACE_BOAT:
        return QStringLiteral("ArduRover");
    case MAV_TYPE_SUBMARINE:
        return QStringLiteral("ArduSub");
    case MAV_TYPE_TRICOPTER:
    case MAV_TYPE_QUADROTOR:
    case MAV_TYPE_COAXIAL:
    case MAV_TYPE_HELICOPTER:
    case MAV_TYPE_HEXAROTOR:
    case MAV_TYPE_OCTOROTOR:
    case MAV_TYPE_DODECAROTOR:
    case MAV_TYPE_DECAROTOR:
        return QStringLiteral("ArduCopter");
    default:
        return QString();
    }
}
