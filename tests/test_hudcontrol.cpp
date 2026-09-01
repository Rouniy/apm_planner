#include "HudControl.h"

#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>

class HudControlTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesMissionPlannerTelemetryProperties();
    void acceptsACompleteTelemetrySnapshot();
    void rendersAttitudeAndInstruments();
    void preservesSelectableAspectRatio();
};

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
