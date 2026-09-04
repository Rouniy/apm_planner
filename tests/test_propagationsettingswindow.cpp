#include "ui/tools/PropagationSettingsStore.h"
#include "ui/tools/PropagationSettingsWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
template<typename Widget>
Widget *required(QWidget *root, const QString &name)
{
    Widget *result = root->findChild<Widget *>(name);
    if (!result) {
        QTest::qFail(qPrintable(QStringLiteral("Missing widget: %1").arg(name)),
                     __FILE__, __LINE__);
    }
    return result;
}

QStringList items(const QComboBox *combo)
{
    QStringList result;
    for (int index = 0; index < combo->count(); ++index) {
        result.append(combo->itemText(index));
    }
    return result;
}
}

class PropagationSettingsWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndExactKeysDoNotWriteOnConstruction();
    void storeLoadsLegacyValuesAndAutosavesEveryKey();
    void rendersCompleteModelessMp10Surface();
    void everyControlAutosavesAndSynchronizesOpenWindows();
    void closeButtonAndCtrlWCloseTheWindow();
};

void PropagationSettingsWindowTest::
defaultsAndExactKeysDoNotWriteOnConstruction()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("propagation.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("sentinel"), 17);
    settings.sync();
    const QStringList before = settings.allKeys();

    PropagationSettingsStore store(&settings);
    QVERIFY(store.settings() == PropagationSettings::Default());
    QVERIFY(store.load() == PropagationSettingsStore::defaults());
    QCOMPARE(settings.allKeys(), before);

    const PropagationSettings defaults = store.settings();
    QCOMPARE(defaults.clearanceMeters, 5.0);
    QCOMPARE(defaults.resolutionPixels, 4);
    QCOMPARE(defaults.azimuthStepDegrees, 1.0);
    QCOMPARE(defaults.convergenceDegrees, 1.0);
    QCOMPARE(defaults.rangeKilometers, 2.0);
    QCOMPARE(defaults.baseHeightMeters, 2.0);
    QCOMPARE(defaults.tolerance, 0.8);
    QCOMPARE(defaults.minimumAltitude, 100.0);
    QCOMPARE(defaults.maximumAltitude, 400.0);
    QVERIFY(!defaults.elevationMap);
    QVERIFY(!defaults.terrainMap);
    QVERIFY(!defaults.rfMap);
    QVERIFY(!defaults.homeDistance);
    QVERIFY(!defaults.droneDistance);
    QVERIFY(!defaults.manualAltitudeRange);
    QVERIFY(!defaults.showScale);

    QCOMPARE(PropagationSettingsStore::clearanceKey(),
             QStringLiteral("Propagation_Clearance"));
    QCOMPARE(PropagationSettingsStore::resolutionKey(),
             QStringLiteral("Propagation_Resolution"));
    QCOMPARE(PropagationSettingsStore::azimuthStepKey(),
             QStringLiteral("Propagation_Rotational"));
    QCOMPARE(PropagationSettingsStore::convergenceKey(),
             QStringLiteral("Propagation_Converge"));
    QCOMPARE(PropagationSettingsStore::rangeKey(),
             QStringLiteral("Propagation_Range"));
    QCOMPARE(PropagationSettingsStore::baseHeightKey(),
             QStringLiteral("Propagation_Height"));
    QCOMPARE(PropagationSettingsStore::toleranceKey(),
             QStringLiteral("Propagation_Tolerance"));
    QCOMPARE(PropagationSettingsStore::minimumAltitudeKey(),
             QStringLiteral("Propagation_Minalt"));
    QCOMPARE(PropagationSettingsStore::maximumAltitudeKey(),
             QStringLiteral("Propagation_Maxalt"));
    QCOMPARE(PropagationSettingsStore::elevationMapKey(),
             QStringLiteral("Propagation_Elemap"));
    QCOMPARE(PropagationSettingsStore::terrainMapKey(),
             QStringLiteral("Propagation_Termap"));
    QCOMPARE(PropagationSettingsStore::rfMapKey(),
             QStringLiteral("Propagation_RFmap"));
    QCOMPARE(PropagationSettingsStore::homeDistanceKey(),
             QStringLiteral("Propagation_home_kmleft"));
    QCOMPARE(PropagationSettingsStore::droneDistanceKey(),
             QStringLiteral("Propagation_drone_kmleft"));
    QCOMPARE(PropagationSettingsStore::manualAltitudeRangeKey(),
             QStringLiteral("Propagation_Setalt"));
    QCOMPARE(PropagationSettingsStore::showScaleKey(),
             QStringLiteral("Propagation_ShowScale"));
}

void PropagationSettingsWindowTest::
storeLoadsLegacyValuesAndAutosavesEveryKey()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("propagation.ini")),
                       QSettings::IniFormat);
    settings.setValue(PropagationSettingsStore::clearanceKey(),
                      QStringLiteral("12.5"));
    settings.setValue(PropagationSettingsStore::resolutionKey(),
                      QStringLiteral("8"));
    settings.setValue(PropagationSettingsStore::azimuthStepKey(),
                      QStringLiteral("0.5"));
    settings.setValue(PropagationSettingsStore::convergenceKey(),
                      QStringLiteral("5"));
    settings.setValue(PropagationSettingsStore::rfMapKey(),
                      QStringLiteral("True"));
    settings.sync();

    PropagationSettingsStore store(&settings);
    QCOMPARE(store.settings().clearanceMeters, 12.5);
    QCOMPARE(store.settings().resolutionPixels, 8);
    QCOMPARE(store.settings().azimuthStepDegrees, 0.5);
    QCOMPARE(store.settings().convergenceDegrees, 5.0);
    QVERIFY(store.settings().rfMap);

    QSignalSpy changed(&store,
        &PropagationSettingsStore::settingsChanged);
    PropagationSettings updated = store.settings();
    updated.clearanceMeters = 6.25;
    updated.resolutionPixels = 10;
    updated.azimuthStepDegrees = 2.0;
    updated.convergenceDegrees = 10.0;
    updated.rangeKilometers = 15.5;
    updated.baseHeightMeters = 7.5;
    updated.tolerance = 0.7;
    updated.minimumAltitude = 80.0;
    updated.maximumAltitude = 600.0;
    updated.elevationMap = true;
    updated.terrainMap = true;
    updated.rfMap = false;
    updated.homeDistance = true;
    updated.droneDistance = true;
    updated.manualAltitudeRange = true;
    updated.showScale = true;
    QVERIFY(store.save(updated));
    QCOMPARE(changed.count(), 1);
    QVERIFY(store.settings() == updated);
    QVERIFY(store.lastError().isEmpty());

    QCOMPARE(settings.value(PropagationSettingsStore::clearanceKey()).toString(),
             QStringLiteral("6.25"));
    QCOMPARE(settings.value(PropagationSettingsStore::resolutionKey()).toString(),
             QStringLiteral("10"));
    QCOMPARE(settings.value(PropagationSettingsStore::azimuthStepKey()).toString(),
             QStringLiteral("2"));
    QCOMPARE(settings.value(PropagationSettingsStore::convergenceKey()).toString(),
             QStringLiteral("10"));
    QCOMPARE(settings.value(PropagationSettingsStore::rangeKey()).toString(),
             QStringLiteral("15.5"));
    QCOMPARE(settings.value(PropagationSettingsStore::baseHeightKey()).toString(),
             QStringLiteral("7.5"));
    QCOMPARE(settings.value(PropagationSettingsStore::toleranceKey()).toString(),
             QStringLiteral("0.7"));
    QCOMPARE(settings.value(PropagationSettingsStore::minimumAltitudeKey()).toString(),
             QStringLiteral("80"));
    QCOMPARE(settings.value(PropagationSettingsStore::maximumAltitudeKey()).toString(),
             QStringLiteral("600"));
    QCOMPARE(settings.value(
        PropagationSettingsStore::elevationMapKey()).toString(),
        QStringLiteral("True"));
    QCOMPARE(settings.value(
        PropagationSettingsStore::terrainMapKey()).toString(),
        QStringLiteral("True"));
    QCOMPARE(settings.value(
        PropagationSettingsStore::rfMapKey()).toString(),
        QStringLiteral("False"));
    QCOMPARE(settings.value(
        PropagationSettingsStore::homeDistanceKey()).toString(),
        QStringLiteral("True"));
    QCOMPARE(settings.value(
        PropagationSettingsStore::droneDistanceKey()).toString(),
        QStringLiteral("True"));
    QCOMPARE(settings.value(
        PropagationSettingsStore::manualAltitudeRangeKey()).toString(),
        QStringLiteral("True"));
    QCOMPARE(settings.value(
        PropagationSettingsStore::showScaleKey()).toString(),
        QStringLiteral("True"));

    QVERIFY(store.save(updated));
    QCOMPARE(changed.count(), 1);
}

void PropagationSettingsWindowTest::rendersCompleteModelessMp10Surface()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("propagation.ini")),
                       QSettings::IniFormat);
    PropagationSettingsStore store(&settings);
    QWidget owner;
    auto *window = new PropagationSettingsWindow(store, &owner);
    QPointer<PropagationSettingsWindow> guard(window);

    QCOMPARE(window->objectName(), QStringLiteral("PropagationSettingsWindow"));
    QCOMPARE(window->windowTitle(), QStringLiteral("RF Propagation Settings"));
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->parentWidget(), &owner);
    QVERIFY(window->isWindow());
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(window->size(), QSize(PropagationSettingsWindow::WindowWidth,
                                   PropagationSettingsWindow::WindowHeight));
    QCOMPARE(window->minimumSize(),
             QSize(PropagationSettingsWindow::MinimumWindowWidth,
                   PropagationSettingsWindow::MinimumWindowHeight));
    QVERIFY(required<QScrollArea>(window,
        QStringLiteral("propagationSettingsScroll"))->widgetResizable());
    QVERIFY(required<QWidget>(window,
        QStringLiteral("propagationSettingsContent")));

    const QStringList checks = {
        QStringLiteral("propagationElevation"),
        QStringLiteral("propagationTerrain"),
        QStringLiteral("propagationRfMap"),
        QStringLiteral("propagationDroneDistance"),
        QStringLiteral("propagationHomeDistance"),
        QStringLiteral("propagationShowScale"),
        QStringLiteral("propagationAltitudeFilter")
    };
    for (const QString &name : checks) {
        QVERIFY(required<QCheckBox>(window, name));
    }
    QCOMPARE(required<QCheckBox>(window,
        QStringLiteral("propagationElevation"))->text(),
        QStringLiteral("Elevation"));
    QCOMPARE(required<QCheckBox>(window,
        QStringLiteral("propagationTerrain"))->text(),
        QStringLiteral("Terrain"));
    QCOMPARE(required<QCheckBox>(window,
        QStringLiteral("propagationRfMap"))->text(),
        QStringLiteral("RF Map"));
    QCOMPARE(required<QCheckBox>(window,
        QStringLiteral("propagationDroneDistance"))->text(),
        QStringLiteral("Drone Dist Left"));
    QCOMPARE(required<QCheckBox>(window,
        QStringLiteral("propagationHomeDistance"))->text(),
        QStringLiteral("Home Dist Left"));
    QCOMPARE(required<QCheckBox>(window,
        QStringLiteral("propagationShowScale"))->text(),
        QStringLiteral("Show Scale"));
    QCOMPARE(required<QCheckBox>(window,
        QStringLiteral("propagationAltitudeFilter"))->text(),
        QStringLiteral("Altitude Filter"));

    auto *clearance = required<QDoubleSpinBox>(
        window, QStringLiteral("propagationClearance"));
    auto *range = required<QDoubleSpinBox>(
        window, QStringLiteral("propagationRange"));
    auto *baseHeight = required<QDoubleSpinBox>(
        window, QStringLiteral("propagationBaseHeight"));
    auto *tolerance = required<QDoubleSpinBox>(
        window, QStringLiteral("propagationTolerance"));
    auto *minimumAltitude = required<QDoubleSpinBox>(
        window, QStringLiteral("propagationMinimumAltitude"));
    auto *maximumAltitude = required<QDoubleSpinBox>(
        window, QStringLiteral("propagationMaximumAltitude"));
    for (QDoubleSpinBox *number : {clearance, range, baseHeight,
                                   minimumAltitude, maximumAltitude}) {
        QCOMPARE(number->minimum(), 0.0);
        QCOMPARE(number->maximum(), 2000.0);
        QCOMPARE(number->decimals(), 1);
        QCOMPARE(number->singleStep(), 0.1);
    }
    QCOMPARE(tolerance->minimum(), 0.0);
    QCOMPARE(tolerance->maximum(), 1.0);
    QCOMPARE(tolerance->decimals(), 2);
    QCOMPARE(tolerance->singleStep(), 0.1);

    auto *resolution = required<QComboBox>(
        window, QStringLiteral("propagationResolution"));
    auto *azimuth = required<QComboBox>(
        window, QStringLiteral("propagationAzimuthStep"));
    auto *convergence = required<QComboBox>(
        window, QStringLiteral("propagationConvergence"));
    QCOMPARE(items(resolution), QStringList({QStringLiteral("2"),
        QStringLiteral("4"), QStringLiteral("6"), QStringLiteral("8"),
        QStringLiteral("10")}));
    QCOMPARE(items(azimuth), QStringList({QStringLiteral("0.5"),
        QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("5"),
        QStringLiteral("10")}));
    QCOMPARE(items(convergence), QStringList({QStringLiteral("0"),
        QStringLiteral("1"), QStringLiteral("5"), QStringLiteral("10"),
        QStringLiteral("15")}));
    QCOMPARE(resolution->currentText(), QStringLiteral("4"));
    QCOMPARE(azimuth->currentText(), QStringLiteral("1"));
    QCOMPARE(convergence->currentText(), QStringLiteral("1"));
    QCOMPARE(clearance->value(), 5.0);
    QCOMPARE(range->value(), 2.0);
    QCOMPARE(baseHeight->value(), 2.0);
    QCOMPARE(tolerance->value(), 0.8);
    QCOMPARE(minimumAltitude->value(), 100.0);
    QCOMPARE(maximumAltitude->value(), 400.0);

    const QStringList labels = {
        QStringLiteral("Clearance [m]"), QStringLiteral("Resolution"),
        QStringLiteral("Azimuth Step"), QStringLiteral("Convergance"),
        QStringLiteral("Range [Km]"), QStringLiteral("Base Height [m]"),
        QStringLiteral("Tolerance 0..1"), QStringLiteral("Min Alt [m]"),
        QStringLiteral("Max Alt [m]")
    };
    const QStringList labelNames = {
        QStringLiteral("propagationClearanceLabel"),
        QStringLiteral("propagationResolutionLabel"),
        QStringLiteral("propagationAzimuthStepLabel"),
        QStringLiteral("propagationConvergenceLabel"),
        QStringLiteral("propagationRangeLabel"),
        QStringLiteral("propagationBaseHeightLabel"),
        QStringLiteral("propagationToleranceLabel"),
        QStringLiteral("propagationMinimumAltitudeLabel"),
        QStringLiteral("propagationMaximumAltitudeLabel")
    };
    for (int index = 0; index < labels.size(); ++index) {
        QCOMPARE(required<QLabel>(window, labelNames.at(index))->text(),
                 labels.at(index));
    }
    QCOMPARE(required<QLabel>(window,
        QStringLiteral("propagationDescription"))->text(),
        QStringLiteral("Propagation overlays are displayed on Flight Data "
                       "and Flight Planner maps."));
    QCOMPARE(required<QLabel>(window,
        QStringLiteral("propagationHelp"))->text(),
        QStringLiteral("Elevation takes precedence when both Elevation and "
                       "Terrain are enabled. Missing SRTM areas are left "
                       "transparent and retried in the background."));
    QCOMPARE(required<QPushButton>(window,
        QStringLiteral("propagationClose"))->text(), QStringLiteral("Close"));
    QCOMPARE(required<QShortcut>(window,
        QStringLiteral("propagationCloseShortcut"))->key(),
        QKeySequence(Qt::CTRL | Qt::Key_W));

    window->show();
    QTRY_VERIFY(window->isVisible());
    window->close();
    QTRY_VERIFY(guard.isNull());
}

void PropagationSettingsWindowTest::
everyControlAutosavesAndSynchronizesOpenWindows()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("propagation.ini")),
                       QSettings::IniFormat);
    PropagationSettingsStore store(&settings);
    PropagationSettingsWindow first(store);
    PropagationSettingsWindow second(store);
    QSignalSpy storeChanged(&store,
        &PropagationSettingsStore::settingsChanged);
    QSignalSpy windowChanged(&second,
        &PropagationSettingsWindow::settingsChanged);

    required<QCheckBox>(&first,
        QStringLiteral("propagationElevation"))->setChecked(true);
    required<QCheckBox>(&first,
        QStringLiteral("propagationTerrain"))->setChecked(true);
    required<QCheckBox>(&first,
        QStringLiteral("propagationRfMap"))->setChecked(true);
    required<QCheckBox>(&first,
        QStringLiteral("propagationDroneDistance"))->setChecked(true);
    required<QCheckBox>(&first,
        QStringLiteral("propagationHomeDistance"))->setChecked(true);
    required<QCheckBox>(&first,
        QStringLiteral("propagationShowScale"))->setChecked(true);
    required<QCheckBox>(&first,
        QStringLiteral("propagationAltitudeFilter"))->setChecked(true);
    required<QDoubleSpinBox>(&first,
        QStringLiteral("propagationClearance"))->setValue(12.3);
    required<QComboBox>(&first,
        QStringLiteral("propagationResolution"))->setCurrentIndex(4);
    required<QComboBox>(&first,
        QStringLiteral("propagationAzimuthStep"))->setCurrentIndex(4);
    required<QComboBox>(&first,
        QStringLiteral("propagationConvergence"))->setCurrentIndex(4);
    required<QDoubleSpinBox>(&first,
        QStringLiteral("propagationRange"))->setValue(15.4);
    required<QDoubleSpinBox>(&first,
        QStringLiteral("propagationBaseHeight"))->setValue(3.2);
    required<QDoubleSpinBox>(&first,
        QStringLiteral("propagationTolerance"))->setValue(0.7);
    required<QDoubleSpinBox>(&first,
        QStringLiteral("propagationMinimumAltitude"))->setValue(123.4);
    required<QDoubleSpinBox>(&first,
        QStringLiteral("propagationMaximumAltitude"))->setValue(567.8);

    QCOMPARE(storeChanged.count(), 16);
    QCOMPARE(windowChanged.count(), 16);
    const PropagationSettings current = store.settings();
    QVERIFY(current.elevationMap);
    QVERIFY(current.terrainMap);
    QVERIFY(current.rfMap);
    QVERIFY(current.droneDistance);
    QVERIFY(current.homeDistance);
    QVERIFY(current.showScale);
    QVERIFY(current.manualAltitudeRange);
    QCOMPARE(current.clearanceMeters, 12.3);
    QCOMPARE(current.resolutionPixels, 10);
    QCOMPARE(current.azimuthStepDegrees, 10.0);
    QCOMPARE(current.convergenceDegrees, 15.0);
    QCOMPARE(current.rangeKilometers, 15.4);
    QCOMPARE(current.baseHeightMeters, 3.2);
    QCOMPARE(current.tolerance, 0.7);
    QCOMPARE(current.minimumAltitude, 123.4);
    QCOMPARE(current.maximumAltitude, 567.8);

    QCOMPARE(required<QDoubleSpinBox>(&second,
        QStringLiteral("propagationClearance"))->value(), 12.3);
    QCOMPARE(required<QComboBox>(&second,
        QStringLiteral("propagationResolution"))->currentText(),
        QStringLiteral("10"));
    QVERIFY(required<QCheckBox>(&second,
        QStringLiteral("propagationHomeDistance"))->isChecked());
    QCOMPARE(settings.value(
        PropagationSettingsStore::maximumAltitudeKey()).toString(),
        QStringLiteral("567.8"));
}

void PropagationSettingsWindowTest::closeButtonAndCtrlWCloseTheWindow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("propagation.ini")),
                       QSettings::IniFormat);
    PropagationSettingsStore store(&settings);

    auto *buttonWindow = new PropagationSettingsWindow(store);
    QPointer<PropagationSettingsWindow> buttonGuard(buttonWindow);
    buttonWindow->show();
    required<QPushButton>(buttonWindow,
        QStringLiteral("propagationClose"))->click();
    QTRY_VERIFY(buttonGuard.isNull());

    auto *shortcutWindow = new PropagationSettingsWindow(store);
    QPointer<PropagationSettingsWindow> shortcutGuard(shortcutWindow);
    shortcutWindow->show();
    auto *shortcut = required<QShortcut>(shortcutWindow,
        QStringLiteral("propagationCloseShortcut"));
    QVERIFY(QMetaObject::invokeMethod(shortcut, "activated",
                                      Qt::DirectConnection));
    QTRY_VERIFY(shortcutGuard.isNull());
}

QTEST_MAIN(PropagationSettingsWindowTest)

#include "test_propagationsettingswindow.moc"
