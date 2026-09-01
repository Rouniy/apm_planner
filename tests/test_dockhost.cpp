#include "ui/docking/DockHost.h"

#include <QAction>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>

class DockHostTest : public QObject
{
    Q_OBJECT

private slots:
    void createsStableMissionPlannerDocks();
    void rejectsForeignAndCorruptLayouts();
};

void DockHostTest::createsStableMissionPlannerDocks()
{
    DockHost host(QStringLiteral("flight-data"));
    auto *map = new QLabel(QStringLiteral("Map"));
    map->setObjectName(QStringLiteral("FdMap"));
    auto *hud = new QLabel(QStringLiteral("HUD"));
    hud->setObjectName(QStringLiteral("HudHost"));
    auto *quick = new QLabel(QStringLiteral("Quick"));
    quick->setObjectName(QStringLiteral("QuickHost"));

    QVERIFY(host.addDock(QStringLiteral("FdMap"), QStringLiteral("Map"), map,
                         DockHost::Location::Left));
    QVERIFY(host.addDock(QStringLiteral("HudHost"), QStringLiteral("HUD"), hud,
                         DockHost::Location::Right, QStringLiteral("FdMap"), QSize(320, 0)));
    QVERIFY(host.addDock(QStringLiteral("QuickHost"), QStringLiteral("Quick"), quick,
                         DockHost::Location::Tabbed, QStringLiteral("HudHost")));
    auto *duplicate = new QLabel;
    QVERIFY(!host.addDock(QStringLiteral("FdMap"), QStringLiteral("Duplicate"),
                          duplicate, DockHost::Location::Left));
    delete duplicate;
    QCOMPARE(host.dockIds(), QStringList({QStringLiteral("FdMap"),
                                         QStringLiteral("HudHost"),
                                         QStringLiteral("QuickHost")}));
    QCOMPARE(map->property("dockId").toString(), QStringLiteral("FdMap"));
    QVERIFY(host.toggleAction(QStringLiteral("QuickHost")));

    const QByteArray saved = host.saveLayout();
    QVERIFY(!saved.isEmpty());
    const QJsonObject envelope = QJsonDocument::fromJson(saved).object();
    QCOMPARE(envelope.value(QStringLiteral("version")).toInt(), DockHost::layoutSchemaVersion());
    QCOMPARE(envelope.value(QStringLiteral("viewId")).toString(), QStringLiteral("flight-data"));
    QVERIFY(host.restoreLayout(saved));
}

void DockHostTest::rejectsForeignAndCorruptLayouts()
{
    DockHost host(QStringLiteral("flight-planner"));
    QVERIFY(host.addDock(QStringLiteral("Map"), QStringLiteral("Map"), new QLabel,
                         DockHost::Location::Left));
    QSignalSpy rejected(&host, &DockHost::layoutRestoreRejected);
    QVERIFY(!host.restoreLayout(QByteArrayLiteral("not-json")));
    QCOMPARE(rejected.count(), 1);

    QJsonObject foreign;
    foreign.insert(QStringLiteral("schema"), QStringLiteral("apmplanner-dock-layout"));
    foreign.insert(QStringLiteral("version"), DockHost::layoutSchemaVersion());
    foreign.insert(QStringLiteral("viewId"), QStringLiteral("flight-data"));
    foreign.insert(QStringLiteral("affinity"), QStringLiteral("apmplanner-flight-data"));
    foreign.insert(QStringLiteral("payload"), QStringLiteral("e30="));
    QVERIFY(!host.restoreLayout(QJsonDocument(foreign).toJson(QJsonDocument::Compact)));
    QCOMPARE(rejected.count(), 2);
}

QTEST_MAIN(DockHostTest)
#include "test_dockhost.moc"
