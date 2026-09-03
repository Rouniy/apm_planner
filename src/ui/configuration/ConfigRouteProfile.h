#ifndef CONFIGROUTEPROFILE_H
#define CONFIGROUTEPROFILE_H

#include <QList>
#include <QString>
#include <QStringList>

enum class ConfigRouteId
{
    FlightModes,
    StandardParams,
    AdvancedParams,
    GeoFence,
    BasicTuning,
    HeliSetup,
    PlaneTuning,
    RoverTuning,
    ExtendedTuning,
    OnboardOsd,
    MavFtp,
    UserParams,
    FullParameterList,
    Planner,
    PlannerAdvanced
};

enum class ConfigVehicleKind
{
    Unknown,
    Copter,
    Helicopter,
    // ArduPlane fixed-wing and VTOL vehicles share the MP10 CONFIG profile.
    Plane,
    Rover,
    Other
};

enum class ConfigProfileFlag
{
    FlightModes,
    StandardParams,
    AdvancedParams,
    GeoFence,
    BasicTuning,
    ExtendedTuning,
    OnboardOsd,
    MavFtp,
    UserParams,
    FullParameterList,
    PlannerSettings
};

struct ConfigRouteProfileFlags
{
    bool flightModes = true;
    // A standalone policy context is permissive for focused tests/callers.
    // Production replaces all eleven values from DisplayViewProfile.
    bool standardParams = true;
    bool advancedParams = true;
    bool geoFence = true;
    bool basicTuning = true;
    bool extendedTuning = true;
    bool onboardOsd = true;
    bool mavFtp = true;
    bool userParams = true;
    bool fullParameterList = true;
    bool plannerSettings = true;

    bool isEnabled(ConfigProfileFlag flag) const;
    void setEnabled(ConfigProfileFlag flag, bool enabled);
};

struct ConfigRouteContext
{
    bool connected = false;
    bool advanced = false;
    ConfigVehicleKind vehicle = ConfigVehicleKind::Unknown;
    ConfigRouteProfileFlags profile;
};

struct ConfigRouteDefinition
{
    ConfigRouteId route = ConfigRouteId::FlightModes;
    QString pageId;
    QString label;
    ConfigProfileFlag profileFlag = ConfigProfileFlag::FlightModes;
    bool requiresConnection = false;
    bool isAdvanced = false;
    bool allowsPartialParameters = false;
    bool currentQtFactory = false;
};

/**
 * Pure MP10 CONFIG navigation policy.
 *
 * Reference visibility describes the Mission Planner 10 route matrix.
 * Actionable visibility additionally rejects routes which do not yet have a
 * truthful Qt factory, or whose retained legacy factory is valid for only a
 * narrower vehicle family. Keeping the two answers separate prevents an
 * unrelated legacy widget from being presented as a completed MP10 page.
 */
class ConfigRouteProfile final
{
public:
    static QList<ConfigRouteDefinition> inventory();
    static ConfigRouteDefinition definition(ConfigRouteId route);
    static QString label(ConfigRouteId route, ConfigVehicleKind vehicle);

    static bool isReferenceVisible(ConfigRouteId route,
                                   const ConfigRouteContext &context);
    static bool isActionable(ConfigRouteId route,
                             const ConfigRouteContext &context);

    static QList<ConfigRouteId> referenceVisibleRoutes(
        const ConfigRouteContext &context);
    static QList<ConfigRouteId> actionableRoutes(
        const ConfigRouteContext &context);
    static QStringList currentFactoryPageIds();
    static QStringList referenceVisiblePageIds(
        const ConfigRouteContext &context);
    static QStringList actionablePageIds(const ConfigRouteContext &context);

private:
    ConfigRouteProfile() = delete;
};

#endif // CONFIGROUTEPROFILE_H
