#include <QtTest>

#include "ui/MissionPlannerToolsMenu.h"

#include <QAction>
#include <QMenu>
#include <QSet>
#include <QStringList>

class MissionPlannerToolsMenuTest final : public QObject
{
    Q_OBJECT

private slots:
    void inventoryMatchesMissionPlanner10();
    void populatedMenuNeverLeavesUnavailableActionsAmbiguous();
};

void MissionPlannerToolsMenuTest::inventoryMatchesMissionPlanner10()
{
    const QList<MissionPlannerToolDefinition> inventory =
        MissionPlannerToolsMenu::Inventory();
    QCOMPARE(inventory.size(), 24);

    const QStringList expectedTitles = {
        QStringLiteral("Developer Tools"),
        QStringLiteral("Plugin Manager"),
        QStringLiteral("MAVLink Inspector"),
        QStringLiteral("MAVLink Mirror"),
        QStringLiteral("NMEA Output"),
        QStringLiteral("Cursor-on-Target / TAK Output"),
        QStringLiteral("DataFlash Spectrogram"),
        QStringLiteral("Map Tile Cache"),
        QStringLiteral("3D Terrain View"),
        QStringLiteral("MAVLink Device Operations"),
        QStringLiteral("RF Propagation Settings"),
        QStringLiteral("Follow Me (NMEA GPS)…"),
        QStringLiteral("External Guided (File)…"),
        QStringLiteral("Moving Base (NMEA GPS)…"),
        QStringLiteral("Swarm Formation (Beta)"),
        QStringLiteral("Swarm Follow Path (Beta)"),
        QStringLiteral("Swarm Follow Leader (Beta)"),
        QStringLiteral("Swarm Waypoint Leader (Beta)"),
        QStringLiteral("Swarm Sequence Layout Editor (Beta)"),
        QStringLiteral("Link Statistics"),
        QStringLiteral("Connection Options"),
        QStringLiteral("Download Logs (MAVLink)"),
        QStringLiteral("Tlog Convert / Extract"),
        QStringLiteral("OSD Video — Telemetry Overlay")
    };
    const QStringList expectedShortcuts = {
        QStringLiteral("Ctrl+F"), QStringLiteral("Ctrl+P"),
        QStringLiteral("Ctrl+I"), QString(), QStringLiteral("Ctrl+G"),
        QString(), QStringLiteral("Ctrl+L"), QStringLiteral("Ctrl+X"),
        QString(), QStringLiteral("Ctrl+J"), QStringLiteral("Ctrl+W"),
        QString(), QString(), QString(), QString(), QString(), QString(),
        QString(), QString(), QString(), QString(), QString(), QString(),
        QString()
    };
    QCOMPARE(expectedTitles.size(), inventory.size());
    QCOMPARE(expectedShortcuts.size(), inventory.size());

    QSet<QString> names;
    for (int index = 0; index < inventory.size(); ++index) {
        const MissionPlannerToolDefinition &definition = inventory.at(index);
        QCOMPARE(definition.title, expectedTitles.at(index));
        QCOMPARE(definition.shortcut, expectedShortcuts.at(index));
        QVERIFY2(!definition.objectName.isEmpty(), "Every tool needs an id");
        QVERIFY2(!definition.title.isEmpty(), "Every tool needs a label");
        QVERIFY2(!names.contains(definition.objectName),
                 qPrintable(definition.objectName));
        names.insert(definition.objectName);
    }
    QVERIFY(inventory.at(2).separatorBefore);
    QVERIFY(inventory.at(19).separatorBefore);
}

void MissionPlannerToolsMenuTest::populatedMenuNeverLeavesUnavailableActionsAmbiguous()
{
    QMenu menu;
    QObject context;
    int inspectorOpenCount = 0;
    int mirrorOpenCount = 0;
    int nmeaOpenCount = 0;
    MissionPlannerToolsMenu::HandlerMap handlers;
    handlers.insert(QStringLiteral("actionMavlinkInspector"),
                    [&inspectorOpenCount]() { ++inspectorOpenCount; });
    handlers.insert(QStringLiteral("actionMavlinkMirror"),
                    [&mirrorOpenCount]() { ++mirrorOpenCount; });
    handlers.insert(QStringLiteral("actionNmeaOutput"),
                    [&nmeaOpenCount]() { ++nmeaOpenCount; });

    MissionPlannerToolsMenu::Populate(&menu, &context, handlers);
    QCOMPARE(menu.title(), QStringLiteral("TOOLS"));

    const QList<QAction *> actions = menu.actions();
    int toolCount = 0;
    for (QAction *action : actions) {
        if (action->isSeparator()) {
            continue;
        }
        ++toolCount;
        if (action->objectName() == QStringLiteral("actionMavlinkInspector")
            || action->objectName() == QStringLiteral("actionMavlinkMirror")
            || action->objectName() == QStringLiteral("actionNmeaOutput")) {
            QVERIFY(action->isEnabled());
            action->trigger();
            continue;
        }
        QVERIFY(!action->isEnabled());
        QVERIFY(action->text().contains(QStringLiteral("not ported yet")));
        QVERIFY(!action->property("unavailableReason").toString().isEmpty());
    }
    QCOMPARE(toolCount, MissionPlannerToolsMenu::Inventory().size());
    QCOMPARE(inspectorOpenCount, 1);
    QCOMPARE(mirrorOpenCount, 1);
    QCOMPARE(nmeaOpenCount, 1);
}

QTEST_MAIN(MissionPlannerToolsMenuTest)
#include "test_missionplannertoolsmenu.moc"
