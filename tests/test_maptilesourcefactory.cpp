#include <QtTest>

#include "ui/configuration/ElevationSourceService.h"
#include "ui/map/MapTileSourceFactory.h"
#include "ui/map/NativeGdalMapService.h"

#include <QImage>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace {
class FakeElevationSourceService final : public ElevationSourceService
{
public:
    bool isNativeGdalAvailable() const override { return nativeAvailable; }
    QByteArray renderRasterTile(int tileX, int tileY, int zoom,
                                int tileSize) const override
    {
        lastX = tileX;
        lastY = tileY;
        lastZoom = zoom;
        lastTileSize = tileSize;
        ++renderCalls;
        return renderedTile;
    }
    void initializeFromSettings() override {}
    void shutdown() override {}

    bool nativeAvailable = false;
    QByteArray renderedTile;
    mutable int renderCalls = 0;
    mutable int lastX = -1;
    mutable int lastY = -1;
    mutable int lastZoom = -1;
    mutable int lastTileSize = -1;
};
}

class MapTileSourceFactoryTest final : public QObject
{
    Q_OBJECT

private slots:
    void normalizationUsesExactMissionPlannerFallback();
    void migratesLegacyIntegerToOneRootString();
    void preservesUnknownFutureProvider();
    void gdalSelectionRequiresConfiguration();
    void configuredGdalSelectionIsGlobalAndRefreshable();
    void nativeProviderReturnsTransparentTileOutsideCoverage();
};

void MapTileSourceFactoryTest::normalizationUsesExactMissionPlannerFallback()
{
    QCOMPARE(MapTileSourceFactory::SettingsName(
                 core::MapType::GoogleSatellite),
             QStringLiteral("GoogleSatelliteMap"));
    QCOMPARE(MapTileSourceFactory::SettingsName(
                 core::MapType::ArcGIS_Satellite),
             QStringLiteral("EsriWorldImagery"));
    QCOMPARE(MapTileSourceFactory::MapTypeFromSettingsName(
                 QStringLiteral("GoogleHybridMap")),
             core::MapType::GoogleHybrid);
    QCOMPARE(MapTileSourceFactory::MapTypeFromSettingsName(
                 QStringLiteral("GDAL Custom")),
             core::MapType::GDALCustom);

    QString status;
    QCOMPARE(MapTileSourceFactory::NormalizeMapType(
                 core::MapType::GDALCustom, false, &status),
             core::MapType::GoogleSatellite);
    QCOMPARE(status, QStringLiteral(
        "Configure GDAL Custom before selecting it."));
    QCOMPARE(MapTileSourceFactory::NormalizeMapType(
                 core::MapType::GDALCustom, true, &status),
             core::MapType::GDALCustom);
    QVERIFY(status.isEmpty());
}

void MapTileSourceFactoryTest::migratesLegacyIntegerToOneRootString()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("QGC_MAPWIDGET/MAP_TYPE"),
                      static_cast<int>(core::MapType::GoogleHybrid));
    FakeElevationSourceService service;

    MapTileSourceFactory factory(&service, &settings);
    QCOMPARE(factory.CurrentMapType(), core::MapType::GoogleHybrid);
    QCOMPARE(settings.value(QStringLiteral("MapType")).toString(),
             QStringLiteral("GoogleHybridMap"));
    QVERIFY(!settings.contains(QStringLiteral("QGC_MAPWIDGET/MAP_TYPE")));
}

void MapTileSourceFactoryTest::preservesUnknownFutureProvider()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("MapType"),
                      QStringLiteral("Future Map Backend"));
    FakeElevationSourceService service;

    MapTileSourceFactory factory(&service, &settings);
    QCOMPARE(factory.CurrentMapType(), core::MapType::GoogleSatellite);
    QCOMPARE(settings.value(QStringLiteral("MapType")).toString(),
             QStringLiteral("Future Map Backend"));
}

void MapTileSourceFactoryTest::gdalSelectionRequiresConfiguration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("MapType"),
                      QStringLiteral("GDAL Custom"));
    settings.setValue(QStringLiteral("GDALImageDir"), directory.path());
    FakeElevationSourceService service;

    MapTileSourceFactory factory(&service, &settings);
    QSignalSpy changed(&factory, &MapTileSourceFactory::MapTypeChanged);
    QCOMPARE(factory.CurrentMapType(), core::MapType::GoogleSatellite);
    QCOMPARE(factory.LastStatus(), QStringLiteral(
        "Configure GDAL Custom before selecting it."));
    QCOMPARE(settings.value(QStringLiteral("MapType")).toString(),
             QStringLiteral("GoogleSatelliteMap"));

    QSignalSpy status(&factory, &MapTileSourceFactory::StatusMessage);
    QVERIFY(!factory.SetMapType(core::MapType::GDALCustom));
    QCOMPARE(status.count(), 1);
    QCOMPARE(status.constFirst().constFirst().toString(),
             QStringLiteral("Configure GDAL Custom before selecting it."));
    QCOMPARE(changed.count(), 0);
}

void MapTileSourceFactoryTest::configuredGdalSelectionIsGlobalAndRefreshable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("MapType"),
                      QStringLiteral("GoogleSatelliteMap"));
    settings.setValue(QStringLiteral("GDALImageDir"), directory.path());
    FakeElevationSourceService service;
    service.nativeAvailable = true;
    MapTileSourceFactory factory(&service, &settings);
    QSignalSpy changed(&factory, &MapTileSourceFactory::MapTypeChanged);
    QSignalSpy refreshed(&factory,
                         &MapTileSourceFactory::MapRefreshRequested);

    QVERIFY(factory.SetMapType(core::MapType::GDALCustom));
    QCOMPARE(factory.CurrentMapType(), core::MapType::GDALCustom);
    QCOMPARE(settings.value(QStringLiteral("MapType")).toString(),
             QStringLiteral("GDAL Custom"));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.constFirst().constFirst().toInt(),
             static_cast<int>(core::MapType::GDALCustom));
    QVERIFY(factory.SetMapType(core::MapType::GDALCustom));
    QCOMPARE(changed.count(), 1);

    emit service.nativeRastersChanged();
    QCOMPARE(refreshed.count(), 1);
}

void MapTileSourceFactoryTest::nativeProviderReturnsTransparentTileOutsideCoverage()
{
    FakeElevationSourceService service;
    core::OPMaps::Instance()->invalidateLocalTiles(core::MapType::GDALCustom);
    {
        NativeGdalMapService provider(&service);
        const QByteArray transparent = core::OPMaps::Instance()->GetImageFrom(
            core::MapType::GDALCustom, core::Point(3, 4), 5);
        QVERIFY(!transparent.isEmpty());
        const QImage image = QImage::fromData(transparent, "PNG")
            .convertToFormat(QImage::Format_RGBA8888);
        QVERIFY(!image.isNull());
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                QCOMPARE(qAlpha(image.pixel(x, y)), 0);
            }
        }
        QCOMPARE(service.renderCalls, 1);
        QCOMPARE(service.lastX, 3);
        QCOMPARE(service.lastY, 4);
        QCOMPARE(service.lastZoom, 5);
        QCOMPARE(service.lastTileSize, 256);
        QCOMPARE(core::OPMaps::Instance()->GetImageFrom(
                     core::MapType::GDALCustom, core::Point(3, 4), 5),
                 transparent);
        QCOMPARE(service.renderCalls, 1);
    }
    core::OPMaps::Instance()->invalidateLocalTiles(core::MapType::GDALCustom);
}

QTEST_APPLESS_MAIN(MapTileSourceFactoryTest)

#include "test_maptilesourcefactory.moc"
