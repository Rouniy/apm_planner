#include <QtTest>

#include "ui/configuration/ConfigDeveloperToolsView.h"

#include <QAction>
#include <QObject>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QWidget>

class ConfigDeveloperToolsViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void mirrorsMissionPlannerInventory();
    void sharedApplicationActionsOpenTools();
    void decodersAppendResultsAndErrors();
    void actionGridAdaptsToAvailableWidth();
};

void ConfigDeveloperToolsViewTest::mirrorsMissionPlannerInventory()
{
    ConfigDeveloperToolsView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigDeveloperToolsView"));
    QCOMPARE(view.Title(), QStringLiteral("Developer Tools"));
    QCOMPARE(view.ActionCount(), 32);
    QCOMPARE(view.ImplementedActionCount(), 2);
    QVERIFY(view.Log().contains(QStringLiteral("2 of 32")));

    const QList<QPushButton *> buttons = view.findChildren<QPushButton *>();
    QCOMPARE(buttons.size(), 32);
    int enabled = 0;
    for (QPushButton *button : buttons) {
        if (button->isEnabled()) {
            ++enabled;
        } else {
            QVERIFY(!button->toolTip().isEmpty());
        }
    }
    QCOMPARE(enabled, 2);
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("DecodeMavlinkPacketButton"))->isEnabled());
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("DecodeHardwareIdButton"))->isEnabled());
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("RebootVehicleButton"))->isEnabled());
}

void ConfigDeveloperToolsViewTest::sharedApplicationActionsOpenTools()
{
    QObject actionSource;
    QAction deviceOperations(&actionSource);
    deviceOperations.setObjectName(
        QStringLiteral("actionMavlinkDeviceOperations"));
    QAction terrain(&actionSource);
    terrain.setObjectName(QStringLiteral("actionTerrain3D"));
    QAction osdVideo(&actionSource);
    osdVideo.setObjectName(QStringLiteral("actionOsdVideoOverlay"));
    bool deviceTriggered = false;
    bool terrainTriggered = false;
    bool osdVideoTriggered = false;
    connect(&deviceOperations, &QAction::triggered,
            this, [&deviceTriggered]() { deviceTriggered = true; });
    connect(&terrain, &QAction::triggered,
            this, [&terrainTriggered]() { terrainTriggered = true; });
    connect(&osdVideo, &QAction::triggered,
            this, [&osdVideoTriggered]() { osdVideoTriggered = true; });

    ConfigDeveloperToolsView view(&actionSource);
    QCOMPARE(view.ActionCount(), 32);
    QCOMPARE(view.ImplementedActionCount(), 5);
    QVERIFY(view.Log().contains(QStringLiteral("5 of 32")));
    auto *deviceButton = view.findChild<QPushButton *>(
        QStringLiteral("MavlinkDeviceOperationsButton"));
    auto *terrainButton = view.findChild<QPushButton *>(
        QStringLiteral("Terrain3dViewButton"));
    auto *osdVideoButton = view.findChild<QPushButton *>(
        QStringLiteral("OsdVideoTelemetryOverlayButton"));
    QVERIFY(deviceButton);
    QVERIFY(terrainButton);
    QVERIFY(osdVideoButton);
    QVERIFY(deviceButton->isEnabled());
    QVERIFY(terrainButton->isEnabled());
    QVERIFY(osdVideoButton->isEnabled());
    deviceButton->click();
    terrainButton->click();
    osdVideoButton->click();
    QVERIFY(deviceTriggered);
    QVERIFY(terrainTriggered);
    QVERIFY(osdVideoTriggered);
    QVERIFY(view.Log().contains(
        QStringLiteral("Opened MAVLink Device Operations.")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened 3D Terrain View.")));
    QVERIFY(view.Log().contains(
        QStringLiteral("Opened OSD Video — Telemetry Overlay.")));

    deviceOperations.setEnabled(false);
    terrain.setEnabled(false);
    osdVideo.setEnabled(false);
    QVERIFY(!deviceButton->isEnabled());
    QVERIFY(!terrainButton->isEnabled());
    QVERIFY(!osdVideoButton->isEnabled());
}

void ConfigDeveloperToolsViewTest::decodersAppendResultsAndErrors()
{
    ConfigDeveloperToolsView view;
    view.ClearLog();

    view.DecodeHardwareIdInput(QStringLiteral("469530"),
                               QStringLiteral("COMPASS_DEV_ID"));
    QVERIFY(view.Log().contains(
        QStringLiteral("bus type SPI bus 3 address 42 devtype HMC5883")));

    view.DecodeHardwareIdInput(QStringLiteral("not-an-id"));
    QVERIFY(view.Log().contains(QStringLiteral("Hardware ID decode failed")));

    view.DecodeMavlinkInput(QStringLiteral("01 02 03"));
    QVERIFY(view.Log().contains(QStringLiteral("MAVLink decode failed")));
    QVERIFY(view.Log().contains(QStringLiteral("start byte")));
}

void ConfigDeveloperToolsViewTest::actionGridAdaptsToAvailableWidth()
{
    ConfigDeveloperToolsView view;
    view.resize(900, 720);
    view.show();
    QCoreApplication::processEvents();

    const int wideColumns = view.ColumnCount();
    QVERIFY(wideColumns >= 2);
    QVERIFY(wideColumns <= 4);

    view.resize(520, 720);
    QCoreApplication::processEvents();
    QVERIFY(view.ColumnCount() >= 1);
    QVERIFY(view.ColumnCount() < wideColumns);

    auto *host = view.findChild<QWidget *>(QStringLiteral("ActionItemsPanel"));
    auto *scroll = view.findChild<QScrollArea *>(QStringLiteral("ActionItemsScroll"));
    QVERIFY(host);
    QVERIFY(scroll);
    QVERIFY(scroll->height() <= scroll->maximumHeight());
    QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
    for (QPushButton *button : view.findChildren<QPushButton *>()) {
        QVERIFY2(button->geometry().right() <= host->contentsRect().right() + 1,
                 qPrintable(button->objectName()));
    }
}

QTEST_MAIN(ConfigDeveloperToolsViewTest)

#include "test_configdevelopertoolsview.moc"
