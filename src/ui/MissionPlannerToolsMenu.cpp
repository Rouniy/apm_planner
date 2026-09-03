#include "MissionPlannerToolsMenu.h"

#include <QAction>
#include <QCoreApplication>
#include <QKeySequence>
#include <QMenu>

namespace
{
QString toolText(const char *source)
{
    return QCoreApplication::translate("MissionPlannerToolsMenu", source);
}
}

QList<MissionPlannerToolDefinition> MissionPlannerToolsMenu::Inventory()
{
    return {
        {QStringLiteral("actionDeveloperTools"),
         toolText("Developer Tools"), QStringLiteral("Ctrl+F"), false},
        {QStringLiteral("actionPluginManager"),
         toolText("Plugin Manager"), QStringLiteral("Ctrl+P"), false},
        {QStringLiteral("actionMavlinkInspector"),
         toolText("MAVLink Inspector"), QStringLiteral("Ctrl+I"), true},
        {QStringLiteral("actionMavlinkMirror"),
         toolText("MAVLink Mirror"), QString(), false},
        {QStringLiteral("actionNmeaOutput"),
         toolText("NMEA Output"), QStringLiteral("Ctrl+G"), false},
        {QStringLiteral("actionCotOutput"),
         toolText("Cursor-on-Target / TAK Output"), QString(), false},
        {QStringLiteral("actionDataFlashSpectrogram"),
         toolText("DataFlash Spectrogram"), QStringLiteral("Ctrl+L"), false},
        {QStringLiteral("actionMapTileCache"),
         toolText("Map Tile Cache"), QStringLiteral("Ctrl+X"), false},
        {QStringLiteral("actionTerrain3D"),
         toolText("3D Terrain View"), QString(), false},
        {QStringLiteral("actionMavlinkDeviceOperations"),
         toolText("MAVLink Device Operations"), QStringLiteral("Ctrl+J"), false},
        {QStringLiteral("actionPropagationSettings"),
         toolText("RF Propagation Settings"), QStringLiteral("Ctrl+W"), false},
        {QStringLiteral("actionFollowMe"),
         toolText("Follow Me (NMEA GPS)…"), QString(), false},
        {QStringLiteral("actionExternalGuided"),
         toolText("External Guided (File)…"), QString(), false},
        {QStringLiteral("actionMovingBase"),
         toolText("Moving Base (NMEA GPS)…"), QString(), false},
        {QStringLiteral("actionSwarmFormation"),
         toolText("Swarm Formation (Beta)"), QString(), false},
        {QStringLiteral("actionSwarmFollowPath"),
         toolText("Swarm Follow Path (Beta)"), QString(), false},
        {QStringLiteral("actionSwarmFollowLeader"),
         toolText("Swarm Follow Leader (Beta)"), QString(), false},
        {QStringLiteral("actionSwarmWaypointLeader"),
         toolText("Swarm Waypoint Leader (Beta)"), QString(), false},
        {QStringLiteral("actionSwarmSequence"),
         toolText("Swarm Sequence Layout Editor (Beta)"), QString(), false},
        {QStringLiteral("actionLinkStatistics"),
         toolText("Link Statistics"), QString(), true},
        {QStringLiteral("actionConnectionOptions"),
         toolText("Connection Options"), QString(), false},
        {QStringLiteral("actionDownloadLogs"),
         toolText("Download Logs (MAVLink)"), QString(), false},
        {QStringLiteral("actionTlogConvertExtract"),
         toolText("Tlog Convert / Extract"), QString(), false},
        {QStringLiteral("actionOsdVideoOverlay"),
         toolText("OSD Video — Telemetry Overlay"), QString(), false},
    };
}

QString MissionPlannerToolsMenu::UnavailableReason(const QString &title)
{
    return QCoreApplication::translate(
               "MissionPlannerToolsMenu",
               "%1 has not been ported yet. The item is disabled so it cannot "
               "open an empty or misleading view.")
        .arg(title);
}

void MissionPlannerToolsMenu::Populate(QMenu *menu, QObject *context,
                                       const HandlerMap &handlers)
{
    if (!menu || !context) {
        return;
    }

    // Removing an action does not destroy actions owned by MainWindow. This
    // keeps the old SETUP action sources alive while excluding legacy dock and
    // DATA/PLAN panel toggles from the MP10 application-tools surface.
    const QList<QAction *> oldActions = menu->actions();
    for (QAction *action : oldActions) {
        menu->removeAction(action);
    }

    menu->setTitle(toolText("TOOLS"));
    for (const MissionPlannerToolDefinition &definition : Inventory()) {
        if (definition.separatorBefore) {
            menu->addSeparator();
        }

        auto *action = new QAction(definition.title, menu);
        action->setObjectName(definition.objectName);
        if (!definition.shortcut.isEmpty()) {
            action->setShortcut(QKeySequence(definition.shortcut));
        }

        const auto handler = handlers.constFind(definition.objectName);
        if (handler == handlers.cend() || !handler.value()) {
            const QString reason = UnavailableReason(definition.title);
            action->setEnabled(false);
            action->setText(QCoreApplication::translate(
                                "MissionPlannerToolsMenu",
                                "%1 (not ported yet)")
                                .arg(definition.title));
            action->setToolTip(reason);
            action->setStatusTip(reason);
            action->setWhatsThis(reason);
            action->setProperty("unavailableReason", reason);
        } else {
            const Handler callback = handler.value();
            QObject::connect(action, &QAction::triggered, context,
                             [callback]() { callback(); });
        }
        menu->addAction(action);
    }
}
