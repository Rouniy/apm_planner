#include <QtTest>

#include "ui/configuration/ConfigRouteProfile.h"

namespace {
QStringList allPageIds(const QList<ConfigRouteDefinition> &inventory)
{
    QStringList result;
    for (const ConfigRouteDefinition &route : inventory) {
        result.append(route.pageId);
    }
    return result;
}

QStringList factoryPageIds(const QList<ConfigRouteDefinition> &inventory)
{
    QStringList result;
    for (const ConfigRouteDefinition &route : inventory) {
        if (route.currentQtFactory) {
            result.append(route.pageId);
        }
    }
    return result;
}

ConfigRouteContext connectedContext(ConfigVehicleKind vehicle,
                                    bool advanced = false)
{
    ConfigRouteContext context;
    context.connected = true;
    context.advanced = advanced;
    context.vehicle = vehicle;
    context.legacyHeliSetupAvailable =
        vehicle == ConfigVehicleKind::Helicopter;
    return context;
}
}

class ConfigRouteProfileTest final : public QObject
{
    Q_OBJECT

private slots:
    void inventoryMatchesMissionPlanner10();
    void profileFlagsDefaultEnabledAndGateRoutes();
    void offlineAndAdvancedGatesMatchReference();
    void copterRoutesSeparateReferenceFromCurrentQt();
    void helicopterDoesNotReceiveCopterBasicTuning();
    void modernHelicopterDoesNotReceiveLegacyConfigEditor();
    void planeUsesActionableQpExtendedPage();
    void roverUsesOnlyItsCurrentTuningPage();
};

void ConfigRouteProfileTest::inventoryMatchesMissionPlanner10()
{
    const QList<ConfigRouteDefinition> inventory =
        ConfigRouteProfile::inventory();
    QCOMPARE(inventory.size(), 15);
    QCOMPARE(allPageIds(inventory), QStringList({
        QStringLiteral("ConfigFlightModesView"),
        QStringLiteral("ConfigFriendlyParamsView"),
        QStringLiteral("ConfigFriendlyParamsAdvView"),
        QStringLiteral("ConfigAC_FenceView"),
        QStringLiteral("ConfigBasicTuningView"),
        QStringLiteral("ConfigTradHeliView"),
        QStringLiteral("ConfigArduplaneView"),
        QStringLiteral("ConfigArduroverView"),
        QStringLiteral("ConfigExtendedTuningView"),
        QStringLiteral("ConfigOSDView"),
        QStringLiteral("MavFTPUIView"),
        QStringLiteral("ConfigUserDefinedView"),
        QStringLiteral("RawParamsView"),
        QStringLiteral("ConfigPlannerView"),
        QStringLiteral("ConfigPlannerAdvView")
    }));

    QStringList labels;
    for (const ConfigRouteDefinition &route : inventory) {
        labels.append(route.label);
    }
    QCOMPARE(labels, QStringList({
        QStringLiteral("Flight Modes"),
        QStringLiteral("Standard Params"),
        QStringLiteral("Advanced Params"),
        QStringLiteral("GeoFence"),
        QStringLiteral("Basic Tuning"),
        QStringLiteral("Heli Setup"),
        QStringLiteral("Basic Tuning (Plane)"),
        QStringLiteral("Basic Tuning (Rover)"),
        QStringLiteral("Extended Tuning"),
        QStringLiteral("Onboard OSD"),
        QStringLiteral("MAVFtp"),
        QStringLiteral("User Params"),
        QStringLiteral("Full Parameter List"),
        QStringLiteral("Planner"),
        QStringLiteral("Planner (Advanced)")
    }));

    QCOMPARE(factoryPageIds(inventory), QStringList({
        QStringLiteral("ConfigFlightModesView"),
        QStringLiteral("ConfigFriendlyParamsView"),
        QStringLiteral("ConfigFriendlyParamsAdvView"),
        QStringLiteral("ConfigAC_FenceView"),
        QStringLiteral("ConfigBasicTuningView"),
        QStringLiteral("ConfigTradHeliView"),
        QStringLiteral("ConfigArduplaneView"),
        QStringLiteral("ConfigArduroverView"),
        QStringLiteral("ConfigExtendedTuningView"),
        QStringLiteral("ConfigOSDView"),
        QStringLiteral("MavFTPUIView"),
        QStringLiteral("ConfigUserDefinedView"),
        QStringLiteral("RawParamsView"),
        QStringLiteral("ConfigPlannerView"),
        QStringLiteral("ConfigPlannerAdvView")
    }));
    QCOMPARE(factoryPageIds(inventory).size(), 15);
    QCOMPARE(ConfigRouteProfile::currentFactoryPageIds(),
             factoryPageIds(inventory));

    QVERIFY(inventory.at(0).requiresConnection);
    QVERIFY(inventory.at(2).requiresConnection);
    QVERIFY(inventory.at(2).isAdvanced);
    QVERIFY(inventory.at(12).allowsPartialParameters);
    QVERIFY(!inventory.at(12).requiresConnection);
    QVERIFY(!inventory.at(13).requiresConnection);
    QVERIFY(inventory.at(14).isAdvanced);
    QCOMPARE(static_cast<int>(inventory.at(4).profileFlag),
             static_cast<int>(ConfigProfileFlag::BasicTuning));
    QCOMPARE(static_cast<int>(inventory.at(5).profileFlag),
             static_cast<int>(ConfigProfileFlag::BasicTuning));
    QCOMPARE(static_cast<int>(inventory.at(6).profileFlag),
             static_cast<int>(ConfigProfileFlag::BasicTuning));
    QCOMPARE(static_cast<int>(inventory.at(7).profileFlag),
             static_cast<int>(ConfigProfileFlag::BasicTuning));
    QCOMPARE(static_cast<int>(inventory.at(13).profileFlag),
             static_cast<int>(ConfigProfileFlag::PlannerSettings));
    QCOMPARE(static_cast<int>(inventory.at(14).profileFlag),
             static_cast<int>(ConfigProfileFlag::PlannerSettings));
}

void ConfigRouteProfileTest::profileFlagsDefaultEnabledAndGateRoutes()
{
    ConfigRouteProfileFlags flags;
    const QList<ConfigProfileFlag> allFlags = {
        ConfigProfileFlag::FlightModes,
        ConfigProfileFlag::StandardParams,
        ConfigProfileFlag::AdvancedParams,
        ConfigProfileFlag::GeoFence,
        ConfigProfileFlag::BasicTuning,
        ConfigProfileFlag::ExtendedTuning,
        ConfigProfileFlag::OnboardOsd,
        ConfigProfileFlag::MavFtp,
        ConfigProfileFlag::UserParams,
        ConfigProfileFlag::FullParameterList,
        ConfigProfileFlag::PlannerSettings
    };
    for (ConfigProfileFlag flag : allFlags) {
        QVERIFY(flags.isEnabled(flag));
        flags.setEnabled(flag, false);
        QVERIFY(!flags.isEnabled(flag));
        flags.setEnabled(flag, true);
    }

    ConfigRouteContext context = connectedContext(
        ConfigVehicleKind::Copter, true);
    context.profile.basicTuning = false;
    context.profile.extendedTuning = false;
    context.profile.plannerSettings = false;
    const QStringList visible =
        ConfigRouteProfile::referenceVisiblePageIds(context);
    QVERIFY(!visible.contains(QStringLiteral("ConfigBasicTuningView")));
    QVERIFY(!visible.contains(QStringLiteral("ConfigTradHeliView")));
    QVERIFY(!visible.contains(QStringLiteral("ConfigArduplaneView")));
    QVERIFY(!visible.contains(QStringLiteral("ConfigArduroverView")));
    QVERIFY(!visible.contains(QStringLiteral("ConfigExtendedTuningView")));
    QVERIFY(!visible.contains(QStringLiteral("ConfigPlannerView")));
    QVERIFY(!visible.contains(QStringLiteral("ConfigPlannerAdvView")));
}

void ConfigRouteProfileTest::offlineAndAdvancedGatesMatchReference()
{
    ConfigRouteContext context;
    QCOMPARE(ConfigRouteProfile::referenceVisiblePageIds(context),
             QStringList({QStringLiteral("RawParamsView"),
                          QStringLiteral("ConfigPlannerView")}));
    QCOMPARE(ConfigRouteProfile::actionablePageIds(context),
             QStringList({QStringLiteral("RawParamsView"),
                          QStringLiteral("ConfigPlannerView")}));

    context.advanced = true;
    QCOMPARE(ConfigRouteProfile::referenceVisiblePageIds(context),
             QStringList({QStringLiteral("RawParamsView"),
                          QStringLiteral("ConfigPlannerView"),
                          QStringLiteral("ConfigPlannerAdvView")}));
    QCOMPARE(ConfigRouteProfile::actionablePageIds(context),
             ConfigRouteProfile::referenceVisiblePageIds(context));
}

void ConfigRouteProfileTest::copterRoutesSeparateReferenceFromCurrentQt()
{
    const ConfigRouteContext context = connectedContext(
        ConfigVehicleKind::Copter);
    QCOMPARE(ConfigRouteProfile::referenceVisiblePageIds(context),
             QStringList({
                 QStringLiteral("ConfigFlightModesView"),
                 QStringLiteral("ConfigFriendlyParamsView"),
                 QStringLiteral("ConfigAC_FenceView"),
                 QStringLiteral("ConfigBasicTuningView"),
                 QStringLiteral("ConfigExtendedTuningView"),
                 QStringLiteral("ConfigOSDView"),
                 QStringLiteral("MavFTPUIView"),
                 QStringLiteral("ConfigUserDefinedView"),
                 QStringLiteral("RawParamsView"),
                 QStringLiteral("ConfigPlannerView")
             }));
    QCOMPARE(ConfigRouteProfile::actionablePageIds(context),
             QStringList({
                 QStringLiteral("ConfigFlightModesView"),
                 QStringLiteral("ConfigFriendlyParamsView"),
                 QStringLiteral("ConfigAC_FenceView"),
                 QStringLiteral("ConfigBasicTuningView"),
                 QStringLiteral("ConfigExtendedTuningView"),
                 QStringLiteral("ConfigOSDView"),
                 QStringLiteral("MavFTPUIView"),
                 QStringLiteral("ConfigUserDefinedView"),
                 QStringLiteral("RawParamsView"),
                 QStringLiteral("ConfigPlannerView")
             }));
}

void ConfigRouteProfileTest::helicopterDoesNotReceiveCopterBasicTuning()
{
    const ConfigRouteContext context = connectedContext(
        ConfigVehicleKind::Helicopter);
    const QStringList reference =
        ConfigRouteProfile::referenceVisiblePageIds(context);
    QVERIFY(reference.contains(QStringLiteral("ConfigTradHeliView")));
    QVERIFY(reference.contains(QStringLiteral("ConfigExtendedTuningView")));
    QVERIFY(!reference.contains(QStringLiteral("ConfigBasicTuningView")));

    const QStringList actionable =
        ConfigRouteProfile::actionablePageIds(context);
    QCOMPARE(actionable, QStringList({
        QStringLiteral("ConfigFlightModesView"),
        QStringLiteral("ConfigFriendlyParamsView"),
        QStringLiteral("ConfigAC_FenceView"),
        QStringLiteral("ConfigTradHeliView"),
        QStringLiteral("ConfigExtendedTuningView"),
        QStringLiteral("ConfigOSDView"),
        QStringLiteral("MavFTPUIView"),
        QStringLiteral("ConfigUserDefinedView"),
        QStringLiteral("RawParamsView"),
        QStringLiteral("ConfigPlannerView")
    }));
}

void ConfigRouteProfileTest::modernHelicopterDoesNotReceiveLegacyConfigEditor()
{
    ConfigRouteContext context = connectedContext(
        ConfigVehicleKind::Helicopter);
    context.legacyHeliSetupAvailable = false;
    QVERIFY(!ConfigRouteProfile::isReferenceVisible(
        ConfigRouteId::HeliSetup, context));
    QVERIFY(!ConfigRouteProfile::isActionable(
        ConfigRouteId::HeliSetup, context));
    QVERIFY(!ConfigRouteProfile::actionablePageIds(context).contains(
        QStringLiteral("ConfigTradHeliView")));
}

void ConfigRouteProfileTest::planeUsesActionableQpExtendedPage()
{
    const ConfigRouteContext context = connectedContext(
        ConfigVehicleKind::Plane);
    QCOMPARE(ConfigRouteProfile::label(ConfigRouteId::ExtendedTuning,
                                       context.vehicle),
             QStringLiteral("QP Extended Tuning"));
    const QStringList reference =
        ConfigRouteProfile::referenceVisiblePageIds(context);
    QVERIFY(reference.contains(QStringLiteral("ConfigArduplaneView")));
    QVERIFY(reference.contains(QStringLiteral("ConfigExtendedTuningView")));
    QVERIFY(!reference.contains(QStringLiteral("ConfigBasicTuningView")));

    const QStringList actionable =
        ConfigRouteProfile::actionablePageIds(context);
    QCOMPARE(actionable, QStringList({
        QStringLiteral("ConfigFlightModesView"),
        QStringLiteral("ConfigFriendlyParamsView"),
        QStringLiteral("ConfigArduplaneView"),
        QStringLiteral("ConfigExtendedTuningView"),
        QStringLiteral("ConfigOSDView"),
        QStringLiteral("MavFTPUIView"),
        QStringLiteral("ConfigUserDefinedView"),
        QStringLiteral("RawParamsView"),
        QStringLiteral("ConfigPlannerView")
    }));
}

void ConfigRouteProfileTest::roverUsesOnlyItsCurrentTuningPage()
{
    const ConfigRouteContext context = connectedContext(
        ConfigVehicleKind::Rover, true);
    QCOMPARE(ConfigRouteProfile::label(ConfigRouteId::ExtendedTuning,
                                       context.vehicle),
             QStringLiteral("Extended Tuning"));
    const QStringList reference =
        ConfigRouteProfile::referenceVisiblePageIds(context);
    QVERIFY(reference.contains(QStringLiteral("ConfigArduroverView")));
    QVERIFY(reference.contains(QStringLiteral("ConfigExtendedTuningView")));

    const QStringList actionable =
        ConfigRouteProfile::actionablePageIds(context);
    QCOMPARE(actionable, QStringList({
        QStringLiteral("ConfigFlightModesView"),
        QStringLiteral("ConfigFriendlyParamsView"),
        QStringLiteral("ConfigFriendlyParamsAdvView"),
        QStringLiteral("ConfigArduroverView"),
        QStringLiteral("ConfigOSDView"),
        QStringLiteral("MavFTPUIView"),
        QStringLiteral("ConfigUserDefinedView"),
        QStringLiteral("RawParamsView"),
        QStringLiteral("ConfigPlannerView"),
        QStringLiteral("ConfigPlannerAdvView")
    }));
}

QTEST_APPLESS_MAIN(ConfigRouteProfileTest)
#include "test_configrouteprofile.moc"
