#include "ui_UASActionsWidget.h"
#include "ui/uas/UASActionCatalog.h"
#include "ui/uas/UASActionsLayout.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QStringList>
#include <QTest>
#include <QWidget>

class UASActionsWidgetTest final : public QObject
{
    Q_OBJECT

private slots:
    void matchesMissionPlannerFiveByFiveLayout();
    void startsInAConnectionSafeState();
    void actionCatalogIncludesConfirmedFormatSdRequest();
};

void UASActionsWidgetTest::matchesMissionPlannerFiveByFiveLayout()
{
    QWidget widget;
    Ui::UASActionsWidget ui;
    ui.setupUi(&widget);

    UASActionsLayout::applyMissionPlannerProportions(ui);

    QCOMPARE(ui.OfficialActionsGrid->rowCount(), 5);
    QCOMPARE(ui.OfficialActionsGrid->columnCount(), 5);
    QCOMPARE(ui.OfficialActionsGrid->horizontalSpacing(), 2);
    QCOMPARE(ui.OfficialActionsGrid->verticalSpacing(), 4);
    for (int column = 0; column < 5; ++column) {
        QCOMPARE(ui.OfficialActionsGrid->columnStretch(column), 1);
    }

    QCOMPARE(ui.OfficialActionsGrid->itemAtPosition(0, 0)->widget(),
             static_cast<QWidget *>(ui.ActionSelector));
    QCOMPARE(ui.OfficialActionsGrid->itemAtPosition(0, 1)->widget(),
             static_cast<QWidget *>(ui.DoActionButton));
    QCOMPARE(ui.OfficialActionsGrid->itemAtPosition(0, 4)->widget(),
             static_cast<QWidget *>(ui.ChangeSpeedEditor));
    QCOMPARE(ui.OfficialActionsGrid->itemAtPosition(1, 4)->widget(),
             static_cast<QWidget *>(ui.ChangeAltitudeEditor));
    QCOMPARE(ui.OfficialActionsGrid->itemAtPosition(2, 4)->widget(),
             static_cast<QWidget *>(ui.LoiterRadiusEditor));
    QCOMPARE(ui.OfficialActionsGrid->itemAtPosition(3, 3)->widget(),
             static_cast<QWidget *>(ui.ArmDisarmButton));
    QCOMPARE(ui.OfficialActionsGrid->itemAtPosition(4, 3)->widget(),
             static_cast<QWidget *>(ui.ResumeMissionButton));
    QCOMPARE(ui.OfficialActionsGrid->itemAtPosition(4, 4)->widget(),
             static_cast<QWidget *>(ui.AbortLandingButton));

    for (QHBoxLayout *editor : {ui.changeSpeedEditorLayout,
                                ui.changeAltitudeEditorLayout,
                                ui.loiterRadiusEditorLayout}) {
        QCOMPARE(editor->count(), 2);
        QCOMPARE(editor->spacing(), 2);
        QCOMPARE(editor->stretch(0), 2);
        QCOMPARE(editor->stretch(1), 3);
    }

    const QSet<QString> expectedLabels{
        QStringLiteral("Do Action"), QStringLiteral("Auto"),
        QStringLiteral("Set Home Alt"), QStringLiteral("Change Speed"),
        QStringLiteral("Set WP"), QStringLiteral("Loiter"),
        QStringLiteral("Restart Mission"), QStringLiteral("Change Alt"),
        QStringLiteral("Set Mode"), QStringLiteral("RTL"),
        QStringLiteral("Raw Sensor View"), QStringLiteral("Set Loiter Rad"),
        QStringLiteral("Set Mount"), QStringLiteral("Joystick"),
        QStringLiteral("Arm / Disarm"), QStringLiteral("Clear Track"),
        QStringLiteral("Message"), QStringLiteral("Resume Mission"),
        QStringLiteral("Abort Landing")
    };
    QSet<QString> actualLabels;
    const QList<QPushButton *> buttons =
        ui.actionsGroupBox->findChildren<QPushButton *>();
    QCOMPARE(buttons.size(), 19);
    for (const QPushButton *button : buttons) {
        actualLabels.insert(button->text());
    }
    QVERIFY(actualLabels == expectedLabels);

    QCOMPARE(ui.ActionsScrollArea->horizontalScrollBarPolicy(),
             Qt::ScrollBarAlwaysOff);
    QVERIFY(ui.ActionsScrollArea->widgetResizable());
}

void UASActionsWidgetTest::startsInAConnectionSafeState()
{
    QWidget widget;
    Ui::UASActionsWidget ui;
    ui.setupUi(&widget);

    QVERIFY(!ui.actionsGroupBox->isEnabled());
    QCOMPARE(ui.ActionStatusLabel->text(), QStringLiteral("No active vehicle"));
    QCOMPARE(ui.ActionStatusLabel->accessibleName(),
             QStringLiteral("Vehicle action status"));
    const QStringList removedCompatibilityControls = {
        QStringLiteral("legacyCompatibilityWidget"),
        QStringLiteral("missionGroupBox"),
        QStringLiteral("shortcutGroupBox"),
        QStringLiteral("altitudeTypeComboBox"),
        QStringLiteral("currentWaypointLabel"),
        QStringLiteral("armedStatuslabel"),
        QStringLiteral("stabilizeModeButton"),
        QStringLiteral("opt1ModeButton"),
        QStringLiteral("opt2ModeButton"),
        QStringLiteral("opt3ModeButton"),
        QStringLiteral("opt4ModeButton")
    };
    for (const QString &objectName : removedCompatibilityControls) {
        QVERIFY2(widget.findChild<QWidget *>(objectName) == nullptr,
                 qPrintable(objectName));
    }

    // Enabling the connected surface must not accidentally arm buttons whose
    // backend workflow has not been ported safely yet.
    ui.actionsGroupBox->setEnabled(true);
    QVERIFY(ui.DoActionButton->isEnabled());
    QVERIFY(ui.RestartMissionButton->isEnabled());
    QVERIFY(ui.AbortLandingButton->isEnabled());
    for (QPushButton *unavailable : {
             ui.SetHomeAltButton,
             ui.LoiterRadiusButton, ui.SetMountButton,
             ui.MessageButton, ui.ResumeMissionButton}) {
        QVERIFY(!unavailable->isEnabled());
        QVERIFY(!unavailable->toolTip().isEmpty());
    }
    QVERIFY(ui.ClearTrackButton->isEnabled());
    QVERIFY(!ui.ClearTrackButton->toolTip().isEmpty());
    QVERIFY(ui.RawSensorViewButton->isEnabled());
    QVERIFY(!ui.RawSensorViewButton->toolTip().isEmpty());
    QVERIFY(ui.JoystickSetupButton->isEnabled());
    QVERIFY(!ui.JoystickSetupButton->toolTip().isEmpty());
}

void UASActionsWidgetTest::actionCatalogIncludesConfirmedFormatSdRequest()
{
    QCOMPARE(static_cast<int>(UASActionCatalog::StandardActions.size()), 7);
    const UASActionCatalog::Entry &formatAction =
        UASActionCatalog::StandardActions.back();
    QCOMPARE(QString::fromLatin1(formatAction.name),
             QStringLiteral("Format_SD_Card"));
    QCOMPARE(formatAction.command, MAV_CMD_STORAGE_FORMAT);

    const UASActionCatalog::CommandRequest request =
        UASActionCatalog::formatSdCardRequest();
    QCOMPARE(request.command, MAV_CMD_STORAGE_FORMAT);
    QCOMPARE(request.confirmation, 1);
    QCOMPARE(request.parameters[0], 1.0F);
    QCOMPARE(request.parameters[1], 1.0F);
    for (std::size_t index = 2; index < request.parameters.size(); ++index) {
        QCOMPARE(request.parameters[index], 0.0F);
    }
    QCOMPARE(request.component, MAV_COMP_ID_PRIMARY);
}

QTEST_MAIN(UASActionsWidgetTest)
#include "test_uasactionswidget.moc"
