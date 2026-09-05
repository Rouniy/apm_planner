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
    QAction *nmea = makeAction(
        &actionSource, QStringLiteral("actionNmeaOutput"));
    QAction *cot = makeAction(
        &actionSource, QStringLiteral("actionCotOutput"));
    QAction *cache = makeAction(
        &actionSource, QStringLiteral("actionMapTileCache"));
    QAction *spectrogram = makeAction(
        &actionSource, QStringLiteral("actionDataFlashSpectrogram"));
    QAction *externalGuided = makeAction(
        &actionSource, QStringLiteral("actionExternalGuided"));
    QAction *followMe = makeAction(
        &actionSource, QStringLiteral("actionFollowMe"));
    QAction *movingBase = makeAction(
        &actionSource, QStringLiteral("actionMovingBase"));
    QAction *proximity = makeAction(
        &actionSource, QStringLiteral("actionProximity"));
    QAction *fft = makeAction(&actionSource, QStringLiteral("actionFftAnalysis"));
    QAction *paramGen = makeAction(&actionSource, QStringLiteral("actionParameterMetaDataRegeneration"));
    QAction *anonAction = makeAction(&actionSource, QStringLiteral("actionAnonLog"));
    QAction *warningAction = makeAction(&actionSource, QStringLiteral("actionWarningManager"));
    QAction *signingAction = makeAction(
        &actionSource, QStringLiteral("actionMavlinkSigning"));

    ConfigAdvancedView view(&actionSource);
    QCOMPARE(view.objectName(), QStringLiteral("ConfigAdvancedView"));
    QCOMPARE(view.Title(), QStringLiteral("Advanced"));
    QCOMPARE(view.ActionCount(), 16);
    QCOMPARE(view.ImplementedActionCount(), 14);
    QCOMPARE(view.PartialActionCount(), 1);
    QVERIFY(view.Log().contains(QStringLiteral("14 of 16")));
    QVERIFY(view.Log().contains(QStringLiteral("1 additional partial tool")));
    QSignalSpy warningSpy(warningAction, &QAction::triggered);
    auto *warningButton = view.findChild<QPushButton *>(QStringLiteral("WarningManagerButton"));
    QVERIFY(warningButton && warningButton->isEnabled());
    warningButton->click();
    QCOMPARE(warningSpy.count(), 1);
    QSignalSpy paramGenSpy(paramGen, &QAction::triggered);
    auto *paramGenButton = view.findChild<QPushButton *>(QStringLiteral("ParamGenButton"));
    QVERIFY(paramGenButton && paramGenButton->isEnabled());
    paramGenButton->click();
    QCOMPARE(paramGenSpy.count(), 1);

    QSignalSpy inspectorSpy(inspector, &QAction::triggered);
    QSignalSpy mirrorSpy(mirror, &QAction::triggered);
    QSignalSpy nmeaSpy(nmea, &QAction::triggered);
    QSignalSpy cotSpy(cot, &QAction::triggered);
    QSignalSpy cacheSpy(cache, &QAction::triggered);
    QSignalSpy spectrogramSpy(spectrogram, &QAction::triggered);
    QSignalSpy externalGuidedSpy(externalGuided, &QAction::triggered);
    QSignalSpy followMeSpy(followMe, &QAction::triggered);
    QSignalSpy movingBaseSpy(movingBase, &QAction::triggered);
    QSignalSpy proximitySpy(proximity, &QAction::triggered);
    QSignalSpy fftSpy(fft, &QAction::triggered);
    auto *fftButton = view.findChild<QPushButton *>(QStringLiteral("FftButton"));
    QVERIFY(fftButton && fftButton->isEnabled());
    fftButton->click();
    QCOMPARE(fftSpy.count(), 1);

    auto *inspectorButton = view.findChild<QPushButton *>(
        QStringLiteral("MAVLinkInspectorButton"));
    auto *mirrorButton = view.findChild<QPushButton *>(
        QStringLiteral("MavlinkMirrorButton"));
    auto *nmeaButton = view.findChild<QPushButton *>(
        QStringLiteral("NmeaButton"));
    auto *cotButton = view.findChild<QPushButton *>(
        QStringLiteral("CotTakButton"));
    auto *cacheButton = view.findChild<QPushButton *>(
        QStringLiteral("MapTileCacheButton"));
    auto *spectrogramButton = view.findChild<QPushButton *>(
        QStringLiteral("SpectrogramButton"));
    auto *externalGuidedButton = view.findChild<QPushButton *>(
        QStringLiteral("ExternalGuidedButton"));
    auto *followMeButton = view.findChild<QPushButton *>(
        QStringLiteral("FollowMeButton"));
    auto *movingBaseButton = view.findChild<QPushButton *>(
        QStringLiteral("MovingBaseButton"));
    auto *proximityButton = view.findChild<QPushButton *>(
        QStringLiteral("ProximityButton"));
    QVERIFY(inspectorButton && inspectorButton->isEnabled());
    QVERIFY(mirrorButton && mirrorButton->isEnabled());
    QVERIFY(nmeaButton && nmeaButton->isEnabled());
    QVERIFY(cotButton && cotButton->isEnabled());
    QVERIFY(cacheButton && cacheButton->isEnabled());
    QVERIFY(spectrogramButton && spectrogramButton->isEnabled());
    QVERIFY(externalGuidedButton && externalGuidedButton->isEnabled());
    QVERIFY(followMeButton && followMeButton->isEnabled());
    QVERIFY(movingBaseButton && movingBaseButton->isEnabled());
    QVERIFY(proximityButton && proximityButton->isEnabled());

    inspectorButton->click();
    mirrorButton->click();
    nmeaButton->click();
    cotButton->click();
    cacheButton->click();
    spectrogramButton->click();
    externalGuidedButton->click();
    followMeButton->click();
    movingBaseButton->click();
    proximityButton->click();
    QCOMPARE(inspectorSpy.count(), 1);
    QCOMPARE(mirrorSpy.count(), 1);
    QCOMPARE(nmeaSpy.count(), 1);
    QCOMPARE(cotSpy.count(), 1);
    QCOMPARE(cacheSpy.count(), 1);
    QCOMPARE(spectrogramSpy.count(), 1);
    QCOMPARE(externalGuidedSpy.count(), 1);
    QCOMPARE(followMeSpy.count(), 1);
    QCOMPARE(movingBaseSpy.count(), 1);
    QCOMPARE(proximitySpy.count(), 1);
    QVERIFY(view.Log().contains(QStringLiteral("Opened MAVLink Inspector")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Mavlink Mirror")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened NMEA")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Cursor-on-Target / TAK")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Map Tile Cache")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Spectrogram")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened External Guided")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Follow Me")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Moving Base")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened Proximity")));

    auto *anonLog = view.findChild<QPushButton *>(
        QStringLiteral("AnonLogButton"));
    auto *signing = view.findChild<QPushButton *>(
        QStringLiteral("MavlinkSigningButton"));
    QVERIFY(anonLog && anonLog->isEnabled());
    QSignalSpy anonSpy(anonAction, &QAction::triggered);
    anonLog->click();
    QCOMPARE(anonSpy.count(), 1);
    QSignalSpy signingSpy(signingAction, &QAction::triggered);
    QVERIFY(signing && signing->isEnabled());
    signing->click();
    QCOMPARE(signingSpy.count(), 1);
    QVERIFY(view.Log().contains(QStringLiteral("Opened MAVLink Signing")));
}

void ConfigAdvancedViewTest::tracksSharedActionAvailability()
{
    QWidget actionSource;
    QAction *inspector = makeAction(
        &actionSource, QStringLiteral("actionMavlinkInspector"));
    QAction *signing = makeAction(
        &actionSource, QStringLiteral("actionMavlinkSigning"));
    makeAction(&actionSource, QStringLiteral("actionMavlinkMirror"));
    makeAction(&actionSource, QStringLiteral("actionNmeaOutput"));
    makeAction(&actionSource, QStringLiteral("actionCotOutput"));
    makeAction(&actionSource, QStringLiteral("actionMapTileCache"));
    makeAction(&actionSource,
               QStringLiteral("actionDataFlashSpectrogram"));
    makeAction(&actionSource, QStringLiteral("actionExternalGuided"));
    makeAction(&actionSource, QStringLiteral("actionFollowMe"));
    makeAction(&actionSource, QStringLiteral("actionMovingBase"));
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

    auto *signingButton = view.findChild<QPushButton *>(
        QStringLiteral("MavlinkSigningButton"));
    QVERIFY(signingButton && signingButton->isEnabled());
    signing->setToolTip(QStringLiteral("Unlock the local signing vault"));
    signing->setEnabled(false);
    QVERIFY(!signingButton->isEnabled());
    QCOMPARE(signingButton->toolTip(),
             QStringLiteral("Unlock the local signing vault"));
    signing->setEnabled(true);
    QVERIFY(signingButton->isEnabled());
}

void ConfigAdvancedViewTest::failsClosedWhenApplicationActionsAreMissing()
{
    QWidget actionSource;
    ConfigAdvancedView view(&actionSource);

    QCOMPARE(view.ActionCount(), 16);
    QCOMPARE(view.ImplementedActionCount(), 0);
    QCOMPARE(view.PartialActionCount(), 0);
    for (QPushButton *button : view.findChildren<QPushButton *>()) {
        QVERIFY(!button->isEnabled());
        QVERIFY(!button->toolTip().isEmpty());
    }
}

QTEST_MAIN(ConfigAdvancedViewTest)

#include "test_configadvancedview.moc"
