#ifndef PLANNERSTARTUPUDPOPTIONS_H
#define PLANNERSTARTUPUDPOPTIONS_H

#include <QList>
#include <QString>

class QSettings;

/**
 * Persistent Mission Planner 10 startup UDP-listener policy.
 *
 * This value type deliberately owns no sockets.  It normalizes persisted
 * values and produces the ordered, duplicate-free port list which the
 * connection layer can consume during application startup.
 */
struct PlannerStartupUdpOptions
{
    static constexpr const char *EnabledSettingKey =
        "startup_udp_listeners_enabled";
    static constexpr const char *PrimaryPortSettingKey =
        "startup_udp_primary_port";
    static constexpr const char *AlternatePortSettingKey =
        "startup_udp_alternate_port";

    static constexpr bool DefaultEnabled = true;
    static constexpr int DefaultPrimaryPort = 14550;
    static constexpr int DefaultAlternatePort = 14551;

    bool enabled = DefaultEnabled;
    int primaryPort = DefaultPrimaryPort;
    int alternatePort = DefaultAlternatePort;

    static PlannerStartupUdpOptions load(QSettings &settings);
    static bool hasExplicitConfiguration(QSettings &settings);
    void save(QSettings &settings) const;

    static int normalizePort(int value, int fallback);
    PlannerStartupUdpOptions normalized() const;
    QList<int> orderedPorts() const;

    QString configurationStatus() const;
    static QString restartNote();
};

inline bool operator==(const PlannerStartupUdpOptions &left,
                       const PlannerStartupUdpOptions &right)
{
    return left.enabled == right.enabled
        && left.primaryPort == right.primaryPort
        && left.alternatePort == right.alternatePort;
}

inline bool operator!=(const PlannerStartupUdpOptions &left,
                       const PlannerStartupUdpOptions &right)
{
    return !(left == right);
}

#endif // PLANNERSTARTUPUDPOPTIONS_H
