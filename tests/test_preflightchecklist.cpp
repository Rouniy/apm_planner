#include "ui/flightdata/PreflightChecklistModel.h"
#include "ui/flightdata/PreflightChecklistWidget.h"
#include "ui/flightdata/SimpleActionsWidget.h"

#include <QCheckBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class PreflightChecklistTest final : public QObject
{
    Q_OBJECT

private slots:
    void matchesMissionPlannerDefaultOrder();
    void evaluatesOnlyConnectedTelemetry();
    void persistsManualChecksInFreshNamespace();
    void widgetKeepsAutomaticChecksReadOnly();
    void simpleActionsForwardCanonicalModesSafely();
};

void PreflightChecklistTest::matchesMissionPlannerDefaultOrder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    PreflightChecklistModel model(&settings);

    QCOMPARE(model.rowCount(), 12);
    QCOMPARE(model.data(model.index(0, 0),
                        PreflightChecklistModel::DescriptionRole).toString(),
             QStringLiteral("Verify GPS"));
    QCOMPARE(model.data(model.index(4, 0),
                        PreflightChecklistModel::DescriptionRole).toString(),
             QStringLiteral("Mode"));
    QCOMPARE(model.data(model.index(11, 0),
                        PreflightChecklistModel::DescriptionRole).toString(),
             QStringLiteral("Camera is on and ready to fly?"));
    QVERIFY(!model.data(model.index(0, 0),
                        PreflightChecklistModel::ManualRole).toBool());
    QVERIFY(model.data(model.index(4, 0),
                       PreflightChecklistModel::ManualRole).toBool());
    QVERIFY(model.data(model.index(11, 0),
                       PreflightChecklistModel::ManualRole).toBool());
}

void PreflightChecklistTest::evaluatesOnlyConnectedTelemetry()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    PreflightChecklistModel model(&settings);
    PreflightTelemetry telemetry;
    telemetry.gpsFixType = 3;
    telemetry.satelliteCount = 8;
    telemetry.linkQuality = 99.0;
    telemetry.batteryVoltage = 24.1;
    telemetry.mode = QStringLiteral("LOITER");
    telemetry.altitude = 1.5;
    model.setTelemetry(telemetry);

    for (int row : {0, 1, 2, 3, 5}) {
        QVERIFY(!model.data(model.index(row, 0),
                            PreflightChecklistModel::AvailableRole).toBool());
        QVERIFY(!model.data(model.index(row, 0),
                            PreflightChecklistModel::SatisfiedRole).toBool());
        QCOMPARE(model.data(model.index(row, 0),
                            PreflightChecklistModel::ValueRole).toString(),
                 QStringLiteral("Not connected"));
    }

    telemetry.connected = true;
    model.setTelemetry(telemetry);
    for (int row : {0, 1, 2, 3, 5}) {
        QVERIFY(model.data(model.index(row, 0),
                           PreflightChecklistModel::AvailableRole).toBool());
        QVERIFY(model.data(model.index(row, 0),
                           PreflightChecklistModel::SatisfiedRole).toBool());
    }
    QCOMPARE(model.data(model.index(4, 0),
                        PreflightChecklistModel::ValueRole).toString(),
             QStringLiteral("LOITER"));

    telemetry.gpsFixType = 2;
    telemetry.satelliteCount = 4;
    telemetry.linkQuality = 95.0;
    telemetry.batteryVoltage = 22.0;
    telemetry.altitude = 5.0;
    model.setTelemetry(telemetry);
    for (int row : {0, 1, 2, 3, 5}) {
        QVERIFY(!model.data(model.index(row, 0),
                            PreflightChecklistModel::SatisfiedRole).toBool());
    }
}

void PreflightChecklistTest::persistsManualChecksInFreshNamespace()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("preflight_manual"),
                          QStringLiteral("Tail and wings secured?"));
        settings.setValue(QStringLiteral("CHECKLIST_ITEMS/tail_wings"), true);
        PreflightChecklistModel model(&settings);
        QVERIFY(!model.data(model.index(6, 0),
                            PreflightChecklistModel::SatisfiedRole).toBool());
        QVERIFY(model.setManualState(6, true));
        QVERIFY(!model.setManualState(0, true));
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        PreflightChecklistModel model(&settings);
        QVERIFY(model.data(model.index(6, 0),
                           PreflightChecklistModel::SatisfiedRole).toBool());
        settings.beginGroup(PreflightChecklistModel::settingsGroup());
        settings.beginGroup(QStringLiteral("manual"));
        QCOMPARE(settings.value(QStringLiteral("tail_wings")).toBool(), true);
        settings.endGroup();
        settings.endGroup();
        QCOMPARE(settings.value(QStringLiteral("preflight_manual")).toString(),
                 QStringLiteral("Tail and wings secured?"));
        QCOMPARE(settings.value(
                     QStringLiteral("CHECKLIST_ITEMS/tail_wings")).toBool(),
                 true);
    }
}

void PreflightChecklistTest::widgetKeepsAutomaticChecksReadOnly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    PreflightChecklistModel model(&settings);
    PreflightChecklistWidget widget(&model);

    QWidget *gpsRow = widget.findChild<QWidget *>(
        QStringLiteral("PreflightCheckRow_gps_fix"));
    QWidget *manualRow = widget.findChild<QWidget *>(
        QStringLiteral("PreflightCheckRow_tail_wings"));
    QVERIFY(gpsRow);
    QVERIFY(manualRow);
    QCheckBox *gpsCheck = gpsRow->findChild<QCheckBox *>(
        QStringLiteral("Ok"));
    QCheckBox *manualCheck = manualRow->findChild<QCheckBox *>(
        QStringLiteral("Ok"));
    QVERIFY(gpsCheck);
    QVERIFY(manualCheck);
    QVERIFY(gpsCheck->testAttribute(Qt::WA_TransparentForMouseEvents));
    QVERIFY(!manualCheck->testAttribute(Qt::WA_TransparentForMouseEvents));

    manualCheck->click();
    QVERIFY(model.data(model.index(6, 0),
                       PreflightChecklistModel::SatisfiedRole).toBool());
}

void PreflightChecklistTest::simpleActionsForwardCanonicalModesSafely()
{
    SimpleActionsWidget widget;
    QSignalSpy spy(&widget, &SimpleActionsWidget::quickModeRequested);
    QPushButton *loiter = widget.findChild<QPushButton *>(
        QStringLiteral("SimpleLoiterButton"));
    QPushButton *rtl = widget.findChild<QPushButton *>(
        QStringLiteral("SimpleRtlButton"));
    QPushButton *automatic = widget.findChild<QPushButton *>(
        QStringLiteral("SimpleAutoButton"));
    QVERIFY(loiter);
    QVERIFY(rtl);
    QVERIFY(automatic);
    QVERIFY(!loiter->isEnabled());
    QVERIFY(!rtl->isEnabled());
    QVERIFY(!automatic->isEnabled());

    loiter->click();
    QCOMPARE(spy.count(), 0);
    widget.setActionsAvailable(true);
    loiter->click();
    rtl->click();
    automatic->click();
    QCOMPARE(spy.count(), 3);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("Loiter"));
    QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("RTL"));
    QCOMPARE(spy.at(2).at(0).toString(), QStringLiteral("Auto"));
}

QTEST_MAIN(PreflightChecklistTest)
#include "test_preflightchecklist.moc"
