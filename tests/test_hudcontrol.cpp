#include "HudControl.h"
#include "services/HudDisplaySettings.h"

#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>

#include <cmath>

namespace {
QImage renderHud(HudControl &hud)
{
    QImage image(hud.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter painter(&image);
    hud.render(&painter);
    painter.end();
    return image;
}
}

class HudControlTest final : public QObject
{
    Q_OBJECT

private slots:
    void displaySettingsDefaultWithoutConstructorWrite();
    void displaySettingsPersistAndReloadActualChanges();
    void sharesInjectedOverlaySettingsLive();
    void exposesMissionPlannerTelemetryProperties();
    void acceptsACompleteTelemetrySnapshot();
    void rendersAttitudeAndInstruments();
    void aoaDoesNotDarkenTranslucentSideTapes();
    void preservesSelectableAspectRatio();
};

void HudControlTest::displaySettingsDefaultWithoutConstructorWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("hud.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("FlightData/Hud/OverlayEnabled"), false);
    const QStringList keysBeforeConstruction = settings.allKeys();

    HudDisplaySettings displaySettings(&settings);

    QVERIFY(displaySettings.overlayEnabled());
    QVERIFY(!settings.contains(QStringLiteral("CHK_hudshow")));
    QCOMPARE(settings.allKeys(), keysBeforeConstruction);
}

void HudControlTest::displaySettingsPersistAndReloadActualChanges()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("hud.ini")),
                       QSettings::IniFormat);
    HudDisplaySettings displaySettings(&settings);
    QSignalSpy changed(&displaySettings,
                       &HudDisplaySettings::overlayEnabledChanged);

    displaySettings.setOverlayEnabled(false);
    QCOMPARE(settings.value(QStringLiteral("CHK_hudshow")).toBool(), false);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toBool(), false);

    displaySettings.setOverlayEnabled(false);
    displaySettings.reload();
    QCOMPARE(changed.count(), 1);

    settings.setValue(QStringLiteral("CHK_hudshow"), true);
    displaySettings.reload();
    QVERIFY(displaySettings.overlayEnabled());
    QCOMPARE(changed.count(), 2);
    QCOMPARE(changed.at(1).at(0).toBool(), true);
}

void HudControlTest::sharesInjectedOverlaySettingsLive()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("hud.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("CHK_hudshow"), false);
    HudDisplaySettings displaySettings(&settings);
    HudControl hud(nullptr, &displaySettings);
    QSignalSpy changed(&hud, &HudControl::telemetryChanged);

    QVERIFY(!hud.overlayEnabled());
    displaySettings.setOverlayEnabled(true);
    QVERIFY(hud.overlayEnabled());
    QCOMPARE(changed.count(), 1);

    hud.setOverlayEnabled(false);
    QVERIFY(!displaySettings.overlayEnabled());
    QCOMPARE(settings.value(QStringLiteral("CHK_hudshow")).toBool(), false);
    QCOMPARE(changed.count(), 2);
}

void HudControlTest::exposesMissionPlannerTelemetryProperties()
{
    HudControl hud;
    const QMetaObject *meta = hud.metaObject();
    const QStringList properties = {
        QStringLiteral("Roll"), QStringLiteral("Pitch"), QStringLiteral("Yaw"),
        QStringLiteral("Alt"), QStringLiteral("AirSpeed"), QStringLiteral("GroundSpeed"),
        QStringLiteral("VerticalSpeed"), QStringLiteral("SatCount"),
        QStringLiteral("GpsFixType"), QStringLiteral("Armed"),
        QStringLiteral("PrearmOk"), QStringLiteral("Mode"),
        QStringLiteral("BatteryVoltage"), QStringLiteral("BatteryRemaining"),
        QStringLiteral("NavBearing"), QStringLiteral("XTrackError"),
        QStringLiteral("TurnRate"), QStringLiteral("WpDist"),
        QStringLiteral("WpNo"), QStringLiteral("ThrottlePercent"),
        QStringLiteral("Failsafe"), QStringLiteral("SafetyActive"),
        QStringLiteral("LinkQuality")
    };
    for (const QString &property : properties) {
        QVERIFY2(meta->indexOfProperty(property.toLatin1().constData()) >= 0,
                 property.toLatin1().constData());
    }
}

void HudControlTest::acceptsACompleteTelemetrySnapshot()
{
    HudControl hud;
    QSignalSpy changed(&hud, &HudControl::telemetryChanged);
    hud.setRoll(21.5);
    hud.setPitch(-7.25);
    hud.setYaw(274.0);
    hud.setAlt(123.4);
    hud.setAirSpeed(18.2);
    hud.setGroundSpeed(16.8);
    hud.setVerticalSpeed(2.3);
    hud.setGpsFixType(3);
    hud.setSatCount(17);
    hud.setArmed(true);
    hud.setMode(QStringLiteral("AUTO"));
    hud.setBatteryVoltage(15.74);
    hud.setBatteryRemaining(72);
    hud.setCurrentAmps(8.4);
    hud.setWpDist(1450);
    hud.setWpNo(7);
    hud.setLinkQuality(93);

    QCOMPARE(hud.roll(), 21.5);
    QCOMPARE(hud.pitch(), -7.25);
    QCOMPARE(hud.yaw(), 274.0);
    QCOMPARE(hud.mode(), QStringLiteral("AUTO"));
    QCOMPARE(hud.gpsFixType(), 3);
    QCOMPARE(hud.satCount(), 17.0);
    QCOMPARE(hud.wpNo(), 7);
    QCOMPARE(hud.linkQuality(), 93.0);
    QVERIFY(changed.count() >= 17);
}

void HudControlTest::rendersAttitudeAndInstruments()
{
    HudControl hud;
    hud.resize(640, 480);
    hud.setOverlayEnabled(true);
    hud.setGroundBrown(false);
    hud.setRoll(14);
    hud.setPitch(8);
    hud.setYaw(82);
    hud.setAlt(115);
    hud.setGroundSpeed(18);
    hud.setVerticalSpeed(1.8);
    hud.setGpsFixType(3);
    hud.setSatCount(14);
    hud.setBatteryVoltage(15.8);
    hud.setBatteryRemaining(78);
    hud.setMode(QStringLiteral("LOITER"));
    hud.setLinkQuality(96);
    hud.snapToValues();

    QImage image(hud.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter painter(&image);
    hud.render(&painter);
    painter.end();

    int skyPixels = 0;
    int groundPixels = 0;
    int brightPixels = 0;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.blue() > pixel.red() + 25) ++skyPixels;
            if (pixel.green() > pixel.blue() + 15) ++groundPixels;
            if (pixel.red() > 200 && pixel.green() > 200 && pixel.blue() > 200) ++brightPixels;
        }
    }
    QVERIFY(skyPixels > 1000);
    QVERIFY(groundPixels > 1000);
    QVERIFY(brightPixels > 50);
}

void HudControlTest::aoaDoesNotDarkenTranslucentSideTapes()
{
    HudControl hud;
    hud.resize(640, 480);
    hud.setOverlayEnabled(true);
    hud.setDisplayHeading(false);
    hud.setDisplayXTrack(false);
    hud.setDisplaySpeed(true);
    hud.setDisplayAlt(true);
    hud.setGroundSpeed(18.0);
    hud.setAlt(120.0);
    hud.setAoa(12.5);
    hud.setSsa(-1.5);
    hud.snapToValues();

    hud.setDisplayAoa(false);
    const QImage withoutAoa = renderHud(hud);
    hud.setDisplayAoa(true);
    const QImage withAoa = renderHud(hud);

    const QRect viewport = hud.contentViewport();
    const double unit = qMin(viewport.width(), viewport.height());
    const int tapeWidth = static_cast<int>(std::floor(qMax(
        20.0, qMin(qMax(unit * 0.12, 42.0), viewport.width() * 0.16))));
    const int tapeTop = viewport.top() + viewport.height() / 4;
    const int tapeBottom = viewport.top() + viewport.height() * 3 / 4;
    int changedPixels = 0;
    for (int y = tapeTop + 2; y < tapeBottom - 2; ++y) {
        for (int x = viewport.left() + 2;
             x < viewport.left() + tapeWidth - 2; ++x) {
            if (withoutAoa.pixel(x, y) != withAoa.pixel(x, y)) {
                ++changedPixels;
            }
        }
    }
    QCOMPARE(changedPixels, 0);
}

void HudControlTest::preservesSelectableAspectRatio()
{
    HudControl hud;
    hud.resize(1000, 600);
    hud.setSixteenByNine(false);
    QCOMPARE(hud.contentViewport(), QRect(100, 0, 800, 600));
    hud.setSixteenByNine(true);
    const QRect wide = hud.contentViewport();
    QCOMPARE(wide.width(), 1000);
    QVERIFY(wide.height() == 562 || wide.height() == 563);
    QVERIFY(qAbs(static_cast<double>(wide.width()) / wide.height() - 16.0 / 9.0) < 0.01);
}

QTEST_MAIN(HudControlTest)
#include "test_hudcontrol.moc"
