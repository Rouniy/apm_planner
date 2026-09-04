#include "ConfigRouteProfile.h"

#include <QCoreApplication>

namespace {
QString routeText(const char *source)
{
    return QCoreApplication::translate("ConfigRouteProfile", source);
}

bool shellAllows(const ConfigRouteDefinition &definition,
                 const ConfigRouteContext &context)
{
    return context.profile.isEnabled(definition.profileFlag)
        && (!definition.requiresConnection || context.connected)
        && (!definition.isAdvanced || context.advanced);
}

bool isFriendlyParameterVehicle(ConfigVehicleKind vehicle)
{
    return vehicle == ConfigVehicleKind::Copter
        || vehicle == ConfigVehicleKind::Helicopter
        || vehicle == ConfigVehicleKind::Plane
        || vehicle == ConfigVehicleKind::Rover;
}

bool referenceVehicleAllows(ConfigRouteId route, ConfigVehicleKind vehicle)
{
    switch (route) {
    case ConfigRouteId::BasicTuning:
        return vehicle == ConfigVehicleKind::Copter;
    case ConfigRouteId::HeliSetup:
        return vehicle == ConfigVehicleKind::Helicopter;
    case ConfigRouteId::PlaneTuning:
        return vehicle == ConfigVehicleKind::Plane;
    case ConfigRouteId::RoverTuning:
        return vehicle == ConfigVehicleKind::Rover;
    default:
        return true;
    }
}

bool referenceCapabilityAllows(ConfigRouteId route,
                               const ConfigRouteContext &context)
{
    return route != ConfigRouteId::HeliSetup
        || context.legacyHeliSetupAvailable;
}

bool currentFactoryAllows(ConfigRouteId route, ConfigVehicleKind vehicle)
{
    switch (route) {
    case ConfigRouteId::StandardParams:
    case ConfigRouteId::AdvancedParams:
        return isFriendlyParameterVehicle(vehicle);
    case ConfigRouteId::GeoFence:
        return vehicle == ConfigVehicleKind::Copter
            || vehicle == ConfigVehicleKind::Helicopter;
    case ConfigRouteId::BasicTuning:
        return vehicle == ConfigVehicleKind::Copter;
    case ConfigRouteId::PlaneTuning:
        return vehicle == ConfigVehicleKind::Plane;
    case ConfigRouteId::RoverTuning:
        return vehicle == ConfigVehicleKind::Rover;
    case ConfigRouteId::ExtendedTuning:
        // The retained ATC_/PSC_/WPNAV editor is useful for ArduCopter,
        // including traditional helicopters. Plane uses the native QP page.
        return vehicle == ConfigVehicleKind::Copter
            || vehicle == ConfigVehicleKind::Helicopter
            || vehicle == ConfigVehicleKind::Plane;
    default:
        return true;
    }
}

QStringList pageIds(const QList<ConfigRouteId> &routes)
{
    QStringList result;
    result.reserve(routes.size());
    for (ConfigRouteId route : routes) {
        result.append(ConfigRouteProfile::definition(route).pageId);
    }
    return result;
}
}

bool ConfigRouteProfileFlags::isEnabled(ConfigProfileFlag flag) const
{
    switch (flag) {
    case ConfigProfileFlag::FlightModes:
        return flightModes;
    case ConfigProfileFlag::StandardParams:
        return standardParams;
    case ConfigProfileFlag::AdvancedParams:
        return advancedParams;
    case ConfigProfileFlag::GeoFence:
        return geoFence;
    case ConfigProfileFlag::BasicTuning:
        return basicTuning;
    case ConfigProfileFlag::ExtendedTuning:
        return extendedTuning;
    case ConfigProfileFlag::OnboardOsd:
        return onboardOsd;
    case ConfigProfileFlag::MavFtp:
        return mavFtp;
    case ConfigProfileFlag::UserParams:
        return userParams;
    case ConfigProfileFlag::FullParameterList:
        return fullParameterList;
    case ConfigProfileFlag::PlannerSettings:
        return plannerSettings;
    }
    return false;
}

void ConfigRouteProfileFlags::setEnabled(ConfigProfileFlag flag, bool enabled)
{
    switch (flag) {
    case ConfigProfileFlag::FlightModes:
        flightModes = enabled;
        break;
    case ConfigProfileFlag::StandardParams:
        standardParams = enabled;
        break;
    case ConfigProfileFlag::AdvancedParams:
        advancedParams = enabled;
        break;
    case ConfigProfileFlag::GeoFence:
        geoFence = enabled;
        break;
    case ConfigProfileFlag::BasicTuning:
        basicTuning = enabled;
        break;
    case ConfigProfileFlag::ExtendedTuning:
        extendedTuning = enabled;
        break;
    case ConfigProfileFlag::OnboardOsd:
        onboardOsd = enabled;
        break;
    case ConfigProfileFlag::MavFtp:
        mavFtp = enabled;
        break;
    case ConfigProfileFlag::UserParams:
        userParams = enabled;
        break;
    case ConfigProfileFlag::FullParameterList:
        fullParameterList = enabled;
        break;
    case ConfigProfileFlag::PlannerSettings:
        plannerSettings = enabled;
        break;
    }
}

QList<ConfigRouteDefinition> ConfigRouteProfile::inventory()
{
    // Order and metadata mirror Mission Planner 10 ConfigViewModel exactly.
    return {
        {ConfigRouteId::FlightModes,
         QStringLiteral("ConfigFlightModesView"), routeText("Flight Modes"),
         ConfigProfileFlag::FlightModes, true, false, false, true},
        {ConfigRouteId::StandardParams,
         QStringLiteral("ConfigFriendlyParamsView"),
         routeText("Standard Params"), ConfigProfileFlag::StandardParams,
         true, false, false, true},
        {ConfigRouteId::AdvancedParams,
         QStringLiteral("ConfigFriendlyParamsAdvView"),
         routeText("Advanced Params"), ConfigProfileFlag::AdvancedParams,
         true, true, false, true},
        {ConfigRouteId::GeoFence,
         QStringLiteral("ConfigAC_FenceView"), routeText("GeoFence"),
         ConfigProfileFlag::GeoFence, true, false, false, true},
        {ConfigRouteId::BasicTuning,
         QStringLiteral("ConfigBasicTuningView"), routeText("Basic Tuning"),
         ConfigProfileFlag::BasicTuning, true, false, false, true},
        {ConfigRouteId::HeliSetup,
         QStringLiteral("ConfigTradHeliView"), routeText("Heli Setup"),
         ConfigProfileFlag::BasicTuning, true, false, false, true},
        {ConfigRouteId::PlaneTuning,
         QStringLiteral("ConfigArduplaneView"),
         routeText("Basic Tuning (Plane)"), ConfigProfileFlag::BasicTuning,
         true, false, false, true},
        {ConfigRouteId::RoverTuning,
         QStringLiteral("ConfigArduroverView"),
         routeText("Basic Tuning (Rover)"), ConfigProfileFlag::BasicTuning,
         true, false, false, true},
        {ConfigRouteId::ExtendedTuning,
         QStringLiteral("ConfigExtendedTuningView"),
         routeText("Extended Tuning"), ConfigProfileFlag::ExtendedTuning,
         true, false, false, true},
        {ConfigRouteId::OnboardOsd,
         QStringLiteral("ConfigOSDView"), routeText("Onboard OSD"),
         ConfigProfileFlag::OnboardOsd, true, false, false, true},
        {ConfigRouteId::MavFtp,
         QStringLiteral("MavFTPUIView"), routeText("MAVFtp"),
         ConfigProfileFlag::MavFtp, true, false, true, true},
        {ConfigRouteId::UserParams,
         QStringLiteral("ConfigUserDefinedView"), routeText("User Params"),
         ConfigProfileFlag::UserParams, true, false, false, true},
        {ConfigRouteId::FullParameterList,
         QStringLiteral("RawParamsView"), routeText("Full Parameter List"),
         ConfigProfileFlag::FullParameterList, false, false, true, true},
        {ConfigRouteId::Planner,
         QStringLiteral("ConfigPlannerView"), routeText("Planner"),
         ConfigProfileFlag::PlannerSettings, false, false, false, true},
        {ConfigRouteId::PlannerAdvanced,
         QStringLiteral("ConfigPlannerAdvView"),
         routeText("Planner (Advanced)"),
         ConfigProfileFlag::PlannerSettings, false, true, false, true}
    };
}

ConfigRouteDefinition ConfigRouteProfile::definition(ConfigRouteId route)
{
    const QList<ConfigRouteDefinition> definitions = inventory();
    for (const ConfigRouteDefinition &definition : definitions) {
        if (definition.route == route) {
            return definition;
        }
    }
    return {};
}

QString ConfigRouteProfile::label(ConfigRouteId route,
                                  ConfigVehicleKind vehicle)
{
    if (route == ConfigRouteId::ExtendedTuning
        && vehicle == ConfigVehicleKind::Plane) {
        return routeText("QP Extended Tuning");
    }
    return definition(route).label;
}

bool ConfigRouteProfile::isReferenceVisible(
    ConfigRouteId route, const ConfigRouteContext &context)
{
    const ConfigRouteDefinition routeDefinition = definition(route);
    return shellAllows(routeDefinition, context)
        && referenceVehicleAllows(route, context.vehicle)
        && referenceCapabilityAllows(route, context);
}

bool ConfigRouteProfile::isActionable(ConfigRouteId route,
                                      const ConfigRouteContext &context)
{
    const ConfigRouteDefinition routeDefinition = definition(route);
    return routeDefinition.currentQtFactory
        && shellAllows(routeDefinition, context)
        && currentFactoryAllows(route, context.vehicle)
        && referenceCapabilityAllows(route, context);
}

QList<ConfigRouteId> ConfigRouteProfile::referenceVisibleRoutes(
    const ConfigRouteContext &context)
{
    QList<ConfigRouteId> result;
    for (const ConfigRouteDefinition &route : inventory()) {
        if (isReferenceVisible(route.route, context)) {
            result.append(route.route);
        }
    }
    return result;
}

QList<ConfigRouteId> ConfigRouteProfile::actionableRoutes(
    const ConfigRouteContext &context)
{
    QList<ConfigRouteId> result;
    for (const ConfigRouteDefinition &route : inventory()) {
        if (isActionable(route.route, context)) {
            result.append(route.route);
        }
    }
    return result;
}

QStringList ConfigRouteProfile::currentFactoryPageIds()
{
    QStringList result;
    for (const ConfigRouteDefinition &route : inventory()) {
        if (route.currentQtFactory) {
            result.append(route.pageId);
        }
    }
    return result;
}

QStringList ConfigRouteProfile::referenceVisiblePageIds(
    const ConfigRouteContext &context)
{
    return pageIds(referenceVisibleRoutes(context));
}

QStringList ConfigRouteProfile::actionablePageIds(
    const ConfigRouteContext &context)
{
    return pageIds(actionableRoutes(context));
}
