#include "VehicleEndpoint.h"

namespace
{

QString indexedName(const QString &base, int index)
{
    return index <= 1 ? base : base + QString::number(index);
}

} // namespace

bool VehicleEndpoint::isValid() const noexcept
{
    return linkId >= 0 && systemId > 0 && systemId <= 255
        && componentId >= 0 && componentId <= 255;
}

bool VehicleEndpoint::sameIdentity(const VehicleEndpoint &other) const noexcept
{
    return linkId == other.linkId && systemId == other.systemId
        && componentId == other.componentId;
}

bool VehicleEndpoint::metadataEquals(const VehicleEndpoint &other) const noexcept
{
    return sameIdentity(other) && linkName == other.linkName
        && componentName == other.componentName;
}

QString VehicleEndpoint::displayName() const
{
    const QString resolvedLink = linkName.trimmed().isEmpty()
        ? QStringLiteral("Link %1").arg(linkId)
        : linkName.trimmed();
    QString resolvedComponent = componentName.trimmed();
    if (resolvedComponent.isEmpty()) {
        resolvedComponent = defaultComponentName(componentId);
    }
    resolvedComponent.replace(QLatin1Char('_'), QLatin1Char(' '));
    return QStringLiteral("%1-%2-%3")
        .arg(resolvedLink).arg(systemId).arg(resolvedComponent);
}

QVariantMap VehicleEndpoint::toVariantMap() const
{
    return {{QStringLiteral("valid"), isValid()},
            {QStringLiteral("linkId"), linkId},
            {QStringLiteral("systemId"), systemId},
            {QStringLiteral("componentId"), componentId},
            {QStringLiteral("linkName"), linkName},
            {QStringLiteral("componentName"),
             componentName.trimmed().isEmpty()
                 ? defaultComponentName(componentId)
                 : componentName.trimmed()},
            {QStringLiteral("displayName"), displayName()}};
}

QString VehicleEndpoint::defaultComponentName(int componentId)
{
    if (componentId >= 100 && componentId <= 105) {
        return indexedName(QStringLiteral("CAMERA"), componentId - 99);
    }
    if (componentId >= 140 && componentId <= 153) {
        return indexedName(QStringLiteral("SERVO"), componentId - 139);
    }
    if (componentId >= 171 && componentId <= 175) {
        return indexedName(QStringLiteral("GIMBAL"), componentId - 169);
    }
    if (componentId >= 180 && componentId <= 181) {
        return indexedName(QStringLiteral("BATTERY"), componentId - 179);
    }
    if (componentId >= 191 && componentId <= 194) {
        return indexedName(QStringLiteral("ONBOARD COMPUTER"),
                           componentId - 190);
    }
    if (componentId >= 200 && componentId <= 202) {
        return indexedName(QStringLiteral("IMU"), componentId - 199);
    }
    if (componentId >= 220 && componentId <= 221) {
        return indexedName(QStringLiteral("GPS"), componentId - 219);
    }

    switch (componentId) {
    case 1: return QStringLiteral("AUTOPILOT1");
    case 68: return QStringLiteral("TELEMETRY RADIO");
    case 154: return QStringLiteral("GIMBAL");
    case 155: return QStringLiteral("LOG");
    case 156: return QStringLiteral("ADSB");
    case 157: return QStringLiteral("OSD");
    case 158: return QStringLiteral("PERIPHERAL");
    case 159: return QStringLiteral("QX1 GIMBAL");
    case 160: return QStringLiteral("FLARM");
    case 161: return QStringLiteral("PARACHUTE");
    case 189: return QStringLiteral("MAVCAN");
    case 190: return QStringLiteral("MISSIONPLANNER");
    case 195: return QStringLiteral("PATHPLANNER");
    case 196: return QStringLiteral("OBSTACLE AVOIDANCE");
    case 197: return QStringLiteral("VISUAL INERTIAL ODOMETRY");
    case 198: return QStringLiteral("PAIRING MANAGER");
    case 236: return QStringLiteral("ODID TXRX 1");
    case 237: return QStringLiteral("ODID TXRX 2");
    case 238: return QStringLiteral("ODID TXRX 3");
    case 240: return QStringLiteral("UDP BRIDGE");
    case 241: return QStringLiteral("UART BRIDGE");
    case 242: return QStringLiteral("TUNNEL NODE");
    case 250: return QStringLiteral("SYSTEM CONTROL");
    default: return QString::number(componentId);
    }
}
