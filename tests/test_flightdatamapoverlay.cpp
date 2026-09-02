#include "ui/flightdata/FlightDataMapOverlay.h"
#include "ui/map/AbstractMapWidget.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>
#include <QtTest/QTest>

class FakeMapBackend final : public AbstractMapWidget
{
public:
    explicit FakeMapBackend(QObject *parent = nullptr)
        : AbstractMapWidget(parent)
    {
    }

    QString BackendId() const override { return QStringLiteral("fake"); }
    QString SharedCacheRoot() const override { return QStringLiteral("/fake"); }
    QWidget *Widget() const override { return nullptr; }
    int MinZoom() const override { return 1; }
    int MaxZoom() const override { return 20; }
    double ZoomReal() const override { return 10.0; }
    int CurrentZoomLevel() const override { return 10; }
    MapGeoBounds VisibleTileExtent() const override { return {}; }
    MapCoordinate CurrentPosition() const override { return {}; }
    core::MapType::Types CurrentMapType() const override
    {
        return core::MapType::OpenStreetMap;
    }
    bool FollowUAVEnabled() const override { return followEnabled; }
    float UpdateRateLimit() const override { return 0.0f; }
    int TrailType() const override { return 0; }
    float TrailInterval() const override { return 0.0f; }
    void SetZoom(double) override {}
    void SetCurrentPosition(double, double) override {}
    void SetAcceleratedRenderingEnabled(bool) override {}
    void SetFollowUAVEnabled(bool enabled) override
    {
        followEnabled = enabled;
        ++followCallCount;
    }
    void SetTrailModeTimed(int) override {}
    void SetTrailModeDistance(int) override {}
    void DeleteTrails() override { ++deleteTrailsCallCount; }
    void SetUpdateRateLimit(float) override {}
    void ShowGoToDialog() override {}
    void GoHome() override {}
    void LastPosition() override {}
    void CacheVisibleRegion() override {}
    void UpdateHomePosition(double, double, double) override {}
    void SetMissionPlanningEnabled(bool) override {}
    void SetPlannerRows(const QVector<WpRowData> &,
                        FlightPlannerMissionModel::MissionStore) override {}
    void SetPlannerAltitudePresentation(double, const QString &) override {}
    void SetPlannerHome(double, double, double) override {}
    void ClearPlannerHome() override {}
    void SetPlannerSelection(int) override {}
    void SetLogTrail(const QVector<MapCoordinate> &) override {}
    void SetLogCursor(const MapCoordinate &, double) override {}

    bool followEnabled = false;
    int followCallCount = 0;
    int deleteTrailsCallCount = 0;
};

class FlightDataMapOverlayTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesStableMissionPlannerGeometry();
    void delegatesMapActionsToBackend();
};

void FlightDataMapOverlayTest::exposesStableMissionPlannerGeometry()
{
    QWidget host;
    host.resize(660, 640);
    auto *layout = new QGridLayout(&host);
    layout->setObjectName(QStringLiteral("MapVideoLayout"));
    layout->setContentsMargins(0, 0, 0, 0);
    auto *mapSurface = new QWidget(&host);
    layout->addWidget(mapSurface, 0, 0);
    FakeMapBackend backend;
    FlightDataMapOverlay overlay(&backend, layout, &host);
    overlay.setTelemetry(14.0, 0.8, 12.4, 275.0, 4.1,
                         QStringLiteral("Mission WP 2/5"), 40.0);

    host.show();
    QCoreApplication::processEvents();

    QWidget *controls = host.findChild<QWidget *>(
        QStringLiteral("FlightMapControls"));
    QWidget *telemetry = host.findChild<QWidget *>(
        QStringLiteral("FlightMapTelemetry"));
    QWidget *progress = host.findChild<QWidget *>(
        QStringLiteral("FlightMissionProgress"));
    QVERIFY(controls);
    QVERIFY(telemetry);
    QVERIFY(progress);
    QVERIFY(controls->isVisibleTo(&host));
    QVERIFY(telemetry->isVisibleTo(&host));
    QVERIFY(progress->isVisibleTo(&host));
    QVERIFY(controls->mapTo(&host, QPoint()).x() >= 8);
    QVERIFY(controls->mapTo(&host, QPoint()).y() >= 8);
    const QPoint telemetryBottomLeft = telemetry->mapTo(
        &host, telemetry->rect().bottomLeft());
    const QPoint progressBottomRight = progress->mapTo(
        &host, progress->rect().bottomRight());
    QVERIFY(qAbs((host.height() - 1) - telemetryBottomLeft.y() - 8) <= 1);
    QVERIFY(qAbs((host.width() - 1) - progressBottomRight.x() - 8) <= 1);
    QVERIFY(qAbs((host.height() - 1) - progressBottomRight.y() - 8) <= 1);
    QVERIFY(telemetry->mapTo(&host, telemetry->rect().topRight()).x()
            < progress->mapTo(&host, progress->rect().topLeft()).x());
    QCOMPARE(host.findChild<QProgressBar *>(
                 QStringLiteral("FlightMissionProgressBar"))->value(),
             400);
}

void FlightDataMapOverlayTest::delegatesMapActionsToBackend()
{
    QWidget host;
    auto *layout = new QGridLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QWidget(&host), 0, 0);
    FakeMapBackend backend;
    FlightDataMapOverlay overlay(&backend, layout, &host);

    auto *autoPan = host.findChild<QCheckBox *>(
        QStringLiteral("AutoPanCheckBox"));
    auto *clearTrack = host.findChild<QPushButton *>(
        QStringLiteral("ClearTrackButton"));
    QVERIFY(autoPan);
    QVERIFY(clearTrack);
    autoPan->setChecked(true);
    QCOMPARE(backend.followCallCount, 1);
    QVERIFY(backend.followEnabled);
    clearTrack->click();
    QCOMPARE(backend.deleteTrailsCallCount, 1);
}

QTEST_MAIN(FlightDataMapOverlayTest)
#include "test_flightdatamapoverlay.moc"
