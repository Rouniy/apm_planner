#include "PlannerStartupUdpOptions.h"

#include <QSettings>
#include <QVariant>

namespace {
bool normalizedEnabled(const QVariant &value, bool fallback)
{
    if (!value.isValid() || value.isNull()) {
        return fallback;
    }

    const QString text = value.toString().trimmed();
    if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        || text == QStringLiteral("1")) {
        return true;
    }
    if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0
        || text == QStringLiteral("0")) {
        return false;
    }
    return fallback;
}

int persistedPort(QSettings &settings, const char *key, int fallback)
{
    bool valid = false;
    const int value = settings.value(QLatin1String(key), fallback).toInt(&valid);
    return valid ? PlannerStartupUdpOptions::normalizePort(value, fallback)
                 : fallback;
}
}

PlannerStartupUdpOptions PlannerStartupUdpOptions::load(QSettings &settings)
{
    settings.setFallbacksEnabled(false);
    PlannerStartupUdpOptions result;
    result.enabled = normalizedEnabled(
        settings.value(QLatin1String(EnabledSettingKey)), DefaultEnabled);
    result.primaryPort = persistedPort(
        settings, PrimaryPortSettingKey, DefaultPrimaryPort);
    result.alternatePort = persistedPort(
        settings, AlternatePortSettingKey, DefaultAlternatePort);
    return result;
}

bool PlannerStartupUdpOptions::hasExplicitConfiguration(QSettings &settings)
{
    settings.setFallbacksEnabled(false);
    return settings.contains(QLatin1String(EnabledSettingKey))
        || settings.contains(QLatin1String(PrimaryPortSettingKey))
        || settings.contains(QLatin1String(AlternatePortSettingKey));
}

void PlannerStartupUdpOptions::save(QSettings &settings) const
{
    const PlannerStartupUdpOptions value = normalized();
    settings.setValue(QLatin1String(EnabledSettingKey), value.enabled);
    settings.setValue(QLatin1String(PrimaryPortSettingKey), value.primaryPort);
    settings.setValue(QLatin1String(AlternatePortSettingKey),
                      value.alternatePort);
}

int PlannerStartupUdpOptions::normalizePort(int value, int fallback)
{
    return value >= 1 && value <= 65535 ? value : fallback;
}

PlannerStartupUdpOptions PlannerStartupUdpOptions::normalized() const
{
    PlannerStartupUdpOptions result = *this;
    result.primaryPort = normalizePort(primaryPort, DefaultPrimaryPort);
    result.alternatePort = normalizePort(alternatePort, DefaultAlternatePort);
    return result;
}

QList<int> PlannerStartupUdpOptions::orderedPorts() const
{
    if (!enabled) {
        return {};
    }

    const PlannerStartupUdpOptions value = normalized();
    QList<int> result{value.primaryPort};
    if (value.alternatePort != value.primaryPort) {
        result.append(value.alternatePort);
    }
    return result;
}

QString PlannerStartupUdpOptions::configurationStatus() const
{
    const QList<int> ports = orderedPorts();
    if (ports.isEmpty()) {
        return QStringLiteral("Automatic startup UDP listeners are disabled.");
    }
    if (ports.size() == 1) {
        return QStringLiteral(
            "Startup UDP listener is configured for port %1. "
            "Changes take effect after restart.")
            .arg(ports.first());
    }
    return QStringLiteral(
        "Startup UDP listeners are configured for ports %1 and %2. "
        "Changes take effect after restart.")
        .arg(ports.at(0))
        .arg(ports.at(1));
}

QString PlannerStartupUdpOptions::restartNote()
{
    return QStringLiteral(
        "Each port is a separate MAVLink connection. Changes take effect "
        "after restart; duplicate port values open one listener.");
}
