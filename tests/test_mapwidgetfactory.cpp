#include <QtTest>

#include "ui/map/AbstractMapWidget.h"
#include "ui/map/MapWidgetFactory.h"
#include "pureimagecache.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWidget>

namespace {
class FakeMapWidget final : public AbstractMapWidget
{
public:
    FakeMapWidget(const QString &backendId, QWidget *widgetParent,
                  QObject *parent, bool createWidget = true)
        : AbstractMapWidget(parent),
          m_backendId(backendId),
          m_widget(createWidget ? new QWidget(widgetParent) : nullptr)
    {
    }

    QString BackendId() const override { return m_backendId; }
    QString SharedCacheRoot() const override
    {
        return core::PureImageCache::sharedCacheRoot();
    }
    QWidget *Widget() const override { return m_widget; }
    int MinZoom() const override { return 1; }
    int MaxZoom() const override { return 21; }
    double ZoomReal() const override { return 7.0; }
    int CurrentZoomLevel() const override { return 7; }
    MapGeoBounds VisibleTileExtent() const override { return {}; }
    MapCoordinate CurrentPosition() const override { return {}; }
    core::MapType::Types CurrentMapType() const override
    {
        return core::MapType::GoogleSatellite;
    }
    bool FollowUAVEnabled() const override { return false; }
    float UpdateRateLimit() const override { return 0.5f; }
    int TrailType() const override { return 0; }
    float TrailInterval() const override { return 0.0f; }
    void SetZoom(double) override {}
    void SetCurrentPosition(double, double) override {}
    void SetAcceleratedRenderingEnabled(bool) override {}
    void SetFollowUAVEnabled(bool) override {}
    void SetTrailModeTimed(int) override {}
    void SetTrailModeDistance(int) override {}
    void DeleteTrails() override {}
    void SetUpdateRateLimit(float) override {}
    void ShowGoToDialog() override {}
    void GoHome() override {}
    void LastPosition() override {}
    void CacheVisibleRegion() override {}
    void UpdateHomePosition(double, double, double) override {}
    void SetMissionPlanningEnabled(bool) override {}
    void SetPlannerRows(
        const QVector<WpRowData> &,
        FlightPlannerMissionModel::MissionStore) override {}
    void SetPlannerAltitudePresentation(double, const QString &) override {}
    void SetPlannerHome(double, double, double) override {}
    void ClearPlannerHome() override {}
    void SetPlannerSelection(int) override {}
    void SetLogTrail(const QVector<MapCoordinate> &) override {}
    void SetLogCursor(const MapCoordinate &, double) override {}

private:
    QString m_backendId;
    QWidget *m_widget = nullptr;
};

MapWidgetFactory::Creator fakeCreator(const QString &id)
{
    return [id](MapWidgetRole, const QString &, QWidget *widgetParent,
                QObject *owner) {
        return new FakeMapWidget(id, widgetParent, owner);
    };
}
} // namespace

class MapWidgetFactoryTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultAndAliasesUseCanonicalBackendId();
    void registryRequiresTheSharedCacheContract();
    void unavailableSelectionIsPreservedAndFallsBack();
    void selectionPersistsAndCreatesRegisteredBackend();
    void effectiveBackendStaysFixedUntilRestart();
    void invalidAdapterFallsBackAndUsesWidgetAsOwner();
    void constructorFailureFallsBackToOPMapControl();
};

void MapWidgetFactoryTest::defaultAndAliasesUseCanonicalBackendId()
{
    const MapGeoBounds wrapped{179.0, 10.0, -179.0, -10.0};
    QVERIFY(wrapped.IsValid());
    QVERIFY(wrapped.CrossesDateLine());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("MapWidgetBackend"),
                      QStringLiteral("QGCMapWidget"));

    MapWidgetFactory factory(&settings);
    QCOMPARE(factory.RequestedBackend(),
             QStringLiteral("OPMapControl"));
    QCOMPARE(settings.value(QStringLiteral("MapWidgetBackend")).toString(),
             QStringLiteral("OPMapControl"));
}

void MapWidgetFactoryTest::registryRequiresTheSharedCacheContract()
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    MapWidgetFactory factory(&settings);
    QSignalSpy available(&factory,
                         &MapWidgetFactory::AvailableBackendsChanged);

    QVERIFY(!factory.RegisterBackend(
        QStringLiteral("Wrong"), QStringLiteral("Wrong"),
        QStringLiteral("another-cache"), fakeCreator(QStringLiteral("Wrong"))));
    QVERIFY(!factory.RegisterBackend(
        QString(), QStringLiteral("Empty"),
        QString::fromLatin1(MapWidgetFactory::SharedCacheContract),
        fakeCreator(QStringLiteral("Empty"))));
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("OPMap"), QStringLiteral("OPMapControl"),
        QString::fromLatin1(MapWidgetFactory::SharedCacheContract),
        fakeCreator(QStringLiteral("OPMapControl"))));
    QCOMPARE(available.count(), 1);
    QCOMPARE(factory.AvailableBackends().size(), 1);
    QCOMPARE(factory.AvailableBackends().constFirst().id,
             QStringLiteral("OPMapControl"));
    QCOMPARE(factory.SharedCacheRoot(),
             core::PureImageCache::sharedCacheRoot());
}

void MapWidgetFactoryTest::unavailableSelectionIsPreservedAndFallsBack()
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("MapWidgetBackend"),
                      QStringLiteral("FutureRenderer"));
    MapWidgetFactory factory(&settings);
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("OPMapControl"), QStringLiteral("OPMapControl"),
        QString::fromLatin1(MapWidgetFactory::SharedCacheContract),
        fakeCreator(QStringLiteral("OPMapControl"))));

    QCOMPARE(factory.RequestedBackend(), QStringLiteral("FutureRenderer"));
    QCOMPARE(factory.CurrentBackend(), QStringLiteral("OPMapControl"));
    QCOMPARE(settings.value(QStringLiteral("MapWidgetBackend")).toString(),
             QStringLiteral("FutureRenderer"));
    QSignalSpy status(&factory, &MapWidgetFactory::StatusMessage);
    QWidget host;
    QScopedPointer<AbstractMapWidget> map(
        factory.CreateMapWidget(MapWidgetRole::FlightPlanner, &host));
    QVERIFY(map);
    QCOMPARE(map->BackendId(), QStringLiteral("OPMapControl"));
    QCOMPARE(status.count(), 1);
    QVERIFY(factory.LastStatus().contains(QStringLiteral("FutureRenderer")));
}

void MapWidgetFactoryTest::selectionPersistsAndCreatesRegisteredBackend()
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    MapWidgetFactory factory(&settings);
    const QString contract = QString::fromLatin1(
        MapWidgetFactory::SharedCacheContract);
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("OPMapControl"), QStringLiteral("OPMapControl"),
        contract, fakeCreator(QStringLiteral("OPMapControl"))));
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("QGroundControl"), QStringLiteral("QGroundControl"),
        contract, fakeCreator(QStringLiteral("QGroundControl"))));
    QSignalSpy changed(&factory, &MapWidgetFactory::BackendChanged);

    QVERIFY(!factory.SetBackend(QStringLiteral("Missing")));
    QVERIFY(factory.SetBackend(QStringLiteral("QGC")));
    QCOMPARE(factory.RequestedBackend(), QStringLiteral("QGroundControl"));
    QCOMPARE(settings.value(QStringLiteral("MapWidgetBackend")).toString(),
             QStringLiteral("QGroundControl"));
    QCOMPARE(changed.count(), 1);
    QWidget host;
    QScopedPointer<AbstractMapWidget> map(
        factory.CreateMapWidget(MapWidgetRole::FlightData, &host));
    QVERIFY(map);
    QCOMPARE(map->BackendId(), QStringLiteral("QGroundControl"));
    QCOMPARE(map->Widget()->parentWidget(), &host);
    QCOMPARE(map->parent(), &host);
}

void MapWidgetFactoryTest::constructorFailureFallsBackToOPMapControl()
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("MapWidgetBackend"),
                      QStringLiteral("QGroundControl"));
    MapWidgetFactory factory(&settings);
    const QString contract = QString::fromLatin1(
        MapWidgetFactory::SharedCacheContract);
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("OPMapControl"), QStringLiteral("OPMapControl"),
        contract, fakeCreator(QStringLiteral("OPMapControl"))));
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("QGroundControl"), QStringLiteral("QGroundControl"),
        contract, [](MapWidgetRole, const QString &, QWidget *, QObject *) {
            return nullptr;
        }));

    QWidget host;
    QScopedPointer<AbstractMapWidget> map(
        factory.CreateMapWidget(MapWidgetRole::Simulation, &host));
    QVERIFY(map);
    QCOMPARE(map->BackendId(), QStringLiteral("OPMapControl"));
    QCOMPARE(settings.value(QStringLiteral("MapWidgetBackend")).toString(),
             QStringLiteral("QGroundControl"));
    QVERIFY(factory.LastStatus().contains(QStringLiteral("could not be created")));
}

void MapWidgetFactoryTest::effectiveBackendStaysFixedUntilRestart()
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    MapWidgetFactory factory(&settings);
    const QString contract = QString::fromLatin1(
        MapWidgetFactory::SharedCacheContract);
    MapWidgetRole receivedRole = MapWidgetRole::Preview;
    QString receivedCacheRoot;
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("OPMapControl"), QStringLiteral("OPMapControl"),
        contract,
        [&receivedRole, &receivedCacheRoot](
                        MapWidgetRole role, const QString &sharedCacheRoot,
                        QWidget *widgetParent, QObject *owner) {
            receivedRole = role;
            receivedCacheRoot = sharedCacheRoot;
            return new FakeMapWidget(
                QStringLiteral("OPMapControl"), widgetParent, owner);
        }));
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("QGroundControl"), QStringLiteral("QGroundControl"),
        contract, fakeCreator(QStringLiteral("QGroundControl"))));

    QWidget host;
    QScopedPointer<AbstractMapWidget> first(factory.CreateMapWidget(
        MapWidgetRole::FlightPlanner, &host));
    QVERIFY(first);
    QCOMPARE(receivedRole, MapWidgetRole::FlightPlanner);
    QCOMPARE(receivedCacheRoot, factory.SharedCacheRoot());
    QCOMPARE(first->BackendId(), QStringLiteral("OPMapControl"));

    QVERIFY(factory.SetBackend(QStringLiteral("QGroundControl")));
    QCOMPARE(factory.RequestedBackend(), QStringLiteral("QGroundControl"));
    QCOMPARE(factory.CurrentBackend(), QStringLiteral("OPMapControl"));
    QScopedPointer<AbstractMapWidget> second(factory.CreateMapWidget(
        MapWidgetRole::Simulation, &host));
    QVERIFY(second);
    QCOMPARE(second->BackendId(), QStringLiteral("OPMapControl"));
    QCOMPARE(receivedRole, MapWidgetRole::Simulation);
}

void MapWidgetFactoryTest::invalidAdapterFallsBackAndUsesWidgetAsOwner()
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("MapWidgetBackend"),
                      QStringLiteral("QGroundControl"));
    MapWidgetFactory factory(&settings);
    const QString contract = QString::fromLatin1(
        MapWidgetFactory::SharedCacheContract);
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("OPMapControl"), QStringLiteral("OPMapControl"),
        contract, fakeCreator(QStringLiteral("OPMapControl"))));
    QVERIFY(factory.RegisterBackend(
        QStringLiteral("QGroundControl"), QStringLiteral("QGroundControl"),
        contract,
        [](MapWidgetRole, const QString &, QWidget *, QObject *owner) {
            return new FakeMapWidget(
                QStringLiteral("QGroundControl"), nullptr, owner, false);
        }));

    QWidget host;
    AbstractMapWidget *map = factory.CreateMapWidget(
        MapWidgetRole::FlightData, &host);
    QVERIFY(map);
    QCOMPARE(map->BackendId(), QStringLiteral("OPMapControl"));
    QCOMPARE(map->parent(), &host);
    QVERIFY(factory.LastStatus().contains(QStringLiteral("could not be created")));
}

QTEST_MAIN(MapWidgetFactoryTest)

#include "test_mapwidgetfactory.moc"
