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
    void deviceOperationsUsesSharedApplicationAction();
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

void ConfigDeveloperToolsViewTest::deviceOperationsUsesSharedApplicationAction()
{
    QObject actionSource;
    QAction deviceOperations(&actionSource);
    deviceOperations.setObjectName(
        QStringLiteral("actionMavlinkDeviceOperations"));
    bool triggered = false;
    connect(&deviceOperations, &QAction::triggered,
            this, [&triggered]() { triggered = true; });

    ConfigDeveloperToolsView view(&actionSource);
    QCOMPARE(view.ActionCount(), 32);
    QCOMPARE(view.ImplementedActionCount(), 3);
    QVERIFY(view.Log().contains(QStringLiteral("3 of 32")));
    auto *button = view.findChild<QPushButton *>(
        QStringLiteral("MavlinkDeviceOperationsButton"));
    QVERIFY(button);
    QVERIFY(button->isEnabled());
    button->click();
    QVERIFY(triggered);
    QVERIFY(view.Log().contains(
        QStringLiteral("Opened MAVLink Device Operations.")));

    deviceOperations.setEnabled(false);
    QVERIFY(!button->isEnabled());
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
