#include <QtTest>

#include "ui/Terrain3DWindow.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>

class Terrain3DWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void uiInventoryAndDefaultsMatchMp10();
    void syntheticDemRendersAndPointerIsInspectable();
    void changedMeshSettingsRequireReloadAndKeyboardUnlocksCamera();
    void invalidGpsIsExplicitAndDoesNotStartDemWork();
    void staleTargetEpochResultIsNotPublished();
    void cooperativeWorkerIsCancelledOnClose();
    void uncooperativeWorkerDoesNotBlockClose();
};

namespace
{
Terrain3DCore::Snapshot validSnapshot()
{
    Terrain3DCore::Snapshot snapshot;
    snapshot.vehicle = {35.1856, 33.3823, 130.0};
    snapshot.relativeAltitudeM = 30.0;
    snapshot.pitchDeg = -25.0;
    snapshot.yawDeg = 0.0;
    snapshot.linkId = 7;
    snapshot.targetGeneration = 3;
    snapshot.capturedMonotonicMs = 1000;
    snapshot.mode = QStringLiteral("LOITER");
    snapshot.systemId = 1;
    snapshot.componentId = 1;
    return snapshot;
}

Terrain3DWindow::Dependencies successfulDependencies(
    const std::shared_ptr<Terrain3DCore::Snapshot> &snapshot = {})
{
    const auto state = snapshot
        ? snapshot : std::make_shared<Terrain3DCore::Snapshot>(validSnapshot());
    Terrain3DWindow::Dependencies dependencies;
    dependencies.snapshot = [state]() { return *state; };
    dependencies.monotonicClock = []() { return qint64(1000); };
    dependencies.elevation = [](double latitude, double longitude) {
        return 100.0 + (latitude - 35.1856) * 2000.0
            + (longitude - 33.3823) * 500.0;
    };
    return dependencies;
}

bool imageHasVariation(const QImage &image)
{
    if (image.isNull()) {
        return false;
    }
    const QRgb first = image.pixel(0, 0);
    for (int y = 0; y < image.height(); y += std::max(1, image.height() / 20)) {
        for (int x = 0; x < image.width(); x += std::max(1, image.width() / 20)) {
            if (image.pixel(x, y) != first) {
                return true;
            }
        }
    }
    return false;
}
}

void Terrain3DWindowTest::uiInventoryAndDefaultsMatchMp10()
{
    Terrain3DWindow window(successfulDependencies());
    QCOMPARE(window.objectName(), QStringLiteral("Terrain3DWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("3D Terrain View"));
    QCOMPARE(window.size(), QSize(1100, 760));
    QCOMPARE(window.minimumSize(), QSize(720, 520));

    auto *lock = window.findChild<QCheckBox *>(
        QStringLiteral("LockToVehicle"));
    auto *fog = window.findChild<QCheckBox *>(QStringLiteral("FogEnabled"));
    auto *imagery = window.findChild<QCheckBox *>(
        QStringLiteral("ImageryEnabled"));
    QVERIFY(lock && lock->isChecked());
    QVERIFY(fog && fog->isChecked());
    QVERIFY(imagery && !imagery->isChecked());
    QVERIFY(!imagery->isEnabled());
    QVERIFY(imagery->toolTip().contains(QStringLiteral("not available")));

    auto *range = window.findChild<QSpinBox *>(QStringLiteral("RangeM"));
    auto *grid = window.findChild<QSpinBox *>(QStringLiteral("GridSize"));
    auto *minimumZoom = window.findChild<QSpinBox *>(
        QStringLiteral("TextureMinZoom"));
    auto *maximumZoom = window.findChild<QSpinBox *>(
        QStringLiteral("TextureMaxZoom"));
    auto *vertical = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("VerticalExaggeration"));
    QVERIFY(range && grid && minimumZoom && maximumZoom && vertical);
    QCOMPARE(range->minimum(), 250);
    QCOMPARE(range->maximum(), 5000);
    QCOMPARE(range->singleStep(), 250);
    QCOMPARE(range->value(), 1500);
    QCOMPARE(grid->minimum(), 17);
    QCOMPARE(grid->maximum(), 65);
    QCOMPARE(grid->singleStep(), 8);
    QCOMPARE(grid->value(), 33);
    QCOMPARE(minimumZoom->value(), 12);
    QCOMPARE(maximumZoom->value(), 20);
    QVERIFY(!minimumZoom->isEnabled());
    QVERIFY(!maximumZoom->isEnabled());
    QCOMPARE(vertical->minimum(), 0.25);
    QCOMPARE(vertical->maximum(), 8.0);
    QCOMPARE(vertical->singleStep(), 0.25);
    QCOMPARE(vertical->value(), 1.0);

    QVERIFY(window.findChild<QPushButton *>(QStringLiteral("ReloadTerrain")));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("TerrainImage")));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("TerrainStatus")));
    QVERIFY(window.findChild<QLabel *>(
        QStringLiteral("TerrainPointerStatus")));
    const QLabel *limitations = window.findChild<QLabel *>(
        QStringLiteral("TerrainLimitations"));
    QVERIFY(limitations);
    QVERIFY(limitations->text().contains(QStringLiteral("guided commands")));
}

void Terrain3DWindowTest::syntheticDemRendersAndPointerIsInspectable()
{
    Terrain3DWindow window(successfulDependencies());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && !window.frame().isNull(),
                             5000);
    QVERIFY(imageHasVariation(window.frame()));
    QVERIFY(window.detailsText().contains(QStringLiteral("33×33 mesh")));
    QVERIFY(window.detailsText().contains(QStringLiteral("elevation shading")));
    QCOMPARE(window.statusText(),
             QStringLiteral("Terrain ready. Move over the view to inspect coordinates."));

    QLabel *image = window.findChild<QLabel *>(QStringLiteral("TerrainImage"));
    QVERIFY(image);
    QTest::mouseMove(image, image->rect().center());
    QTRY_VERIFY_WITH_TIMEOUT(
        window.pointerText().contains(QStringLiteral("terrain")), 1000);
    QVERIFY(window.pointerText().contains(QStringLiteral("AMSL")));

    QTest::mouseClick(image, Qt::LeftButton, Qt::NoModifier,
                      image->rect().center());
    QVERIFY(window.statusText().contains(QStringLiteral("Read-only")));
    QVERIFY(window.statusText().contains(QStringLiteral("disabled")));
}

void Terrain3DWindowTest::changedMeshSettingsRequireReloadAndKeyboardUnlocksCamera()
{
    Terrain3DWindow window(successfulDependencies());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && !window.frame().isNull(),
                             5000);

    auto *range = window.findChild<QSpinBox *>(QStringLiteral("RangeM"));
    auto *reload = window.findChild<QPushButton *>(
        QStringLiteral("ReloadTerrain"));
    QVERIFY(range && reload);
    range->setValue(1750);
    QCOMPARE(window.statusText(),
             QStringLiteral("Terrain settings changed; select Reload terrain to apply them."));
    reload->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 5000);
    QVERIFY(window.detailsText().contains(QStringLiteral("33×33 mesh")));

    QCheckBox *lock = window.findChild<QCheckBox *>(
        QStringLiteral("LockToVehicle"));
    QVERIFY(lock && lock->isChecked());
    const Terrain3DCore::Camera before = window.currentCamera();
    window.setFocus(Qt::OtherFocusReason);
    QTest::keyClick(&window, Qt::Key_W);
    QVERIFY(!lock->isChecked());
    const Terrain3DCore::Camera moved = window.currentCamera();
    QVERIFY(std::abs(moved.eastM - before.eastM) > 0.1
            || std::abs(moved.northM - before.northM) > 0.1);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 5000);
    QVERIFY(window.detailsText().contains(QStringLiteral("free camera")));
}

void Terrain3DWindowTest::invalidGpsIsExplicitAndDoesNotStartDemWork()
{
    std::atomic_int elevationCalls(0);
    Terrain3DWindow::Dependencies dependencies;
    dependencies.snapshot = []() {
        Terrain3DCore::Snapshot snapshot;
        snapshot.vehicle = {0.0, 0.0, 100.0};
        return snapshot;
    };
    dependencies.monotonicClock = []() { return qint64(10); };
    dependencies.elevation = [&elevationCalls](double, double) {
        ++elevationCalls;
        return 0.0;
    };

    Terrain3DWindow window(dependencies);
    QTest::qWait(250);
    QVERIFY(!window.isBusy());
    QVERIFY(window.frame().isNull());
    QCOMPARE(elevationCalls.load(), 0);
    QCOMPARE(window.statusText(),
             QStringLiteral("Waiting for a valid vehicle GPS position."));
}

void Terrain3DWindowTest::staleTargetEpochResultIsNotPublished()
{
    auto snapshot =
        std::make_shared<Terrain3DCore::Snapshot>(validSnapshot());
    std::atomic_bool entered(false);
    std::atomic_bool release(false);
    Terrain3DWindow::Dependencies dependencies =
        successfulDependencies(snapshot);
    dependencies.elevation = [&entered, &release](double, double) {
        if (!entered.exchange(true)) {
            while (!release.load()) {
                QThread::msleep(1);
            }
        }
        return 100.0;
    };

    Terrain3DWindow window(dependencies);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
    *snapshot = Terrain3DCore::Snapshot{};
    release.store(true);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        window.statusText(),
        QStringLiteral("Waiting for a valid vehicle GPS position."), 2000);
    QVERIFY(window.frame().isNull());
}

void Terrain3DWindowTest::cooperativeWorkerIsCancelledOnClose()
{
    std::atomic_int samples(0);
    Terrain3DWindow::Dependencies dependencies = successfulDependencies();
    dependencies.elevation = [&samples](double, double) {
        ++samples;
        QThread::msleep(3);
        return 100.0;
    };

    auto *window = new Terrain3DWindow(dependencies);
    window->show();
    QTRY_VERIFY_WITH_TIMEOUT(samples.load() > 0, 3000);
    QElapsedTimer timer;
    timer.start();
    QPointer<Terrain3DWindow> guarded(window);
    window->close();
    QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 700);
    QVERIFY(timer.elapsed() < 700);
    QVERIFY(samples.load() < 33 * 33);
}

void Terrain3DWindowTest::uncooperativeWorkerDoesNotBlockClose()
{
    const auto entered = std::make_shared<std::atomic_bool>(false);
    const auto completed = std::make_shared<std::atomic_bool>(false);
    Terrain3DWindow::Dependencies dependencies = successfulDependencies();
    dependencies.elevation = [entered, completed](double, double) {
        if (!entered->exchange(true)) {
            QThread::msleep(800);
            completed->store(true);
        }
        return 100.0;
    };

    auto *window = new Terrain3DWindow(dependencies);
    window->show();
    QTRY_VERIFY_WITH_TIMEOUT(entered->load(), 3000);
    QElapsedTimer timer;
    timer.start();
    QPointer<Terrain3DWindow> guarded(window);
    window->close();
    QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 700);
    QVERIFY(timer.elapsed() < 700);
    QTRY_VERIFY_WITH_TIMEOUT(completed->load(), 2000);
}

QTEST_MAIN(Terrain3DWindowTest)
#include "test_terrain3dwindow.moc"
