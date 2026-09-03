#include <QtTest>

#include "ui/configuration/ActionPageView.h"
#include "ui/configuration/ConfigAdvancedView.h"

#include <QAction>
#include <QPushButton>
#include <QSignalSpy>
#include <QWidget>

namespace
{
QAction *makeAction(QObject *owner, const QString &objectName)
{
    auto *action = new QAction(owner);
    action->setObjectName(objectName);
    return action;
}
}

class ConfigAdvancedViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void mirrorsMissionPlannerInventoryAndRoutesSharedActions();
    void tracksSharedActionAvailability();
    void failsClosedWhenApplicationActionsAreMissing();
};

void ConfigAdvancedViewTest::mirrorsMissionPlannerInventoryAndRoutesSharedActions()
{
    QWidget actionSource;
    QAction *inspector = makeAction(
        &actionSource, QStringLiteral("actionMavlinkInspector"));
    QAction *mirror = makeAction(
        &actionSource, QStringLiteral("actionMavlinkMirror"));
    QAction *cache = makeAction(
        &actionSource, QStringLiteral("actionMapTileCache"));
    QAction *proximity = makeAction(
        &actionSource, QStringLiteral("actionProximity"));

    ConfigAdvancedView view(&actionSource);
    QCOMPARE(view.objectName(), QStringLiteral("ConfigAdvancedView"));
    QCOMPARE(view.Title(), QStringLiteral("Advanced"));
    QCOMPARE(view.ActionCount(), 16);
    QCOMPARE(view.ImplementedActionCount(), 4);
    QVERIFY(view.Log().contains(QStringLiteral("4 of 16")));

    QSignalSpy inspectorSpy(inspector, &QAction::triggered);
    QSignalSpy mirrorSpy(mirror, &QAction::triggered);
    QSignalSpy cacheSpy(cache, &QAction::triggered);
    QSignalSpy proximitySpy(proximity, &QAction::triggered);

    auto *inspectorButton = view.findChild<QPushButton *>(
        QStringLiteral("MAVLinkInspectorButton"));
    auto *mirrorButton = view.findChild<QPushButton *>(
        QStringLiteral("MavlinkMirrorButton"));
    auto *cacheButton = view.findChild<QPushButton *>(
        QStringLiteral("MapTileCacheButton"));
    auto *proximityButton = view.findChild<QPushButton *>(
        QStringLiteral("ProximityButton"));
    QVERIFY(inspectorButton && inspectorButton->isEnabled());
    QVERIFY(mirrorButton && mirrorButton->isEnabled());
    QVERIFY(cacheButton && cacheButton->isEnabled());
    QVERIFY(proximityButton && proximityButton->isEnabled());

    inspectorButton->click();
    mirrorButton->click();
    cacheButton->click();
    proximityButton->click();
    QCOMPARE(inspectorSpy.count(), 1);
    QCOMPARE(mirrorSpy.count(), 1);
    QCOMPARE(cacheSpy.count(), 1);
    QCOMPARE(proximitySpy.count(), 1);
    QVERIFY(view.Log().contains(QStringLiteral("Opened MAVLink Inspector")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Mavlink Mirror")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Map Tile Cache")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Proximity")));

    auto *anonLog = view.findChild<QPushButton *>(
        QStringLiteral("AnonLogButton"));
    auto *signing = view.findChild<QPushButton *>(
        QStringLiteral("MavlinkSigningButton"));
    QVERIFY(anonLog && !anonLog->isEnabled());
    QVERIFY(signing && !signing->isEnabled());
    QVERIFY(!anonLog->toolTip().isEmpty());
}

void ConfigAdvancedViewTest::tracksSharedActionAvailability()
{
    QWidget actionSource;
    QAction *inspector = makeAction(
        &actionSource, QStringLiteral("actionMavlinkInspector"));
    makeAction(&actionSource, QStringLiteral("actionMavlinkMirror"));
    makeAction(&actionSource, QStringLiteral("actionMapTileCache"));
    makeAction(&actionSource, QStringLiteral("actionProximity"));

    ConfigAdvancedView view(&actionSource);
    auto *button = view.findChild<QPushButton *>(
        QStringLiteral("MAVLinkInspectorButton"));
    QVERIFY(button && button->isEnabled());

    inspector->setToolTip(QStringLiteral("No active protocol"));
    inspector->setEnabled(false);
    QVERIFY(!button->isEnabled());
    QCOMPARE(button->toolTip(), QStringLiteral("No active protocol"));

    inspector->setEnabled(true);
    QVERIFY(button->isEnabled());
}

void ConfigAdvancedViewTest::failsClosedWhenApplicationActionsAreMissing()
{
    QWidget actionSource;
    ConfigAdvancedView view(&actionSource);

    QCOMPARE(view.ActionCount(), 16);
    QCOMPARE(view.ImplementedActionCount(), 0);
    for (QPushButton *button : view.findChildren<QPushButton *>()) {
        QVERIFY(!button->isEnabled());
        QVERIFY(!button->toolTip().isEmpty());
    }
}

QTEST_MAIN(ConfigAdvancedViewTest)

#include "test_configadvancedview.moc"
